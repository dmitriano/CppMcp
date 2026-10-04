#pragma once

#include "Server/IHandler.h"
#include "Server/IResource.h"
#include "Server/ITool.h"
#include "Transport/TransportServer.h"

#include <map>
#include <memory>
#include <string>

namespace mcp
{
    struct ServerOptions
    {
        std::size_t maxConcurrentRequests = 64;
        std::string name = "CppMcp";
        std::string version = "0.1.0";
    };

    class Server
    {
    public:

        // Executor must wrap a caller-created strand. Input is non-null and
        // exclusively consumed by this server; transports share this channel.
        // Logger is non-null; child transport logging uses the same root logger.
        Server(boost::asio::any_io_executor executor, const std::shared_ptr<InputChannel>& input_channel,
            const std::shared_ptr<awl::ILogger>& logger, ServerOptions options = {});

        // Setup only, before asyncRun(); transfers ownership of a non-null transport.
        void addTransport(std::unique_ptr<ITransport> transport);

        // Setup only, before asyncRun(); handlers are required to be non-null.
        void addHandler(std::string method, std::unique_ptr<IHandler> handler);

        void addTool(std::unique_ptr<ITool> tool_handler);

        void addResource(std::unique_ptr<IResource> resource_handler);

        // Runs once. Requests run concurrently on the caller strand, with a bound
        // on active requests. Handlers must await their child work.
        // Failure/cancellation joins dispatcher and transports before returning.
        // Normal transport completion closes input; cancellation leaves it open.
        // Keep server and its executors alive until this operation completes.
        boost::asio::awaitable<void> asyncRun();

    private:

        struct ToolEntry
        {
            ToolDefinition definition;
            std::unique_ptr<ITool> handler;
        };

        struct ResourceEntry
        {
            ResourceDefinition definition;
            std::unique_ptr<IResource> handler;
        };

        void checkSetup() const;

        boost::asio::awaitable<void> asyncRunImpl();

        boost::asio::awaitable<void> asyncRunTransports();

        boost::asio::awaitable<void> asyncDispatch();

        boost::asio::awaitable<void> asyncProcess(IncomingMessage message, std::stop_token execution_token);

        boost::asio::awaitable<boost::json::value> asyncInvoke(
            std::string method, boost::json::object params, RequestContext context);

        boost::asio::any_io_executor _executor;
        std::shared_ptr<InputChannel> _inputChannel;
        const std::shared_ptr<awl::ILogger> _logger;
        TransportServer _transportServer;
        std::map<std::string, std::unique_ptr<IHandler>, std::less<>> _handlers;
        std::map<std::string, ToolEntry, std::less<>> _tools;
        std::map<std::string, ResourceEntry, std::less<>> _resources;
        ServerOptions _options;
        bool _started = false;
    };
}
