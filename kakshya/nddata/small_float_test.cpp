#include "gtest/gtest.h"

#include "MayaFlux/Kakshya/NDData/SmallFloat.hpp"

namespace MayaFlux::Test {
namespace {

    using Kakshya::decode_small_float;
    using Kakshya::encode_small_float;
    using Kakshya::float_to_half;
    using Kakshya::half_to_float;

    bool half_is_nan(uint16_t bits)
    {
        return (bits & 0x7C00U) == 0x7C00U && (bits & 0x03FFU) != 0U;
    }

    float float_from_bits(uint32_t bits)
    {
        float value {};
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    TEST(SmallFloatTest, EveryHalfPatternDecodesLikeTheGenericDecoder)
    {
        for (uint32_t pattern = 0; pattern <= 0xFFFFU; ++pattern) {
            const float fast = half_to_float(static_cast<uint16_t>(pattern));
            const double generic = decode_small_float(pattern, 5, 10);
            if (half_is_nan(static_cast<uint16_t>(pattern))) {
                ASSERT_TRUE(std::isnan(fast)) << pattern;
                ASSERT_TRUE(std::isnan(generic)) << pattern;
                continue;
            }
            ASSERT_EQ(static_cast<double>(fast), generic) << pattern;
            ASSERT_EQ(std::signbit(fast), std::signbit(generic)) << pattern;
        }
    }

    TEST(SmallFloatTest, EveryHalfPatternRoundTripsThroughFloat)
    {
        for (uint32_t pattern = 0; pattern <= 0xFFFFU; ++pattern) {
            const auto half = static_cast<uint16_t>(pattern);
            const uint16_t back = float_to_half(half_to_float(half));
            if (half_is_nan(half)) {
                ASSERT_TRUE(half_is_nan(back)) << pattern;
                continue;
            }
            ASSERT_EQ(back, half) << pattern;
        }
    }

    TEST(SmallFloatTest, TheFastEncoderMatchesTheGenericEncoderAcrossTheFloatRange)
    {
        const auto agree = [](float value) {
            const uint16_t fast = float_to_half(value);
            const auto generic = static_cast<uint16_t>(encode_small_float(static_cast<double>(value), 5, 10));
            if (std::isnan(value))
                return half_is_nan(fast) && half_is_nan(generic);
            return fast == generic;
        };

        for (uint64_t pattern = 0; pattern <= 0xFFFFFFFFULL; pattern += 4093) {
            const float value = float_from_bits(static_cast<uint32_t>(pattern));
            ASSERT_TRUE(agree(value)) << pattern;
        }

        const std::vector<float> edges {
            0.0F, -0.0F, 1.0F, -1.0F, 65504.0F, 65519.0F, 65520.0F, -65520.0F, 1e-8F, 5.9604645e-8F,
            2.98023224e-8F, 2.98023261e-8F, 6.1035156e-5F, 6.09755516e-5F,
            1.0F + std::ldexp(1.0F, -11), 1.0F + 3.0F * std::ldexp(1.0F, -11),
            std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
            std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::denorm_min(),
            std::numeric_limits<float>::max(), std::numeric_limits<float>::min()
        };
        for (const float value : edges)
            EXPECT_TRUE(agree(value)) << value;
    }

    TEST(SmallFloatTest, KnownValuesEncodeToTheirBitPatterns)
    {
        EXPECT_EQ(float_to_half(0.0F), 0x0000U);
        EXPECT_EQ(float_to_half(-0.0F), 0x8000U);
        EXPECT_EQ(float_to_half(1.0F), 0x3C00U);
        EXPECT_EQ(float_to_half(-2.0F), 0xC000U);
        EXPECT_EQ(float_to_half(65504.0F), 0x7BFFU);
        EXPECT_EQ(float_to_half(65520.0F), 0x7C00U);
        EXPECT_EQ(float_to_half(1.0F + std::ldexp(1.0F, -11)), 0x3C00U);
        EXPECT_EQ(float_to_half(1.0F + 3.0F * std::ldexp(1.0F, -11)), 0x3C02U);
        EXPECT_EQ(float_to_half(std::ldexp(1.0F, -24)), 0x0001U);
        EXPECT_EQ(float_to_half(std::ldexp(1.0F, -25)), 0x0000U);
        EXPECT_EQ(float_to_half(std::numeric_limits<float>::infinity()), 0x7C00U);
        EXPECT_TRUE(half_is_nan(float_to_half(std::numeric_limits<float>::quiet_NaN())));
    }

    TEST(SmallFloatTest, TheGenericCodecServesOtherFieldWidths)
    {
        EXPECT_EQ(encode_small_float(1.0, 8, 7), 0x3F80U);
        EXPECT_EQ(encode_small_float(-2.0, 8, 7), 0xC000U);
        EXPECT_EQ(encode_small_float(3.14159265358979323846, 8, 7), 0x4049U);
        EXPECT_EQ(decode_small_float(0x4040U, 8, 7), 3.0);
        EXPECT_EQ(decode_small_float(0x7F80U, 8, 7), std::numeric_limits<double>::infinity());
        EXPECT_TRUE(std::isnan(decode_small_float(0x7FC0U, 8, 7)));
    }

}
}
