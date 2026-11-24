#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "../support/log_capture.hpp"
#include "../support/vulkan_test_support.hpp"

#include "MayaFlux/Buffers/VKBuffer.hpp"
#include "MayaFlux/Core/Backends/Graphics/Vulkan/ImageAccess.hpp"
#include "MayaFlux/Core/Backends/Graphics/Vulkan/VKCommandManager.hpp"
#include "MayaFlux/Core/Backends/Graphics/Vulkan/VKImage.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))

#define IMAGE_TRANSITION_VALIDATED (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan, ::MayaFlux::Test::Meta::Capability::vulkan_validation>() })

namespace MayaFlux::Test {
namespace {

    using Buffers::VKBuffer;
    using Core::BackendResourceManager;
    using Core::ImageAccess;
    using Core::ImageRange;
    using Core::ImageViewKey;
    using Core::SubmitMode;
    using Core::VKImage;

    constexpr auto k_rgba = vk::Format::eR8G8B8A8Unorm;

    std::vector<uint8_t> pattern(size_t size, uint8_t seed = 1)
    {
        std::vector<uint8_t> bytes(size);
        std::iota(bytes.begin(), bytes.end(), seed);
        return bytes;
    }

    class ImageTransitionFixture : public VulkanDeviceTest {
    protected:
        void SetUp() override
        {
            VulkanDeviceTest::SetUp();
            if (IsSkipped()) {
                return;
            }
            LogCapture::install();
            LogCapture::clear();
        }

        static BackendResourceManager& resources() { return backend().get_resource_manager(); }

        static std::shared_ptr<VKImage> make_image(
            uint32_t width, uint32_t height, vk::Format format,
            VKImage::Usage usage = VKImage::Usage::TEXTURE_2D,
            uint32_t mips = 1, uint32_t layers = 1)
        {
            auto image = std::make_shared<VKImage>(width, height, 1, format, usage,
                VKImage::Type::TYPE_2D, mips, layers);
            resources().initialize_image(image);
            return image;
        }

        static std::shared_ptr<VKBuffer> staging_buffer(size_t size)
        {
            auto buffer = std::make_shared<VKBuffer>(size, VKBuffer::Usage::STAGING, Kakshya::DataModality::IMAGE_COLOR);
            resources().initialize_buffer(buffer);
            return buffer;
        }

        static void settle(const std::shared_ptr<VKImage>& image, vk::ImageLayout layout)
        {
            if (layout != vk::ImageLayout::eUndefined) {
                resources().transition_image_layout(image, layout, SubmitMode::IMMEDIATE);
            }
        }

        static void idle()
        {
            auto& commands = backend().get_command_manager();
            resources().flush_deferred_commands();
            commands.wait(commands.submitted());
        }

        static vk::ImageBlit whole_blit(uint32_t extent)
        {
            vk::ImageBlit region {};
            region.srcSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 };
            region.srcOffsets = std::array<vk::Offset3D, 2> { vk::Offset3D { 0, 0, 0 }, vk::Offset3D { static_cast<int32_t>(extent), static_cast<int32_t>(extent), 1 } };
            region.dstSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 };
            region.dstOffsets = std::array<vk::Offset3D, 2> { vk::Offset3D { 0, 0, 0 }, vk::Offset3D { static_cast<int32_t>(extent), static_cast<int32_t>(extent), 1 } };
            return region;
        }
    };

    class ImageLayoutRegressionTest : public ImageTransitionFixture { };
    class ImageRangeTransitionTest : public ImageTransitionFixture { };
    class ImageViewTest : public ImageTransitionFixture { };

    class ImageMipChainTest : public ImageTransitionFixture {
    protected:
        static std::vector<uint8_t> solid(uint32_t width, uint32_t height, std::array<uint8_t, 4> color)
        {
            std::vector<uint8_t> bytes;
            bytes.reserve(static_cast<size_t>(width) * height * 4);
            for (uint32_t i = 0; i < width * height; ++i) {
                bytes.insert(bytes.end(), color.begin(), color.end());
            }
            return bytes;
        }

        static std::shared_ptr<VKImage> solid_image(
            uint32_t width, uint32_t height, uint32_t mips, std::array<uint8_t, 4> color)
        {
            auto image = make_image(width, height, k_rgba, VKImage::Usage::TEXTURE_2D, mips);
            const auto bytes = solid(width, height, color);
            resources().upload_image_data(image, bytes.data(), bytes.size());
            resources().transition_image_layout(image, vk::ImageLayout::eShaderReadOnlyOptimal, SubmitMode::IMMEDIATE);
            return image;
        }

        static std::vector<uint8_t> read_level(const std::shared_ptr<VKImage>& image, uint32_t level, uint32_t layer = 0)
        {
            const uint32_t width = std::max(1U, image->get_width() >> level);
            const uint32_t height = std::max(1U, image->get_height() >> level);
            const size_t bytes = static_cast<size_t>(width) * height * 4;
            RawVulkanBuffer raw(backend(), bytes, vk::BufferUsageFlagBits::eTransferDst, true);

            resources().execute_immediate_commands([&](vk::CommandBuffer cmd) {
                resources().record_image_transition(cmd, image, ImageAccess::SHADER_READ, ImageAccess::TRANSFER_SRC);
                vk::BufferImageCopy region {};
                region.imageSubresource = { vk::ImageAspectFlagBits::eColor, level, layer, 1 };
                region.imageExtent = vk::Extent3D { width, height, 1 };
                cmd.copyImageToBuffer(image->get_image(), vk::ImageLayout::eTransferSrcOptimal,
                    raw.buffer(), 1, &region);
                resources().record_image_transition(cmd, image, ImageAccess::TRANSFER_SRC, ImageAccess::SHADER_READ);
            });

            std::vector<uint8_t> out(bytes);
            std::memcpy(out.data(), raw.mapped(), bytes);
            return out;
        }

        static void expect_every_level_solid(
            const std::shared_ptr<VKImage>& image, std::array<uint8_t, 4> color, uint32_t layer = 0)
        {
            for (uint32_t level = 0; level < image->get_mip_levels(); ++level) {
                const uint32_t width = std::max(1U, image->get_width() >> level);
                const uint32_t height = std::max(1U, image->get_height() >> level);
                EXPECT_EQ(read_level(image, level, layer), solid(width, height, color)) << "level " << level << " layer " << layer;
            }
        }
    };

    vk::ImageLayout transient_to_settled(vk::ImageLayout layout)
    {
        return layout == vk::ImageLayout::eTransferSrcOptimal || layout == vk::ImageLayout::eTransferDstOptimal
            ? vk::ImageLayout::eShaderReadOnlyOptimal
            : layout;
    }

}

// =========================================================================
// Regression: the layout transitions that existed before the access table
// =========================================================================

MAYAFLUX_TEST_WITH(TEST_F, ImageLayoutRegressionTest, EveryOrderedPairOfColorLayoutsTransitionsWithoutTheGenericFallback, IMAGE_TRANSITION_VALIDATED)
{
    auto image = make_image(8, 8, k_rgba, VKImage::Usage::RENDER_TARGET);
    const std::array layouts {
        vk::ImageLayout::eTransferDstOptimal,
        vk::ImageLayout::eTransferSrcOptimal,
        vk::ImageLayout::eShaderReadOnlyOptimal,
        vk::ImageLayout::eColorAttachmentOptimal,
        vk::ImageLayout::eGeneral,
    };

    for (const auto from : layouts) {
        for (const auto to : layouts) {
            if (from == to) {
                continue;
            }
            resources().transition_image_layout(image, from, SubmitMode::IMMEDIATE);
            resources().transition_image_layout(image, to, SubmitMode::IMMEDIATE);
            EXPECT_EQ(image->get_current_layout(), to) << vk::to_string(from) << " -> " << vk::to_string(to);
        }
    }

    EXPECT_FALSE(LogCapture::contains("generic image layout transition"));
    resources().cleanup_image(image);
}

MAYAFLUX_TEST_WITH(TEST_F, ImageLayoutRegressionTest, EveryOrderedPairOfDepthLayoutsTransitionsWithoutTheGenericFallback, IMAGE_TRANSITION_VALIDATED)
{
    auto image = make_image(8, 8, vk::Format::eD32Sfloat, VKImage::Usage::DEPTH_STENCIL);
    const std::array layouts {
        vk::ImageLayout::eTransferDstOptimal,
        vk::ImageLayout::eTransferSrcOptimal,
        vk::ImageLayout::eShaderReadOnlyOptimal,
        vk::ImageLayout::eDepthStencilAttachmentOptimal,
    };

    for (const auto from : layouts) {
        for (const auto to : layouts) {
            if (from == to) {
                continue;
            }
            resources().transition_image_layout(image, from, SubmitMode::IMMEDIATE);
            resources().transition_image_layout(image, to, SubmitMode::IMMEDIATE);
            EXPECT_EQ(image->get_current_layout(), to) << vk::to_string(from) << " -> " << vk::to_string(to);
        }
    }

    EXPECT_FALSE(LogCapture::contains("generic image layout transition"));
    resources().cleanup_image(image);
}

MAYAFLUX_TEST_WITH(TEST_F, ImageLayoutRegressionTest, EveryTransitionModeRecordsTheSameBarrier, IMAGE_TRANSITION_VALIDATED)
{
    auto image = make_image(8, 8, k_rgba, VKImage::Usage::RENDER_TARGET);

    for (const auto mode : { SubmitMode::DEFERRED, SubmitMode::ASYNC, SubmitMode::IMMEDIATE }) {
        resources().transition_image_layout(image, vk::ImageLayout::eColorAttachmentOptimal, mode);
        EXPECT_EQ(image->get_current_layout(), vk::ImageLayout::eColorAttachmentOptimal);
        resources().transition_image_layout(image, vk::ImageLayout::eShaderReadOnlyOptimal, mode);
        EXPECT_EQ(image->get_current_layout(), vk::ImageLayout::eShaderReadOnlyOptimal);
    }

    idle();
    EXPECT_FALSE(LogCapture::contains("generic image layout transition"));
    resources().cleanup_image(image);
}

MAYAFLUX_TEST_WITH(TEST_F, ImageLayoutRegressionTest, ALayoutNoAccessDescribesStillTransitionsThroughTheGenericBarrier, IMAGE_TRANSITION_VALIDATED)
{
    auto image = make_image(8, 8, vk::Format::eD32Sfloat, VKImage::Usage::DEPTH_STENCIL);
    resources().transition_image_layout(image, vk::ImageLayout::eTransferDstOptimal, SubmitMode::IMMEDIATE);
    LogCapture::clear();

    resources().transition_image_layout(image, vk::ImageLayout::eDepthStencilReadOnlyOptimal, SubmitMode::IMMEDIATE);
    EXPECT_EQ(image->get_current_layout(), vk::ImageLayout::eDepthStencilReadOnlyOptimal);
    EXPECT_TRUE(LogCapture::contains("generic image layout transition"));

    LogCapture::clear();
    resources().transition_image_layout(image, vk::ImageLayout::eTransferDstOptimal, SubmitMode::IMMEDIATE);
    EXPECT_EQ(image->get_current_layout(), vk::ImageLayout::eTransferDstOptimal);
    EXPECT_TRUE(LogCapture::contains("generic image layout transition"));

    resources().cleanup_image(image);
}

MAYAFLUX_TEST_WITH(TEST_F, ImageLayoutRegressionTest, StagedUploadKeepsItsBytesFromEveryStartingLayout, IMAGE_TRANSITION_VALIDATED)
{
    for (const auto start : {
             vk::ImageLayout::eUndefined,
             vk::ImageLayout::eShaderReadOnlyOptimal,
             vk::ImageLayout::eGeneral,
             vk::ImageLayout::eTransferSrcOptimal,
             vk::ImageLayout::eTransferDstOptimal,
         }) {
        auto image = make_image(4, 4, k_rgba);
        settle(image, start);
        auto staging = staging_buffer(64);
        const auto bytes = pattern(64, 11);

        resources().upload_image_data(image, bytes.data(), bytes.size(), staging, true);
        std::vector<uint8_t> out(64, 0);
        resources().download_image_data(image, out.data(), out.size());

        EXPECT_EQ(out, bytes) << vk::to_string(start);
        resources().cleanup_buffer(staging);
        resources().cleanup_image(image);
    }

    EXPECT_FALSE(LogCapture::contains("generic image layout transition"));
}

MAYAFLUX_TEST_WITH(TEST_F, ImageLayoutRegressionTest, BlitCopiesPixelsFromAndToEveryStartingLayoutInEveryMode, IMAGE_TRANSITION_VALIDATED)
{
    for (const auto src_start : {
             vk::ImageLayout::eShaderReadOnlyOptimal,
             vk::ImageLayout::eGeneral,
             vk::ImageLayout::eTransferSrcOptimal,
         }) {
        for (const auto dst_start : {
                 vk::ImageLayout::eShaderReadOnlyOptimal,
                 vk::ImageLayout::eGeneral,
                 vk::ImageLayout::eTransferDstOptimal,
             }) {
            for (const auto mode : { SubmitMode::DEFERRED, SubmitMode::ASYNC, SubmitMode::IMMEDIATE }) {
                auto src = make_image(4, 4, k_rgba);
                auto dst = make_image(4, 4, k_rgba);
                const auto bytes = pattern(64, 17);
                resources().upload_image_data(src, bytes.data(), bytes.size());
                settle(src, src_start);
                settle(dst, dst_start);

                ASSERT_TRUE(resources().blit_image(src, dst, whole_blit(4), vk::Filter::eNearest, mode));
                EXPECT_EQ(src->get_current_layout(), transient_to_settled(src_start));
                EXPECT_EQ(dst->get_current_layout(), transient_to_settled(dst_start));

                std::vector<uint8_t> out(64, 0);
                resources().download_image_data(dst, out.data(), out.size());
                EXPECT_EQ(out, bytes) << vk::to_string(src_start) << " -> " << vk::to_string(dst_start);
                resources().cleanup_image(src);
                resources().cleanup_image(dst);
            }
        }
    }

    EXPECT_FALSE(LogCapture::contains("generic image layout transition"));
}

// =========================================================================
// record_image_transition
// =========================================================================

MAYAFLUX_TEST_WITH(TEST_F, ImageRangeTransitionTest, MovingTheWholeImageUpdatesTheTrackedLayout, IMAGE_TRANSITION_VALIDATED)
{
    auto image = make_image(8, 8, k_rgba, VKImage::Usage::RENDER_TARGET);

    resources().execute_immediate_commands([&](vk::CommandBuffer cmd) {
        resources().record_image_transition(cmd, image, ImageAccess::UNDEFINED, ImageAccess::TRANSFER_DST);
    });
    EXPECT_EQ(image->get_current_layout(), vk::ImageLayout::eTransferDstOptimal);

    resources().execute_immediate_commands([&](vk::CommandBuffer cmd) {
        resources().record_image_transition(cmd, image, ImageAccess::TRANSFER_DST, ImageAccess::SHADER_READ);
    });
    EXPECT_EQ(image->get_current_layout(), vk::ImageLayout::eShaderReadOnlyOptimal);

    resources().cleanup_image(image);
}

MAYAFLUX_TEST_WITH(TEST_F, ImageRangeTransitionTest, AnExplicitRangeThatCoversTheImageAlsoUpdatesTheTrackedLayout, IMAGE_TRANSITION_VALIDATED)
{
    auto image = make_image(8, 8, k_rgba, VKImage::Usage::RENDER_TARGET, 3, 2);

    resources().execute_immediate_commands([&](vk::CommandBuffer cmd) {
        resources().record_image_transition(cmd, image, ImageAccess::UNDEFINED, ImageAccess::SHADER_READ,
            { .base_mip = 0, .mip_count = 3, .base_layer = 0, .layer_count = 2 });
    });

    EXPECT_EQ(image->get_current_layout(), vk::ImageLayout::eShaderReadOnlyOptimal);
    resources().cleanup_image(image);
}

MAYAFLUX_TEST_WITH(TEST_F, ImageRangeTransitionTest, MovingARangeLeavesTheTrackedLayoutToItsOwner, IMAGE_TRANSITION_VALIDATED)
{
    auto image = make_image(8, 8, k_rgba, VKImage::Usage::RENDER_TARGET, 3);

    resources().execute_immediate_commands([&](vk::CommandBuffer cmd) {
        resources().record_image_transition(cmd, image, ImageAccess::UNDEFINED, ImageAccess::TRANSFER_DST,
            { .base_mip = 1, .mip_count = 1 });
        resources().record_image_transition(cmd, image, ImageAccess::TRANSFER_DST, ImageAccess::SHADER_READ,
            { .base_mip = 1, .mip_count = 1 });
    });

    EXPECT_EQ(image->get_current_layout(), vk::ImageLayout::eUndefined);
    resources().cleanup_image(image);
}

MAYAFLUX_TEST_WITH(TEST_F, ImageRangeTransitionTest, MipLevelsCanBeInDifferentUsesAtOnceAndThenJoinAgain, IMAGE_TRANSITION_VALIDATED)
{
    auto image = make_image(8, 8, k_rgba, VKImage::Usage::RENDER_TARGET, 3);

    resources().execute_immediate_commands([&](vk::CommandBuffer cmd) {
        resources().record_image_transition(cmd, image, ImageAccess::UNDEFINED, ImageAccess::TRANSFER_DST,
            { .base_mip = 0, .mip_count = 1 });
        resources().record_image_transition(cmd, image, ImageAccess::UNDEFINED, ImageAccess::COLOR_ATTACHMENT,
            { .base_mip = 1, .mip_count = 1 });
        resources().record_image_transition(cmd, image, ImageAccess::UNDEFINED, ImageAccess::STORAGE,
            { .base_mip = 2, .mip_count = 1 });
    });
    resources().execute_immediate_commands([&](vk::CommandBuffer cmd) {
        resources().record_image_transition(cmd, image, ImageAccess::TRANSFER_DST, ImageAccess::SHADER_READ,
            { .base_mip = 0, .mip_count = 1 });
        resources().record_image_transition(cmd, image, ImageAccess::COLOR_ATTACHMENT, ImageAccess::SHADER_READ,
            { .base_mip = 1, .mip_count = 1 });
        resources().record_image_transition(cmd, image, ImageAccess::STORAGE, ImageAccess::SHADER_READ,
            { .base_mip = 2, .mip_count = 1 });
    });
    resources().execute_immediate_commands([&](vk::CommandBuffer cmd) {
        resources().record_image_transition(cmd, image, ImageAccess::SHADER_READ, ImageAccess::TRANSFER_SRC);
    });

    EXPECT_EQ(image->get_current_layout(), vk::ImageLayout::eTransferSrcOptimal);
    resources().cleanup_image(image);
}

MAYAFLUX_TEST_WITH(TEST_F, ImageRangeTransitionTest, ArrayLayersMoveOnTheirOwn, IMAGE_TRANSITION_VALIDATED)
{
    auto image = make_image(8, 8, k_rgba, VKImage::Usage::RENDER_TARGET, 1, 3);

    resources().execute_immediate_commands([&](vk::CommandBuffer cmd) {
        resources().record_image_transition(cmd, image, ImageAccess::UNDEFINED, ImageAccess::COLOR_ATTACHMENT,
            { .base_layer = 2, .layer_count = 1 });
        resources().record_image_transition(cmd, image, ImageAccess::COLOR_ATTACHMENT, ImageAccess::SHADER_READ,
            { .base_layer = 2, .layer_count = 1 });
    });

    EXPECT_EQ(image->get_current_layout(), vk::ImageLayout::eUndefined);
    resources().cleanup_image(image);
}

MAYAFLUX_TEST_WITH(TEST_F, ImageRangeTransitionTest, ADepthImageMovesBetweenAttachmentAndSampledUse, IMAGE_TRANSITION_VALIDATED)
{
    auto image = make_image(8, 8, vk::Format::eD32Sfloat, VKImage::Usage::DEPTH_STENCIL);

    resources().execute_immediate_commands([&](vk::CommandBuffer cmd) {
        resources().record_image_transition(cmd, image, ImageAccess::UNDEFINED, ImageAccess::DEPTH_ATTACHMENT);
        resources().record_image_transition(cmd, image, ImageAccess::DEPTH_ATTACHMENT, ImageAccess::SHADER_READ);
    });

    EXPECT_EQ(image->get_current_layout(), vk::ImageLayout::eShaderReadOnlyOptimal);
    resources().cleanup_image(image);
}

TEST_F(ImageRangeTransitionTest, ARangeOutsideTheImageRecordsNothingAndSaysSo)
{
    auto image = make_image(8, 8, k_rgba, VKImage::Usage::RENDER_TARGET, 2, 2);

    resources().execute_immediate_commands([&](vk::CommandBuffer cmd) {
        resources().record_image_transition(cmd, image, ImageAccess::UNDEFINED, ImageAccess::TRANSFER_DST,
            { .base_mip = 2 });
        resources().record_image_transition(cmd, image, ImageAccess::UNDEFINED, ImageAccess::TRANSFER_DST,
            { .base_mip = 1, .mip_count = 2 });
        resources().record_image_transition(cmd, image, ImageAccess::UNDEFINED, ImageAccess::TRANSFER_DST,
            { .base_layer = 2 });
        resources().record_image_transition(cmd, image, ImageAccess::UNDEFINED, ImageAccess::TRANSFER_DST,
            { .mip_count = 0 });
        resources().record_image_transition(cmd, image, ImageAccess::UNDEFINED, ImageAccess::TRANSFER_DST,
            { .layer_count = 0 });
    });

    EXPECT_EQ(image->get_current_layout(), vk::ImageLayout::eUndefined);
    EXPECT_TRUE(LogCapture::contains("range lies outside the image"));
    resources().cleanup_image(image);
}

TEST_F(ImageRangeTransitionTest, NullArgumentsAreRefusedWithoutThrowing)
{
    auto image = make_image(8, 8, k_rgba, VKImage::Usage::RENDER_TARGET);
    auto unmanaged = std::make_shared<VKImage>(8, 8, 1, k_rgba, VKImage::Usage::RENDER_TARGET);

    resources().execute_immediate_commands([&](vk::CommandBuffer cmd) {
        EXPECT_NO_THROW(resources().record_image_transition({}, image, ImageAccess::UNDEFINED, ImageAccess::TRANSFER_DST));
        EXPECT_NO_THROW(resources().record_image_transition(cmd, nullptr, ImageAccess::UNDEFINED, ImageAccess::TRANSFER_DST));
        EXPECT_NO_THROW(resources().record_image_transition(cmd, unmanaged, ImageAccess::UNDEFINED, ImageAccess::TRANSFER_DST));
    });

    EXPECT_EQ(image->get_current_layout(), vk::ImageLayout::eUndefined);
    resources().cleanup_image(image);
}

// =========================================================================
// get_image_view
// =========================================================================

TEST_F(ImageViewTest, ADefaultKeyGivesAViewOfTheWholeImage)
{
    auto image = make_image(8, 8, k_rgba, VKImage::Usage::RENDER_TARGET);

    EXPECT_TRUE(resources().get_image_view(image));
    resources().cleanup_image(image);
}

TEST_F(ImageViewTest, TheSameKeyGivesTheSameViewAndAnEquivalentKeyDoes)
{
    auto image = make_image(8, 8, k_rgba, VKImage::Usage::RENDER_TARGET, 3);

    const auto first = resources().get_image_view(image, { .range = { .base_mip = 1, .mip_count = 1 } });
    const auto again = resources().get_image_view(image, { .range = { .base_mip = 1, .mip_count = 1 } });
    const auto spelled_out = resources().get_image_view(image,
        { .type = vk::ImageViewType::e2D,
            .range = { .base_mip = 1, .mip_count = 1, .base_layer = 0, .layer_count = 1 },
            .aspect = vk::ImageAspectFlagBits::eColor });

    ASSERT_TRUE(first);
    EXPECT_EQ(first, again);
    EXPECT_EQ(first, spelled_out);
    resources().cleanup_image(image);
}

TEST_F(ImageViewTest, DifferentRangesGiveDifferentViews)
{
    auto image = make_image(8, 8, k_rgba, VKImage::Usage::RENDER_TARGET, 3, 2);

    const auto whole = resources().get_image_view(image);
    const auto mip0 = resources().get_image_view(image, { .range = { .base_mip = 0, .mip_count = 1, .base_layer = 0, .layer_count = 1 } });
    const auto mip1 = resources().get_image_view(image, { .range = { .base_mip = 1, .mip_count = 1, .base_layer = 0, .layer_count = 1 } });
    const auto layer1 = resources().get_image_view(image, { .range = { .base_mip = 0, .mip_count = 1, .base_layer = 1, .layer_count = 1 } });

    ASSERT_TRUE(whole);
    ASSERT_TRUE(mip0);
    ASSERT_TRUE(mip1);
    ASSERT_TRUE(layer1);
    EXPECT_NE(whole, mip0);
    EXPECT_NE(mip0, mip1);
    EXPECT_NE(mip0, layer1);
    EXPECT_NE(mip1, layer1);
    resources().cleanup_image(image);
}

TEST_F(ImageViewTest, OneLayerOfAnArrayAndOneFaceOfACubeHaveAView)
{
    auto array = make_image(8, 8, k_rgba, VKImage::Usage::RENDER_TARGET, 1, 3);
    auto cube = std::make_shared<VKImage>(8, 8, 1, k_rgba, VKImage::Usage::RENDER_TARGET,
        VKImage::Type::TYPE_CUBE, 1, 6);
    resources().initialize_image(cube);

    EXPECT_TRUE(resources().get_image_view(array, { .range = { .base_layer = 2, .layer_count = 1 } }));
    EXPECT_TRUE(resources().get_image_view(cube, { .range = { .base_layer = 3, .layer_count = 1 } }));
    EXPECT_TRUE(resources().get_image_view(cube));

    resources().cleanup_image(array);
    resources().cleanup_image(cube);
}

TEST_F(ImageViewTest, ADepthImageHasADepthOnlyView)
{
    auto image = make_image(8, 8, vk::Format::eD32Sfloat, VKImage::Usage::DEPTH_STENCIL);

    EXPECT_TRUE(resources().get_image_view(image, { .aspect = vk::ImageAspectFlagBits::eDepth }));
    EXPECT_TRUE(resources().get_image_view(image));
    resources().cleanup_image(image);
}

TEST_F(ImageViewTest, AKeyOutsideTheImageGivesNoViewAndSaysSo)
{
    auto image = make_image(8, 8, k_rgba, VKImage::Usage::RENDER_TARGET, 2, 1);

    EXPECT_FALSE(resources().get_image_view(image, { .range = { .base_mip = 2 } }));
    EXPECT_FALSE(resources().get_image_view(image, { .range = { .mip_count = 3 } }));
    EXPECT_FALSE(resources().get_image_view(image, { .range = { .layer_count = 2 } }));
    EXPECT_FALSE(resources().get_image_view(image, { .range = { .mip_count = 0 } }));
    EXPECT_TRUE(LogCapture::contains("range lies outside the image"));
    resources().cleanup_image(image);
}

TEST_F(ImageViewTest, NullUnmanagedAndCleanedUpImagesGiveNoView)
{
    auto unmanaged = std::make_shared<VKImage>(8, 8, 1, k_rgba, VKImage::Usage::RENDER_TARGET);
    auto cleaned = make_image(8, 8, k_rgba, VKImage::Usage::RENDER_TARGET);
    ASSERT_TRUE(resources().get_image_view(cleaned));
    resources().cleanup_image(cleaned);

    EXPECT_FALSE(resources().get_image_view(nullptr));
    EXPECT_FALSE(resources().get_image_view(unmanaged));
    EXPECT_FALSE(resources().get_image_view(cleaned));
}

TEST_F(ImageViewTest, AnImageThatIsInitializedAgainStartsWithNoSubViewsOfTheOldAllocation)
{
    auto image = make_image(8, 8, k_rgba, VKImage::Usage::RENDER_TARGET, 2);
    const auto before = resources().get_image_view(image, { .range = { .base_mip = 1, .mip_count = 1 } });
    ASSERT_TRUE(before);

    resources().cleanup_image(image);
    resources().initialize_image(image);
    const auto after = resources().get_image_view(image, { .range = { .base_mip = 1, .mip_count = 1 } });

    EXPECT_TRUE(after);
    resources().cleanup_image(image);
}

MAYAFLUX_TEST_WITH(TEST_F, ImageViewTest, ViewsAreReleasedWithTheirImageSoNoViewLeaksAtDeviceTeardown, IMAGE_TRANSITION_VALIDATED)
{
    auto image = make_image(8, 8, k_rgba, VKImage::Usage::RENDER_TARGET, 3, 2);
    ASSERT_TRUE(resources().get_image_view(image, { .range = { .base_mip = 1, .mip_count = 1 } }));
    ASSERT_TRUE(resources().get_image_view(image, { .range = { .base_layer = 1, .layer_count = 1 } }));

    resources().cleanup_image(image);
    idle();
    backend().get_command_manager().collect();

    SUCCEED();
}

MAYAFLUX_TEST_WITH(TEST_F, ImageViewTest, ARecordedPassCanRenderIntoOneMipLevelThroughItsView, IMAGE_TRANSITION_VALIDATED)
{
    auto image = make_image(8, 8, k_rgba, VKImage::Usage::RENDER_TARGET, 3);
    const auto view = resources().get_image_view(image, { .range = { .base_mip = 1, .mip_count = 1 } });
    ASSERT_TRUE(view);
    const ImageRange mip1 { .base_mip = 1, .mip_count = 1 };

    resources().execute_immediate_commands([&](vk::CommandBuffer cmd) {
        resources().record_image_transition(cmd, image, ImageAccess::UNDEFINED, ImageAccess::COLOR_ATTACHMENT, mip1);

        vk::RenderingAttachmentInfo color {};
        color.imageView = view;
        color.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
        color.loadOp = vk::AttachmentLoadOp::eClear;
        color.storeOp = vk::AttachmentStoreOp::eStore;
        color.clearValue = vk::ClearValue(vk::ClearColorValue(std::array<float, 4> { 0.0F, 0.5F, 1.0F, 1.0F }));

        vk::RenderingInfo rendering {};
        rendering.renderArea = vk::Rect2D { { 0, 0 }, { 4, 4 } };
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &color;

        cmd.beginRendering(rendering);
        cmd.endRendering();

        resources().record_image_transition(cmd, image, ImageAccess::COLOR_ATTACHMENT, ImageAccess::SHADER_READ, mip1);
    });

    resources().cleanup_image(image);
}

MAYAFLUX_TEST_WITH(TEST_F, ImageViewTest, ARecordedPassCanRenderIntoOneArrayLayerThroughItsView, IMAGE_TRANSITION_VALIDATED)
{
    auto image = make_image(8, 8, k_rgba, VKImage::Usage::RENDER_TARGET, 1, 3);
    const auto view = resources().get_image_view(image, { .range = { .base_layer = 2, .layer_count = 1 } });
    ASSERT_TRUE(view);
    const ImageRange layer2 { .base_layer = 2, .layer_count = 1 };

    resources().execute_immediate_commands([&](vk::CommandBuffer cmd) {
        resources().record_image_transition(cmd, image, ImageAccess::UNDEFINED, ImageAccess::COLOR_ATTACHMENT, layer2);

        vk::RenderingAttachmentInfo color {};
        color.imageView = view;
        color.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
        color.loadOp = vk::AttachmentLoadOp::eClear;
        color.storeOp = vk::AttachmentStoreOp::eStore;
        color.clearValue = vk::ClearValue(vk::ClearColorValue(std::array<float, 4> { 1.0F, 0.0F, 0.0F, 1.0F }));

        vk::RenderingInfo rendering {};
        rendering.renderArea = vk::Rect2D { { 0, 0 }, { 8, 8 } };
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &color;

        cmd.beginRendering(rendering);
        cmd.endRendering();

        resources().record_image_transition(cmd, image, ImageAccess::COLOR_ATTACHMENT, ImageAccess::SHADER_READ, layer2);
    });

    resources().cleanup_image(image);
}

// =========================================================================
// generate_mip_chain
// =========================================================================

MAYAFLUX_TEST_WITH(TEST_F, ImageMipChainTest, EveryLevelTakesTheColorOfTheFirst, IMAGE_TRANSITION_VALIDATED)
{
    const std::array<uint8_t, 4> color { 200, 100, 50, 255 };
    auto image = solid_image(8, 8, 4, color);

    ASSERT_TRUE(resources().generate_mip_chain(image, vk::Filter::eLinear, SubmitMode::IMMEDIATE));

    expect_every_level_solid(image, color);
    resources().cleanup_image(image);
}

MAYAFLUX_TEST_WITH(TEST_F, ImageMipChainTest, LevelsThatHeldSomethingElseAreOverwritten, IMAGE_TRANSITION_VALIDATED)
{
    const std::array<uint8_t, 4> color { 30, 60, 90, 255 };
    auto image = solid_image(8, 8, 4, color);
    resources().execute_immediate_commands([&](vk::CommandBuffer cmd) {
        resources().record_image_transition(cmd, image, ImageAccess::UNDEFINED, ImageAccess::TRANSFER_DST,
            { .base_mip = 1, .mip_count = 3 });
        const vk::ClearColorValue blank(std::array<float, 4> { 0.0F, 0.0F, 0.0F, 0.0F });
        const vk::ImageSubresourceRange lower { vk::ImageAspectFlagBits::eColor, 1, 3, 0, 1 };
        cmd.clearColorImage(image->get_image(), vk::ImageLayout::eTransferDstOptimal, &blank, 1, &lower);
        resources().record_image_transition(cmd, image, ImageAccess::TRANSFER_DST, ImageAccess::SHADER_READ,
            { .base_mip = 1, .mip_count = 3 });
    });

    ASSERT_TRUE(resources().generate_mip_chain(image));

    expect_every_level_solid(image, color);
    resources().cleanup_image(image);
}

MAYAFLUX_TEST_WITH(TEST_F, ImageMipChainTest, EverySubmitModeProducesTheChain, IMAGE_TRANSITION_VALIDATED)
{
    const std::array<uint8_t, 4> color { 250, 10, 120, 255 };

    for (const auto mode : { SubmitMode::DEFERRED, SubmitMode::ASYNC, SubmitMode::IMMEDIATE }) {
        auto image = solid_image(8, 8, 4, color);

        ASSERT_TRUE(resources().generate_mip_chain(image, vk::Filter::eLinear, mode));
        idle();

        expect_every_level_solid(image, color);
        resources().cleanup_image(image);
    }
}

MAYAFLUX_TEST_WITH(TEST_F, ImageMipChainTest, BothFiltersGiveTheChainForASolidImage, IMAGE_TRANSITION_VALIDATED)
{
    const std::array<uint8_t, 4> color { 11, 22, 33, 255 };

    for (const auto filter : { vk::Filter::eNearest, vk::Filter::eLinear }) {
        auto image = solid_image(8, 8, 4, color);

        ASSERT_TRUE(resources().generate_mip_chain(image, filter));

        expect_every_level_solid(image, color);
        resources().cleanup_image(image);
    }
}

MAYAFLUX_TEST_WITH(TEST_F, ImageMipChainTest, OddAndNonSquareExtentsReachAOneByOneLevel, IMAGE_TRANSITION_VALIDATED)
{
    const std::array<uint8_t, 4> color { 90, 80, 70, 255 };
    auto image = solid_image(5, 3, 3, color);

    ASSERT_TRUE(resources().generate_mip_chain(image));

    expect_every_level_solid(image, color);
    resources().cleanup_image(image);
}

MAYAFLUX_TEST_WITH(TEST_F, ImageMipChainTest, EveryArrayLayerGetsItsOwnChain, IMAGE_TRANSITION_VALIDATED)
{
    auto image = make_image(8, 8, k_rgba, VKImage::Usage::TEXTURE_2D, 3, 2);
    const std::array<uint8_t, 4> first { 255, 0, 0, 255 };
    const std::array<uint8_t, 4> second { 0, 0, 255, 255 };
    const auto first_bytes = solid(8, 8, first);
    const auto second_bytes = solid(8, 8, second);
    ASSERT_TRUE(resources().upload_image_layer(image, 0, first_bytes.data(), first_bytes.size()));
    ASSERT_TRUE(resources().upload_image_layer(image, 1, second_bytes.data(), second_bytes.size()));
    resources().transition_image_layout(image, vk::ImageLayout::eShaderReadOnlyOptimal, SubmitMode::IMMEDIATE);

    ASSERT_TRUE(resources().generate_mip_chain(image));

    expect_every_level_solid(image, first, 0);
    expect_every_level_solid(image, second, 1);
    resources().cleanup_image(image);
}

MAYAFLUX_TEST_WITH(TEST_F, ImageMipChainTest, TheImageKeepsAStableLayoutAndSettlesATransientOne, IMAGE_TRANSITION_VALIDATED)
{
    const std::array<uint8_t, 4> color { 1, 2, 3, 255 };

    for (const auto start : {
             vk::ImageLayout::eShaderReadOnlyOptimal,
             vk::ImageLayout::eGeneral,
             vk::ImageLayout::eTransferSrcOptimal,
             vk::ImageLayout::eTransferDstOptimal,
         }) {
        auto image = solid_image(8, 8, 4, color);
        settle(image, start);

        ASSERT_TRUE(resources().generate_mip_chain(image));

        EXPECT_EQ(image->get_current_layout(), transient_to_settled(start)) << vk::to_string(start);
        resources().cleanup_image(image);
    }

    EXPECT_FALSE(LogCapture::contains("generic image layout transition"));
}

MAYAFLUX_TEST_WITH(TEST_F, ImageMipChainTest, ARenderTargetKeepsItsAttachmentLayoutAfterItsChainIsMade, IMAGE_TRANSITION_VALIDATED)
{
    auto image = make_image(8, 8, k_rgba, VKImage::Usage::RENDER_TARGET, 4);
    settle(image, vk::ImageLayout::eColorAttachmentOptimal);

    ASSERT_TRUE(resources().generate_mip_chain(image));

    EXPECT_EQ(image->get_current_layout(), vk::ImageLayout::eColorAttachmentOptimal);
    resources().cleanup_image(image);
}

TEST_F(ImageMipChainTest, AnImageWithOneLevelIsRefusedAndSaysSo)
{
    auto image = make_image(8, 8, k_rgba);

    EXPECT_FALSE(resources().generate_mip_chain(image));

    EXPECT_TRUE(LogCapture::contains("one mip level"));
    resources().cleanup_image(image);
}

TEST_F(ImageMipChainTest, ADepthImageIsRefusedAndSaysSo)
{
    auto image = make_image(8, 8, vk::Format::eD32Sfloat, VKImage::Usage::DEPTH_STENCIL, 2);

    EXPECT_FALSE(resources().generate_mip_chain(image));

    EXPECT_TRUE(LogCapture::contains("only color images"));
    resources().cleanup_image(image);
}

TEST_F(ImageMipChainTest, NullAndUnmanagedImagesAreRefused)
{
    auto unmanaged = std::make_shared<VKImage>(8, 8, 1, k_rgba, VKImage::Usage::TEXTURE_2D,
        VKImage::Type::TYPE_2D, 3, 1);

    EXPECT_FALSE(resources().generate_mip_chain(nullptr));
    EXPECT_FALSE(resources().generate_mip_chain(unmanaged));
}

}
