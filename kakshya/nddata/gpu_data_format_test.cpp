#include "gtest/gtest.h"

#include <string_view>

#include "MayaFlux/Kakshya/NDData/NDData.hpp"

namespace MayaFlux::Test {
namespace {

    using Kakshya::GpuDataFormat;
    using Kakshya::GpuFormatElement;
    using Kakshya::gpu_data_format_align;
    using Kakshya::gpu_data_format_bytes;
    using Kakshya::gpu_data_format_components;
    using Kakshya::gpu_data_format_v;
    using Kakshya::pack_gpu_components;
    using Kakshya::pack_gpu_value;

    struct FormatCase {
        std::string_view name;
        GpuDataFormat format;
        size_t bytes;
        size_t align;
        size_t components;
    };

    std::vector<FormatCase> format_cases()
    {
        return {
            { "FLOAT32", GpuDataFormat::FLOAT32, 4, 4, 1 },
            { "VEC2_F32", GpuDataFormat::VEC2_F32, 8, 8, 2 },
            { "VEC3_F32", GpuDataFormat::VEC3_F32, 12, 16, 3 },
            { "VEC4_F32", GpuDataFormat::VEC4_F32, 16, 16, 4 },
            { "FLOAT64", GpuDataFormat::FLOAT64, 8, 8, 1 },
            { "VEC2_F64", GpuDataFormat::VEC2_F64, 16, 16, 2 },
            { "VEC3_F64", GpuDataFormat::VEC3_F64, 24, 32, 3 },
            { "VEC4_F64", GpuDataFormat::VEC4_F64, 32, 32, 4 },
            { "INT32", GpuDataFormat::INT32, 4, 4, 1 },
            { "UINT32", GpuDataFormat::UINT32, 4, 4, 1 },
            { "UINT8", GpuDataFormat::UINT8, 1, 1, 1 },
            { "UINT16", GpuDataFormat::UINT16, 2, 2, 1 },
            { "MAT4_F32", GpuDataFormat::MAT4_F32, 64, 16, 16 }
        };
    }

    std::string format_name(const ::testing::TestParamInfo<FormatCase>& info)
    {
        return std::string(info.param.name);
    }

    template <typename T>
    T read_as(const std::vector<std::byte>& bytes, size_t index = 0)
    {
        T value {};
        std::memcpy(&value, bytes.data() + index * sizeof(T), sizeof(T));
        return value;
    }

    class GpuDataFormatTableTest : public ::testing::TestWithParam<FormatCase> { };

    TEST_P(GpuDataFormatTableTest, ByteSizeMatchesTheFormat)
    {
        EXPECT_EQ(gpu_data_format_bytes(GetParam().format), GetParam().bytes);
    }

    TEST_P(GpuDataFormatTableTest, AlignmentFollowsStd430)
    {
        EXPECT_EQ(gpu_data_format_align(GetParam().format), GetParam().align);
    }

    TEST_P(GpuDataFormatTableTest, ComponentCountMatchesTheFormat)
    {
        EXPECT_EQ(gpu_data_format_components(GetParam().format), GetParam().components);
    }

    TEST_P(GpuDataFormatTableTest, AlignmentIsAPowerOfTwoNoSmallerThanTheLargestComponent)
    {
        const size_t align = gpu_data_format_align(GetParam().format);

        EXPECT_EQ(align & (align - 1), 0U);
        EXPECT_GE(align, gpu_data_format_bytes(GetParam().format) / gpu_data_format_components(GetParam().format));
    }

    TEST_P(GpuDataFormatTableTest, PacksExactlyTheComponentsItDeclares)
    {
        const std::vector<double> components(GetParam().components, 1.0);
        std::vector<std::byte> out(GetParam().bytes);

        EXPECT_TRUE(pack_gpu_components(GetParam().format, components, out));
    }

    TEST_P(GpuDataFormatTableTest, RefusesTooFewComponents)
    {
        const std::vector<double> components(GetParam().components - 1, 1.0);
        std::vector<std::byte> out(GetParam().bytes);

        EXPECT_FALSE(pack_gpu_components(GetParam().format, components, out));
    }

    TEST_P(GpuDataFormatTableTest, RefusesTooManyComponents)
    {
        const std::vector<double> components(GetParam().components + 1, 1.0);
        std::vector<std::byte> out(GetParam().bytes);

        EXPECT_FALSE(pack_gpu_components(GetParam().format, components, out));
    }

    TEST_P(GpuDataFormatTableTest, RefusesAnUndersizedDestinationAndLeavesItUntouched)
    {
        const std::vector<double> components(GetParam().components, 1.0);
        std::vector<std::byte> out(GetParam().bytes - 1, std::byte { 0xAB });

        EXPECT_FALSE(pack_gpu_components(GetParam().format, components, out));
        EXPECT_TRUE(std::ranges::all_of(out, [](std::byte b) { return b == std::byte { 0xAB }; }));
    }

    INSTANTIATE_TEST_SUITE_P(Formats, GpuDataFormatTableTest,
        ::testing::ValuesIn(format_cases()), format_name);

    TEST(GpuDataFormatTest, UnrecognisedFormatsHaveNoSizeAlignmentOrComponents)
    {
        const auto bogus = static_cast<GpuDataFormat>(200);

        EXPECT_EQ(gpu_data_format_bytes(bogus), 0U);
        EXPECT_EQ(gpu_data_format_align(bogus), 0U);
        EXPECT_EQ(gpu_data_format_components(bogus), 0U);
    }

    TEST(GpuDataFormatTest, UnrecognisedFormatsRefuseToPack)
    {
        const std::vector<double> components { 1.0 };
        std::vector<std::byte> out(64);

        EXPECT_FALSE(pack_gpu_components(static_cast<GpuDataFormat>(200), components, out));
    }

    template <typename T>
    class GpuDataFormatTraitTest : public ::testing::Test { };

    using TraitTypes = ::testing::Types<float, double, int32_t, uint32_t, uint8_t, uint16_t,
        glm::vec2, glm::vec3, glm::vec4, glm::dvec2, glm::dvec3, glm::dvec4, glm::mat4>;
    TYPED_TEST_SUITE(GpuDataFormatTraitTest, TraitTypes);

    TYPED_TEST(GpuDataFormatTraitTest, IsAFormatElement)
    {
        EXPECT_TRUE(GpuFormatElement<TypeParam>);
    }

    TYPED_TEST(GpuDataFormatTraitTest, FormatByteSizeEqualsTheTypeSize)
    {
        EXPECT_EQ(gpu_data_format_bytes(gpu_data_format_v<TypeParam>), sizeof(TypeParam));
    }

    TYPED_TEST(GpuDataFormatTraitTest, FormatComponentsEqualTheTypeComponents)
    {
        if constexpr (ArithmeticData<TypeParam>) {
            EXPECT_EQ(gpu_data_format_components(gpu_data_format_v<TypeParam>), 1U);
        } else {
            EXPECT_EQ(gpu_data_format_components(gpu_data_format_v<TypeParam>), glm_component_count<TypeParam>());
        }
    }

    TYPED_TEST(GpuDataFormatTraitTest, CvRefQualifiedTypesMapToTheSameFormat)
    {
        EXPECT_EQ(gpu_data_format_v<const TypeParam&>, gpu_data_format_v<TypeParam>);
    }

    TEST(GpuDataFormatTest, TypesWithoutAFormatAreNotFormatElements)
    {
        EXPECT_FALSE(GpuFormatElement<int64_t>);
        EXPECT_FALSE(GpuFormatElement<bool>);
        EXPECT_FALSE(GpuFormatElement<glm::mat3>);
        EXPECT_FALSE(GpuFormatElement<glm::dmat4>);
        EXPECT_FALSE(GpuFormatElement<std::string>);
        EXPECT_FALSE(GpuFormatElement<std::complex<float>>);
    }

    TEST(GpuDataFormatTest, FormatElementsExtendBeyondWhatADataVariantHolds)
    {
        EXPECT_TRUE(GpuFormatElement<int32_t>);
        EXPECT_FALSE(Kakshya::DataVariantElement<int32_t>);
        EXPECT_TRUE(GpuFormatElement<glm::dvec3>);
        EXPECT_FALSE(Kakshya::DataVariantElement<glm::dvec3>);
    }

    TEST(GpuDataFormatTest, EveryDataVariantElementExceptComplexIsAFormatElement)
    {
        EXPECT_TRUE(GpuFormatElement<double>);
        EXPECT_TRUE(GpuFormatElement<float>);
        EXPECT_TRUE(GpuFormatElement<uint8_t>);
        EXPECT_TRUE(GpuFormatElement<uint16_t>);
        EXPECT_TRUE(GpuFormatElement<uint32_t>);
        EXPECT_TRUE(GpuFormatElement<glm::vec2>);
        EXPECT_TRUE(GpuFormatElement<glm::vec3>);
        EXPECT_TRUE(GpuFormatElement<glm::vec4>);
        EXPECT_TRUE(GpuFormatElement<glm::mat4>);
    }

    TEST(GpuDataFormatPackTest, FloatIntoFloat32IsBitExact)
    {
        std::vector<std::byte> out(4);

        ASSERT_TRUE(pack_gpu_value(GpuDataFormat::FLOAT32, 0.15625F, out));
        EXPECT_EQ(read_as<float>(out), 0.15625F);
    }

    TEST(GpuDataFormatPackTest, DoubleIntoFloat32Narrows)
    {
        std::vector<std::byte> out(4);

        ASSERT_TRUE(pack_gpu_value(GpuDataFormat::FLOAT32, 0.1, out));
        EXPECT_EQ(read_as<float>(out), static_cast<float>(0.1));
    }

    TEST(GpuDataFormatPackTest, DoubleIntoFloat64IsExact)
    {
        std::vector<std::byte> out(8);

        ASSERT_TRUE(pack_gpu_value(GpuDataFormat::FLOAT64, 0.1, out));
        EXPECT_EQ(read_as<double>(out), 0.1);
    }

    TEST(GpuDataFormatPackTest, FloatIntoFloat64Widens)
    {
        std::vector<std::byte> out(8);

        ASSERT_TRUE(pack_gpu_value(GpuDataFormat::FLOAT64, 0.5F, out));
        EXPECT_EQ(read_as<double>(out), 0.5);
    }

    TEST(GpuDataFormatPackTest, IntegerIntoFloatConverts)
    {
        std::vector<std::byte> out(4);

        ASSERT_TRUE(pack_gpu_value(GpuDataFormat::FLOAT32, int32_t { -3 }, out));
        EXPECT_EQ(read_as<float>(out), -3.0F);
    }

    TEST(GpuDataFormatPackTest, FloatIntoIntegerTruncatesTowardZero)
    {
        std::vector<std::byte> out(4);

        ASSERT_TRUE(pack_gpu_value(GpuDataFormat::INT32, 2.9F, out));
        EXPECT_EQ(read_as<int32_t>(out), 2);

        ASSERT_TRUE(pack_gpu_value(GpuDataFormat::INT32, -2.9F, out));
        EXPECT_EQ(read_as<int32_t>(out), -2);
    }

    TEST(GpuDataFormatPackTest, Vec3IntoVec3F32IsComponentExact)
    {
        std::vector<std::byte> out(12);
        const glm::vec3 value { 1.0F, -2.0F, 3.5F };

        ASSERT_TRUE(pack_gpu_value(GpuDataFormat::VEC3_F32, value, out));
        EXPECT_EQ(read_as<float>(out, 0), 1.0F);
        EXPECT_EQ(read_as<float>(out, 1), -2.0F);
        EXPECT_EQ(read_as<float>(out, 2), 3.5F);
    }

    TEST(GpuDataFormatPackTest, Vec3IntoVec3F64Widens)
    {
        std::vector<std::byte> out(24);
        const glm::vec3 value { 1.0F, -2.0F, 3.5F };

        ASSERT_TRUE(pack_gpu_value(GpuDataFormat::VEC3_F64, value, out));
        EXPECT_EQ(read_as<double>(out, 0), 1.0);
        EXPECT_EQ(read_as<double>(out, 1), -2.0);
        EXPECT_EQ(read_as<double>(out, 2), 3.5);
    }

    TEST(GpuDataFormatPackTest, Dvec2IntoVec2F32Narrows)
    {
        std::vector<std::byte> out(8);
        const glm::dvec2 value { 0.25, -0.75 };

        ASSERT_TRUE(pack_gpu_value(GpuDataFormat::VEC2_F32, value, out));
        EXPECT_EQ(read_as<float>(out, 0), 0.25F);
        EXPECT_EQ(read_as<float>(out, 1), -0.75F);
    }

    TEST(GpuDataFormatPackTest, VectorsDoNotPackIntoADifferentComponentCount)
    {
        std::vector<std::byte> out(64, std::byte { 0xAB });

        EXPECT_FALSE(pack_gpu_value(GpuDataFormat::FLOAT32, glm::vec3 { 1.0F }, out));
        EXPECT_FALSE(pack_gpu_value(GpuDataFormat::VEC4_F32, glm::vec3 { 1.0F }, out));
        EXPECT_FALSE(pack_gpu_value(GpuDataFormat::VEC3_F32, 1.0F, out));
        EXPECT_TRUE(std::ranges::all_of(out, [](std::byte b) { return b == std::byte { 0xAB }; }));
    }

    TEST(GpuDataFormatPackTest, Mat4WritesColumnMajor)
    {
        std::vector<std::byte> out(64);
        glm::mat4 value(1.0F);
        value[2][1] = 5.0F;
        value[0][3] = 9.0F;

        ASSERT_TRUE(pack_gpu_value(GpuDataFormat::MAT4_F32, value, out));
        EXPECT_EQ(read_as<float>(out, 2 * 4 + 1), 5.0F);
        EXPECT_EQ(read_as<float>(out, 0 * 4 + 3), 9.0F);
        EXPECT_EQ(read_as<float>(out, 0), 1.0F);
        EXPECT_EQ(read_as<float>(out, 15), 1.0F);
    }

    TEST(GpuDataFormatPackTest, Mat4DoesNotPackIntoAVectorFormat)
    {
        std::vector<std::byte> out(64);

        EXPECT_FALSE(pack_gpu_value(GpuDataFormat::VEC4_F32, glm::mat4(1.0F), out));
    }

    TEST(GpuDataFormatPackTest, OversizedDestinationsOnlyReceiveTheElementBytes)
    {
        std::vector<std::byte> out(16, std::byte { 0xAB });

        ASSERT_TRUE(pack_gpu_value(GpuDataFormat::FLOAT32, 1.0F, out));
        EXPECT_EQ(read_as<float>(out), 1.0F);
        for (size_t i = 4; i < out.size(); ++i) {
            EXPECT_EQ(out[i], std::byte { 0xAB }) << i;
        }
    }

    TEST(GpuDataFormatPackTest, UnsignedIntegersSaturateAtBothEnds)
    {
        std::vector<std::byte> out(4);

        ASSERT_TRUE(pack_gpu_value(GpuDataFormat::UINT8, 300.0, out));
        EXPECT_EQ(read_as<uint8_t>(out), 255U);

        ASSERT_TRUE(pack_gpu_value(GpuDataFormat::UINT8, -5.0, out));
        EXPECT_EQ(read_as<uint8_t>(out), 0U);

        ASSERT_TRUE(pack_gpu_value(GpuDataFormat::UINT16, 70000.0, out));
        EXPECT_EQ(read_as<uint16_t>(out), 65535U);

        ASSERT_TRUE(pack_gpu_value(GpuDataFormat::UINT32, -1.0, out));
        EXPECT_EQ(read_as<uint32_t>(out), 0U);

        ASSERT_TRUE(pack_gpu_value(GpuDataFormat::UINT32, 1.0e12, out));
        EXPECT_EQ(read_as<uint32_t>(out), std::numeric_limits<uint32_t>::max());
    }

    TEST(GpuDataFormatPackTest, SignedIntegersSaturateAtBothEnds)
    {
        std::vector<std::byte> out(4);

        ASSERT_TRUE(pack_gpu_value(GpuDataFormat::INT32, 1.0e12, out));
        EXPECT_EQ(read_as<int32_t>(out), std::numeric_limits<int32_t>::max());

        ASSERT_TRUE(pack_gpu_value(GpuDataFormat::INT32, -1.0e12, out));
        EXPECT_EQ(read_as<int32_t>(out), std::numeric_limits<int32_t>::lowest());
    }

    TEST(GpuDataFormatPackTest, NotANumberPacksAsZeroInIntegerFormats)
    {
        std::vector<std::byte> out(4, std::byte { 0xAB });
        const double nan = std::numeric_limits<double>::quiet_NaN();

        ASSERT_TRUE(pack_gpu_value(GpuDataFormat::INT32, nan, out));
        EXPECT_EQ(read_as<int32_t>(out), 0);

        ASSERT_TRUE(pack_gpu_value(GpuDataFormat::UINT32, nan, out));
        EXPECT_EQ(read_as<uint32_t>(out), 0U);
    }

    TEST(GpuDataFormatPackTest, NotANumberSurvivesInFloatFormats)
    {
        std::vector<std::byte> out(4);

        ASSERT_TRUE(pack_gpu_value(GpuDataFormat::FLOAT32, std::numeric_limits<double>::quiet_NaN(), out));
        EXPECT_TRUE(std::isnan(read_as<float>(out)));
    }

    TEST(GpuDataFormatPackTest, ComponentsPackInElementOrder)
    {
        std::vector<std::byte> out(16);
        const std::array<double, 4> components { 4.0, 3.0, 2.0, 1.0 };

        ASSERT_TRUE(pack_gpu_components(GpuDataFormat::VEC4_F32, components, out));
        EXPECT_EQ(read_as<float>(out, 0), 4.0F);
        EXPECT_EQ(read_as<float>(out, 3), 1.0F);
    }

} // namespace
} // namespace MayaFlux::Test
