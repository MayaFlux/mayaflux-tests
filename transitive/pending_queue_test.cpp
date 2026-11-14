#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Transitive/Memory/PendingQueue.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    struct QueuePayload {
        std::shared_ptr<int> owner;
        uint64_t id { 0 };
    };

    template <size_t Capacity>
    using Queue = Memory::PendingQueue<QueuePayload, Capacity>;

    void run_together(size_t count, const std::function<void(size_t)>& body)
    {
        std::vector<std::thread> threads;
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

    void push_blocking(auto& queue, uint64_t id)
    {
        while (!queue.push(QueuePayload { nullptr, id })) {
            std::this_thread::yield();
        }
    }

    std::vector<uint64_t> drain_ids(auto& queue)
    {
        std::vector<uint64_t> ids;
        queue.drain([&ids](const QueuePayload& op) { ids.push_back(op.id); });
        return ids;
    }

    constexpr auto TIMEOUT = std::chrono::seconds(20);

}

TEST(PendingQueueTest, DrainOnEmptyAppliesNothing)
{
    Queue<8> queue;

    EXPECT_FALSE(queue.has_pending());
    EXPECT_EQ(queue.drain([](const QueuePayload&) { FAIL(); }), 0U);
}

TEST(PendingQueueTest, OperationsApplyInClaimOrder)
{
    Queue<8> queue;

    for (uint64_t id = 0; id < 5; ++id) {
        ASSERT_TRUE(queue.push(QueuePayload { nullptr, id }));
    }

    EXPECT_TRUE(queue.has_pending());
    EXPECT_EQ(drain_ids(queue), (std::vector<uint64_t> { 0, 1, 2, 3, 4 }));
    EXPECT_FALSE(queue.has_pending());
}

TEST(PendingQueueTest, FullQueueRejectsWithoutConsumingThePayload)
{
    Queue<4> queue;

    for (uint64_t id = 0; id < 4; ++id) {
        ASSERT_TRUE(queue.push(QueuePayload { nullptr, id }));
    }

    QueuePayload rejected { std::make_shared<int>(7), 9 };
    EXPECT_FALSE(queue.push(std::move(rejected)));
    ASSERT_NE(rejected.owner, nullptr);
    EXPECT_EQ(*rejected.owner, 7);
    EXPECT_EQ(rejected.id, 9U);

    EXPECT_EQ(drain_ids(queue), (std::vector<uint64_t> { 0, 1, 2, 3 }));
    EXPECT_TRUE(queue.push(std::move(rejected)));
    EXPECT_EQ(drain_ids(queue), (std::vector<uint64_t> { 9 }));
}

TEST(PendingQueueTest, DrainedCellsDoNotRetainPayloads)
{
    Queue<4> queue;
    auto owner = std::make_shared<int>(1);

    ASSERT_TRUE(queue.push(QueuePayload { owner, 1 }));
    EXPECT_EQ(owner.use_count(), 2);

    EXPECT_EQ(queue.drain([](const QueuePayload&) { }), 1U);
    EXPECT_EQ(owner.use_count(), 1);
}

TEST(PendingQueueTest, WrapsAroundCapacityRepeatedly)
{
    Queue<4> queue;
    uint64_t next = 0;
    uint64_t expected = 0;

    for (int round = 0; round < 25; ++round) {
        for (int k = 0; k < 3; ++k) {
            ASSERT_TRUE(queue.push(QueuePayload { nullptr, next++ }));
        }
        for (const uint64_t id : drain_ids(queue)) {
            EXPECT_EQ(id, expected++);
        }
    }

    EXPECT_EQ(expected, next);
}

TEST(PendingQueueTest, OperationsPushedByApplyWaitForTheNextDrain)
{
    Queue<8> queue;
    ASSERT_TRUE(queue.push(QueuePayload { nullptr, 1 }));

    std::vector<uint64_t> applied;
    const size_t first = queue.drain([&](const QueuePayload& op) {
        applied.push_back(op.id);
        ASSERT_TRUE(queue.push(QueuePayload { nullptr, op.id + 1 }));
    });

    EXPECT_EQ(first, 1U);
    EXPECT_EQ(applied, (std::vector<uint64_t> { 1 }));
    EXPECT_TRUE(queue.has_pending());

    EXPECT_EQ(drain_ids(queue), (std::vector<uint64_t> { 2 }));
}

TEST(PendingQueueTest, RearmingApplyCannotKeepOneDrainRunning)
{
    Queue<8> queue;
    ASSERT_TRUE(queue.push(QueuePayload { nullptr, 0 }));

    for (uint64_t round = 0; round < 5; ++round) {
        const size_t applied = queue.drain([&queue](const QueuePayload& op) {
            ASSERT_TRUE(queue.push(QueuePayload { nullptr, op.id + 1 }));
        });
        EXPECT_EQ(applied, 1U);
    }
}

TEST(PendingQueueTest, ConcurrentProducersLoseNothingAndKeepTheirOwnOrder)
{
    constexpr size_t PRODUCERS = 4;
    constexpr uint64_t PER_PRODUCER = 5000;
    constexpr uint64_t TOTAL = PRODUCERS * PER_PRODUCER;

    Queue<64> queue;
    std::vector<uint64_t> applied;
    applied.reserve(TOTAL);

    std::thread drainer([&] {
        const auto deadline = std::chrono::steady_clock::now() + TIMEOUT;
        while (applied.size() < TOTAL && std::chrono::steady_clock::now() < deadline) {
            queue.drain([&applied](const QueuePayload& op) { applied.push_back(op.id); });
            std::this_thread::yield();
        }
    });

    run_together(PRODUCERS, [&](size_t producer) {
        for (uint64_t i = 0; i < PER_PRODUCER; ++i) {
            push_blocking(queue, producer * PER_PRODUCER + i);
        }
    });
    drainer.join();

    ASSERT_EQ(applied.size(), TOTAL);

    std::vector<int64_t> last(PRODUCERS, -1);
    for (const uint64_t id : applied) {
        const size_t producer = id / PER_PRODUCER;
        EXPECT_GT(static_cast<int64_t>(id), last[producer]);
        last[producer] = static_cast<int64_t>(id);
    }
}

TEST(PendingQueueTest, OverlappingDrainersApplyEveryOperationExactlyOnce)
{
    constexpr size_t PRODUCERS = 3;
    constexpr size_t DRAINERS = 3;
    constexpr uint64_t PER_PRODUCER = 4000;
    constexpr uint64_t TOTAL = PRODUCERS * PER_PRODUCER;

    Queue<64> queue;
    std::vector<std::atomic<int>> seen(TOTAL);
    std::atomic<uint64_t> applied { 0 };
    std::atomic<size_t> returned { 0 };

    run_together(PRODUCERS + DRAINERS, [&](size_t index) {
        if (index < PRODUCERS) {
            for (uint64_t i = 0; i < PER_PRODUCER; ++i) {
                push_blocking(queue, index * PER_PRODUCER + i);
            }
            return;
        }

        const auto deadline = std::chrono::steady_clock::now() + TIMEOUT;
        while (applied.load() < TOTAL && std::chrono::steady_clock::now() < deadline) {
            const size_t count = queue.drain([&](const QueuePayload& op) {
                seen[op.id].fetch_add(1);
                applied.fetch_add(1);
            });
            returned.fetch_add(count);
            std::this_thread::yield();
        }
    });

    EXPECT_EQ(applied.load(), TOTAL);
    EXPECT_EQ(returned.load(), TOTAL);
    for (uint64_t id = 0; id < TOTAL; ++id) {
        ASSERT_EQ(seen[id].load(), 1) << "operation " << id;
    }
    EXPECT_FALSE(queue.has_pending());
}

TEST(PendingQueueTest, ConcurrentDrainsSplitAFullBacklogWithoutOverlap)
{
    constexpr size_t DRAINERS = 4;
    constexpr uint64_t BACKLOG = 1024;

    Queue<BACKLOG> queue;
    for (uint64_t id = 0; id < BACKLOG; ++id) {
        ASSERT_TRUE(queue.push(QueuePayload { nullptr, id }));
    }

    std::vector<std::atomic<int>> seen(BACKLOG);
    std::atomic<size_t> returned { 0 };

    run_together(DRAINERS, [&](size_t) {
        returned.fetch_add(queue.drain([&](const QueuePayload& op) { seen[op.id].fetch_add(1); }));
    });

    EXPECT_EQ(returned.load(), BACKLOG);
    for (uint64_t id = 0; id < BACKLOG; ++id) {
        ASSERT_EQ(seen[id].load(), 1) << "operation " << id;
    }
    EXPECT_FALSE(queue.has_pending());
}

}
