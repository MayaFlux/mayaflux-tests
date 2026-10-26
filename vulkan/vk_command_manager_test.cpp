#include "gtest/gtest.h"

#include "../support/vulkan_test_support.hpp"

#include "MayaFlux/Core/Backends/Graphics/Vulkan/VKCommandManager.hpp"
#include "MayaFlux/Core/Backends/Graphics/Vulkan/VKContext.hpp"

namespace MayaFlux::Test {
namespace {

    class VKCommandManagerTest : public VulkanDeviceTest {
    protected:
        static Core::VKContext& context() { return backend().get_context(); }
        static Core::VKCommandManager& commands() { return backend().get_command_manager(); }

        static uint32_t graphics_family() { return context().get_queue_families().graphics_family.value(); }
        static uint32_t compute_family() { return context().get_queue_families().compute_family.value(); }
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

TEST_F(VKCommandManagerTest, ResetPoolSucceedsWhenNothingIsPending)
{
    auto cmd = commands().allocate_command_buffer();
    commands().free_command_buffer(cmd);
    EXPECT_NO_THROW(commands().reset_pool());
}

TEST_F(VKCommandManagerTest, PoolIsExposedAfterInitialize)
{
    EXPECT_TRUE(commands().get_pool());
}

TEST_F(VKCommandManagerTest, TimelineIsExposedAfterInitialize)
{
    EXPECT_TRUE(commands().get_timeline());
}

TEST_F(VKCommandManagerTest, SubmitReturnsStrictlyIncreasingValues)
{
    const auto queue = context().get_graphics_queue();

    const uint64_t first = commands().submit(queue, {});
    const uint64_t second = commands().submit(queue, {});

    EXPECT_GT(first, 0U);
    EXPECT_GT(second, first);
    EXPECT_EQ(commands().submitted_value(), second);
}

TEST_F(VKCommandManagerTest, WaitingOnASubmissionCompletesIt)
{
    const uint64_t value = commands().submit(context().get_graphics_queue(), {});
    ASSERT_GT(value, 0U);

    commands().wait(value);

    EXPECT_TRUE(commands().is_complete(value));
    EXPECT_GE(commands().completed_value(), value);
}

TEST_F(VKCommandManagerTest, WaitingOnZeroReturnsImmediately)
{
    EXPECT_NO_THROW(commands().wait(0));
    EXPECT_TRUE(commands().is_complete(0));
}

TEST_F(VKCommandManagerTest, SubmitsOnTheComputeQueueShareOneTimeline)
{
    const uint64_t graphics = commands().submit(context().get_graphics_queue(), {});
    const uint64_t compute = commands().submit(context().get_compute_queue(), {});

    EXPECT_GT(compute, graphics);
    commands().wait(compute);
    EXPECT_TRUE(commands().is_complete(graphics));
}

TEST_F(VKCommandManagerTest, SubmitExecutesRecordedCommandBuffers)
{
    auto cmd = commands().begin_single_time_commands();
    ASSERT_TRUE(cmd);
    cmd.end();

    const vk::CommandBuffer recorded[] = { cmd };
    const uint64_t value = commands().submit(context().get_graphics_queue(), recorded);
    ASSERT_GT(value, 0U);

    commands().wait(value);
    EXPECT_TRUE(commands().is_complete(value));
    commands().free_command_buffer(cmd);
}

TEST_F(VKCommandManagerTest, RetiredBufferIsFreedOnceItsSubmissionCompletes)
{
    auto cmd = commands().begin_single_time_commands();
    ASSERT_TRUE(cmd);
    cmd.end();

    const vk::CommandBuffer recorded[] = { cmd };
    const uint64_t value = commands().submit(context().get_graphics_queue(), recorded);
    ASSERT_GT(value, 0U);

    commands().retire(cmd, value);
    commands().wait(value);
    EXPECT_NO_THROW(commands().collect());
    EXPECT_NO_THROW(commands().free_command_buffer(cmd));
}

TEST_F(VKCommandManagerTest, RetiringAgainstZeroFreesImmediately)
{
    auto cmd = commands().allocate_command_buffer();
    ASSERT_TRUE(cmd);

    commands().retire(cmd, 0);
    EXPECT_NO_THROW(commands().free_command_buffer(cmd));
}

TEST_F(VKCommandManagerTest, RetiringANullBufferIsNoOp)
{
    EXPECT_NO_THROW(commands().retire(vk::CommandBuffer {}, 1));
    EXPECT_NO_THROW(commands().collect());
}

TEST_F(VKCommandManagerTest, DeferredCommandsStartEmpty)
{
    EXPECT_FALSE(commands().has_deferred_commands());
    EXPECT_EQ(commands().flush_deferred(context().get_graphics_queue()), 0U);
}

TEST_F(VKCommandManagerTest, RecordingDeferredCommandsMarksThemPendingAndBatchesIntoOneSubmission)
{
    int calls = 0;
    commands().record_deferred_commands([&calls](vk::CommandBuffer) { ++calls; });
    EXPECT_TRUE(commands().has_deferred_commands());

    commands().record_deferred_commands([&calls](vk::CommandBuffer) { ++calls; });
    EXPECT_EQ(calls, 2);

    const uint64_t value = commands().flush_deferred(context().get_graphics_queue());
    EXPECT_GT(value, 0U);
    EXPECT_FALSE(commands().has_deferred_commands());

    commands().wait(value);
    EXPECT_TRUE(commands().is_complete(value));
}

TEST_F(VKCommandManagerTest, FlushingDeferredCommandsTwiceSubmitsOnce)
{
    commands().record_deferred_commands([](vk::CommandBuffer) { });

    EXPECT_GT(commands().flush_deferred(context().get_graphics_queue()), 0U);
    EXPECT_EQ(commands().flush_deferred(context().get_graphics_queue()), 0U);
}

TEST_F(VKCommandManagerTest, StandaloneManagerOwnsItsPoolsUntilCleanup)
{
    Core::VKCommandManager manager;
    ASSERT_TRUE(manager.initialize(context().get_device(), graphics_family(), compute_family()));
    EXPECT_TRUE(manager.get_pool());

    auto cmd = manager.allocate_command_buffer();
    EXPECT_TRUE(cmd);

    manager.cleanup();
    EXPECT_FALSE(manager.get_pool());
}

TEST_F(VKCommandManagerTest, StandaloneManagerHasItsOwnTimeline)
{
    Core::VKCommandManager manager;
    ASSERT_TRUE(manager.initialize(context().get_device(), graphics_family(), compute_family()));

    EXPECT_NE(manager.get_timeline(), commands().get_timeline());
    EXPECT_EQ(manager.submitted_value(), 0U);
    EXPECT_EQ(manager.completed_value(), 0U);

    const uint64_t value = manager.submit(context().get_graphics_queue(), {});
    EXPECT_EQ(value, 1U);
    manager.wait(value);
    EXPECT_EQ(manager.completed_value(), 1U);

    manager.cleanup();
    EXPECT_FALSE(manager.get_timeline());
}

TEST_F(VKCommandManagerTest, StandaloneCleanupIsIdempotent)
{
    Core::VKCommandManager manager;
    ASSERT_TRUE(manager.initialize(context().get_device(), graphics_family(), compute_family()));
    manager.cleanup();
    EXPECT_NO_THROW(manager.cleanup());
}

TEST_F(VKCommandManagerTest, StandaloneCleanupFreesBuffersStillAllocated)
{
    Core::VKCommandManager manager;
    ASSERT_TRUE(manager.initialize(context().get_device(), graphics_family(), compute_family()));
    (void)manager.allocate_command_buffer();
    (void)manager.begin_single_time_commands_compute();
    EXPECT_NO_THROW(manager.cleanup());
}

TEST(VKCommandManagerUninitializedTest, CleanupWithoutInitializeIsSafe)
{
    Core::VKCommandManager manager;
    EXPECT_NO_THROW(manager.cleanup());
    EXPECT_FALSE(manager.get_pool());
    EXPECT_FALSE(manager.get_timeline());
    EXPECT_FALSE(manager.has_deferred_commands());
}

}
