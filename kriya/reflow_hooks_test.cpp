#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kinesis/Tendency/TendencyFactories.hpp"
#include "MayaFlux/Kriya/Reflow.hpp"
#include "MayaFlux/Kriya/ReflowHelpers/ReflowLive.hpp"
#include "MayaFlux/Kriya/ReflowHelpers/ReflowSelectors.hpp"
#include "MayaFlux/Kriya/ReflowHelpers/ReflowTiming.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Kakshya::FrameRing;
    using Kakshya::ResidentFrames;
    using Kakshya::SampledFrames;
    using Kinesis::TimeSampling;
    using Kriya::Course;
    using Kriya::CourseEdge;
    using Kriya::CourseEventKind;
    using Kriya::CourseSetting;
    using Kriya::FrameContext;
    using Kriya::FrameSelection;
    using Kriya::FrameSelector;
    using Kriya::Reflow;
    using Kriya::ReflowBuilder;
    using Kriya::WeightedFrame;

    constexpr double k_hook_rate = 8.0;

    /** Frame i holds i * scale, one every eighth of a second. */
    std::shared_ptr<const ResidentFrames<int>> hook_numbers(size_t count, int scale = 10)
    {
        std::vector<int> frames;
        for (size_t i = 0; i < count; ++i) {
            frames.push_back(static_cast<int>(i) * scale);
        }
        return ResidentFrames<int>::create(*TimeSampling::uniform(0.125), std::move(frames));
    }

    CourseSetting hook_over(double in, double out, CourseEdge edge = CourseEdge::Once, double speed = 1.0)
    {
        CourseSetting setting;
        setting.in = in;
        setting.out = out;
        setting.edge = edge;
        setting.speed = speed;
        return setting;
    }

    struct HookLog {
        std::vector<int> values;

        Reflow<int>::Sink sink()
        {
            return [this](const std::shared_ptr<const int>& frame, const FrameContext&) { values.push_back(*frame); };
        }
    };

    void hook_run(Reflow<int>& reflow, int cycles)
    {
        for (int i = 0; i < cycles; ++i) {
            reflow.advance();
        }
    }

    FrameSelector<int> hook_value(int value)
    {
        return [value](const FrameContext&, std::vector<WeightedFrame<int>>& into) {
            into.push_back({ .frame = std::make_shared<const int>(value), .weight = 1.0, .index = 0 });
        };
    }

    class HookRamp final : public SampledFrames<int> {
    public:
        [[nodiscard]] std::pair<double, double> extent() const override { return { 0.0, 1.0 }; }

        [[nodiscard]] std::shared_ptr<const int> sample(double time) const override
        {
            return std::make_shared<const int>(static_cast<int>(std::lround(time * 1000.0)));
        }
    };

    std::vector<WeightedFrame<int>> hook_pick(const FrameSelector<int>& selector, double time = 0.0, double elapsed = 0.0)
    {
        std::vector<WeightedFrame<int>> into;
        FrameContext context;
        context.time = time;
        context.elapsed = elapsed;
        selector(context, into);
        return into;
    }

    TEST(ReflowInputTest, ALaneReadsAnotherSourceFromTheNextCycleAndKeepsItsTime)
    {
        HookLog log;
        const auto reflow = ReflowBuilder<int>(hook_numbers(8), log.sink(), k_hook_rate).build();
        ASSERT_NE(reflow, nullptr);
        const auto course = reflow->courses().front();

        hook_run(*reflow, 2);
        EXPECT_TRUE(reflow->set_source(course, hook_numbers(8, 100)));
        hook_run(*reflow, 2);

        EXPECT_EQ(log.values, (std::vector<int> { 0, 10, 200, 300 }));
    }

    TEST(ReflowInputTest, ASwapIsRefusedForACourseThatIsNotTheReflowsOrASourceThatIsEmpty)
    {
        HookLog log;
        const auto reflow = ReflowBuilder<int>(hook_numbers(8), log.sink(), k_hook_rate).build();
        const auto other = ReflowBuilder<int>(hook_numbers(8), log.sink(), k_hook_rate).build();
        ASSERT_NE(reflow, nullptr);
        ASSERT_NE(other, nullptr);

        EXPECT_FALSE(reflow->set_source(other->courses().front(), hook_numbers(4)));
        EXPECT_FALSE(reflow->set_source(reflow->courses().front(), nullptr));
        EXPECT_FALSE(reflow->set_sampled(reflow->courses().front(), nullptr));
        EXPECT_FALSE(reflow->set_selector(reflow->courses().front(), {}));
    }

    TEST(ReflowInputTest, ALaneMayBeGivenASampledSourceOrASelectorInPlaceOfAList)
    {
        HookLog log;
        const auto reflow = ReflowBuilder<int>(hook_numbers(8), log.sink(), k_hook_rate).build();
        ASSERT_NE(reflow, nullptr);
        const auto course = reflow->courses().front();

        hook_run(*reflow, 1);
        ASSERT_TRUE(reflow->set_sampled(course, std::make_shared<HookRamp>()));
        hook_run(*reflow, 1);
        ASSERT_TRUE(reflow->set_selector(course, hook_value(77)));
        hook_run(*reflow, 1);

        ASSERT_EQ(log.values.size(), 3U);
        EXPECT_EQ(log.values[0], 0);
        EXPECT_EQ(log.values[1], 125);
        EXPECT_EQ(log.values[2], 77);
    }

    TEST(ReflowActionTest, AnActionRunsAtTheEndOfEveryIntervalAndNotBeforeTheFirst)
    {
        HookLog log;
        std::vector<uint64_t> seen;
        const auto reflow = ReflowBuilder<int>(hook_numbers(8), log.sink(), k_hook_rate)
                                .every(0.25, [&seen](Reflow<int>& running) { seen.push_back(running.cycle()); })
                                .build();
        ASSERT_NE(reflow, nullptr);

        hook_run(*reflow, 5);

        EXPECT_EQ(seen, (std::vector<uint64_t> { 3, 5 }));
    }

    TEST(ReflowActionTest, AnActionMaySwapTheSourceTheReflowReads)
    {
        HookLog log;
        const auto replacement = hook_numbers(8, 100);
        const auto reflow = ReflowBuilder<int>(hook_numbers(8), log.sink(), k_hook_rate)
                                .every(0.25, [replacement](Reflow<int>& running) {
                                    running.set_source(running.courses().front(), replacement);
                                })
                                .build();
        ASSERT_NE(reflow, nullptr);

        hook_run(*reflow, 5);

        ASSERT_EQ(log.values.size(), 5U);
        EXPECT_EQ(log.values[0], 0);
        EXPECT_EQ(log.values[1], 10);
        EXPECT_EQ(log.values[2], 20);
        EXPECT_EQ(log.values[3], 300);
    }

    TEST(ReflowActionTest, AnActionThatThrowsDoesNotStopTheReflow)
    {
        HookLog log;
        const auto reflow = ReflowBuilder<int>(hook_numbers(8), log.sink(), k_hook_rate)
                                .every(0.125, [](Reflow<int>&) { throw std::runtime_error("action"); })
                                .build();
        ASSERT_NE(reflow, nullptr);

        EXPECT_NO_THROW(hook_run(*reflow, 4));

        EXPECT_EQ(log.values.size(), 4U);
        EXPECT_EQ(reflow->failures(), 0U);
    }

    TEST(ReflowActionTest, AnActionWithNoCallableOrNoIntervalMakesNoReflow)
    {
        HookLog log;

        EXPECT_EQ(ReflowBuilder<int>(hook_numbers(8), log.sink(), k_hook_rate).every(1.0, {}).build(), nullptr);
        EXPECT_EQ(ReflowBuilder<int>(hook_numbers(8), log.sink(), k_hook_rate)
                      .every(std::numeric_limits<double>::quiet_NaN(), [](Reflow<int>&) { })
                      .build(),
            nullptr);
    }

    TEST(ReflowActionTest, ADisabledReflowRunsNoActions)
    {
        HookLog log;
        int calls = 0;
        const auto reflow = ReflowBuilder<int>(hook_numbers(8), log.sink(), k_hook_rate)
                                .every(0.125, [&calls](Reflow<int>&) { ++calls; })
                                .build();
        ASSERT_NE(reflow, nullptr);
        Kriya::ReflowControls off;
        off.enabled = ControlValue(false);
        reflow->set(off);

        hook_run(*reflow, 4);

        EXPECT_EQ(calls, 0);
    }

    std::vector<Kriya::CourseEvent> hook_drain(const std::shared_ptr<Kriya::CourseEvents>& source)
    {
        std::vector<Kriya::CourseEvent> events;
        for (;;) {
            auto awaiter = source->next();
            if (!awaiter.await_ready()) {
                break;
            }
            events.push_back(awaiter.await_resume());
        }
        return events;
    }

    void hook_step(Course& course, int steps = 1)
    {
        for (int i = 0; i < steps; ++i) {
            std::ignore = course.advance(course.advanced());
        }
    }

    TEST(CourseMarkerTest, AMarkerIsAnnouncedWhenAStepPassesIt)
    {
        const auto course = Course::create(k_hook_rate, hook_over(0.0, 1.0));
        const auto id = course->add_marker("cue", 0.3);
        ASSERT_TRUE(id.has_value());
        const auto events = course->subscribe(CourseEventKind::Marked);

        hook_step(*course, 3);
        EXPECT_TRUE(hook_drain(events).empty());
        hook_step(*course, 1);

        const auto told = hook_drain(events);
        ASSERT_EQ(told.size(), 1U);
        EXPECT_EQ(told[0].kind, CourseEventKind::Marked);
        EXPECT_EQ(told[0].value, *id);
        EXPECT_DOUBLE_EQ(told[0].time, 0.3);
    }

    TEST(CourseMarkerTest, AMarkerIsAnnouncedGoingBackwardToo)
    {
        const auto course = Course::create(k_hook_rate, hook_over(0.0, 1.0, CourseEdge::Once, -1.0));
        const auto id = course->add_marker("cue", 0.8);
        const auto events = course->subscribe(CourseEventKind::Marked);

        hook_step(*course, 2);
        EXPECT_TRUE(hook_drain(events).empty());
        hook_step(*course, 1);

        const auto told = hook_drain(events);
        ASSERT_EQ(told.size(), 1U);
        EXPECT_EQ(told[0].value, *id);
    }

    TEST(CourseMarkerTest, ALoopAnnouncesTheMarkersBeforeAndAfterItsEnd)
    {
        const auto course = Course::create(k_hook_rate, hook_over(0.0, 1.0, CourseEdge::Loop, 4.0));
        const auto first = course->add_marker("early", 0.25);
        const auto second = course->add_marker("late", 0.75);
        const auto events = course->subscribe(CourseEventKind::Marked);
        std::vector<uint64_t> heard;

        hook_step(*course, 1);
        for (int step = 0; step < 3; ++step) {
            hook_step(*course, 1);
            for (const auto& event : hook_drain(events)) {
                heard.push_back(event.value);
            }
        }

        EXPECT_EQ(heard, (std::vector<uint64_t> { *first, *second, *first }));
    }

    TEST(CourseMarkerTest, ASeekIsAJumpAndAnnouncesNothing)
    {
        const auto course = Course::create(k_hook_rate, hook_over(0.0, 1.0));
        course->add_marker("cue", 0.5);
        const auto events = course->subscribe(CourseEventKind::Marked);
        hook_step(*course, 2);

        ASSERT_TRUE(course->seek(0.9));
        hook_step(*course, 1);

        EXPECT_TRUE(hook_drain(events).empty());
    }

    TEST(CourseMarkerTest, AMarkerCanBeRemovedAndListed)
    {
        const auto course = Course::create(k_hook_rate, hook_over(0.0, 1.0));
        const auto one = course->add_marker("one", 0.2);
        const auto two = course->add_marker("two", 0.6);

        const auto listed = course->markers();
        ASSERT_EQ(listed.size(), 2U);
        EXPECT_EQ(listed[0].name, "one");
        EXPECT_EQ(listed[1].id, *two);

        EXPECT_TRUE(course->remove_marker(*one));
        EXPECT_FALSE(course->remove_marker(*one));
        EXPECT_EQ(course->markers().size(), 1U);
    }

    TEST(CourseMarkerTest, ATimeThatIsNotFiniteMakesNoMarker)
    {
        const auto course = Course::create(k_hook_rate, hook_over(0.0, 1.0));

        EXPECT_FALSE(course->add_marker("bad", std::numeric_limits<double>::infinity()).has_value());
        EXPECT_TRUE(course->markers().empty());
    }

    TEST(CourseMarkerTest, JumpingGoesToTheFirstMarkerOfAName)
    {
        const auto course = Course::create(k_hook_rate, hook_over(0.0, 1.0));
        course->add_marker("cue", 0.5);
        course->add_marker("cue", 0.8);

        EXPECT_TRUE(course->jump("cue"));
        hook_step(*course, 2);
        EXPECT_NEAR(course->time(), 0.5, 0.26);
        EXPECT_GE(course->time(), 0.5);
        EXPECT_FALSE(course->jump("missing"));
    }

    TEST(CourseFollowTest, ACourseTakesTheTimeOfItsLeaderPlusAnOffset)
    {
        const auto leader = Course::create(k_hook_rate, hook_over(0.0, 1.0, CourseEdge::Hold));
        const auto follower = Course::create(k_hook_rate, hook_over(0.0, 2.0, CourseEdge::Hold));
        ASSERT_TRUE(follower->follow(leader, 0.25));

        for (int i = 0; i < 4; ++i) {
            hook_step(*leader);
            hook_step(*follower);
            EXPECT_DOUBLE_EQ(follower->time(), leader->time() + 0.25);
        }
    }

    TEST(CourseFollowTest, FollowingNothingItselfOrAnOffsetThatIsNotFiniteIsRefused)
    {
        const auto course = Course::create(k_hook_rate, hook_over(0.0, 1.0));
        const auto other = Course::create(k_hook_rate, hook_over(0.0, 1.0));

        EXPECT_FALSE(course->follow(nullptr));
        EXPECT_FALSE(course->follow(course));
        EXPECT_FALSE(course->follow(other, std::numeric_limits<double>::quiet_NaN()));
    }

    TEST(CourseFollowTest, AFollowerHoldsWhereItWasWhenItsLeaderIsGone)
    {
        auto leader = Course::create(k_hook_rate, hook_over(0.0, 1.0, CourseEdge::Hold));
        const auto follower = Course::create(k_hook_rate, hook_over(0.0, 2.0, CourseEdge::Hold));
        ASSERT_TRUE(follower->follow(leader, 0.0));
        hook_step(*leader, 3);
        hook_step(*follower, 2);
        const double held = follower->time();

        leader.reset();
        hook_step(*follower, 2);

        EXPECT_DOUBLE_EQ(follower->time(), held);
    }

    TEST(CourseFollowTest, UnfollowingGivesTheCourseItsOwnClockBack)
    {
        const auto leader = Course::create(k_hook_rate, hook_over(0.0, 1.0));
        const auto follower = Course::create(k_hook_rate, hook_over(0.0, 1.0));
        ASSERT_TRUE(follower->follow(leader));
        ASSERT_NE(follower->setting().time_map, nullptr);

        follower->unfollow();

        EXPECT_EQ(follower->setting().time_map, nullptr);
    }

    TEST(CourseFollowTest, SeekingAFollowerIsRefusedWhileItFollows)
    {
        const auto leader = Course::create(k_hook_rate, hook_over(0.0, 1.0));
        const auto follower = Course::create(k_hook_rate, hook_over(0.0, 1.0));
        ASSERT_TRUE(follower->follow(leader));

        ASSERT_TRUE(follower->seek(0.5));
        hook_step(*follower);

        EXPECT_GE(follower->refusals(), 1U);
    }

    TEST(CourseFollowTest, LockStepMakesEveryCourseFollowTheLeaderAndSkipsWhatCannot)
    {
        const auto leader = Course::create(k_hook_rate, hook_over(0.0, 1.0));
        const std::vector<std::shared_ptr<Course>> followers {
            Course::create(k_hook_rate, hook_over(0.0, 1.0)),
            nullptr,
            Course::create(k_hook_rate, hook_over(0.0, 1.0)),
        };

        const auto count = Kriya::lock_step(leader, followers, 0.5);

        EXPECT_EQ(count, 2U);
        EXPECT_NE(followers[0]->setting().time_map, nullptr);
        EXPECT_NE(followers[2]->setting().time_map, nullptr);
    }

    TEST(ReflowSelectorsTest, FramesAreShownInProportionToTheWeightsSomethingWrites)
    {
        const auto weights = std::make_shared<std::vector<double>>(std::vector<double> { 0.0, 2.0, -1.0, 1.0 });

        const auto picks = hook_pick(Kriya::weighted_frames<int>(hook_numbers(4), weights));

        ASSERT_EQ(picks.size(), 2U);
        EXPECT_EQ(*picks[0].frame, 10);
        EXPECT_DOUBLE_EQ(picks[0].weight, 2.0);
        EXPECT_EQ(picks[0].index, 1U);
        EXPECT_EQ(*picks[1].frame, 30);
    }

    TEST(ReflowSelectorsTest, ChangedWeightsChangeWhatIsShownAtTheNextCycle)
    {
        const auto weights = std::make_shared<std::vector<double>>(std::vector<double> { 1.0 });
        const auto selector = Kriya::weighted_frames<int>(hook_numbers(4), weights);

        EXPECT_EQ(hook_pick(selector).size(), 1U);
        *weights = { 0.0, 0.0, 3.0, 0.0, 9.0 };

        const auto picks = hook_pick(selector);
        ASSERT_EQ(picks.size(), 1U);
        EXPECT_EQ(*picks[0].frame, 20);
    }

    TEST(ReflowSelectorsTest, TheNearestFrameByFeatureIsShown)
    {
        const auto query = std::make_shared<std::vector<float>>(std::vector<float> { 18.0F });
        const auto selector = Kriya::nearest_by_feature<int>(
            hook_numbers(4), [](const int& frame) { return std::vector<float> { static_cast<float>(frame) }; }, query);

        const auto picks = hook_pick(selector);

        ASSERT_EQ(picks.size(), 1U);
        EXPECT_EQ(*picks[0].frame, 20);
        EXPECT_EQ(picks[0].index, 2U);
    }

    TEST(ReflowSelectorsTest, SeveralNearestFramesAreWeightedByTheirCloseness)
    {
        const auto query = std::make_shared<std::vector<float>>(std::vector<float> { 18.0F });
        const auto selector = Kriya::nearest_by_feature<int>(
            hook_numbers(4), [](const int& frame) { return std::vector<float> { static_cast<float>(frame) }; }, query, 2);

        const auto picks = hook_pick(selector);

        ASSERT_EQ(picks.size(), 2U);
        EXPECT_EQ(*picks[0].frame, 20);
        EXPECT_EQ(*picks[1].frame, 10);
        EXPECT_GT(picks[0].weight, picks[1].weight * 3.0);
    }

    TEST(ReflowSelectorsTest, AQueryOfAnotherLengthOrNoQueryPicksNothingAndFeaturesAreComputedOnce)
    {
        int described = 0;
        const auto query = std::make_shared<std::vector<float>>(std::vector<float> { 1.0F, 2.0F });
        const auto selector = Kriya::nearest_by_feature<int>(
            hook_numbers(4), [&described](const int& frame) {
                ++described;
                return std::vector<float> { static_cast<float>(frame) };
            },
            query);

        EXPECT_TRUE(hook_pick(selector).empty());
        *query = { 5.0F };
        EXPECT_EQ(hook_pick(selector).size(), 1U);
        query->clear();
        EXPECT_TRUE(hook_pick(selector).empty());
        EXPECT_EQ(described, 4);
    }

    TEST(ReflowSelectorsTest, ADampedKeyStartsOnItsTargetAndClosesOnItWithoutJumping)
    {
        const auto target = std::make_shared<double>(0.5);
        const auto selector = Kriya::damped_key<int>(hook_numbers(8), target, 3.0);

        const auto first = hook_pick(selector, 0.0);
        ASSERT_EQ(first.size(), 1U);
        EXPECT_EQ(*first[0].frame, 40);

        *target = 0.25;
        int previous = 40;
        int last = 40;
        for (int i = 1; i <= 200; ++i) {
            const auto picks = hook_pick(selector, i * 0.05);
            ASSERT_EQ(picks.size(), 1U);
            last = *picks[0].frame;
            EXPECT_LE(last, previous);
            previous = last;
        }
        EXPECT_EQ(last, 20);
    }

    TEST(ReflowSelectorsTest, ADampedKeyNeedsAResponseAboveZero)
    {
        const auto target = std::make_shared<double>(0.5);

        EXPECT_TRUE(hook_pick(Kriya::damped_key<int>(hook_numbers(8), target, 0.0)).empty());
    }

    TEST(ReflowSelectorsTest, AFlowingKeyIntegratesAVelocityThatMayDependOnWhereItIs)
    {
        const auto selector = Kriya::flowing_key<int>(
            hook_numbers(8), [](double, double) { return 0.5; }, 0.0);

        std::vector<int> shown;
        for (int i = 0; i < 4; ++i) {
            const auto picks = hook_pick(selector, i * 0.125);
            ASSERT_EQ(picks.size(), 1U);
            shown.push_back(*picks[0].frame);
        }

        EXPECT_EQ(shown, (std::vector<int> { 0, 10, 10, 20 }));
    }

    TEST(ReflowSelectorsTest, AFlowingKeyWhoseVelocityIsNotFiniteHoldsItsPlace)
    {
        const auto selector = Kriya::flowing_key<int>(
            hook_numbers(8), [](double, double) { return std::numeric_limits<double>::quiet_NaN(); }, 0.25);

        const auto first = hook_pick(selector, 0.0);
        const auto second = hook_pick(selector, 0.5);

        ASSERT_EQ(first.size(), 1U);
        ASSERT_EQ(second.size(), 1U);
        EXPECT_EQ(*first[0].frame, *second[0].frame);
    }

    TEST(ReflowSelectorsTest, ASelectorFromTheseRunsInALaneOfAReflow)
    {
        HookLog log;
        const auto weights = std::make_shared<std::vector<double>>(std::vector<double> { 0.0, 1.0 });
        const auto reflow = ReflowBuilder<int>(nullptr, log.sink(), k_hook_rate)
                                .route({ .selector = Kriya::weighted_frames<int>(hook_numbers(4), weights) })
                                .build();
        ASSERT_NE(reflow, nullptr);

        hook_run(*reflow, 2);

        EXPECT_EQ(log.values, (std::vector<int> { 10 }));
        EXPECT_EQ(reflow->skips(), 1U);
    }

    TEST(ReflowTimingTest, EachFrameIsHeldForItsOwnDuration)
    {
        const auto times = *TimeSampling::uniform(0.125);
        const auto map = Kriya::held_frames(times, 4, { 1.0, 2.0, 1.0, 1.0 });
        ASSERT_TRUE(static_cast<bool>(map.fn));

        EXPECT_DOUBLE_EQ(map(0.0), 0.0);
        EXPECT_DOUBLE_EQ(map(0.99), 0.0);
        EXPECT_DOUBLE_EQ(map(1.0), 0.125);
        EXPECT_DOUBLE_EQ(map(2.5), 0.125);
        EXPECT_DOUBLE_EQ(map(3.0), 0.25);
        EXPECT_DOUBLE_EQ(map(4.5), 0.375);
        EXPECT_DOUBLE_EQ(map(100.0), 0.375);
        EXPECT_DOUBLE_EQ(map(-1.0), 0.0);
    }

    TEST(ReflowTimingTest, DurationsThatDoNotFitTheSourceMakeNoMap)
    {
        const auto times = *TimeSampling::uniform(0.125);

        EXPECT_FALSE(static_cast<bool>(Kriya::held_frames(times, 4, { 1.0, 1.0 }).fn));
        EXPECT_FALSE(static_cast<bool>(Kriya::held_frames(times, 2, { 1.0, 0.0 }).fn));
        EXPECT_FALSE(static_cast<bool>(Kriya::held_frames(times, 2, { 1.0, std::numeric_limits<double>::quiet_NaN() }).fn));
        EXPECT_FALSE(static_cast<bool>(Kriya::held_frames(times, 0, {}).fn));
    }

    TEST(ReflowTimingTest, ACourseHoldsEachFrameOfAReflowForItsDuration)
    {
        HookLog log;
        const auto frames = hook_numbers(4);
        CourseSetting setting = hook_over(0.0, 0.5, CourseEdge::Hold);
        setting.time_map = std::make_shared<const Kinesis::TimeMap>(Kriya::held_frames(frames->time_sampling(), 4, { 0.25, 0.5, 0.25, 0.25 }));
        const auto reflow = ReflowBuilder<int>(frames, log.sink(), k_hook_rate)
                                .course(setting)
                                .rewrite(Kriya::Rewrite::OnChange)
                                .build();
        ASSERT_NE(reflow, nullptr);

        hook_run(*reflow, 8);

        EXPECT_EQ(log.values, (std::vector<int> { 0, 10, 20 }));
    }

    std::shared_ptr<FrameRing<int>> hook_ring()
    {
        auto ring = FrameRing<int>::create(8);
        for (int i = 0; i < 5; ++i) {
            ring->append(std::make_shared<const int>(i * 10), static_cast<double>(i));
        }
        return ring;
    }

    TEST(RingTrailTest, ATrailShowsTheFrameBehindTheNewestAndCutMovesItToAnotherMoment)
    {
        const auto ring = hook_ring();
        Kriya::RingTrail<int> trail(ring, Kinesis::constant<double, double>(1.0));
        const auto selector = trail.selector();

        EXPECT_EQ(*hook_pick(selector)[0].frame, 30);

        trail.cut(2.5);
        EXPECT_DOUBLE_EQ(trail.lag(), 2.5);
        EXPECT_EQ(*hook_pick(selector)[0].frame, 10);
    }

    TEST(RingTrailTest, AVaryingLagCarriesOnFromTheMomentItWasCutTo)
    {
        const auto ring = hook_ring();
        Kriya::RingTrail<int> trail(ring, Kinesis::TimeMap { .fn = [](const double& elapsed) { return elapsed; } });
        const auto selector = trail.selector();

        EXPECT_EQ(*hook_pick(selector, 0.0, 1.0)[0].frame, 30);
        trail.cut(0.0);

        EXPECT_EQ(*hook_pick(selector, 0.0, 1.0)[0].frame, 40);
        EXPECT_EQ(*hook_pick(selector, 0.0, 2.0)[0].frame, 30);
    }

    TEST(RingTrailTest, ACutToSomethingThatIsNotFiniteIsIgnoredAndASharedTrailIsOne)
    {
        const auto ring = hook_ring();
        Kriya::RingTrail<int> trail(ring, Kinesis::constant<double, double>(1.0));
        auto copy = trail;

        trail.cut(std::numeric_limits<double>::quiet_NaN());
        EXPECT_DOUBLE_EQ(trail.lag(), 1.0);

        copy.cut(3.0);
        EXPECT_DOUBLE_EQ(trail.lag(), 3.0);
    }

    TEST(RingTrailTest, ATrailBehindNothingShowsNothing)
    {
        Kriya::RingTrail<int> trail(nullptr, Kinesis::constant<double, double>(1.0));

        EXPECT_TRUE(hook_pick(trail.selector()).empty());
    }

} // namespace
} // namespace MayaFlux::Test
