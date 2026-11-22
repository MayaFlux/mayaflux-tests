#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kriya/Course.hpp"
#include "MayaFlux/Transitive/Facet/ControlBind.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Kriya::Course;
    using Kriya::CourseEdge;
    using Kriya::CourseEvent;
    using Kriya::CourseEventKind;
    using Kriya::CourseSetting;

    /** Eight steps a second, so every step is an exact eighth of a second. */
    constexpr double k_rate = 8.0;
    constexpr double k_step = 0.125;

    CourseSetting over(double in, double out, CourseEdge edge = CourseEdge::Once)
    {
        CourseSetting setting;
        setting.in = in;
        setting.out = out;
        setting.edge = edge;
        return setting;
    }

    std::shared_ptr<Course> make(const CourseSetting& setting, bool paused = false)
    {
        auto course = Course::create(k_rate, setting, paused);
        if (!course) {
            throw std::runtime_error("course was rejected");
        }
        return course;
    }

    Kinesis::TimeMap map(std::function<double(double)> fn)
    {
        return Kinesis::TimeMap { .fn = [fn = std::move(fn)](const double& x) { return fn(x); } };
    }

    double run(Course& course, int steps)
    {
        std::optional<Kriya::CourseStep> step;
        for (int i = 0; i < steps; ++i) {
            step = course.advance();
            if (!step) {
                throw std::runtime_error("an advance was refused");
            }
        }
        return step->time;
    }

    /** What a subscriber has been told so far, read the way a chain reads it. */
    std::vector<CourseEvent> drain(const std::shared_ptr<Kriya::CourseEvents>& source)
    {
        std::vector<CourseEvent> events;
        for (;;) {
            auto awaiter = source->next();
            if (!awaiter.await_ready()) {
                break;
            }
            events.push_back(awaiter.await_resume());
        }
        return events;
    }

    TEST(CourseTest, ABadStepRateOrSettingMakesNoCourse)
    {
        EXPECT_EQ(Course::create(0.0, over(0.0, 1.0)), nullptr);
        EXPECT_EQ(Course::create(std::numeric_limits<double>::quiet_NaN(), over(0.0, 1.0)), nullptr);
        EXPECT_EQ(Course::create(k_rate, over(1.0, 1.0)), nullptr);
        EXPECT_EQ(Course::create(k_rate, over(0.0, std::numeric_limits<double>::infinity())), nullptr);
        EXPECT_NE(Course::create(k_rate, over(0.0, 1.0)), nullptr);
    }

    TEST(CourseTest, ARunningCourseStepsAndPublishesItsState)
    {
        const auto course = make(over(2.0, 4.0, CourseEdge::Hold));

        EXPECT_EQ(course->time(), 2.0);
        EXPECT_FALSE(course->frame().has_value());

        EXPECT_DOUBLE_EQ(run(*course, 4), 2.5);
        EXPECT_DOUBLE_EQ(course->time(), 2.5);
        EXPECT_DOUBLE_EQ(course->progress(), 0.25);
        EXPECT_EQ(course->steps(), 4U);
        EXPECT_FALSE(course->finished());
        EXPECT_FALSE(course->paused());
    }

    TEST(CourseTest, APausedCourseHoldsUntilStarted)
    {
        const auto course = make(over(0.0, 10.0, CourseEdge::Hold), true);

        EXPECT_DOUBLE_EQ(run(*course, 3), 0.0);
        EXPECT_TRUE(course->paused());
        EXPECT_EQ(course->steps(), 0U);

        ASSERT_TRUE(course->start());
        EXPECT_DOUBLE_EQ(run(*course, 2), 0.25);
        EXPECT_FALSE(course->paused());

        ASSERT_TRUE(course->pause());
        EXPECT_DOUBLE_EQ(run(*course, 5), 0.25);
    }

    TEST(CourseTest, ASeekWhilePausedMovesWhatIsShown)
    {
        const auto course = make(over(0.0, 10.0, CourseEdge::Hold), true);

        ASSERT_TRUE(course->seek(4.0));
        EXPECT_DOUBLE_EQ(run(*course, 1), 4.0);
        ASSERT_TRUE(course->nudge(0.5));
        EXPECT_DOUBLE_EQ(run(*course, 1), 4.5);
        ASSERT_TRUE(course->seek_normalized(0.25));
        EXPECT_DOUBLE_EQ(run(*course, 1), 2.5);
        EXPECT_TRUE(course->paused());
    }

    TEST(CourseTest, CommandsApplyInTheOrderTheyWereQueued)
    {
        const auto course = make(over(0.0, 10.0, CourseEdge::Hold));

        ASSERT_TRUE(course->seek(1.0));
        ASSERT_TRUE(course->pause());
        ASSERT_TRUE(course->seek(6.0));
        ASSERT_TRUE(course->start());

        EXPECT_DOUBLE_EQ(run(*course, 1), 6.0 + k_step);
    }

    TEST(CourseTest, StopGoesBackToTheStartAndRetriggerRunsFromThere)
    {
        const auto course = make(over(0.0, 1.0));
        run(*course, 8);
        ASSERT_TRUE(course->finished());

        ASSERT_TRUE(course->retrigger());
        EXPECT_DOUBLE_EQ(run(*course, 1), k_step);
        EXPECT_FALSE(course->finished());

        ASSERT_TRUE(course->stop());
        EXPECT_DOUBLE_EQ(run(*course, 1), 0.0);
        EXPECT_TRUE(course->paused());
    }

    TEST(CourseTest, ASpeedChangeNeverJumps)
    {
        const auto course = make(over(0.0, 100.0, CourseEdge::Hold));
        run(*course, 4);

        ASSERT_TRUE(course->speed(4.0));
        EXPECT_DOUBLE_EQ(run(*course, 1), 0.5 + 4.0 * k_step);
        ASSERT_TRUE(course->speed(-2.0));
        EXPECT_DOUBLE_EQ(run(*course, 1), 1.0 - 2.0 * k_step);
    }

    TEST(CourseTest, SettersRefuseWhatCannotBeUsedAndLeaveTheSettingAlone)
    {
        const auto course = make(over(0.0, 10.0));
        const double nan = std::numeric_limits<double>::quiet_NaN();

        EXPECT_FALSE(course->speed(nan));
        EXPECT_FALSE(course->offset(std::numeric_limits<double>::infinity()));
        EXPECT_FALSE(course->range(1.0, 1.0));
        EXPECT_FALSE(course->range(2.0, 1.0));
        EXPECT_FALSE(course->range(nan, 3.0));
        EXPECT_FALSE(course->weight(-1.0));
        EXPECT_FALSE(course->weight(nan));
        EXPECT_FALSE(course->seek(nan));
        EXPECT_FALSE(course->seek_clock(std::numeric_limits<double>::infinity()));
        EXPECT_FALSE(course->seek_normalized(nan));
        EXPECT_FALSE(course->nudge(nan));

        const auto setting = course->setting();
        EXPECT_EQ(setting.speed, 1.0);
        EXPECT_EQ(setting.offset, 0.0);
        EXPECT_EQ(setting.in, 0.0);
        EXPECT_EQ(setting.out, 10.0);
        EXPECT_EQ(course->weight(), 1.0);
        EXPECT_DOUBLE_EQ(run(*course, 1), k_step);
    }

    TEST(CourseTest, AControlReaderReadsANumberAndASnapshotMember)
    {
        const auto course = make(over(2.0, 6.0));
        const auto speed = control_reader<&Kriya::CourseControls::speed, double>(course);
        const auto range = control_reader<&Kriya::CourseControls::range, Kriya::CourseRange>(course);
        const auto time_map = control_reader<&Kriya::CourseControls::time_map, Kinesis::TimeMap>(course);

        ASSERT_TRUE(speed().has_value());
        EXPECT_DOUBLE_EQ(*speed(), 1.0);
        ASSERT_TRUE(range().has_value());
        EXPECT_EQ(range()->in, 2.0);
        EXPECT_EQ(range()->out, 6.0);
        EXPECT_FALSE(time_map().has_value());

        ASSERT_TRUE(course->range(0.0, 3.0));
        course->time_map(map([](double t) { return t; }));

        EXPECT_EQ(range()->out, 3.0);
        EXPECT_TRUE(time_map().has_value());
    }

    TEST(CourseTest, ARangeChangeAppliesOnTheNextAdvance)
    {
        const auto course = make(over(0.0, 10.0, CourseEdge::Hold));
        run(*course, 2);

        ASSERT_TRUE(course->range(0.0, 0.25));

        EXPECT_DOUBLE_EQ(run(*course, 1), 0.25);
    }

    TEST(CourseTest, ATimeMapAndARepeatCanBeSetAndClearedWhileRunning)
    {
        const auto course = make(over(0.0, 100.0, CourseEdge::Hold));
        run(*course, 2);

        course->time_map(map([](double x) { return 10.0 + x; }));
        EXPECT_DOUBLE_EQ(run(*course, 1), 10.375);

        course->clear_time_map();
        EXPECT_DOUBLE_EQ(run(*course, 1), 0.5);
    }

    TEST(CourseTest, ASeekUnderATimeMapIsRefusedAndCounted)
    {
        const auto course = make(over(0.0, 100.0, CourseEdge::Hold));
        course->time_map(map([](double x) { return x; }));
        run(*course, 2);

        ASSERT_TRUE(course->seek(50.0));
        EXPECT_DOUBLE_EQ(run(*course, 1), 0.375);
        EXPECT_EQ(course->refusals(), 1U);

        ASSERT_TRUE(course->seek_clock(2.0));
        EXPECT_DOUBLE_EQ(run(*course, 1), 2.125);
    }

    TEST(CourseTest, ATimeMapThatThrowsRefusesTheAdvanceAndCarriesOnWhenFixed)
    {
        const auto course = make(over(0.0, 100.0, CourseEdge::Hold));
        run(*course, 2);

        course->time_map(map([](double x) -> double {
            if (x > 0.3) {
                throw std::runtime_error("no");
            }
            return x;
        }));

        EXPECT_FALSE(course->advance().has_value());
        EXPECT_FALSE(course->advance().has_value());
        EXPECT_EQ(course->refusals(), 2U);
        EXPECT_EQ(course->steps(), 2U);

        course->clear_time_map();
        EXPECT_DOUBLE_EQ(run(*course, 1), 0.375);
    }

    TEST(CourseTest, ATimeMapAnsweringNothingFiniteRefusesTheAdvance)
    {
        const auto course = make(over(0.0, 100.0, CourseEdge::Hold));
        course->time_map(map([](double) { return std::numeric_limits<double>::quiet_NaN(); }));

        EXPECT_FALSE(course->advance().has_value());
        EXPECT_EQ(course->time(), 0.0);
    }

    TEST(CourseTest, FinishingAnnouncesOnceAndASeekMakesItPossibleAgain)
    {
        const auto course = make(over(0.0, 1.0));
        const auto events = course->subscribe(CourseEventKind::Finished);

        run(*course, 12);
        auto told = drain(events);
        ASSERT_EQ(told.size(), 1U);
        EXPECT_EQ(told[0].kind, CourseEventKind::Finished);
        EXPECT_DOUBLE_EQ(told[0].time, 1.0);
        EXPECT_TRUE(course->finished());

        ASSERT_TRUE(course->seek(0.5));
        run(*course, 8);
        told = drain(events);
        ASSERT_EQ(told.size(), 1U);
        EXPECT_EQ(told[0].kind, CourseEventKind::Finished);
    }

    TEST(CourseTest, ARepeatIsAnnouncedAndIsNotACrossing)
    {
        const auto course = make(over(0.0, 1.0, CourseEdge::Loop));
        const auto crossings = course->subscribe(CourseEventKind::Repeated);
        const auto edges = course->subscribe(CourseEventKind::Crossed);
        course->time_map(map([](double x) { return x; }));
        course->repeat(map([](double) { return 0.5; }));

        run(*course, 4);
        const auto told = drain(crossings);

        ASSERT_EQ(told.size(), 1U);
        EXPECT_EQ(told[0].kind, CourseEventKind::Repeated);
        EXPECT_EQ(told[0].value, 1U);
        EXPECT_TRUE(drain(edges).empty());
    }

    TEST(CourseTest, ALoopAnnouncesItsCrossing)
    {
        const auto course = make(over(0.0, 1.0, CourseEdge::Loop));
        const auto events = course->subscribe(CourseEventKind::Crossed);

        run(*course, 8);

        const auto told = drain(events);
        ASSERT_EQ(told.size(), 1U);
        EXPECT_EQ(told[0].kind, CourseEventKind::Crossed);
        EXPECT_EQ(told[0].value, 1U);
        EXPECT_EQ(course->crossings(), 1U);
    }

    TEST(CourseTest, EverySubscriberHearsEveryEvent)
    {
        const auto course = make(over(0.0, 1.0));
        const auto first = course->subscribe(CourseEventKind::Finished);
        const auto second = course->subscribe(CourseEventKind::Finished);

        run(*course, 8);

        EXPECT_EQ(drain(first).size(), 1U);
        EXPECT_EQ(drain(second).size(), 1U);
    }

    TEST(CourseTest, ReportedFramesAreAnnouncedOnlyWhenTheIndexChanges)
    {
        const auto course = make(over(0.0, 10.0, CourseEdge::Hold));
        const auto frames = course->subscribe(CourseEventKind::Frame);
        const auto misses = course->subscribe(CourseEventKind::Missed);

        course->report_frame(3);
        course->report_frame(3);
        EXPECT_EQ(course->frame(), 3U);
        auto told = drain(frames);
        ASSERT_EQ(told.size(), 1U);
        EXPECT_EQ(told[0].value, 3U);

        course->report_frame(4);
        course->report_miss(5);
        EXPECT_EQ(course->frame(), 4U);
        EXPECT_EQ(course->misses(), 1U);

        told = drain(frames);
        ASSERT_EQ(told.size(), 1U);
        EXPECT_EQ(told[0].value, 4U);
        told = drain(misses);
        ASSERT_EQ(told.size(), 1U);
        EXPECT_EQ(told[0].value, 5U);
    }

    TEST(CourseTest, AnAdvanceThatCrossesAndFinishesTellsBothKinds)
    {
        auto setting = over(0.0, 1.0, CourseEdge::Loop);
        setting.loop_count = 1;
        const auto course = make(setting);
        const auto crossed = course->subscribe(CourseEventKind::Crossed);
        const auto finished = course->subscribe(CourseEventKind::Finished);

        run(*course, 8);

        EXPECT_EQ(drain(crossed).size(), 1U);
        EXPECT_EQ(drain(finished).size(), 1U);
    }

    TEST(CourseTest, ASlowSubscriberKeepsOnlyTheLatestOfItsKind)
    {
        const auto course = make(over(0.0, 10.0, CourseEdge::Hold));
        const auto frames = course->subscribe(CourseEventKind::Frame);

        course->report_frame(1);
        course->report_frame(2);
        course->report_frame(3);

        const auto told = drain(frames);
        ASSERT_EQ(told.size(), 1U);
        EXPECT_EQ(told[0].value, 3U);
    }

    TEST(CourseTest, APausedCourseAnnouncesNothing)
    {
        const auto course = make(over(0.0, 1.0, CourseEdge::Loop), true);
        const auto events = course->subscribe(CourseEventKind::Crossed);

        run(*course, 20);

        EXPECT_TRUE(drain(events).empty());
    }

    TEST(CourseTest, TimeAtReadsAnOffsetPartWithoutSteppingOrChangingAnything)
    {
        const auto course = make(over(0.0, 1.0, CourseEdge::Loop));
        run(*course, 2);

        EXPECT_DOUBLE_EQ(*course->time_at(0.0), 0.25);
        EXPECT_DOUBLE_EQ(*course->time_at(0.5), 0.75);
        EXPECT_FALSE(course->time_at(std::numeric_limits<double>::quiet_NaN()).has_value());
        EXPECT_EQ(course->steps(), 2U);
    }

    TEST(CourseTest, ManyThreadsSteerWhileOneThreadAdvances)
    {
        const auto course = make(over(0.0, 4.0, CourseEdge::Loop));
        std::atomic<bool> done { false };
        std::atomic<int> bad { 0 };

        std::vector<std::thread> writers;
        writers.reserve(4);
        for (int t = 0; t < 4; ++t) {
            writers.emplace_back([&course, &done, t] {
                auto i = static_cast<uint64_t>(t);
                while (!done.load()) {
                    switch (i++ % 8) {
                    case 0:
                        course->speed(static_cast<double>(i % 7) - 3.0);
                        break;
                    case 1:
                        course->range(0.0, 1.0 + static_cast<double>(i % 5));
                        break;
                    case 2:
                        (void)course->seek(static_cast<double>(i % 3));
                        break;
                    case 3:
                        (void)course->pause();
                        break;
                    case 4:
                        (void)course->start();
                        break;
                    case 5:
                        course->offset(static_cast<double>(i % 4) * 0.25);
                        break;
                    case 6:
                        course->edge(i % 2 == 0 ? CourseEdge::Loop : CourseEdge::PingPong);
                        break;
                    default:
                        course->weight(static_cast<double>(i % 3));
                        break;
                    }
                }
            });
        }

        for (int i = 0; i < 20000; ++i) {
            const auto step = course->advance();
            EXPECT_TRUE(course->setting().usable());
            if (!step) {
                ++bad;
                continue;
            }
            if (step->time < 0.0 || step->time > 5.0) {
                ++bad;
            }
        }
        done.store(true);
        for (auto& writer : writers) {
            writer.join();
        }

        EXPECT_EQ(bad.load(), 0);
        EXPECT_GT(course->steps(), 0U);
    }

    TEST(CourseTest, TheCourseKeepsASubscriptionAliveAfterTheCallerLetsGo)
    {
        const auto course = make(over(0.0, 1.0));
        std::weak_ptr<Kriya::CourseEvents> weak;
        {
            const auto events = course->subscribe(CourseEventKind::Finished);
            weak = events;
        }

        EXPECT_FALSE(weak.expired());
        run(*course, 8);
    }

}
}
