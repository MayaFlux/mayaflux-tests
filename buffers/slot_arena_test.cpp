#include "gtest/gtest.h"

#include <array>
#include <cstdint>
#include <cstring>

#include "../support/vulkan_test_support.hpp"

#include "MayaFlux/Buffers/Staging/SlotArena.hpp"
#include "MayaFlux/Registry/BackendRegistry.hpp"
#include "MayaFlux/Registry/Service/BufferService.hpp"

namespace MayaFlux::Test {
namespace {

    using Buffers::SlotArena;
    using Buffers::VKBuffer;

    class SlotArenaTest : public VulkanDeviceTest {
    protected:
        static std::array<uint8_t, 16> pattern(uint8_t seed)
        {
            std::array<uint8_t, 16> bytes {};
            for (size_t i = 0; i < bytes.size(); ++i) {
                bytes[i] = static_cast<uint8_t>(seed + i);
            }
            return bytes;
        }
    };

}

TEST_F(SlotArenaTest, InitializesAMappedBufferWithAlignedSlots)
{
    SlotArena arena;
    ASSERT_TRUE(arena.initialize(192, VKBuffer::Usage::UNIFORM, 2));

    EXPECT_TRUE(arena.is_initialized());
    EXPECT_EQ(arena.slot_count(), 2U);
    EXPECT_EQ(arena.slot_bytes(), 192U);

    const auto first = arena.info(0);
    const auto second = arena.info(1);
    EXPECT_TRUE(first.buffer);
    EXPECT_EQ(first.buffer, second.buffer);
    EXPECT_EQ(first.offset, 0U);
    EXPECT_EQ(first.range, 192U);
    EXPECT_GE(second.offset, 192U);
    EXPECT_EQ(second.offset % 256U, 0U);

    arena.release();
}

TEST_F(SlotArenaTest, RejectsUnusableParameters)
{
    SlotArena arena;

    EXPECT_FALSE(arena.initialize(0, VKBuffer::Usage::UNIFORM));
    EXPECT_FALSE(arena.initialize(16, VKBuffer::Usage::UNIFORM, 0));
    EXPECT_FALSE(arena.is_initialized());
}

TEST_F(SlotArenaTest, AcquireReusesASlotOnceItsEpochHasCompleted)
{
    SlotArena arena;
    ASSERT_TRUE(arena.initialize(64, VKBuffer::Usage::UNIFORM, 2));

    EXPECT_EQ(arena.acquire(0), 0U);
    arena.mark_read(0, 5);

    EXPECT_EQ(arena.acquire(4), 1U);
    arena.mark_read(1, 6);

    EXPECT_EQ(arena.acquire(5), 0U);
    EXPECT_EQ(arena.slot_count(), 2U);

    arena.release();
}

TEST_F(SlotArenaTest, AcquireGrowsWhenEverySlotIsStillBeingRead)
{
    SlotArena arena;
    ASSERT_TRUE(arena.initialize(64, VKBuffer::Usage::UNIFORM, 2));
    arena.mark_read(0, 10);
    arena.mark_read(1, 10);

    EXPECT_EQ(arena.acquire(3), 2U);
    EXPECT_EQ(arena.slot_count(), 4U);

    arena.mark_read(2, 11);
    EXPECT_EQ(arena.acquire(3), 3U);

    arena.release();
}

TEST_F(SlotArenaTest, MarkingAnEarlierEpochDoesNotLowerTheStamp)
{
    SlotArena arena;
    ASSERT_TRUE(arena.initialize(64, VKBuffer::Usage::UNIFORM, 1));

    arena.mark_read(0, 8);
    arena.mark_read(0, 2);

    EXPECT_EQ(arena.acquire(5), 1U);
    EXPECT_EQ(arena.slot_count(), 2U);

    arena.release();
}

TEST_F(SlotArenaTest, WrittenBytesLandInTheirSlotAndSurviveGrowth)
{
    SlotArena arena;
    ASSERT_TRUE(arena.initialize(16, VKBuffer::Usage::UNIFORM, 2));

    const auto first = pattern(1);
    const auto second = pattern(100);
    arena.write(0, first.data(), first.size());
    arena.write(1, second.data(), second.size());
    arena.mark_read(0, 9);
    arena.mark_read(1, 9);

    ASSERT_EQ(arena.acquire(0), 2U);

    const auto first_offset = arena.info(0).offset;
    const auto second_offset = arena.info(1).offset;
    auto buffer = arena.detach();
    ASSERT_NE(buffer, nullptr);
    ASSERT_NE(buffer->get_mapped_ptr(), nullptr);

    const auto* mapped = static_cast<const uint8_t*>(buffer->get_mapped_ptr());
    EXPECT_EQ(std::memcmp(mapped + first_offset, first.data(), first.size()), 0);
    EXPECT_EQ(std::memcmp(mapped + second_offset, second.data(), second.size()), 0);

    Registry::BackendRegistry::instance()
        .get_service<Registry::Service::BufferService>()
        ->destroy_buffer(buffer);
}

TEST_F(SlotArenaTest, WritesToAnInvalidSlotOrOversizedDataAreIgnored)
{
    SlotArena arena;
    ASSERT_TRUE(arena.initialize(16, VKBuffer::Usage::UNIFORM, 2));

    const auto bytes = pattern(7);
    const std::array<uint8_t, 32> oversized {};
    EXPECT_NO_THROW(arena.write(5, bytes.data(), bytes.size()));
    EXPECT_NO_THROW(arena.write(0, oversized.data(), oversized.size()));
    EXPECT_NO_THROW(arena.write(0, nullptr, 16));

    arena.release();
}

TEST_F(SlotArenaTest, ReleaseEmptiesTheArenaAndItCanBeInitializedAgain)
{
    SlotArena arena;
    ASSERT_TRUE(arena.initialize(32, VKBuffer::Usage::UNIFORM, 2));

    arena.release();
    EXPECT_FALSE(arena.is_initialized());
    EXPECT_EQ(arena.slot_count(), 0U);

    ASSERT_TRUE(arena.initialize(32, VKBuffer::Usage::UNIFORM, 3));
    EXPECT_EQ(arena.slot_count(), 3U);

    arena.release();
}

TEST_F(SlotArenaTest, InitializingTwiceResetsTheSlotsAndTheirStamps)
{
    SlotArena arena;
    ASSERT_TRUE(arena.initialize(32, VKBuffer::Usage::UNIFORM, 2));
    arena.mark_read(0, 99);
    arena.mark_read(1, 99);

    ASSERT_TRUE(arena.initialize(32, VKBuffer::Usage::UNIFORM, 2));

    EXPECT_TRUE(arena.is_initialized());
    EXPECT_EQ(arena.acquire(0), 0U);
    EXPECT_EQ(arena.slot_count(), 2U);

    arena.release();
}

}
