#include "gtest/gtest.h"

#include <bit>
#include <random>

#include "../../support/gpu_dispatch_test_support.hpp"

#include "MayaFlux/Kinesis/Discrete/OrderKey.hpp"
#include "MayaFlux/Yantra/Executors/Audio/SegmentedReductionContext.hpp"

namespace MayaFlux::Test {
namespace {

    using namespace GpuDispatch;
    using Portal::Graphics::KernelOp;
    using Portal::Graphics::ReductionMap;
    using Yantra::SegmentedReductionContext;
    using Yantra::SegmentGeometry;

    constexpr double k_inf = std::numeric_limits<double>::infinity();
    constexpr double k_nan = std::numeric_limits<double>::quiet_NaN();
    constexpr double k_min_normal = std::numeric_limits<double>::min();
    constexpr double k_min_subnormal = std::numeric_limits<double>::denorm_min();

    ::testing::AssertionResult same_double(double got, double want)
    {
        if (std::isnan(want)) {
            return std::isnan(got) ? ::testing::AssertionSuccess()
                                   : ::testing::AssertionFailure() << "got " << got << ", wanted NaN";
        }
        if (got == want) {
            return ::testing::AssertionSuccess();
        }
        return ::testing::AssertionFailure()
            << "got " << got << " (0x" << std::hex << std::bit_cast<uint64_t>(got) << "), wanted " << want
            << " (0x" << std::bit_cast<uint64_t>(want) << ")";
    }

    SegmentGeometry tiled(uint32_t length, uint32_t count)
    {
        return { .length = length, .stride = length, .offset = 0, .count = count };
    }

    std::vector<double> counting(size_t count, double first = 1.0)
    {
        std::vector<double> out(count);
        std::iota(out.begin(), out.end(), first);
        return out;
    }

    class RandomDoubles {
    public:
        explicit RandomDoubles(uint64_t seed)
            : m_engine(seed)
        {
        }

        double any_finite()
        {
            return build(random_sign(), 1 + (m_engine() % 2046), m_engine());
        }

        double near_exponent(double base, int spread)
        {
            const auto bits = std::bit_cast<uint64_t>(base);
            const int exponent = static_cast<int>((bits >> 52U) & 0x7FFU);
            const int delta = static_cast<int>(m_engine() % static_cast<uint64_t>(2 * spread + 1)) - spread;
            const int clamped = std::clamp(exponent + delta, 1, 2046);
            return build(random_sign(), static_cast<uint64_t>(clamped), m_engine());
        }

        double unit()
        {
            return std::uniform_real_distribution<double>(-1.0, 1.0)(m_engine);
        }

    private:
        static double build(uint64_t sign, uint64_t exponent, uint64_t mantissa)
        {
            return std::bit_cast<double>((sign << 63U) | (exponent << 52U) | (mantissa & 0xFFFFFFFFFFFFFULL));
        }

        uint64_t random_sign() { return m_engine() & 1U; }

        std::mt19937_64 m_engine;
    };

    std::vector<double> edge_values()
    {
        return {
            0.0, 1.0, -1.0, 3.0, 0.1, 0.2, 1.0e300, -1.0e300, 1.0e-300, k_min_normal, -k_min_normal,
            k_min_normal / 2.0, 1.0e-310, k_min_subnormal, -k_min_subnormal, 1.0 + 0x1.0p-52,
            0x1.0p-53, 0x1.8p-53, 0x1.0p-537, 0x1.8p-537, 0x1.0p-538, 0x1.fffffffffffffp-538,
            1.7976931348623157e308, k_inf, -k_inf
        };
    }

    template <typename Fixture>
    class SegmentedReductionGpuTest : public Fixture { };

    using HardwareOptions = ::testing::Types<GpuDispatchFixture<true>, GpuDispatchFixture<false>>;
    TYPED_TEST_SUITE(SegmentedReductionGpuTest, HardwareOptions);

    TYPED_TEST(SegmentedReductionGpuTest, SumOfIntegersIsExactAcrossTiledSegments)
    {
        SegmentedReductionContext context(KernelOp::Sum);
        const auto values = counting(1000);

        const auto sums = context.reduce(values, tiled(100, 10));

        ASSERT_EQ(sums.size(), 10U);
        for (size_t s = 0; s < sums.size(); ++s) {
            const auto first = static_cast<double>(s * 100 + 1);
            EXPECT_TRUE(same_double(sums[s], 100.0 * first + 4950.0)) << "segment " << s;
        }
    }

    TYPED_TEST(SegmentedReductionGpuTest, OffsetAndStrideSelectOverlappingSegments)
    {
        SegmentedReductionContext context(KernelOp::Sum);
        const auto values = counting(100, 0.0);

        const auto sums = context.reduce(values, { .length = 10, .stride = 5, .offset = 3, .count = 4 });

        ASSERT_EQ(sums.size(), 4U);
        for (uint32_t s = 0; s < 4; ++s) {
            double expected = 0.0;
            for (uint32_t i = 0; i < 10; ++i) {
                expected += values[static_cast<size_t>(3) + static_cast<size_t>(s) * 5 + i];
            }
            EXPECT_TRUE(same_double(sums[s], expected)) << "segment " << s;
        }
    }

    TYPED_TEST(SegmentedReductionGpuTest, SegmentsLongerThanTheWorkgroupAreFoldedByStride)
    {
        SegmentedReductionContext wide(KernelOp::Sum, ReductionMap::Identity, 64);
        SegmentedReductionContext narrow(KernelOp::Sum, ReductionMap::Identity, 2);
        const auto values = counting(5000);

        EXPECT_TRUE(same_double(wide.reduce(values, tiled(5000, 1)).at(0), 5000.0 * 5001.0 / 2.0));
        EXPECT_TRUE(same_double(narrow.reduce(values, tiled(5000, 1)).at(0), 5000.0 * 5001.0 / 2.0));
    }

    TYPED_TEST(SegmentedReductionGpuTest, AnEmptySegmentYieldsTheIdentityOfItsCombine)
    {
        SegmentedReductionContext sum(KernelOp::Sum);
        SegmentedReductionContext max(KernelOp::Max);
        SegmentedReductionContext min(KernelOp::Min);
        const auto values = counting(8);

        EXPECT_TRUE(same_double(sum.reduce(values, tiled(0, 1)).at(0), 0.0));
        EXPECT_TRUE(same_double(max.reduce(values, tiled(0, 1)).at(0), -k_inf));
        EXPECT_TRUE(same_double(min.reduce(values, tiled(0, 1)).at(0), k_nan));
    }

    TYPED_TEST(SegmentedReductionGpuTest, NoSegmentsReturnsNothing)
    {
        SegmentedReductionContext context(KernelOp::Sum);
        const auto values = counting(8);

        EXPECT_TRUE(context.reduce(values, tiled(4, 0)).empty());
    }

    TYPED_TEST(SegmentedReductionGpuTest, MaxAndMinFollowTheOrderKeyOnMixedValues)
    {
        SegmentedReductionContext max(KernelOp::Max);
        SegmentedReductionContext min(KernelOp::Min);
        RandomDoubles random(7);
        std::vector<double> values;
        values.reserve(1200);
        for (int i = 0; i < 4 * 300; ++i) {
            values.push_back(random.any_finite());
        }
        values[10] = k_inf;
        values[450] = -k_inf;

        const auto highs = max.reduce(values, tiled(300, 4));
        const auto lows = min.reduce(values, tiled(300, 4));

        for (size_t s = 0; s < 4; ++s) {
            const auto begin = values.begin() + static_cast<std::ptrdiff_t>(s) * 300;
            const auto by_key = [](double a, double b) { return Kinesis::Discrete::order_key(a) < Kinesis::Discrete::order_key(b); };
            EXPECT_TRUE(same_double(highs[s], *std::max_element(begin, begin + 300, by_key))) << "segment " << s;
            EXPECT_TRUE(same_double(lows[s], *std::min_element(begin, begin + 300, by_key))) << "segment " << s;
        }
    }

    TYPED_TEST(SegmentedReductionGpuTest, NegativeZeroAndPositiveZeroOrderAsEqual)
    {
        SegmentedReductionContext max(KernelOp::Max);
        const std::vector<double> values { -0.0, 0.0, -0.0, 0.0 };

        EXPECT_TRUE(same_double(max.reduce(values, tiled(4, 1)).at(0), 0.0));
    }

    TYPED_TEST(SegmentedReductionGpuTest, NaNIsTheLargestAndMinIgnoresItUnlessItIsAlone)
    {
        SegmentedReductionContext max(KernelOp::Max);
        SegmentedReductionContext min(KernelOp::Min);
        const std::vector<double> values { 1.0, k_nan, -5.0, 2.0, k_nan, k_nan };

        EXPECT_TRUE(same_double(max.reduce(values, tiled(3, 1)).at(0), k_nan));
        EXPECT_TRUE(same_double(min.reduce(values, tiled(3, 1)).at(0), -5.0));
        EXPECT_TRUE(same_double(min.reduce(std::span<const double>(values).subspan(4), tiled(2, 1)).at(0), k_nan));
    }

    TYPED_TEST(SegmentedReductionGpuTest, AbsMapFindsTheLargestMagnitude)
    {
        SegmentedReductionContext context(KernelOp::Max, ReductionMap::Abs);
        const std::vector<double> values { 3.0, -9.5, 7.0, -0.25, 9.0, 0.0, -k_min_subnormal, 4.0 };

        const auto peaks = context.reduce(values, tiled(4, 2));

        EXPECT_TRUE(same_double(peaks[0], 9.5));
        EXPECT_TRUE(same_double(peaks[1], 9.0));
    }

    TYPED_TEST(SegmentedReductionGpuTest, SumOfSquaresOfIntegersIsExact)
    {
        SegmentedReductionContext context(KernelOp::Sum, ReductionMap::Square);
        const auto values = counting(100);

        EXPECT_TRUE(same_double(context.reduce(values, tiled(100, 1)).at(0), 100.0 * 101.0 * 201.0 / 6.0));
    }

    TYPED_TEST(SegmentedReductionGpuTest, SquaringMatchesHardwareMultiplyOnEdgeValues)
    {
        SegmentedReductionContext context(KernelOp::Sum, ReductionMap::Square);
        auto values = edge_values();
        values.push_back(k_nan);
        values.push_back(-0.0);

        const auto squares = context.reduce(values, tiled(1, static_cast<uint32_t>(values.size())));

        ASSERT_EQ(squares.size(), values.size());
        for (size_t i = 0; i < values.size(); ++i) {
            EXPECT_TRUE(same_double(squares[i], values[i] * values[i])) << "value " << values[i];
        }
    }

    TYPED_TEST(SegmentedReductionGpuTest, SquaringMatchesHardwareMultiplyOnRandomValues)
    {
        SegmentedReductionContext context(KernelOp::Sum, ReductionMap::Square);
        RandomDoubles random(11);
        std::vector<double> values;
        values.reserve(4000);
        for (int i = 0; i < 4000; ++i) {
            values.push_back(i % 2 == 0 ? random.any_finite() : random.near_exponent(1.0, 400));
        }

        const auto squares = context.reduce(values, tiled(1, static_cast<uint32_t>(values.size())));

        for (size_t i = 0; i < values.size(); ++i) {
            ASSERT_TRUE(same_double(squares[i], values[i] * values[i])) << "value " << values[i];
        }
    }

    TYPED_TEST(SegmentedReductionGpuTest, AddingTwoValuesMatchesHardwareAdditionOnEdgeValues)
    {
        SegmentedReductionContext context(KernelOp::Sum);
        std::vector<double> values;
        std::vector<double> expected;
        const auto edges = edge_values();
        values.reserve(2 * edges.size() * edges.size());
        expected.reserve(edges.size() * edges.size());
        for (const double a : edges) {
            for (const double b : edges) {
                values.push_back(a);
                values.push_back(b);
                expected.push_back(a + b);
            }
        }

        const auto sums = context.reduce(values, tiled(2, static_cast<uint32_t>(expected.size())));

        ASSERT_EQ(sums.size(), expected.size());
        for (size_t i = 0; i < expected.size(); ++i) {
            EXPECT_TRUE(same_double(sums[i], expected[i])) << values[2 * i] << " + " << values[2 * i + 1];
        }
    }

    TYPED_TEST(SegmentedReductionGpuTest, AddingTwoValuesMatchesHardwareAdditionOnRandomPairs)
    {
        SegmentedReductionContext context(KernelOp::Sum);
        RandomDoubles random(23);
        std::vector<double> values;
        std::vector<double> expected;
        values.reserve(8000);
        expected.reserve(4000);
        for (int i = 0; i < 4000; ++i) {
            const double a = random.any_finite();
            const double b = i % 4 == 0 ? random.any_finite() : random.near_exponent(a, 60);
            values.push_back(a);
            values.push_back(i % 7 == 0 ? std::nextafter(-a, 0.0) : b);
            expected.push_back(values.back() + a);
        }

        const auto sums = context.reduce(values, tiled(2, static_cast<uint32_t>(expected.size())));

        for (size_t i = 0; i < expected.size(); ++i) {
            ASSERT_TRUE(same_double(sums[i], expected[i])) << values[2 * i] << " + " << values[2 * i + 1];
        }
    }

    TYPED_TEST(SegmentedReductionGpuTest, InfinitiesAndNaNPropagateThroughASum)
    {
        SegmentedReductionContext context(KernelOp::Sum);
        const std::vector<double> values { 1.0, k_inf, 2.0, k_nan, k_inf, -k_inf, -k_inf, 4.0 };

        const auto sums = context.reduce(values, tiled(2, 4));

        EXPECT_TRUE(same_double(sums[0], k_inf));
        EXPECT_TRUE(same_double(sums[1], k_nan));
        EXPECT_TRUE(same_double(sums[2], k_nan));
        EXPECT_TRUE(same_double(sums[3], -k_inf));
    }

    TYPED_TEST(SegmentedReductionGpuTest, ReducingFewerSegmentsAfterMoreReturnsOnlyTheRequestedCount)
    {
        SegmentedReductionContext context(KernelOp::Sum);
        const auto values = counting(300);

        const auto many = context.reduce(values, tiled(100, 3));
        const auto few = context.reduce(values, tiled(150, 2));
        const auto one = context.reduce(values, tiled(300, 1));

        ASSERT_EQ(many.size(), 3U);
        ASSERT_EQ(few.size(), 2U);
        ASSERT_EQ(one.size(), 1U);
        EXPECT_TRUE(same_double(few[0], 150.0 * 151.0 / 2.0));
        EXPECT_TRUE(same_double(few[1], 300.0 * 301.0 / 2.0 - 150.0 * 151.0 / 2.0));
        EXPECT_TRUE(same_double(one[0], 300.0 * 301.0 / 2.0));
    }

    TYPED_TEST(SegmentedReductionGpuTest, AStagedReductionRunsThroughTheAsynchronousPathAndCanBeRestaged)
    {
        SegmentedReductionContext context(KernelOp::Sum, ReductionMap::Square);
        const auto values = counting(300);

        const auto run = [&context, &values](const SegmentGeometry& geometry) {
            context.stage(values, geometry);
            const auto fence = context.dispatch_async(Yantra::Datum<std::vector<Kakshya::DataVariant>> {});
            EXPECT_NE(fence, Portal::Graphics::INVALID_FENCE);
            foundry().wait_for_fence(fence);
            return context.decode_primary(context.collect_result());
        };

        const auto expected = [&values](const SegmentGeometry& geometry) {
            std::vector<double> sums;
            for (uint32_t s = 0; s < geometry.count; ++s) {
                double sum = 0.0;
                for (uint32_t i = 0; i < geometry.length; ++i) {
                    const double x = values[static_cast<size_t>(geometry.offset) + static_cast<size_t>(s) * geometry.stride + i];
                    sum += x * x;
                }
                sums.push_back(sum);
            }
            return sums;
        };

        const SegmentGeometry three = tiled(100, 3);
        const SegmentGeometry two = tiled(150, 2);

        const auto first = run(three);
        const auto second = run(two);

        ASSERT_EQ(first.size(), 3U);
        ASSERT_EQ(second.size(), 2U);
        for (size_t i = 0; i < first.size(); ++i) {
            EXPECT_TRUE(same_double(first[i], expected(three)[i])) << "segment " << i;
        }
        for (size_t i = 0; i < second.size(); ++i) {
            EXPECT_TRUE(same_double(second[i], expected(two)[i])) << "segment " << i;
        }
    }

    TYPED_TEST(SegmentedReductionGpuTest, SumOfRandomSamplesAgreesWithTheHostWithinRounding)
    {
        SegmentedReductionContext context(KernelOp::Sum);
        RandomDoubles random(5);
        constexpr uint32_t length = 1000;
        constexpr uint32_t count = 6;
        std::vector<double> values;
        values.reserve(static_cast<size_t>(length) * count);
        for (uint32_t i = 0; i < length * count; ++i) {
            values.push_back(random.unit());
        }

        const auto sums = context.reduce(values, tiled(length, count));

        for (uint32_t s = 0; s < count; ++s) {
            const auto begin = values.begin() + static_cast<std::ptrdiff_t>(s) * length;
            const double expected = std::accumulate(begin, begin + length, 0.0);
            const double magnitude = std::accumulate(begin, begin + length, 0.0, [](double acc, double x) { return acc + std::abs(x); });
            EXPECT_NEAR(sums[s], expected, 4.0 * length * std::numeric_limits<double>::epsilon() * magnitude) << "segment " << s;
        }
    }

    TYPED_TEST(SegmentedReductionGpuTest, RootMeanSquareOfASineMatchesTheHost)
    {
        SegmentedReductionContext context(KernelOp::Sum, ReductionMap::Square);
        constexpr uint32_t length = 1024;
        std::vector<double> values(length);
        for (uint32_t i = 0; i < length; ++i) {
            values[i] = 0.5 * std::sin(2.0 * std::numbers::pi * 8.0 * static_cast<double>(i) / length);
        }

        const double energy = context.reduce(values, tiled(length, 1)).at(0);
        const double expected = std::inner_product(values.begin(), values.end(), values.begin(), 0.0);

        EXPECT_NEAR(energy, expected, 1.0e-10);
        EXPECT_NEAR(std::sqrt(energy / length), 0.5 / std::numbers::sqrt2, 1.0e-10);
    }

} // namespace
} // namespace MayaFlux::Test
