#include "Server/Server.h"
#include "Server/TypedTool.h"
#include "Transport/StdioTransport.h"
#include "Transport/HttpTransport.h"

#include "BoostExtras/StopToken.h"
#include "Awl/Testing/UnitTest.h"

#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <functional>
#include <iostream>
#include <stop_token>

namespace
{
    namespace asio = boost::asio;
    namespace json = boost::json;

    class EchoHandler : public mcp::IHandler
    {
    public:

        asio::awaitable<json::value> asyncHandle(json::object params, mcp::RequestContext context) override
        {
            if (params.contains("progress"))
            {
                co_await context.asyncNotify("notifications/progress", json::object{{"progressToken", context.meta().at("progressToken")}, {"progress", 1}});
            }

            params.erase("_meta");
            co_return params;
        }
    };

    struct EchoInput
    {
        std::string text;

        AWL_REFLECT(text)
    };

    struct EchoOutput
    {
        std::string text;

        AWL_REFLECT(text)
    };

    class EchoTool : public mcp::TypedTool<EchoInput, EchoOutput>
    {
    public:

        using TypedTool::TypedTool;

    protected:

        asio::awaitable<EchoOutput> asyncExecute(EchoInput input, mcp::RequestContext) override
        {
            co_return EchoOutput{std::move(input.text)};
        }
    };

    class SlowHandler : public mcp::IHandler
    {
    public:

        explicit SlowHandler(int& cancelled) : _cancelled(cancelled) {}

        asio::awaitable<json::value> asyncHandle(json::object, mcp::RequestContext context) override
        {
            struct Guard
            {
                int& cancelled;
                bool completed = false;
                ~Guard()
                {
                    if (!completed)
                    {
                        ++cancelled;
                    }
                }
            } guard{_cancelled};
            co_await context.asyncNotify("notifications/progress", json::object{{"progressToken", context.meta().at("progressToken")}, {"progress", 0}});
            asio::steady_timer timer(co_await asio::this_coro::executor);
            timer.expires_after(std::chrono::seconds(60));
            co_await timer.async_wait(asio::use_awaitable);
            guard.completed = true;
            co_return json::object{};
        }

    private:

        int& _cancelled;
    };

    class StatsHandler : public mcp::IHandler
    {
    public:

        explicit StatsHandler(int& cancelled) : _cancelled(cancelled) {}

        asio::awaitable<json::value> asyncHandle(json::object, mcp::RequestContext) override
        {
            co_return json::object{{"cancelled", _cancelled}};
        }

    private:

        int& _cancelled;
    };

    class LargeHandler : public mcp::IHandler
    {
    public:

        asio::awaitable<json::value> asyncHandle(json::object, mcp::RequestContext) override
        {
            co_return json::object{{"text", std::string(2 * 1024 * 1024, 'x')}};
        }
    };

    class StopHandler : public mcp::IHandler
    {
    public:

        StopHandler(asio::steady_timer& timer, std::stop_source& source) : _timer(timer), _source(source) {}

        asio::awaitable<json::value> asyncHandle(json::object, mcp::RequestContext) override
        {
            _timer.expires_after(std::chrono::milliseconds(50));
            _timer.async_wait([source = _source](const boost::system::error_code error) mutable
            {
                if (!error)
                {
                    source.request_stop();
                }
            });
            co_return json::object{};
        }

    private:

        asio::steady_timer& _timer;
        std::stop_source& _source;
    };
}

// Test/example entry point. The library itself never contains main().
AWL_EXAMPLE(McpServer)
{
    try
    {
        AWL_ATTRIBUTE(std::string, transport, "stdio");
        AWL_ATTRIBUTE(std::uint32_t, http_port, 0);
        AWL_ATTRIBUTE(std::size_t, max_message_bytes, 1048576);
        if (transport != "stdio" && transport != "http" && transport != "both")
        {
            throw awl::testing::TestException("Transport must be stdio, http or both.");
        }

        if (http_port > 65535)
        {
            throw awl::testing::TestException("HTTP port must be in the range 0..65535.");
        }

        asio::io_context io;
        const auto executor = asio::make_strand(io);
        asio::thread_pool blocking_io(2);
        std::stop_source stop_source;
        std::stop_callback stop_callback(context.stopToken, std::bind(&std::stop_source::request_stop, stop_source));
        asio::steady_timer timer(executor);
        const std::shared_ptr<awl::ILogger>& logger = context.logger;
        std::shared_ptr<mcp::InputChannel> input = std::make_shared<mcp::InputChannel>(executor, 1);
        mcp::Server server(executor, input, logger->createLogger("Server"));
        server.addTool(std::make_unique<EchoTool>("echo", "Return the supplied text unchanged",
            logger->createLogger("EchoTool")));
        int cancelled = 0;
        server.addHandler("test/echo", std::make_unique<EchoHandler>());
        server.addHandler("test/slow", std::make_unique<SlowHandler>(cancelled));
        server.addHandler("test/stats", std::make_unique<StatsHandler>(cancelled));
        server.addHandler("test/large", std::make_unique<LargeHandler>());
        server.addHandler("test/stop", std::make_unique<StopHandler>(timer, stop_source));
        unsigned short port = 0;
        if (transport == "stdio" || transport == "both")
        {
            std::shared_ptr<mcp::OutputChannel> output = std::make_shared<mcp::OutputChannel>(executor, 1);
            std::unique_ptr<mcp::ITransport> stdio_transport = std::make_unique<mcp::StdioTransport>(executor,
                mcp::openStdioStreams(executor, blocking_io.get_executor()), input, output,
                logger->createLogger("Stdio"), mcp::StdioOptions{max_message_bytes, 64});
            server.addTransport(std::move(stdio_transport));
        }

        if (transport == "http" || transport == "both")
        {
            std::shared_ptr<mcp::OutputChannel> output = std::make_shared<mcp::OutputChannel>(executor, 1);
            mcp::HttpOptions options;
            options.endpoint.port(static_cast<unsigned short>(http_port));
            options.maxBodyBytes = max_message_bytes;
            options.allowedOrigins = {"http://allowed.example"};
            options.readTimeout = std::chrono::milliseconds(500);
            options.requestTimeout = std::chrono::milliseconds(3000);
            std::unique_ptr<mcp::HttpTransport> http_transport = std::make_unique<mcp::HttpTransport>(
                executor, input, output, logger->createLogger("Http"), std::move(options));
            port = http_transport->localEndpoint().port();
            server.addTransport(std::move(http_transport));
        }

        std::exception_ptr failure;
        asio::post(executor, [&]
        {
            std::shared_ptr<awl::StopToken> cancellation = std::make_shared<awl::StopToken>(executor, stop_source.get_token());
            asio::co_spawn(executor, server.asyncRun(), asio::bind_cancellation_slot(cancellation->slot(),
                [&, cancellation](const std::exception_ptr error)
                {
                    if (error && !stop_source.stop_requested())
                    {
                        failure = error;
                    }

                    timer.cancel();
                }));
            std::cerr << "READY " << port << std::endl;
        });
        io.run();
        blocking_io.join();
        if (failure)
        {
            std::rethrow_exception(failure);
        }
    }
    catch (const std::exception& error)
    {
        throw awl::testing::TestException(error.what());
    }
}
