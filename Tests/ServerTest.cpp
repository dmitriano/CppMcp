#include "Server/Server.h"
#include "Server/RpcError.h"
#include "Tests/TestTransports.h"
#include "Tests/CancellationProbe.h"

#include "Awl/Testing/UnitTest.h"

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>
#include <exception>
#include <format>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

namespace
{
    namespace asio = boost::asio;
    namespace json = boost::json;
    using namespace asio::experimental::awaitable_operators;
    using namespace mcp;
    using namespace mcp::testing;

    template <class Func>
    void assertAwlException(Func&& function)
    {
        bool thrown = false;
        try
        {
            std::forward<Func>(function)();
        }
        catch (const awl::testing::TestException&)
        {
            throw;
        }
        catch (const awl::GeneralException&)
        {
            thrown = true;
        }

        // Outside try: an assertion failure also derives from GeneralException.
        AWL_ASSERT(thrown);
    }

    class EchoHandler : public IHandler
    {
    public:

        asio::awaitable<json::value> asyncHandle(json::object params, RequestContext context) override
        {
            if (const json::value* token = context.meta().if_contains("progressToken"))
            {
                co_await context.asyncNotify("notifications/progress",
                    json::object{{"progressToken", *token}, {"progress", 1}});
            }

            co_return json::object{{"params", std::move(params)}, {"route", context.routeId()},
                {"id", context.requestId().value_or(json::value{})}};
        }
    };

    class FailingHandler : public IHandler
    {
    public:

        explicit FailingHandler(const bool protocol_error) : _protocolError(protocol_error) {}

        asio::awaitable<json::value> asyncHandle(json::object, RequestContext) override
        {
            if (_protocolError)
            {
                throw RpcError(-32602, L"Explicit validation failure", json::object{{"field", "text"}});
            }

            throw awl::GeneralException("Private handler details");
            co_return nullptr;
        }

    private:

        bool _protocolError;
    };

    class EchoTool : public ITool
    {
    public:

        explicit EchoTool(std::string name = "echo", const bool fail = false) :
            _definition{std::move(name), "Echo text",
                json::object{{"type", "object"}, {"properties", json::object{
                    {"text", json::object{{"type", "string"}}}}}, {"required", json::array{"text"}}},
                json::object{{"type", "object"}, {"properties", json::object{
                    {"text", json::object{{"type", "string"}}}}}, {"required", json::array{"text"}}}},
            _fail(fail)
        {}

        const ToolDefinition& definition() const override { return _definition; }

        asio::awaitable<ToolResult> asyncCall(json::object arguments, RequestContext context) override
        {
            if (_fail)
            {
                if (arguments.if_contains("empty_error")) co_return ToolResult{{}, std::nullopt, true};
                if (arguments.if_contains("standard_error")) throw std::invalid_argument("Invalid order list_id 123");
                if (arguments.if_contains("system_error")) throw boost::system::system_error(asio::error::connection_reset);
                throw awl::GeneralException(L"Tool diagnostic: ?????? SQLite");
            }

            const json::value* text = arguments.if_contains("text");
            if (!text || !text->is_string())
            {
                co_return ToolResult{json::array{
                    json::object{{"type", "text"}, {"text", "text must be a string"}}}, std::nullopt, true};
            }

            co_return ToolResult{json::array{json::object{{"type", "text"}, {"text", *text}}},
                json::object{{"text", *text}, {"requestId", context.requestId().value_or(json::value{})},
                    {"routeId", context.routeId()}, {"meta", context.meta()}}, false};
        }

    private:

        ToolDefinition _definition;
        bool _fail;
    };

    class StaticResource : public IResource
    {
    public:

        explicit StaticResource(std::string uri = "test://text", const bool binary = false) :
            _definition{std::move(uri), binary ? "Binary" : "Text", "Test resource",
                binary ? "application/octet-stream" : "text/plain"}, _binary(binary)
        {}

        const ResourceDefinition& definition() const override { return _definition; }

        asio::awaitable<std::vector<ResourceContents>> asyncRead(RequestContext) override
        {
            ResourceContents item{_definition.uri, _definition.mimeType, TextResource{"hello"}};
            if (_binary)
            {
                item.contents = BlobResource{"AQID"};
            }

            co_return std::vector<ResourceContents>{std::move(item)};
        }

    private:

        ResourceDefinition _definition;
        bool _binary;
    };

    class BlockingHandler : public IHandler
    {
    public:

        BlockingHandler(SignalChannel& entered, bool& finished) : _probe(entered, finished) {}

        asio::awaitable<json::value> asyncHandle(json::object, RequestContext) override
        {
            co_await _probe.asyncWait();
            co_return nullptr;
        }

    private:

        CancellationProbe _probe;
    };

    class BlockingTool : public ITool
    {
    public:

        BlockingTool(SignalChannel& entered, bool& finished) : _probe(entered, finished) {}

        const ToolDefinition& definition() const override { return _definition; }

        asio::awaitable<ToolResult> asyncCall(json::object, RequestContext) override
        {
            co_await _probe.asyncWait();
            co_return ToolResult{};
        }

    private:

        ToolDefinition _definition{"block", "Wait for cancellation", json::object{{"type", "object"}}, std::nullopt};
        CancellationProbe _probe;
    };

    class OutputProbeHandler : public IHandler
    {
    public:

        explicit OutputProbeHandler(SignalChannel& entered) : _entered(entered) {}

        asio::awaitable<json::value> asyncHandle(json::object, RequestContext) override
        {
            // Receiver runs later on the same strand. This coroutine returns
            // synchronously, so the response send starts before it wakes up.
            AWL_ASSERT(_entered.try_send(boost::system::error_code{}, true));
            co_return json::object{};
        }

    private:

        SignalChannel& _entered;
    };

    struct Peer
    {
        Peer(const asio::any_io_executor& executor, const std::size_t capacity = 1) :
            output(std::make_shared<OutputChannel>(executor, capacity)),
            requests(std::make_shared<ClientRequestChannel>(executor, capacity)),
            responses(std::make_shared<ClientResponseChannel>(executor, capacity))
        {}

        void close()
        {
            requests->close();
            output->close();
        }

        std::shared_ptr<OutputChannel> output;
        std::shared_ptr<ClientRequestChannel> requests;
        std::shared_ptr<ClientResponseChannel> responses;
    };

    struct Fixture
    {
        explicit Fixture(const std::shared_ptr<awl::ILogger>& root_logger) :
            logger(root_logger), executor(asio::make_strand(ioContext)),
            input(std::make_shared<InputChannel>(executor, 1)),
            server(executor, input, logger->createLogger("Server")),
            stdio(executor), http(executor)
        {
            std::unique_ptr<ITransport> stdio_transport = std::make_unique<TestStdioTransport>(
                input, stdio.output, stdio.requests, stdio.responses, logger->createLogger("Stdio"));
            std::unique_ptr<ITransport> http_transport = std::make_unique<TestHttpTransport>(
                input, http.output, http.requests, http.responses, logger->createLogger("Http"));
            server.addTransport(std::move(stdio_transport));
            server.addTransport(std::move(http_transport));
        }

        void registerHandlers()
        {
            server.addHandler("test/echo", std::make_unique<EchoHandler>());
            server.addHandler("test/fail", std::make_unique<FailingHandler>(false));
            server.addHandler("test/invalid", std::make_unique<FailingHandler>(true));
            // Reverse insertion order exercises deterministic listing.
            server.addTool(std::make_unique<EchoTool>("fail", true));
            server.addTool(std::make_unique<EchoTool>());
            server.addResource(std::make_unique<StaticResource>());
            server.addResource(std::make_unique<StaticResource>("test://binary", true));
        }

        const std::shared_ptr<awl::ILogger> logger;
        asio::io_context ioContext;
        asio::any_io_executor executor;
        std::shared_ptr<InputChannel> input;
        Server server;
        Peer stdio;
        Peer http;
    };

    std::string makeRequest(std::string method, json::object params = {}, json::value id = 1)
    {
        return json::serialize(json::object{{"jsonrpc", "2.0"}, {"method", std::move(method)},
            {"params", std::move(params)}, {"id", std::move(id)}});
    }

    asio::awaitable<OutgoingMessage> asyncExchange(Peer& peer, const RouteId route_id, std::string body)
    {
        co_await peer.requests->async_send(boost::system::error_code{},
            ClientMessage{route_id, std::move(body)}, asio::use_awaitable);
        OutgoingMessage reply = co_await peer.responses->async_receive(asio::use_awaitable);
        AWL_ASSERT(reply.routeId == route_id);
        co_return reply;
    }

    asio::awaitable<json::object> asyncCall(Peer& peer, const RouteId route_id,
        std::string method, json::object params = {}, json::value id = 1)
    {
        const json::value expected_id = id;
        OutgoingMessage reply = co_await asyncExchange(peer, route_id,
            makeRequest(std::move(method), std::move(params), std::move(id)));
        AWL_ASSERT(reply.kind == MessageKind::Response);
        json::object response = json::parse(reply.body).as_object();
        AWL_ASSERT(response.at("jsonrpc") == "2.0");
        AWL_ASSERT(response.at("id") == expected_id);
        co_return response;
    }

    asio::awaitable<void> asyncCheckPeer(Peer& peer, const RouteId route_id, const std::string marker)
    {
        // Same RPC ID and route 0 are deliberately used on both transports.
        OutgoingMessage progress = co_await asyncExchange(peer, route_id,
            makeRequest("test/echo", json::object{{"text", marker},
                {"_meta", json::object{{"progressToken", marker}}}}));
        AWL_ASSERT(progress.kind == MessageKind::Notification);
        const json::object notification = json::parse(progress.body).as_object();
        AWL_ASSERT(notification.at("method") == "notifications/progress");
        AWL_ASSERT(notification.at("params").as_object().at("progress") == 1);
        AWL_ASSERT(notification.at("params").as_object().at("progressToken").as_string() == marker);
        AWL_ASSERT_FALSE(notification.contains("id"));
        OutgoingMessage reply = co_await peer.responses->async_receive(asio::use_awaitable);
        AWL_ASSERT(reply.kind == MessageKind::Response);
        AWL_ASSERT(reply.routeId == route_id);
        const json::object echo = json::parse(reply.body).as_object();
        AWL_ASSERT(echo.at("id") == 1);
        const json::object& echo_result = echo.at("result").as_object();
        AWL_ASSERT(echo_result.at("params").as_object().at("text").as_string() == marker);
        AWL_ASSERT(echo_result.at("route").as_int64() == static_cast<std::int64_t>(route_id));
        AWL_ASSERT(echo_result.at("id") == 1);

        const json::object tools = co_await asyncCall(peer, route_id, "tools/list");
        const json::array& definitions = tools.at("result").as_object().at("tools").as_array();
        AWL_ASSERT(definitions.size() == 2);
        AWL_ASSERT(definitions[0].as_object().at("name") == "echo");
        AWL_ASSERT(definitions[1].as_object().at("name") == "fail");
        AWL_ASSERT(definitions[0].as_object().at("description") == "Echo text");
        AWL_ASSERT(definitions[0].as_object().at("inputSchema").as_object().at("type") == "object");
        AWL_ASSERT(definitions[0].as_object().contains("outputSchema"));

        const json::object call = co_await asyncCall(peer, route_id, "tools/call",
            json::object{{"name", "echo"}, {"arguments", json::object{{"text", marker}}},
                {"_meta", json::object{{"test", marker}}}}, "tool-id");
        const json::object& tool_result = call.at("result").as_object();
        AWL_ASSERT(tool_result.at("isError") == false);
        AWL_ASSERT(tool_result.at("content").as_array()[0].as_object().at("text").as_string() == marker);
        AWL_ASSERT(tool_result.at("structuredContent").as_object().at("text").as_string() == marker);
        const json::object& structured = tool_result.at("structuredContent").as_object();
        AWL_ASSERT(structured.at("requestId") == "tool-id");
        AWL_ASSERT(structured.at("routeId").as_int64() == static_cast<std::int64_t>(route_id));
        AWL_ASSERT(structured.at("meta").as_object().at("test").as_string() == marker);

        const json::object resources = co_await asyncCall(peer, route_id, "resources/list");
        const json::array& resource_list = resources.at("result").as_object().at("resources").as_array();
        AWL_ASSERT(resource_list.size() == 2);
        AWL_ASSERT(resource_list[0].as_object().at("uri") == "test://binary");
        AWL_ASSERT(resource_list[1].as_object().at("uri") == "test://text");
        AWL_ASSERT(resource_list[1].as_object().at("mimeType") == "text/plain");
        for (const bool binary : {false, true})
        {
            const std::string uri = binary ? "test://binary" : "test://text";
            const json::object read = co_await asyncCall(peer, route_id, "resources/read", json::object{{"uri", uri}});
            const json::array& contents = read.at("result").as_object().at("contents").as_array();
            AWL_ASSERT(contents.size() == 1);
            const json::object& item = contents[0].as_object();
            AWL_ASSERT(item.at("uri").as_string() == uri);
            AWL_ASSERT(item.at(binary ? "blob" : "text") == (binary ? "AQID" : "hello"));
            AWL_ASSERT_FALSE(item.contains(binary ? "text" : "blob"));
        }

        OutgoingMessage accepted = co_await asyncExchange(peer, route_id,
            R"({"jsonrpc":"2.0","method":"test/echo","params":{"text":"notice"}})");
        AWL_ASSERT(accepted.kind == MessageKind::Accepted);
        AWL_ASSERT(accepted.body.empty());
    }

    void checkRegistrationFrozen(Fixture& fixture)
    {
        assertAwlException([&]
        {
            fixture.server.addHandler("late", std::make_unique<EchoHandler>());
        });
        assertAwlException([&] { fixture.server.addTool(std::make_unique<EchoTool>("late")); });
        assertAwlException([&]
        {
            fixture.server.addResource(std::make_unique<StaticResource>("test://late"));
        });
        std::unique_ptr<ITransport> transport = std::make_unique<TestHttpTransport>(
            fixture.input, fixture.http.output, fixture.http.requests, fixture.http.responses,
            fixture.logger->createLogger("LateHttp"));
        assertAwlException([&] { fixture.server.addTransport(std::move(transport)); });
    }

    asio::awaitable<void> asyncClientsAndClose(Fixture& fixture)
    {
        co_await (asyncCheckPeer(fixture.stdio, 0, "stdio") && asyncCheckPeer(fixture.http, 0, "http"));
        checkRegistrationFrozen(fixture);
        fixture.stdio.close();
        AWL_ASSERT(fixture.input->is_open());
        co_await asyncCheckPeer(fixture.http, 7, "second HTTP route");
        fixture.http.close();
    }

    asio::awaitable<void> asyncFullDuplex(Fixture& fixture)
    {
        co_await (fixture.server.asyncRun() && asyncClientsAndClose(fixture));
        AWL_ASSERT_FALSE(fixture.input->is_open());
        bool rejected = false;
        try
        {
            co_await fixture.server.asyncRun();
        }
        catch (const awl::GeneralException&)
        {
            rejected = true;
        }

        AWL_ASSERT(rejected);
    }

    asio::awaitable<void> asyncCheckError(Peer& peer, std::string body,
        const int code, json::value expected_id = 1)
    {
        OutgoingMessage reply = co_await asyncExchange(peer, 9, std::move(body));
        AWL_ASSERT(reply.kind == MessageKind::Response);
        const json::object response = json::parse(reply.body).as_object();
        AWL_ASSERT(response.at("id") == expected_id);
        AWL_ASSERT(response.at("error").as_object().at("code") == code);
        AWL_ASSERT_FALSE(response.contains("result"));
        AWL_ASSERT(reply.body.find("Private") == std::string::npos);
    }

    asio::awaitable<void> asyncErrors(Peer& peer)
    {
        co_await asyncCheckError(peer, "{bad json", -32700, nullptr);
        for (const std::string body : {"[]", "null", "42",
            R"({"jsonrpc":"1.0","method":"tools/list","id":1})",
            R"({"jsonrpc":"2.0","method":5,"id":1})",
            R"({"jsonrpc":"2.0","method":"tools/list","id":null})"})
        {
            co_await asyncCheckError(peer, body, -32600, nullptr);
        }

        co_await asyncCheckError(peer, makeRequest("missing"), -32601);
        co_await asyncCheckError(peer,
            R"({"jsonrpc":"2.0","method":"tools/list","params":[],"id":"array"})", -32602, "array");
        co_await asyncCheckError(peer, makeRequest("tools/call"), -32602);
        co_await asyncCheckError(peer, makeRequest("tools/call", json::object{{"name", "missing"}}), -32602);
        co_await asyncCheckError(peer, makeRequest("tools/call",
            json::object{{"name", "echo"}, {"arguments", json::array{1}}}), -32602);
        co_await asyncCheckError(peer, makeRequest("tools/list", json::object{{"_meta", "bad"}}), -32602);
        co_await asyncCheckError(peer, makeRequest("tools/list", json::object{{"cursor", "next"}}), -32602);
        co_await asyncCheckError(peer, makeRequest("resources/read", json::object{{"uri", 123}}), -32602);
        co_await asyncCheckError(peer, makeRequest("test/fail"), -32603);
        const json::object invalid = co_await asyncCall(peer, 9, "test/invalid");
        AWL_ASSERT(invalid.at("error").as_object().at("code") == -32602);
        AWL_ASSERT(invalid.at("error").as_object().at("message") == "Explicit validation failure");
        AWL_ASSERT(invalid.at("error").as_object().at("data").as_object().at("field") == "text");
        const json::object missing = co_await asyncCall(peer, 9, "resources/read", json::object{{"uri", "test://missing"}});
        AWL_ASSERT(missing.at("error").as_object().at("code") == -32602);
        AWL_ASSERT(missing.at("error").as_object().at("data").as_object().at("uri") == "test://missing");

        for (const std::string name : {"echo", "fail"})
        {
            const json::object call = co_await asyncCall(peer, 9, "tools/call", json::object{{"name", name}});
            AWL_ASSERT_FALSE(call.contains("error"));
            const json::object& result = call.at("result").as_object();
            AWL_ASSERT(result.at("isError") == true);
            AWL_ASSERT_FALSE(result.at("content").as_array().empty());
            const std::string text(result.at("content").as_array()[0].as_object().at("text").as_string());
            AWL_ASSERT(text == (name == "fail" ? "Tool 'fail' failed: Tool diagnostic: ?????? SQLite" : "text must be a string"));
        }

        for (const std::string field : {"standard_error", "system_error", "empty_error"})
        {
            const json::object call = co_await asyncCall(peer, 9, "tools/call", json::object{
                {"name", "fail"}, {"arguments", json::object{{field, true}}}});
            const auto& result = call.at("result").as_object();
            AWL_ASSERT(result.at("isError") == true);
            const std::string text(result.at("content").as_array()[0].as_object().at("text").as_string());
            if (field == "empty_error") AWL_ASSERT(text == "Tool 'fail' failed without a diagnostic message.");
            else
            {
                const std::string reason = field == "standard_error" ? "Invalid order list_id 123" :
                    boost::system::system_error(asio::error::connection_reset).what();
                AWL_ASSERT(text == std::format("Tool 'fail' failed: {}", reason));
            }
        }

        // Even failing and unknown notifications must not produce RPC errors.
        for (const std::string method : {"missing", "test/fail", "test/invalid"})
        {
            OutgoingMessage accepted = co_await asyncExchange(peer, 9,
                json::serialize(json::object{{"jsonrpc", "2.0"}, {"method", method}}));
            AWL_ASSERT(accepted.kind == MessageKind::Accepted);
            AWL_ASSERT(accepted.body.empty());
        }

        const json::object recovery = co_await asyncCall(peer, 9, "tools/call", json::object{
            {"name", "echo"}, {"arguments", json::object{{"text", "recovered"}}}});
        AWL_ASSERT(recovery.at("result").as_object().at("isError") == false);
    }

    asio::awaitable<void> asyncErrorScenario(Fixture& fixture)
    {
        co_await (fixture.server.asyncRun() || asyncErrors(fixture.http));
    }

    asio::awaitable<void> asyncEnterHandler(Fixture& fixture, SignalChannel& entered, std::string request)
    {
        co_await fixture.http.requests->async_send(boost::system::error_code{},
            ClientMessage{9, std::move(request)}, asio::use_awaitable);
        AWL_ASSERT(co_await entered.async_receive(asio::use_awaitable));
    }

    asio::awaitable<void> asyncCancelHandler(Fixture& fixture, SignalChannel& entered,
        bool& finished, std::string request)
    {
        co_await (fixture.server.asyncRun() || asyncEnterHandler(fixture, entered, std::move(request)));
        AWL_ASSERT(finished);
        AWL_ASSERT_FALSE(fixture.http.responses->try_receive(
            [](const boost::system::error_code, OutgoingMessage) {}));
        AWL_ASSERT(fixture.input->is_open());
    }

    asio::awaitable<void> asyncFillOutput(Fixture& fixture, SignalChannel& entered)
    {
        // The HTTP writer blocks at the unconsumed client response channel.
        // Read a stdio reply behind the queued HTTP requests to prove dispatch
        // reached the final request before cancellation, without timing sleeps.
        for (std::size_t index = 0; index < 3; ++index)
        {
            co_await fixture.http.requests->async_send(boost::system::error_code{},
                ClientMessage{9, makeRequest("tools/list")}, asio::use_awaitable);
        }

        const json::object reply = co_await asyncCall(fixture.stdio, 0, "tools/list");
        AWL_ASSERT(reply.contains("result"));
        co_await fixture.http.requests->async_send(boost::system::error_code{},
            ClientMessage{9, makeRequest("test/probe")}, asio::use_awaitable);
        AWL_ASSERT(co_await entered.async_receive(asio::use_awaitable));
        AWL_ASSERT(fixture.http.output->ready());
    }

    asio::awaitable<void> asyncCancelOutput(Fixture& fixture, SignalChannel& entered)
    {
        co_await (fixture.server.asyncRun() || asyncFillOutput(fixture, entered));
        AWL_ASSERT(fixture.input->is_open());
        AWL_ASSERT(fixture.http.output->is_open());
    }

    asio::awaitable<void> asyncCheckEmpty(Fixture& fixture)
    {
        for (const bool tools : {true, false})
        {
            const json::object reply = co_await asyncCall(fixture.stdio, 0, tools ? "tools/list" : "resources/list");
            AWL_ASSERT(reply.at("result").as_object().at(tools ? "tools" : "resources").as_array().empty());
        }
    }

    asio::awaitable<void> asyncEmptyRegistries(Fixture& fixture)
    {
        co_await (fixture.server.asyncRun() || asyncCheckEmpty(fixture));
    }

    asio::awaitable<void> asyncSendToClosedOutput(Fixture& fixture)
    {
        std::shared_ptr<OutputChannel> output = std::make_shared<OutputChannel>(fixture.executor, 1);
        output->close();
        co_await fixture.input->async_send(boost::system::error_code{},
            IncomingMessage{77, makeRequest("tools/list"), output}, asio::use_awaitable);
    }

    asio::awaitable<void> asyncOutputFailure(Fixture& fixture)
    {
        bool failed = false;
        try
        {
            // No subsequent input arrives to wake the dispatcher. A failed
            // reply must wake it and join the idle transports on its own.
            co_await (fixture.server.asyncRun() && asyncSendToClosedOutput(fixture));
        }
        catch (const boost::system::system_error& error)
        {
            AWL_ASSERT(error.code() == asio::experimental::error::channel_closed);
            failed = true;
        }

        AWL_ASSERT(failed);
    }

    void runScenario(Fixture& fixture, asio::awaitable<void> task)
    {
        asio::steady_timer watchdog(fixture.executor, std::chrono::seconds(5));
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
        asio::co_spawn(fixture.executor, std::move(task),
            asio::bind_executor(fixture.executor, [&](const std::exception_ptr error)
            {
                failure = error;
                completed = true;
                watchdog.cancel();
            }));
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

AWL_TEST(ServerRegistration)
{
    Fixture fixture(context.logger);
    fixture.registerHandlers();
    for (const std::string method : {"test/echo", "", "initialize", "notifications/initialized", "ping",
        "server/discover", "tools/list", "tools/call", "resources/list", "resources/read", "rpc.reserved"})
    {
        assertAwlException([&]
        {
            fixture.server.addHandler(method, std::make_unique<EchoHandler>());
        });
    }

    assertAwlException([&] { fixture.server.addTool(std::make_unique<EchoTool>()); });
    assertAwlException([&] { fixture.server.addTool(std::make_unique<EchoTool>("")); });
    assertAwlException([&] { fixture.server.addResource(std::make_unique<StaticResource>()); });
    assertAwlException([&] { fixture.server.addResource(std::make_unique<StaticResource>("")); });
}

AWL_TEST(ServerFullDuplex)
{
    Fixture fixture(context.logger);
    fixture.registerHandlers();
    runScenario(fixture, asyncFullDuplex(fixture));
}

AWL_TEST(ServerErrors)
{
    Fixture fixture(context.logger);
    fixture.registerHandlers();
    runScenario(fixture, asyncErrorScenario(fixture));
}

AWL_TEST(ServerCancelHandler)
{
    Fixture fixture(context.logger);
    SignalChannel entered(fixture.executor, 0);
    bool finished = false;
    fixture.server.addHandler("test/block", std::make_unique<BlockingHandler>(entered, finished));
    runScenario(fixture, asyncCancelHandler(fixture, entered, finished, makeRequest("test/block")));
}

AWL_TEST(ServerCancelTool)
{
    Fixture fixture(context.logger);
    SignalChannel entered(fixture.executor, 0);
    bool finished = false;
    fixture.server.addTool(std::make_unique<BlockingTool>(entered, finished));
    runScenario(fixture, asyncCancelHandler(fixture, entered, finished,
        makeRequest("tools/call", json::object{{"name", "block"}})));
}

AWL_TEST(ServerCancelOutput)
{
    Fixture fixture(context.logger);
    SignalChannel entered(fixture.executor, 1);
    fixture.server.addHandler("test/probe", std::make_unique<OutputProbeHandler>(entered));
    runScenario(fixture, asyncCancelOutput(fixture, entered));
}

AWL_TEST(ServerEmptyRegistries)
{
    Fixture fixture(context.logger);
    runScenario(fixture, asyncEmptyRegistries(fixture));
}

AWL_TEST(ServerOutputFailure)
{
    Fixture fixture(context.logger);
    runScenario(fixture, asyncOutputFailure(fixture));
}
