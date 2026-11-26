#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kriya/ReflowHelpers/CourseTime.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Kriya::CourseEdge;
    using Kriya::CourseSetting;
    using Kriya::CourseStep;
    using Kriya::CourseTime;

    /** Eight steps a second, so every step is an exact eighth of a second. */
    constexpr double k_course_time_rate = 8.0;
    constexpr double k_course_time_step = 0.125;

    CourseTime fresh(double step_rate = k_course_time_rate)
    {
        auto made = CourseTime::create(step_rate);
        if (!made) {
            throw std::runtime_error("step rate was rejected");
        }
        return *made;
    }

    CourseSetting course_time_over(double in, double out, CourseEdge edge = CourseEdge::Once, double speed = 1.0)
    {
        CourseSetting setting;
        setting.in = in;
        setting.out = out;
        setting.edge = edge;
        setting.speed = speed;
        return setting;
    }

    std::shared_ptr<const Kinesis::TimeMap> course_time_map(std::function<double(double)> fn)
    {
        return std::make_shared<const Kinesis::TimeMap>(Kinesis::TimeMap {
            .fn = [fn = std::move(fn)](const double& x) { return fn(x); } });
    }

    CourseStep take(CourseTime& course, const CourseSetting& setting, int steps = 1)
    {
        std::optional<CourseStep> last;
        for (int i = 0; i < steps; ++i) {
            last = course.step(setting);
            if (!last) {
                throw std::runtime_error("a step was refused");
            }
        }
        return *last;
    }

    TEST(CourseTimeTest, AStepRateThatCannotFormAPeriodIsRefused)
    {
        for (const double bad : { 0.0, -1.0, -0.0, std::numeric_limits<double>::quiet_NaN(),
                 std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
                 std::numeric_limits<double>::denorm_min(), 1.0E-320 }) {
            EXPECT_FALSE(CourseTime::create(bad).has_value()) << bad;
        }
        EXPECT_TRUE(CourseTime::create(60.0).has_value());
        EXPECT_TRUE(CourseTime::create(1.0E-3).has_value());
        EXPECT_DOUBLE_EQ(fresh(48000.0).step_rate(), 48000.0);
    }

    TEST(CourseTimeTest, AnUnusableSettingStepsNothing)
    {
        auto course = fresh();
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const double inf = std::numeric_limits<double>::infinity();

        auto bad = course_time_over(0.0, 1.0);
        bad.speed = nan;
        EXPECT_FALSE(course.step(bad).has_value());
        bad = course_time_over(0.0, 1.0);
        bad.speed = inf;
        EXPECT_FALSE(course.step(bad).has_value());
        bad = course_time_over(0.0, 1.0);
        bad.offset = nan;
        EXPECT_FALSE(course.step(bad).has_value());
        EXPECT_FALSE(course.step(course_time_over(nan, 1.0)).has_value());
        EXPECT_FALSE(course.step(course_time_over(0.0, inf)).has_value());
        EXPECT_FALSE(course.step(course_time_over(1.0, 1.0)).has_value());
        EXPECT_FALSE(course.step(course_time_over(2.0, 1.0)).has_value());

        EXPECT_EQ(course.steps(), 0U);
        EXPECT_EQ(course.clock(), 0.0);
        EXPECT_NEAR(take(course, course_time_over(0.0, 1.0)).time, k_course_time_step, 1.0E-12);
    }

    TEST(CourseTimeTest, OnceRunsToTheEndFinishesThereAndHolds)
    {
        auto course = fresh();
        const auto setting = course_time_over(0.0, 1.0);

        const auto before = take(course, setting, 7);
        EXPECT_DOUBLE_EQ(before.time, 0.875);
        EXPECT_FALSE(before.finished);

        const auto at_end = take(course, setting);
        EXPECT_DOUBLE_EQ(at_end.time, 1.0);
        EXPECT_TRUE(at_end.finished);
        EXPECT_TRUE(course.finished());
        EXPECT_EQ(course.steps(), 8U);

        for (int i = 0; i < 5; ++i) {
            const auto held = take(course, setting);
            EXPECT_DOUBLE_EQ(held.time, 1.0);
            EXPECT_TRUE(held.finished);
        }
        EXPECT_EQ(course.steps(), 8U);
    }

    TEST(CourseTimeTest, ANegativeSpeedStartsAtTheEndAndFinishesBelowTheStart)
    {
        auto course = fresh();
        const auto setting = course_time_over(0.0, 1.0, CourseEdge::Once, -1.0);

        EXPECT_DOUBLE_EQ(take(course, setting).time, 0.875);
        const auto at_start = take(course, setting, 7);
        EXPECT_DOUBLE_EQ(at_start.time, 0.0);
        EXPECT_FALSE(at_start.finished);

        const auto below = take(course, setting);
        EXPECT_DOUBLE_EQ(below.time, 0.0);
        EXPECT_TRUE(below.finished);
    }

    TEST(CourseTimeTest, SeekingToTheStartThenRunningBackwardsFinishesAtOnce)
    {
        auto course = fresh();
        const auto setting = course_time_over(0.0, 1.0, CourseEdge::Once, -1.0);

        ASSERT_TRUE(course.seek(0.0, setting));

        const auto step = take(course, setting);
        EXPECT_TRUE(step.finished);
        EXPECT_DOUBLE_EQ(step.time, 0.0);
    }

    TEST(CourseTimeTest, HoldClampsAndNeverFinishes)
    {
        auto course = fresh();
        const auto forward = course_time_over(0.0, 1.0, CourseEdge::Hold);

        const auto end = take(course, forward, 100);
        EXPECT_DOUBLE_EQ(end.time, 1.0);
        EXPECT_FALSE(end.finished);
        EXPECT_EQ(end.crossings, 0U);

        const auto backward = course_time_over(0.0, 1.0, CourseEdge::Hold, -50.0);
        EXPECT_DOUBLE_EQ(take(course, backward, 100).time, 0.0);
        EXPECT_FALSE(course.finished());
    }

    TEST(CourseTimeTest, LoopWrapsAndCountsEveryCrossing)
    {
        auto course = fresh();
        const auto setting = course_time_over(0.0, 1.0, CourseEdge::Loop);

        uint64_t crossings = 0;
        for (int n = 1; n <= 24; ++n) {
            const auto step = take(course, setting);
            crossings += step.crossings;
            EXPECT_GE(step.time, 0.0);
            EXPECT_LE(step.time, 1.0);
            EXPECT_FALSE(step.finished);
            if (n == 9) {
                EXPECT_DOUBLE_EQ(step.time, 0.125);
            }
        }
        EXPECT_EQ(crossings, 3U);
        EXPECT_EQ(course.crossings(), 3U);
    }

    TEST(CourseTimeTest, LoopFinishesAfterItsEdgeCountHoldingTheEdgeItReached)
    {
        auto course = fresh();
        auto setting = course_time_over(0.0, 1.0, CourseEdge::Loop);
        setting.loop_count = 2;

        EXPECT_FALSE(take(course, setting, 15).finished);
        const auto last = take(course, setting);
        EXPECT_TRUE(last.finished);
        EXPECT_EQ(last.crossings, 1U);
        EXPECT_DOUBLE_EQ(last.time, 1.0);
        EXPECT_EQ(course.steps(), 16U);

        EXPECT_TRUE(take(course, setting, 3).finished);
        EXPECT_EQ(course.steps(), 16U);
    }

    TEST(CourseTimeTest, ALoopRunningBackwardsFinishesAtTheStartItReached)
    {
        auto course = fresh();
        auto setting = course_time_over(0.0, 1.0, CourseEdge::Loop, -1.0);
        setting.loop_count = 1;

        const auto last = take(course, setting, 9);

        EXPECT_TRUE(last.finished);
        EXPECT_DOUBLE_EQ(last.time, 0.0);
    }

    TEST(CourseTimeTest, ALoopRunningBackwardsCountsTheCrossingBelowTheStart)
    {
        auto course = fresh();
        const auto setting = course_time_over(0.0, 1.0, CourseEdge::Loop, -1.0);

        EXPECT_EQ(take(course, setting, 8).crossings, 0U);
        const auto below = take(course, setting);
        EXPECT_EQ(below.crossings, 1U);
        EXPECT_DOUBLE_EQ(below.time, 0.875);
    }

    TEST(CourseTimeTest, TheFirstStepCountsACrossingFromTheStartingPosition)
    {
        auto course = fresh();
        const auto setting = course_time_over(0.0, 1.0, CourseEdge::Loop, -1.0);
        ASSERT_TRUE(course.seek(0.0, setting));

        const auto first = take(course, setting);

        EXPECT_EQ(first.crossings, 1U);
        EXPECT_DOUBLE_EQ(first.time, 0.875);
    }

    TEST(CourseTimeTest, PingPongReflectsAndFlagsTheBackwardPass)
    {
        auto course = fresh();
        const auto setting = course_time_over(0.0, 1.0, CourseEdge::PingPong);

        const auto turn = take(course, setting, 8);
        EXPECT_DOUBLE_EQ(turn.time, 1.0);
        EXPECT_TRUE(turn.reversed);
        EXPECT_EQ(turn.crossings, 1U);

        const auto back = take(course, setting);
        EXPECT_DOUBLE_EQ(back.time, 0.875);
        EXPECT_TRUE(back.reversed);
        EXPECT_EQ(back.crossings, 0U);

        const auto forward_again = take(course, setting, 7);
        EXPECT_DOUBLE_EQ(forward_again.time, 0.0);
        EXPECT_FALSE(forward_again.reversed);
        EXPECT_EQ(forward_again.crossings, 1U);
    }

    TEST(CourseTimeTest, LoopedAndReflectedTimeNeverLeavesTheRange)
    {
        uint64_t seed = 12345;
        const auto next = [&seed] {
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            return static_cast<double>(seed >> 11) / 9007199254740992.0;
        };

        for (const auto edge : { CourseEdge::Once, CourseEdge::Hold, CourseEdge::Loop, CourseEdge::PingPong }) {
            auto course = fresh(60.0);
            for (int i = 0; i < 4000; ++i) {
                auto setting = course_time_over(-3.0 + next(), 2.0 + 5.0 * next(), edge, 40.0 * (next() - 0.5));
                setting.offset = 10.0 * (next() - 0.5);
                const auto step = course.step(setting);
                ASSERT_TRUE(step.has_value());
                if (step->finished) {
                    continue;
                }
                EXPECT_GE(step->time, setting.in);
                EXPECT_LE(step->time, setting.out);
            }
        }
    }

    TEST(CourseTimeTest, ASpeedChangeCarriesOnFromWhereTheClockIs)
    {
        auto course = fresh();
        auto setting = course_time_over(0.0, 10.0, CourseEdge::Hold);

        EXPECT_DOUBLE_EQ(take(course, setting, 4).time, 0.5);

        setting.speed = 2.0;
        EXPECT_DOUBLE_EQ(take(course, setting).time, 0.75);

        setting.speed = 0.0;
        EXPECT_DOUBLE_EQ(take(course, setting, 6).time, 0.75);

        setting.speed = -1.0;
        EXPECT_DOUBLE_EQ(take(course, setting).time, 0.625);
        EXPECT_DOUBLE_EQ(course.clock(), 0.625);
    }

    TEST(CourseTimeTest, RunningBackwardsRetracesTheForwardPath)
    {
        auto course = fresh();
        auto setting = course_time_over(0.0, 10.0, CourseEdge::Hold);

        std::vector<double> forward;
        forward.reserve(6);
        for (int i = 0; i < 6; ++i) {
            forward.push_back(take(course, setting).time);
        }

        setting.speed = -1.0;
        for (int i = 4; i >= 0; --i) {
            EXPECT_DOUBLE_EQ(take(course, setting).time, forward[static_cast<size_t>(i)]);
        }
    }

    TEST(CourseTimeTest, ATimeMapReplacesTheClockAndIgnoresTheStart)
    {
        auto course = fresh();
        auto setting = course_time_over(0.0, 10.0, CourseEdge::Hold);
        setting.time_map = course_time_map([](double x) { return 2.0 * x + 0.25; });

        EXPECT_DOUBLE_EQ(take(course, setting).time, 0.5);
        EXPECT_DOUBLE_EQ(take(course, setting).time, 0.75);
    }

    TEST(CourseTimeTest, TheOffsetIsAddedBeforeTheTimeMap)
    {
        auto course = fresh();
        auto setting = course_time_over(0.0, 100.0, CourseEdge::Hold);
        setting.time_map = course_time_map([](double x) { return x * x; });
        setting.offset = 1.0;

        EXPECT_DOUBLE_EQ(take(course, setting).time, 1.125 * 1.125);
    }

    TEST(CourseTimeTest, TheOffsetShiftsAnUnwarpedCourseFromItsStart)
    {
        auto course = fresh();
        auto setting = course_time_over(2.0, 100.0, CourseEdge::Hold);
        setting.offset = 0.5;

        EXPECT_DOUBLE_EQ(take(course, setting).time, 2.0 + 0.5 + k_course_time_step);
    }

    TEST(CourseTimeTest, ATimeMapThatAnswersNothingFiniteStepsNothing)
    {
        auto course = fresh();
        auto setting = course_time_over(0.0, 10.0, CourseEdge::Hold);
        setting.time_map = course_time_map([](double x) { return x > 0.3 ? std::numeric_limits<double>::quiet_NaN() : x; });

        EXPECT_DOUBLE_EQ(take(course, setting, 2).time, 0.25);
        EXPECT_FALSE(course.step(setting).has_value());
        EXPECT_FALSE(course.step(setting).has_value());
        EXPECT_EQ(course.steps(), 2U);
        EXPECT_DOUBLE_EQ(course.clock(), 0.25);

        setting.time_map = course_time_map([](double x) { return x; });
        EXPECT_DOUBLE_EQ(take(course, setting).time, 0.375);
    }

    TEST(CourseTimeTest, ARepeatRestartsTheTimeMapFromZeroWithoutCountingACrossing)
    {
        auto course = fresh();
        auto setting = course_time_over(0.0, 0.4, CourseEdge::Loop);
        setting.time_map = course_time_map([](double x) { return x; });
        setting.repeat = course_time_map([](double) { return 0.5; });

        EXPECT_DOUBLE_EQ(take(course, setting).time, 0.125);
        EXPECT_DOUBLE_EQ(take(course, setting).time, 0.25);
        EXPECT_DOUBLE_EQ(take(course, setting).time, 0.375);
        EXPECT_EQ(course.crossings(), 0U);

        const auto restart = take(course, setting);
        EXPECT_DOUBLE_EQ(restart.time, 0.0);
        EXPECT_EQ(restart.crossings, 0U);
        EXPECT_EQ(course.crossings(), 0U);
        EXPECT_DOUBLE_EQ(take(course, setting).time, 0.125);
    }

    TEST(CourseTimeTest, ARepeatLengthThatShrinksGivesAStutter)
    {
        auto course = fresh();
        auto setting = course_time_over(0.0, 100.0, CourseEdge::Hold);
        setting.time_map = course_time_map([](double x) { return x; });
        setting.repeat = course_time_map([](double at) { return at < 0.5 ? 0.5 : 0.25; });

        std::vector<double> times;
        times.reserve(8);
        for (int i = 0; i < 8; ++i) {
            times.push_back(take(course, setting).time);
        }

        EXPECT_EQ(times, (std::vector<double> { 0.125, 0.25, 0.375, 0.0, 0.125, 0.0, 0.125, 0.0 }));
    }

    TEST(CourseTimeTest, ARepeatLengthBelowOneStepOrNotFiniteRestartsEveryStep)
    {
        for (const double length : { 0.0, -5.0, 1.0E-12, std::numeric_limits<double>::quiet_NaN(),
                 -std::numeric_limits<double>::infinity() }) {
            auto course = fresh();
            auto setting = course_time_over(0.0, 100.0, CourseEdge::Hold);
            setting.time_map = course_time_map([](double x) { return x; });
            setting.repeat = course_time_map([length](double) { return length; });

            for (int i = 0; i < 5; ++i) {
                EXPECT_DOUBLE_EQ(take(course, setting).time, 0.0) << length;
            }
        }
    }

    TEST(CourseTimeTest, ARepeatThatRunsBackwardsRestartsOnTheSameLength)
    {
        auto course = fresh();
        auto setting = course_time_over(-100.0, 100.0, CourseEdge::Hold, -1.0);
        setting.time_map = course_time_map([](double x) { return x; });
        setting.repeat = course_time_map([](double) { return 0.5; });

        std::vector<double> times;
        times.reserve(5);
        for (int i = 0; i < 5; ++i) {
            times.push_back(take(course, setting).time);
        }

        EXPECT_EQ(times, (std::vector<double> { -0.125, -0.25, -0.375, 0.0, -0.125 }));
    }

    TEST(CourseTimeTest, PassesCountTheRepeatsBegunAndStartOverWithANewPass)
    {
        auto course = fresh();
        auto setting = course_time_over(0.0, 100.0, CourseEdge::Hold);
        setting.time_map = course_time_map([](double x) { return x; });
        setting.repeat = course_time_map([](double) { return 0.5; });

        EXPECT_EQ(take(course, setting, 3).passes, 0U);
        EXPECT_EQ(take(course, setting).passes, 1U);
        EXPECT_EQ(take(course, setting, 4).passes, 2U);
        EXPECT_EQ(course.passes(), 2U);

        ASSERT_TRUE(course.seek_clock(0.0));
        EXPECT_EQ(course.passes(), 0U);
        EXPECT_EQ(take(course, setting).passes, 0U);
    }

    TEST(CourseTimeTest, RemovingTheRepeatForgetsItsOrigin)
    {
        auto course = fresh();
        auto setting = course_time_over(0.0, 100.0, CourseEdge::Hold);
        setting.time_map = course_time_map([](double x) { return x; });
        setting.repeat = course_time_map([](double) { return 0.5; });

        take(course, setting, 5);
        EXPECT_DOUBLE_EQ(course.clock(), 0.625);

        setting.repeat.reset();
        EXPECT_DOUBLE_EQ(take(course, setting).time, 0.75);
    }

    TEST(CourseTimeTest, SeekPutsTheSourceTimeAndStartsAFreshPass)
    {
        auto course = fresh();
        auto setting = course_time_over(0.0, 1.0);

        take(course, setting, 8);
        ASSERT_TRUE(course.finished());

        ASSERT_TRUE(course.seek(0.5, setting));
        EXPECT_FALSE(course.finished());
        EXPECT_EQ(course.crossings(), 0U);
        EXPECT_DOUBLE_EQ(*course.resolve(setting), 0.5);
        EXPECT_DOUBLE_EQ(take(course, setting).time, 0.625);
    }

    TEST(CourseTimeTest, SeekTakesTheOffsetIntoAccount)
    {
        auto course = fresh();
        auto setting = course_time_over(0.0, 10.0, CourseEdge::Hold);
        setting.offset = 1.0;

        ASSERT_TRUE(course.seek(3.0, setting));

        EXPECT_DOUBLE_EQ(*course.resolve(setting), 3.0);
        EXPECT_DOUBLE_EQ(take(course, setting).time, 3.125);
    }

    TEST(CourseTimeTest, SeekRefusesATimeMapAnUnusableSettingAndANonFiniteTime)
    {
        auto course = fresh();
        auto setting = course_time_over(0.0, 10.0, CourseEdge::Hold);
        take(course, setting, 3);

        auto warped = setting;
        warped.time_map = course_time_map([](double x) { return x; });
        EXPECT_FALSE(course.seek(1.0, warped));
        EXPECT_FALSE(course.seek(std::numeric_limits<double>::quiet_NaN(), setting));
        EXPECT_FALSE(course.seek(1.0, course_time_over(1.0, 1.0)));

        EXPECT_DOUBLE_EQ(course.clock(), 0.375);
        EXPECT_DOUBLE_EQ(take(course, setting).time, 0.5);
    }

    TEST(CourseTimeTest, SeekClockMovesATimeMapedCourse)
    {
        auto course = fresh();
        auto setting = course_time_over(0.0, 10.0, CourseEdge::Once);
        setting.time_map = course_time_map([](double x) { return x; });

        ASSERT_TRUE(course.seek_clock(0.5));
        EXPECT_DOUBLE_EQ(take(course, setting).time, 0.625);
        EXPECT_FALSE(course.seek_clock(std::numeric_limits<double>::infinity()));
        EXPECT_FALSE(course.seek_clock(std::numeric_limits<double>::quiet_NaN()));
        EXPECT_DOUBLE_EQ(course.clock(), 0.625);
    }

    TEST(CourseTimeTest, RestartGoesBackToTheStartAndKeepsCountingSteps)
    {
        auto course = fresh();
        auto setting = course_time_over(0.0, 1.0);
        take(course, setting, 8);

        course.restart();

        EXPECT_FALSE(course.finished());
        EXPECT_EQ(course.crossings(), 0U);
        EXPECT_EQ(course.clock(), 0.0);
        EXPECT_EQ(course.steps(), 8U);
        EXPECT_DOUBLE_EQ(take(course, setting).time, 0.125);
        EXPECT_EQ(course.steps(), 9U);
    }

    TEST(CourseTimeTest, ResolveReadsAnOffsetPartWithoutSteppingOrChangingAnything)
    {
        auto course = fresh();
        const auto setting = course_time_over(0.0, 1.0, CourseEdge::Loop);
        take(course, setting, 2);

        EXPECT_DOUBLE_EQ(*course.resolve(setting), 0.25);
        EXPECT_DOUBLE_EQ(*course.resolve(setting, 0.5), 0.75);
        EXPECT_NEAR(*course.resolve(setting, 0.9), 0.15, 1.0E-12);
        EXPECT_DOUBLE_EQ(*course.resolve(setting, -0.5), 0.75);
        EXPECT_FALSE(course.resolve(setting, std::numeric_limits<double>::quiet_NaN()).has_value());
        EXPECT_FALSE(course.resolve(course_time_over(1.0, 0.0)).has_value());

        EXPECT_EQ(course.steps(), 2U);
        EXPECT_DOUBLE_EQ(take(course, setting).time, 0.375);
    }

    TEST(CourseTimeTest, ResolveOfAFinishedCourseIsItsHeldTime)
    {
        auto course = fresh();
        const auto setting = course_time_over(0.0, 1.0);
        take(course, setting, 9);

        EXPECT_DOUBLE_EQ(*course.resolve(setting), 1.0);
        EXPECT_DOUBLE_EQ(*course.resolve(setting, 0.5), 1.0);
    }

    TEST(CourseTimeTest, LoweringTheEdgeCountMidRunFinishesWhereTheCourseIs)
    {
        auto course = fresh();
        auto setting = course_time_over(0.0, 1.0, CourseEdge::Loop);
        take(course, setting, 12);
        ASSERT_EQ(course.crossings(), 1U);

        setting.loop_count = 1;
        const auto step = take(course, setting);

        EXPECT_TRUE(step.finished);
        EXPECT_EQ(step.crossings, 0U);
        EXPECT_DOUBLE_EQ(step.time, 0.625);
    }

    TEST(CourseTimeTest, ChangingTheRangeMidRunNeverEscapesTheNewOne)
    {
        auto course = fresh();
        auto setting = course_time_over(0.0, 10.0, CourseEdge::Loop);
        take(course, setting, 40);

        setting.in = 1.0;
        setting.out = 1.5;
        for (int i = 0; i < 20; ++i) {
            const auto step = take(course, setting);
            EXPECT_GE(step.time, 1.0);
            EXPECT_LE(step.time, 1.5);
        }
    }

    TEST(CourseTimeTest, ALongRunStaysExactBecauseTheClockIsTheStepCount)
    {
        auto course = fresh(60.0);
        const auto setting = course_time_over(0.0, 1.0E9, CourseEdge::Hold);

        constexpr int steps = 2'000'000;
        for (int i = 0; i < steps; ++i) {
            ASSERT_TRUE(course.step(setting).has_value());
        }

        EXPECT_EQ(course.steps(), static_cast<uint64_t>(steps));
        EXPECT_NEAR(course.clock(), static_cast<double>(steps) / 60.0, 1.0E-9);
    }

    TEST(CourseTimeTest, ASpeedThatOverflowsTheClockStepsNothing)
    {
        auto course = fresh(1.0E-5);
        auto setting = course_time_over(0.0, 1.0, CourseEdge::Hold, 1.0E308);

        EXPECT_FALSE(course.step(setting).has_value());
        EXPECT_EQ(course.steps(), 0U);

        setting.speed = 1.0;
        EXPECT_TRUE(course.step(setting).has_value());
        EXPECT_EQ(course.steps(), 1U);
    }

    TEST(CourseTimeTest, HugeUnfoldedTimesStillFoldIntoTheRange)
    {
        auto course = fresh();
        auto setting = course_time_over(0.0, 1.0, CourseEdge::PingPong, 1.0E300);

        for (int i = 0; i < 10; ++i) {
            const auto step = take(course, setting);
            EXPECT_GE(step.time, 0.0);
            EXPECT_LE(step.time, 1.0);
        }
    }

    TEST(CourseTimeTest, ManyCoursesOnOneSettingStayIndependent)
    {
        auto a = fresh();
        auto b = fresh();
        const auto setting = course_time_over(0.0, 1.0, CourseEdge::Loop);

        take(a, setting, 5);
        take(b, setting, 2);
        EXPECT_EQ(a.steps(), 5U);
        EXPECT_EQ(b.steps(), 2U);
        EXPECT_DOUBLE_EQ(*a.resolve(setting), 0.625);
        EXPECT_DOUBLE_EQ(*b.resolve(setting), 0.25);
    }

}
}
