#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kinesis/Temporal/TimedValues.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

using Kinesis::KeyInterpolation;
using Kinesis::OutOfRange;
using Kinesis::TimedValues;
using Kinesis::TimeLimit;
using Kinesis::TimeSampling;
using Kinesis::ValueKind;

namespace {

    constexpr double k_inf = std::numeric_limits<double>::infinity();
    constexpr double k_nan = std::numeric_limits<double>::quiet_NaN();
    constexpr float k_fnan = std::numeric_limits<float>::quiet_NaN();
    constexpr float k_finf = std::numeric_limits<float>::infinity();

    TimeSampling must(std::optional<TimeSampling> sampling)
    {
        if (!sampling) {
            throw std::runtime_error("sampling was rejected");
        }
        return *sampling;
    }

    template <typename T>
    TimedValues<T> must_create(typename TimedValues<T>::Keys keys)
    {
        auto created = TimedValues<T>::create(std::move(keys));
        if (!created) {
            throw std::runtime_error("keys were rejected");
        }
        return *created;
    }

    TimedValues<float> ramp(KeyInterpolation mode = KeyInterpolation::Linear)
    {
        return must_create<float>({ .times = must(TimeSampling::acyclic({ 0.0, 1.0, 3.0 })),
            .values = { 0.0F, 10.0F, 30.0F },
            .default_mode = mode });
    }

    glm::vec4 about_z(float degrees)
    {
        const float half = glm::radians(degrees) * 0.5F;
        return { 0.0F, 0.0F, std::sin(half), std::cos(half) };
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

    TEST(TimedValuesCreateTest, RejectsInputsThatCannotBeSampled)
    {
        const auto times = must(TimeSampling::acyclic({ 0.0, 1.0, 3.0 }));

        EXPECT_FALSE(TimedValues<float>::create({ .times = times, .values = {} }).has_value());
        EXPECT_FALSE(TimedValues<float>::create({ .times = times, .values = { 1.0F, 2.0F } }).has_value());
        EXPECT_FALSE(TimedValues<float>::create({ .times = times, .values = { 1.0F, 2.0F, 3.0F, 4.0F } }).has_value());
        EXPECT_FALSE(TimedValues<float>::create({ .times = times, .values = { 1.0F, 2.0F, 3.0F },
                                                    .modes = { KeyInterpolation::Step } })
                         .has_value());
        EXPECT_TRUE(TimedValues<float>::create({ .times = times, .values = { 1.0F, 2.0F, 3.0F } }).has_value());
    }

    TEST(TimedValuesCreateTest, RejectsNonFiniteValuesAndTangents)
    {
        const auto times = must(TimeSampling::uniform(1.0));

        EXPECT_FALSE(TimedValues<float>::create({ .times = times, .values = { 0.0F, k_fnan } }).has_value());
        EXPECT_FALSE(TimedValues<float>::create({ .times = times, .values = { k_finf, 0.0F } }).has_value());
        EXPECT_FALSE(TimedValues<glm::vec3>::create({ .times = times, .values = { glm::vec3(0.0F), glm::vec3(1.0F, k_fnan, 0.0F) } }).has_value());
        EXPECT_FALSE(TimedValues<float>::create({ .times = times,
                                                    .values = { 0.0F, 1.0F },
                                                    .in_tangents = { 0.0F, k_fnan },
                                                    .out_tangents = { 0.0F, 0.0F },
                                                    .default_mode = KeyInterpolation::CubicSpline })
                         .has_value());
    }

    TEST(TimedValuesCreateTest, CubicKeysNeedAFullSetOfTangents)
    {
        const auto times = must(TimeSampling::uniform(1.0));
        const std::vector<float> values { 0.0F, 1.0F, 2.0F };

        EXPECT_FALSE(TimedValues<float>::create({ .times = times, .values = values, .default_mode = KeyInterpolation::CubicSpline }).has_value());
        EXPECT_FALSE(TimedValues<float>::create({ .times = times, .values = values,
                                                    .in_tangents = { 0.0F, 0.0F },
                                                    .out_tangents = { 0.0F, 0.0F },
                                                    .default_mode = KeyInterpolation::CubicSpline })
                         .has_value());
        EXPECT_FALSE(TimedValues<float>::create({ .times = times, .values = values,
                                                    .in_tangents = { 0.0F, 0.0F, 0.0F },
                                                    .out_tangents = {},
                                                    .default_mode = KeyInterpolation::CubicSpline })
                         .has_value());
        EXPECT_FALSE(TimedValues<float>::create({ .times = times, .values = values,
                                                    .modes = { KeyInterpolation::Linear, KeyInterpolation::CubicSpline, KeyInterpolation::Linear } })
                         .has_value());
        EXPECT_TRUE(TimedValues<float>::create({ .times = times, .values = values,
                                                   .modes = { KeyInterpolation::Linear, KeyInterpolation::CubicSpline, KeyInterpolation::Linear },
                                                   .in_tangents = { 0.0F, 0.0F, 0.0F },
                                                   .out_tangents = { 0.0F, 0.0F, 0.0F } })
                        .has_value());
    }

    TEST(TimedValuesCreateTest, ATypeThatCannotBeBlendedAcceptsOnlyStepKeys)
    {
        const auto times = must(TimeSampling::uniform(1.0));

        EXPECT_FALSE(TimedValues<std::string>::create({ .times = times, .values = { "a", "b" } }).has_value());
        EXPECT_FALSE(TimedValues<std::string>::create({ .times = times, .values = { "a", "b" },
                                                          .modes = { KeyInterpolation::Step, KeyInterpolation::Linear } })
                         .has_value());
        EXPECT_TRUE(TimedValues<std::string>::create({ .times = times, .values = { "a", "b" }, .default_mode = KeyInterpolation::Step }).has_value());
        EXPECT_FALSE(TimedValues<int>::create({ .times = times, .values = { 1, 2 } }).has_value());
        EXPECT_TRUE(TimedValues<int>::create({ .times = times, .values = { 1, 2 }, .default_mode = KeyInterpolation::Step }).has_value());
    }

    TEST(TimedValuesCreateTest, RejectsASamplingThatCannotTimeTheLastKey)
    {
        const auto overflowing = must(TimeSampling::uniform_rate({ 24000, 1001 }, std::numeric_limits<int64_t>::max() - 1));

        EXPECT_FALSE(TimedValues<float>::create({ .times = overflowing, .values = { 0.0F, 1.0F, 2.0F } }).has_value());
    }

    TEST(TimedValuesCreateTest, RotationsMustBeNonZeroFiniteVec4s)
    {
        const auto times = must(TimeSampling::uniform(1.0));

        EXPECT_FALSE(TimedValues<glm::vec4>::create({ .times = times, .values = { glm::vec4(0.0F), glm::vec4(0, 0, 0, 1) }, .kind = ValueKind::Rotation }).has_value());
        EXPECT_FALSE(TimedValues<glm::vec4>::create({ .times = times, .values = { glm::vec4(1.0e30F), glm::vec4(0, 0, 0, 1) }, .kind = ValueKind::Rotation }).has_value());
        EXPECT_FALSE(TimedValues<glm::vec4>::create({ .times = times, .values = { glm::vec4(0, 0, 0, k_fnan), glm::vec4(0, 0, 0, 1) }, .kind = ValueKind::Rotation }).has_value());
        EXPECT_FALSE(TimedValues<glm::vec3>::create({ .times = times, .values = { glm::vec3(0, 0, 1), glm::vec3(0, 1, 0) }, .kind = ValueKind::Rotation }).has_value());
        EXPECT_FALSE(TimedValues<float>::create({ .times = times, .values = { 1.0F, 2.0F }, .kind = ValueKind::Rotation }).has_value());
        EXPECT_TRUE(TimedValues<glm::vec4>::create({ .times = times, .values = { glm::vec4(0, 0, 0, 2), glm::vec4(0, 0, 0, 1) }, .kind = ValueKind::Rotation }).has_value());
    }

    TEST(TimedValuesCreateTest, RotationKeysAreStoredNormalised)
    {
        const auto rotation = must_create<glm::vec4>({ .times = must(TimeSampling::uniform(1.0)),
            .values = { glm::vec4(0, 0, 0, 4), glm::vec4(0, 3, 0, 4) },
            .kind = ValueKind::Rotation });

        EXPECT_EQ(rotation.values()[0], glm::vec4(0, 0, 0, 1));
        EXPECT_NEAR(glm::length(rotation.values()[1]), 1.0F, 1e-6F);
    }

    TEST(TimedValuesSampleTest, AKeyReturnsItsStoredValueUntouchedAndSegmentsBlend)
    {
        const auto values = ramp();

        EXPECT_EQ(values.sample(0.0), (std::optional<float> { 0.0F }));
        EXPECT_EQ(values.sample(1.0), (std::optional<float> { 10.0F }));
        EXPECT_EQ(values.sample(3.0), (std::optional<float> { 30.0F }));
        EXPECT_FLOAT_EQ(*values.sample(0.5), 5.0F);
        EXPECT_FLOAT_EQ(*values.sample(2.0), 20.0F);
        EXPECT_EQ(values.size(), 3U);
        EXPECT_EQ(values.first_time(), 0.0);
        EXPECT_EQ(values.last_time(), 3.0);
        EXPECT_EQ(values.duration(), 3.0);
    }

    TEST(TimedValuesSampleTest, AKeyModeAppliesFromThatKeyToTheNext)
    {
        const auto values = must_create<float>({ .times = must(TimeSampling::acyclic({ 0.0, 1.0, 3.0 })),
            .values = { 0.0F, 10.0F, 30.0F },
            .modes = { KeyInterpolation::Step, KeyInterpolation::Linear, KeyInterpolation::Linear } });

        EXPECT_EQ(*values.sample(0.5), 0.0F);
        EXPECT_EQ(*values.sample(0.999999), 0.0F);
        EXPECT_FLOAT_EQ(*values.sample(2.0), 20.0F);
    }

    TEST(TimedValuesSampleTest, NonFiniteTimesAnswerNothingWhateverThePolicy)
    {
        const auto values = ramp();

        for (const auto policy : { OutOfRange::Absent, OutOfRange::Hold, OutOfRange::Repeat, OutOfRange::PingPong, OutOfRange::Extrapolate, OutOfRange::RepeatOffset }) {
            EXPECT_FALSE(values.sample(k_nan, policy).has_value());
            EXPECT_FALSE(values.sample(k_inf, policy).has_value());
            EXPECT_FALSE(values.sample(-k_inf, policy).has_value());
        }
    }

    TEST(TimedValuesOutOfRangeTest, AbsentAnswersNothingOutsideAndSomethingInside)
    {
        const auto values = ramp();

        EXPECT_FALSE(values.sample(-0.001, OutOfRange::Absent).has_value());
        EXPECT_FALSE(values.sample(3.001, OutOfRange::Absent).has_value());
        EXPECT_EQ(values.sample(0.0, OutOfRange::Absent), (std::optional<float> { 0.0F }));
        EXPECT_EQ(values.sample(3.0, OutOfRange::Absent), (std::optional<float> { 30.0F }));
    }

    TEST(TimedValuesOutOfRangeTest, HoldKeepsTheNearestEnd)
    {
        const auto values = ramp();

        EXPECT_EQ(*values.sample(-5.0, OutOfRange::Hold), 0.0F);
        EXPECT_EQ(*values.sample(99.0, OutOfRange::Hold), 30.0F);
        EXPECT_EQ(*values.sample(-5.0), 0.0F);
    }

    TEST(TimedValuesOutOfRangeTest, RepeatWrapsTheWholeRange)
    {
        const auto values = ramp();

        EXPECT_FLOAT_EQ(*values.sample(4.0, OutOfRange::Repeat), 10.0F);
        EXPECT_FLOAT_EQ(*values.sample(-1.0, OutOfRange::Repeat), 20.0F);
        EXPECT_FLOAT_EQ(*values.sample(6.0, OutOfRange::Repeat), 0.0F);
        EXPECT_FLOAT_EQ(*values.sample(301.0, OutOfRange::Repeat), 10.0F);
    }

    TEST(TimedValuesOutOfRangeTest, PingPongReflectsAtBothEnds)
    {
        const auto values = ramp();

        EXPECT_FLOAT_EQ(*values.sample(4.0, OutOfRange::PingPong), 20.0F);
        EXPECT_FLOAT_EQ(*values.sample(7.0, OutOfRange::PingPong), 10.0F);
        EXPECT_FLOAT_EQ(*values.sample(-1.0, OutOfRange::PingPong), 10.0F);
    }

    TEST(TimedValuesOutOfRangeTest, RepeatOffsetContinuesACumulativeValueFromWhereTheLastCopyEnded)
    {
        const auto values = ramp();

        EXPECT_FLOAT_EQ(*values.sample(4.0, OutOfRange::RepeatOffset), 40.0F);
        EXPECT_FLOAT_EQ(*values.sample(-1.0, OutOfRange::RepeatOffset), -10.0F);
        EXPECT_FLOAT_EQ(*values.sample(7.0, OutOfRange::RepeatOffset), 70.0F);
        EXPECT_FLOAT_EQ(*values.sample(1.5, OutOfRange::RepeatOffset), 15.0F);
    }

    TEST(TimedValuesOutOfRangeTest, ExtrapolateContinuesTheSlopeOfTheEndSegments)
    {
        const auto values = ramp();

        EXPECT_FLOAT_EQ(*values.sample(4.0, OutOfRange::Extrapolate), 40.0F);
        EXPECT_FLOAT_EQ(*values.sample(-1.0, OutOfRange::Extrapolate), -10.0F);
    }

    TEST(TimedValuesOutOfRangeTest, ExtrapolationStopsAtAStepSegment)
    {
        const auto values = must_create<float>({ .times = must(TimeSampling::acyclic({ 0.0, 1.0, 3.0 })),
            .values = { 5.0F, 10.0F, 30.0F },
            .modes = { KeyInterpolation::Step, KeyInterpolation::Step, KeyInterpolation::Linear } });

        EXPECT_EQ(*values.sample(-4.0, OutOfRange::Extrapolate), 5.0F);
        EXPECT_EQ(*values.sample(9.0, OutOfRange::Extrapolate), 30.0F);
    }

    TEST(TimedValuesOutOfRangeTest, EveryPolicyOnASingleKeyReturnsThatKey)
    {
        const auto single = must_create<float>({ .times = must(TimeSampling::uniform(1.0, 2.0)), .values = { 7.0F } });

        for (const auto policy : { OutOfRange::Hold, OutOfRange::Repeat, OutOfRange::PingPong, OutOfRange::Extrapolate, OutOfRange::RepeatOffset }) {
            EXPECT_EQ(*single.sample(-100.0, policy), 7.0F);
            EXPECT_EQ(*single.sample(2.0, policy), 7.0F);
            EXPECT_EQ(*single.sample(100.0, policy), 7.0F);
        }
        EXPECT_FALSE(single.sample(100.0, OutOfRange::Absent).has_value());
        EXPECT_EQ(single.duration(), 0.0);
    }

    TEST(TimedValuesOutOfRangeTest, HugeTimesUnderRepeatingPoliciesStayInsideTheValues)
    {
        const auto values = ramp();

        for (const auto policy : { OutOfRange::Repeat, OutOfRange::PingPong, OutOfRange::RepeatOffset }) {
            for (const double t : { 1.0e300, -1.0e300, 1.0e17, -1.0e17 }) {
                const auto sampled = values.sample(t, policy);
                ASSERT_TRUE(sampled.has_value());
                EXPECT_TRUE(std::isfinite(*sampled) || policy == OutOfRange::RepeatOffset);
            }
        }
        const auto held = values.sample(1.0e300, OutOfRange::Repeat);
        EXPECT_GE(*held, 0.0F);
        EXPECT_LE(*held, 30.0F);
    }

    TEST(TimedValuesLimitTest, TheLeftLimitDiffersFromTheValueOnlyAfterAStepSegment)
    {
        const auto values = must_create<float>({ .times = must(TimeSampling::acyclic({ 0.0, 1.0, 3.0 })),
            .values = { 0.0F, 10.0F, 30.0F },
            .modes = { KeyInterpolation::Step, KeyInterpolation::Step, KeyInterpolation::Linear } });

        EXPECT_EQ(*values.sample(1.0, OutOfRange::Hold, TimeLimit::At), 10.0F);
        EXPECT_EQ(*values.sample(1.0, OutOfRange::Hold, TimeLimit::Before), 0.0F);
        EXPECT_EQ(*values.sample(3.0, OutOfRange::Hold, TimeLimit::Before), 10.0F);
        EXPECT_EQ(*values.sample(0.0, OutOfRange::Hold, TimeLimit::Before), 0.0F);

        const auto linear = ramp();
        EXPECT_EQ(*linear.sample(1.0, OutOfRange::Hold, TimeLimit::Before), 10.0F);
    }

    TEST(TimedValuesCubicTest, ZeroTangentsGiveTheSmoothstepSegment)
    {
        const auto values = must_create<float>({ .times = must(TimeSampling::acyclic({ 0.0, 1.0 })),
            .values = { 0.0F, 1.0F },
            .in_tangents = { 0.0F, 0.0F },
            .out_tangents = { 0.0F, 0.0F },
            .default_mode = KeyInterpolation::CubicSpline });

        EXPECT_NEAR(*values.sample(0.5), 0.5F, 1e-6F);
        EXPECT_NEAR(*values.sample(0.25), 0.15625F, 1e-6F);
        EXPECT_EQ(*values.sample(0.0), 0.0F);
        EXPECT_EQ(*values.sample(1.0), 1.0F);
    }

    TEST(TimedValuesCubicTest, TangentsAreInValuePerSecondAndScaledByTheSegmentDuration)
    {
        const auto values = must_create<float>({ .times = must(TimeSampling::acyclic({ 0.0, 2.0 })),
            .values = { 0.0F, 2.0F },
            .in_tangents = { 1.0F, 1.0F },
            .out_tangents = { 1.0F, 1.0F },
            .default_mode = KeyInterpolation::CubicSpline });

        for (const double t : { 0.1, 0.5, 1.0, 1.3, 1.9 }) {
            EXPECT_NEAR(*values.sample(t), static_cast<float>(t), 1e-5F) << t;
        }
    }

    TEST(TimedValuesCubicTest, AMixedKeyListBlendsEachSegmentByItsOwnMode)
    {
        const auto values = must_create<float>({ .times = must(TimeSampling::acyclic({ 0.0, 1.0, 2.0 })),
            .values = { 0.0F, 1.0F, 2.0F },
            .modes = { KeyInterpolation::CubicSpline, KeyInterpolation::Linear, KeyInterpolation::Linear },
            .in_tangents = { 0.0F, 0.0F, 0.0F },
            .out_tangents = { 0.0F, 0.0F, 0.0F } });

        EXPECT_NEAR(*values.sample(0.25), 0.15625F, 1e-6F);
        EXPECT_NEAR(*values.sample(1.5), 1.5F, 1e-6F);
    }

    TEST(TimedValuesRotationTest, LinearKeysBlendAlongTheArcAndStayUnit)
    {
        const auto rotation = must_create<glm::vec4>({ .times = must(TimeSampling::acyclic({ 0.0, 1.0 })),
            .values = { glm::vec4(0, 0, 0, 1), about_z(90.0F) },
            .kind = ValueKind::Rotation });

        const glm::vec4 middle = *rotation.sample(0.5);
        const glm::vec4 expected = about_z(45.0F);

        EXPECT_NEAR(glm::length(middle), 1.0F, 1e-6F);
        EXPECT_NEAR(middle.z, expected.z, 1e-6F);
        EXPECT_NEAR(middle.w, expected.w, 1e-6F);
        EXPECT_NEAR(middle.x, 0.0F, 1e-7F);
    }

    TEST(TimedValuesRotationTest, ABlendTakesTheShortWayRoundTheOppositeSign)
    {
        const auto rotation = must_create<glm::vec4>({ .times = must(TimeSampling::acyclic({ 0.0, 1.0 })),
            .values = { glm::vec4(0, 0, 0, 1), glm::vec4(0, 0, 0, -1) },
            .kind = ValueKind::Rotation });

        for (const double t : { 0.1, 0.5, 0.9 }) {
            const glm::vec4 value = *rotation.sample(t);
            EXPECT_NEAR(std::abs(value.w), 1.0F, 1e-5F) << t;
            EXPECT_NEAR(value.z, 0.0F, 1e-5F) << t;
        }
    }

    TEST(TimedValuesRotationTest, RotationsHoldWhereNumbersWouldExtrapolateOrAccumulate)
    {
        const auto rotation = must_create<glm::vec4>({ .times = must(TimeSampling::acyclic({ 0.0, 1.0 })),
            .values = { glm::vec4(0, 0, 0, 1), about_z(90.0F) },
            .kind = ValueKind::Rotation });

        EXPECT_EQ(*rotation.sample(5.0, OutOfRange::Extrapolate), rotation.values()[1]);
        EXPECT_EQ(*rotation.sample(-5.0, OutOfRange::Extrapolate), rotation.values()[0]);

        const glm::vec4 repeated = *rotation.sample(1.5, OutOfRange::RepeatOffset);
        const glm::vec4 plain = *rotation.sample(1.5, OutOfRange::Repeat);
        EXPECT_NEAR(glm::length(repeated), 1.0F, 1e-6F);
        EXPECT_EQ(repeated, plain);
    }

    TEST(TimedValuesRotationTest, ACubicRotationSegmentIsRenormalised)
    {
        const auto rotation = must_create<glm::vec4>({ .times = must(TimeSampling::acyclic({ 0.0, 1.0 })),
            .values = { glm::vec4(0, 0, 0, 1), about_z(90.0F) },
            .in_tangents = { glm::vec4(0.0F), glm::vec4(0.0F) },
            .out_tangents = { glm::vec4(0.0F), glm::vec4(0.0F) },
            .default_mode = KeyInterpolation::CubicSpline,
            .kind = ValueKind::Rotation });

        for (const double t : { 0.1, 0.4, 0.5, 0.8 }) {
            EXPECT_NEAR(glm::length(*rotation.sample(t)), 1.0F, 1e-6F) << t;
        }
    }

    TEST(TimedValuesTypesTest, VectorAndDoubleValuesBlendComponentWise)
    {
        const auto vec3 = must_create<glm::vec3>({ .times = must(TimeSampling::acyclic({ 0.0, 2.0 })),
            .values = { glm::vec3(0, 10, -4), glm::vec3(2, 20, 4) } });
        const auto vec2 = must_create<glm::vec2>({ .times = must(TimeSampling::acyclic({ 0.0, 2.0 })),
            .values = { glm::vec2(0, 1), glm::vec2(4, 3) } });
        const auto scalar = must_create<double>({ .times = must(TimeSampling::acyclic({ 0.0, 2.0 })),
            .values = { 1.0, 2.0 } });

        EXPECT_EQ(*vec3.sample(1.0), glm::vec3(1, 15, 0));
        EXPECT_EQ(*vec2.sample(1.0), glm::vec2(2, 2));
        EXPECT_DOUBLE_EQ(*scalar.sample(1.0), 1.5);
        EXPECT_EQ(*vec3.sample(4.0, OutOfRange::Extrapolate), glm::vec3(4, 30, 12));
        EXPECT_EQ(*vec3.sample(3.0, OutOfRange::RepeatOffset), glm::vec3(3, 25, 8));
    }

    TEST(TimedValuesTypesTest, HeldTypesStepAndIgnoreBlendingPolicies)
    {
        const auto names = must_create<std::string>({ .times = must(TimeSampling::acyclic({ 0.0, 1.0, 2.0 })),
            .values = { "a", "b", "c" },
            .default_mode = KeyInterpolation::Step });

        EXPECT_EQ(*names.sample(0.5), "a");
        EXPECT_EQ(*names.sample(1.0), "b");
        EXPECT_EQ(*names.sample(1.99), "b");
        EXPECT_EQ(*names.sample(-1.0, OutOfRange::Hold), "a");
        EXPECT_EQ(*names.sample(9.0, OutOfRange::Extrapolate), "c");
        EXPECT_EQ(*names.sample(2.5, OutOfRange::RepeatOffset), "a");
        EXPECT_EQ(*names.sample(1.0, OutOfRange::Hold, TimeLimit::Before), "a");
        EXPECT_FALSE(names.sample(9.0, OutOfRange::Absent).has_value());
    }

    TEST(TimedValuesTypesTest, AnExactRateSamplesEveryKeyBackExactly)
    {
        const auto sampling = must(TimeSampling::uniform_rate({ 24000, 1001 }));
        std::vector<float> frames(100000);
        std::iota(frames.begin(), frames.end(), 0.0F);
        const auto values = must_create<float>({ .times = sampling, .values = frames });

        for (size_t k = 0; k < frames.size(); k += 31) {
            ASSERT_EQ(*values.sample(*sampling.time(k)), static_cast<float>(k)) << k;
        }
        EXPECT_EQ(*values.sample(*sampling.time(99999)), 99999.0F);
    }

    TEST(TimedValuesPropertyTest, MonotoneKeysGiveMonotoneSamplesOnRandomTimes)
    {
        Lcg random(5);
        std::vector<double> times;
        std::vector<float> keys;
        double time = 0.0;
        float key = 0.0F;
        for (int i = 0; i < 300; ++i) {
            time += 1.0e-3 + random.unit();
            key += static_cast<float>(random.unit());
            times.push_back(time);
            keys.push_back(key);
        }
        const auto values = must_create<float>({ .times = must(TimeSampling::acyclic(times)), .values = keys });

        std::vector<double> queries;
        for (int i = 0; i < 100000; ++i) {
            queries.push_back(-1.0 + random.unit() * (time + 2.0));
        }
        std::ranges::sort(queries);

        float previous = -k_finf;
        for (const double t : queries) {
            const float sampled = *values.sample(t);
            ASSERT_GE(sampled, previous - 1.0e-4F) << t;
            ASSERT_GE(sampled, keys.front() - 1.0e-4F);
            ASSERT_LE(sampled, keys.back() + 1.0e-4F);
            previous = sampled;
        }
    }

    TEST(TimedValuesPropertyTest, SamplesStayBetweenTheirNeighbouringKeysWhateverTheSegmentIs)
    {
        Lcg random(11);
        std::vector<double> times;
        std::vector<float> keys;
        double time = -2.0;
        for (int i = 0; i < 200; ++i) {
            time += 0.01 + random.unit();
            times.push_back(time);
            keys.push_back(static_cast<float>((random.unit() - 0.5) * 100.0));
        }
        const auto values = must_create<float>({ .times = must(TimeSampling::acyclic(times)), .values = keys });

        for (int i = 0; i < 50000; ++i) {
            const double t = times.front() + random.unit() * (times.back() - times.front());
            const auto where = values.sampling().locate(t, keys.size());
            const float sampled = *values.sample(t);
            const float low = std::min(keys[where->lower], keys[where->upper]);
            const float high = std::max(keys[where->lower], keys[where->upper]);
            ASSERT_GE(sampled, low - 1e-3F);
            ASSERT_LE(sampled, high + 1e-3F);
        }
    }

    TEST(TimedValuesTendencyTest, TheTendencyAnswersEveryTimeAndMapsAbsentToHold)
    {
        const auto values = ramp();

        const auto held = values.as_tendency(OutOfRange::Absent);
        const auto repeated = values.as_tendency(OutOfRange::Repeat);

        EXPECT_EQ(held(-5.0), 0.0F);
        EXPECT_EQ(held(99.0), 30.0F);
        EXPECT_FLOAT_EQ(held(0.5), 5.0F);
        EXPECT_FLOAT_EQ(repeated(4.0), 10.0F);
        EXPECT_EQ(held(k_nan), 0.0F);
        EXPECT_EQ(repeated(k_inf), 0.0F);
        EXPECT_EQ(held.evaluate(1.0), 10.0F);
    }

    TEST(TimedValuesTendencyTest, ATendencyOutlivesTheKeysItCameFrom)
    {
        std::optional<Kinesis::Tendency<double, float>> tendency;
        {
            const auto values = ramp();
            tendency = values.as_tendency();
        }

        EXPECT_FLOAT_EQ(tendency->operator()(2.0), 20.0F);
    }

    TEST(TimedValuesConcurrencyTest, ManyThreadsSampleOneSetOfKeysWithTheSameAnswers)
    {
        std::vector<glm::vec3> keys(2000);
        for (size_t i = 0; i < keys.size(); ++i) {
            const auto k = static_cast<float>(i);
            keys[i] = glm::vec3(k, k * 0.5F, -k);
        }
        const auto values = must_create<glm::vec3>({ .times = must(TimeSampling::uniform_rate({ 24000, 1001 })),
            .values = keys,
            .default_mode = KeyInterpolation::Linear });

        auto run = [&values](uint64_t seed) {
            Lcg random(seed);
            double sum = 0.0;
            for (int i = 0; i < 20000; ++i) {
                const double t = random.unit() * 100.0 - 5.0;
                const auto policy = i % 3 == 0 ? OutOfRange::Repeat : (i % 3 == 1 ? OutOfRange::PingPong : OutOfRange::Hold);
                const glm::vec3 value = *values.sample(t, policy);
                sum += static_cast<double>(value.x + value.y + value.z);
            }
            return sum;
        };

        constexpr int threads = 8;
        std::vector<double> results(threads, 0.0);
        std::vector<std::thread> pool;
        for (int i = 0; i < threads; ++i) {
            pool.emplace_back([&results, &run, i] { results[static_cast<size_t>(i)] = run(1234); });
        }
        for (auto& thread : pool) {
            thread.join();
        }

        const double expected = run(1234);
        for (const double result : results) {
            EXPECT_EQ(result, expected);
        }
    }

}
}
