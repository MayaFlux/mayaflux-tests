#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kinesis/Temporal/TimeSampling.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

using Kinesis::FoldedTime;
using Kinesis::RationalRate;
using Kinesis::TimeLocation;
using Kinesis::TimePlacement;
using Kinesis::TimeSampling;
using Kinesis::TimeWrap;

namespace {

    constexpr double k_inf = std::numeric_limits<double>::infinity();
    constexpr double k_nan = std::numeric_limits<double>::quiet_NaN();

    TimeSampling must(std::optional<TimeSampling> sampling)
    {
        if (!sampling) {
            throw std::runtime_error("sampling was rejected");
        }
        return *sampling;
    }

    class Lcg {
    public:
        explicit Lcg(uint64_t seed)
            : m_state(seed)
        {
        }

        double unit()
        {
            m_state = m_state * 6364136223846793005ULL + 1442695040888963407ULL;
            return static_cast<double>(m_state >> 11U) / 9007199254740992.0;
        }

    private:
        uint64_t m_state;
    };

    size_t scan_floor(const TimeSampling& s, double t, size_t count)
    {
        size_t best = 0;
        for (size_t i = 0; i < count; ++i) {
            if (*s.time(i) <= t) {
                best = i;
            }
        }
        return best;
    }

    size_t scan_ceil(const TimeSampling& s, double t, size_t count)
    {
        for (size_t i = 0; i < count; ++i) {
            if (*s.time(i) >= t) {
                return i;
            }
        }
        return count - 1;
    }

    size_t scan_nearest(const TimeSampling& s, double t, size_t count)
    {
        size_t best = 0;
        double best_distance = k_inf;
        for (size_t i = 0; i < count; ++i) {
            const double distance = std::abs(t - *s.time(i));
            if (distance < best_distance) {
                best_distance = distance;
                best = i;
            }
        }
        return best;
    }

    TEST(TimeSamplingFactoryTest, UniformRejectsStepsThatCannotAdvanceTime)
    {
        EXPECT_FALSE(TimeSampling::uniform(0.0).has_value());
        EXPECT_FALSE(TimeSampling::uniform(-1.0).has_value());
        EXPECT_FALSE(TimeSampling::uniform(k_nan).has_value());
        EXPECT_FALSE(TimeSampling::uniform(k_inf).has_value());
        EXPECT_FALSE(TimeSampling::uniform(1.0, k_nan).has_value());
        EXPECT_FALSE(TimeSampling::uniform(1.0, k_inf).has_value());
        EXPECT_FALSE(TimeSampling::uniform(1.0, -k_inf).has_value());
        EXPECT_FALSE(TimeSampling::uniform(1.0, 1.0e16).has_value());
        EXPECT_TRUE(TimeSampling::uniform(1.0, 1.0e15).has_value());
        EXPECT_TRUE(TimeSampling::uniform(std::numeric_limits<double>::denorm_min()).has_value());
    }

    TEST(TimeSamplingFactoryTest, RateRejectsAZeroPart)
    {
        EXPECT_FALSE(TimeSampling::uniform_rate({ 0, 1 }).has_value());
        EXPECT_FALSE(TimeSampling::uniform_rate({ 1, 0 }).has_value());
        EXPECT_FALSE(TimeSampling::uniform_rate({ 0, 0 }).has_value());
    }

    TEST(TimeSamplingFactoryTest, RateIsStoredReducedSoEqualRatesCompareEqual)
    {
        const auto reduced = must(TimeSampling::uniform_rate({ 24000, 1001 }));
        const auto doubled = must(TimeSampling::uniform_rate({ 48000, 2002 }));

        EXPECT_EQ(doubled.exact_rate(), (std::optional<RationalRate> { RationalRate { 24000, 1001 } }));
        EXPECT_EQ(reduced, doubled);
        EXPECT_NE(reduced, must(TimeSampling::uniform_rate({ 24000, 1001 }, 1)));
        EXPECT_NE(reduced, must(TimeSampling::uniform(1001.0 / 24000.0)));
    }

    TEST(TimeSamplingFactoryTest, CyclicRejectsTimesThatCannotBeOrdered)
    {
        EXPECT_FALSE(TimeSampling::cyclic({ 0.0 }, 1.0).has_value());
        EXPECT_FALSE(TimeSampling::cyclic({}, 1.0).has_value());
        EXPECT_FALSE(TimeSampling::cyclic({ 0.0, 0.0 }, 1.0).has_value());
        EXPECT_FALSE(TimeSampling::cyclic({ 0.5, 0.1 }, 1.0).has_value());
        EXPECT_FALSE(TimeSampling::cyclic({ 0.0, k_nan }, 1.0).has_value());
        EXPECT_FALSE(TimeSampling::cyclic({ 0.0, k_inf }, 1.0).has_value());
        EXPECT_FALSE(TimeSampling::cyclic({ 0.0, 0.5 }, 0.0).has_value());
        EXPECT_FALSE(TimeSampling::cyclic({ 0.0, 0.5 }, -1.0).has_value());
        EXPECT_FALSE(TimeSampling::cyclic({ 0.0, 0.5 }, k_nan).has_value());
        EXPECT_FALSE(TimeSampling::cyclic({ 0.0, 0.5 }, k_inf).has_value());
    }

    TEST(TimeSamplingFactoryTest, CyclicRequiresTheLastTimeToFallBeforeTheNextCycle)
    {
        EXPECT_FALSE(TimeSampling::cyclic({ 0.0, 1.0 }, 1.0).has_value());
        EXPECT_FALSE(TimeSampling::cyclic({ 0.0, 1.5 }, 1.0).has_value());
        EXPECT_TRUE(TimeSampling::cyclic({ 0.0, 0.9999 }, 1.0).has_value());
        EXPECT_TRUE(TimeSampling::cyclic({ 5.0, 5.5 }, 2.0).has_value());
    }

    TEST(TimeSamplingFactoryTest, AcyclicRejectsEmptyUnorderedAndNonFiniteLists)
    {
        EXPECT_FALSE(TimeSampling::acyclic({}).has_value());
        EXPECT_FALSE(TimeSampling::acyclic({ 0.0, 0.0 }).has_value());
        EXPECT_FALSE(TimeSampling::acyclic({ 1.0, 0.0 }).has_value());
        EXPECT_FALSE(TimeSampling::acyclic({ 0.0, k_nan }).has_value());
        EXPECT_FALSE(TimeSampling::acyclic({ 0.0, k_inf }).has_value());
        EXPECT_FALSE(TimeSampling::acyclic({ -k_inf, 0.0 }).has_value());
        EXPECT_TRUE(TimeSampling::acyclic({ -3.0, -2.0, 7.0 }).has_value());
        EXPECT_TRUE(TimeSampling::acyclic({ 4.0 }).has_value());
    }

    TEST(TimeSamplingFactoryTest, FromCyclesMapsTheShapesAlembicStores)
    {
        const auto uniform = TimeSampling::from_cycles(1, 0.5, { 2.0 });
        ASSERT_TRUE(uniform.has_value());
        EXPECT_EQ(uniform->kind(), TimeSampling::Kind::Uniform);
        EXPECT_EQ(*uniform->time(0), 2.0);
        EXPECT_EQ(*uniform->time(4), 4.0);

        const auto cyclic = TimeSampling::from_cycles(3, 1.0, { 0.0, 0.1, 0.9 });
        ASSERT_TRUE(cyclic.has_value());
        EXPECT_EQ(cyclic->kind(), TimeSampling::Kind::Cyclic);

        EXPECT_FALSE(TimeSampling::from_cycles(0, 1.0, {}).has_value());
        EXPECT_FALSE(TimeSampling::from_cycles(3, 1.0, { 0.0, 0.1 }).has_value());
        EXPECT_FALSE(TimeSampling::from_cycles(1, 1.0, { 0.0, 0.1 }).has_value());
        EXPECT_FALSE(TimeSampling::from_cycles(2, 1.0, { 0.0, 1.5 }).has_value());
        EXPECT_FALSE(TimeSampling::from_cycles(1, 0.0, { 0.0 }).has_value());
    }

    TEST(TimeSamplingTimeTest, AUniformStepComputesEachTimeFromItsIndexAndNeverAccumulates)
    {
        const auto sampling = must(TimeSampling::uniform(0.1));

        double accumulated = 0.0;
        for (int i = 0; i < 10; ++i) {
            accumulated += 0.1;
        }
        EXPECT_NE(accumulated, 1.0);
        EXPECT_EQ(*sampling.time(10), 1.0);
        EXPECT_EQ(*sampling.time(0), 0.0);

        const auto offset = must(TimeSampling::uniform(0.25, 3.0));
        for (size_t i = 0; i < 1000; ++i) {
            EXPECT_EQ(*offset.time(i), 3.0 + static_cast<double>(i) * 0.25);
        }
    }

    TEST(TimeSamplingTimeTest, AnExactRateLandsOnWholeSecondsWithoutDrift)
    {
        const auto ntsc = must(TimeSampling::uniform_rate({ 24000, 1001 }));

        for (size_t k = 1; k <= 100000; ++k) {
            ASSERT_EQ(*ntsc.time(24000 * k), 1001.0 * static_cast<double>(k)) << k;
        }

        const auto ntsc30 = must(TimeSampling::uniform_rate({ 30000, 1001 }));
        for (size_t k = 1; k <= 100000; k += 997) {
            ASSERT_EQ(*ntsc30.time(30000 * k), 1001.0 * static_cast<double>(k)) << k;
        }

        double accumulated = 0.0;
        for (int i = 0; i < 24000; ++i) {
            accumulated += 1001.0 / 24000.0;
        }
        EXPECT_NE(accumulated, 1001.0);
    }

    TEST(TimeSamplingTimeTest, AnExactRateRoundsOncePerSample)
    {
        const auto ntsc = must(TimeSampling::uniform_rate({ 24000, 1001 }));

        for (const size_t i : { size_t { 1 }, size_t { 7 }, size_t { 23999 }, size_t { 10'000'000 }, size_t { 123'456'789 } }) {
            EXPECT_EQ(*ntsc.time(i), static_cast<double>(i * 1001) / 24000.0) << i;
        }
    }

    TEST(TimeSamplingTimeTest, AFirstFrameShiftsTheSequenceExactly)
    {
        const auto shifted = must(TimeSampling::uniform_rate({ 24000, 1001 }, -5));

        EXPECT_EQ(*shifted.time(5), 0.0);
        EXPECT_EQ(*shifted.time(0), -5.0 * 1001.0 / 24000.0);
        EXPECT_LT(*shifted.time(0), 0.0);
        EXPECT_EQ(*shifted.time(24005), 1001.0);
    }

    TEST(TimeSamplingTimeTest, AnExactRateKeepsTheLeadingDigitsPastTheExactIntegerLimitAndMayRepeatADouble)
    {
        const auto fast = must(TimeSampling::uniform_rate({ 3, 1 }, int64_t { 1 } << 60));

        const double expected = std::ldexp(1.0, 60) / 3.0;
        EXPECT_NEAR(*fast.time(0), expected, expected * 1e-15);
        EXPECT_GE(*fast.time(1), *fast.time(0));
        EXPECT_NEAR(*fast.time(300), expected + 100.0, 64.0);
    }

    TEST(TimeSamplingTimeTest, AnExactRateReportsNothingWhereItsArithmeticWouldOverflow)
    {
        const auto at_limit = must(TimeSampling::uniform_rate({ 24000, 1001 }, std::numeric_limits<int64_t>::max()));
        EXPECT_FALSE(at_limit.time(1).has_value());
        EXPECT_FALSE(at_limit.time(0).has_value());

        const auto product_overflow = must(TimeSampling::uniform_rate({ 24000, 1001 },
            std::numeric_limits<int64_t>::max() / 1001 + 1));
        EXPECT_FALSE(product_overflow.time(0).has_value());
        EXPECT_FALSE(product_overflow.locate(0.0, 4).has_value());

        const auto negative = must(TimeSampling::uniform_rate({ 24000, 1001 },
            std::numeric_limits<int64_t>::min() / 1001 - 1));
        EXPECT_FALSE(negative.time(0).has_value());

        const auto plain = must(TimeSampling::uniform_rate({ 24000, 1001 }));
        EXPECT_FALSE(plain.time(std::numeric_limits<size_t>::max()).has_value());
    }

    TEST(TimeSamplingTimeTest, ADoubleStepAtAHugeIndexStaysFinite)
    {
        const auto sampling = must(TimeSampling::uniform(1.0));

        const auto last = sampling.time(std::numeric_limits<size_t>::max());
        ASSERT_TRUE(last.has_value());
        EXPECT_TRUE(std::isfinite(*last));
    }

    TEST(TimeSamplingTimeTest, CyclicTimesRepeatEveryCycleAndNeverGoBackwards)
    {
        const auto sampling = must(TimeSampling::cyclic({ 0.0, 0.1, 0.9 }, 1.0));

        constexpr std::array<double, 9> expected { 0.0, 0.1, 0.9, 1.0, 1.1, 1.9, 2.0, 2.1, 2.9 };
        for (size_t i = 0; i < expected.size(); ++i) {
            EXPECT_DOUBLE_EQ(*sampling.time(i), expected[i]) << i;
        }

        double previous = -k_inf;
        for (size_t i = 0; i < 300000; ++i) {
            const double time = *sampling.time(i);
            ASSERT_GT(time, previous) << i;
            previous = time;
        }
    }

    TEST(TimeSamplingTimeTest, ACyclicSamplingKeepsItsFirstTimeAsTheOffsetOfEveryCycle)
    {
        const auto sampling = must(TimeSampling::cyclic({ 5.0, 5.5 }, 2.0));

        EXPECT_EQ(*sampling.time(0), 5.0);
        EXPECT_EQ(*sampling.time(1), 5.5);
        EXPECT_EQ(*sampling.time(2), 7.0);
        EXPECT_EQ(*sampling.time(3), 7.5);
    }

    TEST(TimeSamplingTimeTest, AnAcyclicListEndsWhereItEnds)
    {
        const auto sampling = must(TimeSampling::acyclic({ 0.0, 1.0, 3.0 }));

        EXPECT_EQ(*sampling.time(2), 3.0);
        EXPECT_FALSE(sampling.time(3).has_value());
        EXPECT_FALSE(sampling.time(std::numeric_limits<size_t>::max()).has_value());
        EXPECT_EQ(sampling.stored_count(), (std::optional<size_t> { 3 }));
        EXPECT_FALSE(must(TimeSampling::uniform(1.0)).stored_count().has_value());
        EXPECT_FALSE(must(TimeSampling::cyclic({ 0.0, 0.5 }, 1.0)).stored_count().has_value());
    }

    TEST(TimeSamplingTimeTest, AccessorsReportOnlyWhatTheirKindHolds)
    {
        const auto uniform = must(TimeSampling::uniform(0.5, 2.0));
        EXPECT_EQ(uniform.step(), 0.5);
        EXPECT_EQ(uniform.start(), 2.0);
        EXPECT_EQ(uniform.time_per_cycle(), 0.0);
        EXPECT_TRUE(uniform.stored_times().empty());
        EXPECT_FALSE(uniform.exact_rate().has_value());

        const auto cyclic = must(TimeSampling::cyclic({ 0.0, 0.5 }, 1.0));
        EXPECT_EQ(cyclic.time_per_cycle(), 1.0);
        EXPECT_EQ(cyclic.step(), 0.0);
        EXPECT_EQ(cyclic.stored_times().size(), 2U);

        const auto rate = must(TimeSampling::uniform_rate({ 2, 1 }, 4));
        EXPECT_EQ(rate.step(), 0.5);
        EXPECT_EQ(rate.start(), 2.0);
    }

    TEST(TimeSamplingLocateTest, PlacesATimeBeforeWithinAndAfterTheSamples)
    {
        const auto sampling = must(TimeSampling::acyclic({ 0.0, 1.0, 3.0 }));

        const auto before = sampling.locate(-1.0, 3);
        ASSERT_TRUE(before.has_value());
        EXPECT_EQ(before->placement, TimePlacement::Before);
        EXPECT_EQ(before->lower, 0U);
        EXPECT_EQ(before->upper, 0U);
        EXPECT_EQ(before->fraction, 0.0);

        const auto first = sampling.locate(0.0, 3);
        EXPECT_EQ(first->placement, TimePlacement::Within);
        EXPECT_EQ(first->lower, 0U);
        EXPECT_EQ(first->upper, 0U);

        const auto middle = sampling.locate(0.5, 3);
        EXPECT_EQ(middle->lower, 0U);
        EXPECT_EQ(middle->upper, 1U);
        EXPECT_DOUBLE_EQ(middle->fraction, 0.5);

        const auto on_key = sampling.locate(1.0, 3);
        EXPECT_EQ(on_key->lower, 1U);
        EXPECT_EQ(on_key->upper, 1U);
        EXPECT_EQ(on_key->fraction, 0.0);

        const auto second = sampling.locate(2.0, 3);
        EXPECT_EQ(second->lower, 1U);
        EXPECT_EQ(second->upper, 2U);
        EXPECT_DOUBLE_EQ(second->fraction, 0.5);

        const auto last = sampling.locate(3.0, 3);
        EXPECT_EQ(last->placement, TimePlacement::Within);
        EXPECT_EQ(last->lower, 2U);
        EXPECT_EQ(last->upper, 2U);

        const auto after = sampling.locate(std::nextafter(3.0, k_inf), 3);
        EXPECT_EQ(after->placement, TimePlacement::After);
        EXPECT_EQ(after->lower, 2U);
        EXPECT_EQ(after->upper, 2U);
    }

    TEST(TimeSamplingLocateTest, TheCountCapsTheRangeOfAnUnboundedSampling)
    {
        const auto sampling = must(TimeSampling::uniform(1.0));

        EXPECT_EQ(sampling.locate(1.5, 3)->placement, TimePlacement::Within);
        EXPECT_EQ(sampling.locate(2.5, 3)->placement, TimePlacement::After);
        EXPECT_EQ(sampling.locate(2.5, 3)->lower, 2U);
        EXPECT_EQ(sampling.locate(2.5, 4)->placement, TimePlacement::Within);
    }

    TEST(TimeSamplingLocateTest, ASingleSampleIsBeforeOnAndAfter)
    {
        const auto sampling = must(TimeSampling::uniform(1.0, 5.0));

        EXPECT_EQ(sampling.locate(4.0, 1)->placement, TimePlacement::Before);
        EXPECT_EQ(sampling.locate(5.0, 1)->placement, TimePlacement::Within);
        EXPECT_EQ(sampling.locate(5.5, 1)->placement, TimePlacement::After);
        EXPECT_EQ(sampling.locate(5.0, 1)->lower, 0U);
    }

    TEST(TimeSamplingLocateTest, RefusesWhatItCannotAnswer)
    {
        const auto acyclic = must(TimeSampling::acyclic({ 0.0, 1.0 }));
        const auto uniform = must(TimeSampling::uniform(1.0));

        EXPECT_FALSE(uniform.locate(0.0, 0).has_value());
        EXPECT_FALSE(uniform.locate(k_nan, 4).has_value());
        EXPECT_FALSE(uniform.locate(k_inf, 4).has_value());
        EXPECT_FALSE(uniform.locate(-k_inf, 4).has_value());
        EXPECT_FALSE(acyclic.locate(0.5, 3).has_value());
        EXPECT_TRUE(acyclic.locate(0.5, 2).has_value());
        EXPECT_TRUE(acyclic.locate(0.5, 1).has_value());
    }

    TEST(TimeSamplingLocateTest, AnAcyclicListLongerThanTheCountIsReadAsAPrefix)
    {
        const auto sampling = must(TimeSampling::acyclic({ 0.0, 1.0, 3.0, 6.0 }));

        const auto where = sampling.locate(2.0, 3);

        EXPECT_EQ(where->lower, 1U);
        EXPECT_EQ(where->upper, 2U);
        EXPECT_EQ(sampling.locate(5.0, 3)->placement, TimePlacement::After);
    }

    TEST(TimeSamplingLocateTest, ExtremeTimesFallOutsideWithoutOverflow)
    {
        const auto sampling = must(TimeSampling::uniform(1.0));
        const auto rate = must(TimeSampling::uniform_rate({ 24000, 1001 }));

        for (const auto* s : { &sampling, &rate }) {
            EXPECT_EQ(s->locate(1.0e300, 10)->placement, TimePlacement::After);
            EXPECT_EQ(s->locate(-1.0e300, 10)->placement, TimePlacement::Before);
            EXPECT_EQ(s->locate(std::numeric_limits<double>::max(), 10)->placement, TimePlacement::After);
            EXPECT_EQ(s->locate(std::numeric_limits<double>::lowest(), 10)->placement, TimePlacement::Before);
            EXPECT_EQ(s->locate(std::numeric_limits<double>::denorm_min(), 10)->placement, TimePlacement::Within);
        }
    }

    TEST(TimeSamplingLocateTest, TheNeighboursOfASampleTimeLandOnTheRightSides)
    {
        const auto sampling = must(TimeSampling::acyclic({ -2.0, 0.1, 0.1000000001, 7.0, 7.5 }));

        for (size_t k = 0; k < 5; ++k) {
            const double time = *sampling.time(k);

            const auto exact = sampling.locate(time, 5);
            ASSERT_TRUE(exact.has_value());
            EXPECT_EQ(exact->lower, k) << k;
            EXPECT_EQ(exact->upper, k) << k;
            EXPECT_EQ(exact->fraction, 0.0) << k;

            const auto below = sampling.locate(std::nextafter(time, -k_inf), 5);
            if (k == 0) {
                EXPECT_EQ(below->placement, TimePlacement::Before);
            } else {
                EXPECT_EQ(below->lower, k - 1) << k;
                EXPECT_EQ(below->upper, k) << k;
                EXPECT_LE(below->fraction, 1.0);
                EXPECT_GE(below->fraction, 0.0);
            }

            const auto above = sampling.locate(std::nextafter(time, k_inf), 5);
            if (k == 4) {
                EXPECT_EQ(above->placement, TimePlacement::After);
            } else {
                EXPECT_EQ(above->lower, k) << k;
            }
        }
    }

    TEST(TimeSamplingLocateTest, AFractionNeverLeavesZeroToOneEvenAcrossATinyGap)
    {
        const auto sampling = must(TimeSampling::acyclic({ 0.0, 1.0e-300, 1.0 }));

        for (const double t : { 1.0e-301, 9.9999999999999e-301, std::nextafter(1.0e-300, 0.0), 0.3, 0.9999999999999999 }) {
            const auto where = sampling.locate(t, 3);
            ASSERT_TRUE(where.has_value());
            EXPECT_GE(where->fraction, 0.0);
            EXPECT_LE(where->fraction, 1.0);
        }
    }

    TEST(TimeSamplingLocateTest, AnExactRateFindsEverySampleByItsOwnTime)
    {
        const auto sampling = must(TimeSampling::uniform_rate({ 30000, 1001 }));
        constexpr size_t count = 200000;

        for (size_t k = 0; k < count; k += 997) {
            const auto where = sampling.locate(*sampling.time(k), count);
            ASSERT_TRUE(where.has_value());
            ASSERT_EQ(where->lower, k);
            ASSERT_EQ(where->upper, k);
            ASSERT_EQ(where->fraction, 0.0);
        }
        const auto last = sampling.locate(*sampling.time(count - 1), count);
        EXPECT_EQ(last->placement, TimePlacement::Within);
        EXPECT_EQ(last->lower, count - 1);
    }

    TEST(TimeSamplingLocateTest, ABillionSamplesAreSearchedWithoutScanning)
    {
        const auto sampling = must(TimeSampling::uniform_rate({ 24, 1 }));
        constexpr size_t count = 1'000'000'000;

        const auto where = sampling.locate(*sampling.time(500'000'000) + 0.02, count);

        ASSERT_TRUE(where.has_value());
        EXPECT_EQ(where->lower, 500'000'000U);
        EXPECT_EQ(where->upper, 500'000'001U);
    }

    TEST(TimeSamplingLocateTest, FloorCeilAndNearestFollowTheAlembicRules)
    {
        const auto sampling = must(TimeSampling::acyclic({ 0.0, 1.0, 3.0 }));

        EXPECT_EQ(sampling.floor_index(-5.0, 3), (std::optional<size_t> { 0 }));
        EXPECT_EQ(sampling.floor_index(2.0, 3), (std::optional<size_t> { 1 }));
        EXPECT_EQ(sampling.floor_index(99.0, 3), (std::optional<size_t> { 2 }));

        EXPECT_EQ(sampling.ceil_index(-5.0, 3), (std::optional<size_t> { 0 }));
        EXPECT_EQ(sampling.ceil_index(1.0, 3), (std::optional<size_t> { 1 }));
        EXPECT_EQ(sampling.ceil_index(1.5, 3), (std::optional<size_t> { 2 }));
        EXPECT_EQ(sampling.ceil_index(99.0, 3), (std::optional<size_t> { 2 }));

        EXPECT_EQ(sampling.nearest_index(0.4, 3), (std::optional<size_t> { 0 }));
        EXPECT_EQ(sampling.nearest_index(0.5, 3), (std::optional<size_t> { 0 }));
        EXPECT_EQ(sampling.nearest_index(0.6, 3), (std::optional<size_t> { 1 }));
        EXPECT_EQ(sampling.nearest_index(99.0, 3), (std::optional<size_t> { 2 }));
        EXPECT_FALSE(sampling.floor_index(k_nan, 3).has_value());
    }

    struct SamplingCase {
        const char* name;
        TimeSampling sampling;
        size_t count;
    };

    std::vector<SamplingCase> sampling_cases()
    {
        Lcg random(42);
        std::vector<double> scattered;
        double time = -3.0;
        for (int i = 0; i < 400; ++i) {
            time += 1.0e-6 + random.unit();
            scattered.push_back(time);
        }
        std::vector<double> cycle { 0.0 };
        for (int i = 0; i < 6; ++i) {
            cycle.push_back(cycle.back() + 0.01 + random.unit() * 0.1);
        }
        const double period = cycle.back() + 0.2;

        return {
            { "UniformDouble", must(TimeSampling::uniform(0.37, -1.5)), 500 },
            { "UniformRate", must(TimeSampling::uniform_rate({ 24000, 1001 }, -17)), 500 },
            { "Cyclic", must(TimeSampling::cyclic(cycle, period)), 500 },
            { "Acyclic", must(TimeSampling::acyclic(scattered)), scattered.size() },
        };
    }

    TEST(TimeSamplingPropertyTest, LookupsAgreeWithALinearScanOnEveryKind)
    {
        Lcg random(7);

        for (const auto& item : sampling_cases()) {
            const double first = *item.sampling.time(0);
            const double last = *item.sampling.time(item.count - 1);

            for (int query = 0; query < 20000; ++query) {
                double t = first - 1.0 + random.unit() * (last - first + 2.0);
                if (query % 5 == 0) {
                    t = *item.sampling.time(static_cast<size_t>(random.unit() * static_cast<double>(item.count)));
                } else if (query % 7 == 0) {
                    t = std::nextafter(*item.sampling.time(static_cast<size_t>(random.unit() * static_cast<double>(item.count))),
                        query % 2 == 0 ? k_inf : -k_inf);
                }

                const auto floor_index = item.sampling.floor_index(t, item.count);
                const auto ceil_index = item.sampling.ceil_index(t, item.count);
                const auto near_index = item.sampling.nearest_index(t, item.count);
                ASSERT_TRUE(floor_index && ceil_index && near_index) << item.name;
                ASSERT_EQ(*floor_index, scan_floor(item.sampling, t, item.count)) << item.name << ' ' << t;
                ASSERT_EQ(*ceil_index, scan_ceil(item.sampling, t, item.count)) << item.name << ' ' << t;
                ASSERT_EQ(*near_index, scan_nearest(item.sampling, t, item.count)) << item.name << ' ' << t;
            }
        }
    }

    TEST(TimeSamplingPropertyTest, EveryKindIsStrictlyIncreasingAndLocatesItsOwnTimes)
    {
        for (const auto& item : sampling_cases()) {
            for (size_t i = 1; i < item.count; ++i) {
                ASSERT_GT(*item.sampling.time(i), *item.sampling.time(i - 1)) << item.name << ' ' << i;
            }
            for (size_t i = 0; i < item.count; ++i) {
                const auto where = item.sampling.locate(*item.sampling.time(i), item.count);
                ASSERT_TRUE(where.has_value());
                ASSERT_EQ(where->lower, i) << item.name;
                ASSERT_EQ(where->upper, i) << item.name;
            }
        }
    }

    TEST(FoldTimeTest, ClampHoldsTheNearestEnd)
    {
        const auto below = Kinesis::fold_time(-3.0, 0.0, 1.0, TimeWrap::Clamp);
        const auto above = Kinesis::fold_time(3.0, 0.0, 1.0, TimeWrap::Clamp);
        const auto inside = Kinesis::fold_time(0.25, 0.0, 1.0, TimeWrap::Clamp);

        EXPECT_EQ(below->time, 0.0);
        EXPECT_EQ(above->time, 1.0);
        EXPECT_EQ(inside->time, 0.25);
        EXPECT_EQ(below->cycles, 0);
        EXPECT_FALSE(above->reversed);
    }

    TEST(FoldTimeTest, RepeatWrapsBothWaysAndCountsTheCycles)
    {
        const auto negative = Kinesis::fold_time(-0.25, 0.0, 1.0, TimeWrap::Repeat);
        const auto later = Kinesis::fold_time(2.5, 0.0, 1.0, TimeWrap::Repeat);
        const auto shifted = Kinesis::fold_time(-0.5, -2.0, -1.0, TimeWrap::Repeat);

        EXPECT_DOUBLE_EQ(negative->time, 0.75);
        EXPECT_EQ(negative->cycles, -1);
        EXPECT_DOUBLE_EQ(later->time, 0.5);
        EXPECT_EQ(later->cycles, 2);
        EXPECT_DOUBLE_EQ(shifted->time, -1.5);
        EXPECT_EQ(shifted->cycles, 1);
        EXPECT_FALSE(later->reversed);
    }

    TEST(FoldTimeTest, RepeatSendsTheEndBackToTheBeginning)
    {
        const auto end = Kinesis::fold_time(1.0, 0.0, 1.0, TimeWrap::Repeat);

        EXPECT_EQ(end->time, 0.0);
        EXPECT_EQ(end->cycles, 1);
    }

    TEST(FoldTimeTest, PingPongRunsBackwardOverOddCycles)
    {
        const auto forward = Kinesis::fold_time(0.25, 0.0, 1.0, TimeWrap::PingPong);
        const auto backward = Kinesis::fold_time(1.25, 0.0, 1.0, TimeWrap::PingPong);
        const auto forward_again = Kinesis::fold_time(2.25, 0.0, 1.0, TimeWrap::PingPong);
        const auto before = Kinesis::fold_time(-0.25, 0.0, 1.0, TimeWrap::PingPong);

        EXPECT_DOUBLE_EQ(forward->time, 0.25);
        EXPECT_FALSE(forward->reversed);
        EXPECT_DOUBLE_EQ(backward->time, 0.75);
        EXPECT_TRUE(backward->reversed);
        EXPECT_EQ(backward->cycles, 1);
        EXPECT_DOUBLE_EQ(forward_again->time, 0.25);
        EXPECT_FALSE(forward_again->reversed);
        EXPECT_DOUBLE_EQ(before->time, 0.25);
        EXPECT_EQ(before->cycles, -1);
        EXPECT_TRUE(before->reversed);
    }

    TEST(FoldTimeTest, PingPongTurnsAtTheEnds)
    {
        const auto start = Kinesis::fold_time(0.0, 0.0, 1.0, TimeWrap::PingPong);
        const auto turn = Kinesis::fold_time(1.0, 0.0, 1.0, TimeWrap::PingPong);
        const auto round_trip = Kinesis::fold_time(2.0, 0.0, 1.0, TimeWrap::PingPong);

        EXPECT_EQ(start->time, 0.0);
        EXPECT_FALSE(start->reversed);
        EXPECT_EQ(turn->time, 1.0);
        EXPECT_TRUE(turn->reversed);
        EXPECT_EQ(round_trip->time, 0.0);
        EXPECT_FALSE(round_trip->reversed);
    }

    TEST(FoldTimeTest, AZeroLengthRangeFoldsEverythingToItsBegin)
    {
        for (const auto mode : { TimeWrap::Clamp, TimeWrap::Repeat, TimeWrap::PingPong }) {
            const auto folded = Kinesis::fold_time(7.0, 2.0, 2.0, mode);
            ASSERT_TRUE(folded.has_value());
            EXPECT_EQ(folded->time, 2.0);
            EXPECT_EQ(folded->cycles, 0);
            EXPECT_FALSE(folded->reversed);
        }
    }

    TEST(FoldTimeTest, RefusesArgumentsThatAreNotARange)
    {
        for (const auto mode : { TimeWrap::Clamp, TimeWrap::Repeat, TimeWrap::PingPong }) {
            EXPECT_FALSE(Kinesis::fold_time(k_nan, 0.0, 1.0, mode).has_value());
            EXPECT_FALSE(Kinesis::fold_time(k_inf, 0.0, 1.0, mode).has_value());
            EXPECT_FALSE(Kinesis::fold_time(0.5, k_nan, 1.0, mode).has_value());
            EXPECT_FALSE(Kinesis::fold_time(0.5, 0.0, k_inf, mode).has_value());
            EXPECT_FALSE(Kinesis::fold_time(0.5, 1.0, 0.0, mode).has_value());
        }
    }

    TEST(FoldTimeTest, ATinyNegativeTimeNeverEscapesTheRange)
    {
        for (const auto mode : { TimeWrap::Repeat, TimeWrap::PingPong }) {
            for (const double t : { -1.0e-20, -1.0e-300, -std::numeric_limits<double>::denorm_min(), 1.0 - 1.0e-17 }) {
                const auto folded = Kinesis::fold_time(t, 0.0, 1.0, mode);
                ASSERT_TRUE(folded.has_value());
                EXPECT_GE(folded->time, 0.0);
                EXPECT_LE(folded->time, 1.0);
            }
        }
    }

    TEST(FoldTimeTest, HugeTimesSaturateTheCycleCountInsteadOfOverflowing)
    {
        const auto far = Kinesis::fold_time(1.0e300, 0.0, 1.0, TimeWrap::Repeat);
        const auto far_back = Kinesis::fold_time(-1.0e300, 0.0, 1.0, TimeWrap::PingPong);

        ASSERT_TRUE(far.has_value());
        ASSERT_TRUE(far_back.has_value());
        EXPECT_EQ(far->cycles, 9'000'000'000'000'000'000LL);
        EXPECT_EQ(far_back->cycles, -9'000'000'000'000'000'000LL);
        EXPECT_GE(far->time, 0.0);
        EXPECT_LE(far->time, 1.0);
        EXPECT_GE(far_back->time, 0.0);
        EXPECT_LE(far_back->time, 1.0);
    }

    TEST(FoldTimeTest, FoldedTimesAlwaysStayInsideTheRangeOnRandomInput)
    {
        Lcg random(99);

        for (int i = 0; i < 50000; ++i) {
            const double begin = (random.unit() - 0.5) * 100.0;
            const double end = begin + random.unit() * 10.0 + 1.0e-9;
            const double t = (random.unit() - 0.5) * 1.0e4;
            for (const auto mode : { TimeWrap::Clamp, TimeWrap::Repeat, TimeWrap::PingPong }) {
                const auto folded = Kinesis::fold_time(t, begin, end, mode);
                ASSERT_TRUE(folded.has_value());
                ASSERT_GE(folded->time, begin);
                ASSERT_LE(folded->time, end);
            }
        }
    }

}
}
