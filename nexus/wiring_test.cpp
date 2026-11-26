#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "../support/frame_scheduler.hpp"
#include "../support/log_capture.hpp"
#include "../support/scripted_window.hpp"

#include "MayaFlux/Buffers/BufferManager.hpp"
#include "MayaFlux/Kriya/Course.hpp"
#include "MayaFlux/Nexus/Fabric.hpp"
#include "MayaFlux/Nexus/Principals/Locus.hpp"
#include "MayaFlux/Vruta/EventManager.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Nexus::Agent;
    using Nexus::Emitter;
    using Nexus::Fabric;
    using Nexus::InfluenceContext;
    using Nexus::PlacementControls;
    using Nexus::PerceptionContext;
    using Nexus::Sensor;
    using Nexus::Wiring;

    constexpr uint64_t k_interval_samples = 48;

    Vruta::SoundRoutine counting_routine(int* counter)
    {
        ++*counter;
        co_return;
    }

    class NexusWiringTest : public ::testing::Test {
    protected:
        std::shared_ptr<Emitter> make_counting_emitter()
        {
            return std::make_shared<Emitter>([this](const InfluenceContext& ctx) {
                ++fired;
                last = ctx;
            });
        }

        void run_samples(uint64_t count)
        {
            frames.scheduler->process_token(Vruta::ProcessingToken::SAMPLE_ACCURATE, count);
        }

        void step_samples(uint64_t count)
        {
            for (uint64_t i = 0; i < count; ++i) {
                run_samples(1);
            }
        }

        void step_frames(uint64_t count)
        {
            for (uint64_t i = 0; i < count; ++i) {
                frames.tick(1);
            }
        }

        FrameScheduler frames;
        std::shared_ptr<Vruta::EventManager> events = std::make_shared<Vruta::EventManager>();
        std::shared_ptr<Buffers::BufferManager> buffers = std::make_shared<Buffers::BufferManager>();
        Fabric fabric { frames.scheduler, events, buffers, 1.0F };
        int fired {};
        InfluenceContext last;
    };

    TEST_F(NexusWiringTest, EveryFiresOnTheFirstTickAndThenOncePerInterval)
    {
        const auto emitter = make_counting_emitter();
        fabric.wire(emitter).every(0.001).finalise();

        run_samples(1);
        EXPECT_EQ(fired, 1);

        run_samples(k_interval_samples);
        EXPECT_EQ(fired, 2);

        run_samples(k_interval_samples);
        EXPECT_EQ(fired, 3);
    }

    TEST_F(NexusWiringTest, EveryDoesNotFireOnCommit)
    {
        const auto emitter = make_counting_emitter();
        fabric.wire(emitter).every(0.001).finalise();

        fabric.commit();
        fabric.commit();

        EXPECT_EQ(fired, 0);
    }

    TEST_F(NexusWiringTest, EveryTokenOnTheSampleTokenFiresOncePerSample)
    {
        const auto emitter = make_counting_emitter();
        fabric.wire(emitter).every(Vruta::ProcessingToken::SAMPLE_ACCURATE).finalise();

        run_samples(5);
        EXPECT_EQ(fired, 5);
    }

    TEST_F(NexusWiringTest, EveryTokenStopsAfterTheRequestedCount)
    {
        const auto emitter = make_counting_emitter();
        fabric.wire(emitter).every(Vruta::ProcessingToken::SAMPLE_ACCURATE).times(3).finalise();

        run_samples(10);
        EXPECT_EQ(fired, 3);
    }

    TEST_F(NexusWiringTest, WindowEventWiringFiresForMatchingEventsOnly)
    {
        const auto window = std::make_shared<ScriptedWindow>(400U, 300U);
        const auto emitter = make_counting_emitter();
        fabric.wire(emitter)
            .on(window, Vruta::WindowEventFilter { Core::WindowEventType::MOUSE_MOTION })
            .finalise();

        window->key_down(IO::Keys::A);
        EXPECT_EQ(fired, 0);

        window->move_to_pixel(10.0, 20.0);
        EXPECT_EQ(fired, 1);
    }

    TEST_F(NexusWiringTest, FiringsFromDifferentThreadsKeepTheirOwnCursor)
    {
        constexpr int k_rounds = 4000;
        const auto window = std::make_shared<ScriptedWindow>(400U, 300U);

        std::atomic<int> aimed { 0 };
        std::atomic<int> unaimed { 0 };
        std::atomic<int> leaked { 0 };
        const auto main_thread = std::this_thread::get_id();

        const auto emitter = std::make_shared<Emitter>([&](const InfluenceContext& ctx) {
            if (std::this_thread::get_id() != main_thread) {
                ++(ctx.cursor_pos.has_value() ? leaked : unaimed);
            } else if (ctx.cursor_pos.has_value()) {
                ++aimed;
            }
        });
        fabric.wire(emitter)
            .on(window, Vruta::WindowEventFilter { Core::WindowEventType::MOUSE_MOTION })
            .finalise();

        const uint32_t id = emitter->id();
        std::atomic<bool> go { false };
        std::thread worker([&] {
            while (!go.load()) { }
            for (int i = 0; i < k_rounds; ++i) {
                fabric.fire(id);
            }
        });

        go.store(true);
        for (int i = 0; i < k_rounds; ++i) {
            window->move_to_pixel(static_cast<double>(i % 400), static_cast<double>(i % 300));
        }
        worker.join();

        EXPECT_EQ(leaked.load(), 0);
        EXPECT_EQ(aimed.load(), k_rounds);
        EXPECT_EQ(unaimed.load(), k_rounds);
    }

    TEST_F(NexusWiringTest, FinalisedWiringReportsItsConfiguration)
    {
        const auto emitter = make_counting_emitter();
        fabric.wire(emitter).every(0.25).for_duration(2.0).times(3).finalise();

        const Wiring* wiring = fabric.wiring_for(emitter->id());

        ASSERT_NE(wiring, nullptr);
        EXPECT_EQ(wiring->entity_id(), emitter->id());
        ASSERT_TRUE(wiring->interval().has_value());
        EXPECT_DOUBLE_EQ(*wiring->interval(), 0.25);
        ASSERT_TRUE(wiring->duration().has_value());
        EXPECT_DOUBLE_EQ(*wiring->duration(), 2.0);
        EXPECT_EQ(wiring->times_count(), 3U);
        EXPECT_FALSE(wiring->has_bind());
        EXPECT_FALSE(wiring->has_position_fn());
    }

    TEST_F(NexusWiringTest, WiringDefaultsToOneRepetitionAndNoInterval)
    {
        const auto emitter = make_counting_emitter();
        fabric.wire(emitter).bind([] { }).finalise();

        const Wiring* wiring = fabric.wiring_for(emitter->id());

        ASSERT_NE(wiring, nullptr);
        EXPECT_FALSE(wiring->interval().has_value());
        EXPECT_FALSE(wiring->duration().has_value());
        EXPECT_EQ(wiring->times_count(), 1U);
        EXPECT_TRUE(wiring->move_steps().empty());
    }

    TEST_F(NexusWiringTest, RemoveCancelsTheRecurringTask)
    {
        const auto emitter = make_counting_emitter();
        fabric.wire(emitter).every(0.001).finalise();
        run_samples(1);
        ASSERT_EQ(fired, 1);

        fabric.remove(emitter->id());
        run_samples(k_interval_samples * 10);

        EXPECT_EQ(fired, 1);
    }

    TEST_F(NexusWiringTest, DestroyingTheFabricCancelsTheRecurringTask)
    {
        int local_fired {};
        {
            Fabric local(frames.scheduler, events, buffers, 1.0F);
            auto emitter = std::make_shared<Emitter>([&local_fired](const InfluenceContext&) { ++local_fired; });
            local.wire(emitter).every(0.001).finalise();
            run_samples(1);
            ASSERT_EQ(local_fired, 1);
        }

        run_samples(k_interval_samples * 10);

        EXPECT_EQ(local_fired, 1);
    }

    TEST_F(NexusWiringTest, CancelWiringStopsTheTaskAndKeepsTheEntity)
    {
        const auto emitter = make_counting_emitter();
        fabric.wire(emitter).every(0.001).finalise();
        run_samples(1);
        ASSERT_EQ(fired, 1);

        fabric.cancel_wiring(emitter->id());
        run_samples(k_interval_samples * 10);

        EXPECT_EQ(fired, 1);
        EXPECT_NE(fabric.get_emitter(emitter->id()), nullptr);
        EXPECT_NE(fabric.wiring_for(emitter->id()), nullptr);
    }

    TEST_F(NexusWiringTest, CancelWiringStopsAChoreography)
    {
        const auto emitter = make_counting_emitter();
        fabric.wire(emitter).move_to(glm::vec3(1.0F, 0.0F, 0.0F), 0.01).finalise();

        fabric.cancel_wiring(emitter->id());
        run_samples(2000);

        EXPECT_EQ(fired, 0);
    }

    TEST_F(NexusWiringTest, CancelWiringRunsTheBindDetachOnce)
    {
        int attached {};
        int detached {};
        const auto emitter = make_counting_emitter();
        fabric.wire(emitter).bind([&attached] { ++attached; }, [&detached] { ++detached; }).finalise();
        ASSERT_EQ(attached, 1);
        ASSERT_EQ(detached, 0);

        fabric.cancel_wiring(emitter->id());
        fabric.cancel_wiring(emitter->id());

        EXPECT_EQ(detached, 1);
        EXPECT_TRUE(fabric.wiring_for(emitter->id())->has_bind_detach());
    }

    TEST_F(NexusWiringTest, CancelWiringForAnUnknownOrUnwiredIdIsHarmless)
    {
        const auto emitter = make_counting_emitter();
        [[maybe_unused]] auto pending = fabric.wire(emitter);

        EXPECT_NO_THROW(fabric.cancel_wiring(emitter->id()));
        EXPECT_NO_THROW(fabric.cancel_wiring(999));
    }

    TEST_F(NexusWiringTest, CancelWiringStopsCommitFromFiring)
    {
        const auto emitter = make_counting_emitter();
        fabric.wire(emitter).finalise();
        fabric.commit();
        ASSERT_EQ(fired, 1);

        fabric.cancel_wiring(emitter->id());
        fabric.commit();

        EXPECT_EQ(fired, 1);
    }

    TEST_F(NexusWiringTest, CancelledEntitiesCanStillBeFiredById)
    {
        const auto emitter = make_counting_emitter();
        fabric.wire(emitter).every(0.001).finalise();
        fabric.cancel_wiring(emitter->id());

        fabric.fire(emitter->id());

        EXPECT_EQ(fired, 1);
    }

    TEST_F(NexusWiringTest, FinalisedWiringIsOnlyReachableAsConst)
    {
        static_assert(std::is_const_v<std::remove_pointer_t<decltype(fabric.wiring_for(1))>>);
        SUCCEED();
    }

    TEST_F(NexusWiringTest, ForDurationStopsRecurringFiring)
    {
        const auto emitter = make_counting_emitter();
        fabric.wire(emitter).every(0.001).for_duration(0.005).finalise();

        step_samples(k_interval_samples * 30);

        EXPECT_GE(fired, 4);
        EXPECT_LE(fired, 8);
    }

    TEST_F(NexusWiringTest, ForDurationOnTheFrameTokenFollowsTheFrameClock)
    {
        const auto emitter = make_counting_emitter();
        fabric.wire(emitter)
            .every(0.05, Vruta::ProcessingToken::FRAME_ACCURATE)
            .for_duration(0.1, Vruta::ProcessingToken::FRAME_ACCURATE)
            .finalise();

        step_frames(30);

        EXPECT_GE(fired, 2);
        EXPECT_LE(fired, 4);
    }

    TEST_F(NexusWiringTest, EveryOnTheFrameTokenFiresOncePerIntervalInFrames)
    {
        const auto emitter = make_counting_emitter();
        fabric.wire(emitter).every(0.05, Vruta::ProcessingToken::FRAME_ACCURATE).finalise();

        frames.tick(1);
        EXPECT_EQ(fired, 1);

        frames.tick(30);
        EXPECT_GE(fired, 9);
        EXPECT_LE(fired, 12);
    }

    TEST_F(NexusWiringTest, TimesLimitsAnEveryWiringToThatManyFirings)
    {
        const auto emitter = make_counting_emitter();
        fabric.wire(emitter).every(0.001).times(3).finalise();

        step_samples(k_interval_samples * 20);

        EXPECT_EQ(fired, 3);
    }

    TEST_F(NexusWiringTest, TimesIsExactEvenWhenOneCallCoversManyIntervals)
    {
        const auto emitter = make_counting_emitter();
        fabric.wire(emitter).every(0.001).times(3).finalise();

        run_samples(k_interval_samples * 20);

        EXPECT_EQ(fired, 3);
    }

    TEST_F(NexusWiringTest, TimesOfOneOnEveryStaysUnlimited)
    {
        const auto emitter = make_counting_emitter();
        fabric.wire(emitter).every(0.001).times(1).finalise();

        step_samples(k_interval_samples * 5);

        EXPECT_GE(fired, 5);
    }

    TEST_F(NexusWiringTest, TimesStopsPositionFromOnTheLastFiring)
    {
        int steps {};
        const auto emitter = make_counting_emitter();
        fabric.wire(emitter)
            .every(0.001)
            .times(2)
            .position_from([&steps] { return glm::vec3(static_cast<float>(++steps), 0.0F, 0.0F); })
            .finalise();

        step_samples(k_interval_samples * 10);

        EXPECT_EQ(fired, 2);
        EXPECT_EQ(steps, 2);
        EXPECT_EQ(*emitter->position(), glm::vec3(2.0F, 0.0F, 0.0F));
    }

    TEST_F(NexusWiringTest, TimesCountsFramesOnTheFrameToken)
    {
        const auto emitter = make_counting_emitter();
        fabric.wire(emitter).every(0.05, Vruta::ProcessingToken::FRAME_ACCURATE).times(3).finalise();

        step_frames(40);

        EXPECT_EQ(fired, 3);
    }

    TEST_F(NexusWiringTest, PositionFromMovesTheEntityBeforeEachFiring)
    {
        int steps {};
        const auto emitter = make_counting_emitter();
        fabric.wire(emitter)
            .every(0.001)
            .position_from("walk", [&steps] { return glm::vec3(static_cast<float>(++steps), 0.0F, 0.0F); })
            .finalise();

        run_samples(1);
        EXPECT_EQ(last.position, glm::vec3(1.0F, 0.0F, 0.0F));

        run_samples(k_interval_samples);
        EXPECT_EQ(last.position, glm::vec3(2.0F, 0.0F, 0.0F));
        ASSERT_TRUE(emitter->position().has_value());
        EXPECT_EQ(*emitter->position(), glm::vec3(2.0F, 0.0F, 0.0F));
        EXPECT_EQ(fabric.wiring_for(emitter->id())->position_fn_name(), "walk");
    }

    TEST_F(NexusWiringTest, ACommitDrivenWiringIsPlacedBeforeItIsIndexed)
    {
        const auto emitter = make_counting_emitter();
        const glm::quat turn(0.5F, 0.5F, 0.5F, 0.5F);
        fabric.wire(emitter)
            .place_from("anchor", [turn] { return PlacementControls { .position = glm::vec3(3.0F), .orientation = turn }; })
            .finalise();

        fabric.commit();

        EXPECT_EQ(fired, 1);
        EXPECT_EQ(last.position, glm::vec3(3.0F));
        EXPECT_EQ(last.orientation, turn);
        EXPECT_EQ(fabric.within_radius(glm::vec3(3.0F), 0.5F).size(), 1U);
        EXPECT_EQ(fabric.wiring_for(emitter->id())->position_fn_name(), "anchor");
    }

    TEST_F(NexusWiringTest, AMemberASourceLeavesEmptyStaysAsItWas)
    {
        const auto emitter = make_counting_emitter();
        const glm::quat turn(0.5F, 0.5F, 0.5F, 0.5F);
        emitter->set({ .orientation = turn });
        fabric.wire(emitter).place_from([] { return PlacementControls { .position = glm::vec3(1.0F) }; }).finalise();

        fabric.fire(emitter->id());

        EXPECT_EQ(emitter->position(), glm::vec3(1.0F));
        EXPECT_EQ(emitter->orientation(), turn);
    }

    TEST_F(NexusWiringTest, ASourceWinsOverMoveTo)
    {
        const auto emitter = make_counting_emitter();
        fabric.wire(emitter)
            .move_to(glm::vec3(1.0F, 0.0F, 0.0F), 0.01)
            .position_from([] { return glm::vec3(9.0F); })
            .finalise();

        run_samples(1000);

        EXPECT_EQ(fired, 1);
        EXPECT_EQ(last.position, glm::vec3(9.0F));
    }

    TEST_F(NexusWiringTest, CancelWiringStopsThePlacementSource)
    {
        int asked {};
        const auto emitter = make_counting_emitter();
        fabric.wire(emitter)
            .position_from([&asked] {
                ++asked;
                return glm::vec3(1.0F);
            })
            .finalise();

        fabric.cancel_wiring(emitter->id());
        fabric.fire(emitter->id());
        fabric.commit();

        EXPECT_EQ(fired, 1);
        EXPECT_EQ(asked, 0);
    }

    TEST_F(NexusWiringTest, MoveToChoreographsPositionsWithTheirDelays)
    {
        const auto emitter = make_counting_emitter();
        fabric.wire(emitter)
            .move_to(glm::vec3(1.0F, 0.0F, 0.0F), 0.01)
            .move_to(glm::vec3(2.0F, 0.0F, 0.0F), 0.02)
            .finalise();

        run_samples(300);
        EXPECT_EQ(fired, 0);

        run_samples(300);
        EXPECT_EQ(fired, 1);
        EXPECT_EQ(last.position, glm::vec3(1.0F, 0.0F, 0.0F));

        run_samples(1000);
        EXPECT_EQ(fired, 2);
        EXPECT_EQ(last.position, glm::vec3(2.0F, 0.0F, 0.0F));
        EXPECT_EQ(fabric.wiring_for(emitter->id())->move_steps().size(), 2U);
    }

    TEST_F(NexusWiringTest, TimesRepeatsAChoreography)
    {
        const auto emitter = make_counting_emitter();
        fabric.wire(emitter)
            .move_to(glm::vec3(1.0F, 0.0F, 0.0F), 0.01)
            .move_to(glm::vec3(2.0F, 0.0F, 0.0F), 0.02)
            .times(2)
            .finalise();

        run_samples(5000);

        EXPECT_EQ(fired, 4);
    }

    TEST_F(NexusWiringTest, BindFiresTheEntityOnceImmediately)
    {
        const auto emitter = make_counting_emitter();

        fabric.wire(emitter).bind().finalise();

        EXPECT_EQ(fired, 1);
        EXPECT_TRUE(fabric.wiring_for(emitter->id())->has_bind());

        fabric.commit();
        run_samples(k_interval_samples * 10);
        EXPECT_EQ(fired, 1);
    }

    TEST_F(NexusWiringTest, BindWithAFunctionRunsItInsteadOfTheEntity)
    {
        int custom {};
        const auto emitter = make_counting_emitter();

        fabric.wire(emitter).bind("poke", [&custom] { ++custom; }).finalise();

        EXPECT_EQ(custom, 1);
        EXPECT_EQ(fired, 0);
        EXPECT_EQ(fabric.wiring_for(emitter->id())->bind_attach_name(), "poke");
    }

    TEST_F(NexusWiringTest, BindDetachRunsAfterTheDuration)
    {
        int attached {};
        int detached {};
        const auto emitter = make_counting_emitter();

        fabric.wire(emitter)
            .bind("on", [&attached] { ++attached; }, "off", [&detached] { ++detached; })
            .for_duration(0.005)
            .finalise();

        EXPECT_EQ(attached, 1);
        run_samples(100);
        EXPECT_EQ(detached, 0);

        run_samples(400);
        EXPECT_EQ(detached, 1);

        run_samples(1000);
        EXPECT_EQ(detached, 1);

        const Wiring* wiring = fabric.wiring_for(emitter->id());
        EXPECT_TRUE(wiring->has_bind_detach());
        EXPECT_EQ(wiring->bind_attach_name(), "on");
        EXPECT_EQ(wiring->bind_detach_name(), "off");
    }

    TEST_F(NexusWiringTest, ADurationTimerDoesNotRunTheDetachAgainAfterCancelWiring)
    {
        int detached {};
        const auto emitter = make_counting_emitter();
        fabric.wire(emitter).bind([] { }, [&detached] { ++detached; }).for_duration(0.005).finalise();

        fabric.cancel_wiring(emitter->id());
        ASSERT_EQ(detached, 1);
        run_samples(1000);

        EXPECT_EQ(detached, 1);
    }

    TEST_F(NexusWiringTest, CancelWiringAfterTheDurationEndedDoesNotRunTheDetachAgain)
    {
        int detached {};
        const auto emitter = make_counting_emitter();
        fabric.wire(emitter).bind([] { }, [&detached] { ++detached; }).for_duration(0.005).finalise();
        run_samples(1000);
        ASSERT_EQ(detached, 1);

        fabric.cancel_wiring(emitter->id());

        EXPECT_EQ(detached, 1);
    }

    TEST_F(NexusWiringTest, RemovingAnEntityRunsTheBindDetachOnce)
    {
        int detached {};
        const auto emitter = make_counting_emitter();
        fabric.wire(emitter).bind([] { }, [&detached] { ++detached; }).finalise();

        fabric.remove(emitter->id());

        EXPECT_EQ(detached, 1);
    }

    TEST_F(NexusWiringTest, RemovingAnEntityBeforeTheDurationEndsRunsTheDetachOnlyOnce)
    {
        int detached {};
        const auto emitter = make_counting_emitter();
        fabric.wire(emitter).bind([] { }, [&detached] { ++detached; }).for_duration(0.005).finalise();

        fabric.remove(emitter->id());
        ASSERT_EQ(detached, 1);
        run_samples(1000);

        EXPECT_EQ(detached, 1);
    }

    TEST_F(NexusWiringTest, RemovingAnEntityAfterCancelWiringDoesNotRunTheDetachAgain)
    {
        int detached {};
        const auto emitter = make_counting_emitter();
        fabric.wire(emitter).bind([] { }, [&detached] { ++detached; }).finalise();
        fabric.cancel_wiring(emitter->id());

        fabric.remove(emitter->id());

        EXPECT_EQ(detached, 1);
    }

    TEST_F(NexusWiringTest, DestroyingTheFabricRunsTheBindDetach)
    {
        int detached {};
        const auto emitter = make_counting_emitter();
        {
            Fabric local(frames.scheduler, events, buffers, 1.0F);
            local.wire(emitter).bind([] { }, [&detached] { ++detached; }).finalise();
        }

        EXPECT_EQ(detached, 1);
    }

    class NexusWiringEndingTest : public NexusWiringTest {
    protected:
        std::function<void()> note_completion()
        {
            return [this] { order.emplace_back("complete"); };
        }

        std::function<void()> note_detach()
        {
            return [this] { order.emplace_back("detach"); };
        }

        const std::vector<std::string> ran_out { "complete", "detach" };
        const std::vector<std::string> taken_away { "detach" };
        std::vector<std::string> order;
    };

    TEST_F(NexusWiringEndingTest, UsingUpTimesCompletesThenDetachesOnce)
    {
        const auto emitter = make_counting_emitter();
        fabric.wire(emitter).every(0.001).times(3).on_complete(note_completion()).on_detach(note_detach()).finalise();

        step_samples(k_interval_samples * 10);
        EXPECT_EQ(order, ran_out);

        fabric.remove(emitter->id());
        run_samples(k_interval_samples);
        EXPECT_EQ(order, ran_out);
    }

    TEST_F(NexusWiringEndingTest, AnExpiredDurationCompletes)
    {
        fabric.wire(make_counting_emitter())
            .every(0.001)
            .for_duration(0.005)
            .on_complete(note_completion())
            .on_detach(note_detach())
            .finalise();

        step_samples(k_interval_samples * 30);

        EXPECT_EQ(order, ran_out);
    }

    TEST_F(NexusWiringEndingTest, ARepeatedChoreographyCompletesAfterItsLastStep)
    {
        fabric.wire(make_counting_emitter())
            .move_to(glm::vec3(1.0F), 0.01)
            .move_to(glm::vec3(2.0F), 0.01)
            .times(2)
            .on_complete(note_completion())
            .on_detach(note_detach())
            .finalise();

        run_samples(700);
        EXPECT_TRUE(order.empty());

        run_samples(5000);
        EXPECT_EQ(fired, 4);
        EXPECT_EQ(order, ran_out);
    }

    TEST_F(NexusWiringEndingTest, AReturningRoutineCompletes)
    {
        int runs {};
        fabric.wire(make_counting_emitter())
            .use([&runs]() { return counting_routine(&runs); })
            .on_complete(note_completion())
            .on_detach(note_detach())
            .finalise();

        run_samples(10);

        EXPECT_EQ(runs, 1);
        EXPECT_EQ(order, ran_out);
    }

    TEST_F(NexusWiringEndingTest, CancelWiringDetachesWithoutCompleting)
    {
        const auto emitter = make_counting_emitter();
        fabric.wire(emitter).every(0.001).on_complete(note_completion()).on_detach(note_detach()).finalise();
        run_samples(1);

        fabric.cancel_wiring(emitter->id());
        fabric.cancel_wiring(emitter->id());
        run_samples(k_interval_samples * 10);

        EXPECT_EQ(order, taken_away);
    }

    TEST_F(NexusWiringEndingTest, RemoveDetachesWithoutCompleting)
    {
        const auto emitter = make_counting_emitter();
        fabric.wire(emitter).every(0.001).times(50).on_complete(note_completion()).on_detach(note_detach()).finalise();
        run_samples(1);

        fabric.remove(emitter->id());
        run_samples(k_interval_samples * 100);

        EXPECT_EQ(order, taken_away);
    }

    TEST_F(NexusWiringEndingTest, DestroyingTheFabricDetachesWithoutCompleting)
    {
        {
            Fabric local(frames.scheduler, events, buffers, 1.0F);
            local.wire(make_counting_emitter()).on_complete(note_completion()).on_detach(note_detach()).finalise();
        }

        EXPECT_EQ(order, taken_away);
    }

    TEST_F(NexusWiringEndingTest, TimesOfOneOnEveryNeverCompletes)
    {
        fabric.wire(make_counting_emitter()).every(0.001).times(1).on_complete(note_completion()).finalise();

        step_samples(k_interval_samples * 5);

        EXPECT_TRUE(order.empty());
    }

    TEST_F(NexusWiringEndingTest, ABindDetachRunsBeforeOnDetach)
    {
        const auto emitter = make_counting_emitter();
        fabric.wire(emitter)
            .bind([] { }, [this] { order.emplace_back("bound"); })
            .on_detach(note_detach())
            .finalise();

        fabric.remove(emitter->id());

        EXPECT_EQ(order, (std::vector<std::string> { "bound", "detach" }));
    }

    class NexusWiringTriggerTest : public NexusWiringTest {
    protected:
        std::shared_ptr<ScriptedWindow> window = std::make_shared<ScriptedWindow>(400U, 300U);
    };

    TEST_F(NexusWiringTriggerTest, SeveralTriggersOnOneWiringReportWhichFired)
    {
        std::vector<int> seen;
        const auto emitter = std::make_shared<Emitter>([&seen](const InfluenceContext& ctx) {
            const auto* key = ctx.input ? std::get_if<Core::WindowEvent::KeyData>(&ctx.input->data) : nullptr;
            if (key) {
                seen.push_back(key->key);
            }
        });
        fabric.wire(emitter).on(window, IO::Keys::A).on(window, IO::Keys::D).finalise();

        window->key_down(IO::Keys::A);
        window->key_down(IO::Keys::S);
        window->key_down(IO::Keys::D);

        EXPECT_EQ(seen, (std::vector<int> { static_cast<int>(IO::Keys::A), static_cast<int>(IO::Keys::D) }));
    }

    TEST_F(NexusWiringTriggerTest, ATriggerCompletesWhenItsTimesAreUsedUp)
    {
        int completed {};
        int detached {};
        fabric.wire(make_counting_emitter())
            .on(window, IO::Keys::Space)
            .times(2)
            .on_complete([&completed] { ++completed; })
            .on_detach([&detached] { ++detached; })
            .finalise();

        window->key_down(IO::Keys::Space);
        EXPECT_EQ(completed, 0);

        window->key_down(IO::Keys::Space);
        window->key_down(IO::Keys::Space);
        EXPECT_EQ(completed, 1);
        EXPECT_EQ(detached, 1);
    }

    TEST_F(NexusWiringTriggerTest, TimesIsSharedAcrossTheTriggersOfOneWiring)
    {
        fabric.wire(make_counting_emitter()).on(window, IO::Keys::A).on(window, IO::Keys::D).times(3).finalise();

        window->key_down(IO::Keys::A);
        window->key_down(IO::Keys::D);
        window->key_down(IO::Keys::A);
        window->key_down(IO::Keys::D);
        window->key_down(IO::Keys::A);

        EXPECT_EQ(fired, 3);
    }

    TEST_F(NexusWiringTriggerTest, CancellingAWiringRemovesEveryTrigger)
    {
        const auto emitter = make_counting_emitter();
        fabric.wire(emitter).on(window, IO::Keys::A).on(window, IO::Keys::D).finalise();

        fabric.cancel_wiring(emitter->id());
        window->key_down(IO::Keys::A);
        window->key_down(IO::Keys::D);

        EXPECT_EQ(fired, 0);
    }

    TEST_F(NexusWiringTriggerTest, AKeyTriggerFiresOnEveryPressWithoutTimes)
    {
        fabric.wire(make_counting_emitter()).on(window, IO::Keys::Space).finalise();

        for (int i = 0; i < 5; ++i) {
            window->key_down(IO::Keys::Space);
        }

        EXPECT_EQ(fired, 5);
    }

    TEST_F(NexusWiringTriggerTest, AKeyTriggerWithModifiersFiresOnlyWithExactlyThoseHeld)
    {
        fabric.wire(make_counting_emitter()).on(window, IO::Keys::S | IO::Modifiers::Ctrl).finalise();

        window->key_down(IO::Keys::S);
        window->key_down(IO::Keys::S, IO::Modifiers::Shift);
        window->key_down(IO::Keys::S, IO::Modifiers::Ctrl | IO::Modifiers::Shift);
        EXPECT_EQ(fired, 0);

        window->key_down(IO::Keys::S, IO::Modifiers::Ctrl);
        EXPECT_EQ(fired, 1);
    }

    TEST_F(NexusWiringTriggerTest, ABareKeyTriggerStillFiresWithAnyModifiers)
    {
        fabric.wire(make_counting_emitter()).on(window, IO::Keys::S).finalise();

        window->key_down(IO::Keys::S);
        window->key_down(IO::Keys::S, IO::Modifiers::Ctrl | IO::Modifiers::Alt);

        EXPECT_EQ(fired, 2);
    }

    TEST_F(NexusWiringTriggerTest, AKeyTriggerReleaseCallbackMatchesTheKeyAlone)
    {
        int released = 0;
        fabric.wire(make_counting_emitter())
            .on(window, IO::Keys::S | IO::Modifiers::Ctrl, true, [&released] { ++released; })
            .finalise();

        window->key_down(IO::Keys::S, IO::Modifiers::Ctrl);
        window->key_up(IO::Keys::S);

        EXPECT_EQ(fired, 1);
        EXPECT_EQ(released, 1);
    }

    TEST_F(NexusWiringTriggerTest, AKeyTriggerFiresExactlyTimesThenStops)
    {
        fabric.wire(make_counting_emitter()).on(window, IO::Keys::Space).times(3).finalise();

        for (int i = 0; i < 6; ++i) {
            window->key_down(IO::Keys::Space);
        }

        EXPECT_EQ(fired, 3);
    }

    TEST_F(NexusWiringTriggerTest, AMouseTriggerFiresExactlyTimesThenStops)
    {
        fabric.wire(make_counting_emitter()).on(window, IO::MouseButtons::Left).times(2).finalise();

        for (int i = 0; i < 5; ++i) {
            window->press(IO::MouseButtons::Left);
        }

        EXPECT_EQ(fired, 2);
    }

    TEST_F(NexusWiringTriggerTest, AScrollTriggerCallsItsFunctionExactlyTimesThenStops)
    {
        int scrolled {};
        fabric.wire(make_counting_emitter())
            .on_scroll(window, [&scrolled](double, double) { ++scrolled; })
            .times(2)
            .finalise();

        for (int i = 0; i < 5; ++i) {
            window->scroll(0.0, 1.0);
        }

        EXPECT_EQ(scrolled, 2);
    }

    TEST_F(NexusWiringTriggerTest, AWindowEventTriggerFiresExactlyTimesThenStops)
    {
        Vruta::WindowEventFilter filter;
        filter.event_type = Core::WindowEventType::MOUSE_SCROLLED;
        fabric.wire(make_counting_emitter()).on(window, filter).times(2).finalise();

        for (int i = 0; i < 5; ++i) {
            window->scroll(0.0, 1.0);
        }

        EXPECT_EQ(fired, 2);
    }

    TEST_F(NexusWiringTriggerTest, AHeldKeyTriggerWithModifiersFiresOnPressAndRepeatWhileThoseAreHeld)
    {
        fabric.wire(make_counting_emitter()).on(window, IO::Keys::S | IO::Modifiers::Ctrl, true).finalise();

        window->key_down(IO::Keys::S, IO::Modifiers::Ctrl);
        window->key_repeat(IO::Keys::S, IO::Modifiers::Ctrl);
        EXPECT_EQ(fired, 2);

        window->key_repeat(IO::Keys::S);
        window->key_repeat(IO::Keys::S, IO::Modifiers::Ctrl | IO::Modifiers::Shift);
        EXPECT_EQ(fired, 2);
    }

    TEST_F(NexusWiringTriggerTest, AKeyTriggerWithModifiersCountsOnlyMatchingPressesTowardTimes)
    {
        fabric.wire(make_counting_emitter()).on(window, IO::Keys::S | IO::Modifiers::Ctrl).times(2).finalise();

        window->key_down(IO::Keys::S);
        window->key_down(IO::Keys::S, IO::Modifiers::Ctrl);
        window->key_down(IO::Keys::S, IO::Modifiers::Shift);
        window->key_down(IO::Keys::S, IO::Modifiers::Ctrl);
        window->key_down(IO::Keys::S, IO::Modifiers::Ctrl);

        EXPECT_EQ(fired, 2);
    }

    TEST_F(NexusWiringTriggerTest, AWindowEventTriggerFiltersByModifiers)
    {
        Vruta::WindowEventFilter filter { IO::Keys::S | IO::Modifiers::Alt };
        filter.event_type = Core::WindowEventType::KEY_PRESSED;
        fabric.wire(make_counting_emitter()).on(window, filter).finalise();

        window->key_down(IO::Keys::S);
        window->key_down(IO::Keys::S, IO::Modifiers::Ctrl);
        EXPECT_EQ(fired, 0);

        window->key_down(IO::Keys::S, IO::Modifiers::Alt);
        EXPECT_EQ(fired, 1);
    }

    TEST_F(NexusWiringTriggerTest, ALimitedTriggerLeavesAnotherWiringOnTheSameWindowRunning)
    {
        int other {};
        fabric.wire(make_counting_emitter()).on(window, IO::Keys::Space).times(2).finalise();
        fabric.wire(std::make_shared<Emitter>([&other](const InfluenceContext&) { ++other; }))
            .on(window, IO::Keys::Space)
            .finalise();

        for (int i = 0; i < 5; ++i) {
            window->key_down(IO::Keys::Space);
        }

        EXPECT_EQ(fired, 2);
        EXPECT_EQ(other, 5);
    }

    TEST_F(NexusWiringTriggerTest, ABroadcastTriggerFiresOnEverySignalAndStopsWhenCancelled)
    {
        const auto source = std::make_shared<Vruta::BroadcastSource<int>>();
        const auto emitter = make_counting_emitter();
        fabric.wire(emitter).on(source).finalise();

        source->signal(1);
        source->signal(2);
        EXPECT_EQ(fired, 2);

        fabric.cancel_wiring(emitter->id());
        source->signal(3);
        EXPECT_EQ(fired, 2);
    }

    TEST_F(NexusWiringTriggerTest, ABroadcastTriggerFiresExactlyTimesThenStops)
    {
        const auto source = std::make_shared<Vruta::BroadcastSource<int>>();
        fabric.wire(make_counting_emitter()).on(source).times(2).finalise();

        for (int i = 0; i < 5; ++i) {
            source->signal(i);
        }

        EXPECT_EQ(fired, 2);
    }

    TEST_F(NexusWiringTriggerTest, ACourseEventFiresTheEntityWhenTheCourseFinishes)
    {
        Kriya::CourseSetting setting;
        setting.in = 0.0;
        setting.out = 0.1;
        setting.edge = Kriya::CourseEdge::Once;
        const auto course = Kriya::Course::create(10.0, setting);
        ASSERT_NE(course, nullptr);
        fabric.wire(make_counting_emitter()).on(course->subscribe(Kriya::CourseEventKind::Finished)).finalise();

        for (int i = 0; i < 20; ++i) {
            (void)course->advance();
        }

        EXPECT_TRUE(course->finished());
        EXPECT_EQ(fired, 1);
    }

    TEST_F(NexusWiringTriggerTest, TimesOfOneIsNotALimitOnATrigger)
    {
        fabric.wire(make_counting_emitter()).on(window, IO::Keys::Space).times(1).finalise();

        for (int i = 0; i < 4; ++i) {
            window->key_down(IO::Keys::Space);
        }

        EXPECT_EQ(fired, 4);
    }

    class NexusWiringAudioTokenTest : public NexusWiringTest {
    protected:
        void SetUp() override
        {
            LogCapture::install();
            LogCapture::clear();
        }

        std::shared_ptr<Nexus::Locus> make_locus()
        {
            Kinesis::NavigationConfig config;
            config.initial_eye = { 0.0F, 0.0F, 5.0F };
            config.initial_target = { 0.0F, 0.0F, 0.0F };
            return std::make_shared<Nexus::Locus>(
                config, 1.0F,
                [](const PerceptionContext&) { },
                [this](const InfluenceContext&) { ++fired; });
        }

        static constexpr const char* k_warning = "runs on the audio token";
    };

    TEST_F(NexusWiringAudioTokenTest, ALocusDrivenOnTheAudioTokenWarns)
    {
        fabric.wire(make_locus()).every(0.01).finalise();

        EXPECT_TRUE(LogCapture::contains(k_warning));
    }

    TEST_F(NexusWiringAudioTokenTest, ALocusDrivenOnTheGraphicsTokenDoesNotWarn)
    {
        fabric.wire(make_locus()).every(0.01, Vruta::ProcessingToken::FRAME_ACCURATE).finalise();

        EXPECT_FALSE(LogCapture::contains(k_warning));
    }

    TEST_F(NexusWiringAudioTokenTest, ALocusWithoutAnIntervalDoesNotWarn)
    {
        fabric.wire(make_locus()).finalise();

        EXPECT_FALSE(LogCapture::contains(k_warning));
    }

    TEST_F(NexusWiringAudioTokenTest, AnEmitterWithoutRenderSinksOnTheAudioTokenDoesNotWarn)
    {
        fabric.wire(make_counting_emitter()).every(0.01).finalise();

        EXPECT_FALSE(LogCapture::contains(k_warning));
    }

    TEST_F(NexusWiringAudioTokenTest, AnAgentThatIsNotALocusOnTheAudioTokenDoesNotWarn)
    {
        fabric.wire(std::make_shared<Agent>(1.0F)).every(0.01).finalise();

        EXPECT_FALSE(LogCapture::contains(k_warning));
    }

    TEST_F(NexusWiringAudioTokenTest, TheWarningNamesTheEntity)
    {
        const auto locus = make_locus();
        fabric.wire(locus).every(0.01).finalise();

        EXPECT_TRUE(LogCapture::contains("Entity " + std::to_string(locus->id())));
    }

    TEST_F(NexusWiringAudioTokenTest, TheWiringStillRunsAfterTheWarning)
    {
        const auto locus = make_locus();
        fabric.wire(locus).every(0.001).finalise();

        run_samples(1);

        EXPECT_NE(fabric.wiring_for(locus->id()), nullptr);
        EXPECT_EQ(fired, 1);
    }

    TEST_F(NexusWiringTest, BindWithADurationButNoDetachSchedulesNothing)
    {
        int attached {};
        const auto emitter = make_counting_emitter();

        fabric.wire(emitter).bind([&attached] { ++attached; }).for_duration(0.005).finalise();
        run_samples(1000);

        EXPECT_EQ(attached, 1);
        EXPECT_FALSE(fabric.wiring_for(emitter->id())->has_bind_detach());
    }

    TEST_F(NexusWiringTest, UseRunsTheSuppliedRoutineAndRecordsItsName)
    {
        int runs {};
        const auto emitter = make_counting_emitter();

        fabric.wire(emitter).use("custom", [&runs]() { return counting_routine(&runs); }).finalise();
        run_samples(1);

        EXPECT_EQ(runs, 1);
        EXPECT_EQ(fabric.wiring_for(emitter->id())->factory_name(), "custom");
        EXPECT_FALSE(std::holds_alternative<std::monostate>(fabric.wiring_for(emitter->id())->factory()));
    }

    TEST_F(NexusWiringTest, UseDoesNotFireTheEntityOnItsOwn)
    {
        int runs {};
        const auto emitter = make_counting_emitter();

        fabric.wire(emitter).use([&runs]() { return counting_routine(&runs); }).finalise();
        run_samples(1);
        fabric.commit();

        EXPECT_EQ(fired, 0);
    }

    TEST_F(NexusWiringTest, FinaliseRegistersNamedInfluenceAndPerceptionFunctions)
    {
        int influence_calls {};
        int perception_calls {};
        auto emitter = std::make_shared<Emitter>("pulse", [&influence_calls](const InfluenceContext&) { ++influence_calls; });
        auto sensor = std::make_shared<Sensor>(1.0F, "watch", [&perception_calls](const PerceptionContext&) { ++perception_calls; });
        auto agent = std::make_shared<Agent>(1.0F,
            "see", [&perception_calls](const PerceptionContext&) { ++perception_calls; },
            "act", [&influence_calls](const InfluenceContext&) { ++influence_calls; });

        fabric.wire(emitter).finalise();
        fabric.wire(sensor).finalise();
        fabric.wire(agent).finalise();

        const auto pulse = fabric.resolve_influence_fn("pulse");
        const auto act = fabric.resolve_influence_fn("act");
        const auto watch = fabric.resolve_perception_fn("watch");
        const auto see = fabric.resolve_perception_fn("see");
        ASSERT_NE(pulse, nullptr);
        ASSERT_NE(act, nullptr);
        ASSERT_NE(watch, nullptr);
        ASSERT_NE(see, nullptr);

        (*pulse)({});
        (*act)({});
        (*watch)({});
        (*see)({});

        EXPECT_EQ(influence_calls, 2);
        EXPECT_EQ(perception_calls, 2);
    }

    TEST_F(NexusWiringTest, AnonymousFunctionsAreNotRegistered)
    {
        fabric.wire(make_counting_emitter()).finalise();

        EXPECT_EQ(fabric.resolve_influence_fn(""), nullptr);
        EXPECT_EQ(fabric.resolve_influence_fn("missing"), nullptr);
        EXPECT_EQ(fabric.resolve_perception_fn("missing"), nullptr);
    }

    TEST_F(NexusWiringTest, TheFirstFunctionRegisteredUnderANameWins)
    {
        int first {};
        int second {};
        fabric.wire(std::make_shared<Emitter>("same", [&first](const InfluenceContext&) { ++first; })).finalise();
        fabric.wire(std::make_shared<Emitter>("same", [&second](const InfluenceContext&) { ++second; })).finalise();

        (*fabric.resolve_influence_fn("same"))({});

        EXPECT_EQ(first, 1);
        EXPECT_EQ(second, 0);
    }

    TEST_F(NexusWiringTest, NamesAreRegisteredEvenWhenTheWiringSchedulesNothing)
    {
        auto emitter = std::make_shared<Emitter>("quiet", [](const InfluenceContext&) { });

        fabric.wire(emitter).every(0.5).finalise();

        EXPECT_NE(fabric.resolve_influence_fn("quiet"), nullptr);
    }

}
}
