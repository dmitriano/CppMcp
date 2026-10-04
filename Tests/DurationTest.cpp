#include "BoostExtras/Json/JsonUtil.h"

#include "Awl/Time.h"
#include "Awl/Testing/UnitTest.h"

#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using namespace std::chrono;

    template <class Exception = awl::GeneralException, class Func>
    void assertDurationException(Func&& function)
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
        catch (const Exception&)
        {
            thrown = true;
        }

        AWL_ASSERT(thrown);
    }

    struct DurationCase
    {
        nanoseconds value;
        const char* text;
    };

    const std::vector<DurationCase> duration_cases{
        {nanoseconds::zero(), "0"},
        {nanoseconds(1), "0.000000001"},
        {milliseconds(1500), "1s.5"},
        {minutes(1) + milliseconds(1), "1m.0s.001"},
        {days(2) + hours(3) + minutes(4) + seconds(5) + milliseconds(6), "2d.3h.4m.5s.006"},
        {-milliseconds(1500), "-1s.5"},
        {-milliseconds(1), "-0.001"},
        {nanoseconds::min(), "-106751d.23h.47m.16s.854775808"},
        {nanoseconds::max(), "106751d.23h.47m.16s.854775807"}};

    struct TimerData
    {
        milliseconds retryDelay;
        std::optional<nanoseconds> elapsed;
        std::vector<seconds> offsets;

        AWL_REFLECT(retryDelay, elapsed, offsets)
    };
}

AWL_TEST(DurationFormatting)
{
    AWL_UNUSED_CONTEXT;
    for (const auto& [value, text] : duration_cases)
    {
        AWL_ASSERT(awl::duration_to_string<char>(value) == text);
        std::wostringstream wide;
        awl::format_duration(wide, value);
        const std::string narrow(text);
        AWL_ASSERT(wide.str() == std::wstring(narrow.begin(), narrow.end()));
    }

    AWL_ASSERT(awl::duration_to_string<char>(duration<double>(1.25)) == "1s.25");
    AWL_ASSERT(awl::duration_to_string<char>(hours(25)) == "1d.1h");
}

AWL_TEST(DurationParsing)
{
    AWL_UNUSED_CONTEXT;
    for (const auto& [value, text] : duration_cases)
    {
        AWL_ASSERT(awl::parse_duration(text) == value);
        const std::string narrow(text);
        AWL_ASSERT(awl::parse_duration(std::wstring(narrow.begin(), narrow.end())) == value);
    }

    AWL_ASSERT(awl::parse_duration<milliseconds>("1s.500000000000") == milliseconds(1500));
    AWL_ASSERT(awl::parse_duration<seconds>("120s") == seconds(120));
    AWL_ASSERT(awl::parse_duration<duration<double>>("1s.25") == duration<double>(1.25));
    AWL_ASSERT(awl::parse_duration("-0") == nanoseconds::zero());
}

AWL_TEST(DurationErrors)
{
    AWL_UNUSED_CONTEXT;
    for (const char* text : {"", "-", "--1s", "+1s", "1", "1s.", "1s..5", ".5", "1s.5.1",
        "1s.1m", "1m.1m", "1d.5", "1s.-1", " 1s", "1s ", "0.0000000001", "1s.123456789x",
        "999999999999999999999d", "106751d.23h.47m.16s.854775808", "-106751d.23h.47m.16s.854775809"})
    {
        assertDurationException([&] { awl::parse_duration(text); });
    }

    assertDurationException([] { awl::parse_duration<seconds>("1s.5"); });
    assertDurationException([] { awl::parse_duration<duration<std::int8_t, std::milli>>("0.128"); });
    assertDurationException([] { awl::parse_duration<duration<std::uint64_t, std::nano>>("-1s"); });
    assertDurationException([] { awl::parse_duration(L"1s.\u0661"); });
    assertDurationException([] { awl::duration_to_string<char>(seconds::max()); });
    assertDurationException([] { awl::duration_to_string<char>(duration<int, std::pico>(1)); });
    assertDurationException([] { awl::duration_to_string<char>(duration<double>(std::numeric_limits<double>::infinity())); });
    assertDurationException([] { awl::duration_to_string<char>(duration<double>(std::numeric_limits<double>::quiet_NaN())); });
}

AWL_TEST(JsonDuration)
{
    AWL_UNUSED_CONTEXT;
    for (const auto& [value, text] : duration_cases)
    {
        const boost::json::value json = awl::toJson(value);
        AWL_ASSERT(json.is_string());
        AWL_ASSERT(json.as_string() == text);
        nanoseconds restored{};
        awl::fromJson(boost::json::parse(boost::json::serialize(json)), restored);
        AWL_ASSERT(restored == value);
    }

    milliseconds value(7);
    for (const boost::json::value json : {boost::json::value("1s."), boost::json::value("0.000000001"),
        boost::json::value(1500), boost::json::value(true), boost::json::value(nullptr)})
    {
        assertDurationException<awl::JsonException>([&] { awl::fromJson(json, value); });
        AWL_ASSERT(value == milliseconds(7));
    }

    assertDurationException<awl::JsonException>([] { awl::toJson(seconds::max()); });
    const TimerData data{milliseconds(1500), nanoseconds(1), {seconds(0), seconds(30)}};
    const boost::json::value json = awl::toJson(data);
    AWL_ASSERT(json.as_object().at("retryDelay") == "1s.5");
    AWL_ASSERT(json.as_object().at("elapsed") == "0.000000001");
    AWL_ASSERT(json.as_object().at("offsets") == boost::json::array({"0", "30s"}));
    TimerData restored{};
    awl::fromJson(json, restored);
    AWL_ASSERT(restored.as_tuple() == data.as_tuple());
}
