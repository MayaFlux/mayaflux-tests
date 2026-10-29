#include "gtest/gtest.h"

#include "MayaFlux/Transitive/Memory/SnapshotCell.hpp"

namespace MayaFlux::Test {
namespace {

    struct LiveCount {
        static inline std::atomic<int> live { 0 };

        LiveCount() { ++live; }
        LiveCount(const LiveCount&) { ++live; }
        LiveCount(LiveCount&&) noexcept { ++live; }
        LiveCount& operator=(const LiveCount&) = default;
        LiveCount& operator=(LiveCount&&) noexcept = default;
        ~LiveCount() { --live; }
    };

    struct Versioned : LiveCount {
        std::vector<int> values;

        Versioned() = default;
        Versioned(int version, size_t count)
            : values(count, version)
        {
        }
    };

    using Table = std::unordered_map<int, int>;

    class SnapshotCellTest : public ::testing::Test {
    protected:
        void SetUp() override { ASSERT_EQ(LiveCount::live.load(), 0); }
        void TearDown() override { EXPECT_EQ(LiveCount::live.load(), 0); }
    };

    void start_together(std::vector<std::thread>& threads, size_t count, const std::function<void(size_t)>& body)
    {
        std::atomic<bool> go { false };
        for (size_t i = 0; i < count; ++i) {
            threads.emplace_back([&go, &body, i] {
                while (!go.load()) {
                    std::this_thread::yield();
                }
                body(i);
            });
        }
        go.store(true);
        for (auto& thread : threads) {
            thread.join();
        }
    }

    TEST_F(SnapshotCellTest, EmptyCellReadsAnEmptyGuard)
    {
        Memory::SnapshotCell<Versioned> cell;

        const auto guard = cell.read();

        EXPECT_FALSE(guard);
        EXPECT_EQ(guard.get(), nullptr);
    }

    TEST_F(SnapshotCellTest, ConstructedWithAValueReadsIt)
    {
        Memory::SnapshotCell<Versioned> cell(Versioned(5, 3));

        const auto guard = cell.read();

        ASSERT_TRUE(guard);
        EXPECT_EQ(guard->values, std::vector<int>(3, 5));
    }

    TEST_F(SnapshotCellTest, PublishReplacesTheValueForLaterReads)
    {
        Memory::SnapshotCell<Versioned> cell;

        cell.publish(Versioned(1, 2));
        cell.publish(Versioned(2, 4));

        const auto guard = cell.read();
        ASSERT_TRUE(guard);
        EXPECT_EQ(guard->values, std::vector<int>(4, 2));
    }

    TEST_F(SnapshotCellTest, AGuardKeepsItsSnapshotAcrossPublishes)
    {
        Memory::SnapshotCell<Versioned> cell(Versioned(1, 3));
        auto held = cell.read();

        cell.publish(Versioned(2, 3));
        cell.publish(Versioned(3, 3));

        ASSERT_TRUE(held);
        EXPECT_EQ(held->values, std::vector<int>(3, 1));
        EXPECT_EQ(cell.read()->values, std::vector<int>(3, 3));
    }

    TEST_F(SnapshotCellTest, RetiredSnapshotsAreFreedByTheNextWriteOnceNoGuardIsHeld)
    {
        Memory::SnapshotCell<Versioned> cell(Versioned(1, 1));
        auto held = cell.read();

        cell.publish(Versioned(2, 1));
        EXPECT_EQ(LiveCount::live.load(), 2);

        held = {};
        cell.publish(Versioned(3, 1));

        EXPECT_EQ(LiveCount::live.load(), 1);
    }

    TEST_F(SnapshotCellTest, ResetEmptiesTheCellAndFreesTheSnapshot)
    {
        Memory::SnapshotCell<Versioned> cell(Versioned(1, 1));

        cell.reset();

        EXPECT_FALSE(cell.read());
        EXPECT_EQ(LiveCount::live.load(), 0);
    }

    TEST_F(SnapshotCellTest, ResetWhileGuardedKeepsTheGuardedSnapshot)
    {
        Memory::SnapshotCell<Versioned> cell(Versioned(4, 2));
        auto held = cell.read();

        cell.reset();

        EXPECT_FALSE(cell.read());
        ASSERT_TRUE(held);
        EXPECT_EQ(held->values, std::vector<int>(2, 4));
        EXPECT_EQ(LiveCount::live.load(), 1);
    }

    TEST_F(SnapshotCellTest, DestructionFreesTheCurrentAndTheRetiredSnapshots)
    {
        {
            Memory::SnapshotCell<Versioned> cell(Versioned(1, 1));
            auto held = cell.read();
            cell.publish(Versioned(2, 1));
            cell.publish(Versioned(3, 1));
            held = {};
            EXPECT_GE(LiveCount::live.load(), 1);
        }

        EXPECT_EQ(LiveCount::live.load(), 0);
    }

    TEST_F(SnapshotCellTest, UpdateOnAnEmptyCellStartsFromADefaultValue)
    {
        Memory::SnapshotCell<Table> cell;

        const bool published = cell.update([](Table& table) { table[1] = 10; });

        EXPECT_TRUE(published);
        const auto guard = cell.read();
        ASSERT_TRUE(guard);
        EXPECT_EQ(guard->size(), 1U);
        EXPECT_EQ(guard->at(1), 10);
    }

    TEST_F(SnapshotCellTest, UpdateEditsACopyAndLeavesOpenGuardsUntouched)
    {
        Memory::SnapshotCell<Table> cell(Table { { 1, 10 } });
        auto before = cell.read();

        cell.update([](Table& table) { table[2] = 20; });

        EXPECT_EQ(before->size(), 1U);
        const auto after = cell.read();
        EXPECT_EQ(after->size(), 2U);
        EXPECT_EQ(after->at(2), 20);
    }

    TEST_F(SnapshotCellTest, UpdateThatDeclinesPublishesNothing)
    {
        Memory::SnapshotCell<Table> cell(Table { { 1, 10 } });
        const Table* before = cell.read().get();

        const bool published = cell.update([](Table& table) { return table.erase(99) != 0; });

        EXPECT_FALSE(published);
        EXPECT_EQ(cell.read().get(), before);
    }

    TEST_F(SnapshotCellTest, UpdateReturningTrueOnAChangePublishesIt)
    {
        Memory::SnapshotCell<Table> cell(Table { { 1, 10 }, { 2, 20 } });

        const bool published = cell.update([](Table& table) { return table.erase(1) != 0; });

        EXPECT_TRUE(published);
        const auto guard = cell.read();
        EXPECT_EQ(guard->size(), 1U);
        EXPECT_EQ(guard->count(1), 0U);
    }

    TEST_F(SnapshotCellTest, UpdateThatThrowsLeavesTheCellAndReclamationIntact)
    {
        Memory::SnapshotCell<Versioned> cell(Versioned(1, 2));

        EXPECT_THROW(cell.update([](Versioned&) { throw std::runtime_error("edit failed"); }), std::runtime_error);

        EXPECT_EQ(cell.read()->values, std::vector<int>(2, 1));
        cell.publish(Versioned(2, 2));
        EXPECT_EQ(LiveCount::live.load(), 1);
    }

    TEST_F(SnapshotCellTest, GuardMoveTransfersTheSnapshotAndEmptiesTheSource)
    {
        Memory::SnapshotCell<Versioned> cell(Versioned(1, 1));
        auto first = cell.read();

        auto second = std::move(first);

        EXPECT_FALSE(first);
        ASSERT_TRUE(second);
        EXPECT_EQ(second->values, std::vector<int>(1, 1));
    }

    TEST_F(SnapshotCellTest, MoveAssigningAGuardReleasesWhatItHeld)
    {
        Memory::SnapshotCell<Versioned> cell(Versioned(1, 1));
        auto guard = cell.read();
        cell.publish(Versioned(2, 1));
        ASSERT_EQ(LiveCount::live.load(), 2);

        guard = cell.read();
        EXPECT_EQ(guard->values, std::vector<int>(1, 2));
        cell.publish(Versioned(3, 1));
        EXPECT_EQ(LiveCount::live.load(), 3);

        guard = {};
        cell.publish(Versioned(4, 1));

        EXPECT_EQ(LiveCount::live.load(), 1);
    }

    TEST_F(SnapshotCellTest, ConcurrentUpdatesLoseNoInsert)
    {
        constexpr size_t writers = 4;
        constexpr int per_writer = 400;
        Memory::SnapshotCell<Table> cell;

        std::vector<std::thread> threads;
        start_together(threads, writers, [&](size_t writer) {
            for (int i = 0; i < per_writer; ++i) {
                const int key = static_cast<int>(writer) * per_writer + i;
                cell.update([key](Table& table) { table[key] = key * 2; });
            }
        });

        const auto guard = cell.read();
        ASSERT_TRUE(guard);
        EXPECT_EQ(guard->size(), writers * per_writer);
        for (const auto& [key, value] : *guard) {
            EXPECT_EQ(value, key * 2);
        }
    }

    TEST_F(SnapshotCellTest, ConcurrentUpdatesAndErasesAgreeWithTheirNetEffect)
    {
        constexpr size_t workers = 4;
        constexpr int per_worker = 300;
        Memory::SnapshotCell<Table> cell;

        std::vector<std::thread> threads;
        start_together(threads, workers, [&](size_t worker) {
            for (int i = 0; i < per_worker; ++i) {
                const int key = static_cast<int>(worker) * per_worker + i;
                cell.update([key](Table& table) { table[key] = key; });
                if (i % 2 == 0) {
                    cell.update([key](Table& table) { return table.erase(key) != 0; });
                }
            }
        });

        const auto guard = cell.read();
        ASSERT_TRUE(guard);
        EXPECT_EQ(guard->size(), workers * per_worker / 2);
        for (size_t worker = 0; worker < workers; ++worker) {
            for (int i = 0; i < per_worker; ++i) {
                const int key = static_cast<int>(worker) * per_worker + i;
                EXPECT_EQ(guard->count(key), i % 2 == 0 ? 0U : 1U);
            }
        }
    }

    TEST_F(SnapshotCellTest, ReadersDuringUpdatesSeeConsistentGrowingSnapshots)
    {
        constexpr size_t readers = 4;
        constexpr size_t writers = 2;
        constexpr int per_writer = 300;
        Memory::SnapshotCell<Table> cell;
        std::atomic<size_t> finished_writers { 0 };
        std::atomic<bool> consistent { true };

        std::vector<std::thread> threads;
        start_together(threads, readers + writers, [&](size_t index) {
            if (index < writers) {
                for (int i = 0; i < per_writer; ++i) {
                    const int key = static_cast<int>(index) * per_writer + i;
                    cell.update([key](Table& table) { table[key] = key * 2; });
                }
                finished_writers.fetch_add(1);
                return;
            }

            size_t last_size = 0;
            while (finished_writers.load() < writers) {
                const auto guard = cell.read();
                if (!guard) {
                    continue;
                }
                if (guard->size() < last_size) {
                    consistent.store(false);
                }
                last_size = guard->size();
                for (const auto& [key, value] : *guard) {
                    if (value != key * 2) {
                        consistent.store(false);
                    }
                }
            }
        });

        EXPECT_TRUE(consistent.load());
        EXPECT_EQ(cell.read()->size(), writers * per_writer);
    }

    TEST_F(SnapshotCellTest, ReadersDuringPublishesNeverSeeATornSnapshot)
    {
        constexpr size_t readers = 4;
        constexpr int versions = 2000;
        Memory::SnapshotCell<Versioned> cell(Versioned(0, 64));
        std::atomic<bool> done { false };
        std::atomic<bool> uniform { true };

        std::vector<std::thread> threads;
        start_together(threads, readers + 1, [&](size_t index) {
            if (index == 0) {
                for (int version = 1; version <= versions; ++version) {
                    cell.publish(Versioned(version, 64));
                }
                done.store(true);
                return;
            }

            int last_version = 0;
            while (!done.load()) {
                const auto guard = cell.read();
                if (!guard || guard->values.size() != 64) {
                    uniform.store(false);
                    continue;
                }
                const int version = guard->values.front();
                for (const int value : guard->values) {
                    if (value != version) {
                        uniform.store(false);
                    }
                }
                if (version < last_version) {
                    uniform.store(false);
                }
                last_version = version;
            }
        });

        EXPECT_TRUE(uniform.load());
        EXPECT_EQ(cell.read()->values.front(), versions);
    }

    TEST_F(SnapshotCellTest, EveryRetiredSnapshotIsFreedOnceTheContentionEnds)
    {
        constexpr size_t threads_count = 6;
        Memory::SnapshotCell<Versioned> cell(Versioned(0, 8));

        std::vector<std::thread> threads;
        start_together(threads, threads_count, [&](size_t index) {
            for (int i = 0; i < 500; ++i) {
                if (index % 2 == 0) {
                    cell.publish(Versioned(i, 8));
                } else {
                    const auto guard = cell.read();
                    ASSERT_TRUE(guard);
                }
            }
        });

        cell.publish(Versioned(-1, 8));

        EXPECT_EQ(LiveCount::live.load(), 1);
    }

} // namespace
} // namespace MayaFlux::Test
