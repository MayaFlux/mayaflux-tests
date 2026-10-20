#include "gtest/gtest.h"

#include "MayaFlux/Nodes/Conduit/Constant.hpp"

namespace MayaFlux::Test {
namespace {

    using Nodes::HookId;
    using Nodes::HookList;
    using Nodes::NodeContext;

    class NodeHookListTest : public ::testing::TestWithParam<size_t> {
    protected:
        NodeContext& context()
        {
            Nodes::Node& base = owner;
            return base.get_last_context();
        }

        Nodes::Constant owner { 1.0 };
        HookList list { owner };
        int count {};
        bool gate {};
    };

    TEST_P(NodeHookListTest, IdsAreNonZeroAndUnique)
    {
        std::vector<HookId> ids;
        ids.reserve(GetParam());
        for (size_t i = 0; i < GetParam(); ++i) {
            ids.push_back(list.add([](NodeContext&) { }, typeid(void)));
        }

        for (const HookId id : ids) {
            EXPECT_NE(id, 0U);
        }
        std::ranges::sort(ids);
        EXPECT_EQ(std::ranges::adjacent_find(ids), ids.end());
    }

    TEST_P(NodeHookListTest, FireRunsEntriesInInsertionOrder)
    {
        std::vector<size_t> order;
        for (size_t i = 0; i < GetParam(); ++i) {
            list.add([&order, i](NodeContext&) { order.push_back(i); }, typeid(void));
        }

        list.fire(context());

        ASSERT_EQ(order.size(), GetParam());
        for (size_t i = 0; i < order.size(); ++i) {
            EXPECT_EQ(order[i], i);
        }
    }

    TEST_P(NodeHookListTest, RemoveByIdRemovesOnlyThatEntry)
    {
        std::vector<HookId> ids;
        ids.reserve(GetParam());
        for (size_t i = 0; i < GetParam(); ++i) {
            ids.push_back(list.add([this](NodeContext&) { ++count; }, typeid(void)));
        }

        EXPECT_TRUE(list.remove(ids.front()));
        EXPECT_FALSE(list.remove(ids.front()));
        list.fire(context());

        EXPECT_EQ(count, static_cast<int>(GetParam()) - 1);
    }

    TEST_P(NodeHookListTest, OwnerUnhookRemovesTheRegistrationFromWhicheverListHoldsIt)
    {
        std::vector<HookId> ids;
        ids.reserve(GetParam());
        for (size_t i = 0; i < GetParam(); ++i) {
            ids.push_back(list.add([this](NodeContext&) { ++count; }, typeid(void)));
        }

        EXPECT_TRUE(owner.unhook(ids.back()));
        EXPECT_FALSE(owner.unhook(ids.back()));
        list.fire(context());

        EXPECT_EQ(count, static_cast<int>(GetParam()) - 1);
    }

    TEST_P(NodeHookListTest, ClearEmptiesTheList)
    {
        for (size_t i = 0; i < GetParam(); ++i) {
            list.add([](NodeContext&) { }, typeid(void));
        }

        list.clear();

        EXPECT_TRUE(list.empty());
        EXPECT_EQ(list.size(), 0U);
    }

    TEST_P(NodeHookListTest, OwnerRemoveAllHooksClearsTheList)
    {
        for (size_t i = 0; i < GetParam(); ++i) {
            list.add([this](NodeContext&) { ++count; }, typeid(void));
        }

        owner.remove_all_hooks();
        list.fire(context());

        EXPECT_TRUE(list.empty());
        EXPECT_EQ(count, 0);
    }

    TEST_P(NodeHookListTest, AddUniqueReturnsTheExistingIdForARepeatedKeyAndParams)
    {
        std::vector<HookId> ids;
        ids.reserve(GetParam());
        for (size_t i = 0; i < GetParam(); ++i) {
            ids.push_back(list.add_unique([](NodeContext&) { }, typeid(int)));
        }

        EXPECT_EQ(list.size(), 1U);
        for (const HookId id : ids) {
            EXPECT_NE(id, 0U);
            EXPECT_EQ(id, ids.front());
        }
    }

    TEST_P(NodeHookListTest, AddUniqueKeepsDistinctParams)
    {
        for (size_t i = 0; i < GetParam(); ++i) {
            list.add_unique([](NodeContext&) { }, typeid(int), 0, { static_cast<double>(i), 0.0 });
        }

        EXPECT_EQ(list.size(), GetParam());
    }

    TEST_P(NodeHookListTest, AddDoesNotDeduplicate)
    {
        for (size_t i = 0; i < GetParam(); ++i) {
            list.add([](NodeContext&) { }, typeid(int));
        }

        EXPECT_EQ(list.size(), GetParam());
    }

    TEST_P(NodeHookListTest, ConditionalEntriesFireOnlyWhileTheirConditionHolds)
    {
        for (size_t i = 0; i < GetParam(); ++i) {
            list.add_conditional(
                [this](NodeContext&) { ++count; },
                [this](NodeContext&) { return gate; },
                typeid(void));
        }

        gate = false;
        list.fire(context());
        EXPECT_EQ(count, 0);

        gate = true;
        list.fire(context());
        EXPECT_EQ(count, static_cast<int>(GetParam()));
    }

    TEST_P(NodeHookListTest, AddConditionalUniqueReturnsTheExistingIdForTheSameHookAndCondition)
    {
        std::vector<HookId> ids;
        ids.reserve(GetParam());
        for (size_t i = 0; i < GetParam(); ++i) {
            ids.push_back(list.add_conditional_unique(
                [](NodeContext&) { },
                [](NodeContext&) { return true; },
                typeid(int)));
        }

        EXPECT_EQ(list.size(), 1U);
        for (const HookId id : ids) {
            EXPECT_EQ(id, ids.front());
        }
    }

    TEST_P(NodeHookListTest, RemoveByKeyRemovesEveryMatchingEntry)
    {
        for (size_t i = 0; i < GetParam(); ++i) {
            list.add([](NodeContext&) { }, typeid(int));
        }
        list.add([](NodeContext&) { }, typeid(double));

        EXPECT_TRUE(list.remove_by_key(typeid(int)));
        EXPECT_FALSE(list.remove_by_key(typeid(int)));

        const auto remaining = list.snapshot();
        ASSERT_EQ(remaining.size(), 1U);
        EXPECT_EQ(*remaining.begin()->key, typeid(double));
    }

    TEST_P(NodeHookListTest, RemoveFirstByKeyRemovesExactlyOne)
    {
        for (size_t i = 0; i < GetParam(); ++i) {
            list.add([](NodeContext&) { }, typeid(int));
        }

        EXPECT_TRUE(list.remove_first_by_key(typeid(int)));

        EXPECT_EQ(list.size(), GetParam() - 1);
        EXPECT_FALSE(list.remove_first_by_key(typeid(double)));
    }

    TEST_P(NodeHookListTest, RemoveByTagRemovesOnlyThatTag)
    {
        for (size_t i = 0; i < GetParam(); ++i) {
            list.add([](NodeContext&) { }, typeid(void), 1);
        }
        list.add([](NodeContext&) { }, typeid(void), 2);

        EXPECT_TRUE(list.remove_by_tag(1));

        const auto remaining = list.snapshot();
        ASSERT_EQ(remaining.size(), 1U);
        EXPECT_EQ(remaining.begin()->tag, 2U);
    }

    TEST_P(NodeHookListTest, RemoveByConditionKeyMatchesTheConditionType)
    {
        auto condition = [](NodeContext&) { return true; };
        for (size_t i = 0; i < GetParam(); ++i) {
            list.add_conditional([](NodeContext&) { }, condition, typeid(void));
        }
        list.add([](NodeContext&) { }, typeid(void));

        EXPECT_TRUE(list.remove_by_condition_key(Nodes::NodeCondition(condition).target_type()));

        EXPECT_EQ(list.size(), 1U);
    }

    TEST_P(NodeHookListTest, SnapshotKeepsItsEntriesWhileTheListChanges)
    {
        for (size_t i = 0; i < GetParam(); ++i) {
            list.add([](NodeContext&) { }, typeid(void));
        }

        const auto before = list.snapshot();
        list.clear();
        list.add([](NodeContext&) { }, typeid(int));

        EXPECT_EQ(before.size(), GetParam());
        EXPECT_EQ(list.size(), 1U);
    }

    TEST_P(NodeHookListTest, AHookMayRemoveItselfWhileItRuns)
    {
        HookId self = 0;
        self = list.add([this, &self](NodeContext&) {
            ++count;
            list.remove(self);
        },
            typeid(void));
        int others = 0;
        for (size_t i = 0; i < GetParam(); ++i) {
            list.add([&others](NodeContext&) { ++others; }, typeid(int));
        }

        list.fire(context());
        list.fire(context());

        EXPECT_EQ(count, 1);
        EXPECT_EQ(others, static_cast<int>(2 * GetParam()));
    }

    TEST_P(NodeHookListTest, AHookMayAddHooksWhileItRuns)
    {
        list.add([this](NodeContext&) {
            for (size_t i = 0; i < GetParam(); ++i) {
                list.add([this](NodeContext&) { ++count; }, typeid(int));
            }
        },
            typeid(void));

        list.fire(context());
        EXPECT_EQ(count, 0);
        EXPECT_EQ(list.size(), GetParam() + 1);

        list.fire(context());
        EXPECT_EQ(count, static_cast<int>(GetParam()));
    }

    TEST_P(NodeHookListTest, ConcurrentWritersNeverDisturbAFiringReader)
    {
        std::atomic<bool> stop { false };
        std::atomic<int> fired { 0 };

        std::thread reader([&] {
            for (int i = 0; i < 20000; ++i) {
                list.fire(context());
            }
            stop.store(true);
        });

        std::vector<std::thread> writers;
        writers.reserve(GetParam());
        for (size_t w = 0; w < GetParam(); ++w) {
            writers.emplace_back([&] {
                while (!stop.load()) {
                    const HookId id = list.add([&fired](NodeContext&) { fired.fetch_add(1); }, typeid(void));
                    list.add_unique([](NodeContext&) { }, typeid(int));
                    list.remove(id);
                    list.remove_by_key(typeid(int));
                }
            });
        }

        reader.join();
        for (auto& writer : writers) {
            writer.join();
        }

        list.clear();
        const int after = fired.load();
        list.fire(context());
        EXPECT_EQ(fired.load(), after);
        EXPECT_TRUE(list.empty());
    }

    INSTANTIATE_TEST_SUITE_P(
        Sizes,
        NodeHookListTest,
        ::testing::Values(size_t { 1 }, size_t { 2 }, size_t { 5 }));

}
}
