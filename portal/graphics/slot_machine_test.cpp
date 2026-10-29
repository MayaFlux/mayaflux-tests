#include "gtest/gtest.h"

#include <array>
#include <cstdint>
#include <cstring>

#include "../../support/vulkan_test_support.hpp"

#include "MayaFlux/Buffers/VKBuffer.hpp"
#include "MayaFlux/Portal/Graphics/SlotMachine.hpp"
#include "MayaFlux/Registry/BackendRegistry.hpp"
#include "MayaFlux/Registry/Service/BufferService.hpp"

namespace MayaFlux::Test {
namespace {

    using Portal::Graphics::BufferUsageHint;
    using Portal::Graphics::SlotArena;
    using Portal::Graphics::SlotLease;

    class SlotMachineTest : public VulkanDeviceTest {
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

TEST_F(SlotMachineTest, InitializesAMappedBufferWithAlignedSlots)
{
    SlotArena arena;
    ASSERT_TRUE(arena.initialize(192, BufferUsageHint::UNIFORM, 2));

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

TEST_F(SlotMachineTest, RejectsUnusableParameters)
{
    SlotArena arena;

    EXPECT_FALSE(arena.initialize(0, BufferUsageHint::UNIFORM));
    EXPECT_FALSE(arena.initialize(16, BufferUsageHint::UNIFORM, 0));
    EXPECT_FALSE(arena.is_initialized());
}

TEST_F(SlotMachineTest, AcquireSkipsALeasedSlotAndTakesItBackOnceTheLeaseIsGone)
{
    SlotArena arena;
    ASSERT_TRUE(arena.initialize(64, BufferUsageHint::UNIFORM, 2));

    {
        const SlotLease held = arena.lease(0);
        EXPECT_TRUE(held);
        EXPECT_EQ(arena.acquire(), 1U);
    }

    EXPECT_EQ(arena.acquire(), 0U);
    EXPECT_EQ(arena.slot_count(), 2U);

    arena.release();
}

TEST_F(SlotMachineTest, AcquireGrowsWhenEverySlotIsLeased)
{
    SlotArena arena;
    ASSERT_TRUE(arena.initialize(64, BufferUsageHint::UNIFORM, 2));
    const SlotLease first = arena.lease(0);
    const SlotLease second = arena.lease(1);

    EXPECT_EQ(arena.acquire(), 2U);
    EXPECT_EQ(arena.slot_count(), 4U);

    const SlotLease third = arena.lease(2);
    EXPECT_EQ(arena.acquire(), 3U);

    arena.release();
}

TEST_F(SlotMachineTest, ACopyOfALeaseKeepsTheSlotHeld)
{
    SlotArena arena;
    ASSERT_TRUE(arena.initialize(64, BufferUsageHint::UNIFORM, 1));

    auto original = std::make_unique<SlotLease>(arena.lease(0));
    const SlotLease copy = *original;
    original.reset();

    EXPECT_EQ(arena.acquire(), 1U);
    EXPECT_EQ(arena.slot_count(), 2U);

    arena.release();
}

TEST_F(SlotMachineTest, AMovedLeaseKeepsTheSlotHeldAndTheSourceHoldsNothing)
{
    SlotArena arena;
    ASSERT_TRUE(arena.initialize(64, BufferUsageHint::UNIFORM, 2));

    SlotLease source = arena.lease(0);
    SlotLease target = std::move(source);

    EXPECT_FALSE(source);
    EXPECT_TRUE(target);
    EXPECT_EQ(arena.acquire(), 1U);

    arena.release();
}

TEST_F(SlotMachineTest, LeasingAnInvalidSlotGivesAnEmptyLease)
{
    SlotArena arena;
    ASSERT_TRUE(arena.initialize(64, BufferUsageHint::UNIFORM, 2));

    EXPECT_FALSE(arena.lease(2));
    EXPECT_FALSE(arena.lease(99));
    EXPECT_FALSE(SlotLease {});

    arena.release();
}

TEST_F(SlotMachineTest, WrittenBytesLandInTheirSlotAndSurviveGrowth)
{
    SlotArena arena;
    ASSERT_TRUE(arena.initialize(16, BufferUsageHint::UNIFORM, 2));

    const auto first = pattern(1);
    const auto second = pattern(100);
    arena.write(0, first.data(), first.size());
    arena.write(1, second.data(), second.size());
    const SlotLease first_lease = arena.lease(0);
    const SlotLease second_lease = arena.lease(1);

    ASSERT_EQ(arena.acquire(), 2U);

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

TEST_F(SlotMachineTest, WritesToAnInvalidSlotOrOversizedDataAreIgnored)
{
    SlotArena arena;
    ASSERT_TRUE(arena.initialize(16, BufferUsageHint::UNIFORM, 2));

    const auto bytes = pattern(7);
    const std::array<uint8_t, 32> oversized {};
    EXPECT_NO_THROW(arena.write(5, bytes.data(), bytes.size()));
    EXPECT_NO_THROW(arena.write(0, oversized.data(), oversized.size()));
    EXPECT_NO_THROW(arena.write(0, nullptr, 16));

    arena.release();
}

TEST_F(SlotMachineTest, ALeaseOutlivingTheArenaIsHarmless)
{
    SlotLease survivor;
    {
        SlotArena arena;
        ASSERT_TRUE(arena.initialize(64, BufferUsageHint::UNIFORM, 2));
        survivor = arena.lease(1);
        arena.release();
    }

    EXPECT_TRUE(survivor);
    EXPECT_NO_THROW(survivor = SlotLease {});
}

TEST_F(SlotMachineTest, ReleaseEmptiesTheArenaAndItCanBeInitializedAgain)
{
    SlotArena arena;
    ASSERT_TRUE(arena.initialize(32, BufferUsageHint::UNIFORM, 2));

    arena.release();
    EXPECT_FALSE(arena.is_initialized());
    EXPECT_EQ(arena.slot_count(), 0U);

    ASSERT_TRUE(arena.initialize(32, BufferUsageHint::UNIFORM, 3));
    EXPECT_EQ(arena.slot_count(), 3U);

    arena.release();
}

TEST_F(SlotMachineTest, InitializingTwiceStartsFromFreeSlots)
{
    SlotArena arena;
    ASSERT_TRUE(arena.initialize(32, BufferUsageHint::UNIFORM, 2));
    const SlotLease held = arena.lease(0);

    ASSERT_TRUE(arena.initialize(32, BufferUsageHint::UNIFORM, 2));

    EXPECT_EQ(arena.acquire(), 0U);
    EXPECT_EQ(arena.slot_count(), 2U);

    arena.release();
}

}
