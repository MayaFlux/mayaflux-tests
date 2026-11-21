#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "../support/frame_scheduler.hpp"

#include "MayaFlux/Kriya/Reflow.hpp"
#include "MayaFlux/Kriya/ReflowHelpers/ReflowSelect.hpp"
#include "MayaFlux/Vruta/ChronUtils.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Kakshya::FrameSource;
    using Kakshya::ResidentFrames;
    using Kinesis::TimeLocation;
    using Kinesis::TimePlacement;
    using Kinesis::TimeSampling;
    using Kriya::CourseEdge;
    using Kriya::CourseEventKind;
    using Kriya::CourseSetting;
    using Kriya::FrameContext;
    using Kriya::FrameOperation;
    using Kriya::FrameSelection;
    using Kriya::Reflow;
    using Kriya::ReflowBuilder;
    using Kriya::Rewrite;
    using Kriya::WeightedFrame;

    /** Eight cycles a second, so every advance is an exact eighth of a second. */
    constexpr double k_rate = 8.0;

    TimeSampling every(double step)
    {
        auto sampling = TimeSampling::uniform(step);
        if (!sampling) {
            throw std::runtime_error("sampling was rejected");
        }
        return *sampling;
    }

    /** Frame i holds the value 10 * i. */
    std::shared_ptr<const ResidentFrames<int>> numbers(size_t count, double step = 0.125)
    {
        std::vector<int> frames;
        frames.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            frames.push_back(static_cast<int>(i) * 10);
        }
        return ResidentFrames<int>::create(every(step), std::move(frames));
    }

    CourseSetting over(double in, double out, CourseEdge edge = CourseEdge::Once, double rate = 1.0)
    {
        CourseSetting setting;
        setting.in = in;
        setting.out = out;
        setting.edge = edge;
        setting.rate = rate;
        return setting;
    }

    class Empty final : public FrameSource<int> {
    public:
        Empty()
            : m_times(every(1.0))
        {
        }

        [[nodiscard]] size_t count() const override { return 0; }
        [[nodiscard]] const TimeSampling& time_sampling() const override { return m_times; }
        [[nodiscard]] std::shared_ptr<const int> read(size_t) const override { return nullptr; }

    private:
        TimeSampling m_times;
    };

    /** A source that can be told to fail on some indices. */
    class Flaky final : public FrameSource<int> {
    public:
        explicit Flaky(std::shared_ptr<const ResidentFrames<int>> inner)
            : m_inner(std::move(inner))
        {
        }

        [[nodiscard]] size_t count() const override { return m_inner->count(); }
        [[nodiscard]] const TimeSampling& time_sampling() const override { return m_inner->time_sampling(); }

        [[nodiscard]] std::shared_ptr<const int> read(size_t index) const override
        {
            if (index == m_fail.load()) {
                return nullptr;
            }
            if (index == m_throw.load()) {
                throw std::runtime_error("unreadable");
            }
            return m_inner->read(index);
        }

        mutable std::atomic<size_t> m_fail { std::numeric_limits<size_t>::max() };
        mutable std::atomic<size_t> m_throw { std::numeric_limits<size_t>::max() };

    private:
        std::shared_ptr<const ResidentFrames<int>> m_inner;
    };

    struct Written {
        std::vector<int> values;
        std::vector<uint64_t> cycles;

        Reflow<int>::Sink sink()
        {
            return [this](const std::shared_ptr<const int>& frame, const FrameContext& context) {
                values.push_back(*frame);
                cycles.push_back(context.cycle);
            };
        }
    };

    ReflowBuilder<int> builder(const std::shared_ptr<const FrameSource<int>>& source, Written& written)
    {
        return ReflowBuilder<int>(source, written.sink(), k_rate);
    }

    void run(Reflow<int>& reflow, int cycles)
    {
        for (int i = 0; i < cycles; ++i) {
            reflow.advance();
        }
    }

    TEST(ReflowTest, WhatCannotFormAReflowMakesNone)
    {
        Written written;
        const auto frames = numbers(4);

        EXPECT_EQ(ReflowBuilder<int>(nullptr, written.sink(), k_rate).build(), nullptr);
        EXPECT_EQ(ReflowBuilder<int>(frames, nullptr, k_rate).build(), nullptr);
        EXPECT_EQ(builder(std::make_shared<Empty>(), written).build(), nullptr);

        EXPECT_EQ(builder(frames, written).operation(FrameOperation<int>::transform({})).build(), nullptr);

        const auto mix = [](const FrameContext&, std::span<const WeightedFrame<int>>) {
            return std::make_shared<const int>(0);
        };
        EXPECT_EQ(builder(frames, written)
                      .operation(FrameOperation<int>::mix(mix))
                      .operation(FrameOperation<int>::mix(mix))
                      .build(),
            nullptr);
        EXPECT_NE(builder(frames, written).operation(FrameOperation<int>::mix(mix)).build(), nullptr);

        EXPECT_EQ(builder(frames, written).course(over(1.0, 1.0)).build(), nullptr);
    }

    TEST(ReflowTest, TheDefaultCoursePlaysTheWholeSourceOnceFromTheStartAndHolds)
    {
        Written written;
        const auto reflow = builder(numbers(5), written).build();
        ASSERT_NE(reflow, nullptr);
        ASSERT_EQ(reflow->courses().size(), 1U);

        run(*reflow, 5);
        EXPECT_EQ(written.values, (std::vector<int> { 0, 10, 20, 30, 40 }));
        EXPECT_EQ(written.cycles, (std::vector<uint64_t> { 0, 1, 2, 3, 4 }));
        EXPECT_TRUE(reflow->courses().front()->finished());

        run(*reflow, 3);
        EXPECT_EQ(written.values.size(), 5U);
        EXPECT_EQ(reflow->writes(), 5U);
        EXPECT_EQ(reflow->skips(), 3U);
        EXPECT_EQ(reflow->cycle(), 8U);
    }

    TEST(ReflowTest, ASingleFrameSourceIsWrittenOnceAndHeld)
    {
        Written written;
        const auto reflow = builder(numbers(1), written).build();
        ASSERT_NE(reflow, nullptr);

        run(*reflow, 20);

        EXPECT_EQ(written.values, (std::vector<int> { 0 }));
        EXPECT_FALSE(reflow->courses().front()->finished());
    }

    TEST(ReflowTest, CurrentIsTheSharedFrameLastWrittenAndNotACopy)
    {
        Written written;
        const auto source = numbers(3);
        const auto reflow = builder(source, written).build();
        ASSERT_NE(reflow, nullptr);
        EXPECT_EQ(reflow->current(), nullptr);

        run(*reflow, 2);

        EXPECT_EQ(reflow->current().get(), source->read(1).get());
    }

    TEST(ReflowTest, FloorNearestAndBlendPickDifferentFramesBetweenTwo)
    {
        const auto source = numbers(4, 0.25);

        {
            Written written;
            auto reflow = builder(source, written).selection(FrameSelection::Floor).build();
            run(*reflow, 3);
            EXPECT_EQ(written.values, (std::vector<int> { 0, 10 }));
        }
        {
            Written written;
            auto reflow = builder(source, written).selection(FrameSelection::Nearest).build();
            run(*reflow, 3);
            EXPECT_EQ(written.values, (std::vector<int> { 0, 10 }));
            EXPECT_EQ(written.cycles, (std::vector<uint64_t> { 0, 1 }));
        }
        {
            Written written;
            auto reflow = builder(source, written)
                              .selection(FrameSelection::Blend)
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
            run(*reflow, 3);
            EXPECT_EQ(written.values, (std::vector<int> { 0, 5, 10 }));
        }
    }

    TEST(ReflowTest, WithoutAMixTheHeaviestFrameWinsAndTiesTakeTheFirst)
    {
        Written written;
        const auto source = numbers(9);
        auto reflow = builder(source, written)
                          .course(over(0.0, 0.5, CourseEdge::Hold, 0.0))
                          .course(over(0.5, 1.0, CourseEdge::Hold, 0.0))
                          .build();
        ASSERT_NE(reflow, nullptr);
        const auto courses = reflow->courses();
        ASSERT_EQ(courses.size(), 2U);

        run(*reflow, 1);
        EXPECT_EQ(written.values, (std::vector<int> { 0 }));

        ASSERT_TRUE(courses[1]->weight(3.0));
        run(*reflow, 1);
        EXPECT_EQ(written.values, (std::vector<int> { 0, 40 }));

        ASSERT_TRUE(courses[0]->weight(9.0));
        run(*reflow, 1);
        EXPECT_EQ(written.values, (std::vector<int> { 0, 40, 0 }));
    }

    TEST(ReflowTest, AZeroWeightCourseStillAdvancesButContributesNothing)
    {
        Written written;
        auto reflow = builder(numbers(9), written)
                          .course(over(0.0, 1.0, CourseEdge::Hold))
                          .course(over(0.0, 1.0, CourseEdge::Hold, 0.0))
                          .build();
        const auto courses = reflow->courses();
        ASSERT_TRUE(courses[0]->weight(0.0));

        run(*reflow, 4);

        EXPECT_TRUE(written.values.size() == 1);
        EXPECT_EQ(courses[0]->steps(), 3U);
        EXPECT_EQ(reflow->holds(), 0U);
    }

    TEST(ReflowTest, AMixSeesEveryPickedFrameWithItsWeightAndTheLiveCourses)
    {
        Written written;
        std::vector<double> weights;
        size_t seen_courses = 0;
        auto reflow = builder(numbers(9), written)
                          .course(over(0.0, 1.0, CourseEdge::Hold, 0.0))
                          .course(over(0.5, 1.0, CourseEdge::Hold, 0.0))
                          .operation(FrameOperation<int>::mix([&](const FrameContext& context, std::span<const WeightedFrame<int>> picks) {
                              seen_courses = context.courses.size();
                              for (const auto& pick : picks) {
                                  weights.push_back(pick.weight);
                              }
                              return picks.front().frame;
                          }))
                          .build();
        reflow->courses()[1]->weight(2.0);

        run(*reflow, 1);

        EXPECT_EQ(seen_courses, 2U);
        EXPECT_EQ(weights, (std::vector<double> { 1.0, 2.0 }));
    }

    TEST(ReflowTest, OperationsRunInOrderOnTheMixedFrame)
    {
        Written written;
        std::vector<int> derived;
        auto reflow = builder(numbers(5), written)
                          .operation(FrameOperation<int>::transform([](const FrameContext&, const std::shared_ptr<const int>& frame) {
                              return std::make_shared<const int>(*frame + 1);
                          }))
                          .operation(FrameOperation<int>::derive([&derived](const FrameContext&, const std::shared_ptr<const int>& frame) {
                              derived.push_back(*frame);
                          }))
                          .operation(FrameOperation<int>::condition([](const FrameContext&, const std::shared_ptr<const int>& frame) {
                              return *frame != 21;
                          }))
                          .build();

        run(*reflow, 5);

        EXPECT_EQ(derived, (std::vector<int> { 1, 11, 21, 31, 41 }));
        EXPECT_EQ(written.values, (std::vector<int> { 1, 11, 31, 41 }));
        EXPECT_EQ(reflow->skips(), 1U);
    }

    TEST(ReflowTest, ATransformThatAnswersNullWritesNothingThatCycle)
    {
        Written written;
        auto reflow = builder(numbers(4), written)
                          .operation(FrameOperation<int>::transform([](const FrameContext& context, const std::shared_ptr<const int>& frame) {
                              return context.cycle % 2 == 1 ? nullptr : frame;
                          }))
                          .build();

        run(*reflow, 4);

        EXPECT_EQ(written.values, (std::vector<int> { 0, 20 }));
        EXPECT_EQ(reflow->skips(), 2U);
    }

    TEST(ReflowTest, AlwaysWritesEveryCycleEvenWhenTheFrameIsUnchanged)
    {
        Written written;
        auto reflow = builder(numbers(3), written).rewrite(Rewrite::Always).build();

        run(*reflow, 6);

        EXPECT_EQ(written.values, (std::vector<int> { 0, 10, 20, 20, 20, 20 }));
        EXPECT_EQ(reflow->skips(), 0U);
    }

    TEST(ReflowTest, AFrameThatCannotBeReadKeepsTheLastOneAndIsCounted)
    {
        Written written;
        const auto source = std::make_shared<Flaky>(numbers(6));
        source->m_fail.store(2);
        auto reflow = builder(source, written).build();
        const auto course = reflow->courses().front();
        const auto misses = course->subscribe(CourseEventKind::Missed);

        run(*reflow, 5);

        EXPECT_EQ(written.values, (std::vector<int> { 0, 10, 30, 40 }));
        EXPECT_EQ(reflow->holds(), 1U);
        EXPECT_EQ(course->misses(), 1U);
        EXPECT_EQ(reflow->current().get(), source->read(4).get());

        auto awaiter = misses->next();
        ASSERT_TRUE(awaiter.await_ready());
        EXPECT_EQ(awaiter.await_resume().value, 2U);
    }

    TEST(ReflowTest, ASourceOrOperationOrSinkThatThrowsLosesOnlyThatCycle)
    {
        {
            Written written;
            const auto source = std::make_shared<Flaky>(numbers(5));
            source->m_throw.store(1);
            auto reflow = builder(source, written).build();

            run(*reflow, 4);

            EXPECT_EQ(written.values, (std::vector<int> { 0, 20, 30 }));
            EXPECT_EQ(reflow->failures(), 1U);
        }
        {
            std::vector<int> values;
            auto reflow = ReflowBuilder<int>(
                numbers(4),
                [&values](const std::shared_ptr<const int>& frame, const FrameContext&) {
                    if (*frame == 10) {
                        throw std::runtime_error("sink");
                    }
                    values.push_back(*frame);
                },
                k_rate)
                              .build();

            run(*reflow, 4);

            EXPECT_EQ(values, (std::vector<int> { 0, 20, 30 }));
            EXPECT_EQ(reflow->failures(), 1U);
            EXPECT_EQ(reflow->writes(), 3U);
        }
        {
            Written written;
            auto reflow = builder(numbers(3), written)
                              .operation(FrameOperation<int>::derive([](const FrameContext& context, const std::shared_ptr<const int>&) {
                                  if (context.cycle == 1) {
                                      throw std::runtime_error("operation");
                                  }
                              }))
                              .build();

            run(*reflow, 3);

            EXPECT_EQ(written.values, (std::vector<int> { 0, 20 }));
            EXPECT_EQ(reflow->failures(), 1U);
        }
    }

    TEST(ReflowTest, AHookCanSteerACourseSoWhatItSawChangesWhatComesNext)
    {
        Written written;
        auto reflow = builder(numbers(8), written)
                          .operation(FrameOperation<int>::derive([](const FrameContext& context, const std::shared_ptr<const int>& frame) {
                              if (*frame == 20) {
                                  (void)context.courses.front()->pause();
                              }
                          }))
                          .build();

        run(*reflow, 6);

        EXPECT_EQ(written.values, (std::vector<int> { 0, 10, 20 }));
        EXPECT_TRUE(reflow->courses().front()->paused());
    }

    TEST(ReflowTest, APausedCourseShowsItsStartAndMovesWhenStarted)
    {
        Written written;
        auto reflow = builder(numbers(6), written).course(over(0.0, 1.0), true).build();
        const auto course = reflow->courses().front();

        run(*reflow, 4);
        EXPECT_EQ(written.values, (std::vector<int> { 0 }));

        ASSERT_TRUE(course->start());
        run(*reflow, 2);
        EXPECT_EQ(written.values, (std::vector<int> { 0, 10, 20 }));
    }

    TEST(ReflowTest, SeekingAPausedCourseScrubsWhatIsWritten)
    {
        Written written;
        auto reflow = builder(numbers(9), written).course(over(0.0, 1.0, CourseEdge::Hold), true).build();
        const auto course = reflow->courses().front();

        run(*reflow, 1);
        ASSERT_TRUE(course->seek(0.5));
        run(*reflow, 1);
        ASSERT_TRUE(course->seek_normalized(1.0));
        run(*reflow, 1);

        EXPECT_EQ(written.values, (std::vector<int> { 0, 40, 80 }));
    }

    TEST(ReflowTest, CoursesCanBeAddedAndRemovedWhileRunning)
    {
        Written written;
        auto reflow = builder(numbers(9), written).course(over(0.0, 1.0, CourseEdge::Hold, 0.0)).build();
        ASSERT_EQ(reflow->courses().size(), 1U);

        const auto extra = reflow->add_course(over(0.5, 1.0, CourseEdge::Hold, 0.0));
        ASSERT_NE(extra, nullptr);
        ASSERT_TRUE(extra->weight(5.0));
        EXPECT_EQ(reflow->courses().size(), 2U);
        run(*reflow, 1);
        EXPECT_EQ(written.values, (std::vector<int> { 40 }));

        EXPECT_TRUE(reflow->remove_course(extra));
        EXPECT_FALSE(reflow->remove_course(extra));
        run(*reflow, 1);
        EXPECT_EQ(written.values, (std::vector<int> { 40, 0 }));

        EXPECT_EQ(reflow->add_course(over(1.0, 1.0)), nullptr);
        EXPECT_EQ(reflow->courses().size(), 1U);
    }

    TEST(ReflowTest, NothingToShowWritesNothingAndWarnsOnce)
    {
        Written written;
        auto reflow = builder(numbers(4), written).course(over(0.0, 1.0)).build();
        ASSERT_TRUE(reflow->courses().front()->weight(0.0));

        run(*reflow, 5);

        EXPECT_TRUE(written.values.empty());
        EXPECT_EQ(reflow->holds(), 5U);
        EXPECT_EQ(reflow->current(), nullptr);
    }

    TEST(ReflowTest, EachCourseAnnouncesTheFramesItPicks)
    {
        Written written;
        auto reflow = builder(numbers(4), written).build();
        const auto frames = reflow->courses().front()->subscribe(CourseEventKind::Frame);

        run(*reflow, 2);

        auto awaiter = frames->next();
        ASSERT_TRUE(awaiter.await_ready());
        EXPECT_EQ(awaiter.await_resume().value, 1U);
        EXPECT_EQ(reflow->courses().front()->frame(), 1U);
    }

    TEST(ReflowTest, SelectionPicksWeightedIndicesAndNothingForAnUnusableWeight)
    {
        const TimeLocation between { .lower = 2, .upper = 3, .fraction = 0.25, .placement = TimePlacement::Within };
        const TimeLocation exact { .lower = 5, .upper = 5, .fraction = 0.0, .placement = TimePlacement::Within };
        const TimeLocation late { .lower = 2, .upper = 3, .fraction = 1.0, .placement = TimePlacement::Within };

        const auto blend = Kriya::select_frames(between, FrameSelection::Blend, 2.0);
        ASSERT_EQ(blend.count, 2U);
        EXPECT_EQ(blend.items[0].index, 2U);
        EXPECT_DOUBLE_EQ(blend.items[0].weight, 1.5);
        EXPECT_EQ(blend.items[1].index, 3U);
        EXPECT_DOUBLE_EQ(blend.items[1].weight, 0.5);

        EXPECT_EQ(Kriya::select_frames(exact, FrameSelection::Blend, 1.0).count, 1U);
        EXPECT_EQ(Kriya::select_frames(late, FrameSelection::Blend, 1.0).items[0].index, 3U);
        EXPECT_EQ(Kriya::select_frames(between, FrameSelection::Floor, 1.0).items[0].index, 2U);
        EXPECT_EQ(Kriya::select_frames(between, FrameSelection::Nearest, 1.0).items[0].index, 2U);
        EXPECT_EQ(Kriya::nearest_frame({ .lower = 2, .upper = 3, .fraction = 0.5 }), 3U);

        for (const double weight : { 0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
                 std::numeric_limits<double>::infinity() }) {
            EXPECT_EQ(Kriya::select_frames(between, FrameSelection::Blend, weight).count, 0U) << weight;
        }
    }

    TEST(ReflowTest, TheFrameClockRunsItOncePerFrameAndDroppingItStopsIt)
    {
        const auto rate = static_cast<double>(Vruta::s_registered_frame_rate);
        if (rate <= 0.0) {
            GTEST_SKIP() << "no frame rate is registered";
        }

        FrameScheduler clock;
        Written written;
        auto reflow = ReflowBuilder<int>(numbers(40, 1.0 / rate), written.sink(), rate, clock.scheduler)
                          .start();
        ASSERT_NE(reflow, nullptr);

        clock.tick(10);
        const auto cycles = reflow->cycle();
        EXPECT_GE(cycles, 9U);
        EXPECT_LE(cycles, 11U);
        EXPECT_GE(written.values.size(), 1U);
        EXPECT_LE(written.values.size(), cycles);

        reflow.reset();
        const auto after = written.values.size();
        clock.tick(10);
        EXPECT_EQ(written.values.size(), after);
    }

}
}
