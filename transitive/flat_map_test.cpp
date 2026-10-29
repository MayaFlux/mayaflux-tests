#include "gtest/gtest.h"

#include "MayaFlux/Transitive/Memory/FlatMap.hpp"
#include "MayaFlux/Transitive/Memory/SnapshotCell.hpp"

namespace MayaFlux::Test {
namespace {

    using IntMap = Memory::FlatMap<int, int>;

    TEST(FlatMapTest, StartsEmpty)
    {
        const IntMap map;

        EXPECT_TRUE(map.empty());
        EXPECT_EQ(map.size(), 0U);
        EXPECT_EQ(map.find(1), map.end());
    }

    TEST(FlatMapTest, InsertKeepsKeysSortedWhateverTheInsertionOrder)
    {
        IntMap map;

        for (const int key : { 5, 1, 9, 3, 7 }) {
            EXPECT_TRUE(map.insert_or_assign(key, key * 10));
        }

        std::vector<int> keys;
        for (const auto& [key, value] : map) {
            keys.push_back(key);
            EXPECT_EQ(value, key * 10);
        }
        EXPECT_EQ(keys, (std::vector<int> { 1, 3, 5, 7, 9 }));
    }

    TEST(FlatMapTest, InsertOrAssignOverwritesAnExistingKeyWithoutGrowing)
    {
        IntMap map;
        map.insert_or_assign(2, 20);

        EXPECT_FALSE(map.insert_or_assign(2, 21));

        EXPECT_EQ(map.size(), 1U);
        EXPECT_EQ(map.find(2)->second, 21);
    }

    TEST(FlatMapTest, FindReturnsEndForAbsentKeysBelowBetweenAndAboveTheRange)
    {
        IntMap map;
        map.insert_or_assign(10, 1);
        map.insert_or_assign(20, 2);

        EXPECT_EQ(map.find(5), map.end());
        EXPECT_EQ(map.find(15), map.end());
        EXPECT_EQ(map.find(25), map.end());
        EXPECT_NE(map.find(10), map.end());
        EXPECT_NE(map.find(20), map.end());
    }

    TEST(FlatMapTest, EraseRemovesOnlyThatKeyAndReportsWhetherItExisted)
    {
        IntMap map;
        map.insert_or_assign(1, 10);
        map.insert_or_assign(2, 20);
        map.insert_or_assign(3, 30);

        EXPECT_TRUE(map.erase(2));
        EXPECT_FALSE(map.erase(2));
        EXPECT_FALSE(map.erase(99));

        EXPECT_EQ(map.size(), 2U);
        EXPECT_EQ(map.find(2), map.end());
        EXPECT_EQ(map.find(1)->second, 10);
        EXPECT_EQ(map.find(3)->second, 30);
    }

    TEST(FlatMapTest, ClearEmptiesTheMap)
    {
        IntMap map;
        map.insert_or_assign(1, 1);

        map.clear();

        EXPECT_TRUE(map.empty());
    }

    TEST(FlatMapTest, CopiesAreIndependent)
    {
        IntMap original;
        original.insert_or_assign(1, 10);

        IntMap copy = original;
        copy.insert_or_assign(2, 20);
        copy.erase(1);

        EXPECT_EQ(original.size(), 1U);
        EXPECT_NE(original.find(1), original.end());
        EXPECT_EQ(copy.size(), 1U);
        EXPECT_EQ(copy.find(1), copy.end());
    }

    TEST(FlatMapTest, PointerKeysOrderAndLookUp)
    {
        std::array<int, 4> storage {};
        Memory::FlatMap<const int*, int> map;

        for (size_t i = storage.size(); i-- > 0;) {
            map.insert_or_assign(&storage[i], static_cast<int>(i));
        }

        for (size_t i = 0; i < storage.size(); ++i) {
            ASSERT_NE(map.find(&storage[i]), map.end());
            EXPECT_EQ(map.find(&storage[i])->second, static_cast<int>(i));
        }
    }

    TEST(FlatMapTest, WorksAsTheValueOfASnapshotCellUnderConcurrentWriters)
    {
        constexpr size_t writers = 4;
        constexpr int per_writer = 250;
        Memory::SnapshotCell<IntMap> cell;

        std::vector<std::thread> threads;
        threads.reserve(writers);
        for (size_t writer = 0; writer < writers; ++writer) {
            threads.emplace_back([&cell, writer] {
                for (int i = 0; i < per_writer; ++i) {
                    const int key = static_cast<int>(writer) * per_writer + i;
                    cell.update([key](IntMap& map) { map.insert_or_assign(key, key + 1); });
                }
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }

        const auto guard = cell.read();
        ASSERT_TRUE(guard);
        EXPECT_EQ(guard->size(), writers * per_writer);
        for (const auto& [key, value] : *guard) {
            EXPECT_EQ(value, key + 1);
        }
    }

} // namespace
} // namespace MayaFlux::Test
