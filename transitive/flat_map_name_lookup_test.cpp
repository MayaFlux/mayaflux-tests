#include "gtest/gtest.h"

#include "MayaFlux/Transitive/Memory/FlatMap.hpp"

namespace MayaFlux::Test {
namespace {

    using Memory::FlatMap;
    using NameMap = FlatMap<std::string, int, std::less<>>;

    template <typename Map, typename Key>
    concept FindableBy = requires(const Map& map, const Key& key) { map.find(key); };

    template <typename Map, typename Key>
    concept ErasableBy = requires(Map& map, const Key& key) { map.erase(key); };

    template <typename Map>
    concept PrefixMergeable = requires(Map& map, const Map& other, std::string& error) {
        map.merge_prefixed(other, std::string_view {}, error);
    };

    static_assert(FindableBy<NameMap, std::string_view>);
    static_assert(ErasableBy<NameMap, std::string_view>);
    static_assert(PrefixMergeable<NameMap>);
    static_assert(!FindableBy<FlatMap<std::string, int>, std::string_view>);
    static_assert(!PrefixMergeable<FlatMap<int, int>>);

    NameMap map_of(std::initializer_list<std::pair<std::string, int>> entries)
    {
        NameMap map;
        for (const auto& [name, value] : entries)
            map.insert_or_assign(name, value);
        return map;
    }

    TEST(FlatMapNameLookupTest, AStringViewOrLiteralFindsAStringKey)
    {
        const NameMap map = map_of({ { "dial", 7 }, { "pad", 9 } });

        const auto by_view = map.find(std::string_view("dial"));
        ASSERT_NE(by_view, map.end());
        EXPECT_EQ(by_view->second, 7);

        const auto by_literal = map.find("pad");
        ASSERT_NE(by_literal, map.end());
        EXPECT_EQ(by_literal->second, 9);

        EXPECT_EQ(map.find(std::string_view("absent")), map.end());
        EXPECT_EQ(map.find(std::string("dial"))->second, 7);
    }

    TEST(FlatMapNameLookupTest, AStringViewErasesAStringKey)
    {
        NameMap map = map_of({ { "a", 1 }, { "b", 2 } });
        EXPECT_TRUE(map.erase(std::string_view("a")));
        EXPECT_FALSE(map.erase(std::string_view("a")));
        EXPECT_EQ(map.size(), 1U);
        EXPECT_NE(map.find("b"), map.end());
    }

    TEST(FlatMapNameLookupTest, IterationIsInKeyOrderSoNamesComeOutSorted)
    {
        const NameMap map = map_of({ { "b", 2 }, { "c", 3 }, { "a", 1 } });
        std::vector<std::string> names;
        for (const auto& entry : map)
            names.push_back(entry.first);
        EXPECT_EQ(names, (std::vector<std::string> { "a", "b", "c" }));
    }

    TEST(FlatMapNameLookupTest, MergePrefixedAddsEntriesUnderAPrefix)
    {
        NameMap shared = map_of({ { "base", 0 } });
        const NameMap mine = map_of({ { "dial", 1 }, { "pad", 2 } });

        std::string error;
        ASSERT_TRUE(shared.merge_prefixed(mine, "studio.", error)) << error;
        EXPECT_EQ(shared.size(), 3U);
        EXPECT_EQ(shared.find("studio.pad")->second, 2);
        EXPECT_NE(shared.find("base"), shared.end());
    }

    TEST(FlatMapNameLookupTest, ACollisionFailsAndLeavesTheMapUnchanged)
    {
        NameMap shared = map_of({ { "dial", 9 } });
        const NameMap mine = map_of({ { "aaa", 1 }, { "dial", 2 } });

        std::string error;
        EXPECT_FALSE(shared.merge_prefixed(mine, "", error));
        EXPECT_NE(error.find("dial"), std::string::npos);
        EXPECT_EQ(shared.size(), 1U);
        EXPECT_EQ(shared.find("dial")->second, 9);
        EXPECT_EQ(shared.find("aaa"), shared.end());
    }

    TEST(FlatMapNameLookupTest, ReplaceOverwritesCollidingEntries)
    {
        NameMap shared = map_of({ { "dial", 9 } });
        const NameMap mine = map_of({ { "dial", 2 }, { "pad", 3 } });

        std::string error;
        ASSERT_TRUE(shared.merge_prefixed(mine, "", error, true)) << error;
        EXPECT_EQ(shared.find("dial")->second, 2);
        EXPECT_EQ(shared.find("pad")->second, 3);
    }

    TEST(FlatMapNameLookupTest, ACopyIsIndependentAndHoldsCallables)
    {
        FlatMap<std::string, std::function<int(int)>, std::less<>> registry;
        registry.insert_or_assign("double", [](int v) { return v * 2; });

        auto copy = registry;
        copy.insert_or_assign("triple", [](int v) { return v * 3; });

        EXPECT_EQ(registry.find("double")->second(4), 8);
        EXPECT_EQ(registry.find("triple"), registry.end());
        EXPECT_EQ(copy.find("triple")->second(4), 12);
    }

    TEST(FlatMapNameLookupTest, TheDefaultComparatorKeepsItsExactKeyBehavior)
    {
        FlatMap<int, int> map;
        map.insert_or_assign(3, 30);
        map.insert_or_assign(1, 10);
        EXPECT_EQ(map.find(1)->second, 10);
        EXPECT_TRUE(map.erase(3));
        EXPECT_EQ(map.size(), 1U);
    }

}
}
