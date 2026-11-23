#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kinesis/Tendency/TendencyFactories.hpp"
#include "MayaFlux/Kriya/Reflow.hpp"
#include "MayaFlux/Kriya/ReflowHelpers/ReflowLive.hpp"
#include "MayaFlux/Transitive/Facet/ControlBind.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Kakshya::FrameRing;
    using Kakshya::ResidentFrames;
    using Kakshya::SampledFrames;
    using Kinesis::TimeSampling;
    using Kriya::CourseEdge;
    using Kriya::CourseRoute;
    using Kriya::CourseSetting;
    using Kriya::FrameContext;
    using Kriya::FrameOperation;
    using Kriya::FrameSelection;
    using Kriya::FrameSelector;
    using Kriya::Reflow;
    using Kriya::ReflowBuilder;
    using Kriya::ReflowControls;
    using Kriya::ReflowEventKind;
    using Kriya::ReflowEvents;
    using Kriya::Rewrite;
    using Kriya::WeightedFrame;

    /** Eight cycles a second, so every advance is an exact eighth of a second. */
    constexpr double k_pick_rate = 8.0;

    /** Frame i holds the value 10 * i, one every eighth of a second. */
    std::shared_ptr<const ResidentFrames<int>> pick_numbers(size_t count)
    {
        std::vector<int> frames;
        for (size_t i = 0; i < count; ++i) {
            frames.push_back(static_cast<int>(i) * 10);
        }
        auto sampling = TimeSampling::uniform(0.125);
        if (!sampling) {
            throw std::runtime_error("sampling was rejected");
        }
        return ResidentFrames<int>::create(*sampling, std::move(frames));
    }

    CourseSetting pick_over(double in, double out, CourseEdge edge = CourseEdge::Once, double speed = 1.0)
    {
        CourseSetting setting;
        setting.in = in;
        setting.out = out;
        setting.edge = edge;
        setting.speed = speed;
        return setting;
    }

    struct PickLog {
        std::vector<int> values;

        Reflow<int>::Sink sink()
        {
            return [this](const std::shared_ptr<const int>& frame, const FrameContext&) { values.push_back(*frame); };
        }
    };

    Reflow<int>::Sink pick_discard()
    {
        return [](const std::shared_ptr<const int>&, const FrameContext&) { };
    }

    void pick_run(Reflow<int>& reflow, int cycles)
    {
        for (int i = 0; i < cycles; ++i) {
            reflow.advance();
        }
    }

    FrameSelector<int> pick_value(int value, double weight = 1.0)
    {
        return [value, weight](const FrameContext&, std::vector<WeightedFrame<int>>& into) {
            into.push_back({ .frame = std::make_shared<const int>(value), .weight = weight, .index = 0 });
        };
    }

    std::vector<Kriya::ReflowEvent> pick_drain(const std::shared_ptr<ReflowEvents>& source)
    {
        std::vector<Kriya::ReflowEvent> events;
        for (;;) {
            auto awaiter = source->next();
            if (!awaiter.await_ready()) {
                break;
            }
            events.push_back(awaiter.await_resume());
        }
        return events;
    }

    class PickRamp final : public SampledFrames<int> {
    public:
        [[nodiscard]] std::pair<double, double> extent() const override { return { 0.0, 1.0 }; }

        [[nodiscard]] std::shared_ptr<const int> sample(double time) const override
        {
            return std::make_shared<const int>(static_cast<int>(std::lround(time * 1000.0)));
        }
    };

    TEST(ReflowSelectorTest, ASelectorLaneReadsNoSourceAndTheReflowNeedsNone)
    {
        PickLog log;
        const auto reflow = ReflowBuilder<int>(nullptr, log.sink(), k_pick_rate)
                                .route({ .selector = pick_value(7) })
                                .build();
        ASSERT_NE(reflow, nullptr);
        EXPECT_EQ(reflow->source(), nullptr);
        EXPECT_EQ(reflow->sampled(), nullptr);

        pick_run(*reflow, 3);

        EXPECT_EQ(log.values, (std::vector<int> { 7, 7, 7 }));
    }

    TEST(ReflowSelectorTest, ASelectorReadsWhateverItClosedOverAtEveryCycle)
    {
        PickLog log;
        const auto cell = std::make_shared<int>(1);
        const auto reflow = ReflowBuilder<int>(nullptr, log.sink(), k_pick_rate)
                                .route({ .selector = [cell](const FrameContext&, std::vector<WeightedFrame<int>>& into) {
                                    into.push_back({ .frame = std::make_shared<const int>(*cell), .weight = 1.0, .index = 0 });
                                } })
                                .build();
        ASSERT_NE(reflow, nullptr);

        pick_run(*reflow, 2);
        *cell = 9;
        pick_run(*reflow, 2);

        EXPECT_EQ(log.values, (std::vector<int> { 1, 1, 9, 9 }));
    }

    TEST(ReflowSelectorTest, ASelectorIsGivenTheReflowsTimeAndItsLanesOwn)
    {
        PickLog log;
        std::vector<double> times;
        std::vector<double> elapsed;
        CourseRoute<int> route;
        route.enters_after = 0.25;
        route.selector = [&](const FrameContext& context, std::vector<WeightedFrame<int>>& into) {
            times.push_back(context.time);
            elapsed.push_back(context.elapsed);
            EXPECT_NE(context.course, nullptr);
            into.push_back({ .frame = std::make_shared<const int>(1), .weight = 1.0, .index = 0 });
        };
        const auto reflow = ReflowBuilder<int>(nullptr, log.sink(), k_pick_rate).route(route).build();
        ASSERT_NE(reflow, nullptr);

        pick_run(*reflow, 5);

        EXPECT_EQ(times, (std::vector<double> { 0.25, 0.375, 0.5 }));
        EXPECT_EQ(elapsed, (std::vector<double> { 0.0, 0.125, 0.25 }));
    }

    TEST(ReflowSelectorTest, ASelectorLaneStillStepsItsCourse)
    {
        PickLog log;
        const auto reflow = ReflowBuilder<int>(nullptr, log.sink(), k_pick_rate)
                                .route({ .selector = pick_value(1) })
                                .build();
        ASSERT_NE(reflow, nullptr);

        pick_run(*reflow, 4);

        const auto course = reflow->courses().front();
        EXPECT_EQ(course->steps(), 3U);
        EXPECT_TRUE(course->frame().has_value());
        EXPECT_FALSE(course->finished());
    }

    TEST(ReflowSelectorTest, ACourseWeightScalesPicksAndTheHeaviestWinsWithoutAMix)
    {
        PickLog log;
        const auto reflow = ReflowBuilder<int>(nullptr, log.sink(), k_pick_rate)
                                .route({ .selector = pick_value(1) })
                                .route({ .selector = pick_value(2) })
                                .build();
        ASSERT_NE(reflow, nullptr);
        const auto courses = reflow->courses();
        ASSERT_EQ(courses.size(), 2U);

        pick_run(*reflow, 1);
        ASSERT_TRUE(courses[1]->weight(3.0));
        pick_run(*reflow, 1);
        ASSERT_TRUE(courses[1]->weight(0.0));
        pick_run(*reflow, 1);

        EXPECT_EQ(log.values.size(), 3U);
        EXPECT_EQ(log.values[1], 2);
        EXPECT_EQ(log.values[2], 1);
    }

    TEST(ReflowSelectorTest, ASelectorWhoseLaneHasNoWeightIsNotCalled)
    {
        PickLog log;
        int calls = 0;
        const auto reflow = ReflowBuilder<int>(nullptr, log.sink(), k_pick_rate)
                                .route({ .selector = [&calls](const FrameContext&, std::vector<WeightedFrame<int>>& into) {
                                    ++calls;
                                    into.push_back({ .frame = std::make_shared<const int>(1), .weight = 1.0, .index = 0 });
                                } })
                                .build();
        ASSERT_NE(reflow, nullptr);
        ASSERT_TRUE(reflow->courses().front()->weight(0.0));

        pick_run(*reflow, 3);

        EXPECT_EQ(calls, 0);
        EXPECT_TRUE(log.values.empty());
    }

    TEST(ReflowSelectorTest, ASelectorThatPicksNothingCountsAMissAndTheReflowHolds)
    {
        PickLog log;
        const auto reflow = ReflowBuilder<int>(nullptr, log.sink(), k_pick_rate)
                                .route({ .selector = [](const FrameContext&, std::vector<WeightedFrame<int>>&) { } })
                                .build();
        ASSERT_NE(reflow, nullptr);

        pick_run(*reflow, 3);

        EXPECT_EQ(reflow->courses().front()->misses(), 3U);
        EXPECT_EQ(reflow->holds(), 3U);
        EXPECT_TRUE(log.values.empty());
    }

    TEST(ReflowSelectorTest, PicksWithNoFrameOrNoWeightAreDropped)
    {
        PickLog log;
        const auto reflow = ReflowBuilder<int>(nullptr, log.sink(), k_pick_rate)
                                .route({ .selector = [](const FrameContext&, std::vector<WeightedFrame<int>>& into) {
                                    into.push_back({ .frame = nullptr, .weight = 1.0, .index = 0 });
                                    into.push_back({ .frame = std::make_shared<const int>(4), .weight = 0.0, .index = 0 });
                                    into.push_back({ .frame = std::make_shared<const int>(5), .weight = 1.0, .index = 0 });
                                } })
                                .build();
        ASSERT_NE(reflow, nullptr);

        pick_run(*reflow, 1);

        EXPECT_EQ(log.values, (std::vector<int> { 5 }));
    }

    TEST(ReflowSelectorTest, SeveralPicksAreBlendedByTheMix)
    {
        PickLog log;
        const auto reflow = ReflowBuilder<int>(nullptr, log.sink(), k_pick_rate)
                                .route({ .selector = [](const FrameContext&, std::vector<WeightedFrame<int>>& into) {
                                    into.push_back({ .frame = std::make_shared<const int>(10), .weight = 0.5, .index = 0 });
                                    into.push_back({ .frame = std::make_shared<const int>(30), .weight = 0.5, .index = 1 });
                                } })
                                .operation(FrameOperation<int>::mix([](const FrameContext&, std::span<const WeightedFrame<int>> picks) {
                                    double sum = 0.0;
                                    double total = 0.0;
                                    for (const auto& pick : picks) {
                                        sum += pick.weight * *pick.frame;
                                        total += pick.weight;
                                    }
                                    return std::make_shared<const int>(static_cast<int>(std::lround(sum / total)));
                                }))
                                .build();
        ASSERT_NE(reflow, nullptr);

        pick_run(*reflow, 1);

        EXPECT_EQ(log.values, (std::vector<int> { 20 }));
    }

    TEST(ReflowSelectorTest, ASelectorLaneMayHaveOperationsAndASinkOfItsOwn)
    {
        PickLog shared;
        PickLog own;
        CourseRoute<int> route;
        route.selector = pick_value(3);
        route.operations.push_back(FrameOperation<int>::transform(
            [](const FrameContext&, std::shared_ptr<const int> frame) { return std::make_shared<const int>(*frame * 10); }));
        route.sink = own.sink();
        const auto reflow = ReflowBuilder<int>(nullptr, shared.sink(), k_pick_rate).route(route).build();
        ASSERT_NE(reflow, nullptr);

        pick_run(*reflow, 2);

        EXPECT_EQ(own.values, (std::vector<int> { 30, 30 }));
        EXPECT_TRUE(shared.values.empty());
    }

    TEST(ReflowSelectorTest, ASelectorExcludesASourceOnTheSameRoute)
    {
        PickLog log;
        const auto reflow = ReflowBuilder<int>(pick_numbers(4), log.sink(), k_pick_rate).build();
        ASSERT_NE(reflow, nullptr);

        CourseRoute<int> with_source;
        with_source.source = pick_numbers(2);
        with_source.selector = pick_value(1);
        EXPECT_EQ(reflow->add_route(with_source), nullptr);

        CourseRoute<int> with_sampled;
        with_sampled.sampled = std::make_shared<PickRamp>();
        with_sampled.selector = pick_value(1);
        EXPECT_EQ(reflow->add_route(with_sampled), nullptr);

        CourseRoute<int> alone;
        alone.selector = pick_value(1);
        EXPECT_NE(reflow->add_route(alone), nullptr);
    }

    TEST(ReflowSelectorTest, ASelectorLaneNeedsASinkFromTheReflowOrItself)
    {
        EXPECT_EQ(ReflowBuilder<int>(nullptr, {}, k_pick_rate).route({ .selector = pick_value(1) }).build(), nullptr);

        PickLog own;
        CourseRoute<int> route;
        route.selector = pick_value(1);
        route.sink = own.sink();
        EXPECT_NE(ReflowBuilder<int>(nullptr, {}, k_pick_rate).route(route).build(), nullptr);
    }

    TEST(ReflowSelectorTest, ASelectorLaneGivenAUsableCourseKeepsIt)
    {
        PickLog log;
        CourseRoute<int> route;
        route.course = pick_over(2.0, 4.0, CourseEdge::Hold, 0.0);
        route.selector = [](const FrameContext& context, std::vector<WeightedFrame<int>>& into) {
            into.push_back({ .frame = std::make_shared<const int>(static_cast<int>(context.course->time())), .weight = 1.0, .index = 0 });
        };
        const auto reflow = ReflowBuilder<int>(nullptr, log.sink(), k_pick_rate).route(route).build();
        ASSERT_NE(reflow, nullptr);

        pick_run(*reflow, 2);

        EXPECT_EQ(log.values, (std::vector<int> { 2, 2 }));
    }

    TEST(ReflowControlsTest, DisablingStopsEveryCycleAndEnablingResumesThem)
    {
        PickLog log;
        const auto reflow = ReflowBuilder<int>(pick_numbers(8), log.sink(), k_pick_rate).build();
        ASSERT_NE(reflow, nullptr);

        ReflowControls off;
        off.enabled = ControlValue(false);
        reflow->set(off);
        pick_run(*reflow, 3);
        EXPECT_EQ(reflow->cycle(), 0U);
        EXPECT_TRUE(log.values.empty());

        ReflowControls on;
        on.enabled = ControlValue(true);
        reflow->set(on);
        pick_run(*reflow, 2);
        EXPECT_EQ(reflow->cycle(), 2U);
        EXPECT_EQ(log.values.size(), 2U);
    }

    TEST(ReflowControlsTest, TheSelectionControlReplacesWhatTheBuilderSaid)
    {
        PickLog floor;
        const auto by_floor = ReflowBuilder<int>(pick_numbers(8), floor.sink(), k_pick_rate)
                                  .course(pick_over(0.0, 1.0, CourseEdge::Once, 0.5))
                                  .rewrite(Rewrite::Always)
                                  .build();
        ASSERT_NE(by_floor, nullptr);
        pick_run(*by_floor, 2);
        EXPECT_EQ(floor.values, (std::vector<int> { 0, 0 }));

        PickLog nearest;
        const auto by_control = ReflowBuilder<int>(pick_numbers(8), nearest.sink(), k_pick_rate)
                                    .course(pick_over(0.0, 1.0, CourseEdge::Once, 0.5))
                                    .rewrite(Rewrite::Always)
                                    .build();
        ASSERT_NE(by_control, nullptr);
        ReflowControls incoming;
        incoming.selection = ControlValue(FrameSelection::Nearest);
        by_control->set(incoming);
        pick_run(*by_control, 2);
        EXPECT_EQ(nearest.values, (std::vector<int> { 0, 10 }));
    }

    TEST(ReflowControlsTest, TheRewriteControlReplacesWhatTheBuilderSaid)
    {
        PickLog held;
        const auto reflow = ReflowBuilder<int>(pick_numbers(8), held.sink(), k_pick_rate)
                                .course(pick_over(0.0, 1.0, CourseEdge::Hold, 0.0))
                                .build();
        ASSERT_NE(reflow, nullptr);

        pick_run(*reflow, 3);
        EXPECT_EQ(held.values.size(), 1U);

        ReflowControls incoming;
        incoming.rewrite = ControlValue(Rewrite::Always);
        reflow->set(incoming);
        pick_run(*reflow, 3);
        EXPECT_EQ(held.values.size(), 4U);
    }

    TEST(ReflowControlsTest, GetReturnsWhatWasSetAndLeavesTheRestEmpty)
    {
        PickLog log;
        const auto reflow = ReflowBuilder<int>(pick_numbers(4), log.sink(), k_pick_rate).build();
        ASSERT_NE(reflow, nullptr);

        ReflowControls incoming;
        incoming.selection = ControlValue(FrameSelection::Blend);
        reflow->set(incoming);

        const auto held = reflow->get();
        ASSERT_TRUE(held.selection.has_value());
        EXPECT_EQ(held.selection.value<FrameSelection>(), FrameSelection::Blend);
        EXPECT_FALSE(held.rewrite.has_value());
        EXPECT_FALSE(held.enabled.has_value());
    }

    TEST(ReflowControlsTest, AControlWriterDrivesAReflowTheWayItDrivesACourse)
    {
        PickLog log;
        const auto reflow = ReflowBuilder<int>(pick_numbers(8), log.sink(), k_pick_rate).build();
        ASSERT_NE(reflow, nullptr);

        const auto enable = control_writer<&ReflowControls::enabled, bool>(reflow);
        enable(false);
        pick_run(*reflow, 2);
        EXPECT_EQ(reflow->cycle(), 0U);

        enable(true);
        pick_run(*reflow, 2);
        EXPECT_EQ(reflow->cycle(), 2U);
    }

    TEST(ReflowEventsTest, EveryWriteIsAnnouncedWithTheCountSoFar)
    {
        PickLog log;
        const auto reflow = ReflowBuilder<int>(nullptr, log.sink(), k_pick_rate)
                                .route({ .selector = pick_value(1) })
                                .build();
        ASSERT_NE(reflow, nullptr);
        const auto written = reflow->subscribe(ReflowEventKind::Written);

        pick_run(*reflow, 3);

        const auto told = pick_drain(written);
        ASSERT_EQ(told.size(), 1U);
        EXPECT_EQ(told[0].kind, ReflowEventKind::Written);
        EXPECT_EQ(told[0].value, 3U);
        EXPECT_EQ(told[0].cycle, 2U);
    }

    TEST(ReflowEventsTest, AHeldCycleIsAnnouncedAndAWriteIsNotAHold)
    {
        PickLog log;
        const auto reflow = ReflowBuilder<int>(nullptr, log.sink(), k_pick_rate)
                                .route({ .selector = [](const FrameContext&, std::vector<WeightedFrame<int>>&) { } })
                                .build();
        ASSERT_NE(reflow, nullptr);
        const auto held = reflow->subscribe(ReflowEventKind::Held);
        const auto written = reflow->subscribe(ReflowEventKind::Written);

        pick_run(*reflow, 2);

        const auto told = pick_drain(held);
        ASSERT_EQ(told.size(), 1U);
        EXPECT_EQ(told[0].value, 2U);
        EXPECT_TRUE(pick_drain(written).empty());
    }

    TEST(ReflowEventsTest, AThrowingSinkIsAnnouncedAsAFailure)
    {
        const auto reflow = ReflowBuilder<int>(
            nullptr, [](const std::shared_ptr<const int>&, const FrameContext&) { throw std::runtime_error("sink"); }, k_pick_rate)
                                .route({ .selector = pick_value(1) })
                                .build();
        ASSERT_NE(reflow, nullptr);
        const auto failed = reflow->subscribe(ReflowEventKind::Failed);

        pick_run(*reflow, 1);

        const auto told = pick_drain(failed);
        ASSERT_EQ(told.size(), 1U);
        EXPECT_EQ(told[0].value, 1U);
        EXPECT_EQ(reflow->failures(), 1U);
    }

    TEST(ReflowEventsTest, FinishedIsAnnouncedOnceWhenEveryCourseHasFinished)
    {
        PickLog log;
        const auto reflow = ReflowBuilder<int>::sampling(std::make_shared<PickRamp>(), log.sink(), k_pick_rate).build();
        ASSERT_NE(reflow, nullptr);
        const auto finished = reflow->subscribe(ReflowEventKind::Finished);

        pick_run(*reflow, 6);
        EXPECT_TRUE(pick_drain(finished).empty());

        pick_run(*reflow, 12);
        const auto told = pick_drain(finished);
        ASSERT_EQ(told.size(), 1U);
        EXPECT_EQ(told[0].kind, ReflowEventKind::Finished);

        pick_run(*reflow, 4);
        EXPECT_TRUE(pick_drain(finished).empty());
    }

    TEST(ReflowEventsTest, FinishedWaitsForEveryCourse)
    {
        PickLog log;
        const auto reflow = ReflowBuilder<int>::sampling(std::make_shared<PickRamp>(), log.sink(), k_pick_rate)
                                .course(pick_over(0.0, 1.0, CourseEdge::Once))
                                .course(pick_over(0.0, 1.0, CourseEdge::Hold))
                                .build();
        ASSERT_NE(reflow, nullptr);
        const auto finished = reflow->subscribe(ReflowEventKind::Finished);

        pick_run(*reflow, 20);

        EXPECT_TRUE(reflow->courses().front()->finished());
        EXPECT_TRUE(pick_drain(finished).empty());
    }

    TEST(ReflowEventsTest, EverySubscriberOfAKindHearsIt)
    {
        PickLog log;
        const auto reflow = ReflowBuilder<int>(nullptr, log.sink(), k_pick_rate)
                                .route({ .selector = pick_value(1) })
                                .build();
        ASSERT_NE(reflow, nullptr);
        const auto first = reflow->subscribe(ReflowEventKind::Written);
        const auto second = reflow->subscribe(ReflowEventKind::Written);

        pick_run(*reflow, 1);

        EXPECT_EQ(pick_drain(first).size(), 1U);
        EXPECT_EQ(pick_drain(second).size(), 1U);
    }

    TEST(ReflowLaneViewTest, ALaneWithItsOwnSinkShowsWhatItLastWrote)
    {
        PickLog shared;
        PickLog own;
        CourseRoute<int> mine;
        mine.selector = pick_value(5);
        mine.sink = own.sink();
        CourseRoute<int> ours;
        ours.selector = pick_value(6);
        const auto reflow = ReflowBuilder<int>(nullptr, shared.sink(), k_pick_rate).route(mine).route(ours).build();
        ASSERT_NE(reflow, nullptr);
        const auto courses = reflow->courses();
        ASSERT_EQ(courses.size(), 2U);

        EXPECT_EQ(reflow->current(courses[0]), nullptr);

        pick_run(*reflow, 2);

        ASSERT_NE(reflow->current(courses[0]), nullptr);
        EXPECT_EQ(*reflow->current(courses[0]), 5);
        EXPECT_EQ(reflow->current(courses[1]), nullptr);
        ASSERT_NE(reflow->current(), nullptr);
        EXPECT_EQ(*reflow->current(), 6);
    }

    TEST(ReflowLaneViewTest, ACourseOfAnotherReflowShowsNothing)
    {
        PickLog log;
        const auto reflow = ReflowBuilder<int>(nullptr, log.sink(), k_pick_rate)
                                .route({ .selector = pick_value(1) })
                                .build();
        const auto other = ReflowBuilder<int>(nullptr, log.sink(), k_pick_rate)
                               .route({ .selector = pick_value(1) })
                               .build();
        ASSERT_NE(reflow, nullptr);
        ASSERT_NE(other, nullptr);

        pick_run(*reflow, 1);

        EXPECT_EQ(reflow->current(other->courses().front()), nullptr);
        EXPECT_EQ(reflow->current(nullptr), nullptr);
    }

    TEST(ReflowLiveTest, RecordingAppendsEveryFrameTheReflowShowsAtTheReflowsTime)
    {
        const auto ring = FrameRing<int>::create(16);
        ASSERT_NE(ring, nullptr);
        PickLog log;
        const auto reflow = ReflowBuilder<int>(pick_numbers(8), log.sink(), k_pick_rate)
                                .operation(Kriya::recording_frames(ring))
                                .build();
        ASSERT_NE(reflow, nullptr);

        pick_run(*reflow, 4);

        EXPECT_EQ(ring->head(), 4U);
        EXPECT_EQ(*ring->read(2), 20);
        EXPECT_DOUBLE_EQ(ring->extent().first, 0.0);
        EXPECT_DOUBLE_EQ(ring->extent().second, 0.375);
    }

    TEST(ReflowLiveTest, AnUnchangedFrameIsStillRecordedSoTheRingKeepsTime)
    {
        const auto ring = FrameRing<int>::create(16);
        ASSERT_NE(ring, nullptr);
        PickLog log;
        const auto reflow = ReflowBuilder<int>(pick_numbers(8), log.sink(), k_pick_rate)
                                .course(pick_over(0.0, 1.0, CourseEdge::Hold, 0.0))
                                .operation(Kriya::recording_frames(ring))
                                .build();
        ASSERT_NE(reflow, nullptr);

        pick_run(*reflow, 4);

        EXPECT_EQ(log.values.size(), 1U);
        EXPECT_EQ(ring->head(), 4U);
    }

    TEST(ReflowLiveTest, TrailingPicksTheFrameSomeSecondsBehindTheNewest)
    {
        const auto ring = FrameRing<int>::create(8);
        ASSERT_NE(ring, nullptr);
        for (int i = 0; i < 5; ++i) {
            ASSERT_TRUE(ring->append(std::make_shared<const int>(i * 10), static_cast<double>(i)));
        }

        const auto pick = [&](Kinesis::TimeMap lag, double elapsed) {
            std::vector<WeightedFrame<int>> into;
            FrameContext context;
            context.elapsed = elapsed;
            Kriya::trailing_frames(ring, std::move(lag))(context, into);
            return into;
        };

        const auto steady = pick(Kinesis::constant<double, double>(1.5), 0.0);
        ASSERT_EQ(steady.size(), 1U);
        EXPECT_EQ(*steady[0].frame, 20);
        EXPECT_EQ(steady[0].index, 2U);

        const auto live = pick(Kinesis::constant<double, double>(0.0), 0.0);
        ASSERT_EQ(live.size(), 1U);
        EXPECT_EQ(*live[0].frame, 40);

        const auto negative = pick(Kinesis::constant<double, double>(-3.0), 0.0);
        ASSERT_EQ(negative.size(), 1U);
        EXPECT_EQ(*negative[0].frame, 40);

        const auto growing = pick(Kinesis::TimeMap { .fn = [](const double& elapsed) { return elapsed; } }, 2.0);
        ASSERT_EQ(growing.size(), 1U);
        EXPECT_EQ(*growing[0].frame, 20);

        EXPECT_TRUE(pick(Kinesis::constant<double, double>(10.0), 0.0).empty());
        EXPECT_TRUE(pick(Kinesis::TimeMap {}, 0.0).empty());
    }

    TEST(ReflowLiveTest, TrailingAnEmptyRingPicksNothing)
    {
        const auto ring = FrameRing<int>::create(4);
        ASSERT_NE(ring, nullptr);
        std::vector<WeightedFrame<int>> into;

        Kriya::trailing_frames(ring, Kinesis::constant<double, double>(0.0))(FrameContext {}, into);

        EXPECT_TRUE(into.empty());
    }

    TEST(ReflowLiveTest, OneLaneRecordsAndAnotherTrailsItAsAnEcho)
    {
        const auto ring = FrameRing<int>::create(16);
        ASSERT_NE(ring, nullptr);

        PickLog echo;
        CourseRoute<int> source;
        source.course = pick_over(0.0, 1.0, CourseEdge::Once);
        source.source = pick_numbers(8);
        source.operations.push_back(Kriya::recording_frames(ring));
        source.sink = pick_discard();
        CourseRoute<int> trail;
        trail.selector = Kriya::trailing_frames(ring, Kinesis::constant<double, double>(0.25));

        const auto reflow = ReflowBuilder<int>(nullptr, echo.sink(), k_pick_rate).route(source).route(trail).build();
        ASSERT_NE(reflow, nullptr);

        pick_run(*reflow, 6);

        EXPECT_EQ(reflow->holds(), 2U);
        EXPECT_EQ(echo.values, (std::vector<int> { 0, 10, 20, 30 }));
    }

    TEST(ReflowLiveTest, TheLagFollowsACellWrittenWhileTheLaneRuns)
    {
        const auto ring = FrameRing<int>::create(16);
        ASSERT_NE(ring, nullptr);
        for (int i = 0; i < 8; ++i) {
            ASSERT_TRUE(ring->append(std::make_shared<const int>(i * 10), static_cast<double>(i)));
        }

        const auto lag = std::make_shared<double>(1.0);
        PickLog log;
        const auto reflow = ReflowBuilder<int>(nullptr, log.sink(), k_pick_rate)
                                .route({ .selector = Kriya::trailing_frames(ring, Kinesis::TimeMap { .fn = [lag](const double&) { return *lag; } }) })
                                .build();
        ASSERT_NE(reflow, nullptr);

        pick_run(*reflow, 1);
        *lag = 3.0;
        pick_run(*reflow, 1);

        EXPECT_EQ(log.values, (std::vector<int> { 60, 40 }));
    }

} // namespace
} // namespace MayaFlux::Test
