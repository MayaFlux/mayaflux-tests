#include "gtest/gtest.h"
#include "../../support/test_metadata.hpp"

#include <random>

#include "../../support/gpu_dispatch_test_support.hpp"

#include "MayaFlux/Yantra/Executors/Audio/WordPairMap.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))

namespace MayaFlux::Test {
namespace {

    using namespace GpuDispatch;
    using Portal::Graphics::WordPairOp;
    using Yantra::word_pair_map_spec;
    using Yantra::word_pair_map_stage;
    using Yantra::WordPairExecutionContext;

    constexpr uint64_t k_mantissa_mask = (1ULL << 52) - 1ULL;

    uint64_t bits_of(double value) { return std::bit_cast<uint64_t>(value); }

    double make(uint64_t sign, uint64_t field, uint64_t mantissa)
    {
        return std::bit_cast<double>((sign << 63) | (field << 52) | (mantissa & k_mantissa_mask));
    }

    std::string hex(double value)
    {
        char text[24];
        std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(bits_of(value)));
        return text;
    }

    double oracle(WordPairOp op, double a, double b)
    {
        switch (op) {
        case WordPairOp::Add:
            return a + b;
        case WordPairOp::Mul:
            return a * b;
        case WordPairOp::Div:
            return a / b;
        case WordPairOp::Sqrt:
        default:
            return std::sqrt(a);
        }
    }

    bool same(double got, double want)
    {
        return (std::isnan(got) && std::isnan(want)) || bits_of(got) == bits_of(want);
    }

    struct Pass {
        std::unique_ptr<WordPairExecutionContext> context;
        Yantra::GpuComputeConfig config;
        WordPairOp op;
    };

    Pass make_pass(WordPairOp op)
    {
        const auto spec = word_pair_map_spec(op);
        auto config = Yantra::config_from_spec(spec);
        auto context = std::make_unique<WordPairExecutionContext>(config, Yantra::bindings_from_spec(spec), false, "WordPairMap");
        return { std::move(context), std::move(config), op };
    }

    std::vector<double> run(Pass& pass, std::span<const double> a, std::span<const double> b)
    {
        const bool unary = pass.op == WordPairOp::Sqrt;
        const size_t count = unary ? a.size() : std::min(a.size(), b.size());
        if (count == 0) {
            return {};
        }
        pass.context->record_stages({ word_pair_map_stage(*pass.context, pass.config, pass.op, a, b) });
        return pass.context->download_words(unary ? 1 : 2, count);
    }

    void expect_exact(WordPairOp op, const std::vector<double>& left, const std::vector<double>& right = {})
    {
        const bool unary = op == WordPairOp::Sqrt;
        const size_t count = unary ? left.size() : std::min(left.size(), right.size());
        ASSERT_GT(count, 0U);

        auto pass = make_pass(op);
        const auto got = run(pass, left, right);
        ASSERT_EQ(got.size(), count);

        size_t wrong = 0;
        std::string report;
        for (size_t i = 0; i < count; ++i) {
            const double b = unary ? 0.0 : right[i];
            const double want = oracle(op, left[i], b);
            if (!same(got[i], want)) {
                if (wrong < 8) {
                    report += "  a " + hex(left[i]) + "  b " + hex(b) + "  got " + hex(got[i]) + "  want " + hex(want) + "\n";
                }
                ++wrong;
            }
        }
        EXPECT_EQ(wrong, 0U) << wrong << " of " << count << " differ from the host result, first:\n"
                             << report;
    }

    void expect_exact_pairs(WordPairOp op, const std::vector<std::pair<double, double>>& pairs)
    {
        std::vector<double> left;
        std::vector<double> right;
        for (const auto& [a, b] : pairs) {
            left.push_back(a);
            right.push_back(b);
        }
        expect_exact(op, left, right);
    }

    constexpr std::array<uint64_t, 7> k_mantissas { 0ULL, 1ULL, 2ULL, 0x0008000000000000ULL, 0x000FFFFFFFFFFFFFULL, 0x0005555555555555ULL, 0x000AAAAAAAAAAAAAULL };
    constexpr std::array<uint64_t, 10> k_fields { 0ULL, 1ULL, 2ULL, 1022ULL, 1023ULL, 1024ULL, 1075ULL, 2045ULL, 2046ULL, 2047ULL };

    std::vector<double> special_values()
    {
        std::vector<double> out;
        for (uint64_t sign = 0; sign < 2; ++sign) {
            for (const uint64_t field : k_fields) {
                for (const uint64_t mantissa : k_mantissas) {
                    out.push_back(make(sign, field, mantissa));
                }
            }
        }
        return out;
    }

    void cross(const std::vector<double>& lefts, const std::vector<double>& rights, std::vector<double>& a, std::vector<double>& b)
    {
        for (const double left : lefts) {
            for (const double right : rights) {
                a.push_back(left);
                b.push_back(right);
            }
        }
    }

    std::vector<double> random_bit_patterns(size_t count, uint64_t seed)
    {
        std::mt19937_64 engine(seed);
        std::vector<double> out(count);
        for (auto& value : out) {
            value = std::bit_cast<double>(engine());
        }
        return out;
    }

    std::vector<double> random_moderate(size_t count, uint64_t seed)
    {
        std::mt19937_64 engine(seed);
        std::vector<double> out(count);
        for (auto& value : out) {
            const uint64_t draw = engine();
            value = make(draw >> 63, 1023 - 80 + (draw >> 20) % 161, draw);
        }
        return out;
    }

    std::vector<double> exponent_sweep(const std::vector<uint64_t>& fields)
    {
        std::vector<double> out;
        for (const uint64_t field : fields) {
            for (const uint64_t mantissa : k_mantissas) {
                out.push_back(make(0, field, mantissa));
                out.push_back(make(1, field, mantissa));
            }
        }
        return out;
    }

    std::vector<uint64_t> every_field()
    {
        std::vector<uint64_t> out(2048);
        std::iota(out.begin(), out.end(), 0ULL);
        return out;
    }

    struct Wide {
        uint64_t hi;
        uint64_t lo;
    };

    Wide multiply(uint64_t x, uint64_t y)
    {
        constexpr uint64_t low = 0xFFFFFFFFULL;
        const uint64_t p00 = (x & low) * (y & low);
        const uint64_t p01 = (x & low) * (y >> 32);
        const uint64_t p10 = (x >> 32) * (y & low);
        const uint64_t p11 = (x >> 32) * (y >> 32);
        const uint64_t middle = (p00 >> 32) + (p01 & low) + (p10 & low);
        return { p11 + (p01 >> 32) + (p10 >> 32) + (middle >> 32), (p00 & low) | (middle << 32) };
    }

    uint64_t inverse_mod_2_64(uint64_t odd)
    {
        uint64_t inverse = odd;
        for (int i = 0; i < 6; ++i) {
            inverse *= 2ULL - odd * inverse;
        }
        return inverse;
    }

    std::vector<std::pair<double, double>> division_near_ties(size_t wanted, uint64_t seed)
    {
        std::mt19937_64 engine(seed);
        std::vector<std::pair<double, double>> out;
        for (size_t attempt = 0; attempt < 4000000 && out.size() < wanted; ++attempt) {
            const uint64_t tie = (engine() >> 10) | (1ULL << 53) | 1ULL;
            const uint64_t shift = 53 + (engine() & 1ULL);
            const int64_t delta = static_cast<int64_t>(engine() % 33) - 16;
            if (delta == 0) {
                continue;
            }
            const uint64_t modulus_mask = (1ULL << shift) - 1ULL;
            const uint64_t divisor = (static_cast<uint64_t>(delta) * inverse_mod_2_64(tie)) & modulus_mask;
            if (divisor < (1ULL << 52) || divisor >= (1ULL << 53)) {
                continue;
            }
            Wide product = multiply(tie, divisor);
            if (delta > 0) {
                const uint64_t borrow = product.lo < static_cast<uint64_t>(delta) ? 1ULL : 0ULL;
                product.lo -= static_cast<uint64_t>(delta);
                product.hi -= borrow;
            } else {
                const uint64_t added = product.lo + static_cast<uint64_t>(-delta);
                product.hi += added < product.lo ? 1ULL : 0ULL;
                product.lo = added;
            }
            if ((product.lo & modulus_mask) != 0) {
                continue;
            }
            const uint64_t dividend = (product.hi << (64 - shift)) | (product.lo >> shift);
            if (dividend < (1ULL << 52) || dividend >= (1ULL << 53)) {
                continue;
            }
            const uint64_t draw = engine();
            const uint64_t field_a = 1023 - 10 + (draw % 21);
            const uint64_t field_b = 1023 - 10 + ((draw >> 8) % 21);
            out.emplace_back(make((draw >> 16) & 1ULL, field_a, dividend), make((draw >> 17) & 1ULL, field_b, divisor));
        }
        return out;
    }

    std::vector<double> square_root_near_ties(size_t wanted, uint64_t seed)
    {
        std::mt19937_64 engine(seed);
        std::vector<double> out;
        constexpr uint64_t low54 = (1ULL << 54) - 1ULL;
        constexpr uint64_t closeness = 1ULL << 46;
        for (size_t attempt = 0; attempt < 4000000 && out.size() < wanted; ++attempt) {
            const uint64_t root = (engine() >> 10) | (1ULL << 53) | 1ULL;
            const Wide square = multiply(root, root);
            const uint64_t fraction = square.lo & low54;
            const bool below = fraction < closeness;
            const bool above = fraction > low54 - closeness;
            if (!below && !above) {
                continue;
            }
            uint64_t radicand = (square.hi << 10) | (square.lo >> 54);
            if (above) {
                ++radicand;
            }
            const int64_t even = 2 * (static_cast<int64_t>(engine() % 41) - 20);
            if (radicand < (1ULL << 53)) {
                out.push_back(make(0, static_cast<uint64_t>(1023 + even), radicand));
            } else if ((radicand & 1ULL) == 0) {
                out.push_back(make(0, static_cast<uint64_t>(1023 + even + 1), radicand >> 1));
            }
        }
        return out;
    }

    std::vector<std::pair<double, double>> boundary_pairs(WordPairOp op, size_t count, uint64_t seed)
    {
        std::mt19937_64 engine(seed);
        std::vector<std::pair<double, double>> out;
        while (out.size() < count) {
            const uint64_t draw = engine();
            const bool low_edge = (draw & 1ULL) != 0;
            const int64_t target = low_edge ? -70 + static_cast<int64_t>((draw >> 1) % 76) : 2040 + static_cast<int64_t>((draw >> 1) % 17);
            const int64_t field_a = static_cast<int64_t>((draw >> 8) % 2047);
            const int64_t field_b = op == WordPairOp::Mul ? target - field_a + 1023 : field_a - target + 1023;
            if (field_b < 0 || field_b > 2046) {
                continue;
            }
            const uint64_t more = engine();
            out.emplace_back(make(more >> 63, static_cast<uint64_t>(field_a), more), make((more >> 62) & 1ULL, static_cast<uint64_t>(field_b), engine()));
        }
        return out;
    }

    template <typename Fixture>
    class WordPairMapGpuTest : public Fixture { };

    using HardwareOptions = ::testing::Types<GpuDispatchFixture<true>, GpuDispatchFixture<false>>;
    TYPED_TEST_SUITE(WordPairMapGpuTest, HardwareOptions);

    TYPED_TEST(WordPairMapGpuTest, EveryOperationMatchesTheHostOnRandomBitPatterns)
    {
        const auto a = random_bit_patterns(1U << 18, 11);
        const auto b = random_bit_patterns(1U << 18, 12);
        for (const WordPairOp op : { WordPairOp::Add, WordPairOp::Mul, WordPairOp::Div, WordPairOp::Sqrt }) {
            expect_exact(op, a, b);
        }
    }

    TYPED_TEST(WordPairMapGpuTest, EveryOperationMatchesTheHostOnRandomModerateExponents)
    {
        const auto a = random_moderate(1U << 18, 21);
        const auto b = random_moderate(1U << 18, 22);
        for (const WordPairOp op : { WordPairOp::Add, WordPairOp::Mul, WordPairOp::Div, WordPairOp::Sqrt }) {
            expect_exact(op, a, b);
        }
    }

    TYPED_TEST(WordPairMapGpuTest, EveryOperationMatchesTheHostOnEveryPairOfSpecialValues)
    {
        const auto values = special_values();
        std::vector<double> a;
        std::vector<double> b;
        cross(values, values, a, b);
        for (const WordPairOp op : { WordPairOp::Add, WordPairOp::Mul, WordPairOp::Div }) {
            expect_exact(op, a, b);
        }
        expect_exact(WordPairOp::Sqrt, values);
    }

    TYPED_TEST(WordPairMapGpuTest, EveryOperationMatchesTheHostAcrossTheWholeExponentRange)
    {
        const std::vector<uint64_t> partner_fields { 0, 1, 1000, 1023, 1075, 2046 };
        const auto sweep = exponent_sweep(every_field());
        const auto partners = exponent_sweep(partner_fields);
        std::vector<double> a;
        std::vector<double> b;
        cross(sweep, partners, a, b);
        for (const WordPairOp op : { WordPairOp::Add, WordPairOp::Mul, WordPairOp::Div }) {
            expect_exact(op, a, b);
        }
        expect_exact(WordPairOp::Sqrt, sweep);
    }

    TYPED_TEST(WordPairMapGpuTest, ProductsAndQuotientsAtTheUnderflowAndOverflowEdgesMatchTheHost)
    {
        expect_exact_pairs(WordPairOp::Mul, boundary_pairs(WordPairOp::Mul, 1U << 17, 31));
        expect_exact_pairs(WordPairOp::Div, boundary_pairs(WordPairOp::Div, 1U << 17, 32));
    }

    TYPED_TEST(WordPairMapGpuTest, QuotientsWithinAFractionOfAnUlpOfATieRoundLikeTheHost)
    {
        const auto pairs = division_near_ties(1U << 15, 41);
        ASSERT_GE(pairs.size(), 1024U);
        expect_exact_pairs(WordPairOp::Div, pairs);
    }

    TYPED_TEST(WordPairMapGpuTest, RootsWithinAFractionOfAnUlpOfATieRoundLikeTheHost)
    {
        const auto values = square_root_near_ties(1U << 14, 51);
        ASSERT_GE(values.size(), 512U);
        expect_exact(WordPairOp::Sqrt, values);
    }

    TYPED_TEST(WordPairMapGpuTest, ExactQuotientsAndExactRootsAreExact)
    {
        std::mt19937_64 engine(61);
        std::vector<double> products;
        std::vector<double> divisors;
        std::vector<double> squares;
        for (size_t i = 0; i < (1U << 16); ++i) {
            const double p = static_cast<double>(1 + engine() % ((1ULL << 26) - 1));
            const double q = static_cast<double>(1 + engine() % ((1ULL << 26) - 1));
            products.push_back(p * q);
            divisors.push_back(q);
            squares.push_back(p * p);
        }
        expect_exact(WordPairOp::Div, products, divisors);
        expect_exact(WordPairOp::Sqrt, squares);

        auto pass = make_pass(WordPairOp::Sqrt);
        const auto roots = run(pass, squares, {});
        ASSERT_EQ(roots.size(), squares.size());
        for (size_t i = 0; i < squares.size(); ++i) {
            ASSERT_EQ(roots[i], std::sqrt(squares[i]));
            ASSERT_EQ(roots[i] * roots[i], squares[i]);
        }
    }

    TYPED_TEST(WordPairMapGpuTest, SumsOfNearlyOppositeValuesCancelLikeTheHost)
    {
        std::mt19937_64 engine(71);
        std::vector<double> a;
        std::vector<double> b;
        for (size_t i = 0; i < (1U << 17); ++i) {
            const uint64_t draw = engine();
            const double value = make(0, 1023 - 60 + (draw % 121), draw >> 8);
            const int64_t step = static_cast<int64_t>((draw >> 3) % 9) - 4;
            const uint64_t neighbour = static_cast<uint64_t>(static_cast<int64_t>(bits_of(value)) + step);
            a.push_back(value);
            b.push_back(-std::bit_cast<double>(neighbour));
        }
        expect_exact(WordPairOp::Add, a, b);
    }

    TYPED_TEST(WordPairMapGpuTest, ACountSmallerThanOneWorkgroupLeavesTheGuardedThreadsIdle)
    {
        auto pass = make_pass(WordPairOp::Div);
        const std::vector<double> a { 1.0, 2.0, 3.0 };
        const std::vector<double> b { 4.0, 5.0, 6.0 };
        pass.context->record_stages({ word_pair_map_stage(*pass.context, pass.config, WordPairOp::Div, a, b) });
        const auto got = pass.context->download_words(2, 3);
        ASSERT_EQ(got.size(), 3U);
        EXPECT_EQ(got[0], 0.25);
        EXPECT_EQ(got[1], 0.4);
        EXPECT_EQ(got[2], 0.5);
    }

    bool same_float(float got, float want)
    {
        return (std::isnan(got) && std::isnan(want)) || std::bit_cast<uint32_t>(got) == std::bit_cast<uint32_t>(want);
    }

    void expect_float_conversion(const std::vector<double>& values)
    {
        ASSERT_GT(values.size(), 0U);
        auto pass = make_pass(WordPairOp::ToFloat32);
        pass.context->record_stages({ word_pair_map_stage(*pass.context, pass.config, WordPairOp::ToFloat32, values, std::span<const double> {}) });
        const auto got = Yantra::read_float_results(*pass.context, values.size());

        ASSERT_EQ(got.size(), values.size());
        size_t wrong = 0;
        std::string report;
        for (size_t i = 0; i < values.size(); ++i) {
            const auto want = static_cast<float>(values[i]);
            if (!same_float(got[i], want)) {
                if (wrong < 8) {
                    char text[80];
                    std::snprintf(text, sizeof(text), "  a %s  got %08x  want %08x\n", hex(values[i]).c_str(),
                        std::bit_cast<uint32_t>(got[i]), std::bit_cast<uint32_t>(want));
                    report += text;
                }
                ++wrong;
            }
        }
        EXPECT_EQ(wrong, 0U) << wrong << " of " << values.size() << " differ from the host conversion, first:\n"
                             << report;
    }

    std::vector<double> float_rounding_values(size_t count, uint64_t seed)
    {
        std::mt19937_64 engine(seed);
        std::vector<double> out;
        while (out.size() < count) {
            const uint64_t draw = engine();
            const uint64_t field = 840 + draw % 321;
            const uint64_t drop = field >= 897 ? 29 : 926 - field;
            if (drop > 53) {
                continue;
            }
            const uint64_t below = (1ULL << drop) - 1ULL;
            uint64_t tail = 0;
            switch ((draw >> 12) % 4) {
            case 0:
                tail = 0;
                break;
            case 1:
                tail = 1;
                break;
            case 2:
                tail = below >> 1;
                break;
            default:
                tail = engine() & (below >> 1);
                break;
            }
            uint64_t full = (engine() & ~below) | (1ULL << (drop - 1)) | tail;
            full |= 1ULL << 52;
            full &= (1ULL << 53) - 1ULL;
            out.push_back(make((draw >> 20) & 1ULL, field, full));
        }
        return out;
    }

    TYPED_TEST(WordPairMapGpuTest, AFloatConversionMatchesTheHostOnRandomBitPatterns)
    {
        expect_float_conversion(random_bit_patterns(1U << 18, 81));
        expect_float_conversion(random_moderate(1U << 18, 82));
    }

    TYPED_TEST(WordPairMapGpuTest, AFloatConversionMatchesTheHostOnSpecialValuesAndEveryExponent)
    {
        expect_float_conversion(special_values());
        expect_float_conversion(exponent_sweep(every_field()));
    }

    TYPED_TEST(WordPairMapGpuTest, AFloatConversionRoundsTiesAndNearTiesLikeTheHostAtEveryPosition)
    {
        expect_float_conversion(float_rounding_values(1U << 18, 83));
    }

    TYPED_TEST(WordPairMapGpuTest, AFloatConversionHandlesTheOverflowAndSubnormalBoundaries)
    {
        constexpr double float_max = std::numeric_limits<float>::max();
        const double max_ulp = std::ldexp(1.0, 104);
        const double min_normal = std::numeric_limits<float>::min();
        const double min_subnormal = std::numeric_limits<float>::denorm_min();
        std::vector<double> values;
        for (const double sign : { 1.0, -1.0 }) {
            for (const double base : { float_max, float_max + max_ulp / 2.0, float_max + max_ulp / 2.0 + 1.0e20,
                     float_max + max_ulp, min_normal, min_normal / 2.0, min_normal * (1.0 - 1.0e-9),
                     min_subnormal, min_subnormal / 2.0, min_subnormal * 0.5000001, min_subnormal * 0.4999999,
                     min_subnormal * 1.5, min_subnormal * 2.5, 1.0e-46, 1.0e-45, 1.4e-45, 1.0e300, 1.0e-300 }) {
                values.push_back(sign * base);
                values.push_back(sign * std::nextafter(base, 0.0));
                values.push_back(sign * std::nextafter(base, 1.0e308));
            }
        }
        expect_float_conversion(values);
    }

    TYPED_TEST(WordPairMapGpuTest, AFloatResultCanBeSharedWithAnotherContext)
    {
        auto pass = make_pass(WordPairOp::ToFloat32);
        const auto values = random_moderate(5000, 84);
        pass.context->ensure_shared_buffer(0, 1, values.size(), Yantra::GpuBufferBinding::ElementType::FLOAT32);

        pass.context->record_stages({ word_pair_map_stage(*pass.context, pass.config, WordPairOp::ToFloat32, values, std::span<const double> {}) });
        const auto got = Yantra::read_float_results(*pass.context, values.size());

        ASSERT_EQ(got.size(), values.size());
        for (size_t i = 0; i < values.size(); ++i) {
            ASSERT_TRUE(same_float(got[i], static_cast<float>(values[i]))) << "element " << i;
        }
        const auto info = pass.context->shared_buffer_info(0, 1);
        ASSERT_TRUE(info.has_value());
        EXPECT_EQ(info->element_type, Yantra::GpuBufferBinding::ElementType::FLOAT32);
        EXPECT_GE(info->element_count, values.size());
    }

} // namespace
} // namespace MayaFlux::Test
