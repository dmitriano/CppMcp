#include "Common/JsonSchema.h"
#include "Tests/JsonSchemaTestData.h"

#include "BoostExtras/Json/JsonUtil.h"
#include "Awl/Testing/UnitTest.h"

#include <limits>

namespace
{
    struct CustomNumber { int value{}; };
    struct OpaqueNumber { int value{}; };
    struct NonDefaultNumber
    {
        explicit NonDefaultNumber(const int initial) : value(initial) {}

        int value;
    };

    template <class T>
    struct NumberSerializer
    {
        void fromJson(const boost::json::value& json, T& output)
        {
            if (!json.is_int64() || json.as_int64() < 0 || json.as_int64() > 100)
            {
                throw awl::JsonException("Expected an integer between 0 and 100.");
            }

            output.value = static_cast<int>(json.as_int64());
        }

        void toJson(const T& input, boost::json::value& json)
        {
            json = input.value;
        }
    };

    struct CustomData
    {
        CustomNumber custom;
        OpaqueNumber opaque;

        AWL_REFLECT(custom, opaque)
    };
}

namespace awl
{
    template <>
    class JsonSerializer<CustomNumber> : public NumberSerializer<CustomNumber> {};

    template <>
    class JsonSerializer<OpaqueNumber> : public NumberSerializer<OpaqueNumber> {};

    template <>
    class JsonSerializer<NonDefaultNumber> : public NumberSerializer<NonDefaultNumber> {};
}

namespace mcp
{
    template <>
    struct JsonSchemaTraits<CustomNumber>
    {
        static boost::json::object schema()
        {
            return {{"type", "integer"}, {"minimum", 0}, {"maximum", 100}};
        }
    };

    template <>
    struct JsonSchemaTraits<NonDefaultNumber> : JsonSchemaTraits<CustomNumber>
    {
        static void validate(const boost::json::value& value)
        {
            NonDefaultNumber temporary(0);
            awl::fromJson(value, temporary);
        }
    };
}

namespace
{
    namespace json = boost::json;
    using namespace mcp;
    using namespace mcp::testing;
    using namespace std::chrono;

    template <class Func>
    void assertJsonError(Func&& function, const std::string_view path = {})
    {
        bool caught = false;
        try
        {
            function();
        }
        catch (const awl::JsonException& error)
        {
            caught = true;
            AWL_ASSERT(std::string_view(error.what()).find(path) != std::string_view::npos);
        }

        AWL_ASSERT(caught);
    }

    template <class T>
    void assertInvalid(const json::value& value, const std::string_view path = "$")
    {
        assertJsonError([&] { validateJson<T>(value); }, path);
    }

    template <class T>
    void roundTrip(const T& original)
    {
        const json::value wire = awl::toJson(original);
        validateJson<T>(wire);
        AWL_ASSERT(!makeJsonSchema<T>().empty());
        T restored{};
        awl::fromJson(wire, restored);
        AWL_ASSERT(awl::toJson(restored) == wire);
    }

    template <class... Types>
    void arithmeticCases()
    {
        (roundTrip(Types{1}), ...);
        (roundTrip(std::numeric_limits<Types>::max()), ...);
        (roundTrip(std::numeric_limits<Types>::lowest()), ...);
    }
}

AWL_TEST(JsonSchemaAllTypes)
{
    AWL_UNUSED_CONTEXT;
    arithmeticCases<bool, char, signed char, unsigned char, wchar_t, char8_t, char16_t, char32_t,
        short, unsigned short, int, unsigned int, long, unsigned long, long long, unsigned long long,
        float, double>();
    roundTrip(1.0L);
    roundTrip(static_cast<long double>(std::numeric_limits<double>::max()));
    roundTrip(std::list<int>{1, 2});
    roundTrip(std::deque<int>{1, 2});
    roundTrip(std::vector<bool>{true, false});
    roundTrip(std::set<int>{1, 2});
    roundTrip(std::multiset<int>{1, 1});
    roundTrip(std::unordered_set<int>{1});
    roundTrip(std::unordered_multiset<int>{1, 1});
    roundTrip(std::map<std::string, std::optional<int>>{{"first", 1}, {"second", std::nullopt}});
    roundTrip(std::multimap<std::string, int>{{"first", 1}, {"second", 2}});
    roundTrip(std::unordered_map<std::string, int>{{"first", 1}});
    roundTrip(std::unordered_multimap<std::string, int>{{"first", 1}});
    roundTrip(std::tuple<int, std::wstring, std::optional<bool>>{2, L"\u0416\U0001F642", true});
    roundTrip(std::tuple<>{});
    roundTrip(awl::decimal<std::uint64_t, 4>("123.456"));
    roundTrip(milliseconds(-1500));
    roundTrip(duration<double>(1.25));
    roundTrip(time_point<system_clock, seconds>(seconds(123)));
    roundTrip(time_point<steady_clock, milliseconds>(milliseconds(-123)));
    roundTrip(time_point<system_clock, milliseconds>(milliseconds::max()));
    roundTrip(time_point<system_clock, milliseconds>(milliseconds::min()));
    roundTrip(time_point<system_clock, milliseconds>(milliseconds((std::int64_t{1} << 53) + 1)));
    roundTrip(json::value(json::object{{"free", json::array{nullptr, 1.0, true}}}));
    roundTrip(json::object{{"free", 2.0}});
    roundTrip(json::array{3.0});
    roundTrip(AllTypesData{});
}

AWL_TEST(JsonSchemaShapes)
{
    AWL_UNUSED_CONTEXT;
    const json::object schema = makeJsonSchema<AllTypesData>();
    const json::object& fields = schema.at("properties").as_object();
    AWL_ASSERT(fields.at("text").as_object().at("type") == "string");
    AWL_ASSERT(fields.at("price").as_object().at("type") == "string");
    AWL_ASSERT(fields.at("delay").as_object().at("type") == "string");
    AWL_ASSERT(fields.at("timestamp").as_object().at("type") == "integer");
    AWL_ASSERT(fields.at("list").as_object().at("type") == "array");
    AWL_ASSERT(fields.at("map").as_object().at("type") == "object");
    AWL_ASSERT(fields.at("map").as_object().at("additionalProperties").as_object().contains("anyOf"));
    const json::object& tuple = fields.at("tuple").as_object();
    AWL_ASSERT(tuple.at("prefixItems").as_array().size() == 3);
    AWL_ASSERT(tuple.at("items") == false);
    AWL_ASSERT(tuple.at("minItems") == 3 && tuple.at("maxItems") == 3);
    AWL_ASSERT_FALSE(fields.at("emptyTuple").as_object().contains("prefixItems"));
    AWL_ASSERT(fields.at("json").as_object().empty());
    AWL_ASSERT(fields.at("object").as_object().at("type") == "object");
    AWL_ASSERT(fields.at("array").as_object().at("type") == "array");
    AWL_ASSERT(schema.contains("$defs"));
    const auto& definitions = schema.at("$defs").as_object();
    AWL_ASSERT(definitions.size() == 1);
    const json::object& node = definitions.begin()->value().as_object();
    const json::object& children = node.at("properties").as_object().at("children").as_object();
    AWL_ASSERT(children.at("items").as_object().at("$ref") == "#/$defs/type0");
    roundTrip(SchemaNode{"root", {{"child", {{"leaf", {}}}}}});
    const auto array_root = makeJsonSchema<std::vector<SchemaNode>>();
    AWL_ASSERT(array_root.contains("$defs"));
    validateJson<std::vector<SchemaNode>>(json::array{awl::toJson(SchemaNode{"root", {}})});
    assertInvalid<SchemaNode>(json::object{{"name", "root"}, {"children", json::array{
        json::object{{"name", 5}, {"children", json::array{}}}}}}, "$.children[0].name");
    assertInvalid<SchemaNode>(json::object{{"name", "root"}, {"children", json::array{}}, {"extra", 1}}, "$.extra");
}

AWL_TEST(JsonSchemaValidation)
{
    AWL_UNUSED_CONTEXT;
    assertInvalid<std::list<int>>(json::array{1, "bad"}, "$[1]");
    assertInvalid<std::map<std::string, int>>(json::object{{"bad", "text"}}, "$.bad");
    assertInvalid<std::tuple<int, bool>>(json::array{1});
    assertInvalid<std::tuple<int, bool>>(json::array{1, true, 3});
    assertInvalid<std::tuple<int, bool>>(json::array{1, "bad"}, "$[1]");
    assertInvalid<std::tuple<>>(json::array{1});
    assertInvalid<awl::decimal<std::uint64_t, 4>>("not-a-decimal");
    assertInvalid<awl::decimal<std::uint64_t, 4>>(12.5);
    assertInvalid<milliseconds>("1s.0001");
    assertInvalid<milliseconds>("bad");
    assertInvalid<milliseconds>(1500);
    assertInvalid<time_point<system_clock, seconds>>(1001);
    assertInvalid<time_point<system_clock, nanoseconds>>(std::numeric_limits<std::int64_t>::max());
    assertInvalid<time_point<system_clock, milliseconds>>(std::ldexp(1.0, 63));
    assertInvalid<time_point<system_clock, milliseconds>>("123junk");
    assertInvalid<json::object>(json::array{});
    assertInvalid<json::array>(json::object{});
    assertInvalid<json::value>(json::object{{"bad", std::numeric_limits<double>::infinity()}}, "$.bad");
    assertInvalid<json::value>(json::array{std::numeric_limits<double>::quiet_NaN()}, "$[0]");
    assertInvalid<json::value>(json::object{{"bad", std::string("\xC0\xAF")}}, "$.bad");
    assertJsonError([] { awl::toJson(std::multimap<std::string, int>{{"same", 1}, {"same", 2}}); });
    assertJsonError([]
    {
        time_point<system_clock, seconds> output;
        awl::fromJson(json::value("123junk"), output);
    });
    assertJsonError([] { awl::toJson(time_point<system_clock, seconds>(seconds::max())); });
}

AWL_TEST(JsonSchemaWrappers)
{
    AWL_UNUSED_CONTEXT;
    WrappedData output;
    json::value wire = json::object{{"counter", std::ldexp(1.0, 63)}, {"link", 42.0}};
    validateJson<WrappedData>(wire);
    detail::normalizeJsonIntegers<WrappedData>(wire);
    awl::fromJson(wire, output);
    AWL_ASSERT(output.counter.load() == (std::uint64_t{1} << 63));
    AWL_ASSERT(output.storage == 42);
    AWL_ASSERT_FALSE(output.noteStorage.has_value());
    const json::object schema = makeJsonSchema<WrappedData>();
    AWL_ASSERT(schema.at("required") == json::array({"counter", "link"}));
    AWL_ASSERT(schema.at("properties").as_object().at("note").as_object().contains("anyOf"));
    const WrappedData moved(std::move(output));
    AWL_ASSERT(moved.link.get() == 42);
    validateJson<WrappedData>(awl::toJson(moved));
    assertInvalid<std::atomic<int>>("bad");
    assertInvalid<std::reference_wrapper<int>>(1.5);
    roundTrip(std::optional<std::tuple<int>>{std::tuple<int>{1}});
}

AWL_TEST(JsonSchemaUnicode)
{
    AWL_UNUSED_CONTEXT;
    const std::wstring original(L"\u0416\U0001F642\0end", 7);
    const json::value wire = awl::toJson(original);
    std::wstring restored;
    awl::fromJson(wire, restored);
    AWL_ASSERT(restored == original);
    AWL_ASSERT(wire.as_string().starts_with("\xD0\x96\xF0\x9F\x99\x82"));
    validateJson<std::wstring>(wire);
    assertJsonError([]
    {
        std::wstring output;
        awl::fromJson(json::value(std::string("\xED\xA0\x80")), output);
    });
    assertJsonError([] { awl::toJson(std::wstring(1, static_cast<wchar_t>(0xD800))); });
    assertInvalid<std::wstring>(json::value(std::string("\xF4\x90\x80\x80")));
}

AWL_TEST(JsonSchemaCustomSerializer)
{
    AWL_UNUSED_CONTEXT;
    const auto schema = makeJsonSchema<CustomData>();
    const auto& fields = schema.at("properties").as_object();
    AWL_ASSERT(fields.at("custom").as_object().at("type") == "integer");
    AWL_ASSERT(fields.at("custom").as_object().at("maximum") == 100);
    AWL_ASSERT(fields.at("opaque").as_object().empty());
    roundTrip(CustomData{{42}, {73}});
    assertInvalid<CustomData>(json::object{{"custom", -1}, {"opaque", 2}}, "$.custom");
    assertInvalid<CustomData>(json::object{{"custom", 1}, {"opaque", "bad"}}, "$.opaque");
    AWL_ASSERT(makeJsonSchema<NonDefaultNumber>().at("type") == "integer");
    validateJson<NonDefaultNumber>(25);
    assertInvalid<NonDefaultNumber>(101);
}
