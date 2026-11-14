#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include <chrono>
#include <future>

#include "../support/vulkan_test_support.hpp"

#include "MayaFlux/Buffers/VKBuffer.hpp"
#include "MayaFlux/Core/Backends/Graphics/Vulkan/VKCommandManager.hpp"
#include "MayaFlux/Core/Backends/Graphics/Vulkan/VKEnumUtils.hpp"
#include "MayaFlux/Core/Backends/Graphics/Vulkan/VKImage.hpp"
#include "MayaFlux/Registry/BackendRegistry.hpp"
#include "MayaFlux/Registry/Service/BufferService.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))

namespace MayaFlux::Test {
namespace {

    using Buffers::VKBuffer;
    using Core::BackendResourceManager;
    using Core::VKImage;
    using Registry::Service::BufferService;

    constexpr vk::BufferUsageFlags k_transfer_usage = vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst;

    std::vector<uint8_t> pattern(size_t size, uint8_t seed = 1)
    {
        std::vector<uint8_t> bytes(size);
        std::iota(bytes.begin(), bytes.end(), seed);
        return bytes;
    }

    void* as_handle(vk::Buffer buffer)
    {
        return static_cast<void*>(static_cast<VkBuffer>(buffer));
    }

    class BackendResourceManagerTest : public VulkanDeviceTest {
    protected:
        static BackendResourceManager& resources() { return backend().get_resource_manager(); }
        static BufferService* service() { return Registry::BackendRegistry::instance().get_service<BufferService>(); }

        static std::shared_ptr<VKBuffer> staging_buffer(size_t size = 256)
        {
            auto buffer = std::make_shared<VKBuffer>(size, VKBuffer::Usage::STAGING, Kakshya::DataModality::IMAGE_COLOR);
            resources().initialize_buffer(buffer);
            return buffer;
        }

        static std::shared_ptr<VKImage> make_image(
            uint32_t width, uint32_t height, vk::Format format,
            VKImage::Usage usage = VKImage::Usage::TEXTURE_2D)
        {
            auto image = std::make_shared<VKImage>(width, height, 1, format, usage);
            resources().initialize_image(image);
            return image;
        }
    };

    struct RoundTripCase {
        VKImage::Usage usage;
        vk::Format format;
    };

    class ImageRoundTripTest : public BackendResourceManagerTest,
                               public ::testing::WithParamInterface<RoundTripCase> { };

}

TEST_F(BackendResourceManagerTest, RawHostVisibleBufferIsPersistentlyMapped)
{
    RawVulkanBuffer raw(backend(), 128, k_transfer_usage, true);

    ASSERT_NE(raw.mapped(), nullptr);
    const auto bytes = pattern(128);
    std::memcpy(raw.mapped(), bytes.data(), bytes.size());
    EXPECT_EQ(std::memcmp(raw.mapped(), bytes.data(), bytes.size()), 0);
}

TEST_F(BackendResourceManagerTest, RawDeviceLocalBufferIsNotMapped)
{
    RawVulkanBuffer raw(backend(), 128, k_transfer_usage, false);

    EXPECT_TRUE(raw.buffer());
    EXPECT_EQ(raw.mapped(), nullptr);
}

TEST_F(BackendResourceManagerTest, InitializeBufferAllocatesAndMapsStagingBuffers)
{
    auto buffer = staging_buffer();

    EXPECT_TRUE(buffer->is_initialized());
    EXPECT_NE(buffer->get_mapped_ptr(), nullptr);
    EXPECT_TRUE(buffer->get_buffer());

    resources().cleanup_buffer(buffer);
}

TEST_F(BackendResourceManagerTest, InitializingAnInitializedBufferKeepsItsAllocation)
{
    auto buffer = staging_buffer();
    const auto handle = buffer->get_buffer();

    resources().initialize_buffer(buffer);

    EXPECT_EQ(buffer->get_buffer(), handle);
    resources().cleanup_buffer(buffer);
}

TEST_F(BackendResourceManagerTest, NullBuffersAreRejectedWithoutThrowing)
{
    EXPECT_NO_THROW(resources().initialize_buffer(nullptr));
    EXPECT_NO_THROW(resources().cleanup_buffer(nullptr));
}

TEST_F(BackendResourceManagerTest, CleaningUpAnUnmanagedBufferIsNoOp)
{
    auto buffer = std::make_shared<VKBuffer>(64, VKBuffer::Usage::STAGING, Kakshya::DataModality::IMAGE_COLOR);
    EXPECT_NO_THROW(resources().cleanup_buffer(buffer));
}

TEST_F(BackendResourceManagerTest, CleaningUpTwiceDoesNotFreeTwice)
{
    auto buffer = staging_buffer();
    resources().cleanup_buffer(buffer);
    EXPECT_NO_THROW(resources().cleanup_buffer(buffer));
}

TEST_F(BackendResourceManagerTest, RetiredBufferIsDetachedAtOnceAndCanBeInitializedAgain)
{
    auto buffer = staging_buffer();
    ASSERT_TRUE(buffer->is_initialized());

    resources().retire_buffer(buffer);

    EXPECT_FALSE(buffer->is_initialized());
    EXPECT_EQ(buffer->get_mapped_ptr(), nullptr);

    resources().initialize_buffer(buffer);
    EXPECT_TRUE(buffer->is_initialized());
    EXPECT_NE(buffer->get_mapped_ptr(), nullptr);

    resources().cleanup_buffer(buffer);
}

TEST_F(BackendResourceManagerTest, RetiringNullOrUnmanagedBuffersIsNoOp)
{
    auto buffer = std::make_shared<VKBuffer>(64, VKBuffer::Usage::STAGING, Kakshya::DataModality::IMAGE_COLOR);

    EXPECT_NO_THROW(resources().retire_buffer(nullptr));
    EXPECT_NO_THROW(resources().retire_buffer(buffer));
}

TEST_F(BackendResourceManagerTest, BufferServiceRetireDetachesTheBuffer)
{
    auto buffer = staging_buffer();
    ASSERT_NE(service(), nullptr);
    ASSERT_TRUE(service()->retire_buffer);

    service()->retire_buffer(buffer);

    EXPECT_FALSE(buffer->is_initialized());
}

TEST_F(BackendResourceManagerTest, ResizingAManagedBufferPreservesItsBytesAndKeepsItUsable)
{
    auto buffer = staging_buffer(64);
    const auto bytes = pattern(64);
    std::memcpy(buffer->get_mapped_ptr(), bytes.data(), bytes.size());

    buffer->resize(256, true);

    EXPECT_EQ(buffer->get_size_bytes(), 256U);
    ASSERT_NE(buffer->get_mapped_ptr(), nullptr);
    EXPECT_EQ(std::memcmp(buffer->get_mapped_ptr(), bytes.data(), bytes.size()), 0);

    buffer->resize(512, false);
    EXPECT_EQ(buffer->get_size_bytes(), 512U);
    EXPECT_TRUE(buffer->is_initialized());

    resources().cleanup_buffer(buffer);
}

TEST_F(BackendResourceManagerTest, DeviceAddressIsNonZeroForAddressableBuffers)
{
    auto buffer = std::make_shared<VKBuffer>(256, VKBuffer::Usage::STORAGE_BDA, Kakshya::DataModality::IMAGE_COLOR);
    resources().initialize_buffer(buffer);

    EXPECT_NE(resources().get_buffer_device_address(buffer), 0U);
    resources().cleanup_buffer(buffer);
}

TEST_F(BackendResourceManagerTest, DeviceAddressOfUninitializedBufferIsZero)
{
    auto buffer = std::make_shared<VKBuffer>(256, VKBuffer::Usage::STORAGE_BDA, Kakshya::DataModality::IMAGE_COLOR);
    EXPECT_EQ(resources().get_buffer_device_address(buffer), 0U);
    EXPECT_EQ(resources().get_buffer_device_address(nullptr), 0U);
}

TEST_F(BackendResourceManagerTest, FlushPendingOperationsConsumesDirtyRanges)
{
    auto buffer = staging_buffer();
    buffer->mark_dirty_range(0, 64);

    resources().flush_pending_buffer_operations();

    EXPECT_TRUE(buffer->get_and_clear_dirty_ranges().empty());
    resources().cleanup_buffer(buffer);
}

TEST_F(BackendResourceManagerTest, BufferServiceCopiesBytesBetweenBuffers)
{
    ASSERT_NE(service(), nullptr);
    RawVulkanBuffer src(backend(), 128, k_transfer_usage, true);
    RawVulkanBuffer dst(backend(), 128, k_transfer_usage, true);
    const auto bytes = pattern(128);
    std::memcpy(src.mapped(), bytes.data(), bytes.size());
    std::memset(dst.mapped(), 0, 128);

    service()->copy_buffer(as_handle(src.buffer()), as_handle(dst.buffer()), 128, 0, 0);

    EXPECT_EQ(std::memcmp(dst.mapped(), bytes.data(), bytes.size()), 0);
}

TEST_F(BackendResourceManagerTest, BufferServiceCopyHonoursOffsets)
{
    ASSERT_NE(service(), nullptr);
    RawVulkanBuffer src(backend(), 128, k_transfer_usage, true);
    RawVulkanBuffer dst(backend(), 128, k_transfer_usage, true);
    const auto bytes = pattern(128);
    std::memcpy(src.mapped(), bytes.data(), bytes.size());
    std::memset(dst.mapped(), 0, 128);

    service()->copy_buffer(as_handle(src.buffer()), as_handle(dst.buffer()), 16, 32, 64);

    EXPECT_EQ(std::memcmp(static_cast<uint8_t*>(dst.mapped()) + 64, bytes.data() + 32, 16), 0);
    EXPECT_EQ(static_cast<uint8_t*>(dst.mapped())[63], 0);
    EXPECT_EQ(static_cast<uint8_t*>(dst.mapped())[80], 0);
}

TEST_F(BackendResourceManagerTest, FencedCopyCompletesAfterWait)
{
    ASSERT_NE(service(), nullptr);
    RawVulkanBuffer src(backend(), 128, k_transfer_usage, true);
    RawVulkanBuffer dst(backend(), 128, k_transfer_usage, true);
    const auto bytes = pattern(128, 9);
    std::memcpy(src.mapped(), bytes.data(), bytes.size());
    std::memset(dst.mapped(), 0, 128);

    auto handle = service()->copy_buffer_fenced(as_handle(src.buffer()), as_handle(dst.buffer()), 128, 0, 0);
    ASSERT_NE(handle, nullptr);
    service()->wait_fenced(handle);
    service()->release_fenced(handle);

    EXPECT_EQ(std::memcmp(dst.mapped(), bytes.data(), bytes.size()), 0);
}

TEST_F(BackendResourceManagerTest, DeferredFillIsVisibleToALaterImmediateCopyWithoutAWindow)
{
    RawVulkanBuffer src(backend(), 128, k_transfer_usage, false);
    RawVulkanBuffer dst(backend(), 128, k_transfer_usage, true);
    std::memset(dst.mapped(), 0, 128);

    resources().record_deferred_commands([&src](vk::CommandBuffer cmd) {
        cmd.fillBuffer(src.buffer(), 0, 128, 0xA5A5A5A5U);
    });

    resources().execute_immediate_commands([&src, &dst](vk::CommandBuffer cmd) {
        const vk::BufferCopy region { 0, 0, 128 };
        cmd.copyBuffer(src.buffer(), dst.buffer(), 1, &region);

        const vk::MemoryBarrier2 to_host {
            vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
            vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostRead
        };
        vk::DependencyInfo dependency {};
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &to_host;
        cmd.pipelineBarrier2(dependency);
    });

    EXPECT_FALSE(backend().get_command_manager().has_deferred_commands());
    const std::vector<uint8_t> expected(128, 0xA5);
    EXPECT_EQ(std::memcmp(dst.mapped(), expected.data(), expected.size()), 0);
}

TEST_F(BackendResourceManagerTest, BackendFlushSubmitsDeferredCommandsWithoutAWindow)
{
    auto& commands = backend().get_command_manager();
    const auto graphics = commands.timeline_index(backend().get_context().get_graphics_queue());

    resources().record_deferred_commands([](vk::CommandBuffer) { });
    ASSERT_TRUE(commands.has_deferred_commands());
    const auto before = commands.submitted_value(graphics);

    backend().flush_deferred();

    EXPECT_FALSE(commands.has_deferred_commands());
    EXPECT_EQ(commands.submitted_value(graphics), before + 1);
    commands.wait(commands.submitted());
}

TEST_F(BackendResourceManagerTest, BufferServiceFlushSubmitsDeferredCommandsOnce)
{
    ASSERT_NE(service(), nullptr);
    ASSERT_TRUE(service()->flush_deferred);

    int calls = 0;
    service()->record_deferred([&calls](void*) { ++calls; });
    EXPECT_EQ(calls, 1);

    const auto [timeline, value] = service()->flush_deferred();
    EXPECT_NE(value, 0U);
    EXPECT_FALSE(backend().get_command_manager().has_deferred_commands());
    service()->await_point(timeline, value);

    EXPECT_EQ(service()->flush_deferred().second, 0U);
}

TEST_F(BackendResourceManagerTest, ExecuteFencedRunsTheRecorderOnce)
{
    ASSERT_NE(service(), nullptr);
    int calls = 0;

    auto handle = service()->execute_fenced([&calls](void* cmd) {
        EXPECT_NE(cmd, nullptr);
        ++calls;
    });

    ASSERT_NE(handle, nullptr);
    service()->wait_fenced(handle);
    service()->release_fenced(handle);
    EXPECT_EQ(calls, 1);
}

TEST_F(BackendResourceManagerTest, FencedHelpersIgnoreNullHandles)
{
    ASSERT_NE(service(), nullptr);
    EXPECT_NO_THROW(service()->wait_fenced(nullptr));
    EXPECT_NO_THROW(service()->release_fenced(nullptr));
}

TEST_F(BackendResourceManagerTest, ReleasingAFencedHandleTwiceIsSafe)
{
    ASSERT_NE(service(), nullptr);
    auto handle = service()->execute_fenced([](void*) { });
    ASSERT_NE(handle, nullptr);
    service()->wait_fenced(handle);
    service()->release_fenced(handle);

    EXPECT_NO_THROW(service()->release_fenced(handle));
}

TEST_F(BackendResourceManagerTest, ExecuteImmediateRunsTheRecorderOnce)
{
    ASSERT_NE(service(), nullptr);
    int calls = 0;
    service()->execute_immediate([&calls](void* cmd) {
        EXPECT_NE(cmd, nullptr);
        ++calls;
    });
    EXPECT_EQ(calls, 1);
}

TEST_F(BackendResourceManagerTest, FlushAndInvalidateAcceptWholeRangeOnCoherentMemory)
{
    ASSERT_NE(service(), nullptr);
    RawVulkanBuffer raw(backend(), 128, k_transfer_usage, true);
    void* memory = reinterpret_cast<void*>(static_cast<VkDeviceMemory>(raw.memory()));

    EXPECT_NO_THROW(service()->flush_range(memory, 0, 0));
    EXPECT_NO_THROW(service()->invalidate_range(memory, 0, 0));
    EXPECT_NO_THROW(service()->flush_range(memory, 0, 64));
}

TEST_F(BackendResourceManagerTest, DeferredCommandsFlushOnceThenReportNothingPending)
{
    int calls = 0;
    resources().record_deferred_commands([&calls](vk::CommandBuffer) { ++calls; });
    EXPECT_EQ(calls, 1);

    const auto semaphore = resources().flush_deferred_commands();
    EXPECT_TRUE(semaphore);
    backend().wait_idle();

    EXPECT_FALSE(resources().flush_deferred_commands());
}

TEST_F(BackendResourceManagerTest, FlushingWithoutDeferredCommandsReturnsNoSemaphore)
{
    EXPECT_FALSE(resources().flush_deferred_commands());
}

TEST_F(BackendResourceManagerTest, ExecuteImmediateCommandsRunsTheRecorder)
{
    int calls = 0;
    resources().execute_immediate_commands([&calls](vk::CommandBuffer cmd) {
        EXPECT_TRUE(cmd);
        ++calls;
    });
    EXPECT_EQ(calls, 1);
}

TEST_F(BackendResourceManagerTest, InitializeImageCreatesImageViewAndMemory)
{
    auto image = make_image(4, 4, vk::Format::eR8G8B8A8Unorm);

    EXPECT_TRUE(image->is_initialized());
    EXPECT_TRUE(image->get_image());
    EXPECT_TRUE(image->get_image_view());
    EXPECT_TRUE(image->get_memory());

    resources().cleanup_image(image);
}

TEST_F(BackendResourceManagerTest, InitializingAnInitializedImageKeepsItsHandle)
{
    auto image = make_image(4, 4, vk::Format::eR8G8B8A8Unorm);
    const auto handle = image->get_image();

    resources().initialize_image(image);

    EXPECT_EQ(image->get_image(), handle);
    resources().cleanup_image(image);
}

TEST_F(BackendResourceManagerTest, CleanupImageReleasesHandlesAndIsRepeatable)
{
    auto image = make_image(4, 4, vk::Format::eR8G8B8A8Unorm);

    resources().cleanup_image(image);

    EXPECT_FALSE(image->is_initialized());
    EXPECT_NO_THROW(resources().cleanup_image(image));
    EXPECT_NO_THROW(resources().cleanup_image(nullptr));
}

TEST_F(BackendResourceManagerTest, MipChainAndArrayImagesInitialize)
{
    auto image = std::make_shared<VKImage>(8, 8, 1, vk::Format::eR8G8B8A8Unorm, VKImage::Usage::TEXTURE_2D,
        VKImage::Type::TYPE_2D, 4, 3);
    resources().initialize_image(image);

    EXPECT_TRUE(image->is_initialized());
    resources().cleanup_image(image);
}

TEST_F(BackendResourceManagerTest, VolumeImagesInitialize)
{
    auto image = std::make_shared<VKImage>(4, 4, 4, vk::Format::eR8G8B8A8Unorm, VKImage::Usage::STORAGE,
        VKImage::Type::TYPE_3D);
    resources().initialize_image(image);

    EXPECT_TRUE(image->is_initialized());
    resources().cleanup_image(image);
}

TEST_P(ImageRoundTripTest, UploadedBytesReadBackUnchanged)
{
    const auto [usage, format] = GetParam();
    constexpr uint32_t k_extent = 4;
    const size_t size = static_cast<size_t>(k_extent) * k_extent * Core::vk_format_bytes_per_pixel(format);
    auto image = make_image(k_extent, k_extent, format, usage);
    const auto bytes = pattern(size, 3);

    resources().upload_image_data(image, bytes.data(), bytes.size());
    std::vector<uint8_t> out(size, 0);
    resources().download_image_data(image, out.data(), out.size());

    EXPECT_EQ(out, bytes);
    resources().cleanup_image(image);
}

INSTANTIATE_TEST_SUITE_P(UsagesAndFormats, ImageRoundTripTest,
    ::testing::Values(
        RoundTripCase { VKImage::Usage::TEXTURE_2D, vk::Format::eR8G8B8A8Unorm },
        RoundTripCase { VKImage::Usage::STORAGE, vk::Format::eR8G8B8A8Unorm },
        RoundTripCase { VKImage::Usage::RENDER_TARGET, vk::Format::eR8G8B8A8Unorm },
        RoundTripCase { VKImage::Usage::TEXTURE_2D, vk::Format::eR8Unorm },
        RoundTripCase { VKImage::Usage::TEXTURE_2D, vk::Format::eR32G32B32A32Sfloat }));

TEST_F(BackendResourceManagerTest, UploadWithNullArgumentsIsRejected)
{
    auto image = make_image(4, 4, vk::Format::eR8G8B8A8Unorm);
    const auto bytes = pattern(64);

    EXPECT_NO_THROW(resources().upload_image_data(nullptr, bytes.data(), bytes.size()));
    EXPECT_NO_THROW(resources().upload_image_data(image, nullptr, bytes.size()));
    resources().cleanup_image(image);
}

TEST_F(BackendResourceManagerTest, DeferredUploadThroughProvidedStagingBufferRoundTrips)
{
    auto image = make_image(4, 4, vk::Format::eR8G8B8A8Unorm);
    auto staging = staging_buffer(64);
    const auto bytes = pattern(64, 5);

    resources().upload_image_data(image, bytes.data(), bytes.size(), staging, true);
    std::vector<uint8_t> out(64, 0);
    resources().download_image_data(image, out.data(), out.size());

    EXPECT_EQ(out, bytes);
    resources().cleanup_buffer(staging);
    resources().cleanup_image(image);
}

TEST_F(BackendResourceManagerTest, LayerUploadReturnsTrueForAnArrayImage)
{
    auto image = std::make_shared<VKImage>(4, 4, 1, vk::Format::eR8G8B8A8Unorm, VKImage::Usage::TEXTURE_2D,
        VKImage::Type::TYPE_2D, 1, 2);
    resources().initialize_image(image);
    const auto bytes = pattern(64);

    EXPECT_TRUE(resources().upload_image_layer(image, 1, bytes.data(), bytes.size()));
    resources().cleanup_image(image);
}

TEST_F(BackendResourceManagerTest, LayerUploadPreservesOtherLayersAfterWholeArrayUpload)
{
    auto image = std::make_shared<VKImage>(2, 2, 1, vk::Format::eR8G8B8A8Unorm, VKImage::Usage::TEXTURE_2D,
        VKImage::Type::TYPE_2D, 1, 3);
    resources().initialize_image(image);

    auto expected = pattern(48, 7);
    resources().upload_image_data(image, expected.data(), expected.size());

    const auto replacement = pattern(16, 101);
    ASSERT_TRUE(resources().upload_image_layer(image, 1, replacement.data(), replacement.size()));
    std::memcpy(expected.data() + 16, replacement.data(), replacement.size());

    std::vector<uint8_t> out(expected.size(), 0);
    resources().download_image_data(image, out.data(), out.size());

    EXPECT_EQ(out, expected);
    resources().cleanup_image(image);
}

TEST_F(BackendResourceManagerTest, HostReadbackRestoresGeneralLayoutForAnotherUpload)
{
    auto image = make_image(2, 2, vk::Format::eR8G8B8A8Unorm, VKImage::Usage::STORAGE);
    const auto first = pattern(16, 11);
    const auto second = pattern(16, 51);

    resources().upload_image_data(image, first.data(), first.size());

    std::vector<uint8_t> out(first.size(), 0);
    resources().download_image_data(
        image, out.data(), out.size(),
        vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader);

    EXPECT_EQ(out, first);
    EXPECT_EQ(image->get_current_layout(), vk::ImageLayout::eGeneral);

    resources().upload_image_data(image, second.data(), second.size());
    resources().download_image_data(image, out.data(), out.size());

    EXPECT_EQ(out, second);
    resources().cleanup_image(image);
}

TEST_F(BackendResourceManagerTest, HostImageTransfersDoNotWaitForUnrelatedBlockedWork)
{
    auto& context = backend().get_context();
    if (context.get_compute_queue() == context.get_graphics_queue()) {
        GTEST_SKIP() << "The device has no separate compute queue to hold unrelated work on";
    }

    auto image = make_image(2, 2, vk::Format::eR8G8B8A8Unorm, VKImage::Usage::STORAGE);
    const auto bytes = pattern(16, 3);

    TimelineGate gate(context.get_device());
    const std::array<vk::SemaphoreSubmitInfo, 1> waits { gate.closed() };
    const auto blocked = backend().get_command_manager().submit(context.get_compute_queue(), {}, waits);
    ASSERT_TRUE(blocked);

    auto transfer = std::async(std::launch::async, [&] {
        resources().upload_image_data(image, bytes.data(), bytes.size());
        std::vector<uint8_t> out(bytes.size(), 0);
        resources().download_image_data(image, out.data(), out.size());
        return out;
    });

    const bool finished = transfer.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    gate.open();

    ASSERT_TRUE(finished);
    EXPECT_EQ(transfer.get(), bytes);
    resources().cleanup_image(image);
}

TEST_F(BackendResourceManagerTest, TransitionsBetweenCommonLayoutsInEveryModeTrackTheLayout)
{
    auto image = make_image(4, 4, vk::Format::eR8G8B8A8Unorm, VKImage::Usage::STORAGE);
    const std::array steps {
        std::pair { vk::ImageLayout::eTransferDstOptimal, Core::SubmitMode::DEFERRED },
        std::pair { vk::ImageLayout::eShaderReadOnlyOptimal, Core::SubmitMode::ASYNC },
        std::pair { vk::ImageLayout::eGeneral, Core::SubmitMode::IMMEDIATE },
        std::pair { vk::ImageLayout::eShaderReadOnlyOptimal, Core::SubmitMode::DEFERRED },
        std::pair { vk::ImageLayout::eTransferSrcOptimal, Core::SubmitMode::ASYNC },
    };

    for (const auto& [layout, mode] : steps) {
        EXPECT_NO_THROW(resources().transition_image_layout(image, layout, mode));
        EXPECT_EQ(image->get_current_layout(), layout);
    }

    auto& commands = backend().get_command_manager();
    resources().flush_deferred_commands();
    commands.wait(commands.submitted());
    resources().cleanup_image(image);
}

TEST_F(BackendResourceManagerTest, TransitionToTheTrackedLayoutRecordsNothing)
{
    auto image = make_image(4, 4, vk::Format::eR8G8B8A8Unorm, VKImage::Usage::STORAGE);
    resources().transition_image_layout(image, vk::ImageLayout::eGeneral, Core::SubmitMode::IMMEDIATE);

    auto& commands = backend().get_command_manager();
    const auto graphics = commands.timeline_index(backend().get_context().get_graphics_queue());
    const auto before = commands.submitted_value(graphics);

    resources().transition_image_layout(image, vk::ImageLayout::eGeneral);

    EXPECT_FALSE(commands.has_deferred_commands());
    EXPECT_EQ(commands.submitted_value(graphics), before);
    resources().cleanup_image(image);
}

TEST_F(BackendResourceManagerTest, DeferredTransitionOfAnUploadedImageIsSeenByALaterDownload)
{
    auto image = make_image(4, 4, vk::Format::eR8G8B8A8Unorm);
    const auto bytes = pattern(64, 3);
    resources().upload_image_data(image, bytes.data(), bytes.size());

    resources().transition_image_layout(image, vk::ImageLayout::eGeneral);
    EXPECT_EQ(image->get_current_layout(), vk::ImageLayout::eGeneral);

    std::vector<uint8_t> out(64, 0);
    resources().download_image_data(image, out.data(), out.size());

    EXPECT_EQ(out, bytes);
    EXPECT_FALSE(backend().get_command_manager().has_deferred_commands());
    resources().cleanup_image(image);
}

TEST_F(BackendResourceManagerTest, ExecuteAsyncSubmitsAndItsPointCompletes)
{
    RawVulkanBuffer buffer(backend(), 64, vk::BufferUsageFlagBits::eTransferDst, true);

    const auto point = resources().execute_async([&](vk::CommandBuffer cmd) {
        cmd.fillBuffer(buffer.buffer(), 0, 64, 0xABABABABU);
    });

    ASSERT_TRUE(point);
    backend().get_command_manager().wait(point);
    const auto* bytes = static_cast<const uint8_t*>(buffer.mapped());
    for (size_t i = 0; i < 64; ++i) {
        EXPECT_EQ(bytes[i], 0xAB) << i;
    }
}

TEST_F(BackendResourceManagerTest, ExecuteAsyncReleasesWhatItKeepsAliveOnceItCompletes)
{
    auto token = std::make_shared<int>(1);
    const std::weak_ptr<int> watcher = token;

    const auto point = resources().execute_async([](vk::CommandBuffer) { }, { token });
    token.reset();

    ASSERT_TRUE(point);
    backend().get_command_manager().wait(point);
    backend().get_command_manager().collect();
    EXPECT_TRUE(watcher.expired());
}

TEST_F(BackendResourceManagerTest, ConsecutiveAsyncSubmissionsOrderOnTheQueue)
{
    RawVulkanBuffer buffer(backend(), 64, vk::BufferUsageFlagBits::eTransferDst, true);

    const auto first = resources().execute_async([&](vk::CommandBuffer cmd) {
        cmd.fillBuffer(buffer.buffer(), 0, 64, 0x11111111U);
    });
    const auto second = resources().execute_async([&](vk::CommandBuffer cmd) {
        cmd.fillBuffer(buffer.buffer(), 0, 64, 0x22222222U);
    });

    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_GT(second.value, first.value);
    backend().get_command_manager().wait(second);
    EXPECT_EQ(static_cast<const uint8_t*>(buffer.mapped())[0], 0x22);
}

TEST_F(BackendResourceManagerTest, AsyncBlitCopiesPixelsAndAHostReadAfterwardsSeesThem)
{
    auto src = make_image(4, 4, vk::Format::eR8G8B8A8Unorm);
    auto dst = make_image(4, 4, vk::Format::eR8G8B8A8Unorm);
    const auto bytes = pattern(64, 9);
    resources().upload_image_data(src, bytes.data(), bytes.size());

    vk::ImageBlit region {};
    region.srcSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 };
    region.srcOffsets = std::array<vk::Offset3D, 2> { vk::Offset3D { 0, 0, 0 }, vk::Offset3D { 4, 4, 1 } };
    region.dstSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 };
    region.dstOffsets = std::array<vk::Offset3D, 2> { vk::Offset3D { 0, 0, 0 }, vk::Offset3D { 4, 4, 1 } };

    ASSERT_TRUE(resources().blit_image(src, dst, region, vk::Filter::eNearest, Core::SubmitMode::ASYNC));
    std::vector<uint8_t> out(64, 0);
    resources().download_image_data(dst, out.data(), out.size());

    EXPECT_EQ(out, bytes);
    resources().cleanup_image(src);
    resources().cleanup_image(dst);
}

TEST_F(BackendResourceManagerTest, TheServiceHookSubmitsWithoutAHandleAndItsPointCompletes)
{
    RawVulkanBuffer buffer(backend(), 64, vk::BufferUsageFlagBits::eTransferDst, true);

    const auto [timeline, value] = service()->execute_async(
        [&](void* handle) {
            vk::CommandBuffer cmd(static_cast<VkCommandBuffer>(handle));
            cmd.fillBuffer(buffer.buffer(), 0, 64, 0xCDCDCDCDU);
        },
        {});

    ASSERT_NE(value, 0U);
    service()->await_point(timeline, value);
    EXPECT_EQ(static_cast<const uint8_t*>(buffer.mapped())[0], 0xCD);
}

TEST_F(BackendResourceManagerTest, DeferredBlitIsSeenByALaterAsyncBlitOfItsDestination)
{
    auto first = make_image(4, 4, vk::Format::eR8G8B8A8Unorm);
    auto middle = make_image(4, 4, vk::Format::eR8G8B8A8Unorm);
    auto last = make_image(4, 4, vk::Format::eR8G8B8A8Unorm);
    const auto bytes = pattern(64, 11);
    resources().upload_image_data(first, bytes.data(), bytes.size());

    vk::ImageBlit region {};
    region.srcSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 };
    region.srcOffsets = std::array<vk::Offset3D, 2> { vk::Offset3D { 0, 0, 0 }, vk::Offset3D { 4, 4, 1 } };
    region.dstSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 };
    region.dstOffsets = std::array<vk::Offset3D, 2> { vk::Offset3D { 0, 0, 0 }, vk::Offset3D { 4, 4, 1 } };

    ASSERT_TRUE(resources().blit_image(first, middle, region, vk::Filter::eNearest, Core::SubmitMode::DEFERRED));
    ASSERT_TRUE(backend().get_command_manager().has_deferred_commands());
    ASSERT_TRUE(resources().blit_image(middle, last, region, vk::Filter::eNearest, Core::SubmitMode::ASYNC));
    EXPECT_FALSE(backend().get_command_manager().has_deferred_commands());

    std::vector<uint8_t> out(64, 0);
    resources().download_image_data(last, out.data(), out.size());

    EXPECT_EQ(out, bytes);
    resources().cleanup_image(first);
    resources().cleanup_image(middle);
    resources().cleanup_image(last);
}

TEST_F(BackendResourceManagerTest, DeferredBlitHoldsItsImagesUntilTheFlushCompletes)
{
    auto src = make_image(4, 4, vk::Format::eR8G8B8A8Unorm);
    auto dst = make_image(4, 4, vk::Format::eR8G8B8A8Unorm);
    const auto src_count = src.use_count();
    const auto dst_count = dst.use_count();

    vk::ImageBlit region {};
    region.srcSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 };
    region.srcOffsets = std::array<vk::Offset3D, 2> { vk::Offset3D { 0, 0, 0 }, vk::Offset3D { 4, 4, 1 } };
    region.dstSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 };
    region.dstOffsets = std::array<vk::Offset3D, 2> { vk::Offset3D { 0, 0, 0 }, vk::Offset3D { 4, 4, 1 } };

    ASSERT_TRUE(resources().blit_image(src, dst, region, vk::Filter::eNearest, Core::SubmitMode::DEFERRED));
    EXPECT_EQ(src.use_count(), src_count + 1);
    EXPECT_EQ(dst.use_count(), dst_count + 1);

    auto& commands = backend().get_command_manager();
    const auto point = resources().flush_deferred_commands();
    ASSERT_TRUE(point);
    commands.wait(point);
    commands.collect();

    EXPECT_EQ(src.use_count(), src_count);
    EXPECT_EQ(dst.use_count(), dst_count);
    resources().cleanup_image(src);
    resources().cleanup_image(dst);
}

TEST_F(BackendResourceManagerTest, DeferredUploadThroughStagingIsSeenByALaterDownloadAndReleasesTheStaging)
{
    auto image = make_image(4, 4, vk::Format::eR8G8B8A8Unorm);
    auto staging = staging_buffer();
    const auto staging_count = staging.use_count();
    const auto bytes = pattern(64, 5);

    resources().upload_image_data(image, bytes.data(), bytes.size(), staging, true);
    ASSERT_TRUE(backend().get_command_manager().has_deferred_commands());
    EXPECT_EQ(staging.use_count(), staging_count + 1);

    std::vector<uint8_t> out(64, 0);
    resources().download_image_data(image, out.data(), out.size());
    EXPECT_EQ(out, bytes);
    EXPECT_FALSE(backend().get_command_manager().has_deferred_commands());

    auto& commands = backend().get_command_manager();
    commands.wait(commands.submitted());
    commands.collect();
    EXPECT_EQ(staging.use_count(), staging_count);

    resources().cleanup_image(image);
    resources().cleanup_buffer(staging);
}

TEST_F(BackendResourceManagerTest, SameFormatBlitIsSupportedAndUndefinedIsNot)
{
    EXPECT_TRUE(resources().supports_blit(vk::Format::eR8G8B8A8Unorm, vk::Format::eR8G8B8A8Unorm));
    EXPECT_FALSE(resources().supports_blit(vk::Format::eUndefined, vk::Format::eUndefined));
}

TEST_F(BackendResourceManagerTest, FullExtentBlitCopiesPixels)
{
    auto src = make_image(4, 4, vk::Format::eR8G8B8A8Unorm);
    auto dst = make_image(4, 4, vk::Format::eR8G8B8A8Unorm);
    const auto bytes = pattern(64, 7);
    resources().upload_image_data(src, bytes.data(), bytes.size());

    vk::ImageBlit region {};
    region.srcSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 };
    region.srcOffsets = std::array<vk::Offset3D, 2> { vk::Offset3D { 0, 0, 0 }, vk::Offset3D { 4, 4, 1 } };
    region.dstSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 };
    region.dstOffsets = std::array<vk::Offset3D, 2> { vk::Offset3D { 0, 0, 0 }, vk::Offset3D { 4, 4, 1 } };

    ASSERT_TRUE(resources().blit_image(src, dst, region, vk::Filter::eNearest));
    std::vector<uint8_t> out(64, 0);
    resources().download_image_data(dst, out.data(), out.size());

    EXPECT_EQ(out, bytes);
    resources().cleanup_image(src);
    resources().cleanup_image(dst);
}

TEST_F(BackendResourceManagerTest, SamplersWithTheSameParametersAreShared)
{
    const auto a = resources().create_sampler(vk::Filter::eLinear, vk::SamplerAddressMode::eRepeat, 0.0F);
    const auto b = resources().create_sampler(vk::Filter::eLinear, vk::SamplerAddressMode::eRepeat, 0.0F);

    ASSERT_TRUE(a);
    EXPECT_EQ(a, b);
}

TEST_F(BackendResourceManagerTest, SamplersDifferByFilterAddressModeAndAnisotropy)
{
    const auto base = resources().create_sampler(vk::Filter::eLinear, vk::SamplerAddressMode::eRepeat, 0.0F);
    const auto nearest = resources().create_sampler(vk::Filter::eNearest, vk::SamplerAddressMode::eRepeat, 0.0F);
    const auto clamped = resources().create_sampler(vk::Filter::eLinear, vk::SamplerAddressMode::eClampToEdge, 0.0F);
    const auto aniso = resources().create_sampler(vk::Filter::eLinear, vk::SamplerAddressMode::eRepeat, 4.0F);

    EXPECT_NE(base, nearest);
    EXPECT_NE(base, clamped);
    EXPECT_NE(base, aniso);
}

TEST_F(BackendResourceManagerTest, DestroyedSamplerIsRecreatedOnNextRequest)
{
    const auto first = resources().create_sampler(vk::Filter::eNearest, vk::SamplerAddressMode::eMirroredRepeat, 0.0F);
    resources().destroy_sampler(first);

    const auto second = resources().create_sampler(vk::Filter::eNearest, vk::SamplerAddressMode::eMirroredRepeat, 0.0F);

    EXPECT_TRUE(second);
    resources().destroy_sampler(second);
}

TEST_F(BackendResourceManagerTest, DestroyingNullSamplerIsNoOp)
{
    EXPECT_NO_THROW(resources().destroy_sampler(vk::Sampler {}));
}

TEST_F(BackendResourceManagerTest, FindMemoryTypeHonoursRequestedProperties)
{
    const auto index = resources().find_memory_type(0xFFFFFFFFU,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

    const auto properties = backend().get_context().get_physical_device().getMemoryProperties();
    ASSERT_LT(index, properties.memoryTypeCount);
    EXPECT_TRUE(properties.memoryTypes[index].propertyFlags & vk::MemoryPropertyFlagBits::eHostVisible);
}

TEST_F(BackendResourceManagerTest, FindMemoryTypeThrowsWhenNothingMatches)
{
    EXPECT_THROW(resources().find_memory_type(0U, vk::MemoryPropertyFlagBits::eHostVisible), std::runtime_error);
}

TEST_F(BackendResourceManagerTest, StandaloneManagerCleanupReleasesItsBuffersAndSamplers)
{
    BackendResourceManager manager(backend().get_context(), backend().get_command_manager());
    auto buffer = std::make_shared<VKBuffer>(64, VKBuffer::Usage::STAGING, Kakshya::DataModality::IMAGE_COLOR);
    manager.initialize_buffer(buffer);
    ASSERT_TRUE(manager.create_sampler(vk::Filter::eLinear, vk::SamplerAddressMode::eClampToBorder, 0.0F));

    EXPECT_NO_THROW(manager.cleanup());
    EXPECT_NO_THROW(manager.cleanup());
}

TEST_F(BackendResourceManagerTest, CleaningUpAVertexBufferLeavesItsLinkedIndexBufferToItsOwner)
{
    auto vertex = staging_buffer(256);
    auto index = std::make_shared<VKBuffer>(144, VKBuffer::Usage::INDEX, Kakshya::DataModality::UNKNOWN);
    resources().initialize_buffer(index);
    vertex->set_index_resources(index->get_buffer(), index->get_buffer_resources().memory, index->get_size_bytes());

    resources().cleanup_buffer(vertex);

    EXPECT_TRUE(index->is_initialized());
    EXPECT_NE(index->get_buffer_resources().buffer, vk::Buffer {});
    resources().cleanup_buffer(index);
    EXPECT_FALSE(index->is_initialized());
}

TEST_F(BackendResourceManagerTest, ManagerCleanupReleasesRawSlotsButBufferCleanupKeepsThem)
{
    BackendResourceManager manager(backend().get_context(), backend().get_command_manager());
    auto owner = std::make_shared<VKBuffer>(64, VKBuffer::Usage::STAGING, Kakshya::DataModality::IMAGE_COLOR);
    manager.initialize_buffer(owner);

    for (int i = 0; i < 2; ++i) {
        Buffers::VKBufferResources::GenerationSlot slot;
        vk::Buffer raw_buffer;
        vk::DeviceMemory raw_memory;
        void* mapped = nullptr;
        manager.allocate_raw_buffer(
            128, vk::BufferUsageFlagBits::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
            true, raw_buffer, raw_memory, mapped);
        slot.buffer = raw_buffer;
        slot.memory = raw_memory;
        slot.mapped_ptr = mapped;
        owner->get_buffer_resources().back_buffers.push_back(slot);
    }

    manager.cleanup_buffer(owner);
    EXPECT_EQ(owner->get_buffer_resources().back_buffers.size(), 2U);
    for (const auto& slot : owner->get_buffer_resources().back_buffers) {
        EXPECT_NE(slot.buffer, vk::Buffer {});
    }

    manager.initialize_buffer(owner);
    manager.cleanup();
    EXPECT_TRUE(owner->get_buffer_resources().back_buffers.empty());
}

TEST_F(BackendResourceManagerTest, StandaloneManagerCleanupReleasesEveryManagedBuffer)
{
    BackendResourceManager manager(backend().get_context(), backend().get_command_manager());
    std::vector<std::shared_ptr<VKBuffer>> buffers;
    for (int i = 0; i < 5; ++i) {
        auto buffer = std::make_shared<VKBuffer>(64, VKBuffer::Usage::STAGING, Kakshya::DataModality::IMAGE_COLOR);
        manager.initialize_buffer(buffer);
        ASSERT_TRUE(buffer->is_initialized());
        buffers.push_back(buffer);
    }

    manager.cleanup();

    for (const auto& buffer : buffers) {
        EXPECT_FALSE(buffer->is_initialized());
        EXPECT_EQ(buffer->get_mapped_ptr(), nullptr);
    }
    EXPECT_NO_THROW(manager.cleanup());
}

}
