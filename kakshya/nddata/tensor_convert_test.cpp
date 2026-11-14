#include "gtest/gtest.h"
#include "../../support/test_metadata.hpp"

#include "MayaFlux/Kakshya/NDData/TensorConvert.hpp"
#include "MayaFlux/Kakshya/NDData/TensorInsertion.hpp"
#include "MayaFlux/Kakshya/Region/Region.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Kakshya::convert_tensor;
    using Kakshya::diagnose_conversion;
    using Kakshya::TensorData;
    using Kakshya::TensorDevice;
    using Kakshya::TensorDeviceType;
    using Kakshya::TensorDType;
    using Kakshya::TensorDTypeKind;
    using Kakshya::TensorInsertion;
    using Kakshya::TensorQuantization;
    using Kakshya::TensorSpec;

    constexpr double nan = std::numeric_limits<double>::quiet_NaN();
    constexpr double inf = std::numeric_limits<double>::infinity();

    template <typename T>
    std::optional<TensorData> typed(const std::vector<T>& values, std::vector<uint64_t> shape = {}, std::string name = "t")
    {
        return TensorData::from_values<T>(values, std::move(shape), std::move(name));
    }

    std::optional<TensorData> halves(TensorDType dtype, const std::vector<uint16_t>& bits)
    {
        return TensorData::from_bytes(
            TensorSpec::contiguous(dtype, { bits.size() }, "t"), std::as_bytes(std::span<const uint16_t>(bits)));
    }

    std::vector<uint16_t> bits_of(const TensorData& tensor)
    {
        std::vector<uint16_t> bits(tensor.byte_size() / sizeof(uint16_t));
        std::memcpy(bits.data(), tensor.storage().data(), tensor.byte_size());
        return bits;
    }

    template <typename T>
    std::vector<T> values_of(const TensorData& tensor)
    {
        const auto view = tensor.access();
        if (!view)
            return {};
        const auto span = view->template as<T>();
        return { span.begin(), span.end() };
    }

    TensorData converted(const TensorData& source, TensorDType target,
        const std::optional<TensorQuantization>& quantization = std::nullopt)
    {
        const auto view = source.access();
        auto result = convert_tensor(*view, target, quantization);
        EXPECT_TRUE(result.has_value()) << (result.has_value() ? std::string {} : result.error());
        return result.has_value() ? std::move(*result) : TensorData {};
    }

    // ---------------------------------------------------------------------
    // Half and bfloat16 encodings
    // ---------------------------------------------------------------------

    struct EncodingCase {
        std::string name;
        double value;
        uint16_t half;
        uint16_t brain;
    };

    class TensorConvertEncodingTest : public ::testing::TestWithParam<EncodingCase> { };

    INSTANTIATE_TEST_SUITE_P(Encodings, TensorConvertEncodingTest,
        ::testing::Values(
            EncodingCase { "Zero", 0.0, 0x0000, 0x0000 },
            EncodingCase { "NegativeZero", -0.0, 0x8000, 0x8000 },
            EncodingCase { "One", 1.0, 0x3C00, 0x3F80 },
            EncodingCase { "MinusTwo", -2.0, 0xC000, 0xC000 },
            EncodingCase { "Half", 0.5, 0x3800, 0x3F00 },
            EncodingCase { "Three", 3.0, 0x4200, 0x4040 },
            EncodingCase { "LargestHalf", 65504.0, 0x7BFF, 0x4780 },
            EncodingCase { "HalfOverflowTie", 65520.0, 0x7C00, 0x4780 },
            EncodingCase { "SmallestHalfSubnormal", 5.9604644775390625e-8, 0x0001, 0x3380 },
            EncodingCase { "HalfUnderflowTie", 2.98023223876953125e-8, 0x0000, 0x3300 },
            EncodingCase { "OneThird", 1.0 / 3.0, 0x3555, 0x3EAB },
            EncodingCase { "Pi", 3.14159265358979323846, 0x4248, 0x4049 },
            EncodingCase { "Infinity", inf, 0x7C00, 0x7F80 },
            EncodingCase { "NegativeInfinity", -inf, 0xFC00, 0xFF80 },
            EncodingCase { "NotANumber", nan, 0x7E00, 0x7FC0 }),
        [](const ::testing::TestParamInfo<EncodingCase>& info) { return info.param.name; });

    TEST_P(TensorConvertEncodingTest, DoublesEncodeToTheExpectedBitPatterns)
    {
        const auto& c = GetParam();
        const auto source = typed<double>({ c.value });
        ASSERT_TRUE(source.has_value());

        const auto half = converted(*source, TensorDType::float16());
        const auto brain = converted(*source, TensorDType::bfloat16());

        EXPECT_EQ(bits_of(half), (std::vector<uint16_t> { c.half }));
        EXPECT_EQ(bits_of(brain), (std::vector<uint16_t> { c.brain }));
    }

    TEST(TensorConvertFloatTest, ExactlyRepresentableBitPatternsDecodeToTheirValues)
    {
        const std::vector<std::pair<uint16_t, double>> half_values {
            { 0x0000, 0.0 }, { 0x3C00, 1.0 }, { 0xC000, -2.0 }, { 0x3800, 0.5 }, { 0x4200, 3.0 },
            { 0x7BFF, 65504.0 }, { 0x0001, 5.9604644775390625e-8 }, { 0x7C00, inf }, { 0xFC00, -inf }
        };
        const std::vector<std::pair<uint16_t, double>> brain_values {
            { 0x0000, 0.0 }, { 0x3F80, 1.0 }, { 0xC000, -2.0 }, { 0x3F00, 0.5 }, { 0x4040, 3.0 },
            { 0x4780, 65536.0 }, { 0x3380, 5.9604644775390625e-8 }, { 0x7F80, inf }, { 0xFF80, -inf }
        };

        for (const auto& [kind, table] : { std::pair { TensorDType::float16(), half_values }, std::pair { TensorDType::bfloat16(), brain_values } }) {
            std::vector<uint16_t> bits;
            std::vector<double> expected;
            for (const auto& [pattern, value] : table) {
                bits.push_back(pattern);
                expected.push_back(value);
            }
            const auto source = halves(kind, bits);
            ASSERT_TRUE(source.has_value());

            EXPECT_EQ(values_of<double>(converted(*source, TensorDType::of<double>())), expected) << kind.name();
        }
    }

    TEST(TensorConvertFloatTest, NotANumberDecodesToNotANumber)
    {
        const auto source = halves(TensorDType::float16(), { 0x7E00, 0x7C01 });
        ASSERT_TRUE(source.has_value());

        const auto values = values_of<float>(converted(*source, TensorDType::of<float>()));

        ASSERT_EQ(values.size(), 2U);
        EXPECT_TRUE(std::isnan(values[0]));
        EXPECT_TRUE(std::isnan(values[1]));
    }

    TEST(TensorConvertFloatTest, TiesRoundToEvenWithoutDoubleRounding)
    {
        const double tie = 1.0 + std::ldexp(1.0, -8);
        const auto brain_source = typed<double>({ tie, 1.0 + 3.0 * std::ldexp(1.0, -8), tie + std::ldexp(1.0, -30) });
        const auto half_source = typed<double>({ 1.0 + std::ldexp(1.0, -11), 1.0 + 3.0 * std::ldexp(1.0, -11) });
        ASSERT_TRUE(brain_source.has_value());
        ASSERT_TRUE(half_source.has_value());

        EXPECT_EQ(bits_of(converted(*brain_source, TensorDType::bfloat16())), (std::vector<uint16_t> { 0x3F80, 0x3F82, 0x3F81 }));
        EXPECT_EQ(bits_of(converted(*half_source, TensorDType::float16())), (std::vector<uint16_t> { 0x3C00, 0x3C02 }));
    }

    TEST(TensorConvertFloatTest, FloatNarrowingRoundsAndOverflowsToInfinity)
    {
        const auto source = typed<double>({ 1.0, 0.1, 1e300, -1e300 });
        ASSERT_TRUE(source.has_value());

        const auto narrowed = values_of<float>(converted(*source, TensorDType::of<float>()));

        ASSERT_EQ(narrowed.size(), 4U);
        EXPECT_EQ(narrowed[0], 1.0F);
        EXPECT_EQ(narrowed[1], 0.1F);
        EXPECT_EQ(narrowed[2], std::numeric_limits<float>::infinity());
        EXPECT_EQ(narrowed[3], -std::numeric_limits<float>::infinity());
        EXPECT_EQ(values_of<double>(converted(*typed<float>({ 0.1F }), TensorDType::of<double>())), (std::vector<double> { static_cast<double>(0.1F) }));
    }

    // ---------------------------------------------------------------------
    // Integers and bool
    // ---------------------------------------------------------------------

    TEST(TensorConvertIntegerTest, FloatsRoundToNearestEvenAndSaturate)
    {
        const auto source = typed<float>({ 300.5F, -300.0F, 127.5F, 0.5F, 1.5F, 2.5F, -0.5F,
            std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
            -std::numeric_limits<float>::infinity() });
        ASSERT_TRUE(source.has_value());

        EXPECT_EQ(values_of<int8_t>(converted(*source, TensorDType::of<int8_t>())),
            (std::vector<int8_t> { 127, -128, 127, 0, 2, 2, 0, 0, 127, -128 }));
    }

    TEST(TensorConvertIntegerTest, UnsignedTargetsClampNegativesToZero)
    {
        const auto source = typed<double>({ -1.0, 255.5, 256.0, 0.4, 254.5 });
        ASSERT_TRUE(source.has_value());

        EXPECT_EQ(values_of<uint8_t>(converted(*source, TensorDType::of<uint8_t>())),
            (std::vector<uint8_t> { 0, 255, 255, 0, 254 }));
    }

    TEST(TensorConvertIntegerTest, SixtyFourBitIntegersConvertExactly)
    {
        constexpr int64_t big = (int64_t { 1 } << 62) + 1;
        const auto source = typed<int64_t>({ big, -big, std::numeric_limits<int64_t>::max(), std::numeric_limits<int64_t>::min(), 0 });
        ASSERT_TRUE(source.has_value());

        EXPECT_EQ(values_of<uint64_t>(converted(*source, TensorDType::of<uint64_t>())),
            (std::vector<uint64_t> { static_cast<uint64_t>(big), 0, static_cast<uint64_t>(std::numeric_limits<int64_t>::max()), 0, 0 }));
        EXPECT_EQ(values_of<int32_t>(converted(*source, TensorDType::of<int32_t>())),
            (std::vector<int32_t> { std::numeric_limits<int32_t>::max(), std::numeric_limits<int32_t>::min(),
                std::numeric_limits<int32_t>::max(), std::numeric_limits<int32_t>::min(), 0 }));
        EXPECT_EQ(values_of<int64_t>(converted(*source, TensorDType::of<int64_t>())), values_of<int64_t>(*source));
    }

    TEST(TensorConvertIntegerTest, UnsignedSixtyFourBitValuesSaturateIntoNarrowerTypes)
    {
        const auto source = typed<uint64_t>({ std::numeric_limits<uint64_t>::max(), uint64_t { 1 } << 63, 1 });
        ASSERT_TRUE(source.has_value());

        EXPECT_EQ(values_of<int64_t>(converted(*source, TensorDType::of<int64_t>())),
            (std::vector<int64_t> { std::numeric_limits<int64_t>::max(), std::numeric_limits<int64_t>::max(), 1 }));
        EXPECT_EQ(values_of<uint8_t>(converted(*source, TensorDType::of<uint8_t>())), (std::vector<uint8_t> { 255, 255, 1 }));
    }

    TEST(TensorConvertIntegerTest, LargeIntegersRoundOnceIntoFloat)
    {
        const int64_t tricky = (int64_t { 1 } << 60) + (int64_t { 1 } << 36) + 1;
        const auto source = typed<int64_t>({ tricky, -tricky });
        ASSERT_TRUE(source.has_value());
        const float expected = std::ldexp(1.0F, 60) + std::ldexp(1.0F, 37);

        EXPECT_EQ(values_of<float>(converted(*source, TensorDType::of<float>())), (std::vector<float> { expected, -expected }));
    }

    TEST(TensorConvertIntegerTest, BoolIsNonZeroAndConvertsBack)
    {
        const auto source = typed<float>({ 0.0F, 0.5F, -1.0F, std::numeric_limits<float>::quiet_NaN(), -0.0F });
        ASSERT_TRUE(source.has_value());

        const auto truth = converted(*source, TensorDType::of<bool>());

        EXPECT_EQ(values_of<bool>(truth), (std::vector<bool> { false, true, true, true, false }));
        EXPECT_EQ(values_of<float>(converted(truth, TensorDType::of<float>())), (std::vector<float> { 0, 1, 1, 1, 0 }));
        EXPECT_EQ(values_of<int8_t>(converted(truth, TensorDType::of<int8_t>())), (std::vector<int8_t> { 0, 1, 1, 1, 0 }));
    }

    TEST(TensorConvertIntegerTest, ComplexConvertsBetweenPrecisions)
    {
        using C32 = std::complex<float>;
        using C64 = std::complex<double>;
        const auto source = typed<C32>({ C32 { 1.5F, -2.0F }, C32 { 0.0F, 3.0F } });
        ASSERT_TRUE(source.has_value());

        const auto wide = converted(*source, TensorDType::of<C64>());

        EXPECT_EQ(values_of<C64>(wide), (std::vector<C64> { C64 { 1.5, -2.0 }, C64 { 0.0, 3.0 } }));
        EXPECT_EQ(values_of<C32>(converted(wide, TensorDType::of<C32>())), values_of<C32>(*source));
    }

    // ---------------------------------------------------------------------
    // Every supported pair keeps small integers exactly
    // ---------------------------------------------------------------------

    struct PairCase {
        std::string name;
        TensorDType from;
        TensorDType to;
    };

    const std::vector<TensorDType>& real_types()
    {
        static const std::vector<TensorDType> types {
            TensorDType::of<int8_t>(), TensorDType::of<uint8_t>(), TensorDType::of<int16_t>(), TensorDType::of<uint16_t>(),
            TensorDType::of<int32_t>(), TensorDType::of<uint32_t>(), TensorDType::of<int64_t>(), TensorDType::of<uint64_t>(),
            TensorDType::float16(), TensorDType::bfloat16(), TensorDType::of<float>(), TensorDType::of<double>()
        };
        return types;
    }

    std::vector<PairCase> all_pairs()
    {
        std::vector<PairCase> pairs;
        for (const auto from : real_types()) {
            for (const auto to : real_types())
                pairs.push_back({ from.name() + "_to_" + to.name(), from, to });
        }
        return pairs;
    }

    std::optional<TensorData> small_ramp(TensorDType dtype)
    {
        if (dtype == TensorDType::float16())
            return halves(dtype, { 0x0000, 0x3C00, 0x4000, 0x4200 });
        if (dtype == TensorDType::bfloat16())
            return halves(dtype, { 0x0000, 0x3F80, 0x4000, 0x4040 });
        if (dtype == TensorDType::of<int8_t>())
            return typed<int8_t>({ 0, 1, 2, 3 });
        if (dtype == TensorDType::of<uint8_t>())
            return typed<uint8_t>({ 0, 1, 2, 3 });
        if (dtype == TensorDType::of<int16_t>())
            return typed<int16_t>({ 0, 1, 2, 3 });
        if (dtype == TensorDType::of<uint16_t>())
            return typed<uint16_t>({ 0, 1, 2, 3 });
        if (dtype == TensorDType::of<int32_t>())
            return typed<int32_t>({ 0, 1, 2, 3 });
        if (dtype == TensorDType::of<uint32_t>())
            return typed<uint32_t>({ 0, 1, 2, 3 });
        if (dtype == TensorDType::of<int64_t>())
            return typed<int64_t>({ 0, 1, 2, 3 });
        if (dtype == TensorDType::of<uint64_t>())
            return typed<uint64_t>({ 0, 1, 2, 3 });
        if (dtype == TensorDType::of<float>())
            return typed<float>({ 0, 1, 2, 3 });
        return typed<double>({ 0, 1, 2, 3 });
    }

    bool is_small_ramp(const TensorData& tensor)
    {
        const auto dtype = tensor.spec().dtype;
        if (dtype == TensorDType::float16())
            return bits_of(tensor) == std::vector<uint16_t> { 0x0000, 0x3C00, 0x4000, 0x4200 };
        if (dtype == TensorDType::bfloat16())
            return bits_of(tensor) == std::vector<uint16_t> { 0x0000, 0x3F80, 0x4000, 0x4040 };
        const auto expect = [&]<typename T>() { return values_of<T>(tensor) == std::vector<T> { 0, 1, 2, 3 }; };
        if (dtype == TensorDType::of<int8_t>())
            return expect.operator()<int8_t>();
        if (dtype == TensorDType::of<uint8_t>())
            return expect.operator()<uint8_t>();
        if (dtype == TensorDType::of<int16_t>())
            return expect.operator()<int16_t>();
        if (dtype == TensorDType::of<uint16_t>())
            return expect.operator()<uint16_t>();
        if (dtype == TensorDType::of<int32_t>())
            return expect.operator()<int32_t>();
        if (dtype == TensorDType::of<uint32_t>())
            return expect.operator()<uint32_t>();
        if (dtype == TensorDType::of<int64_t>())
            return expect.operator()<int64_t>();
        if (dtype == TensorDType::of<uint64_t>())
            return expect.operator()<uint64_t>();
        if (dtype == TensorDType::of<float>())
            return expect.operator()<float>();
        return expect.operator()<double>();
    }

    class TensorConvertPairTest : public ::testing::TestWithParam<PairCase> { };

    INSTANTIATE_TEST_SUITE_P(Pairs, TensorConvertPairTest, ::testing::ValuesIn(all_pairs()),
        [](const ::testing::TestParamInfo<PairCase>& info) { return info.param.name; });

    TEST_P(TensorConvertPairTest, ZeroToThreeSurviveTheConversion)
    {
        const auto& c = GetParam();
        const auto source = small_ramp(c.from);
        ASSERT_TRUE(source.has_value());
        ASSERT_TRUE(is_small_ramp(*source));

        const auto result = converted(*source, c.to);

        EXPECT_EQ(result.spec().dtype, c.to);
        EXPECT_TRUE(is_small_ramp(result));
    }

    // ---------------------------------------------------------------------
    // Quantization
    // ---------------------------------------------------------------------

    TEST(TensorConvertQuantizationTest, ASourceWithQuantizationIsDequantized)
    {
        auto source = typed<int8_t>({ -3, -1, 0, 5 });
        ASSERT_TRUE(source.has_value());
        ASSERT_TRUE(TensorInsertion(*source).set_quantization(TensorQuantization { .scale = { 0.5 }, .zero_point = { -3 }, .axis = std::nullopt }));

        const auto result = converted(*source, TensorDType::of<float>());

        EXPECT_EQ(values_of<float>(result), (std::vector<float> { 0.0F, 1.0F, 1.5F, 4.0F }));
        EXPECT_FALSE(result.spec().quantization.has_value());
    }

    TEST(TensorConvertQuantizationTest, ScalesFollowTheirAxis)
    {
        auto rows = typed<int8_t>({ 2, 4, 6, 2, 4, 6 }, { 2, 3 });
        auto columns = typed<int8_t>({ 1, 1, 1, 2, 2, 2 }, { 2, 3 });
        ASSERT_TRUE(rows.has_value());
        ASSERT_TRUE(columns.has_value());
        ASSERT_TRUE(TensorInsertion(*rows).set_quantization(TensorQuantization { .scale = { 0.5, 2.0 }, .zero_point = {}, .axis = 0 }));
        ASSERT_TRUE(TensorInsertion(*columns).set_quantization(TensorQuantization { .scale = { 1.0, 10.0, 100.0 }, .zero_point = {}, .axis = 1 }));

        EXPECT_EQ(values_of<float>(converted(*rows, TensorDType::of<float>())), (std::vector<float> { 1, 2, 3, 4, 8, 12 }));
        EXPECT_EQ(values_of<float>(converted(*columns, TensorDType::of<float>())), (std::vector<float> { 1, 10, 100, 2, 20, 200 }));
    }

    TEST(TensorConvertQuantizationTest, ARequestedQuantizationStoresTheClampedRoundedValue)
    {
        const auto source = typed<double>({ 0.0, 0.25, 0.125, 0.375, 100.0, -100.0, nan });
        ASSERT_TRUE(source.has_value());
        const TensorQuantization request { .scale = { 0.25 }, .zero_point = { 10 }, .axis = std::nullopt };

        const auto result = converted(*source, TensorDType::of<int8_t>(), request);

        EXPECT_EQ(values_of<int8_t>(result), (std::vector<int8_t> { 10, 11, 10, 12, 127, -128, 10 }));
        ASSERT_TRUE(result.spec().quantization.has_value());
        EXPECT_EQ(result.spec().quantization->scale, request.scale);
        EXPECT_EQ(result.spec().quantization->zero_point, request.zero_point);
    }

    TEST(TensorConvertQuantizationTest, QuantizingPerAxisUsesEachSlicesScale)
    {
        const auto source = typed<float>({ 1, 1, 2, 2 }, { 2, 2 });
        ASSERT_TRUE(source.has_value());

        const auto result = converted(*source, TensorDType::of<int16_t>(),
            TensorQuantization { .scale = { 1.0, 0.5 }, .zero_point = { 0, 4 }, .axis = 1 });

        EXPECT_EQ(values_of<int16_t>(result), (std::vector<int16_t> { 1, 6, 2, 8 }));
    }

    TEST(TensorConvertQuantizationTest, RequantizingMovesBetweenIntegerEncodings)
    {
        auto source = typed<int8_t>({ -4, 3, 127 });
        ASSERT_TRUE(source.has_value());
        ASSERT_TRUE(TensorInsertion(*source).set_quantization(TensorQuantization { .scale = { 0.5 }, .zero_point = {}, .axis = std::nullopt }));

        const auto result = converted(*source, TensorDType::of<uint8_t>(),
            TensorQuantization { .scale = { 1.0 }, .zero_point = { 128 }, .axis = std::nullopt });

        EXPECT_EQ(values_of<uint8_t>(result), (std::vector<uint8_t> { 126, 130, 192 }));
    }

    TEST(TensorConvertQuantizationTest, TheSameTypeAndQuantizationCopiesTheBytes)
    {
        auto source = typed<int8_t>({ -128, -1, 0, 127 });
        ASSERT_TRUE(source.has_value());
        const TensorQuantization q { .scale = { 0.3 }, .zero_point = { 7 }, .axis = std::nullopt };
        ASSERT_TRUE(TensorInsertion(*source).set_quantization(q));

        const auto result = converted(*source, TensorDType::of<int8_t>(), q);

        EXPECT_EQ(values_of<int8_t>(result), values_of<int8_t>(*source));
        EXPECT_EQ(result.spec().quantization->scale, q.scale);
    }

    // ---------------------------------------------------------------------
    // Layout, metadata and degenerate shapes
    // ---------------------------------------------------------------------

    TEST(TensorConvertLayoutTest, AColumnMajorSourceIsReadLogicallyIntoDenseOrder)
    {
        std::vector<float> storage { 0, 1, 2, 3, 4, 5 };
        auto spec = TensorSpec::contiguous(TensorDType::of<float>(), { 2, 3 }, "fortran");
        spec.strides = { 1, 2 };
        const auto source = TensorData::from_bytes(spec, std::as_bytes(std::span<const float>(storage)));
        ASSERT_TRUE(source.has_value());

        const auto result = converted(*source, TensorDType::of<int32_t>());

        EXPECT_EQ(values_of<int32_t>(result), (std::vector<int32_t> { 0, 2, 4, 1, 3, 5 }));
        EXPECT_TRUE(result.spec().is_contiguous());
        EXPECT_TRUE(result.spec().strides.empty());
    }

    TEST(TensorConvertLayoutTest, AReversedAxisWithAnOffsetIsReadInLogicalOrder)
    {
        std::vector<float> storage { 0, 1, 2, 3 };
        auto spec = TensorSpec::contiguous(TensorDType::of<float>(), { 4 }, "reversed");
        spec.strides = { -1 };
        spec.byte_offset = 3 * sizeof(float);
        const auto source = TensorData::from_bytes(spec, std::as_bytes(std::span<const float>(storage)));
        ASSERT_TRUE(source.has_value());

        EXPECT_EQ(values_of<double>(converted(*source, TensorDType::of<double>())), (std::vector<double> { 3, 2, 1, 0 }));
    }

    TEST(TensorConvertLayoutTest, AWindowOfATensorConvertsOnlyThatBox)
    {
        std::vector<float> values(20);
        std::iota(values.begin(), values.end(), 0.0F);
        const auto base = typed<float>(values, { 4, 5 });
        ASSERT_TRUE(base.has_value());
        const auto window = base->spec().window(Kakshya::Region({ 1, 1 }, { 2, 3 }));
        ASSERT_TRUE(window.has_value());
        const auto view = Kakshya::as_tensor_access(base->storage(), *window);
        ASSERT_TRUE(view.has_value());

        const auto result = convert_tensor(*view, TensorDType::of<int16_t>());

        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(values_of<int16_t>(*result), (std::vector<int16_t> { 6, 7, 8, 11, 12, 13 }));
        EXPECT_EQ(result->spec().shape, (std::vector<uint64_t> { 2, 3 }));
    }

    TEST(TensorConvertLayoutTest, NameAndAxesSurviveAndTheResultIsDense)
    {
        auto source = typed<float>({ 1, 2, 3, 4, 5, 6 }, { 2, 3 }, "weights");
        ASSERT_TRUE(source.has_value());
        ASSERT_TRUE(TensorInsertion(*source).set_axes({ Kakshya::DataDimension::time(2), Kakshya::DataDimension::channel(3) }));

        const auto result = converted(*source, TensorDType::float16());

        EXPECT_EQ(result.spec().name, "weights");
        ASSERT_EQ(result.spec().axes.size(), 2U);
        EXPECT_EQ(result.spec().axes[1].role, Kakshya::DataDimension::Role::CHANNEL);
        EXPECT_EQ(result.spec().byte_offset, 0U);
        EXPECT_TRUE(result.spec().is_contiguous());
        EXPECT_EQ(result.byte_size(), 6U * sizeof(uint16_t));
    }

    TEST(TensorConvertLayoutTest, EmptyAndScalarTensorsConvert)
    {
        const auto empty = TensorData::allocate(TensorSpec::contiguous(TensorDType::of<float>(), { 0 }));
        auto scalar = TensorData::allocate(TensorSpec::contiguous(TensorDType::of<float>(), {}));
        ASSERT_TRUE(empty.has_value());
        ASSERT_TRUE(scalar.has_value());
        ASSERT_TRUE(TensorInsertion(*scalar).write<float>(std::vector<float> { 5.0F }));

        EXPECT_EQ(converted(*empty, TensorDType::of<int32_t>()).spec().shape, (std::vector<uint64_t> { 0 }));
        const auto result = converted(*scalar, TensorDType::of<int32_t>());
        EXPECT_EQ(result.spec().rank(), 0U);
        EXPECT_EQ(values_of<int32_t>(result), (std::vector<int32_t> { 5 }));
    }

    // ---------------------------------------------------------------------
    // Refusals
    // ---------------------------------------------------------------------

    bool mentions(const std::string& text, std::string_view word)
    {
        return text.find(word) != std::string::npos;
    }

    TEST(TensorConvertRefusalTest, UnsupportedElementTypesAreRefusedWithAReason)
    {
        const auto spec = TensorSpec::contiguous(TensorDType::of<float>(), { 4 }, "w");
        const auto float8 = TensorDType { .kind = TensorDTypeKind::FLOAT8_E4M3FN, .bits = 8, .lanes = 1 };
        const auto int4 = TensorDType { .kind = TensorDTypeKind::INT, .bits = 4, .lanes = 1 };
        const auto vec4 = TensorDType { .kind = TensorDTypeKind::FLOAT, .bits = 32, .lanes = 4 };

        EXPECT_TRUE(mentions(diagnose_conversion(spec, float8), "not supported"));
        EXPECT_TRUE(mentions(diagnose_conversion(spec, int4), "not supported"));
        EXPECT_TRUE(mentions(diagnose_conversion(spec, vec4), "lanes"));

        const auto source = TensorSpec::contiguous(float8, { 4 }, "q");
        EXPECT_TRUE(mentions(diagnose_conversion(source, TensorDType::of<float>()), "source"));
    }

    TEST(TensorConvertRefusalTest, ComplexAndRealDoNotConvert)
    {
        const auto real = TensorSpec::contiguous(TensorDType::of<float>(), { 4 });
        const auto complex = TensorSpec::contiguous(TensorDType::of<std::complex<float>>(), { 4 });

        EXPECT_TRUE(mentions(diagnose_conversion(real, TensorDType::of<std::complex<float>>()), "complex"));
        EXPECT_TRUE(mentions(diagnose_conversion(complex, TensorDType::of<float>()), "complex"));
        EXPECT_TRUE(diagnose_conversion(complex, TensorDType::of<std::complex<double>>()).empty());
    }

    TEST(TensorConvertRefusalTest, QuantizationNeedsAnIntegerTargetAndSaneScales)
    {
        const auto spec = TensorSpec::contiguous(TensorDType::of<float>(), { 3 }, "w");
        const auto request = [](std::vector<double> scales, std::optional<size_t> axis) {
            return TensorQuantization { .scale = std::move(scales), .zero_point = {}, .axis = axis };
        };

        EXPECT_TRUE(mentions(diagnose_conversion(spec, TensorDType::of<float>(), request({ 1.0 }, std::nullopt)), "cannot hold quantization"));
        EXPECT_TRUE(mentions(diagnose_conversion(spec, TensorDType::of<int64_t>(), request({ 1.0 }, std::nullopt)), "cannot hold quantization"));
        EXPECT_FALSE(diagnose_conversion(spec, TensorDType::of<int8_t>(), request({ 1.0, 2.0 }, 0)).empty());
        EXPECT_TRUE(mentions(diagnose_conversion(spec, TensorDType::of<int8_t>(), request({ 0.0 }, std::nullopt)), "finite and non-zero"));
        EXPECT_TRUE(mentions(diagnose_conversion(spec, TensorDType::of<int8_t>(), request({ nan }, std::nullopt)), "finite and non-zero"));
        EXPECT_TRUE(diagnose_conversion(spec, TensorDType::of<int8_t>(), request({ 1.0, -2.0, 4.0 }, 0)).empty());
    }

    TEST(TensorConvertRefusalTest, QuantizationOnWideOrFloatStorageIsRefused)
    {
        auto wide = TensorSpec::contiguous(TensorDType::of<int64_t>(), { 2 }, "wide");
        wide.quantization = TensorQuantization { .scale = { 1.0 }, .zero_point = {}, .axis = std::nullopt };
        auto floats = TensorSpec::contiguous(TensorDType::of<float>(), { 2 }, "floats");
        floats.quantization = TensorQuantization { .scale = { 1.0 }, .zero_point = {}, .axis = std::nullopt };

        EXPECT_TRUE(mentions(diagnose_conversion(wide, TensorDType::of<float>()), "quantization on"));
        EXPECT_TRUE(mentions(diagnose_conversion(floats, TensorDType::of<double>()), "quantization on"));
    }

    TEST(TensorConvertRefusalTest, DeviceMemoryIsRefused)
    {
        auto spec = TensorSpec::contiguous(TensorDType::of<float>(), { 2 }, "gpu");
        spec.device = TensorDevice { TensorDeviceType::CUDA, 0 };

        EXPECT_TRUE(mentions(diagnose_conversion(spec, TensorDType::of<float>()), "host"));
    }

    TEST(TensorConvertRefusalTest, ConvertReportsTheDiagnosis)
    {
        const auto source = typed<float>({ 1, 2 });
        ASSERT_TRUE(source.has_value());
        const auto view = source->access();
        const auto target = TensorDType { .kind = TensorDTypeKind::FLOAT4_E2M1FN, .bits = 4, .lanes = 1 };

        const auto result = convert_tensor(*view, target);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error(), diagnose_conversion(view->spec(), target));
    }

}
}
