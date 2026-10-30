#include "gtest/gtest.h"

#include "MayaFlux/Kakshya/NDData/TextureAccess.hpp"

namespace MayaFlux::Test {
namespace {

    using Kakshya::DataVariant;
    using Kakshya::GpuDataFormat;
    using Kakshya::TextureAccess;

    TEST(TextureAccessTest, DefaultAccessIsAnEmptyVec4Float)
    {
        const TextureAccess access;

        EXPECT_EQ(access.data_ptr, nullptr);
        EXPECT_EQ(access.byte_count, 0U);
        EXPECT_EQ(access.format, GpuDataFormat::VEC4_F32);
        EXPECT_EQ(access.bytes_per_texel(), 16U);
        EXPECT_FALSE(access.was_promoted());
    }

    struct FormatCase {
        GpuDataFormat format;
        uint32_t channels;
        size_t bytes;
    };

    class TextureAccessFormatTest : public ::testing::TestWithParam<FormatCase> { };

    TEST_P(TextureAccessFormatTest, ChannelCountMatchesTheFormat)
    {
        TextureAccess access;
        access.format = GetParam().format;

        EXPECT_EQ(access.channel_count(), GetParam().channels);
    }

    TEST_P(TextureAccessFormatTest, BytesPerTexelMatchesTheFormat)
    {
        TextureAccess access;
        access.format = GetParam().format;

        EXPECT_EQ(access.bytes_per_texel(), GetParam().bytes);
    }

    INSTANTIATE_TEST_SUITE_P(Formats, TextureAccessFormatTest,
        ::testing::Values(
            FormatCase { GpuDataFormat::UINT8, 1, 1 },
            FormatCase { GpuDataFormat::UINT16, 1, 2 },
            FormatCase { GpuDataFormat::UINT32, 1, 4 },
            FormatCase { GpuDataFormat::INT32, 1, 4 },
            FormatCase { GpuDataFormat::FLOAT32, 1, 4 },
            FormatCase { GpuDataFormat::FLOAT64, 1, 8 },
            FormatCase { GpuDataFormat::VEC2_F32, 2, 8 },
            FormatCase { GpuDataFormat::VEC2_F64, 2, 16 },
            FormatCase { GpuDataFormat::VEC3_F32, 3, 12 },
            FormatCase { GpuDataFormat::VEC3_F64, 3, 24 },
            FormatCase { GpuDataFormat::VEC4_F32, 4, 16 },
            FormatCase { GpuDataFormat::VEC4_F64, 4, 32 }));

    template <typename T>
    void expect_zero_copy(std::vector<T> source, GpuDataFormat format)
    {
        const DataVariant variant = std::move(source);
        const auto& stored = std::get<std::vector<T>>(variant);

        const auto access = Kakshya::as_texture_access(variant);

        ASSERT_TRUE(access.has_value());
        EXPECT_EQ(access->format, format);
        EXPECT_EQ(access->data_ptr, static_cast<const void*>(stored.data()));
        EXPECT_EQ(access->byte_count, stored.size() * sizeof(T));
        EXPECT_FALSE(access->was_promoted());
        EXPECT_EQ(access->byte_count, stored.size() * access->bytes_per_texel());
    }

    TEST(AsTextureAccessTest, BytesMapDirectly)
    {
        expect_zero_copy<uint8_t>({ 1, 2, 3, 4 }, GpuDataFormat::UINT8);
    }

    TEST(AsTextureAccessTest, HalfWordsMapDirectly)
    {
        expect_zero_copy<uint16_t>({ 1, 2, 3 }, GpuDataFormat::UINT16);
    }

    TEST(AsTextureAccessTest, WordsMapDirectly)
    {
        expect_zero_copy<uint32_t>({ 1, 2, 3 }, GpuDataFormat::UINT32);
    }

    TEST(AsTextureAccessTest, FloatsMapDirectly)
    {
        expect_zero_copy<float>({ 0.5F, 1.5F }, GpuDataFormat::FLOAT32);
    }

    TEST(AsTextureAccessTest, Vec2MapsDirectly)
    {
        expect_zero_copy<glm::vec2>({ { 1, 2 }, { 3, 4 } }, GpuDataFormat::VEC2_F32);
    }

    TEST(AsTextureAccessTest, Vec4MapsDirectly)
    {
        expect_zero_copy<glm::vec4>({ { 1, 2, 3, 4 } }, GpuDataFormat::VEC4_F32);
    }

    TEST(AsTextureAccessTest, ComplexFloatMapsToTwoChannelsWithoutCopying)
    {
        expect_zero_copy<std::complex<float>>({ { 1.0F, 2.0F }, { 3.0F, 4.0F } }, GpuDataFormat::VEC2_F32);
    }

    TEST(AsTextureAccessTest, ComplexFloatKeepsRealThenImaginaryTexels)
    {
        const DataVariant variant = std::vector<std::complex<float>> { { 1.0F, 2.0F } };

        const auto access = Kakshya::as_texture_access(variant);

        ASSERT_TRUE(access.has_value());
        float texel[2] {};
        std::memcpy(texel, access->data_ptr, sizeof(texel));
        EXPECT_FLOAT_EQ(texel[0], 1.0F);
        EXPECT_FLOAT_EQ(texel[1], 2.0F);
    }

    TEST(AsTextureAccessTest, DoublesAreNarrowedIntoAConversionBuffer)
    {
        const DataVariant variant = std::vector<double> { 0.5, -1.25, 3.0 };

        const auto access = Kakshya::as_texture_access(variant);

        ASSERT_TRUE(access.has_value());
        EXPECT_EQ(access->format, GpuDataFormat::FLOAT32);
        EXPECT_EQ(access->byte_count, 3U * sizeof(float));
        EXPECT_EQ(access->data_ptr, access->conversion_buffer.data());
        EXPECT_NE(access->data_ptr, static_cast<const void*>(std::get<std::vector<double>>(variant).data()));
        float texels[3] {};
        std::memcpy(texels, access->data_ptr, sizeof(texels));
        EXPECT_FLOAT_EQ(texels[0], 0.5F);
        EXPECT_FLOAT_EQ(texels[1], -1.25F);
        EXPECT_FLOAT_EQ(texels[2], 3.0F);
    }

    TEST(AsTextureAccessTest, Vec3IsPromotedToVec4WithZeroW)
    {
        const DataVariant variant = std::vector<glm::vec3> { { 1, 2, 3 }, { 4, 5, 6 } };

        const auto access = Kakshya::as_texture_access(variant);

        ASSERT_TRUE(access.has_value());
        EXPECT_EQ(access->format, GpuDataFormat::VEC4_F32);
        EXPECT_TRUE(access->was_promoted());
        EXPECT_EQ(access->byte_count, 2U * sizeof(glm::vec4));
        EXPECT_EQ(access->data_ptr, access->conversion_buffer.data());
        glm::vec4 texels[2] {};
        std::memcpy(texels, access->data_ptr, sizeof(texels));
        EXPECT_EQ(texels[0], glm::vec4(1, 2, 3, 0));
        EXPECT_EQ(texels[1], glm::vec4(4, 5, 6, 0));
    }

    TEST(AsTextureAccessTest, PromotedBufferIsOwnedByTheAccess)
    {
        std::optional<TextureAccess> access;
        {
            const DataVariant variant = std::vector<glm::vec3> { { 1, 2, 3 } };
            access = Kakshya::as_texture_access(variant);
        }

        ASSERT_TRUE(access.has_value());
        glm::vec4 texel {};
        std::memcpy(&texel, access->data_ptr, sizeof(texel));
        EXPECT_EQ(texel, glm::vec4(1, 2, 3, 0));
    }

    TEST(AsTextureAccessTest, MovingAnAccessKeepsItsBufferAlive)
    {
        const DataVariant variant = std::vector<double> { 2.0, 4.0 };
        auto first = Kakshya::as_texture_access(variant);
        ASSERT_TRUE(first.has_value());

        const TextureAccess moved = std::move(*first);

        float texels[2] {};
        std::memcpy(texels, moved.conversion_buffer.data(), sizeof(texels));
        EXPECT_FLOAT_EQ(texels[1], 4.0F);
        EXPECT_EQ(moved.byte_count, 2U * sizeof(float));
    }

    TEST(AsTextureAccessTest, DoublePrecisionComplexIsRejected)
    {
        const DataVariant variant = std::vector<std::complex<double>> { { 1.0, 2.0 } };

        EXPECT_FALSE(Kakshya::as_texture_access(variant).has_value());
    }

    TEST(AsTextureAccessTest, MatricesAreRejected)
    {
        const DataVariant variant = std::vector<glm::mat4> { glm::mat4(1.0F) };

        EXPECT_FALSE(Kakshya::as_texture_access(variant).has_value());
    }

    TEST(AsTextureAccessTest, EmptyVariantsAreRejected)
    {
        EXPECT_FALSE(Kakshya::as_texture_access(DataVariant { std::vector<uint8_t> {} }).has_value());
        EXPECT_FALSE(Kakshya::as_texture_access(DataVariant { std::vector<float> {} }).has_value());
        EXPECT_FALSE(Kakshya::as_texture_access(DataVariant { std::vector<double> {} }).has_value());
        EXPECT_FALSE(Kakshya::as_texture_access(DataVariant { std::vector<glm::vec3> {} }).has_value());
        EXPECT_FALSE(Kakshya::as_texture_access(DataVariant { std::vector<glm::vec4> {} }).has_value());
    }

    TEST(AsTextureAccessTest, EveryAcceptedFormatCarriesWholeTexels)
    {
        const std::vector<DataVariant> variants {
            std::vector<uint8_t>(8, 1),
            std::vector<uint16_t>(8, 1),
            std::vector<uint32_t>(8, 1),
            std::vector<float>(8, 1.0F),
            std::vector<double>(8, 1.0),
            std::vector<std::complex<float>>(8, { 1.0F, 1.0F }),
            std::vector<glm::vec2>(8, glm::vec2(1.0F)),
            std::vector<glm::vec3>(8, glm::vec3(1.0F)),
            std::vector<glm::vec4>(8, glm::vec4(1.0F)),
        };

        for (const auto& variant : variants) {
            const auto access = Kakshya::as_texture_access(variant);

            ASSERT_TRUE(access.has_value()) << variant.index();
            EXPECT_EQ(access->byte_count, 8U * access->bytes_per_texel()) << variant.index();
            EXPECT_NE(access->data_ptr, nullptr) << variant.index();
        }
    }

} // namespace
} // namespace MayaFlux::Test
