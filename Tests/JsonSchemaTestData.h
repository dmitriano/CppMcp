#pragma once

#include "Awl/Reflection.h"
#include "Awl/Decimal.h"

#include <boost/json.hpp>
#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mcp::testing
{
    struct SchemaNode
    {
        std::string name;
        std::vector<SchemaNode> children;

        AWL_REFLECT(name, children)
    };

    struct AllTypesData
    {
        std::wstring text;
        awl::decimal<std::uint64_t, 4> price;
        std::chrono::milliseconds delay{};
        std::chrono::time_point<std::chrono::system_clock, std::chrono::seconds> timestamp;
        std::list<int> list;
        std::deque<int> deque;
        std::set<int> set;
        std::multiset<int> multiset;
        std::unordered_set<int> unorderedSet;
        std::unordered_multiset<int> unorderedMultiset;
        std::map<std::string, std::optional<int>> map;
        std::multimap<std::string, int> multimap;
        std::unordered_map<std::string, int> unorderedMap;
        std::unordered_multimap<std::string, int> unorderedMultimap;
        std::tuple<int, std::wstring, std::optional<bool>> tuple;
        std::tuple<> emptyTuple;
        boost::json::value json;
        boost::json::object object;
        boost::json::array array;
        SchemaNode tree;
        long double number{};

        AWL_REFLECT(text, price, delay, timestamp, list, deque, set, multiset,
            unorderedSet, unorderedMultiset, map, multimap, unorderedMap,
            unorderedMultimap, tuple, emptyTuple, json, object, array, tree, number)
    };

    // Reference wrappers must remain bound to storage owned by the moved DTO.
    // std::atomic requires an explicit move constructor in value-based tools.
    struct WrappedData
    {
        int storage{};
        std::optional<std::string> noteStorage;
        std::atomic<std::uint64_t> counter{};
        std::reference_wrapper<int> link{storage};
        std::reference_wrapper<std::optional<std::string>> note{noteStorage};

        WrappedData() = default;

        WrappedData(WrappedData&& other) noexcept : storage(other.storage),
            noteStorage(std::move(other.noteStorage)), counter(other.counter.load()),
            link(storage), note(noteStorage)
        {}

        AWL_REFLECT(counter, link, note)
    };
}
