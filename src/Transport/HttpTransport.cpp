#include "Transport/HttpTransport.h"
#include "Common/Protocol.h"

#include "Awl/Exception.h"

#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/json.hpp>
#include <algorithm>
#include <array>
#include <charconv>
#include <optional>
#include <string_view>
#include <stop_token>

namespace mcp
{
    namespace asio = boost::asio;
    namespace beast = boost::beast;
    namespace http = beast::http;
    namespace json = boost::json;
    using namespace asio::experimental::awaitable_operators;

    namespace
    {
        std::string_view trim(std::string_view text)
        {
            const auto first = text.find_first_not_of(" \t");
            if (first == std::string_view::npos)
            {
                return {};
            }

            return text.substr(first, text.find_last_not_of(" \t") - first + 1);
        }

        bool equalsIgnoreCase(const std::string_view left, const std::string_view right)
        {
            return std::ranges::equal(left, right, [](const unsigned char a, const unsigned char b)
            {
                const auto lower = [](const unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
                return lower(a) == lower(b);
            });
        }

        bool accepts(const std::string_view field, const std::string_view expected)
        {
            std::string_view remaining = field;
            while (!remaining.empty())
            {
                const auto comma = remaining.find(',');
                const auto item = trim(remaining.substr(0, comma));
                const auto semicolon = item.find(';');
                if (equalsIgnoreCase(trim(item.substr(0, semicolon)), expected))
                {
                    double quality = 1;
                    auto parameters = semicolon == std::string_view::npos ? std::string_view{} : item.substr(semicolon + 1);
                    while (!parameters.empty())
                    {
                        const auto end = parameters.find(';');
                        const auto parameter = trim(parameters.substr(0, end));
                        const auto equals = parameter.find('=');
                        if (equalsIgnoreCase(trim(parameter.substr(0, equals)), "q"))
                        {
                            if (equals == std::string_view::npos)
                            {
                                return false;
                            }

                            const auto value = trim(parameter.substr(equals + 1));
                            const auto [last, error] = std::from_chars(value.data(), value.data() + value.size(), quality);
                            if (error != std::errc{} || last != value.data() + value.size() || !(quality >= 0 && quality <= 1))
                            {
                                return false;
                            }
                        }

                        parameters = end == std::string_view::npos ? std::string_view{} : parameters.substr(end + 1);
                    }

                    if (quality > 0)
                    {
                        return true;
                    }
                }

                remaining = comma == std::string_view::npos ? std::string_view{} : remaining.substr(comma + 1);
            }

            return false;
        }

        std::optional<std::string> decodeHeader(const std::string_view field)
        {
            if (!(field.starts_with("=?base64?") && field.ends_with("?=")))
            {
                if (trim(field) != field || std::ranges::any_of(field, [](const unsigned char c)
                {
                    return (c < 0x20 && c != '\t') || c > 0x7e;
                }))
                {
                    return std::nullopt;
                }

                return std::string(field);
            }

            const auto encoded = field.substr(9, field.size() - 11);
            if (encoded.size() % 4 != 0)
            {
                return std::nullopt;
            }

            constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::string decoded;
            for (std::size_t offset = 0; offset < encoded.size(); offset += 4)
            {
                unsigned bits = 0;
                unsigned padding = 0;
                for (unsigned i = 0; i < 4; ++i)
                {
                    const char c = encoded[offset + i];
                    if (c == '=')
                    {
                        if (i < 2 || offset + 4 != encoded.size())
                        {
                            return std::nullopt;
                        }

                        ++padding;
                        bits <<= 6;
                    }
                    else
                    {
                        const auto digit = alphabet.find(c);
                        if (padding || digit == std::string_view::npos)
                        {
                            return std::nullopt;
                        }

                        bits = (bits << 6) | static_cast<unsigned>(digit);
                    }
                }

                if ((padding == 1 && (bits & 0xff) != 0) || (padding == 2 && (bits & 0xffff) != 0))
                {
                    return std::nullopt;
                }

                decoded.push_back(static_cast<char>(bits >> 16));
                if (padding < 2)
                {
                    decoded.push_back(static_cast<char>(bits >> 8));
                }

                if (!padding)
                {
                    decoded.push_back(static_cast<char>(bits));
                }
            }

            return decoded;
        }

        json::value errorBody(const int code, const std::string& message, const json::value& id = nullptr)
        {
            return json::object{{"jsonrpc", "2.0"}, {"id", id},
                {"error", json::object{{"code", code}, {"message", message}}}};
        }

        http::status rpcStatus(const json::value& reply)
        {
            const auto* error = reply.as_object().if_contains("error");
            if (error && error->is_object())
            {
                const auto* code = error->as_object().if_contains("code");
                if (code && code->is_int64())
                {
                    if (code->as_int64() == -32601)
                    {
                        return http::status::not_found;
                    }

                    if (code->as_int64() == -32022 || code->as_int64() == -32020
                        || code->as_int64() == -32600 || code->as_int64() == -32700)
                    {
                        return http::status::bad_request;
                    }
                }
            }

            return http::status::ok;
        }
    }

    struct HttpTransport::Session
    {
        Session(const asio::any_io_executor& executor, asio::ip::tcp::socket socket, const RouteId route_id,
            const std::shared_ptr<InputChannel>& input_channel, const std::shared_ptr<OutputChannel>& output_channel,
            const HttpOptions& options) : stream(std::move(socket)), routeId(route_id), input(input_channel),
            output(output_channel), settings(options), responses(executor, options.maxQueuedResponses)
        {}

        void cancel()
        {
            source.request_stop();
            cancellation.emit(asio::cancellation_type::terminal);
            boost::system::error_code ignored;
            stream.socket().cancel(ignored);
            stream.socket().close(ignored);
        }

        asio::awaitable<void> asyncReply(const http::status status, const std::string body, const unsigned version, const bool drain_input = false)
        {
            http::response<http::string_body> reply(status, version);
            reply.set(http::field::content_type, "application/json");
            reply.keep_alive(false);
            if (status == http::status::method_not_allowed)
            {
                reply.set(http::field::allow, "POST");
            }

            reply.body() = body;
            reply.prepare_payload();
            if (drain_input)
            {
                stream.expires_after(settings.readTimeout);
            }

            co_await http::async_write(stream, reply, asio::use_awaitable);
            // Sending FIN before draining prevents unread oversized/pipelined
            // input from resetting the connection and discarding our error body.
            if (!drain_input)
            {
                co_return;
            }

            boost::system::error_code error;
            stream.socket().shutdown(asio::ip::tcp::socket::shutdown_send, error);
            stream.expires_after(std::chrono::milliseconds(250));
            std::array<char, 4096> discarded{};
            while (!error)
            {
                co_await stream.async_read_some(asio::buffer(discarded),
                    asio::redirect_error(asio::use_awaitable, error));
            }
        }

        asio::awaitable<void> asyncDisconnect()
        {
            std::array<char, 1> buffer{};
            boost::system::error_code error;
            co_await stream.async_read_some(asio::buffer(buffer), asio::redirect_error(asio::use_awaitable, error));
            // EOF, reset, or pipelined data ends this one-request connection.
        }

        asio::awaitable<void> asyncTimeout()
        {
            asio::steady_timer timer(co_await asio::this_coro::executor);
            timer.expires_after(settings.requestTimeout);
            co_await timer.async_wait(asio::use_awaitable);
        }

        asio::awaitable<void> asyncResponses(const unsigned version, const bool is_request)
        {
            OutgoingMessage first = co_await responses.async_receive(asio::use_awaitable);
            while (!is_request && first.kind == MessageKind::Notification)
            {
                first = co_await responses.async_receive(asio::use_awaitable);
            }

            if (first.kind == MessageKind::Accepted)
            {
                co_await asyncReply(http::status::accepted, {}, version);
            }
            else if (first.kind == MessageKind::Response)
            {
                const auto status = rpcStatus(json::parse(first.body));
                co_await asyncReply(status, std::move(first.body), version);
            }
            else
            {
                http::response<http::empty_body> reply(http::status::ok, version);
                reply.set(http::field::content_type, "text/event-stream");
                reply.set("X-Accel-Buffering", "no");
                reply.keep_alive(false);
                reply.chunked(true);
                http::response_serializer<http::empty_body> serializer(reply);
                co_await http::async_write_header(stream, serializer, asio::use_awaitable);
                OutgoingMessage message = std::move(first);
                for (;;)
                {
                    const std::string event = std::format("event: message\ndata: {}\n\n", message.body);
                    co_await asio::async_write(stream, http::make_chunk(asio::buffer(event)), asio::use_awaitable);
                    if (message.kind == MessageKind::Response)
                    {
                        break;
                    }

                    message = co_await responses.async_receive(asio::use_awaitable);
                }

                co_await asio::async_write(stream, http::make_chunk_last(), asio::use_awaitable);
            }
        }

        asio::awaitable<void> asyncRun()
        {
            struct Guard
            {
                Session& session;
                ~Guard()
                {
                    session.source.request_stop();
                    boost::system::error_code ignored;
                    session.stream.socket().close(ignored);
                }
            } guard{*this};
            beast::flat_buffer buffer;
            http::request_parser<http::string_body> parser;
            parser.body_limit(settings.maxBodyBytes);
            stream.expires_after(settings.readTimeout);
            boost::system::error_code read_error;
            co_await http::async_read(stream, buffer, parser, asio::redirect_error(asio::use_awaitable, read_error));
            if (read_error)
            {
                if (read_error == http::error::body_limit)
                {
                    co_await asyncReply(http::status::payload_too_large,
                        json::serialize(errorBody(-32600, "HTTP body exceeds the byte limit")), 11, true);
                }

                co_return;
            }

            const auto request = parser.release();
            const unsigned version = request.version();
            const auto reject = [&](const http::status status, const int code, const std::string& reason,
                const json::value& id = json::value(nullptr))
            {
                return asyncReply(status, json::serialize(errorBody(code, reason, id)), version, true);
            };
            if (request.count(http::field::origin) > 1 || request.count("MCP-Protocol-Version") > 1
                || request.count("Mcp-Method") > 1 || request.count("Mcp-Name") > 1)
            {
                co_await reject(http::status::bad_request, -32020, "Duplicate MCP or Origin headers");
                co_return;
            }

            if (request.target() != settings.path)
            {
                co_await reject(http::status::not_found, -32600, "Unknown endpoint");
                co_return;
            }

            const auto origin = request[http::field::origin];
            if (request.find(http::field::origin) != request.end() && std::ranges::find(settings.allowedOrigins, std::string(origin)) == settings.allowedOrigins.end())
            {
                co_await reject(http::status::forbidden, -32600, "Origin is not allowed");
                co_return;
            }

            if (request.method() != http::verb::post)
            {
                co_await reject(http::status::method_not_allowed, -32600, "Only POST is supported");
                co_return;
            }

            const std::string_view content_type(request[http::field::content_type]);
            if (!equalsIgnoreCase(trim(content_type.substr(0, content_type.find(';'))), "application/json"))
            {
                co_await reject(http::status::unsupported_media_type, -32600, "Expected application/json");
                co_return;
            }

            const std::string_view accept(request[http::field::accept]);
            if (!accepts(accept, "application/json") || !accepts(accept, "text/event-stream"))
            {
                co_await reject(http::status::not_acceptable, -32600, "Accept must include JSON and SSE");
                co_return;
            }

            boost::system::error_code parse_error;
            const json::value body = json::parse(request.body(), parse_error);
            if (parse_error || !body.is_object())
            {
                co_await reject(http::status::bad_request, parse_error ? -32700 : -32600, "Invalid JSON-RPC body");
                co_return;
            }

            const auto& rpc = body.as_object();
            const auto* method = rpc.if_contains("method");
            const auto* id = rpc.if_contains("id");
            const auto* rpc_version = rpc.if_contains("jsonrpc");
            if (!method || !method->is_string() || !rpc_version || *rpc_version != "2.0"
                || (id && !id->is_string() && !id->is_int64() && !id->is_uint64()))
            {
                co_await reject(http::status::bad_request, -32600, "Invalid JSON-RPC request");
                co_return;
            }

            const std::string protocol_header(request["MCP-Protocol-Version"]);
            const auto* params = rpc.if_contains("params");
            const json::object* meta = nullptr;
            if (params && params->is_object())
            {
                if (const auto* supplied = params->as_object().if_contains("_meta"); supplied && supplied->is_object())
                {
                    meta = &supplied->as_object();
                }
            }

            const auto* protocol = meta ? meta->if_contains("io.modelcontextprotocol/protocolVersion") : nullptr;
            const bool legacy_initialize = id && *method == "initialize" && !protocol;
            const bool legacy = protocol_header == protocol::legacyVersion;
            const bool require_metadata = id && !legacy && !legacy_initialize;
            if (id)
            {
                const std::string method_header(request["Mcp-Method"]);
                if ((require_metadata && (!protocol || !protocol->is_string() || protocol_header.empty() || method_header.empty()))
                    || (protocol && (!protocol->is_string() || protocol->as_string() != protocol_header))
                    || (!method_header.empty() && method->as_string() != method_header))
                {
                    co_await reject(http::status::bad_request, -32020, "Missing or mismatched MCP headers", *id);
                    co_return;
                }

                if ((*method == "tools/call" || *method == "resources/read" || *method == "prompts/get")
                    && (!legacy || request.find("Mcp-Name") != request.end()))
                {
                    const auto* name = params && params->is_object()
                        ? params->as_object().if_contains(*method == "resources/read" ? "uri" : "name") : nullptr;
                    const auto header = decodeHeader(std::string_view(request["Mcp-Name"]));
                    if (request.find("Mcp-Name") == request.end() || !name || !name->is_string()
                        || !header || name->as_string() != *header)
                    {
                        co_await reject(http::status::bad_request, -32020, "Missing or mismatched Mcp-Name", *id);
                        co_return;
                    }
                }
            }

            stream.expires_never();
            if (buffer.size() != 0)
            {
                co_await reject(http::status::bad_request, -32600, "HTTP pipelining is not supported");
                co_return;
            }

            // Include delivery in the race: a disconnected client must cancel
            // even while the application's input channel applies backpressure.
            const std::string effective_protocol = legacy_initialize ? std::string(protocol::legacyVersion) : protocol_header;
            co_await (asyncExchange(request.body(), version, id != nullptr, require_metadata, effective_protocol)
                || asyncDisconnect() || asyncTimeout());
        }

        asio::awaitable<void> asyncExchange(std::string body, const unsigned version, const bool is_request,
            const bool require_metadata, const std::string protocol_version)
        {
            co_await input->async_send(boost::system::error_code{},
                IncomingMessage{routeId, std::move(body), output, source.get_token(), require_metadata, protocol_version}, asio::use_awaitable);
            try
            {
                co_await asyncResponses(version, is_request);
            }
            catch (const boost::system::system_error& error)
            {
                if (error.code() == asio::error::operation_aborted)
                {
                    throw;
                }

                // A failed write finishes the race immediately and cancels the
                // handler; do not wait for the disconnect/read or request timer.
            }
        }

        beast::tcp_stream stream;
        RouteId routeId;
        std::shared_ptr<InputChannel> input;
        std::shared_ptr<OutputChannel> output;
        HttpOptions settings;
        awl::Channel<void(boost::system::error_code, OutgoingMessage)> responses;
        std::stop_source source;
        asio::cancellation_signal cancellation;
    };

    HttpTransport::HttpTransport(asio::any_io_executor executor, const std::shared_ptr<InputChannel>& input_channel,
        const std::shared_ptr<OutputChannel>& output_channel, const std::shared_ptr<awl::ILogger>& logger,
        HttpOptions options) : _executor(std::move(executor)), _inputChannel(input_channel), _outputChannel(output_channel),
        _logger(logger), _options(std::move(options)), _acceptor(_executor), _completed(_executor, 1)
    {
        if (!_options.maxConnections || !_options.maxBodyBytes || !_options.maxQueuedResponses
            || _options.readTimeout.count() <= 0 || _options.requestTimeout.count() <= 0
            || _options.path.empty() || _options.path.front() != '/')
        {
            throw awl::GeneralException("Invalid HTTP transport options.");
        }

        _acceptor.open(_options.endpoint.protocol());
        _acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true));
        _acceptor.bind(_options.endpoint);
        _acceptor.listen();
    }

    asio::ip::tcp::endpoint HttpTransport::localEndpoint() const
    {
        return _acceptor.local_endpoint();
    }

    asio::awaitable<void> HttpTransport::asyncAccept()
    {
        for (;;)
        {
            asio::ip::tcp::socket socket = co_await _acceptor.async_accept(asio::use_awaitable);
            if (_sessions.size() >= _options.maxConnections)
            {
                socket.close();
                continue;
            }

            const RouteId route = _nextRouteId++;
            std::shared_ptr<Session> session = std::make_shared<Session>(_executor, std::move(socket), route,
                _inputChannel, _outputChannel, _options);
            _sessions.emplace(route, session);
            asio::co_spawn(_executor, session->asyncRun(), asio::bind_cancellation_slot(session->cancellation.slot(),
                [this, session, route](const std::exception_ptr error)
                {
                    if (error && !session->source.stop_requested())
                    {
                        _logger->warning("HTTP connection failed on route {}", route);
                    }

                    _sessions.erase(route);
                    _completed.try_send(boost::system::error_code{}, true);
                }));
        }
    }

    asio::awaitable<void> HttpTransport::asyncRoute()
    {
        for (;;)
        {
            OutgoingMessage message = co_await _outputChannel->async_receive(asio::use_awaitable);
            if (message.stopToken.stop_requested())
            {
                continue;
            }

            if (const auto found = _sessions.find(message.routeId); found != _sessions.end())
            {
                if (!found->second->responses.try_send(boost::system::error_code{}, std::move(message)))
                {
                    // A slow client only cancels its own route, not other peers.
                    found->second->cancel();
                }
            }
        }
    }

    asio::awaitable<void> HttpTransport::asyncRun()
    {
        co_await asio::dispatch(_executor, asio::use_awaitable);
        if (_started)
        {
            throw awl::GeneralException("HTTP transport can only run once.");
        }

        _started = true;
        _logger->info("HTTP transport listening on {}:{}", _options.endpoint.address().to_string(), localEndpoint().port());
        std::exception_ptr failure;
        try
        {
            co_await (asyncAccept() && asyncRoute());
        }
        catch (...)
        {
            failure = std::current_exception();
        }

        boost::system::error_code ignored;
        _acceptor.close(ignored);
        for (const auto& [route, session] : _sessions)
        {
            session->cancel();
        }

        co_await asio::this_coro::throw_if_cancelled(false);
        while (!_sessions.empty())
        {
            co_await _completed.async_receive(asio::use_awaitable);
        }

        if (failure)
        {
            std::rethrow_exception(failure);
        }
    }
}
