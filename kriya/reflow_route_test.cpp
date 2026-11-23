#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kakshya/Semantic/MeshAssemblyMotionFrames.hpp"
#include "MayaFlux/Kriya/Reflow.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Kakshya::MeshAssemblyMotionFrames;
    using Kakshya::MeshAssemblyContainer;
    using Kakshya::MeshAssemblyLocals;
    using Kakshya::ResidentFrames;
    using Kakshya::SampledFrames;
    using Kinesis::TimeSampling;
    using Kriya::CourseEdge;
    using Kriya::CourseSetting;
    using Kriya::FrameContext;
    using Kriya::FrameOperation;
    using Kriya::Reflow;
    using Kriya::ReflowBuilder;

    constexpr double k_route_rate = 8.0;

    TimeSampling route_step(double step)
    {
        auto sampling = TimeSampling::uniform(step);
        if (!sampling) {
            throw std::runtime_error("sampling was rejected");
        }
        return *sampling;
    }

    std::shared_ptr<const ResidentFrames<int>> route_values(size_t count, int scale)
    {
        std::vector<int> frames;
        for (size_t i = 0; i < count; ++i) {
            frames.push_back(static_cast<int>(i) * scale);
        }
        return ResidentFrames<int>::create(route_step(0.125), std::move(frames));
    }

    CourseSetting route_over(double in, double out, CourseEdge edge = CourseEdge::Once, double speed = 1.0)
    {
        CourseSetting setting;
        setting.in = in;
        setting.out = out;
        setting.edge = edge;
        setting.speed = speed;
        return setting;
    }

    struct RouteLog {
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

    Reflow<int>::Sink route_discard()
    {
        return [](const std::shared_ptr<const int>&, const FrameContext&) { };
    }

    void route_run(Reflow<int>& reflow, int cycles)
    {
        for (int i = 0; i < cycles; ++i) {
            reflow.advance();
        }
    }

    /** The frame at a time is the time in thousandths, so every eighth of a second is 125 more. */
    class Ramp final : public SampledFrames<int> {
    public:
        [[nodiscard]] std::pair<double, double> extent() const override { return { 0.0, 1.0 }; }

        [[nodiscard]] std::shared_ptr<const int> sample(double time) const override
        {
            if (time > m_refuse_after.load()) {
                return nullptr;
            }
            return std::make_shared<const int>(static_cast<int>(std::lround(time * 1000.0)));
        }

        mutable std::atomic<double> m_refuse_after { 1e9 };
    };

    TEST(ReflowSampledTest, ASampledSourceIsAskedForTheTimeOfItsCourse)
    {
        RouteLog log;
        auto reflow = ReflowBuilder<int>::sampling(std::make_shared<Ramp>(), log.sink(), k_route_rate).build();
        ASSERT_NE(reflow, nullptr);

        route_run(*reflow, 4);

        EXPECT_EQ(log.values, (std::vector<int> { 0, 125, 250, 375 }));
        EXPECT_EQ(reflow->source(), nullptr);
        EXPECT_NE(reflow->sampled(), nullptr);
    }

    TEST(ReflowSampledTest, ASampledSourceTakesItsCoursesRangeFromItsExtent)
    {
        RouteLog log;
        auto reflow = ReflowBuilder<int>::sampling(std::make_shared<Ramp>(), log.sink(), k_route_rate).build();
        ASSERT_NE(reflow, nullptr);

        route_run(*reflow, 12);

        EXPECT_EQ(log.values.back(), 1000);
        EXPECT_TRUE(reflow->courses().front()->finished());
    }

    TEST(ReflowSampledTest, ATimeTheSourceCannotAnswerIsAMissAndTheLastFrameStays)
    {
        RouteLog log;
        const auto ramp = std::make_shared<Ramp>();
        ramp->m_refuse_after.store(0.3);
        auto reflow = ReflowBuilder<int>::sampling(ramp, log.sink(), k_route_rate).build();
        ASSERT_NE(reflow, nullptr);

        route_run(*reflow, 6);

        EXPECT_EQ(log.values, (std::vector<int> { 0, 125, 250 }));
        EXPECT_EQ(reflow->holds(), 3U);
        EXPECT_EQ(reflow->courses().front()->misses(), 3U);
    }

    TEST(ReflowSampledTest, ACourseWithNoWeightShowsNothing)
    {
        RouteLog log;
        auto reflow = ReflowBuilder<int>::sampling(std::make_shared<Ramp>(), log.sink(), k_route_rate).build();
        ASSERT_NE(reflow, nullptr);
        ASSERT_TRUE(reflow->courses().front()->weight(0.0));

        route_run(*reflow, 3);

        EXPECT_TRUE(log.values.empty());
        EXPECT_EQ(reflow->holds(), 3U);
    }

    TEST(ReflowSampledTest, ANullSampledSourceMakesNoReflow)
    {
        RouteLog log;

        EXPECT_EQ(ReflowBuilder<int>::sampling(nullptr, log.sink(), k_route_rate).build(), nullptr);
    }

    TEST(ReflowRouteTest, ARouteWithASinkOfItsOwnLeavesTheSharedMix)
    {
        RouteLog shared;
        RouteLog own;
        auto reflow = ReflowBuilder<int>(route_values(8, 10), shared.sink(), k_route_rate)
                          .course(route_over(0.0, 0.875))
                          .route({ .course = route_over(0.0, 0.875), .source = route_values(8, 100), .sink = own.sink() })
                          .build();
        ASSERT_NE(reflow, nullptr);

        route_run(*reflow, 4);

        EXPECT_EQ(shared.values, (std::vector<int> { 0, 10, 20, 30 }));
        EXPECT_EQ(own.values, (std::vector<int> { 0, 100, 200, 300 }));
        EXPECT_EQ(reflow->courses().size(), 2U);
        EXPECT_EQ(reflow->writes(), 8U);
    }

    TEST(ReflowRouteTest, ARouteEntersAfterItsDelayAndStartsWhereItsCourseStarts)
    {
        RouteLog own;
        auto reflow = ReflowBuilder<int>(route_values(8, 10), route_discard(), k_route_rate)
                          .route({ .course = route_over(0.0, 0.875), .sink = own.sink(), .enters_after = 0.25 })
                          .build();
        ASSERT_NE(reflow, nullptr);

        route_run(*reflow, 5);

        EXPECT_EQ(own.cycles, (std::vector<uint64_t> { 2, 3, 4 }));
        EXPECT_EQ(own.values, (std::vector<int> { 0, 10, 20 }));
    }

    TEST(ReflowRouteTest, ARouteShownEveryOtherCycleStillKeepsTheTimeOfEveryCycle)
    {
        RouteLog own;
        auto reflow = ReflowBuilder<int>(route_values(8, 10), route_discard(), k_route_rate)
                          .route({ .course = route_over(0.0, 0.875), .sink = own.sink(), .every = 2 })
                          .build();
        ASSERT_NE(reflow, nullptr);

        route_run(*reflow, 6);

        EXPECT_EQ(own.cycles, (std::vector<uint64_t> { 0, 2, 4 }));
        EXPECT_EQ(own.values, (std::vector<int> { 0, 20, 40 }));
    }

    TEST(ReflowRouteTest, TwoRoutesOverOneSourceReadAtTheirOwnSpeeds)
    {
        RouteLog slow;
        RouteLog fast;
        const auto source = route_values(8, 10);
        auto reflow = ReflowBuilder<int>(source, route_discard(), k_route_rate)
                          .route({ .course = route_over(0.0, 0.875, CourseEdge::Once, 0.5), .sink = slow.sink() })
                          .route({ .course = route_over(0.0, 0.875, CourseEdge::Once, 2.0), .sink = fast.sink() })
                          .build();
        ASSERT_NE(reflow, nullptr);

        route_run(*reflow, 4);

        EXPECT_EQ(slow.values, (std::vector<int> { 0, 10 }));
        EXPECT_EQ(fast.values, (std::vector<int> { 0, 20, 40, 60 }));
    }

    TEST(ReflowRouteTest, ARoutesOperationsRunOnItsFrameBeforeItJoinsTheMix)
    {
        RouteLog shared;
        std::vector<const Kriya::Course*> seen;
        auto reflow = ReflowBuilder<int>(route_values(8, 10), shared.sink(), k_route_rate)
                          .route({ .course = route_over(0.0, 0.875),
                              .operations = { FrameOperation<int>::transform([](const FrameContext&, const std::shared_ptr<const int>& frame) {
                                                  return std::make_shared<const int>(*frame + 1000);
                                              }),
                                  FrameOperation<int>::derive([&seen](const FrameContext& context, const std::shared_ptr<const int>&) {
                                      seen.push_back(context.course);
                                  }) } })
                          .build();
        ASSERT_NE(reflow, nullptr);

        route_run(*reflow, 3);

        EXPECT_EQ(shared.values, (std::vector<int> { 1000, 1010, 1020 }));
        ASSERT_EQ(seen.size(), 3U);
        EXPECT_EQ(seen.front(), reflow->courses().front().get());
    }

    TEST(ReflowRouteTest, ARouteWhoseOperationDeclinesWritesNothingAndIsCounted)
    {
        RouteLog own;
        auto reflow = ReflowBuilder<int>(route_values(8, 10), route_discard(), k_route_rate)
                          .route({ .course = route_over(0.0, 0.875),
                              .operations = { FrameOperation<int>::condition([](const FrameContext&, const std::shared_ptr<const int>& frame) {
                                  return *frame != 10;
                              }) },
                              .sink = own.sink() })
                          .build();
        ASSERT_NE(reflow, nullptr);

        route_run(*reflow, 3);

        EXPECT_EQ(own.values, (std::vector<int> { 0, 20 }));
        EXPECT_EQ(reflow->skips(), 1U);
    }

    TEST(ReflowRouteTest, ARouteOnASampledSourceOfItsOwnPlaysBesideAListedOne)
    {
        RouteLog shared;
        RouteLog own;
        auto reflow = ReflowBuilder<int>(route_values(8, 10), shared.sink(), k_route_rate)
                          .course(route_over(0.0, 0.875))
                          .route({ .course = route_over(0.0, 1.0), .sampled = std::make_shared<Ramp>(), .sink = own.sink() })
                          .build();
        ASSERT_NE(reflow, nullptr);

        route_run(*reflow, 3);

        EXPECT_EQ(shared.values, (std::vector<int> { 0, 10, 20 }));
        EXPECT_EQ(own.values, (std::vector<int> { 0, 125, 250 }));
    }

    TEST(ReflowRouteTest, ARouteThatCannotWorkIsRefused)
    {
        RouteLog log;
        auto reflow = ReflowBuilder<int>(route_values(8, 10), log.sink(), k_route_rate).course(route_over(0.0, 0.875)).build();
        ASSERT_NE(reflow, nullptr);
        const auto source = route_values(4, 10);

        EXPECT_EQ(reflow->add_route({ .course = route_over(0.0, 0.5), .every = 0 }), nullptr);
        EXPECT_EQ(reflow->add_route({ .course = route_over(0.0, 0.5), .enters_after = -1.0 }), nullptr);
        EXPECT_EQ(reflow->add_route({ .course = route_over(0.0, 0.5), .source = source, .sampled = std::make_shared<Ramp>() }), nullptr);
        EXPECT_EQ(reflow->add_route({ .course = route_over(0.0, 0.5),
                      .operations = { FrameOperation<int>::mix([](const FrameContext&, std::span<const Kriya::WeightedFrame<int>> frames) {
                          return frames.front().frame;
                      }) } }),
            nullptr);
        EXPECT_EQ(reflow->add_route({ .course = route_over(0.0, 0.5), .operations = { FrameOperation<int>::transform({}) } }), nullptr);
        EXPECT_EQ(reflow->add_route({ .course = route_over(0.5, 0.5) }), nullptr);
        EXPECT_EQ(reflow->courses().size(), 1U);
    }

    TEST(ReflowRouteTest, AReflowOfOnlySelfSufficientRoutesNeedsNoSharedSourceOrSink)
    {
        RouteLog own;
        auto reflow = ReflowBuilder<int>(nullptr, nullptr, k_route_rate)
                          .route({ .course = route_over(0.0, 0.875), .source = route_values(8, 10), .sink = own.sink() })
                          .build();
        ASSERT_NE(reflow, nullptr);

        route_run(*reflow, 3);

        EXPECT_EQ(own.values, (std::vector<int> { 0, 10, 20 }));
        EXPECT_EQ(reflow->holds(), 0U);
    }

    TEST(ReflowRouteTest, ARouteThatLeavesOutWhatNothingSharesMakesNoReflow)
    {
        RouteLog own;

        EXPECT_EQ(ReflowBuilder<int>(nullptr, nullptr, k_route_rate)
                      .route({ .course = route_over(0.0, 0.875), .sink = own.sink() })
                      .build(),
            nullptr);
        EXPECT_EQ(ReflowBuilder<int>(nullptr, nullptr, k_route_rate)
                      .route({ .course = route_over(0.0, 0.875), .source = route_values(8, 10) })
                      .build(),
            nullptr);
    }

    TEST(ReflowRouteTest, ARemovedRouteStopsWriting)
    {
        RouteLog own;
        auto reflow = ReflowBuilder<int>(route_values(8, 10), route_discard(), k_route_rate)
                          .route({ .course = route_over(0.0, 0.875), .sink = own.sink() })
                          .build();
        ASSERT_NE(reflow, nullptr);

        route_run(*reflow, 2);
        ASSERT_TRUE(reflow->remove_course(reflow->courses().front()));
        route_run(*reflow, 2);

        EXPECT_EQ(own.values, (std::vector<int> { 0, 10 }));
    }

    class AssemblyMotionFramesTest : public ::testing::Test {
    protected:
        void SetUp() override
        {
            assembly = std::make_shared<MeshAssemblyContainer>();
            row = *assembly->add_placement({ .name = "arm" });

            const auto times = *TimeSampling::acyclic({ 0.0, 1.0, 2.0 });
            auto translation = Kinesis::TimedValues<glm::vec3>::create({ .times = times,
                .values = { glm::vec3(0.0F), glm::vec3(1.0F, 0.0F, 0.0F), glm::vec3(3.0F, 0.0F, 0.0F) } });
            std::ignore = assembly->add_motion({ .name = "swing", .channels = { { .row = row, .translation = translation } } });
            std::ignore = assembly->add_motion({ .name = "idle", .channels = {} });
        }

        std::shared_ptr<MeshAssemblyContainer> assembly;
        size_t row {};
    };

    TEST_F(AssemblyMotionFramesTest, AMotionsRangeRunsFromItsFirstKeyToItsLast)
    {
        const auto frames = MeshAssemblyMotionFrames::create(assembly, 0);

        ASSERT_NE(frames, nullptr);
        EXPECT_EQ(frames->extent(), (std::pair<double, double> { 0.0, 2.0 }));
    }

    TEST_F(AssemblyMotionFramesTest, ATimeBetweenKeysGivesTheInterpolatedPlacement)
    {
        const auto frames = MeshAssemblyMotionFrames::create(assembly, 0);
        ASSERT_NE(frames, nullptr);

        const auto frame = frames->sample(1.5);

        ASSERT_NE(frame, nullptr);
        ASSERT_EQ(frame->matrices.size(), 1U);
        EXPECT_NEAR(frame->matrices[0][3].x, 2.0F, 1e-5F);
    }

    TEST_F(AssemblyMotionFramesTest, ANullAssemblyAMissingMotionAndAMotionWithNoKeysMakeNoSource)
    {
        EXPECT_EQ(MeshAssemblyMotionFrames::create(nullptr, 0), nullptr);
        EXPECT_EQ(MeshAssemblyMotionFrames::create(assembly, 5), nullptr);
        EXPECT_EQ(MeshAssemblyMotionFrames::create(assembly, 1), nullptr);
    }

    TEST_F(AssemblyMotionFramesTest, AReflowPlaysAMotionWithoutBakingIt)
    {
        const auto frames = MeshAssemblyMotionFrames::create(assembly, 0);
        ASSERT_NE(frames, nullptr);
        std::vector<float> x;
        auto reflow = ReflowBuilder<MeshAssemblyLocals>::sampling(
            frames,
            [&x](const std::shared_ptr<const MeshAssemblyLocals>& frame, const FrameContext&) {
                x.push_back(frame->matrices[0][3].x);
            },
            k_route_rate)
                          .build();
        ASSERT_NE(reflow, nullptr);

        for (int i = 0; i < 5; ++i) {
            reflow->advance();
        }

        ASSERT_EQ(x.size(), 5U);
        EXPECT_NEAR(x[0], 0.0F, 1e-5F);
        EXPECT_NEAR(x[4], 0.5F, 1e-5F);
    }

} // namespace

}
