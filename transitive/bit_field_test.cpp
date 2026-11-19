#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Transitive/Protocol/BitField.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Transitive::Protocol::load_bits_le;
    using Transitive::Protocol::load_signed_bits_le;
    using Transitive::Protocol::sign_extend;

    using Bytes = std::vector<uint8_t>;

    TEST(BitFieldTest, ByteAlignedFieldReadsTheWholeByte)
    {
        const Bytes bytes { 0x12, 0xAB, 0xCD };

        EXPECT_EQ(load_bits_le(bytes, 0, 8), 0x12U);
        EXPECT_EQ(load_bits_le(bytes, 8, 8), 0xABU);
        EXPECT_EQ(load_bits_le(bytes, 16, 8), 0xCDU);
    }

    TEST(BitFieldTest, FieldsPackFromTheLowBitOfEachByte)
    {
        const Bytes bytes { 0b1010'0110 };

        EXPECT_EQ(load_bits_le(bytes, 0, 1), 0U);
        EXPECT_EQ(load_bits_le(bytes, 1, 1), 1U);
        EXPECT_EQ(load_bits_le(bytes, 2, 1), 1U);
        EXPECT_EQ(load_bits_le(bytes, 3, 1), 0U);
        EXPECT_EQ(load_bits_le(bytes, 0, 4), 0b0110U);
        EXPECT_EQ(load_bits_le(bytes, 4, 4), 0b1010U);
    }

    TEST(BitFieldTest, FieldCrossingAByteBoundaryTakesTheLowByteFirst)
    {
        const Bytes bytes { 0xF0, 0x0F };

        EXPECT_EQ(load_bits_le(bytes, 4, 8), 0xFFU);
        EXPECT_EQ(load_bits_le(bytes, 0, 16), 0x0FF0U);
    }

    TEST(BitFieldTest, TwelveBitAxisAtANibbleOffsetIsAssembledLittleEndian)
    {
        const Bytes bytes { 0x0A, 0xBC, 0xDE };

        EXPECT_EQ(load_bits_le(bytes, 4, 12), 0xBC0U);
        EXPECT_EQ(load_bits_le(bytes, 0, 12), 0xC0AU);
    }

    TEST(BitFieldTest, ThirtyTwoBitFieldMatchesLittleEndianWord)
    {
        const Bytes bytes { 0x04, 0x03, 0x02, 0x01 };

        EXPECT_EQ(load_bits_le(bytes, 0, 32), 0x01020304U);
    }

    TEST(BitFieldTest, SixtyFourBitFieldMatchesLittleEndianWord)
    {
        const Bytes bytes { 0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01 };

        EXPECT_EQ(load_bits_le(bytes, 0, 64), 0x0123456789ABCDEFULL);
    }

    TEST(BitFieldTest, WidthAboveSixtyFourIsClamped)
    {
        const Bytes bytes(12, 0xFF);

        EXPECT_EQ(load_bits_le(bytes, 0, 96), ~uint64_t { 0 });
    }

    TEST(BitFieldTest, ZeroWidthReadsZero)
    {
        const Bytes bytes { 0xFF };

        EXPECT_EQ(load_bits_le(bytes, 0, 0), 0U);
        EXPECT_EQ(load_bits_le(bytes, 3, 0), 0U);
    }

    TEST(BitFieldTest, BitsPastTheEndOfTheBufferReadAsZero)
    {
        const Bytes bytes { 0xFF };

        EXPECT_EQ(load_bits_le(bytes, 4, 8), 0x0FU);
        EXPECT_EQ(load_bits_le(bytes, 8, 8), 0U);
        EXPECT_EQ(load_bits_le(bytes, 64, 8), 0U);
    }

    TEST(BitFieldTest, EmptyBufferReadsZero)
    {
        EXPECT_EQ(load_bits_le({}, 0, 8), 0U);
        EXPECT_EQ(load_signed_bits_le({}, 0, 8), 0);
    }

    TEST(BitFieldTest, SignExtendTreatsTheTopFieldBitAsTheSign)
    {
        EXPECT_EQ(sign_extend(0xFF, 8), -1);
        EXPECT_EQ(sign_extend(0x80, 8), -128);
        EXPECT_EQ(sign_extend(0x7F, 8), 127);
        EXPECT_EQ(sign_extend(0x00, 8), 0);
        EXPECT_EQ(sign_extend(0x8, 4), -8);
        EXPECT_EQ(sign_extend(0x7, 4), 7);
        EXPECT_EQ(sign_extend(0x1, 1), -1);
        EXPECT_EQ(sign_extend(0x0, 1), 0);
        EXPECT_EQ(sign_extend(0x800, 12), -2048);
        EXPECT_EQ(sign_extend(0xFFFF, 16), -1);
        EXPECT_EQ(sign_extend(0x8000, 16), -32768);
    }

    TEST(BitFieldTest, SignExtendIgnoresBitsAboveTheWidth)
    {
        EXPECT_EQ(sign_extend(0xFFFFFF7F, 8), 127);
        EXPECT_EQ(sign_extend(0x100, 8), 0);
        EXPECT_EQ(sign_extend(0xABCD'F0, 4), 0);
    }

    TEST(BitFieldTest, SignExtendEdgeWidths)
    {
        EXPECT_EQ(sign_extend(0xFF, 0), 0);
        EXPECT_EQ(sign_extend(0xFFFFFFFFULL, 32), -1);
        EXPECT_EQ(sign_extend(0x80000000ULL, 32), -2147483648LL);
        EXPECT_EQ(sign_extend(~uint64_t { 0 }, 64), -1);
        EXPECT_EQ(sign_extend(~uint64_t { 0 }, 80), -1);
        EXPECT_EQ(sign_extend(uint64_t { 1 } << 63, 64), std::numeric_limits<int64_t>::min());
    }

    TEST(BitFieldTest, SignedReadMatchesAnEightBitTiltValue)
    {
        const Bytes bytes { 0x81, 0x7F, 0x00 };

        EXPECT_EQ(load_signed_bits_le(bytes, 0, 8), -127);
        EXPECT_EQ(load_signed_bits_le(bytes, 8, 8), 127);
        EXPECT_EQ(load_signed_bits_le(bytes, 16, 8), 0);
    }

    TEST(BitFieldTest, SignedReadOfATwelveBitFieldCrossingAByteBoundary)
    {
        const Bytes bytes { 0xF0, 0xFF };

        EXPECT_EQ(load_signed_bits_le(bytes, 4, 12), -1);
        EXPECT_EQ(load_signed_bits_le(bytes, 5, 12), 0x7FF);
    }

    TEST(BitFieldTest, SignedReadOfASingleBitIsMinusOneWhenSet)
    {
        const Bytes bytes { 0x02 };

        EXPECT_EQ(load_signed_bits_le(bytes, 0, 1), 0);
        EXPECT_EQ(load_signed_bits_le(bytes, 1, 1), -1);
    }

    TEST(BitFieldTest, UnsignedReadOfTheSameBitsStaysPositive)
    {
        const Bytes bytes { 0xFF };

        EXPECT_EQ(load_bits_le(bytes, 0, 8), 0xFFU);
        EXPECT_EQ(load_signed_bits_le(bytes, 0, 8), -1);
    }

    TEST(BitFieldTest, ReadsAreUsableInConstantExpressions)
    {
        constexpr std::array<uint8_t, 2> bytes { 0xF0, 0xFF };

        static_assert(load_bits_le(bytes, 4, 12) == 0xFFFU);
        static_assert(load_signed_bits_le(bytes, 4, 12) == -1);
        static_assert(sign_extend(0x80, 8) == -128);

        SUCCEED();
    }

} // namespace
} // namespace MayaFlux::Test
