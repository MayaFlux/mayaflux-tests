#include "gtest/gtest.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "MayaFlux/Buffers/Textures/TextureBuffer.hpp"

namespace MayaFlux::Test {
namespace {

    using Buffers::TextureBuffer;
    using Portal::Graphics::ImageFormat;

    // ------------------------------------------------------------------
    // Construction
    // ------------------------------------------------------------------

    TEST(TextureBufferConstructionTest, DescribesAnEmptyQuadBackedBuffer)
    {
        const TextureBuffer buffer(64, 32, ImageFormat::RGBA8);

        EXPECT_EQ(buffer.get_width(), 64U);
        EXPECT_EQ(buffer.get_height(), 32U);
        EXPECT_EQ(buffer.get_format(), ImageFormat::RGBA8);
        EXPECT_EQ(buffer.get_usage(), Buffers::VKBuffer::Usage::VERTEX);
        EXPECT_EQ(buffer.get_modality(), Kakshya::DataModality::IMAGE_COLOR);
        EXPECT_EQ(buffer.get_size_bytes(), 4 * sizeof(Kakshya::TextureQuadVertex));
        EXPECT_TRUE(buffer.get_vertex_layout().has_value());
        EXPECT_FALSE(buffer.has_texture());
        EXPECT_EQ(buffer.get_texture(), nullptr);
        EXPECT_TRUE(buffer.get_pixel_data().empty());
        EXPECT_TRUE(buffer.is_texture_dirty());
    }

    TEST(TextureBufferConstructionTest, InitialPixelDataIsCopiedByWidthHeightAndFormat)
    {
        std::vector<uint8_t> pixels(size_t { 4 } * 3 * 4);
        for (size_t i = 0; i < pixels.size(); ++i) {
            pixels[i] = static_cast<uint8_t>(i * 7U);
        }

        TextureBuffer buffer(4, 3, ImageFormat::RGBA8, pixels.data());
        pixels.assign(pixels.size(), 0);

        ASSERT_EQ(buffer.get_pixel_data().size(), 48U);
        for (size_t i = 0; i < 48; ++i) {
            EXPECT_EQ(buffer.get_pixel_data()[i], static_cast<uint8_t>(i * 7U)) << i;
        }
    }

    TEST(TextureBufferConstructionTest, DefaultTransformIsIdentity)
    {
        const TextureBuffer buffer(8, 8, ImageFormat::RGBA8);
        EXPECT_EQ(buffer.get_position(), glm::vec2(0.F, 0.F));
        EXPECT_EQ(buffer.get_scale(), glm::vec2(1.F, 1.F));
        EXPECT_FLOAT_EQ(buffer.get_rotation(), 0.F);
    }

    struct FormatCase {
        std::string_view name;
        ImageFormat format;
        std::string_view fragment_shader;
        size_t bytes_per_pixel;
    };

    class TextureBufferFormatTest : public ::testing::TestWithParam<FormatCase> { };

    TEST_P(TextureBufferFormatTest, FormatSelectsTheFragmentShaderAndPixelSize)
    {
        const auto& c = GetParam();
        std::vector<uint8_t> pixels(size_t { 5 } * 2 * c.bytes_per_pixel, 0x5A);
        const TextureBuffer buffer(5, 2, c.format, pixels.data());

        const auto config = buffer.get_render_config();
        EXPECT_EQ(config.vertex_shader, "texture.vert.spv");
        EXPECT_EQ(config.fragment_shader, std::string(c.fragment_shader));
        EXPECT_EQ(config.default_texture_binding, "texSampler");
        EXPECT_EQ(config.topology, Portal::Graphics::PrimitiveTopology::TRIANGLE_STRIP);
        EXPECT_EQ(buffer.get_pixel_data().size(), pixels.size());
    }

    INSTANTIATE_TEST_SUITE_P(
        Formats, TextureBufferFormatTest,
        ::testing::Values(
            FormatCase { "Rgba8", ImageFormat::RGBA8, "texture.frag.spv", 4 },
            FormatCase { "Gray8", ImageFormat::R8, "texture_gray.frag.spv", 1 },
            FormatCase { "GrayAlpha8", ImageFormat::RG8, "texture_gray_alpha.frag.spv", 2 }),
        [](const ::testing::TestParamInfo<FormatCase>& info) {
            return std::string(info.param.name);
        });

    // ------------------------------------------------------------------
    // Pixel data
    // ------------------------------------------------------------------

    class TextureBufferPixelTest : public ::testing::Test {
    protected:
        TextureBuffer buffer { 4, 4, ImageFormat::RGBA8 };
    };

    TEST_F(TextureBufferPixelTest, SetPixelDataReplacesTheStoreAndMarksItDirty)
    {
        buffer.clear_dirty_flag();
        const std::vector<uint8_t> pixels(64, 0xAB);

        buffer.set_pixel_data(pixels.data(), pixels.size());

        EXPECT_EQ(buffer.get_pixel_data(), pixels);
        EXPECT_TRUE(buffer.is_texture_dirty());
    }

    TEST_F(TextureBufferPixelTest, SetPixelDataResizesToTheSuppliedLength)
    {
        const std::vector<uint8_t> big(64, 1);
        const std::vector<uint8_t> small(16, 2);

        buffer.set_pixel_data(big.data(), big.size());
        buffer.set_pixel_data(small.data(), small.size());

        EXPECT_EQ(buffer.get_pixel_data(), small);
    }

    TEST_F(TextureBufferPixelTest, NullOrEmptyPixelDataIsIgnored)
    {
        const std::vector<uint8_t> pixels(64, 9);
        buffer.set_pixel_data(pixels.data(), pixels.size());
        buffer.clear_dirty_flag();

        buffer.set_pixel_data(nullptr, 64);
        buffer.set_pixel_data(pixels.data(), 0);

        EXPECT_EQ(buffer.get_pixel_data(), pixels);
        EXPECT_FALSE(buffer.is_texture_dirty());
    }

    TEST_F(TextureBufferPixelTest, DirtyFlagCanBeMarkedAndCleared)
    {
        buffer.clear_dirty_flag();
        EXPECT_FALSE(buffer.is_texture_dirty());

        buffer.mark_pixels_dirty();
        EXPECT_TRUE(buffer.is_texture_dirty());

        buffer.clear_dirty_flag();
        buffer.mark_texture_dirty();
        EXPECT_TRUE(buffer.is_texture_dirty());
    }

    TEST_F(TextureBufferPixelTest, MutablePixelAccessAliasesTheStoredData)
    {
        auto& bytes = buffer.get_pixel_data_mutable();
        bytes.assign(8, 0x11);
        EXPECT_EQ(buffer.get_pixel_data().size(), 8U);
        EXPECT_EQ(buffer.get_pixel_data().front(), 0x11);
    }

    // ------------------------------------------------------------------
    // Resize
    // ------------------------------------------------------------------

    TEST_F(TextureBufferPixelTest, ResizeToTheSameExtentChangesNothing)
    {
        buffer.clear_dirty_flag();
        buffer.resize_texture(4, 4);
        EXPECT_FALSE(buffer.is_texture_dirty());
        EXPECT_EQ(buffer.get_width(), 4U);
        EXPECT_EQ(buffer.get_height(), 4U);
    }

    TEST_F(TextureBufferPixelTest, ResizeToANewExtentUpdatesDimensionsAndMarksDirty)
    {
        buffer.clear_dirty_flag();
        buffer.resize_texture(16, 9);
        EXPECT_EQ(buffer.get_width(), 16U);
        EXPECT_EQ(buffer.get_height(), 9U);
        EXPECT_TRUE(buffer.is_texture_dirty());
        EXPECT_FALSE(buffer.has_texture());
    }

    // ------------------------------------------------------------------
    // Display transform
    // ------------------------------------------------------------------

    TEST_F(TextureBufferPixelTest, TransformSettersRoundTripThroughTheGetters)
    {
        buffer.set_position(0.25F, -0.5F);
        buffer.set_scale(2.F, 0.5F);
        buffer.set_rotation(1.25F);

        EXPECT_EQ(buffer.get_position(), glm::vec2(0.25F, -0.5F));
        EXPECT_EQ(buffer.get_scale(), glm::vec2(2.F, 0.5F));
        EXPECT_FLOAT_EQ(buffer.get_rotation(), 1.25F);
    }

} // namespace
} // namespace MayaFlux::Test
