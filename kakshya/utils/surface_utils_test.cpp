#include "gtest/gtest.h"

#include "../window_test_support.hpp"

#include "MayaFlux/Kakshya/Utils/SurfaceUtils.hpp"

namespace MayaFlux::Test {
namespace {

    using Kakshya::DataVariant;
    using Kakshya::SurfaceElement;
    using Kakshya::surface_shape;
    using namespace WindowSupport;

    struct ShapeCase {
        SurfaceFormat format;
        SurfaceElement element;
        std::type_index variant;
        uint32_t channels;
        bool packed;
        const char* name;
    };

    class SurfaceShapeTest : public ::testing::TestWithParam<ShapeCase> { };

    INSTANTIATE_TEST_SUITE_P(Surfaces, SurfaceShapeTest,
        ::testing::Values(
            ShapeCase { SurfaceFormat::B8G8R8A8_SRGB, SurfaceElement::U8, typeid(uint8_t), 4, false, "Bgra8Srgb" },
            ShapeCase { SurfaceFormat::B8G8R8A8_UNORM, SurfaceElement::U8, typeid(uint8_t), 4, false, "Bgra8Unorm" },
            ShapeCase { SurfaceFormat::R8G8B8A8_SRGB, SurfaceElement::U8, typeid(uint8_t), 4, false, "Rgba8Srgb" },
            ShapeCase { SurfaceFormat::R8G8B8A8_UNORM, SurfaceElement::U8, typeid(uint8_t), 4, false, "Rgba8Unorm" },
            ShapeCase { SurfaceFormat::R16G16B16A16_SFLOAT, SurfaceElement::U16, typeid(uint16_t), 4, false, "Rgba16F" },
            ShapeCase { SurfaceFormat::R32G32B32A32_SFLOAT, SurfaceElement::F32, typeid(float), 4, false, "Rgba32F" },
            ShapeCase { SurfaceFormat::A2B10G10R10_UNORM, SurfaceElement::U32, typeid(uint32_t), 1, true, "A2b10g10r10" }),
        [](const ::testing::TestParamInfo<ShapeCase>& info) { return std::string(info.param.name); });

    TEST_P(SurfaceShapeTest, TheShapeNamesTheElementTypeAndChannels)
    {
        const auto& c = GetParam();

        const auto shape = surface_shape(c.format);

        EXPECT_EQ(shape.element, c.element);
        EXPECT_EQ(shape.channels, c.channels);
        EXPECT_EQ(shape.packed, c.packed);
    }

    TEST_P(SurfaceShapeTest, ReadbackProducesTheVariantTheShapeNames)
    {
        const auto& c = GetParam();
        WindowRig rig(k_history, c.format);
        const auto frame = rig.display.present(2);
        DataVariant out = std::vector<uint8_t> {};

        Kakshya::readback_region(rig.window, 0, 0, k_surface_width, k_surface_height, out);

        EXPECT_EQ(element_type(out), c.variant);
        EXPECT_EQ(raw_bytes(out), frame);
    }

    TEST(SurfaceUtilsTest, ReadbackCropsARectangleRowByRow)
    {
        WindowRig rig;
        const auto frame = rig.display.present(1);
        DataVariant out = std::vector<uint8_t> {};

        Kakshya::readback_region(rig.window, 1, 1, 2, 2, out);

        std::vector<uint8_t> expected;
        for (uint32_t y = 1; y <= 2; ++y) {
            const auto* row = frame.data() + (size_t { y } * k_surface_width + 1) * k_surface_channels;
            expected.insert(expected.end(), row, row + size_t { 2 } * k_surface_channels);
        }
        EXPECT_EQ(raw_bytes(out), expected);
    }

    TEST(SurfaceUtilsTest, ReadbackLeavesTheVariantAloneWithoutAFrame)
    {
        WindowRig rig;
        DataVariant out = std::vector<uint8_t> { 9, 9 };

        Kakshya::readback_region(rig.window, 0, 0, k_surface_width, k_surface_height, out);

        EXPECT_EQ(raw_bytes(out), (std::vector<uint8_t> { 9, 9 }));
    }

    TEST(SurfaceUtilsTest, ReadbackRefusesARectangleOutsideTheSurface)
    {
        WindowRig rig;
        rig.display.present(1);
        DataVariant out = std::vector<uint8_t> { 9, 9 };

        Kakshya::readback_region(rig.window, 3, 0, 2, 1, out);

        EXPECT_EQ(raw_bytes(out), (std::vector<uint8_t> { 9, 9 }));
    }

    TEST(SurfaceUtilsTest, TheExtentAndAvailabilityFollowTheDisplay)
    {
        WindowRig rig;

        EXPECT_EQ(Kakshya::query_surface_extent(rig.window), (std::pair<uint32_t, uint32_t> { k_surface_width, k_surface_height }));
        EXPECT_TRUE(Kakshya::is_readback_available(rig.window));

        rig.display.resize(0, 0);
        EXPECT_FALSE(Kakshya::is_readback_available(rig.window));
    }

    TEST(SurfaceUtilsTest, TheSurfaceFormatFollowsTheDisplay)
    {
        WindowRig rig(k_history, SurfaceFormat::R16G16B16A16_SFLOAT);

        EXPECT_EQ(Kakshya::query_surface_format(rig.window), SurfaceFormat::R16G16B16A16_SFLOAT);
    }

} // namespace
} // namespace MayaFlux::Test
