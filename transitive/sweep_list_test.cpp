#include "gtest/gtest.h"

#include "MayaFlux/Transitive/Memory/SweepList.hpp"

namespace MayaFlux::Test {
namespace {

    TEST(SweepListTryTakeTest, EmptyListGivesNothing)
    {
        Memory::SweepList<int> list;

        EXPECT_FALSE(list.try_take().has_value());
    }

    TEST(SweepListTryTakeTest, TakesTheMostRecentlyPushedItem)
    {
        Memory::SweepList<int> list;
        list.push(1);
        list.push(2);
        list.push(3);

        const auto taken = list.try_take();

        ASSERT_TRUE(taken.has_value());
        EXPECT_EQ(*taken, 3);
    }

    TEST(SweepListTryTakeTest, LeavesTheOtherItemsForLaterTakes)
    {
        Memory::SweepList<int> list;
        list.push(1);
        list.push(2);
        list.push(3);

        std::vector<int> taken;
        while (auto item = list.try_take()) {
            taken.push_back(*item);
        }

        std::ranges::sort(taken);
        EXPECT_EQ(taken, (std::vector<int> { 1, 2, 3 }));
        EXPECT_TRUE(list.empty());
    }

    TEST(SweepListTryTakeTest, MovesMoveOnlyItemsOut)
    {
        Memory::SweepList<std::unique_ptr<int>> list;
        list.push(std::make_unique<int>(7));

        auto taken = list.try_take();

        ASSERT_TRUE(taken.has_value());
        ASSERT_NE(*taken, nullptr);
        EXPECT_EQ(**taken, 7);
        EXPECT_TRUE(list.empty());
    }

    TEST(SweepListTryTakeTest, ATakenItemCanBePushedBackAndTakenAgain)
    {
        Memory::SweepList<int> list;
        list.push(4);

        auto first = list.try_take();
        ASSERT_TRUE(first.has_value());
        list.push(*first);

        const auto second = list.try_take();
        ASSERT_TRUE(second.has_value());
        EXPECT_EQ(*second, 4);
    }

    TEST(SweepListTryTakeTest, ConcurrentTakersNeverReceiveTheSameItem)
    {
        constexpr int items = 2000;
        constexpr size_t takers = 4;
        Memory::SweepList<int> list;
        for (int i = 0; i < items; ++i) {
            list.push(i);
        }

        std::vector<std::vector<int>> taken(takers);
        std::vector<std::thread> threads;
        threads.reserve(takers);
        for (size_t taker = 0; taker < takers; ++taker) {
            threads.emplace_back([&list, &taken, taker] {
                for (int misses = 0; misses < 1000;) {
                    if (auto item = list.try_take()) {
                        taken[taker].push_back(*item);
                        misses = 0;
                    } else {
                        ++misses;
                        std::this_thread::yield();
                    }
                }
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }

        std::vector<int> everything;
        for (const auto& group : taken) {
            everything.insert(everything.end(), group.begin(), group.end());
        }
        list.drain([&everything](int& item) { everything.push_back(item); });

        std::ranges::sort(everything);
        ASSERT_EQ(everything.size(), static_cast<size_t>(items));
        for (int i = 0; i < items; ++i) {
            EXPECT_EQ(everything[static_cast<size_t>(i)], i);
        }
    }

    TEST(SweepListTryTakeTest, ConcurrentPushAndTakeLosesNothing)
    {
        constexpr int per_producer = 1000;
        constexpr size_t producers = 3;
        Memory::SweepList<int> list;
        std::atomic<size_t> produced_done { 0 };
        std::vector<int> taken;

        std::vector<std::thread> threads;
        threads.reserve(producers + 1);
        for (size_t producer = 0; producer < producers; ++producer) {
            threads.emplace_back([&list, &produced_done, producer] {
                for (int i = 0; i < per_producer; ++i) {
                    list.push(static_cast<int>(producer) * per_producer + i);
                }
                produced_done.fetch_add(1);
            });
        }
        threads.emplace_back([&list, &produced_done, &taken] {
            while (produced_done.load() < producers) {
                if (auto item = list.try_take()) {
                    taken.push_back(*item);
                }
            }
        });
        for (auto& thread : threads) {
            thread.join();
        }

        list.drain([&taken](int& item) { taken.push_back(item); });

        std::ranges::sort(taken);
        ASSERT_EQ(taken.size(), producers * per_producer);
        for (size_t i = 0; i < taken.size(); ++i) {
            EXPECT_EQ(taken[i], static_cast<int>(i));
        }
    }

} // namespace
} // namespace MayaFlux::Test
