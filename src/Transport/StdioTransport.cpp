#include "Transport/StdioTransport.h"
#include "Common/Protocol.h"

#include "Awl/Exception.h"

#include <boost/asio/dispatch.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/experimental/channel_error.hpp>
#include <boost/system/system_error.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/json.hpp>
#include <array>
#include <algorithm>

namespace mcp
{
    namespace asio = boost::asio;
    namespace json = boost::json;

    StdioTransport::StdioTransport(asio::any_io_executor executor, StdioStreams streams,
        const std::shared_ptr<InputChannel>& input_channel, const std::shared_ptr<OutputChannel>& output_channel,
        const std::shared_ptr<awl::ILogger>& logger, const StdioOptions options) :
        _executor(std::move(executor)), _streams(std::move(streams)), _inputChannel(input_channel),
        _outputChannel(output_channel), _logger(logger), _options(options), _drained(_executor, 1)
    {
        if (!options.maxMessageBytes || !options.maxPendingRequests || options.shutdownTimeout.count() <= 0)
        {
            throw awl::GeneralException("Stdio limits must be positive.");
        }
    }

    asio::awaitable<void> StdioTransport::asyncRun()
    {
        using namespace asio::experimental::awaitable_operators;
        co_await asio::dispatch(_executor, asio::use_awaitable);
        if (_started)
        {
            throw awl::GeneralException("Stdio transport can only run once.");
        }

        _started = true;
        _logger->info("Stdio transport starting");
        // EOF allows a bounded drain. Expiry cancels even a blocked native write.
        // An I/O error cancels and joins its sibling through &&.
        try
        {
            co_await (asyncRead() && asyncWrite());
        }
        catch (...)
        {
            for (auto& source : _pending)
            {
                source.request_stop();
            }

            if (!_shutdownTimedOut)
            {
                throw;
            }
        }

        _logger->info("Stdio transport stopped");
    }

    void StdioTransport::completeRequest(const std::stop_token token)
    {
        const auto found = std::ranges::find_if(_pending, [token](const std::stop_source& source)
        {
            return source.get_token() == token;
        });
        if (found == _pending.end())
        {
            throw awl::GeneralException("Unknown stdio response token.");
        }

        _pending.erase(found);
        if (_pending.empty())
        {
            _drained.try_send(boost::system::error_code{}, true);
        }
    }

    asio::awaitable<void> StdioTransport::asyncDrain()
    {
        while (!_pending.empty())
        {
            co_await _drained.async_receive(asio::use_awaitable);
        }
    }

    asio::awaitable<void> StdioTransport::asyncDrainTimeout()
    {
        asio::steady_timer timer(_executor);
        timer.expires_after(_options.shutdownTimeout);
        co_await timer.async_wait(asio::use_awaitable);
    }

    asio::awaitable<void> StdioTransport::asyncRead()
    {
        std::array<char, 4096> buffer{};
        std::string pending;
        for (;;)
        {
            const std::size_t size = co_await _streams.input->asyncReadSome(asio::buffer(buffer));
            if (size == 0)
            {
                if (!pending.empty())
                {
                    throw awl::GeneralException("Stdio ended inside a message without a newline.");
                }

                using namespace asio::experimental::awaitable_operators;
                co_await (asyncDrain() || asyncDrainTimeout());
                for (auto& source : _pending)
                {
                    source.request_stop();
                }

                if (!_pending.empty())
                {
                    _shutdownTimedOut = true;
                    throw boost::system::system_error(asio::error::operation_aborted);
                }

                _requests.clear();
                _outputChannel->close();
                co_return;
            }

            pending.append(buffer.data(), size);
            std::size_t start = 0;
            for (;;)
            {
                const auto end = pending.find('\n', start);
                if (end == std::string::npos)
                {
                    break;
                }

                std::string body = pending.substr(start, end - start);
                if (body.ends_with('\r'))
                {
                    body.pop_back();
                }

                if (body.size() > _options.maxMessageBytes)
                {
                    throw awl::GeneralException("Stdio message exceeds the byte limit.");
                }

                co_await asyncDeliver(std::move(body));
                start = end + 1;
            }

            pending.erase(0, start);
            if (pending.size() > _options.maxMessageBytes)
            {
                throw awl::GeneralException("Stdio message exceeds the byte limit.");
            }
        }
    }

    asio::awaitable<void> StdioTransport::asyncDeliver(std::string body)
    {
        boost::system::error_code error;
        const json::value document = json::parse(body, error);
        std::stop_source source;
        const std::stop_token token = source.get_token();
        bool require_metadata = true;
        bool initializing = false;
        if (!error && document.is_object())
        {
            const auto& object = document.as_object();
            const auto* method = object.if_contains("method");
            initializing = method && *method == "initialize" && object.contains("id");
            require_metadata = object.contains("id") && !initializing && _protocolVersion != protocol::legacyVersion;
            if (method && *method == "notifications/cancelled" && !object.contains("id"))
            {
                const auto* params = object.if_contains("params");
                if (params && params->is_object())
                {
                    if (const auto* id = params->as_object().if_contains("requestId"))
                    {
                        const auto found = _requests.find(json::serialize(*id));
                        if (found != _requests.end())
                        {
                            const auto cancelled_token = found->second.get_token();
                            found->second.request_stop();
                            _requests.erase(found);
                            _initializeRequests.erase(json::serialize(*id));
                            completeRequest(cancelled_token);
                        }
                    }
                }

                co_return;
            }

            if (const auto* id = object.if_contains("id"); id && method && method->is_string()
                && (id->is_string() || id->is_int64() || id->is_uint64()) && object.if_contains("jsonrpc")
                && object.at("jsonrpc") == "2.0")
            {
                const bool inserted = _requests.try_emplace(json::serialize(*id), source).second;
                if (!inserted)
                {
                    throw awl::GeneralException("Duplicate in-flight stdio request ID.");
                }

                if (initializing)
                {
                    _initializeRequests.insert(json::serialize(*id));
                }
            }
        }

        if (_pending.size() >= _options.maxPendingRequests)
        {
            throw awl::GeneralException("Too many pending stdio requests.");
        }

        _pending.push_back(source);
        co_await _inputChannel->async_send(boost::system::error_code{},
            IncomingMessage{0, std::move(body), _outputChannel, token, require_metadata,
                initializing ? std::string(protocol::legacyVersion) : _protocolVersion}, asio::use_awaitable);
    }

    asio::awaitable<void> StdioTransport::asyncWrite()
    {
        for (;;)
        {
            boost::system::error_code error;
            OutgoingMessage message = co_await _outputChannel->async_receive(asio::redirect_error(asio::use_awaitable, error));
            if (error == asio::experimental::error::channel_closed)
            {
                co_return;
            }

            if (error)
            {
                throw boost::system::system_error(error);
            }

            if (message.stopToken.stop_requested())
            {
                continue;
            }

            if (message.routeId != 0 || message.body.find_first_of("\r\n") != std::string::npos)
            {
                throw awl::GeneralException("Invalid outgoing stdio frame.");
            }

            if (message.kind != MessageKind::Accepted)
            {
                co_await _streams.output->asyncWrite(std::format("{}\n", message.body));
            }

            if (message.stopToken.stop_requested())
            {
                continue;
            }

            if (message.kind == MessageKind::Response)
            {
                const json::value reply = json::parse(message.body);
                if (const auto* id = reply.as_object().if_contains("id"))
                {
                    const std::string key = json::serialize(*id);
                    if (_initializeRequests.erase(key))
                    {
                        const auto* result = reply.as_object().if_contains("result");
                        if (result && result->is_object())
                        {
                            const auto* version = result->as_object().if_contains("protocolVersion");
                            if (version && version->is_string() && std::string_view(version->as_string()) == protocol::legacyVersion)
                            {
                                _protocolVersion = protocol::legacyVersion;
                            }
                        }
                    }

                    _requests.erase(key);
                }
            }

            if (message.kind != MessageKind::Notification)
            {
                completeRequest(message.stopToken);
            }
        }
    }
}
