#include "Server/TypedTool.h"
#include "Server/Server.h"
#include "Tests/CancellationProbe.h"
#include "Tests/TestTransports.h"
#include "Tests/JsonSchemaTestData.h"

#include "Awl/Testing/UnitTest.h"

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/strand.hpp>
#include <chrono>
#include <exception>
#include <limits>
#include <memory>
#include <thread>

namespace
{
    AWL_SEQUENTIAL_ENUM(Mode, first, second)
}

AWL_ENUM_TRAITS(, Mode)

namespace
{
    namespace asio = boost::asio;
    namespace json = boost::json;
    using namespace asio::experimental::awaitable_operators;
    using namespace mcp;
    using namespace mcp::testing;

    struct SumInput
    {
        std::int32_t a{};
        std::int32_t b = 9;
        std::optional<std::string> note;

        AWL_REFLECT(a, b, note)
    };

    struct SumOutput
    {
        std::int64_t sum{};
        std::optional<std::string> note;

        AWL_REFLECT(sum, note)
    };

    struct Item
    {
        std::string label;
        std::int8_t count{};

        AWL_REFLECT(label, count)
    };

    struct Record
    {
        std::vector<Item> items;
        std::optional<std::string> note;
        Mode mode{};
        bool enabled{};
        float factor{};
        std::uint64_t serial{};

        AWL_REFLECT(items, note, mode, enabled, factor, serial)
    };

    struct Empty : awl::EmptyStringizable {};

    struct FloatingOutput
    {
        double value{};

        AWL_REFLECT(value)
    };

    class SumTool : public TypedTool<SumInput, SumOutput>
    {
    public:

        using TypedTool::TypedTool;

    protected:

        asio::awaitable<SumOutput> asyncExecute(SumInput input, RequestContext context) override
        {
            if (const json::value* token = context.meta().if_contains("progressToken"))
            {
                co_await context.asyncNotify("notifications/progress",
                    json::object{{"progressToken", *token}, {"progress", 1}});
            }

            co_return SumOutput{static_cast<std::int64_t>(input.a) + input.b, std::move(input.note)};
        }
    };

    class RecordTool : public TypedTool<Record, Record>
    {
    public:

        using TypedTool::TypedTool;

    protected:

        asio::awaitable<Record> asyncExecute(Record input, RequestContext) override
        {
            co_return input;
        }
    };

    class BrokenTool : public TypedTool<Empty, FloatingOutput>
    {
    public:

        BrokenTool(const bool bad_output, const std::shared_ptr<awl::ILogger>& logger) :
            TypedTool(bad_output ? "bad_output" : "fail", "Test failure", logger), _badOutput(bad_output)
        {}

    protected:

        asio::awaitable<FloatingOutput> asyncExecute(Empty, RequestContext) override
        {
            if (!_badOutput)
            {
                throw awl::GeneralException("Private typed tool details");
            }

            co_return FloatingOutput{std::numeric_limits<double>::infinity()};
        }

    private:

        bool _badOutput;
    };

    class BlockingTypedTool : public TypedTool<Empty, Empty>
    {
    public:

        BlockingTypedTool(SignalChannel& entered, bool& finished, const std::shared_ptr<awl::ILogger>& logger) :
            TypedTool("block", "Wait for cancellation", logger), _probe(entered, finished)
        {}

    protected:

        asio::awaitable<Empty> asyncExecute(Empty, RequestContext) override
        {
            co_await _probe.asyncWait();
            co_return Empty{};
        }

    private:

        CancellationProbe _probe;
    };

    template <awl::reflectable T>
    class EchoTypedTool : public TypedTool<T, T>
    {
    public:

        using TypedTool<T, T>::TypedTool;

    protected:

        asio::awaitable<T> asyncExecute(T input, RequestContext) override
        {
            co_return std::move(input);
        }
    };

    struct Peer
    {
        explicit Peer(const asio::any_io_executor& executor) :
            output(std::make_shared<OutputChannel>(executor, 1)),
            requests(std::make_shared<ClientRequestChannel>(executor, 1)),
            responses(std::make_shared<ClientResponseChannel>(executor, 1))
        {}

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

        void registerTools()
        {
            server.addTool(std::make_unique<SumTool>("sum", "Sum two integers", logger->createLogger("SumTool")));
            server.addTool(std::make_unique<RecordTool>("record", "Echo nested data", logger->createLogger("RecordTool")));
            server.addTool(std::make_unique<BrokenTool>(true, logger->createLogger("BadOutputTool")));
            server.addTool(std::make_unique<BrokenTool>(false, logger->createLogger("FailingTool")));
        }

        const std::shared_ptr<awl::ILogger> logger;
        asio::io_context ioContext;
        asio::any_io_executor executor;
        std::shared_ptr<InputChannel> input;
        Server server;
        Peer stdio;
        Peer http;
    };

    json::object makeRecord()
    {
        return json::object{{"items", json::array{
            json::object{{"label", "hello"}, {"count", 127}},
            json::object{{"label", "world"}, {"count", -128}}}},
            {"mode", "second"}, {"enabled", true}, {"factor", 0.5},
            {"serial", std::numeric_limits<std::uint64_t>::max()}};
    }

    std::string makeRequest(std::string method, json::object params)
    {
        return json::serialize(json::object{{"jsonrpc", "2.0"}, {"id", "same-id"},
            {"method", std::move(method)}, {"params", std::move(params)}});
    }

    asio::awaitable<json::object> asyncRequest(Peer& peer, const RouteId route_id,
        std::string method, json::object params)
    {
        co_await peer.requests->async_send(boost::system::error_code{},
            ClientMessage{route_id, makeRequest(std::move(method), std::move(params))}, asio::use_awaitable);
        OutgoingMessage reply = co_await peer.responses->async_receive(asio::use_awaitable);
        AWL_ASSERT(reply.kind == MessageKind::Response);
        AWL_ASSERT(reply.routeId == route_id);
        json::object response = json::parse(reply.body).as_object();
        AWL_ASSERT(response.at("id") == "same-id");
        AWL_ASSERT_FALSE(response.contains("error"));
        co_return response.at("result").as_object();
    }

    asio::awaitable<json::object> asyncCall(Peer& peer, const RouteId route_id,
        std::string name, json::object arguments)
    {
        co_return co_await asyncRequest(peer, route_id, "tools/call",
            json::object{{"name", std::move(name)}, {"arguments", std::move(arguments)}});
    }

    void checkResult(const json::object& result)
    {
        AWL_ASSERT(result.at("isError") == false);
        const json::array& content = result.at("content").as_array();
        AWL_ASSERT(content.size() == 1);
        AWL_ASSERT(content[0].as_object().at("type") == "text");
        const std::string text(content[0].as_object().at("text").as_string());
        AWL_ASSERT(json::parse(text) == result.at("structuredContent"));
    }

    asio::awaitable<void> asyncCheckPeer(Peer& peer, const RouteId route_id, const std::int32_t a)
    {
        const json::object list = co_await asyncRequest(peer, route_id, "tools/list", {});
        const json::array& tools = list.at("tools").as_array();
        AWL_ASSERT(tools.size() == 4);
        const json::object& sum = tools.back().as_object();
        AWL_ASSERT(sum.at("name") == "sum");
        AWL_ASSERT(sum.at("inputSchema") == makeJsonSchema<SumInput>());
        AWL_ASSERT(sum.at("outputSchema") == makeJsonSchema<SumOutput>());

        const json::object result = co_await asyncCall(peer, route_id, "sum", json::object{{"a", a}, {"b", 4.0}});
        checkResult(result);
        validateJson<SumOutput>(result.at("structuredContent"));
        const json::object& output = result.at("structuredContent").as_object();
        AWL_ASSERT(output.at("sum") == static_cast<std::int64_t>(a) + 4);
        AWL_ASSERT(output.at("note").is_null());

        const json::object annotated = co_await asyncCall(peer, route_id, "sum",
            json::object{{"a", 1}, {"b", 2}, {"note", "typed"}});
        checkResult(annotated);
        AWL_ASSERT(annotated.at("structuredContent").as_object().at("note") == "typed");

        for (const std::int32_t operand : {std::numeric_limits<std::int32_t>::lowest(), std::numeric_limits<std::int32_t>::max()})
        {
            const json::object boundary = co_await asyncCall(peer, route_id, "sum",
                json::object{{"a", operand}, {"b", operand}});
            checkResult(boundary);
            AWL_ASSERT(boundary.at("structuredContent").as_object().at("sum") == static_cast<std::int64_t>(operand) * 2);
        }

        json::object record = makeRecord();
        const json::object echo = co_await asyncCall(peer, route_id, "record", record);
        checkResult(echo);
        validateJson<Record>(echo.at("structuredContent"));
        const json::object& echoed = echo.at("structuredContent").as_object();
        AWL_ASSERT(echoed.at("items") == record.at("items"));
        AWL_ASSERT(echoed.at("note").is_null());
        AWL_ASSERT(echoed.at("mode") == "second");
        AWL_ASSERT(echoed.at("enabled") == true);
        AWL_ASSERT(echoed.at("factor").as_double() == 0.5);
        AWL_ASSERT(echoed.at("serial").as_uint64() == std::numeric_limits<std::uint64_t>::max());

        // Mathematical JSON integers include 4.0 and unsigned values above INT64_MAX.
        record["serial"] = std::ldexp(1.0, 63);
        record["note"] = nullptr;
        const json::object large = co_await asyncCall(peer, route_id, "record", std::move(record));
        checkResult(large);
        AWL_ASSERT(large.at("structuredContent").as_object().at("serial").as_uint64() == (std::uint64_t{1} << 63));
    }

    asio::awaitable<void> asyncAllTypes(Peer& peer, const RouteId route_id)
    {
        const json::object list = co_await asyncRequest(peer, route_id, "tools/list", {});
        AWL_ASSERT(list.at("tools").as_array().size() == 2);
        AllTypesData input;
        input.text = L"\u0416\U0001F642";
        input.price = decltype(input.price)("123.456");
        input.delay = std::chrono::milliseconds(-1500);
        input.timestamp = decltype(input.timestamp)(std::chrono::seconds(123));
        input.list = {1, 2};
        input.deque = {3, 4};
        input.set = {5, 6};
        input.multiset = {7, 7};
        input.unorderedSet = {8};
        input.unorderedMultiset = {9, 9};
        input.map = {{"first", 10}, {"second", std::nullopt}};
        input.multimap = {{"first", 11}};
        input.unorderedMap = {{"first", 12}};
        input.unorderedMultimap = {{"first", 13}};
        input.tuple = {14, input.text, true};
        input.json = json::object{{"opaque", 4.0}};
        input.object = {{"opaque", 5.0}};
        input.array = {6.0, nullptr, true};
        input.tree = {"root", {{"child", {}}}};
        input.number = 0.5L;
        const json::value original = awl::toJson(input);
        const json::object result = co_await asyncCall(peer, route_id, "all_types", original.as_object());
        checkResult(result);
        const auto& output = result.at("structuredContent");
        AWL_ASSERT(output == original);
        AWL_ASSERT(output.as_object().at("json").as_object().at("opaque").is_double());
        AWL_ASSERT(output.as_object().at("object").as_object().at("opaque").is_double());
        AWL_ASSERT(output.as_object().at("array").as_array()[0].is_double());
        validateJson<AllTypesData>(output);

        const std::vector<std::pair<std::string, json::value>> bad_fields{
            {"price", json::value("bad")}, {"delay", json::value("1s.0001")},
            {"timestamp", json::value(1001)}, {"tuple", json::array{1}},
            {"map", json::object{{"first", "bad"}}}, {"list", json::array{1, "bad"}},
            {"tree", json::object{{"name", "root"}, {"children", json::array{1}}}}};
        for (const auto& [name, bad_value] : bad_fields)
        {
            json::object arguments = original.as_object();
            arguments[name] = bad_value;
            const json::object error = co_await asyncCall(peer, route_id, "all_types", std::move(arguments));
            AWL_ASSERT(error.at("isError") == true);
            AWL_ASSERT_FALSE(error.contains("structuredContent"));
            const std::string message(error.at("content").as_array()[0].as_object().at("text").as_string());
            AWL_ASSERT(message.find(std::format("$.{}", name)) != std::string::npos);
        }

        const json::object wrapped = co_await asyncCall(peer, route_id, "wrapped",
            json::object{{"counter", std::ldexp(1.0, 63)}, {"link", 42.0}});
        checkResult(wrapped);
        const auto& fields = wrapped.at("structuredContent").as_object();
        AWL_ASSERT(fields.at("counter").as_uint64() == (std::uint64_t{1} << 63));
        AWL_ASSERT(fields.at("link") == 42);
        AWL_ASSERT(fields.at("note").is_null());
    }

    asio::awaitable<void> asyncAllTypesScenario(Fixture& fixture)
    {
        co_await (fixture.server.asyncRun() || (asyncAllTypes(fixture.stdio, 0) && asyncAllTypes(fixture.http, 21)));
    }

    asio::awaitable<void> asyncProgress(Peer& peer)
    {
        co_await peer.requests->async_send(boost::system::error_code{}, ClientMessage{17, makeRequest("tools/call",
            json::object{{"name", "sum"}, {"arguments", json::object{{"a", 2}, {"b", 3}}},
                {"_meta", json::object{{"progressToken", "typed-progress"}}}})}, asio::use_awaitable);
        const OutgoingMessage progress = co_await peer.responses->async_receive(asio::use_awaitable);
        AWL_ASSERT(progress.routeId == 17);
        AWL_ASSERT(progress.kind == MessageKind::Notification);
        AWL_ASSERT(json::parse(progress.body).as_object().at("params").as_object().at("progressToken") == "typed-progress");
        const OutgoingMessage reply = co_await peer.responses->async_receive(asio::use_awaitable);
        AWL_ASSERT(reply.kind == MessageKind::Response);
        AWL_ASSERT(reply.routeId == 17);
        AWL_ASSERT(json::parse(reply.body).as_object().at("result").as_object().at("structuredContent").as_object().at("sum") == 5);
    }

    asio::awaitable<void> asyncClients(Fixture& fixture)
    {
        co_await (asyncCheckPeer(fixture.stdio, 0, 10) && asyncCheckPeer(fixture.http, 0, 20));
        co_await asyncProgress(fixture.http);
    }

    asio::awaitable<void> asyncFullDuplex(Fixture& fixture)
    {
        co_await (fixture.server.asyncRun() || asyncClients(fixture));
    }

    asio::awaitable<void> asyncCheckInputError(Peer& peer, std::string name,
        json::object arguments, const std::string path)
    {
        const json::object result = co_await asyncCall(peer, 7, std::move(name), std::move(arguments));
        AWL_ASSERT(result.at("isError") == true);
        AWL_ASSERT_FALSE(result.contains("structuredContent"));
        const std::string text(result.at("content").as_array()[0].as_object().at("text").as_string());
        AWL_ASSERT(text.find(path) != std::string::npos);
    }

    asio::awaitable<void> asyncInputErrors(Peer& peer)
    {
        co_await asyncCheckInputError(peer, "sum", json::object{{"a", 1}}, "$.b");
        co_await asyncCheckInputError(peer, "sum", json::object{{"a", "42"}, {"b", 1}}, "$.a");
        co_await asyncCheckInputError(peer, "sum", json::object{{"a", 1.5}, {"b", 1}}, "$.a");
        co_await asyncCheckInputError(peer, "sum", json::object{{"a", nullptr}, {"b", 1}}, "$.a");
        co_await asyncCheckInputError(peer, "sum", json::object{{"a", std::int64_t{2147483648}}, {"b", 1}}, "$.a");
        co_await asyncCheckInputError(peer, "sum", json::object{{"a", 1}, {"b", 1}, {"extra", true}}, "$.extra");
        co_await asyncCheckInputError(peer, "sum", json::object{{"a", 1}, {"b", 1}, {"note", 5}}, "$.note");
        json::object record = makeRecord();
        record["items"].as_array()[1].as_object()["count"] = 128;
        co_await asyncCheckInputError(peer, "record", record, "$.items[1].count");
        record = makeRecord();
        record["mode"] = "unknown";
        co_await asyncCheckInputError(peer, "record", record, "$.mode");
        record = makeRecord();
        record["enabled"] = 1;
        co_await asyncCheckInputError(peer, "record", record, "$.enabled");
        record = makeRecord();
        record["serial"] = -1;
        co_await asyncCheckInputError(peer, "record", record, "$.serial");
        const json::object recovery = co_await asyncCall(peer, 7, "sum", json::object{{"a", 3}, {"b", 4}});
        checkResult(recovery);
        AWL_ASSERT(recovery.at("structuredContent").as_object().at("sum") == 7);
    }

    asio::awaitable<void> asyncInputErrorScenario(Fixture& fixture)
    {
        co_await (fixture.server.asyncRun() || asyncInputErrors(fixture.http));
    }

    asio::awaitable<void> asyncOutputErrors(Peer& peer)
    {
        for (const std::string name : {"bad_output", "fail"})
        {
            const json::object result = co_await asyncCall(peer, 7, name, {});
            AWL_ASSERT(result.at("isError") == true);
            AWL_ASSERT_FALSE(result.contains("structuredContent"));
            AWL_ASSERT(result.at("content").as_array()[0].as_object().at("text") == "Tool execution failed");
        }

        const json::object recovery = co_await asyncCall(peer, 7, "sum", json::object{{"a", 2}, {"b", 2}});
        checkResult(recovery);
    }

    asio::awaitable<void> asyncOutputErrorScenario(Fixture& fixture)
    {
        co_await (fixture.server.asyncRun() || asyncOutputErrors(fixture.http));
    }

    asio::awaitable<void> asyncWaitForEntry(Fixture& fixture, SignalChannel& entered)
    {
        co_await fixture.http.requests->async_send(boost::system::error_code{}, ClientMessage{7,
            makeRequest("tools/call", json::object{{"name", "block"}, {"arguments", json::object{}}})}, asio::use_awaitable);
        AWL_ASSERT(co_await entered.async_receive(asio::use_awaitable));
    }

    asio::awaitable<void> asyncCancel(Fixture& fixture, SignalChannel& entered, bool& finished)
    {
        co_await (fixture.server.asyncRun() || asyncWaitForEntry(fixture, entered));
        AWL_ASSERT(finished);
        AWL_ASSERT_FALSE(fixture.http.responses->try_receive(
            [](const boost::system::error_code, OutgoingMessage) {}));
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

    template <class T>
    void assertInvalid(const json::value& value)
    {
        awl::testing::Assert::throws<awl::JsonException>([&] { validateJson<T>(value); });
    }
}

AWL_TEST(TypedToolSchema)
{
    const SumTool tool("sum", "Sum two integers", context.logger->createLogger("SumTool"));
    const ToolDefinition& definition = tool.definition();
    AWL_ASSERT(definition.name == "sum");
    AWL_ASSERT(definition.description == "Sum two integers");
    const json::object& input = definition.inputSchema;
    AWL_ASSERT(input.at("type") == "object");
    AWL_ASSERT(input.at("$schema") == "https://json-schema.org/draft/2020-12/schema");
    AWL_ASSERT(input.at("additionalProperties") == false);
    AWL_ASSERT(input.at("required") == json::array({"a", "b"}));
    const json::object& fields = input.at("properties").as_object();
    AWL_ASSERT(fields.at("a").as_object().at("minimum") == std::numeric_limits<std::int32_t>::lowest());
    AWL_ASSERT(fields.at("a").as_object().at("maximum") == std::numeric_limits<std::int32_t>::max());
    const json::array& optional = fields.at("note").as_object().at("anyOf").as_array();
    AWL_ASSERT(optional[0].as_object().at("type") == "string");
    AWL_ASSERT(optional[1].as_object().at("type") == "null");
    AWL_ASSERT(definition.outputSchema->at("required") == json::array({"sum"}));

    const json::object record = makeJsonSchema<Record>();
    const json::object& properties = record.at("properties").as_object();
    AWL_ASSERT(properties.at("mode").as_object().at("enum") == json::array({"first", "second"}));
    AWL_ASSERT(properties.at("enabled").as_object().at("type") == "boolean");
    AWL_ASSERT(properties.at("factor").as_object().at("type") == "number");
    AWL_ASSERT(properties.at("serial").as_object().at("minimum") == 0);
    AWL_ASSERT(properties.at("serial").as_object().at("maximum").as_uint64() == std::numeric_limits<std::uint64_t>::max());
    const json::object& items = properties.at("items").as_object();
    AWL_ASSERT(items.at("type") == "array");
    AWL_ASSERT(items.at("items").as_object().at("required") == json::array({"label", "count"}));
    AWL_ASSERT(items.at("items").as_object().at("additionalProperties") == false);
    const json::object empty = makeJsonSchema<Empty>();
    AWL_ASSERT(empty.at("properties").as_object().empty());
    AWL_ASSERT(empty.at("required").as_array().empty());
}

AWL_TEST(TypedToolValidation)
{
    AWL_UNUSED_CONTEXT;
    validateJson<std::int8_t>(-128);
    validateJson<std::int8_t>(127.0);
    validateJson<std::uint8_t>(255);
    validateJson<std::int64_t>(std::numeric_limits<std::int64_t>::lowest());
    validateJson<std::uint64_t>(std::numeric_limits<std::uint64_t>::max());
    validateJson<std::uint64_t>(std::ldexp(1.0, 63));
    validateJson<Record>(makeRecord());
    assertInvalid<std::int8_t>(128);
    assertInvalid<std::int8_t>(-129);
    assertInvalid<std::uint8_t>(-1);
    assertInvalid<std::uint8_t>(256);
    assertInvalid<std::int64_t>(std::ldexp(1.0, 63));
    assertInvalid<std::uint64_t>(std::ldexp(1.0, 64));
    assertInvalid<int>(1.5);
    assertInvalid<int>(true);
    assertInvalid<int>("42");
    assertInvalid<double>("42.5");
    assertInvalid<float>(std::numeric_limits<double>::max());
    assertInvalid<double>(std::numeric_limits<double>::infinity());
    assertInvalid<double>(std::numeric_limits<double>::quiet_NaN());
    assertInvalid<std::vector<int>>(json::array{1, "2"});
    assertInvalid<Record>(nullptr);
}

AWL_TEST(TypedToolFullDuplex)
{
    Fixture fixture(context.logger);
    fixture.registerTools();
    runScenario(fixture, asyncFullDuplex(fixture));
}

AWL_TEST(TypedToolInputErrors)
{
    Fixture fixture(context.logger);
    fixture.registerTools();
    runScenario(fixture, asyncInputErrorScenario(fixture));
}

AWL_TEST(TypedToolOutputErrors)
{
    Fixture fixture(context.logger);
    fixture.registerTools();
    runScenario(fixture, asyncOutputErrorScenario(fixture));
}

AWL_TEST(TypedToolCancel)
{
    Fixture fixture(context.logger);
    SignalChannel entered(fixture.executor, 0);
    bool finished = false;
    fixture.server.addTool(std::make_unique<BlockingTypedTool>(entered, finished,
        fixture.logger->createLogger("BlockingTool")));
    runScenario(fixture, asyncCancel(fixture, entered, finished));
}

AWL_TEST(TypedToolAllTypes)
{
    Fixture fixture(context.logger);
    fixture.server.addTool(std::make_unique<EchoTypedTool<AllTypesData>>(
        "all_types", "Echo all AWL JSON representations", context.logger->createLogger("AllTypesTool")));
    fixture.server.addTool(std::make_unique<EchoTypedTool<WrappedData>>(
        "wrapped", "Echo wrapped values", context.logger->createLogger("WrappedTool")));
    runScenario(fixture, asyncAllTypesScenario(fixture));
}
