#include "gtest/gtest.h"

#include "MayaFlux/Transitive/Protocol/BinaryBuffer.hpp"

namespace MayaFlux::Test {
namespace {

    using Bytes8 = std::array<uint8_t, 8>;

    uint64_t bits_of(double value)
    {
        return std::bit_cast<uint64_t>(value);
    }

    TEST(BinaryBufferTest, StoreLeU64WritesTheLowByteFirst)
    {
        Bytes8 bytes {};

        Transitive::Protocol::store_le_u64(bytes.data(), 0x0123456789ABCDEFULL);

        const Bytes8 expected { 0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01 };
        EXPECT_EQ(bytes, expected);
    }

    TEST(BinaryBufferTest, LoadLeU64ReadsTheLowByteFirst)
    {
        const Bytes8 bytes { 0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01 };

        EXPECT_EQ(Transitive::Protocol::load_le_u64(bytes.data()), 0x0123456789ABCDEFULL);
    }

    TEST(BinaryBufferTest, LeU64RoundTripsEdgeValues)
    {
        for (const uint64_t value : { uint64_t { 0 }, uint64_t { 1 }, uint64_t { 0x80 },
                 uint64_t { 0xFF00FF00FF00FF00ULL }, ~uint64_t { 0 } }) {
            Bytes8 bytes {};
            Transitive::Protocol::store_le_u64(bytes.data(), value);
            EXPECT_EQ(Transitive::Protocol::load_le_u64(bytes.data()), value);
        }
    }

    TEST(BinaryBufferTest, StoreLeU32WritesTheLowByteFirst)
    {
        std::array<uint8_t, 4> bytes {};

        Transitive::Protocol::store_le_u32(bytes.data(), 0x01020304U);

        const std::array<uint8_t, 4> expected { 0x04, 0x03, 0x02, 0x01 };
        EXPECT_EQ(bytes, expected);
    }

    TEST(BinaryBufferTest, LoadLeU32ReadsTheLowByteFirst)
    {
        const std::array<uint8_t, 4> bytes { 0x04, 0x03, 0x02, 0x01 };

        EXPECT_EQ(Transitive::Protocol::load_le_u32(bytes.data()), 0x01020304U);
    }

    TEST(BinaryBufferTest, LeU32RoundTripsEdgeValues)
    {
        for (const uint32_t value : { uint32_t { 0 }, uint32_t { 1 }, uint32_t { 0x80000000U },
                 uint32_t { 0xFF00FF00U }, ~uint32_t { 0 } }) {
            std::array<uint8_t, 4> bytes {};
            Transitive::Protocol::store_le_u32(bytes.data(), value);
            EXPECT_EQ(Transitive::Protocol::load_le_u32(bytes.data()), value);
        }
    }

    TEST(BinaryBufferTest, LeU32MatchesTheLowWordOfLeU64)
    {
        Bytes8 bytes {};

        Transitive::Protocol::store_le_u64(bytes.data(), 0xDEADBEEF12345678ULL);

        EXPECT_EQ(Transitive::Protocol::load_le_u32(bytes.data()), 0x12345678U);
        EXPECT_EQ(Transitive::Protocol::load_le_u32(bytes.data() + 4), 0xDEADBEEFU);
    }

    TEST(BinaryBufferTest, StoreLeDoubleWritesTheIeeeBitsLowByteFirst)
    {
        Bytes8 bytes {};

        Transitive::Protocol::store_le_double(bytes.data(), 1.0);

        const Bytes8 expected { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0x3F };
        EXPECT_EQ(bytes, expected);
    }

    TEST(BinaryBufferTest, LoadLeDoubleReadsTheIeeeBitsLowByteFirst)
    {
        const Bytes8 bytes { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC0 };

        EXPECT_EQ(Transitive::Protocol::load_le_double(bytes.data()), -2.0);
    }

    TEST(BinaryBufferTest, LeDoubleRoundTripKeepsEveryBitPattern)
    {
        const std::array<double, 8> values {
            0.0,
            -0.0,
            std::numeric_limits<double>::denorm_min(),
            -std::numeric_limits<double>::min(),
            std::numeric_limits<double>::max(),
            std::numeric_limits<double>::infinity(),
            -std::numeric_limits<double>::infinity(),
            std::bit_cast<double>(uint64_t { 0x7FF8000000000123ULL }),
        };

        for (const double value : values) {
            Bytes8 bytes {};
            Transitive::Protocol::store_le_double(bytes.data(), value);
            EXPECT_EQ(bits_of(Transitive::Protocol::load_le_double(bytes.data())), bits_of(value));
        }
    }

    TEST(BinaryBufferTest, LeDoubleWordsAreTheSameBytesAsTheBitPattern)
    {
        const double value = 0.3;
        Bytes8 bytes {};

        Transitive::Protocol::store_le_double(bytes.data(), value);

        EXPECT_EQ(Transitive::Protocol::load_le_u64(bytes.data()), bits_of(value));
    }

    TEST(BinaryBufferTest, WriteInt32IsBigEndianAndReadsBack)
    {
        std::vector<uint8_t> out;

        Transitive::Protocol::write_int32(out, 0x01020304);

        const std::vector<uint8_t> expected { 0x01, 0x02, 0x03, 0x04 };
        EXPECT_EQ(out, expected);

        size_t offset = 0;
        EXPECT_EQ(Transitive::Protocol::read_int32(out.data(), offset), 0x01020304);
        EXPECT_EQ(offset, 4U);
    }

    TEST(BinaryBufferTest, BigEndianAndLittleEndianReadTheSameBytesDifferently)
    {
        const Bytes8 bytes { 0x01, 0x02, 0x03, 0x04, 0x00, 0x00, 0x00, 0x00 };

        size_t offset = 0;
        const auto big = Transitive::Protocol::read_int32(bytes.data(), offset);
        const auto little = Transitive::Protocol::load_le_u64(bytes.data()) & 0xFFFFFFFFULL;

        EXPECT_EQ(static_cast<uint32_t>(big), 0x01020304U);
        EXPECT_EQ(little, 0x04030201ULL);
    }

    TEST(BinaryBufferTest, WriteStringPadsToFourBytesAndReadsBack)
    {
        std::vector<uint8_t> out;

        Transitive::Protocol::write_string(out, "abc");

        const std::vector<uint8_t> expected { 'a', 'b', 'c', 0x00 };
        EXPECT_EQ(out, expected);

        size_t offset = 0;
        EXPECT_EQ(Transitive::Protocol::read_string(out.data(), out.size(), offset), "abc");
        EXPECT_EQ(offset, 4U);
    }

    TEST(BinaryBufferTest, PaddedSizeRoundsUpToFourBytes)
    {
        EXPECT_EQ(Transitive::Protocol::padded_size(0), 0U);
        EXPECT_EQ(Transitive::Protocol::padded_size(4), 4U);
        EXPECT_EQ(Transitive::Protocol::padded_size(5), 8U);
    }

} // namespace
} // namespace MayaFlux::Test
