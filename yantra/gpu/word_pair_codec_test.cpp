#include "gtest/gtest.h"

#include <bit>
#include <cstdint>
#include <limits>
#include <vector>

#include "MayaFlux/Kinesis/Discrete/OrderKey.hpp"
#include "MayaFlux/Yantra/OperationSpec/OperationHelper.hpp"

namespace MayaFlux::Test {
namespace {

    using Yantra::OperationHelper;

    /** Reads each 8-byte group as a little-endian word, independently of the helper under test. */
    std::vector<uint64_t> words_of(const std::vector<uint8_t>& bytes)
    {
        std::vector<uint64_t> words(bytes.size() / 8);
        for (size_t i = 0; i < words.size(); ++i) {
            uint64_t word = 0;
            for (size_t k = 0; k < 8; ++k) {
                word |= static_cast<uint64_t>(bytes[i * 8 + k]) << (8 * k);
            }
            words[i] = word;
        }
        return words;
    }

    uint64_t bits_of(double value)
    {
        return std::bit_cast<uint64_t>(value);
    }

    TEST(YantraWordPairCodec, EncodeWritesTheIeeeBitsLowByteFirst)
    {
        const auto bytes = OperationHelper::encode_word_pairs(std::vector<double> { std::bit_cast<double>(uint64_t { 0x0102030405060708 }) });

        const std::vector<uint8_t> expected { 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01 };
        EXPECT_EQ(bytes, expected);
    }

    TEST(YantraWordPairCodec, EncodeOfOneIsTheExactBytesOfItsBitPattern)
    {
        const auto bytes = OperationHelper::encode_word_pairs(std::vector<double> { 1.0 });

        const std::vector<uint8_t> expected { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0x3F };
        EXPECT_EQ(bytes, expected);
    }

    TEST(YantraWordPairCodec, EncodeEmitsEightBytesPerValue)
    {
        const auto bytes = OperationHelper::encode_word_pairs(std::vector<double> { 1.0, 2.0, 3.0 });

        EXPECT_EQ(bytes.size(), 3 * sizeof(uint64_t));
    }

    TEST(YantraWordPairCodec, EncodePadsPastTheValuesWithAllOnesWords)
    {
        const auto bytes = OperationHelper::encode_word_pairs(std::vector<double> { 1.0 }, 3);

        const auto words = words_of(bytes);
        ASSERT_EQ(words.size(), 3U);
        EXPECT_EQ(words[1], ~uint64_t { 0 });
        EXPECT_EQ(words[2], ~uint64_t { 0 });
    }

    TEST(YantraWordPairCodec, EncodePaddingNeverShrinksTheValues)
    {
        const auto bytes = OperationHelper::encode_word_pairs(std::vector<double> { 1.0, 2.0 }, 1);

        EXPECT_EQ(bytes.size(), 2 * sizeof(uint64_t));
    }

    TEST(YantraWordPairCodec, EncodeOfNothingIsEmpty)
    {
        EXPECT_TRUE(OperationHelper::encode_word_pairs(std::vector<double> {}).empty());
    }

    TEST(YantraWordPairCodec, EncodeWithOrderKeyStoresTheOrderKeyOfEachValue)
    {
        const std::vector<double> values { -1.5, 2.0, 0.0 };
        const auto words = words_of(OperationHelper::encode_word_pairs(values, 0, true));

        ASSERT_EQ(words.size(), values.size());
        for (size_t i = 0; i < values.size(); ++i) {
            EXPECT_EQ(words[i], Kinesis::Discrete::order_key(values[i])) << "value " << i;
        }
    }

    TEST(YantraWordPairCodec, EncodeWithComplementInvertsEveryValueWord)
    {
        const std::vector<double> values { 1.0, -2.5 };
        const auto words = words_of(OperationHelper::encode_word_pairs(values, 0, false, true));

        ASSERT_EQ(words.size(), values.size());
        for (size_t i = 0; i < values.size(); ++i) {
            EXPECT_EQ(words[i], ~bits_of(values[i])) << "value " << i;
        }
    }

    TEST(YantraWordPairCodec, ComplementLeavesThePaddingAllOnes)
    {
        const auto words = words_of(OperationHelper::encode_word_pairs(std::vector<double> { 1.0 }, 2, false, true));

        ASSERT_EQ(words.size(), 2U);
        EXPECT_EQ(words[1], ~uint64_t { 0 });
    }

    TEST(YantraWordPairCodec, DecodeReadsTheLowWordFirst)
    {
        const std::vector<uint8_t> bytes { 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01 };

        const auto values = OperationHelper::decode_word_pairs(bytes);

        ASSERT_EQ(values.size(), 1U);
        EXPECT_EQ(bits_of(values[0]), uint64_t { 0x0102030405060708 });
    }

    TEST(YantraWordPairCodec, RoundTripKeepsEveryBitPattern)
    {
        const std::vector<double> values {
            0.0,
            -0.0,
            1.5,
            -2.25,
            std::numeric_limits<double>::infinity(),
            -std::numeric_limits<double>::infinity(),
            std::numeric_limits<double>::denorm_min(),
            std::numeric_limits<double>::max(),
            std::bit_cast<double>(uint64_t { 0x7FF8000000001234 }),
        };

        const auto decoded = OperationHelper::decode_word_pairs(OperationHelper::encode_word_pairs(values));

        ASSERT_EQ(decoded.size(), values.size());
        for (size_t i = 0; i < values.size(); ++i) {
            EXPECT_EQ(bits_of(decoded[i]), bits_of(values[i])) << "value " << i;
        }
    }

    TEST(YantraWordPairCodec, DecodeIgnoresATrailingPartialWord)
    {
        const std::vector<uint8_t> bytes(12, 0);

        EXPECT_EQ(OperationHelper::decode_word_pairs(bytes).size(), 1U);
    }

    TEST(YantraWordPairCodec, DecodeOfNothingIsEmpty)
    {
        EXPECT_TRUE(OperationHelper::decode_word_pairs(std::vector<uint8_t> {}).empty());
    }

} // namespace
} // namespace MayaFlux::Test
