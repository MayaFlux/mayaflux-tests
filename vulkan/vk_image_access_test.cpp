#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Core/Backends/Graphics/Vulkan/ImageAccess.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Core::ImageAccess;
    using Core::ImageRange;
    using Core::ImageViewKey;

    struct AccessCase {
        const char* name;
        ImageAccess access;
        vk::ImageLayout layout;
    };

    constexpr std::array k_cases {
        AccessCase { "Undefined", ImageAccess::UNDEFINED, vk::ImageLayout::eUndefined },
        AccessCase { "ColorAttachment", ImageAccess::COLOR_ATTACHMENT, vk::ImageLayout::eColorAttachmentOptimal },
        AccessCase { "DepthAttachment", ImageAccess::DEPTH_ATTACHMENT, vk::ImageLayout::eDepthStencilAttachmentOptimal },
        AccessCase { "ShaderRead", ImageAccess::SHADER_READ, vk::ImageLayout::eShaderReadOnlyOptimal },
        AccessCase { "Storage", ImageAccess::STORAGE, vk::ImageLayout::eGeneral },
        AccessCase { "TransferSrc", ImageAccess::TRANSFER_SRC, vk::ImageLayout::eTransferSrcOptimal },
        AccessCase { "TransferDst", ImageAccess::TRANSFER_DST, vk::ImageLayout::eTransferDstOptimal },
        AccessCase { "Present", ImageAccess::PRESENT, vk::ImageLayout::ePresentSrcKHR },
    };

    bool has(vk::AccessFlags2 flags, vk::AccessFlagBits2 bit)
    {
        return static_cast<bool>(flags & bit);
    }

    bool has(vk::PipelineStageFlags2 flags, vk::PipelineStageFlagBits2 bit)
    {
        return static_cast<bool>(flags & bit);
    }

    class ImageAccessTableTest : public ::testing::TestWithParam<AccessCase> { };

}

TEST_P(ImageAccessTableTest, EachAccessHasItsOwnLayout)
{
    EXPECT_EQ(Core::image_access_info(GetParam().access).layout, GetParam().layout);
}

TEST_P(ImageAccessTableTest, ItsLayoutMapsBackToIt)
{
    const auto back = Core::image_access_of(Core::image_access_info(GetParam().access).layout);

    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(*back, GetParam().access);
}

TEST_P(ImageAccessTableTest, AnAccessThatTouchesMemoryNamesAStageAndAnAccessMask)
{
    const auto info = Core::image_access_info(GetParam().access);
    const bool touches_memory = GetParam().access != ImageAccess::UNDEFINED
        && GetParam().access != ImageAccess::PRESENT;

    EXPECT_EQ(info.stages != vk::PipelineStageFlags2 {}, touches_memory);
    EXPECT_EQ(info.access != vk::AccessFlags2 {}, touches_memory);
}

INSTANTIATE_TEST_SUITE_P(Accesses, ImageAccessTableTest, ::testing::ValuesIn(k_cases),
    [](const ::testing::TestParamInfo<AccessCase>& info) { return std::string(info.param.name); });

TEST(ImageAccessInfoTest, UsesThatWriteNameAWriteAccess)
{
    EXPECT_TRUE(has(Core::image_access_info(ImageAccess::COLOR_ATTACHMENT).access, vk::AccessFlagBits2::eColorAttachmentWrite));
    EXPECT_TRUE(has(Core::image_access_info(ImageAccess::DEPTH_ATTACHMENT).access, vk::AccessFlagBits2::eDepthStencilAttachmentWrite));
    EXPECT_TRUE(has(Core::image_access_info(ImageAccess::STORAGE).access, vk::AccessFlagBits2::eShaderWrite));
    EXPECT_TRUE(has(Core::image_access_info(ImageAccess::TRANSFER_DST).access, vk::AccessFlagBits2::eTransferWrite));
}

TEST(ImageAccessInfoTest, ReadOnlyUsesNameNoWriteAccess)
{
    EXPECT_FALSE(has(Core::image_access_info(ImageAccess::SHADER_READ).access, vk::AccessFlagBits2::eShaderWrite));
    EXPECT_FALSE(has(Core::image_access_info(ImageAccess::TRANSFER_SRC).access, vk::AccessFlagBits2::eTransferWrite));
    EXPECT_TRUE(has(Core::image_access_info(ImageAccess::TRANSFER_SRC).access, vk::AccessFlagBits2::eTransferRead));
}

TEST(ImageAccessInfoTest, ASampledImageIsReadableFromEveryShaderStage)
{
    const auto stages = Core::image_access_info(ImageAccess::SHADER_READ).stages;

    EXPECT_TRUE(has(stages, vk::PipelineStageFlagBits2::eAllGraphics));
    EXPECT_TRUE(has(stages, vk::PipelineStageFlagBits2::eComputeShader));
}

TEST(ImageAccessInfoTest, AStorageImageIsReachableFromGraphicsAndCompute)
{
    const auto stages = Core::image_access_info(ImageAccess::STORAGE).stages;

    EXPECT_TRUE(has(stages, vk::PipelineStageFlagBits2::eAllGraphics));
    EXPECT_TRUE(has(stages, vk::PipelineStageFlagBits2::eComputeShader));
}

TEST(ImageAccessInfoTest, AttachmentsAreOrderedAtTheirOwnStages)
{
    EXPECT_TRUE(has(Core::image_access_info(ImageAccess::COLOR_ATTACHMENT).stages, vk::PipelineStageFlagBits2::eColorAttachmentOutput));
    const auto depth = Core::image_access_info(ImageAccess::DEPTH_ATTACHMENT).stages;
    EXPECT_TRUE(has(depth, vk::PipelineStageFlagBits2::eEarlyFragmentTests));
    EXPECT_TRUE(has(depth, vk::PipelineStageFlagBits2::eLateFragmentTests));
}

TEST(ImageAccessInfoTest, TransfersAreOrderedAtTheTransferStage)
{
    EXPECT_TRUE(has(Core::image_access_info(ImageAccess::TRANSFER_SRC).stages, vk::PipelineStageFlagBits2::eAllTransfer));
    EXPECT_TRUE(has(Core::image_access_info(ImageAccess::TRANSFER_DST).stages, vk::PipelineStageFlagBits2::eAllTransfer));
}

TEST(ImageAccessOfTest, LayoutsNoAccessDescribesMapToNothing)
{
    for (const auto layout : {
             vk::ImageLayout::ePreinitialized,
             vk::ImageLayout::eDepthStencilReadOnlyOptimal,
             vk::ImageLayout::eDepthReadOnlyStencilAttachmentOptimal,
             vk::ImageLayout::eDepthAttachmentStencilReadOnlyOptimal,
             vk::ImageLayout::eDepthAttachmentOptimal,
             vk::ImageLayout::eDepthReadOnlyOptimal,
             vk::ImageLayout::eStencilAttachmentOptimal,
             vk::ImageLayout::eReadOnlyOptimal,
             vk::ImageLayout::eAttachmentOptimal,
         }) {
        EXPECT_FALSE(Core::image_access_of(layout).has_value()) << vk::to_string(layout);
    }
}

TEST(ImageAccessOfTest, EveryDescribedAccessHasADistinctLayout)
{
    std::set<VkImageLayout> layouts;
    for (const auto& item : k_cases) {
        layouts.insert(static_cast<VkImageLayout>(Core::image_access_info(item.access).layout));
    }

    EXPECT_EQ(layouts.size(), k_cases.size());
}

TEST(ImageRangeTest, ADefaultRangeCoversEverythingFromTheFirstMipAndLayer)
{
    const ImageRange range;

    EXPECT_EQ(range.base_mip, 0U);
    EXPECT_EQ(range.base_layer, 0U);
    EXPECT_EQ(range.mip_count, VK_REMAINING_MIP_LEVELS);
    EXPECT_EQ(range.layer_count, VK_REMAINING_ARRAY_LAYERS);
}

TEST(ImageRangeTest, RangesCompareByEveryField)
{
    const ImageRange base { .base_mip = 1, .mip_count = 2, .base_layer = 3, .layer_count = 4 };

    EXPECT_EQ(base, (ImageRange { .base_mip = 1, .mip_count = 2, .base_layer = 3, .layer_count = 4 }));
    EXPECT_NE(base, (ImageRange { .base_mip = 0, .mip_count = 2, .base_layer = 3, .layer_count = 4 }));
    EXPECT_NE(base, (ImageRange { .base_mip = 1, .mip_count = 1, .base_layer = 3, .layer_count = 4 }));
    EXPECT_NE(base, (ImageRange { .base_mip = 1, .mip_count = 2, .base_layer = 0, .layer_count = 4 }));
    EXPECT_NE(base, (ImageRange { .base_mip = 1, .mip_count = 2, .base_layer = 3, .layer_count = 1 }));
}

TEST(ImageViewKeyTest, ADefaultKeyAsksForNothingInParticular)
{
    const ImageViewKey key;

    EXPECT_FALSE(key.type.has_value());
    EXPECT_EQ(key.aspect, vk::ImageAspectFlags {});
    EXPECT_EQ(key.range, ImageRange {});
}

TEST(ImageViewKeyTest, KeysCompareByTypeRangeAndAspect)
{
    const ImageViewKey base { .type = vk::ImageViewType::e2DArray, .range = { .base_mip = 1 }, .aspect = vk::ImageAspectFlagBits::eDepth };

    EXPECT_EQ(base, (ImageViewKey { .type = vk::ImageViewType::e2DArray, .range = { .base_mip = 1 }, .aspect = vk::ImageAspectFlagBits::eDepth }));
    EXPECT_NE(base, (ImageViewKey { .type = vk::ImageViewType::e2D, .range = { .base_mip = 1 }, .aspect = vk::ImageAspectFlagBits::eDepth }));
    EXPECT_NE(base, (ImageViewKey { .type = vk::ImageViewType::e2DArray, .range = { .base_mip = 2 }, .aspect = vk::ImageAspectFlagBits::eDepth }));
    EXPECT_NE(base, (ImageViewKey { .type = vk::ImageViewType::e2DArray, .range = { .base_mip = 1 }, .aspect = vk::ImageAspectFlagBits::eColor }));
    EXPECT_NE(base, (ImageViewKey { .range = { .base_mip = 1 }, .aspect = vk::ImageAspectFlagBits::eDepth }));
}

}
