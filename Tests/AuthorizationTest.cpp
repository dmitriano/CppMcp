#include "Server/Server.h"
#include "Tests/TestTransports.h"
#include "Tests/CancellationProbe.h"

#include "Awl/Testing/UnitTest.h"

#include <boost/asio.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <algorithm>
#include <exception>
#include <memory>
#include <string>
#include <vector>

namespace
{
    namespace asio = boost::asio;
    namespace json = boost::json;
    using namespace asio::experimental::awaitable_operators;

    class Authorizer : public mcp::IAuthorizer
    {
    public:

        asio::awaitable<bool> asyncIsAllowed(std::string token, std::string permission) override
        {
            // Suspend to exercise per-request credential ownership and concurrency.
            co_await asio::post(asio::use_awaitable);

            if (token == "storage-error")
            {
                throw awl::GeneralException("Authorization storage unavailable");
            }

            co_return (permission == "read" && (token == "reader" || token == "writer")) ||
                (permission == "write" && token == "writer");
        }
    };

    class ProbeTool : public mcp::ITool
    {
    public:

        ProbeTool(std::string name, std::vector<std::string> permissions, int& calls) :
            _definition{std::move(name), "Count admitted calls", json::object{{"type", "object"}},
                std::nullopt, std::move(permissions)}, _calls(calls)
        {}

        const mcp::ToolDefinition& definition() const override { return _definition; }

        asio::awaitable<mcp::ToolResult> asyncCall(json::object, mcp::RequestContext) override
        {
            ++_calls;
            co_return mcp::ToolResult{{}, json::object{{"called", true}}, false};
        }

    private:

        const mcp::ToolDefinition _definition;
        int& _calls;
    };

    class BlockingAuthorizer : public mcp::IAuthorizer
    {
    public:

        BlockingAuthorizer(mcp::testing::SignalChannel& entered, bool& finished) : _probe(entered, finished) {}

        asio::awaitable<bool> asyncIsAllowed(std::string, std::string) override
        {
            co_await _probe.asyncWait();
            co_return true;
        }

    private:

        mcp::testing::CancellationProbe _probe;
    };

    asio::awaitable<void> asyncCancelAuthorization(mcp::testing::SignalChannel& entered,
        asio::cancellation_signal& cancellation)
    {
        co_await entered.async_receive(asio::use_awaitable);
        cancellation.emit(asio::cancellation_type::terminal);
    }

    class ProbeHandler : public mcp::IHandler
    {
    public:

        explicit ProbeHandler(int& calls) : _calls(calls) {}

        asio::awaitable<json::value> asyncHandle(json::object, mcp::RequestContext) override
        {
            ++_calls;
            co_return json::object{};
        }

    private:

        int& _calls;
    };

    class ProbeResource : public mcp::IResource
    {
    public:

        const mcp::ResourceDefinition& definition() const override { return _definition; }

        asio::awaitable<std::vector<mcp::ResourceContents>> asyncRead(mcp::RequestContext) override
        {
            co_return std::vector<mcp::ResourceContents>{};
        }

    private:

        const mcp::ResourceDefinition _definition{"test://restricted", "restricted", "Restricted resource", "text/plain", {"read"}};
    };

    struct Fixture
    {
        explicit Fixture(const std::shared_ptr<awl::ILogger>& logger) :
            executor(asio::make_strand(io)), input(std::make_shared<mcp::InputChannel>(executor, 8)),
            output(std::make_shared<mcp::OutputChannel>(executor, 8)),
            requests(std::make_shared<mcp::testing::ClientRequestChannel>(executor, 8)),
            responses(std::make_shared<mcp::testing::ClientResponseChannel>(executor, 8)),
            server(executor, input, logger)
        {
            server.addTransport(std::make_unique<mcp::testing::TestHttpTransport>(
                input, output, requests, responses, logger));
            server.addTool(std::make_unique<ProbeTool>("open", std::vector<std::string>{}, calls));
            server.addTool(std::make_unique<ProbeTool>("read", std::vector<std::string>{"read"}, calls));
            server.addTool(std::make_unique<ProbeTool>("write", std::vector<std::string>{"write"}, calls));
            server.addTool(std::make_unique<ProbeTool>("both", std::vector<std::string>{"read", "write"}, calls));
            server.addHandler("test/stop", std::make_unique<ProbeHandler>(calls), {"write"});
            server.addResource(std::make_unique<ProbeResource>());
        }

        asio::io_context io;
        asio::any_io_executor executor;
        std::shared_ptr<mcp::InputChannel> input;
        std::shared_ptr<mcp::OutputChannel> output;
        std::shared_ptr<mcp::testing::ClientRequestChannel> requests;
        std::shared_ptr<mcp::testing::ClientResponseChannel> responses;
        mcp::Server server;
        int calls = 0;
    };

    asio::awaitable<json::object> asyncRequest(Fixture& fixture, std::string method,
        json::object params, mcp::Authorization authorization)
    {
        const std::string body = json::serialize(json::object{
            {"jsonrpc", "2.0"}, {"id", 1}, {"method", std::move(method)}, {"params", std::move(params)}});
        co_await fixture.requests->async_send(boost::system::error_code{},
            mcp::testing::ClientMessage{1, body, std::move(authorization)}, asio::use_awaitable);
        const auto response = co_await fixture.responses->async_receive(asio::use_awaitable);
        co_return json::parse(response.body).as_object();
    }

    asio::awaitable<void> asyncPermissions(Fixture& fixture)
    {
        std::shared_ptr<mcp::IAuthorizer> authorizer = std::make_shared<Authorizer>();

        for (const std::string token : {"", "wrong", "reader", "writer"})
        {
            const mcp::Authorization authorization(authorizer, token);
            const auto listing = co_await asyncRequest(fixture, "tools/list", {}, authorization);
            const auto& tools = listing.at("result").as_object().at("tools").as_array();
            const auto expected_size = token == "writer" ? 4u : token == "reader" ? 2u : 1u;

            AWL_ASSERT_EQUAL(expected_size, tools.size());

            for (const std::string name : {"open", "read", "write", "both"})
            {
                const bool allowed = name == "open" || token == "writer" || (token == "reader" && name == "read");
                const auto listed = std::ranges::any_of(tools, [&](const auto& tool) { return std::string(tool.at("name").as_string()) == name; });
                AWL_ASSERT(listed == allowed);
                const int previous_calls = fixture.calls;
                // Client metadata must never grant authorization.
                const auto result = co_await asyncRequest(fixture, "tools/call", json::object{
                    {"name", name}, {"arguments", json::object{}},
                    {"_meta", json::object{{"token", "writer"}, {"permissions", json::array{"write"}}}}}, authorization);
                AWL_ASSERT(result.contains("result") == allowed);
                AWL_ASSERT_EQUAL(previous_calls + (allowed ? 1 : 0), fixture.calls);

                if (!allowed)
                {
                    AWL_ASSERT(result.at("error").at("code") == -32003);
                }
            }

            const int previous_calls = fixture.calls;
            const auto stop = co_await asyncRequest(fixture, "test/stop", {}, authorization);
            AWL_ASSERT(stop.contains("result") == (token == "writer"));
            AWL_ASSERT_EQUAL(previous_calls + (token == "writer" ? 1 : 0), fixture.calls);
            const auto resources = co_await asyncRequest(fixture, "resources/list", {}, authorization);
            AWL_ASSERT_EQUAL(token == "reader" || token == "writer" ? 1u : 0u,
                resources.at("result").at("resources").as_array().size());
            const auto resource = co_await asyncRequest(fixture, "resources/read",
                json::object{{"uri", "test://restricted"}}, authorization);
            AWL_ASSERT(resource.contains("result") == (token == "reader" || token == "writer"));
        }

        const auto trusted = co_await asyncRequest(fixture, "tools/list", {}, mcp::Authorization::trusted());
        AWL_ASSERT_EQUAL(std::size_t{4}, trusted.at("result").at("tools").as_array().size());
        const auto anonymous = co_await asyncRequest(fixture, "tools/list", {}, {});
        AWL_ASSERT_EQUAL(std::size_t{1}, anonymous.at("result").at("tools").as_array().size());
        const auto denied = co_await asyncRequest(fixture, "tools/call", json::object{{"name", "write"}}, {});
        AWL_ASSERT(denied.at("error").at("code") == -32003);

        const auto error = co_await asyncRequest(fixture, "tools/call", json::object{{"name", "write"}},
            mcp::Authorization(authorizer, "storage-error"));
        AWL_ASSERT(error.at("error").at("code") == -32603);
        const auto recovered = co_await asyncRequest(fixture, "tools/list", {}, mcp::Authorization(authorizer, "writer"));
        AWL_ASSERT_EQUAL(std::size_t{4}, recovered.at("result").at("tools").as_array().size());
    }
}

AWL_TEST(ServerPermissions)
{
    Fixture fixture(context.logger);
    std::exception_ptr failure;
    asio::steady_timer watchdog(fixture.executor, std::chrono::seconds(5));
    bool timed_out = false;
    watchdog.async_wait([&](const boost::system::error_code error)
    {
        if (!error)
        {
            timed_out = true;
            fixture.io.stop();
        }
    });
    asio::co_spawn(fixture.executor, fixture.server.asyncRun() || asyncPermissions(fixture),
        [&](std::exception_ptr error, auto)
        {
            failure = error;
            watchdog.cancel();
        });
    fixture.io.run();
    AWL_ASSERT_FALSE(timed_out);

    if (failure)
    {
        std::rethrow_exception(failure);
    }
}

AWL_TEST(AuthorizationCancellation)
{
    AWL_UNUSED_CONTEXT;
    asio::io_context io;
    const auto executor = asio::make_strand(io);
    mcp::testing::SignalChannel entered(executor, 1);
    bool finished = false;
    std::shared_ptr<mcp::IAuthorizer> authorizer = std::make_shared<BlockingAuthorizer>(entered, finished);
    const mcp::Authorization authorization(authorizer, "token");
    asio::cancellation_signal cancellation;
    std::exception_ptr failure;
    std::exception_ptr coordinator_failure;
    asio::steady_timer watchdog(executor, std::chrono::seconds(5));
    bool timed_out = false;
    watchdog.async_wait([&](const boost::system::error_code error)
    {
        if (!error)
        {
            timed_out = true;
            io.stop();
        }
    });
    asio::co_spawn(executor, authorization.asyncHasPermissions({"read"}),
        asio::bind_cancellation_slot(cancellation.slot(), [&](const std::exception_ptr error, bool)
        {
            failure = error;
            watchdog.cancel();
        }));
    asio::co_spawn(executor, asyncCancelAuthorization(entered, cancellation),
        [&](const std::exception_ptr error) { coordinator_failure = error; });
    io.run();
    AWL_ASSERT_FALSE(timed_out);
    AWL_ASSERT(coordinator_failure == nullptr);
    AWL_ASSERT(finished);
    AWL_ASSERT(failure != nullptr);

    try
    {
        std::rethrow_exception(failure);
    }
    catch (const boost::system::system_error& error)
    {
        AWL_ASSERT(error.code() == asio::error::operation_aborted);
    }
}
