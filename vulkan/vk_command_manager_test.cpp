#include "gtest/gtest.h"

#include "../support/vulkan_test_support.hpp"

#include "MayaFlux/Core/Backends/Graphics/Vulkan/VKCommandManager.hpp"
#include "MayaFlux/Core/Backends/Graphics/Vulkan/VKContext.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace MayaFlux::Test {
namespace {

    class VKCommandManagerTest : public VulkanDeviceTest {
    protected:
        static Core::VKContext& context() { return backend().get_context(); }
        static Core::VKCommandManager& commands() { return backend().get_command_manager(); }

        static uint32_t graphics_family() { return context().get_queue_families().graphics_family.value(); }
        static uint32_t compute_family() { return context().get_queue_families().compute_family.value(); }

        static bool initialize_standalone(Core::VKCommandManager& manager)
        {
            return manager.initialize(
                context().get_device(), graphics_family(), compute_family(),
                context().get_graphics_queue(), context().get_compute_queue(), context().get_transfer_queue());
        }
    };

}

TEST_F(VKCommandManagerTest, AllocatesPrimaryAndSecondaryBuffers)
{
    auto primary = commands().allocate_command_buffer();
    auto secondary = commands().allocate_command_buffer(vk::CommandBufferLevel::eSecondary);

    EXPECT_TRUE(primary);
    EXPECT_TRUE(secondary);
    EXPECT_NE(static_cast<VkCommandBuffer>(primary), static_cast<VkCommandBuffer>(secondary));

    commands().free_command_buffer(primary);
    commands().free_command_buffer(secondary);
}

TEST_F(VKCommandManagerTest, FreeingNullOrUnknownBufferIsNoOp)
{
    EXPECT_NO_THROW(commands().free_command_buffer(vk::CommandBuffer {}));
}

TEST_F(VKCommandManagerTest, FreedBufferIsNotFreedTwice)
{
    auto cmd = commands().allocate_command_buffer();
    commands().free_command_buffer(cmd);
    EXPECT_NO_THROW(commands().free_command_buffer(cmd));
}

TEST_F(VKCommandManagerTest, SingleTimeGraphicsCommandsSubmitAndComplete)
{
    auto cmd = commands().begin_single_time_commands();
    ASSERT_TRUE(cmd);

    EXPECT_NO_THROW(commands().end_single_time_commands(cmd, context().get_graphics_queue()));
}

TEST_F(VKCommandManagerTest, SingleTimeComputeCommandsSubmitOnComputeQueue)
{
    auto cmd = commands().begin_single_time_commands_compute();
    ASSERT_TRUE(cmd);

    EXPECT_NO_THROW(commands().end_single_time_commands(cmd, context().get_compute_queue()));
}

TEST_F(VKCommandManagerTest, FreedBufferLeavesTheManagerAbleToAllocateAgain)
{
    auto first = commands().allocate_command_buffer();
    ASSERT_TRUE(first);
    commands().free_command_buffer(first);

    auto second = commands().allocate_command_buffer();
    EXPECT_TRUE(second);
    commands().free_command_buffer(second);
}

TEST_F(VKCommandManagerTest, LiveBuffersGetDistinctHandlesAndOwnPools)
{
    std::vector<vk::CommandBuffer> live;
    for (int i = 0; i < 16; ++i) {
        auto cmd = commands().begin_single_time_commands();
        ASSERT_TRUE(cmd);
        cmd.end();
        live.push_back(cmd);
    }

    std::vector<VkCommandBuffer> handles;
    for (const auto cmd : live) {
        handles.push_back(static_cast<VkCommandBuffer>(cmd));
    }
    std::ranges::sort(handles);
    EXPECT_EQ(std::ranges::adjacent_find(handles), handles.end());

    for (const auto cmd : live) {
        commands().free_command_buffer(cmd);
    }
}

TEST_F(VKCommandManagerTest, TimelineIsExposedAfterInitialize)
{
    EXPECT_TRUE(commands().get_timeline(context().get_graphics_queue()));
}

TEST_F(VKCommandManagerTest, SubmitReturnsStrictlyIncreasingValues)
{
    const auto queue = context().get_graphics_queue();

    const auto first = commands().submit(queue, {});
    const auto second = commands().submit(queue, {});

    EXPECT_GT(first.value, 0U);
    EXPECT_EQ(second.timeline, first.timeline);
    EXPECT_GT(second.value, first.value);
    EXPECT_EQ(commands().submitted_value(second.timeline), second.value);
}

TEST_F(VKCommandManagerTest, WaitingOnASubmissionCompletesIt)
{
    const auto point = commands().submit(context().get_graphics_queue(), {});
    ASSERT_TRUE(point);

    commands().wait(point);

    EXPECT_TRUE(commands().is_complete(point));
    EXPECT_GE(commands().completed_value(point.timeline), point.value);
}

TEST_F(VKCommandManagerTest, WaitingOnZeroReturnsImmediately)
{
    EXPECT_NO_THROW(commands().wait({}));
    EXPECT_TRUE(commands().is_complete({}));
}

TEST_F(VKCommandManagerTest, AliasedQueuesShareOneTimelineAndDistinctQueuesDoNot)
{
    const auto graphics = commands().submit(context().get_graphics_queue(), {});
    const auto compute = commands().submit(context().get_compute_queue(), {});
    ASSERT_TRUE(graphics);
    ASSERT_TRUE(compute);

    const bool aliased = context().get_compute_queue() == context().get_graphics_queue();
    EXPECT_EQ(graphics.timeline == compute.timeline, aliased);
    if (aliased) {
        EXPECT_GT(compute.value, graphics.value);
    }

    commands().wait(commands().submitted());
    EXPECT_TRUE(commands().is_complete(graphics));
    EXPECT_TRUE(commands().is_complete(compute));
}

TEST_F(VKCommandManagerTest, TimelineCountMatchesDistinctQueues)
{
    std::vector<vk::Queue> distinct;
    for (const vk::Queue queue : { context().get_graphics_queue(), context().get_compute_queue(), context().get_transfer_queue() }) {
        if (std::ranges::find(distinct, queue) == distinct.end()) {
            distinct.push_back(queue);
        }
    }

    EXPECT_EQ(commands().timeline_count(), distinct.size());
    for (const vk::Queue queue : distinct) {
        EXPECT_LT(commands().timeline_index(queue), commands().timeline_count());
    }
}

TEST_F(VKCommandManagerTest, SubmitExecutesRecordedCommandBuffers)
{
    auto cmd = commands().begin_single_time_commands();
    ASSERT_TRUE(cmd);
    cmd.end();

    const vk::CommandBuffer recorded[] = { cmd };
    const auto point = commands().submit(context().get_graphics_queue(), recorded);
    ASSERT_TRUE(point);

    commands().wait(point);
    EXPECT_TRUE(commands().is_complete(point));
    commands().free_command_buffer(cmd);
}

TEST_F(VKCommandManagerTest, RetiredBufferIsFreedOnceItsSubmissionCompletes)
{
    auto cmd = commands().begin_single_time_commands();
    ASSERT_TRUE(cmd);
    cmd.end();

    const vk::CommandBuffer recorded[] = { cmd };
    const auto point = commands().submit(context().get_graphics_queue(), recorded);
    ASSERT_TRUE(point);

    commands().retire(cmd, point);
    commands().wait(point);
    EXPECT_NO_THROW(commands().collect());
    EXPECT_NO_THROW(commands().free_command_buffer(cmd));
}

TEST_F(VKCommandManagerTest, RetiringAgainstZeroFreesImmediately)
{
    auto cmd = commands().allocate_command_buffer();
    ASSERT_TRUE(cmd);

    commands().retire(cmd, {});
    EXPECT_NO_THROW(commands().free_command_buffer(cmd));
}

TEST_F(VKCommandManagerTest, RetiringANullBufferIsNoOp)
{
    EXPECT_NO_THROW(commands().retire(vk::CommandBuffer {}, Core::TimelinePoint { .timeline = 0, .value = 1 }));
    EXPECT_NO_THROW(commands().collect());
}

TEST_F(VKCommandManagerTest, RetiredResourceIsReleasedOnceItsSubmissionCompletes)
{
    const auto point = commands().submit(context().get_graphics_queue(), {});
    ASSERT_TRUE(point);

    int released = 0;
    commands().retire_resource([&released] { ++released; }, point);
    commands().wait(point);
    commands().collect();
    commands().collect();

    EXPECT_EQ(released, 1);
}

TEST_F(VKCommandManagerTest, RetiringAResourceAgainstZeroReleasesItImmediately)
{
    bool released = false;
    commands().retire_resource([&released] { released = true; }, {});
    EXPECT_TRUE(released);
}

TEST_F(VKCommandManagerTest, RetiringAnEmptyReleaseActionIsNoOp)
{
    EXPECT_NO_THROW(commands().retire_resource({}, Core::TimelinePoint { .timeline = 0, .value = 1 }));
    EXPECT_NO_THROW(commands().collect());
}

TEST_F(VKCommandManagerTest, ReleaseActionsMayCollectAgain)
{
    const auto point = commands().submit(context().get_graphics_queue(), {});
    ASSERT_TRUE(point);

    bool ran = false;
    commands().retire_resource([&ran] {
        commands().collect();
        ran = true;
    },
        point);
    commands().wait(point);

    EXPECT_NO_THROW(commands().collect());
    EXPECT_TRUE(ran);
}

TEST_F(VKCommandManagerTest, ResourcesRetiredFromManyThreadsAreEachReleasedOnce)
{
    TimelineGate gate(context().get_device());
    const std::array<vk::SemaphoreSubmitInfo, 1> waits { gate.closed() };
    const auto point = commands().submit(context().get_graphics_queue(), {}, waits);
    ASSERT_TRUE(point);

    constexpr int producers = 8;
    constexpr int per_producer = 200;
    std::atomic<int> released { 0 };

    std::vector<std::thread> threads;
    for (int t = 0; t < producers; ++t) {
        threads.emplace_back([&released, point] {
            for (int i = 0; i < per_producer; ++i) {
                commands().retire_resource([&released] { ++released; }, point);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    commands().collect();
    EXPECT_EQ(released.load(), 0);

    gate.open();
    commands().wait(point);
    commands().collect();

    EXPECT_EQ(released.load(), producers * per_producer);
}

TEST_F(VKCommandManagerTest, ConcurrentCollectorsReleaseEveryResourceExactlyOnce)
{
    TimelineGate gate(context().get_device());
    const std::array<vk::SemaphoreSubmitInfo, 1> waits { gate.closed() };
    const auto point = commands().submit(context().get_graphics_queue(), {}, waits);
    ASSERT_TRUE(point);

    constexpr int total = 1000;
    std::atomic<int> released { 0 };
    for (int i = 0; i < total; ++i) {
        commands().retire_resource([&released] { ++released; }, point);
    }

    gate.open();
    commands().wait(point);

    std::vector<std::thread> collectors;
    for (int t = 0; t < 4; ++t) {
        collectors.emplace_back([] {
            for (int i = 0; i < 50; ++i) {
                commands().collect();
            }
        });
    }
    for (auto& collector : collectors) {
        collector.join();
    }
    commands().collect();

    EXPECT_EQ(released.load(), total);
}

TEST_F(VKCommandManagerTest, PendingReleaseActionsAreDroppedAtCleanup)
{
    Core::VKCommandManager manager;
    ASSERT_TRUE(initialize_standalone(manager));

    bool ran = false;
    manager.retire_resource([&ran] { ran = true; }, Core::TimelinePoint { .timeline = 0, .value = 1000 });
    manager.cleanup();

    EXPECT_FALSE(ran);
}

TEST_F(VKCommandManagerTest, DeferredCommandsStartEmpty)
{
    EXPECT_FALSE(commands().has_deferred_commands());
    EXPECT_FALSE(commands().flush_deferred(context().get_graphics_queue()));
}

TEST_F(VKCommandManagerTest, RecordingDeferredCommandsMarksThemPendingAndBatchesIntoOneSubmission)
{
    int calls = 0;
    commands().record_deferred_commands([&calls](vk::CommandBuffer) { ++calls; });
    EXPECT_TRUE(commands().has_deferred_commands());

    commands().record_deferred_commands([&calls](vk::CommandBuffer) { ++calls; });
    EXPECT_EQ(calls, 2);

    const auto point = commands().flush_deferred(context().get_graphics_queue());
    EXPECT_TRUE(point);
    EXPECT_FALSE(commands().has_deferred_commands());

    commands().wait(point);
    EXPECT_TRUE(commands().is_complete(point));
}

TEST_F(VKCommandManagerTest, FlushingDeferredCommandsTwiceSubmitsOnce)
{
    commands().record_deferred_commands([](vk::CommandBuffer) { });

    EXPECT_TRUE(commands().flush_deferred(context().get_graphics_queue()));
    EXPECT_FALSE(commands().flush_deferred(context().get_graphics_queue()));
}

TEST_F(VKCommandManagerTest, StandaloneManagerOwnsItsPoolsUntilCleanup)
{
    Core::VKCommandManager manager;
    ASSERT_TRUE(initialize_standalone(manager));

    auto cmd = manager.allocate_command_buffer();
    EXPECT_TRUE(cmd);
    auto compute = manager.begin_single_time_commands_compute();
    EXPECT_TRUE(compute);

    manager.cleanup();
    EXPECT_FALSE(manager.allocate_command_buffer());
    EXPECT_NO_THROW(manager.free_command_buffer(cmd));
}

TEST_F(VKCommandManagerTest, StandaloneManagerHasItsOwnTimeline)
{
    Core::VKCommandManager manager;
    ASSERT_TRUE(initialize_standalone(manager));

    const auto queue = context().get_graphics_queue();
    EXPECT_NE(manager.get_timeline(queue), commands().get_timeline(queue));

    const uint32_t timeline = manager.timeline_index(queue);
    ASSERT_LT(timeline, manager.timeline_count());
    EXPECT_EQ(manager.submitted_value(timeline), 0U);
    EXPECT_EQ(manager.completed_value(timeline), 0U);

    const auto point = manager.submit(queue, {});
    EXPECT_EQ(point.value, 1U);
    manager.wait(point);
    EXPECT_EQ(manager.completed_value(timeline), 1U);

    manager.cleanup();
    EXPECT_FALSE(manager.get_timeline(queue));
}

TEST_F(VKCommandManagerTest, StandaloneCleanupIsIdempotent)
{
    Core::VKCommandManager manager;
    ASSERT_TRUE(initialize_standalone(manager));
    manager.cleanup();
    EXPECT_NO_THROW(manager.cleanup());
}

TEST_F(VKCommandManagerTest, StandaloneCleanupFreesBuffersStillAllocated)
{
    Core::VKCommandManager manager;
    ASSERT_TRUE(initialize_standalone(manager));
    (void)manager.allocate_command_buffer();
    (void)manager.begin_single_time_commands_compute();
    EXPECT_NO_THROW(manager.cleanup());
}

TEST(TimelineSnapshotTest, APointFillsOnlyItsOwnTimeline)
{
    const Core::TimelineSnapshot snapshot(Core::TimelinePoint { .timeline = 1, .value = 5 });

    EXPECT_EQ(snapshot.values[0], 0U);
    EXPECT_EQ(snapshot.values[1], 5U);
    EXPECT_EQ(snapshot.values[2], 0U);
}

TEST(TimelineSnapshotTest, AnOutOfRangeTimelineIsIgnored)
{
    const Core::TimelineSnapshot snapshot(Core::TimelinePoint { .timeline = Core::k_max_timelines, .value = 5 });

    for (const uint64_t value : snapshot.values) {
        EXPECT_EQ(value, 0U);
    }
}

TEST(TimelineSnapshotTest, AnEmptyPointIsFalseAndAnAssignedOneIsTrue)
{
    EXPECT_FALSE(Core::TimelinePoint {});
    EXPECT_TRUE((Core::TimelinePoint { .timeline = 0, .value = 1 }));
}

TEST_F(VKCommandManagerTest, SubmittingToAQueueWithoutATimelineFails)
{
    EXPECT_FALSE(commands().submit(vk::Queue {}, {}));
    EXPECT_EQ(commands().timeline_index(vk::Queue {}), commands().timeline_count());
    EXPECT_FALSE(commands().get_timeline(vk::Queue {}));
}

TEST_F(VKCommandManagerTest, ASubmissionBlockedOnAWaitIsNotCompleteUntilItIsSatisfied)
{
    TimelineGate gate(context().get_device());
    const std::array<vk::SemaphoreSubmitInfo, 1> waits { gate.closed() };

    const auto point = commands().submit(context().get_graphics_queue(), {}, waits);
    ASSERT_TRUE(point);
    EXPECT_FALSE(commands().is_complete(point));

    auto released = std::make_shared<bool>(false);
    commands().retire_resource([released] { *released = true; }, point);
    commands().collect();
    EXPECT_FALSE(*released);

    gate.open();
    commands().wait(point);
    commands().collect();

    EXPECT_TRUE(commands().is_complete(point));
    EXPECT_TRUE(*released);
}

TEST_F(VKCommandManagerTest, SubmittedSnapshotHoldsAReleaseUntilPendingWorkCompletes)
{
    TimelineGate gate(context().get_device());
    const std::array<vk::SemaphoreSubmitInfo, 1> waits { gate.closed() };

    const auto point = commands().submit(context().get_graphics_queue(), {}, waits);
    ASSERT_TRUE(point);

    const auto everything = commands().submitted();
    EXPECT_FALSE(commands().is_complete(everything));

    auto released = std::make_shared<bool>(false);
    commands().retire_resource([released] { *released = true; }, everything);
    commands().collect();
    EXPECT_FALSE(*released);

    gate.open();
    commands().wait(everything);
    commands().collect();
    EXPECT_TRUE(*released);
}

TEST_F(VKCommandManagerTest, RetiredCommandBufferIsHeldWhileItsSubmissionIsPending)
{
    TimelineGate gate(context().get_device());
    const std::array<vk::SemaphoreSubmitInfo, 1> waits { gate.closed() };

    auto cmd = commands().begin_single_time_commands();
    ASSERT_TRUE(cmd);
    cmd.end();

    const vk::CommandBuffer recorded[] = { cmd };
    const auto point = commands().submit(context().get_graphics_queue(), recorded, waits);
    ASSERT_TRUE(point);
    commands().retire(cmd, point);

    EXPECT_NO_THROW(commands().collect());
    EXPECT_FALSE(commands().is_complete(point));

    gate.open();
    commands().wait(point);
    EXPECT_NO_THROW(commands().collect());
}

TEST_F(VKCommandManagerTest, DistinctQueuesCompleteIndependentlyAndAReleaseWaitsForBoth)
{
    if (context().get_compute_queue() == context().get_graphics_queue()) {
        GTEST_SKIP() << "Compute is aliased to graphics on this device";
    }

    TimelineGate gate(context().get_device());
    const std::array<vk::SemaphoreSubmitInfo, 1> waits { gate.closed() };

    const auto compute = commands().submit(context().get_compute_queue(), {}, waits);
    const auto graphics = commands().submit(context().get_graphics_queue(), {});
    ASSERT_TRUE(compute);
    ASSERT_TRUE(graphics);
    EXPECT_NE(compute.timeline, graphics.timeline);

    commands().wait(graphics);
    EXPECT_TRUE(commands().is_complete(graphics));
    EXPECT_FALSE(commands().is_complete(compute));

    auto after_compute = std::make_shared<bool>(false);
    auto after_everything = std::make_shared<bool>(false);
    commands().retire_resource([after_compute] { *after_compute = true; }, compute);
    commands().retire_resource([after_everything] { *after_everything = true; }, commands().submitted());
    commands().collect();
    EXPECT_FALSE(*after_compute);
    EXPECT_FALSE(*after_everything);

    gate.open();
    commands().wait(commands().submitted());
    commands().collect();

    EXPECT_TRUE(commands().is_complete(compute));
    EXPECT_TRUE(*after_compute);
    EXPECT_TRUE(*after_everything);
}

TEST_F(VKCommandManagerTest, OneWaitCoversPointsOnEveryTimeline)
{
    if (context().get_compute_queue() == context().get_graphics_queue()) {
        GTEST_SKIP() << "Compute is aliased to graphics on this device";
    }

    TimelineGate gate(context().get_device());
    const std::array<vk::SemaphoreSubmitInfo, 1> waits { gate.closed() };

    const auto compute = commands().submit(context().get_compute_queue(), {}, waits);
    const auto graphics = commands().submit(context().get_graphics_queue(), {}, waits);
    ASSERT_TRUE(compute);
    ASSERT_TRUE(graphics);

    Core::TimelineSnapshot both;
    both.values[compute.timeline] = compute.value;
    both.values[graphics.timeline] = graphics.value;
    EXPECT_FALSE(commands().is_complete(both));

    gate.open();
    commands().wait(both);

    EXPECT_TRUE(commands().is_complete(both));
    EXPECT_TRUE(commands().is_complete(compute));
    EXPECT_TRUE(commands().is_complete(graphics));
}

TEST_F(VKCommandManagerTest, WaitingWithATimeoutOnBlockedWorkReturnsWithoutCompleting)
{
    TimelineGate gate(context().get_device());
    const std::array<vk::SemaphoreSubmitInfo, 1> waits { gate.closed() };

    const auto point = commands().submit(context().get_graphics_queue(), {}, waits);
    ASSERT_TRUE(point);

    commands().wait(point, 1'000'000);
    EXPECT_FALSE(commands().is_complete(point));

    gate.open();
    commands().wait(point);
    EXPECT_TRUE(commands().is_complete(point));
}

TEST_F(VKCommandManagerTest, BuffersAllocatedAtOnceFromManyThreadsAreAllDistinct)
{
    constexpr size_t threads_count = 4;
    constexpr size_t per_thread = 24;

    std::array<std::vector<vk::CommandBuffer>, threads_count> held;
    std::atomic<size_t> allocated { 0 };
    std::atomic<bool> release { false };

    std::vector<std::thread> threads;
    threads.reserve(threads_count);
    for (size_t t = 0; t < threads_count; ++t) {
        threads.emplace_back([&, t] {
            for (size_t i = 0; i < per_thread; ++i) {
                auto cmd = commands().begin_single_time_commands();
                if (cmd) {
                    cmd.end();
                }
                held[t].push_back(cmd);
            }
            allocated.fetch_add(1);
            while (!release.load()) {
                std::this_thread::yield();
            }
            for (const auto cmd : held[t]) {
                commands().free_command_buffer(cmd);
            }
        });
    }

    while (allocated.load() < threads_count) {
        std::this_thread::yield();
    }

    std::vector<VkCommandBuffer> handles;
    for (const auto& group : held) {
        for (const auto cmd : group) {
            EXPECT_TRUE(cmd);
            handles.push_back(static_cast<VkCommandBuffer>(cmd));
        }
    }
    std::ranges::sort(handles);
    EXPECT_EQ(std::ranges::adjacent_find(handles), handles.end());
    EXPECT_EQ(handles.size(), threads_count * per_thread);

    release.store(true);
    for (auto& thread : threads) {
        thread.join();
    }
}

TEST_F(VKCommandManagerTest, ThreadsCyclingAllocateRecordAndFreeKeepTheManagerConsistent)
{
    constexpr size_t threads_count = 4;
    constexpr int cycles = 150;
    std::atomic<int> failures { 0 };

    std::vector<std::thread> threads;
    threads.reserve(threads_count);
    for (size_t t = 0; t < threads_count; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < cycles; ++i) {
                auto cmd = (t % 2 == 0) ? commands().begin_single_time_commands()
                                        : commands().begin_single_time_commands_compute();
                if (!cmd) {
                    failures.fetch_add(1);
                    continue;
                }
                cmd.end();
                commands().free_command_buffer(cmd);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(failures.load(), 0);
    auto after = commands().allocate_command_buffer();
    EXPECT_TRUE(after);
    commands().free_command_buffer(after);
}

TEST_F(VKCommandManagerTest, BuffersFreedByAnotherThreadReturnTheirPoolForReuse)
{
    std::vector<vk::CommandBuffer> made;
    for (int i = 0; i < 8; ++i) {
        auto cmd = commands().begin_single_time_commands();
        ASSERT_TRUE(cmd);
        cmd.end();
        made.push_back(cmd);
    }

    std::thread freeing([&made] {
        for (const auto cmd : made) {
            commands().free_command_buffer(cmd);
        }
    });
    freeing.join();

    for (int i = 0; i < 8; ++i) {
        auto cmd = commands().begin_single_time_commands();
        ASSERT_TRUE(cmd);
        cmd.end();
        commands().free_command_buffer(cmd);
    }
}

TEST_F(VKCommandManagerTest, FreeingOneBufferFromManyThreadsAtOnceFreesItOnce)
{
    constexpr size_t threads_count = 6;

    for (int round = 0; round < 20; ++round) {
        auto cmd = commands().allocate_command_buffer();
        ASSERT_TRUE(cmd);

        std::atomic<bool> go { false };
        std::vector<std::thread> threads;
        threads.reserve(threads_count);
        for (size_t t = 0; t < threads_count; ++t) {
            threads.emplace_back([&go, cmd] {
                while (!go.load()) {
                    std::this_thread::yield();
                }
                commands().free_command_buffer(cmd);
            });
        }
        go.store(true);
        for (auto& thread : threads) {
            thread.join();
        }
    }

    auto after = commands().allocate_command_buffer();
    EXPECT_TRUE(after);
    commands().free_command_buffer(after);
}

TEST_F(VKCommandManagerTest, RetiringFromManyThreadsWhileAllocatingFreesEveryBuffer)
{
    constexpr size_t threads_count = 4;
    constexpr int per_thread = 60;
    std::atomic<int> failures { 0 };
    std::mutex submit_mutex;

    std::vector<std::thread> threads;
    threads.reserve(threads_count);
    for (size_t t = 0; t < threads_count; ++t) {
        threads.emplace_back([&failures, &submit_mutex] {
            for (int i = 0; i < per_thread; ++i) {
                auto cmd = commands().begin_single_time_commands();
                if (!cmd) {
                    failures.fetch_add(1);
                    continue;
                }
                cmd.end();
                const vk::CommandBuffer recorded[] = { cmd };
                Core::TimelinePoint point;
                {
                    std::lock_guard lock(submit_mutex);
                    point = commands().submit(context().get_graphics_queue(), recorded);
                }
                commands().retire(cmd, point);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }
    commands().wait(commands().submitted());
    commands().collect();

    EXPECT_EQ(failures.load(), 0);
}

TEST_F(VKCommandManagerTest, DeferredRecordingFromManyThreadsGivesEachRecorderABatchToItself)
{
    constexpr size_t threads_count = 4;
    constexpr int per_thread = 80;

    std::mutex active_mutex;
    std::set<VkCommandBuffer> active;
    std::atomic<int> overlaps { 0 };
    std::atomic<int> recorded { 0 };

    std::vector<std::thread> threads;
    threads.reserve(threads_count);
    for (size_t t = 0; t < threads_count; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < per_thread; ++i) {
                commands().record_deferred_commands([&](vk::CommandBuffer batch) {
                    const auto handle = static_cast<VkCommandBuffer>(batch);
                    {
                        std::lock_guard lock(active_mutex);
                        if (!active.insert(handle).second) {
                            overlaps.fetch_add(1);
                        }
                    }
                    std::this_thread::yield();
                    recorded.fetch_add(1);
                    {
                        std::lock_guard lock(active_mutex);
                        active.erase(handle);
                    }
                });
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(overlaps.load(), 0);
    EXPECT_EQ(recorded.load(), static_cast<int>(threads_count) * per_thread);
    EXPECT_TRUE(commands().has_deferred_commands());

    const auto point = commands().flush_deferred(context().get_graphics_queue());
    ASSERT_TRUE(point);
    EXPECT_FALSE(commands().has_deferred_commands());
    commands().wait(point);
    EXPECT_TRUE(commands().is_complete(point));
}

TEST_F(VKCommandManagerTest, OneThreadRecordingDeferredCommandsKeepsOneBatchAcrossCalls)
{
    std::set<VkCommandBuffer> seen;
    for (int i = 0; i < 5; ++i) {
        commands().record_deferred_commands([&seen](vk::CommandBuffer batch) {
            seen.insert(static_cast<VkCommandBuffer>(batch));
        });
    }

    EXPECT_EQ(seen.size(), 1U);
    const auto point = commands().flush_deferred(context().get_graphics_queue());
    EXPECT_TRUE(point);
    commands().wait(point);
}

TEST(VKCommandManagerUninitializedTest, CleanupWithoutInitializeIsSafe)
{
    Core::VKCommandManager manager;
    EXPECT_NO_THROW(manager.cleanup());
    EXPECT_FALSE(manager.allocate_command_buffer());
    EXPECT_FALSE(manager.get_timeline(vk::Queue {}));
    EXPECT_FALSE(manager.has_deferred_commands());
}

}
