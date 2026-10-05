#include "Server/Server.h"
#include "Server/RpcError.h"

#include "BoostExtras/StopToken.h"
#include "Awl/StringFormat.h"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <functional>
#include <boost/asio/deferred.hpp>
#include <boost/asio/experimental/parallel_group.hpp>
#include <boost/asio/multiple_exceptions.hpp>
#include <array>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/system_error.hpp>
#include <string_view>
#include <utility>

namespace mcp
{
    namespace
    {
        namespace json = boost::json;

        bool isBuiltin(const std::string_view method)
        {
            return method == "initialize" || method == "notifications/initialized" || method == "ping"
                || method == "server/discover" || method == "tools/list" || method == "tools/call"
                || method == "resources/list" || method == "resources/read";
        }

        std::string requiredString(const json::object& params, const std::string_view key)
        {
            const json::value* value = params.if_contains(key);
            if (!value || !value->is_string() || value->as_string().empty())
            {
                throw RpcError(-32602, "Invalid params");
            }

            return std::string(value->as_string());
        }

        json::object makeError(const RpcError& error)
        {
            json::object result{{"code", error.code()}, {"message", error.what()}};
            if (error.data())
            {
                result["data"] = *error.data();
            }

            return result;
        }

        json::object serializeTool(const ToolDefinition& definition)
        {
            json::object result{{"name", definition.name}, {"description", definition.description},
                {"inputSchema", definition.inputSchema}};
            if (definition.outputSchema)
            {
                result["outputSchema"] = *definition.outputSchema;
            }

            return result;
        }

        json::object serializeResource(const ResourceDefinition& definition)
        {
            json::object result{{"uri", definition.uri}, {"name", definition.name},
                {"description", definition.description}};
            if (!definition.mimeType.empty())
            {
                result["mimeType"] = definition.mimeType;
            }

            return result;
        }

        json::object serializeContents(const ResourceContents& contents)
        {
            json::object result{{"uri", contents.uri}};
            if (!contents.mimeType.empty())
            {
                result["mimeType"] = contents.mimeType;
            }

            if (const auto* text = std::get_if<TextResource>(&contents.contents))
            {
                result["text"] = text->text;
            }
            else
            {
                result["blob"] = std::get<BlobResource>(contents.contents).base64;
            }

            return result;
        }
    }

    Server::Server(boost::asio::any_io_executor executor,
        const std::shared_ptr<InputChannel>& input_channel, const std::shared_ptr<awl::ILogger>& logger, ServerOptions options) :
        _executor(std::move(executor)), _inputChannel(input_channel), _logger(logger),
        _transportServer(_executor, logger->createLogger("TransportServer")), _options(std::move(options))
    {
        if (!_options.maxConcurrentRequests || _options.name.empty() || _options.version.empty())
        {
            throw awl::GeneralException("Invalid server options.");
        }
    }

    void Server::checkSetup() const
    {
        if (_started)
        {
            throw awl::GeneralException("Cannot register after the server has started.");
        }
    }

    void Server::addTransport(std::unique_ptr<ITransport> transport)
    {
        checkSetup();
        _transportServer.addTransport(std::move(transport));
    }

    void Server::addHandler(std::string method, std::unique_ptr<IHandler> handler)
    {
        checkSetup();
        if (method.empty() || method.starts_with("rpc.") || isBuiltin(method) || _handlers.contains(method))
        {
            throw awl::GeneralException("Empty, reserved or duplicate handler method.");
        }

        _logger->debug("Registering handler: {}", method);
        _handlers.emplace(std::move(method), std::move(handler));
    }

    void Server::addTool(std::unique_ptr<ITool> tool_handler)
    {
        checkSetup();
        ToolDefinition definition = tool_handler->definition();
        if (definition.name.empty() || _tools.contains(definition.name))
        {
            throw awl::GeneralException("Empty or duplicate tool name.");
        }

        const json::value* input_type = definition.inputSchema.if_contains("type");
        if (!input_type || *input_type != "object")
        {
            throw awl::GeneralException("Tool input schema must describe an object.");
        }

        if (definition.outputSchema)
        {
            const json::value* output_type = definition.outputSchema->if_contains("type");
            if (!output_type || *output_type != "object")
            {
                throw awl::GeneralException("Tool output schema must describe an object.");
            }
        }

        const std::string name = definition.name;
        _tools.emplace(name, ToolEntry{std::move(definition), std::move(tool_handler)});
        _logger->debug("Tool registered: {}", name);
    }

    void Server::addResource(std::unique_ptr<IResource> resource_handler)
    {
        checkSetup();
        ResourceDefinition definition = resource_handler->definition();
        if (definition.uri.empty() || definition.name.empty() || _resources.contains(definition.uri))
        {
            throw awl::GeneralException("Empty resource URI/name or duplicate URI.");
        }

        const std::string uri = definition.uri;
        _resources.emplace(uri, ResourceEntry{std::move(definition), std::move(resource_handler)});
        _logger->debug("Resource registered: {}", uri);
    }

    boost::asio::awaitable<void> Server::asyncRun()
    {
        co_await boost::asio::co_spawn(_executor, asyncRunImpl(), boost::asio::use_awaitable);
    }

    boost::asio::awaitable<void> Server::asyncRunImpl()
    {
        namespace asio = boost::asio;
        checkSetup();
        _started = true;
        const auto cancellation = co_await boost::asio::this_coro::cancellation_state;
        _logger->info("Server starting");
        try
        {
            auto [order, transport_error, dispatch_error] = co_await asio::experimental::make_parallel_group(
                asio::co_spawn(_executor, asyncRunTransports(), asio::deferred),
                asio::co_spawn(_executor, asyncDispatch(), asio::deferred)
            ).async_wait(asio::experimental::wait_for_one_error(), asio::use_awaitable);
            const std::array errors{transport_error, dispatch_error};
            for (const auto index : order)
            {
                if (errors[index])
                {
                    // Preserve the original failure instead of masking it with
                    // the sibling's operation_aborted during joined cleanup.
                    std::rethrow_exception(errors[index]);
                }
            }
        }
        catch (const boost::system::system_error& error)
        {
            if (error.code() == boost::asio::error::operation_aborted)
            {
                _logger->debug("Server cancelled");
            }
            else
            {
                _logger->error("Server failure: {}", error.what());
            }

            throw;
        }
        catch (const awl::Exception& error)
        {
            _logger->error(_T("Server failure: {}"), error.message());
            throw;
        }
        catch (const boost::asio::multiple_exceptions& error)
        {
            // Parallel children may each throw operation_aborted on cancellation.
            if (cancellation.cancelled() != boost::asio::cancellation_type::none)
            {
                _logger->debug("Server cancelled");
            }
            else
            {
                _logger->error("Server failure: {}", error.what());
            }

            throw;
        }
        catch (const std::exception& error)
        {
            _logger->error("Server failure: {}", error.what());
            throw;
        }

        _logger->info("Server stopped");
    }

    boost::asio::awaitable<void> Server::asyncRunTransports()
    {
        co_await _transportServer.asyncRun();
        _inputChannel->close();
    }

    boost::asio::awaitable<void> Server::asyncDispatch()
    {
        namespace asio = boost::asio;
        struct Job
        {
            std::stop_source source;
            std::unique_ptr<std::stop_callback<std::function<void()>>> callback;
            std::shared_ptr<awl::StopToken> cancellation;
        };
        struct Completion
        {
            std::uint64_t id;
            std::exception_ptr error;
            bool cancelled;
        };
        using CompletionChannel = awl::Channel<void(boost::system::error_code, Completion)>;
        std::shared_ptr<CompletionChannel> completions = std::make_shared<CompletionChannel>(
            _executor, _options.maxConcurrentRequests);
        std::map<std::uint64_t, std::shared_ptr<Job>> jobs;
        std::uint64_t next_id = 0;
        std::exception_ptr failure;
        std::exception_ptr async_failure;
        const auto complete = [&jobs](const Completion& completion)
        {
            jobs.erase(completion.id);
            if (completion.error && !completion.cancelled)
            {
                std::rethrow_exception(completion.error);
            }
        };
        try
        {
            for (;;)
            {
                while (completions->try_receive([&](const boost::system::error_code, Completion completion)
                {
                    complete(completion);
                }))
                {}

                if (jobs.size() >= _options.maxConcurrentRequests)
                {
                    complete(co_await completions->async_receive(asio::use_awaitable));
                    continue;
                }

                boost::system::error_code error;
                IncomingMessage message = co_await _inputChannel->async_receive(
                    asio::redirect_error(asio::use_awaitable, error));
                if (error == asio::experimental::error::channel_closed)
                {
                    break;
                }

                if (error)
                {
                    throw boost::system::system_error(error);
                }

                std::shared_ptr<Job> job = std::make_shared<Job>();
                job->callback = std::make_unique<std::stop_callback<std::function<void()>>>(message.stopToken,
                    [source = job->source]() mutable { source.request_stop(); });
                // Also expose the combined request/server shutdown token to CPU
                // handlers through RequestContext. Keep the adapter through join.
                job->cancellation = std::make_shared<awl::StopToken>(_executor, job->source.get_token());
                const auto id = next_id++;
                jobs.emplace(id, job);
                asio::co_spawn(_executor, asyncProcess(std::move(message), job->source.get_token()),
                    asio::bind_cancellation_slot(job->cancellation->slot(),
                        [this, &async_failure, completions, job, id](const std::exception_ptr exception)
                        {
                            if (exception && !job->source.stop_requested())
                            {
                                async_failure = exception;
                                // Wake an idle dispatcher so output failures are
                                // observed without waiting for another request.
                                _inputChannel->cancel();
                            }

                            // At most maxConcurrentRequests jobs/completions exist;
                            // completed jobs retain their admission until consumed.
                            completions->try_send(boost::system::error_code{},
                                Completion{id, exception, job->source.stop_requested()});
                        }));
            }

            while (!jobs.empty())
            {
                complete(co_await completions->async_receive(asio::use_awaitable));
            }
        }
        catch (...)
        {
            failure = async_failure ? async_failure : std::current_exception();
        }

        if (failure)
        {
            for (const auto& [id, job] : jobs)
            {
                job->source.request_stop();
            }

            co_await asio::this_coro::throw_if_cancelled(false);
            while (!jobs.empty())
            {
                const auto completion = co_await completions->async_receive(asio::use_awaitable);
                jobs.erase(completion.id);
            }

            std::rethrow_exception(failure);
        }
    }

    boost::asio::awaitable<void> Server::asyncProcess(const IncomingMessage message, const std::stop_token execution_token)
    {
        namespace asio = boost::asio;
        json::value request_id;
        bool notification = false;
        json::object response{{"jsonrpc", "2.0"}};
        try
        {
            boost::system::error_code parse_error;
            json::value document = json::parse(message.body, parse_error);
            if (parse_error)
            {
                throw RpcError(-32700, "Parse error");
            }

            if (!document.is_object())
            {
                throw RpcError(-32600, "Invalid Request");
            }

            const json::object& request = document.as_object();
            const json::value* version = request.if_contains("jsonrpc");
            const json::value* method = request.if_contains("method");
            const json::value* id = request.if_contains("id");
            if (!version || *version != "2.0" || !method || !method->is_string()
                || (id && !id->is_string() && !id->is_int64() && !id->is_uint64()))
            {
                throw RpcError(-32600, "Invalid Request");
            }

            notification = id == nullptr;
            if (id)
            {
                request_id = *id;
            }

            _logger->debug("Request: method={}, route={}, id={}",
                std::string_view(method->as_string()), message.routeId, json::serialize(request_id));

            json::object params;
            if (const json::value* supplied_params = request.if_contains("params"))
            {
                if (!supplied_params->is_object())
                {
                    throw RpcError(-32602, "Invalid params");
                }

                params = supplied_params->as_object();
            }

            json::object meta;
            if (const json::value* supplied_meta = params.if_contains("_meta"))
            {
                if (!supplied_meta->is_object())
                {
                    throw RpcError(-32602, "Invalid request metadata");
                }

                meta = supplied_meta->as_object();
            }

            const auto* protocol = meta.if_contains("io.modelcontextprotocol/protocolVersion");
            if (message.requireProtocolMetadata && (!protocol || !protocol->is_string()))
            {
                throw RpcError(-32602, "Protocol version metadata is required");
            }

            std::string protocol_version = message.protocolVersion;
            if (protocol)
            {
                if (!protocol->is_string())
                {
                    throw RpcError(-32602, "Protocol version metadata must be a string");
                }

                if (!protocol_version.empty() && protocol->as_string() != protocol_version)
                {
                    throw RpcError(-32020, "Transport and metadata protocol versions do not match");
                }

                protocol_version = std::string(protocol->as_string());
            }

            if (protocol_version.empty())
            {
                protocol_version = protocol::modernVersion;
            }

            if (!protocol::isSupported(protocol_version))
            {
                throw RpcError(-32022, "Unsupported protocol version",
                    json::object{{"supported", json::array{std::string(protocol::modernVersion), std::string(protocol::legacyVersion)}},
                        {"requested", protocol_version}});
            }

            if (message.requireProtocolMetadata)
            {
                const auto* capabilities = meta.if_contains("io.modelcontextprotocol/clientCapabilities");
                if (!capabilities || !capabilities->is_object())
                {
                    throw RpcError(-32602, "Client capabilities metadata must be an object");
                }

                if (const auto* info = meta.if_contains("io.modelcontextprotocol/clientInfo"))
                {
                    if (!info->is_object() || !info->as_object().if_contains("name")
                        || !info->as_object().at("name").is_string() || !info->as_object().if_contains("version")
                        || !info->as_object().at("version").is_string())
                    {
                        throw RpcError(-32602, "Invalid client info metadata");
                    }
                }
            }

            RequestContext context(message.routeId,
                notification ? std::nullopt : std::optional<json::value>(request_id),
                message.outputChannel, std::move(meta), execution_token, message.stopToken, std::move(protocol_version));
            response["result"] = co_await asyncInvoke(
                std::string(method->as_string()), std::move(params), std::move(context));
        }
        catch (const RpcError& error)
        {
            _logger->warning(_T("RPC error on route {}: {} ({})"),
                message.routeId, error.message(), error.code());
            response["error"] = makeError(error);
        }
        catch (const boost::system::system_error& error)
        {
            if (error.code() == asio::error::operation_aborted)
            {
                throw;
            }

            _logger->error("Request failure on route {}: {}", message.routeId, error.what());
            response["error"] = makeError(RpcError(-32603, "Internal error"));
        }
        catch (const awl::Exception& error)
        {
            _logger->error(_T("Request failure on route {}: {}"), message.routeId, error.message());
            response["error"] = makeError(RpcError(-32603, "Internal error"));
        }
        catch (const std::exception& error)
        {
            _logger->error("Request failure on route {}: {}", message.routeId, error.what());
            response["error"] = makeError(RpcError(-32603, "Internal error"));
        }

        OutgoingMessage outgoing{message.routeId, MessageKind::Accepted, {}, message.stopToken};
        if (!notification)
        {
            response["id"] = std::move(request_id);
            outgoing.kind = MessageKind::Response;
            outgoing.body = json::serialize(response);
        }

        co_await message.outputChannel->async_send(
            boost::system::error_code{}, std::move(outgoing), asio::use_awaitable);
    }

    boost::asio::awaitable<boost::json::value> Server::asyncInvoke(
        const std::string method, json::object params, RequestContext context)
    {
        const bool legacy = context.protocolVersion() == protocol::legacyVersion;
        if (method == "initialize")
        {
            if (!context.requestId())
            {
                throw RpcError(-32600, "Initialize must be a request");
            }

            const std::string requested = requiredString(params, "protocolVersion");
            _logger->debug("Initializing legacy client, requested protocol: {}", requested);
            const auto* capabilities = params.if_contains("capabilities");
            const auto* client_info = params.if_contains("clientInfo");
            if (!capabilities || !capabilities->is_object() || !client_info || !client_info->is_object())
            {
                throw RpcError(-32602, "Initialize requires capabilities and clientInfo objects");
            }

            requiredString(client_info->as_object(), "name");
            requiredString(client_info->as_object(), "version");
            // This handshake serves the legacy dialect. A client requesting a
            // different version can accept the offered version or disconnect.
            co_return json::object{{"protocolVersion", std::string(protocol::legacyVersion)},
                {"capabilities", json::object{{"tools", json::object{}}, {"resources", json::object{}}}},
                {"serverInfo", json::object{{"name", _options.name}, {"version", _options.version}}}};
        }

        if (method == "notifications/initialized")
        {
            if (context.requestId())
            {
                throw RpcError(-32600, "Initialized must be a notification");
            }

            co_return json::object{};
        }

        if (method == "ping")
        {
            co_return json::object{};
        }

        if (method == "server/discover")
        {
            co_return json::object{{"resultType", "complete"},
                {"supportedVersions", json::array{std::string(protocol::modernVersion), std::string(protocol::legacyVersion)}},
                {"capabilities", json::object{{"tools", json::object{}}, {"resources", json::object{}}}},
                {"_meta", json::object{{"io.modelcontextprotocol/serverInfo",
                    json::object{{"name", _options.name}, {"version", _options.version}}}}}};
        }

        if (method == "tools/list" || method == "resources/list")
        {
            // Pagination is not implemented; never silently ignore a cursor.
            if (params.contains("cursor"))
            {
                throw RpcError(-32602, "Pagination is not supported");
            }

            json::array items;
            if (method == "tools/list")
            {
                for (const auto& [name, tool] : _tools)
                {
                    items.push_back(serializeTool(tool.definition));
                }

                json::object result{{"tools", std::move(items)}};
                if (!legacy)
                {
                    result["resultType"] = "complete";
                }

                co_return result;
            }
            else
            {
                for (const auto& [uri, resource] : _resources)
                {
                    items.push_back(serializeResource(resource.definition));
                }

                json::object result{{"resources", std::move(items)}};
                if (!legacy)
                {
                    result["resultType"] = "complete";
                }

                co_return result;
            }
        }

        if (method == "tools/call")
        {
            const std::string name = requiredString(params, "name");
            const auto found = _tools.find(name);
            if (found == _tools.end())
            {
                throw RpcError(-32602, "Unknown tool");
            }

            json::object arguments;
            if (const json::value* supplied = params.if_contains("arguments"))
            {
                if (!supplied->is_object())
                {
                    throw RpcError(-32602, "Invalid tool arguments");
                }

                arguments = supplied->as_object();
            }

            ToolResult result;
            _logger->debug("Calling tool: {}", name);
            try
            {
                result = co_await found->second.handler->asyncCall(std::move(arguments), std::move(context));
            }
            catch (const RpcError&)
            {
                throw;
            }
            catch (const boost::system::system_error& error)
            {
                if (error.code() == boost::asio::error::operation_aborted)
                {
                    throw;
                }

                _logger->error("Tool {} failed: {}", name, error.what());
                result.isError = true;
                result.content.push_back(json::object{{"type", "text"},
                    {"text", std::format("Tool '{}' failed: {}", name, error.what())}});
            }
            catch (const awl::Exception& error)
            {
                _logger->error(_T("Tool {} failed: {}"), awl::fromAString(name), error.message());
                result.isError = true;
                result.content.push_back(json::object{{"type", "text"},
                    {"text", std::format("Tool '{}' failed: {}", name, error.message())}});
            }
            catch (const std::exception& error)
            {
                _logger->error("Tool {} failed: {}", name, error.what());
                result.isError = true;
                result.content.push_back(json::object{{"type", "text"},
                    {"text", std::format("Tool '{}' failed: {}", name, error.what())}});
            }

            if (result.isError && result.content.empty())
            {
                result.content.push_back(json::object{{"type", "text"},
                    {"text", std::format("Tool '{}' failed without a diagnostic message.", name)}});
            }

            json::object serialized{{"content", std::move(result.content)}, {"isError", result.isError}};
            if (!legacy)
            {
                serialized["resultType"] = "complete";
            }
            if (result.structuredContent)
            {
                serialized["structuredContent"] = std::move(*result.structuredContent);
            }

            co_return serialized;
        }

        if (method == "resources/read")
        {
            const std::string uri = requiredString(params, "uri");
            const auto found = _resources.find(uri);
            if (found == _resources.end())
            {
                throw RpcError(-32602, "Resource not found", json::object{{"uri", uri}});
            }

            _logger->debug("Reading resource: {}", uri);
            const std::vector<ResourceContents> contents = co_await found->second.handler->asyncRead(std::move(context));
            json::array items;
            for (const ResourceContents& item : contents)
            {
                items.push_back(serializeContents(item));
            }

            json::object result{{"contents", std::move(items)}};
            if (!legacy)
            {
                result["resultType"] = "complete";
            }

            co_return result;
        }

        const auto found = _handlers.find(method);
        if (found == _handlers.end())
        {
            throw RpcError(-32601, "Method not found");
        }

        co_return co_await found->second->asyncHandle(std::move(params), std::move(context));
    }
}
