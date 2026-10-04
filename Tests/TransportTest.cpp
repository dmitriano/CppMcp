#include "Tests/TestTransports.h"
#include "Transport/TransportServer.h"

#include "Awl/Testing/UnitTest.h"

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/json.hpp>
#include <chrono>
#include <exception>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace
{
    namespace asio = boost::asio;
    using namespace asio::experimental::awaitable_operators;
    using namespace mcp;
    using namespace mcp::testing;

    struct TestPeer
    {
        TestPeer(const asio::any_io_executor& executor, const std::size_t capacity) :
            outputChannel(std::make_shared<OutputChannel>(executor, capacity)),
            requests(std::make_shared<ClientRequestChannel>(executor, capacity)),
            responses(std::make_shared<ClientResponseChannel>(executor, capacity))
        {}

        std::shared_ptr<OutputChannel> outputChannel;
        std::shared_ptr<ClientRequestChannel> requests;
        std::shared_ptr<ClientResponseChannel> responses;
    };

    struct Fixture
    {
        explicit Fixture(const std::shared_ptr<awl::ILogger>& logger, const std::size_t capacity = 1) :
            server(asio::make_strand(ioContext), logger->createLogger("TransportServer")),
            inputChannel(std::make_shared<InputChannel>(ioContext.get_executor(), capacity)),
            stdio(ioContext.get_executor(), capacity),
            http(ioContext.get_executor(), capacity)
        {
            std::unique_ptr<ITransport> stdio_transport = std::make_unique<TestStdioTransport>(
                inputChannel, stdio.outputChannel, stdio.requests, stdio.responses, logger->createLogger("Stdio"));
            std::unique_ptr<ITransport> http_transport = std::make_unique<TestHttpTransport>(
                inputChannel, http.outputChannel, http.requests, http.responses, logger->createLogger("Http"));
            server.addTransport(std::move(stdio_transport));
            server.addTransport(std::move(http_transport));
        }

        void closeLocalChannels()
        {
            stdio.requests->close();
            http.requests->close();
            stdio.outputChannel->close();
            http.outputChannel->close();
        }

        asio::io_context ioContext;
        TransportServer server;
        std::shared_ptr<InputChannel> inputChannel;
        TestPeer stdio;
        TestPeer http;
    };

    std::string makeRequest(const std::string& method)
    {
        return boost::json::serialize(boost::json::object{
            {"jsonrpc", "2.0"}, {"id", 1}, {"method", method}});
    }

    asio::awaitable<void> asyncSendStdio(TestPeer& peer)
    {
        co_await peer.requests->async_send(boost::system::error_code{},
            ClientMessage{0, makeRequest("stdio")}, asio::use_awaitable);
        co_await peer.requests->async_send(boost::system::error_code{},
            ClientMessage{0, R"({"jsonrpc":"2.0","method":"notice"})"}, asio::use_awaitable);
    }

    asio::awaitable<void> asyncSendHttp(TestPeer& peer)
    {
        // Both HTTP routes and stdio deliberately use the same JSON-RPC ID.
        for (const RouteId route_id : {RouteId{0}, RouteId{7}})
        {
            co_await peer.requests->async_send(boost::system::error_code{},
                ClientMessage{route_id, makeRequest("http")}, asio::use_awaitable);
        }
    }

    asio::awaitable<void> asyncHandleRequests(Fixture& fixture)
    {
        std::size_t stdio_count = 0;
        std::size_t http_count = 0;
        std::size_t notice_count = 0;
        for (std::size_t index = 0; index < 4; ++index)
        {
            IncomingMessage request = co_await fixture.inputChannel->async_receive(asio::use_awaitable);
            const boost::json::object json = boost::json::parse(request.body).as_object();
            const std::string method(json.at("method").as_string());
            if (method == "http")
            {
                ++http_count;
                AWL_ASSERT(request.outputChannel == fixture.http.outputChannel);
                AWL_ASSERT(request.routeId == 0 || request.routeId == 7);
            }
            else
            {
                AWL_ASSERT(request.outputChannel == fixture.stdio.outputChannel);
                AWL_ASSERT(request.routeId == 0);
                if (method == "stdio")
                {
                    ++stdio_count;
                }
                else
                {
                    AWL_ASSERT(method == "notice");
                    ++notice_count;
                }
            }

            if (json.contains("id"))
            {
                co_await request.outputChannel->async_send(boost::system::error_code{},
                    OutgoingMessage{request.routeId, MessageKind::Notification,
                        R"({"jsonrpc":"2.0","method":"notifications/progress"})"}, asio::use_awaitable);
                co_await request.outputChannel->async_send(boost::system::error_code{},
                    OutgoingMessage{request.routeId, MessageKind::Response,
                        boost::json::serialize(boost::json::object{
                            {"jsonrpc", "2.0"}, {"id", json.at("id")}, {"result", method}})}, asio::use_awaitable);
            }
            else
            {
                co_await request.outputChannel->async_send(boost::system::error_code{},
                    OutgoingMessage{request.routeId, MessageKind::Accepted, {}}, asio::use_awaitable);
            }
        }

        AWL_ASSERT(stdio_count == 1);
        AWL_ASSERT(http_count == 2);
        AWL_ASSERT(notice_count == 1);
    }

    asio::awaitable<void> asyncCheckReply(TestPeer& peer, const RouteId route_id,
        const std::string expected_result)
    {
        OutgoingMessage progress = co_await peer.responses->async_receive(asio::use_awaitable);
        AWL_ASSERT(progress.routeId == route_id);
        AWL_ASSERT(progress.kind == MessageKind::Notification);
        AWL_ASSERT(boost::json::parse(progress.body).as_object().at("method") == "notifications/progress");
        OutgoingMessage response = co_await peer.responses->async_receive(asio::use_awaitable);
        AWL_ASSERT(response.routeId == route_id);
        AWL_ASSERT(response.kind == MessageKind::Response);
        const boost::json::object json = boost::json::parse(response.body).as_object();
        AWL_ASSERT(json.at("id") == 1);
        AWL_ASSERT(json.at("result").as_string() == expected_result);
    }

    asio::awaitable<void> asyncCheckStdio(TestPeer& peer)
    {
        co_await asyncCheckReply(peer, 0, "stdio");
        OutgoingMessage accepted = co_await peer.responses->async_receive(asio::use_awaitable);
        AWL_ASSERT(accepted.routeId == 0);
        AWL_ASSERT(accepted.kind == MessageKind::Accepted);
        AWL_ASSERT(accepted.body.empty());
    }

    asio::awaitable<void> asyncCheckHttp(TestPeer& peer)
    {
        co_await asyncCheckReply(peer, 0, "http");
        co_await asyncCheckReply(peer, 7, "http");
    }

    asio::awaitable<void> asyncExchange(Fixture& fixture)
    {
        co_await (asyncSendStdio(fixture.stdio) && asyncSendHttp(fixture.http)
            && asyncHandleRequests(fixture) && asyncCheckStdio(fixture.stdio) && asyncCheckHttp(fixture.http));
    }

    asio::awaitable<void> asyncHttpAfterStdioClose(Fixture& fixture)
    {
        co_await fixture.http.requests->async_send(boost::system::error_code{},
            ClientMessage{42, makeRequest("http")}, asio::use_awaitable);
        IncomingMessage request = co_await fixture.inputChannel->async_receive(asio::use_awaitable);
        AWL_ASSERT(request.routeId == 42);
        AWL_ASSERT(request.body == makeRequest("http"));
        AWL_ASSERT(request.outputChannel == fixture.http.outputChannel);
        co_await request.outputChannel->async_send(boost::system::error_code{},
            OutgoingMessage{42, MessageKind::Response, R"({"jsonrpc":"2.0","id":1,"result":"still running"})"},
            asio::use_awaitable);
        OutgoingMessage response = co_await fixture.http.responses->async_receive(asio::use_awaitable);
        AWL_ASSERT(response.routeId == 42);
        AWL_ASSERT(response.kind == MessageKind::Response);
        AWL_ASSERT(boost::json::parse(response.body).as_object().at("result") == "still running");
    }

    asio::awaitable<void> asyncExchangeAndClose(Fixture& fixture)
    {
        co_await asyncExchange(fixture);
        fixture.stdio.requests->close();
        fixture.stdio.outputChannel->close();
        AWL_ASSERT(fixture.inputChannel->is_open());
        co_await asyncHttpAfterStdioClose(fixture);
        fixture.closeLocalChannels();
    }

    asio::awaitable<void> asyncFullDuplex(Fixture& fixture)
    {
        co_await (fixture.server.asyncRun() && asyncExchangeAndClose(fixture));
        AWL_ASSERT(fixture.inputChannel->is_open());
    }

    asio::awaitable<void> asyncCancelIdle(Fixture& fixture)
    {
        // Successful exchange leaves all readers/writers waiting for more data.
        // Finishing this branch cancels and joins server.asyncRun() via operator||.
        co_await (fixture.server.asyncRun() || asyncExchange(fixture));
        AWL_ASSERT(fixture.inputChannel->is_open());
        AWL_ASSERT(fixture.stdio.outputChannel->is_open());
        AWL_ASSERT(fixture.http.outputChannel->is_open());
    }

    asio::awaitable<void> asyncSendBlockedRequest(Fixture& fixture)
    {
        // Capacity zero: completing this send proves the stdio reader is running.
        // It must then await the shared input channel, which has no consumer.
        co_await fixture.stdio.requests->async_send(boost::system::error_code{},
            ClientMessage{0, makeRequest("stdio")}, asio::use_awaitable);
    }

    asio::awaitable<void> asyncCancelBlockedSend(Fixture& fixture)
    {
        co_await (fixture.server.asyncRun() || asyncSendBlockedRequest(fixture));
        AWL_ASSERT(fixture.inputChannel->is_open());
        AWL_ASSERT_FALSE(fixture.inputChannel->try_receive(
            [](const boost::system::error_code, IncomingMessage) {}));
    }

    void runScenario(Fixture& fixture, asio::awaitable<void> task)
    {
        // CTest also supplies a process timeout; this watchdog reports a test failure.
        const auto executor = asio::make_strand(fixture.ioContext);
        asio::steady_timer watchdog(executor, std::chrono::seconds(5));
        bool timed_out = false;
        bool completed = false;
        std::exception_ptr failure;
        watchdog.async_wait([&](const boost::system::error_code error)
        {
            if (!error)
            {
                timed_out = true;
                fixture.ioContext.stop();
            }
        });
        asio::co_spawn(fixture.ioContext, std::move(task),
            asio::bind_executor(executor, [&](const std::exception_ptr error)
            {
                failure = error;
                completed = true;
                watchdog.cancel();
            }));
        // Transport workers and the handler can execute on different threads.
        std::jthread worker([&] { fixture.ioContext.run(); });
        fixture.ioContext.run();
        worker.join();
        AWL_ASSERT_FALSE(timed_out);
        AWL_ASSERT(completed);
        if (failure)
        {
            std::rethrow_exception(failure);
        }
    }
}

AWL_TEST(TransportFullDuplex)
{
    Fixture fixture(context.logger);
    runScenario(fixture, asyncFullDuplex(fixture));
}

AWL_TEST(TransportCancelIdle)
{
    Fixture fixture(context.logger);
    runScenario(fixture, asyncCancelIdle(fixture));
}

AWL_TEST(TransportCancelBlockedSend)
{
    Fixture fixture(context.logger, 0);
    runScenario(fixture, asyncCancelBlockedSend(fixture));
}
