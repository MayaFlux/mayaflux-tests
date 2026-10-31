#include "gtest/gtest.h"

#include "MayaFlux/Kriya/Awaiters/ConditionAwaiter.hpp"
#include "MayaFlux/Kriya/Awaiters/DelayAwaiters.hpp"
#include "MayaFlux/Kriya/Awaiters/GetPromise.hpp"
#include "MayaFlux/Vruta/Routine.hpp"

#include <stdexcept>

namespace MayaFlux::Test {

namespace {

    constexpr uint64_t DUE = 10U;

    enum class Kind : uint8_t {
        Sound,
        Graphics,
        Cross,
        Free
    };

    std::string kind_label(Kind kind)
    {
        switch (kind) {
        case Kind::Sound:
            return "Sound";
        case Kind::Graphics:
            return "Graphics";
        case Kind::Cross:
            return "Cross";
        case Kind::Free:
            return "Free";
        }
        return "Unknown";
    }

    Vruta::ProcessingToken expected_token(Kind kind)
    {
        switch (kind) {
        case Kind::Sound:
            return Vruta::ProcessingToken::SAMPLE_ACCURATE;
        case Kind::Graphics:
            return Vruta::ProcessingToken::FRAME_ACCURATE;
        case Kind::Cross:
            return Vruta::ProcessingToken::MULTI_RATE;
        case Kind::Free:
            return Vruta::ProcessingToken::CONDITIONAL;
        }
        return Vruta::ProcessingToken::ON_DEMAND;
    }

    struct Probe {
        int ticks = 0;
        int destroyed = 0;
        bool ready = false;
    };

    struct Guard {
        Probe* probe;

        ~Guard() { ++probe->destroyed; }
    };

    template <typename Promise>
    struct Suspend {
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<Promise>) const noexcept { }
        void await_resume() const noexcept { }
    };

    template <typename Promise>
    struct Capture {
        Promise** slot;

        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<Promise> h) const noexcept { *slot = &h.promise(); }
        void await_resume() const noexcept { }
    };

    Vruta::SoundRoutine sound_once(Probe* p)
    {
        Guard guard { p };
        ++p->ticks;
        co_await Kriya::SampleDelay { DUE };
        ++p->ticks;
    }

    Vruta::SoundRoutine sound_loop(Probe* p)
    {
        Guard guard { p };
        for (;;) {
            ++p->ticks;
            co_await Kriya::SampleDelay { DUE };
        }
    }

    Vruta::SoundRoutine sound_bare(Probe* p)
    {
        Guard guard { p };
        ++p->ticks;
        co_await Suspend<Vruta::audio_promise> {};
        ++p->ticks;
    }

    Vruta::SoundRoutine sound_await(Probe* p)
    {
        Guard guard { p };
        ++p->ticks;
        [[maybe_unused]] auto& promise = co_await Kriya::GetAudioPromise {};
        ++p->ticks;
        co_await Kriya::SampleDelay { DUE };
    }

    Vruta::SoundRoutine sound_buffer(Probe* p)
    {
        Guard guard { p };
        for (;;) {
            ++p->ticks;
            co_await Kriya::BufferDelay { 2 };
        }
    }

    Vruta::SoundRoutine sound_capture(Vruta::audio_promise** slot, Probe* p)
    {
        Guard guard { p };
        co_await Capture<Vruta::audio_promise> { slot };
    }

    Vruta::GraphicsRoutine graphics_once(Probe* p)
    {
        Guard guard { p };
        ++p->ticks;
        co_await Kriya::FrameDelay { .frames_to_wait = DUE };
        ++p->ticks;
    }

    Vruta::GraphicsRoutine graphics_loop(Probe* p)
    {
        Guard guard { p };
        for (;;) {
            ++p->ticks;
            co_await Kriya::FrameDelay { .frames_to_wait = 3 };
        }
    }

    Vruta::GraphicsRoutine graphics_bare(Probe* p)
    {
        Guard guard { p };
        ++p->ticks;
        co_await Suspend<Vruta::graphics_promise> {};
        ++p->ticks;
    }

    Vruta::GraphicsRoutine graphics_capture(Vruta::graphics_promise** slot, Probe* p)
    {
        Guard guard { p };
        co_await Capture<Vruta::graphics_promise> { slot };
    }

    Vruta::CrossRoutine cross_once(Probe* p)
    {
        Guard guard { p };
        ++p->ticks;
        co_await Kriya::MultiRateDelay { DUE, 0 };
        ++p->ticks;
    }

    Vruta::CrossRoutine cross_loop(Probe* p)
    {
        Guard guard { p };
        for (;;) {
            ++p->ticks;
            co_await Kriya::MultiRateDelay { DUE, 0 };
        }
    }

    Vruta::CrossRoutine cross_both(Probe* p)
    {
        Guard guard { p };
        ++p->ticks;
        co_await Kriya::MultiRateDelay { DUE, 5 };
        ++p->ticks;
    }

    Vruta::CrossRoutine cross_frame_once(Probe* p)
    {
        Guard guard { p };
        ++p->ticks;
        co_await Kriya::MultiRateDelay { 0, 1 };
        ++p->ticks;
    }

    Vruta::CrossRoutine cross_bare(Probe* p)
    {
        Guard guard { p };
        ++p->ticks;
        co_await Suspend<Vruta::cross_promise> {};
        ++p->ticks;
    }

    Vruta::CrossRoutine cross_await(Probe* p)
    {
        Guard guard { p };
        ++p->ticks;
        [[maybe_unused]] auto& promise = co_await Kriya::GetCrossPromise {};
        ++p->ticks;
    }

    Vruta::CrossRoutine cross_capture(Vruta::cross_promise** slot, Probe* p)
    {
        Guard guard { p };
        co_await Capture<Vruta::cross_promise> { slot };
    }

    Vruta::FreeRoutine free_once(Probe* p)
    {
        Guard guard { p };
        ++p->ticks;
        co_await Kriya::ConditionAwaiter { [p] { return p->ready; } };
        ++p->ticks;
    }

    Vruta::FreeRoutine free_loop(Probe* p)
    {
        Guard guard { p };
        for (;;) {
            ++p->ticks;
            co_await Kriya::ConditionAwaiter { [p] { return p->ready; } };
            p->ready = false;
        }
    }

    Vruta::FreeRoutine free_capture(Vruta::conditional_promise** slot, Probe* p)
    {
        Guard guard { p };
        co_await Capture<Vruta::conditional_promise> { slot };
    }

    std::shared_ptr<Vruta::Routine> make_once(Kind kind, Probe* p)
    {
        switch (kind) {
        case Kind::Sound:
            return std::make_shared<Vruta::SoundRoutine>(sound_once(p));
        case Kind::Graphics:
            return std::make_shared<Vruta::GraphicsRoutine>(graphics_once(p));
        case Kind::Cross:
            return std::make_shared<Vruta::CrossRoutine>(cross_once(p));
        case Kind::Free:
            return std::make_shared<Vruta::FreeRoutine>(free_once(p));
        }
        return nullptr;
    }

    bool release(Kind kind, Probe& probe, Vruta::Routine& routine)
    {
        if (kind == Kind::Free) {
            probe.ready = true;
            return routine.try_resume(0U);
        }
        return routine.try_resume(DUE);
    }

} // namespace

class RoutineContractTest : public ::testing::TestWithParam<Kind> { };

TEST_P(RoutineContractTest, RunsToItsFirstSuspensionOnCreation)
{
    Probe probe;
    auto routine = make_once(GetParam(), &probe);

    EXPECT_EQ(probe.ticks, 1);
    EXPECT_TRUE(routine->is_active());
}

TEST_P(RoutineContractTest, ReportsTheProcessingTokenOfItsDomain)
{
    Probe probe;
    auto routine = make_once(GetParam(), &probe);

    EXPECT_EQ(routine->get_processing_token(), expected_token(GetParam()));
}

TEST_P(RoutineContractTest, ReleaseCompletesOnceAndNotifiesOnce)
{
    Probe probe;
    int completions = 0;
    auto routine = make_once(GetParam(), &probe);
    routine->on_complete([&completions] { ++completions; });

    EXPECT_TRUE(release(GetParam(), probe, *routine));
    EXPECT_EQ(probe.ticks, 2);
    EXPECT_FALSE(routine->is_active());
    EXPECT_EQ(completions, 1);

    EXPECT_FALSE(release(GetParam(), probe, *routine));
    EXPECT_EQ(completions, 1);
}

TEST_P(RoutineContractTest, CallbackRegisteredAfterCompletionRunsImmediately)
{
    Probe probe;
    int completions = 0;
    auto routine = make_once(GetParam(), &probe);
    ASSERT_TRUE(release(GetParam(), probe, *routine));

    routine->on_complete([&completions] { ++completions; });

    EXPECT_EQ(completions, 1);
}

TEST_P(RoutineContractTest, TerminationRequestNotifiesWhileStillActiveAndBlocksResume)
{
    Probe probe;
    int completions = 0;
    auto routine = make_once(GetParam(), &probe);
    routine->on_complete([&completions] { ++completions; });

    routine->set_should_terminate(true);

    EXPECT_TRUE(routine->get_should_terminate());
    EXPECT_EQ(completions, 1);
    EXPECT_TRUE(routine->is_active());
    EXPECT_FALSE(release(GetParam(), probe, *routine));
    EXPECT_EQ(probe.ticks, 1);

    routine->set_should_terminate(true);
    EXPECT_EQ(completions, 1);
}

TEST_P(RoutineContractTest, AutoResumeFlagGatesResume)
{
    Probe probe;
    auto routine = make_once(GetParam(), &probe);

    routine->set_auto_resume(false);
    EXPECT_FALSE(routine->get_auto_resume());
    EXPECT_FALSE(release(GetParam(), probe, *routine));
    EXPECT_EQ(probe.ticks, 1);

    routine->set_auto_resume(true);
    EXPECT_TRUE(release(GetParam(), probe, *routine));
    EXPECT_EQ(probe.ticks, 2);
}

TEST_P(RoutineContractTest, ForceResumeBypassesGatesAndStopsOnceFinished)
{
    Probe probe;
    int completions = 0;
    auto routine = make_once(GetParam(), &probe);
    routine->on_complete([&completions] { ++completions; });

    routine->set_auto_resume(false);
    EXPECT_TRUE(routine->force_resume());
    EXPECT_EQ(probe.ticks, 2);
    EXPECT_FALSE(routine->is_active());
    EXPECT_EQ(completions, 1);

    EXPECT_FALSE(routine->force_resume());
}

TEST_P(RoutineContractTest, RestartRearmsAndResumesASuspendedRoutine)
{
    Probe probe;
    auto routine = make_once(GetParam(), &probe);

    routine->set_auto_resume(false);
    EXPECT_TRUE(routine->restart());
    EXPECT_TRUE(routine->get_auto_resume());
    const bool* flag = routine->get_state<bool>("restart");
    ASSERT_NE(flag, nullptr);
    EXPECT_TRUE(*flag);
    EXPECT_EQ(probe.ticks, 2);

    EXPECT_FALSE(routine->restart());
}

TEST_P(RoutineContractTest, StateDictionaryStoresTypedValues)
{
    Probe probe;
    auto routine = make_once(GetParam(), &probe);

    routine->set_state("count", 7);
    routine->update_params("scale", 2.5, "label", std::string("wave"));

    const int* count = routine->get_state<int>("count");
    ASSERT_NE(count, nullptr);
    EXPECT_EQ(*count, 7);
    const double* scale = routine->get_state<double>("scale");
    ASSERT_NE(scale, nullptr);
    EXPECT_EQ(*scale, 2.5);
    const std::string* label = routine->get_state<std::string>("label");
    ASSERT_NE(label, nullptr);
    EXPECT_EQ(*label, "wave");

    EXPECT_EQ(routine->get_state<float>("count"), nullptr);
    EXPECT_EQ(routine->get_state<int>("missing"), nullptr);
}

TEST_P(RoutineContractTest, DestroyingTheRoutineDestroysItsFrame)
{
    Probe probe;
    int completions = 0;
    {
        auto routine = make_once(GetParam(), &probe);
        routine->on_complete([&completions] { ++completions; });
    }

    EXPECT_EQ(probe.destroyed, 1);
    EXPECT_EQ(completions, 0);
}

TEST_P(RoutineContractTest, NullHandleIsRejectedAtConstruction)
{
    switch (GetParam()) {
    case Kind::Sound:
        EXPECT_THROW(static_cast<void>(Vruta::SoundRoutine { std::coroutine_handle<Vruta::audio_promise> {} }), std::invalid_argument);
        break;
    case Kind::Graphics:
        EXPECT_THROW(static_cast<void>(Vruta::GraphicsRoutine { std::coroutine_handle<Vruta::graphics_promise> {} }), std::invalid_argument);
        break;
    case Kind::Cross:
        EXPECT_THROW(static_cast<void>(Vruta::CrossRoutine { std::coroutine_handle<Vruta::cross_promise> {} }), std::invalid_argument);
        break;
    case Kind::Free:
        EXPECT_THROW(static_cast<void>(Vruta::FreeRoutine { std::coroutine_handle<Vruta::conditional_promise> {} }), std::invalid_argument);
        break;
    }
}

INSTANTIATE_TEST_SUITE_P(
    RoutineKinds,
    RoutineContractTest,
    ::testing::Values(Kind::Sound, Kind::Graphics, Kind::Cross, Kind::Free),
    [](const ::testing::TestParamInfo<Kind>& info) { return kind_label(info.param); });

TEST(SoundRoutineTest, PromiseHoldsItsDefaults)
{
    Probe probe;
    Vruta::audio_promise* promise = nullptr;
    Vruta::SoundRoutine routine(sound_capture(&promise, &probe));

    ASSERT_NE(promise, nullptr);
    EXPECT_EQ(promise->processing_token, Vruta::ProcessingToken::SAMPLE_ACCURATE);
    EXPECT_TRUE(promise->sync_to_clock);
    EXPECT_TRUE(promise->auto_resume);
    EXPECT_FALSE(promise->should_terminate.load());
    EXPECT_EQ(promise->next_sample, 0U);
    EXPECT_EQ(promise->next_buffer_cycle, 0U);
    EXPECT_EQ(promise->delay_amount, 0U);
    EXPECT_EQ(promise->active_delay_context, Vruta::DelayContext::NONE);
    EXPECT_FALSE(promise->completion_registered.load());
}

TEST(SoundRoutineTest, TryResumeWaitsForTheDueSampleThenAdvancesByTheDelay)
{
    Probe probe;
    Vruta::SoundRoutine routine(sound_loop(&probe));

    EXPECT_EQ(routine.next_execution(), DUE);
    EXPECT_FALSE(routine.try_resume(DUE - 1));
    EXPECT_EQ(probe.ticks, 1);

    EXPECT_TRUE(routine.try_resume(DUE));
    EXPECT_EQ(probe.ticks, 2);
    EXPECT_EQ(routine.next_execution(), 2U * DUE);
}

TEST(SoundRoutineTest, OnlyTheSampleContextResumesASampleDelay)
{
    Probe probe;
    Vruta::SoundRoutine routine(sound_loop(&probe));

    EXPECT_FALSE(routine.try_resume_with_context(100, Vruta::DelayContext::BUFFER_BASED));
    EXPECT_FALSE(routine.try_resume_with_context(100, Vruta::DelayContext::NONE));
    EXPECT_EQ(probe.ticks, 1);
}

TEST(SoundRoutineTest, UndelayedSuspensionResumesOnlyUnderTheNoneContext)
{
    Probe probe;
    Vruta::SoundRoutine routine(sound_bare(&probe));

    EXPECT_FALSE(routine.try_resume(100));
    EXPECT_EQ(probe.ticks, 1);

    EXPECT_TRUE(routine.try_resume_with_context(0, Vruta::DelayContext::NONE));
    EXPECT_EQ(probe.ticks, 2);
    EXPECT_FALSE(routine.is_active());
}

TEST(SoundRoutineTest, AwaitStateInitialisesTheRoutineOnTheNextResume)
{
    Probe probe;
    Vruta::SoundRoutine routine(sound_await(&probe));

    EXPECT_TRUE(routine.try_resume_with_context(7, Vruta::DelayContext::SAMPLE_BASED));
    EXPECT_EQ(probe.ticks, 2);
    EXPECT_EQ(routine.next_execution(), 7U + DUE);
    EXPECT_EQ(routine.get_delay_context(), Vruta::DelayContext::SAMPLE_BASED);
}

TEST(SoundRoutineTest, InitializeStateSetsTheStartAndRunsToTheNextSuspension)
{
    Probe probe;
    Vruta::SoundRoutine routine(sound_loop(&probe));

    EXPECT_TRUE(routine.initialize_state(50));
    EXPECT_EQ(probe.ticks, 2);
    EXPECT_EQ(routine.next_execution(), 50U + DUE);
}

TEST(SoundRoutineTest, BufferDelayAdvancesTheBufferCycleAndResumesOnIt)
{
    Probe probe;
    Vruta::SoundRoutine routine(sound_buffer(&probe));

    EXPECT_EQ(routine.get_next_frame(), 2U);
    EXPECT_FALSE(routine.try_resume_with_context(1, Vruta::DelayContext::BUFFER_BASED));
    EXPECT_TRUE(routine.try_resume_with_context(2, Vruta::DelayContext::BUFFER_BASED));
    EXPECT_EQ(probe.ticks, 2);
    EXPECT_EQ(routine.get_next_frame(), 4U);
    EXPECT_EQ(routine.get_delay_context(), Vruta::DelayContext::BUFFER_BASED);
}

TEST(SoundRoutineTest, SampleAndFrameAccessorsWriteTheirFields)
{
    Probe probe;
    Vruta::SoundRoutine routine(sound_loop(&probe));

    routine.set_next_sample(500);
    EXPECT_EQ(routine.get_next_sample(), 500U);
    EXPECT_EQ(routine.next_execution(), 500U);

    routine.set_next_frame(42);
    EXPECT_EQ(routine.get_next_frame(), 42U);
}

TEST(SoundRoutineTest, MoveTransfersTheFrameAndMoveAssignmentReleasesTheOldOne)
{
    Probe first;
    Probe second;
    Vruta::SoundRoutine source(sound_loop(&first));
    Vruta::SoundRoutine target(std::move(source));

    EXPECT_FALSE(source.is_active());
    EXPECT_EQ(source.next_execution(), UINT64_MAX);
    EXPECT_TRUE(target.is_active());
    EXPECT_EQ(target.next_execution(), DUE);
    EXPECT_EQ(first.destroyed, 0);

    Vruta::SoundRoutine replacement(sound_loop(&second));
    target = std::move(replacement);

    EXPECT_EQ(first.destroyed, 1);
    EXPECT_TRUE(target.is_active());
}

TEST(GraphicsRoutineTest, PromiseHoldsItsDefaults)
{
    Probe probe;
    Vruta::graphics_promise* promise = nullptr;
    Vruta::GraphicsRoutine routine(graphics_capture(&promise, &probe));

    ASSERT_NE(promise, nullptr);
    EXPECT_EQ(promise->processing_token, Vruta::ProcessingToken::FRAME_ACCURATE);
    EXPECT_TRUE(promise->sync_to_clock);
    EXPECT_TRUE(promise->auto_resume);
    EXPECT_EQ(promise->next_frame, 0U);
    EXPECT_EQ(promise->delay_amount, 0U);
    EXPECT_EQ(promise->active_delay_context, Vruta::DelayContext::NONE);
}

TEST(GraphicsRoutineTest, TokenAndSampleAccessorsAreFixed)
{
    Probe probe;
    Vruta::GraphicsRoutine routine(graphics_loop(&probe));

    EXPECT_EQ(routine.get_processing_token(), Vruta::ProcessingToken::FRAME_ACCURATE);
    EXPECT_TRUE(routine.requires_clock_sync());
    EXPECT_TRUE(routine.get_sync_to_clock());

    routine.set_next_sample(99);
    EXPECT_EQ(routine.get_next_sample(), 0U);
}

TEST(GraphicsRoutineTest, FrameGateAdvancesByTheDelay)
{
    Probe probe;
    Vruta::GraphicsRoutine routine(graphics_loop(&probe));

    EXPECT_EQ(routine.next_execution(), 3U);
    EXPECT_FALSE(routine.try_resume(2));
    EXPECT_EQ(probe.ticks, 1);

    EXPECT_TRUE(routine.try_resume(3));
    EXPECT_EQ(probe.ticks, 2);
    EXPECT_EQ(routine.next_execution(), 6U);
}

TEST(GraphicsRoutineTest, OnlyTheFrameContextResumesAFrameDelay)
{
    Probe probe;
    Vruta::GraphicsRoutine routine(graphics_loop(&probe));

    EXPECT_FALSE(routine.try_resume_with_context(100, Vruta::DelayContext::SAMPLE_BASED));
    EXPECT_FALSE(routine.try_resume_with_context(100, Vruta::DelayContext::BUFFER_BASED));
    EXPECT_FALSE(routine.try_resume_with_context(100, Vruta::DelayContext::NONE));
    EXPECT_EQ(probe.ticks, 1);
}

TEST(GraphicsRoutineTest, UndelayedSuspensionResumesOnlyUnderTheNoneContext)
{
    Probe probe;
    Vruta::GraphicsRoutine routine(graphics_bare(&probe));

    EXPECT_FALSE(routine.try_resume(0));
    EXPECT_TRUE(routine.try_resume_with_context(0, Vruta::DelayContext::NONE));
    EXPECT_EQ(probe.ticks, 2);
}

TEST(GraphicsRoutineTest, InitializeStateSetsTheFrameAndRunsToTheNextSuspension)
{
    Probe probe;
    Vruta::GraphicsRoutine routine(graphics_loop(&probe));

    EXPECT_TRUE(routine.initialize_state(30));
    EXPECT_EQ(probe.ticks, 2);
    EXPECT_EQ(routine.next_execution(), 33U);
}

TEST(GraphicsRoutineTest, MoveConstructorTransfersTheFrame)
{
    Probe probe;
    Vruta::GraphicsRoutine source(graphics_loop(&probe));
    Vruta::GraphicsRoutine target(std::move(source));

    EXPECT_FALSE(source.is_active());
    EXPECT_TRUE(target.is_active());
    EXPECT_EQ(target.next_execution(), 3U);
}

TEST(SoundRoutineTest, MovedFromRoutineReportsTerminatedAndIgnoresSetTerminate)
{
    Probe probe;
    Vruta::SoundRoutine source(sound_loop(&probe));
    Vruta::SoundRoutine target(std::move(source));

    EXPECT_TRUE(source.get_should_terminate());
    source.set_should_terminate(false);
    EXPECT_TRUE(source.get_should_terminate());
    EXPECT_FALSE(target.get_should_terminate());
    EXPECT_TRUE(target.is_active());
}

TEST(GraphicsRoutineTest, MovedFromRoutineReportsTerminatedAndIgnoresSetTerminate)
{
    Probe probe;
    Vruta::GraphicsRoutine source(graphics_loop(&probe));
    Vruta::GraphicsRoutine target(std::move(source));

    EXPECT_TRUE(source.get_should_terminate());
    source.set_should_terminate(false);
    EXPECT_TRUE(source.get_should_terminate());
    EXPECT_FALSE(target.get_should_terminate());
    EXPECT_TRUE(target.is_active());
}

TEST(CrossRoutineTest, MovedFromRoutineReportsTerminatedAndIgnoresSetTerminate)
{
    Probe probe;
    Vruta::CrossRoutine source(cross_loop(&probe));
    Vruta::CrossRoutine target(std::move(source));

    EXPECT_TRUE(source.get_should_terminate());
    source.set_should_terminate(false);
    EXPECT_TRUE(source.get_should_terminate());
    EXPECT_FALSE(target.get_should_terminate());
    EXPECT_TRUE(target.is_active());
}

TEST(GraphicsRoutineTest, MoveAssignmentReleasesTheOldFrame)
{
    Probe first;
    Probe second;
    Vruta::GraphicsRoutine source(graphics_loop(&second));
    Vruta::GraphicsRoutine target(graphics_loop(&first));

    target = std::move(source);

    EXPECT_EQ(first.destroyed, 1);
    EXPECT_EQ(second.destroyed, 0);
    EXPECT_FALSE(source.is_active());
    EXPECT_TRUE(target.is_active());
    EXPECT_EQ(target.next_execution(), 3U);
}

TEST(CrossRoutineTest, CopyIsDeleted)
{
    EXPECT_FALSE(std::is_copy_constructible_v<Vruta::CrossRoutine>);
    EXPECT_FALSE(std::is_copy_assignable_v<Vruta::CrossRoutine>);
}

TEST(CrossRoutineTest, PromiseHoldsItsDefaults)
{
    Probe probe;
    Vruta::cross_promise* promise = nullptr;
    Vruta::CrossRoutine routine(cross_capture(&promise, &probe));

    ASSERT_NE(promise, nullptr);
    EXPECT_EQ(promise->processing_token, Vruta::ProcessingToken::MULTI_RATE);
    EXPECT_TRUE(promise->sync_to_clock);
    EXPECT_TRUE(promise->auto_resume);
    EXPECT_EQ(promise->next_sample.load(), 0U);
    EXPECT_EQ(promise->next_frame.load(), 0U);
    EXPECT_EQ(promise->sample_delay_amount, 0U);
    EXPECT_EQ(promise->frame_delay_amount, 0U);
    EXPECT_FALSE(promise->sample_satisfied.load());
    EXPECT_FALSE(promise->frame_satisfied.load());
    EXPECT_EQ(promise->active_delay_context.load(), Vruta::DelayContext::NONE);
}

TEST(CrossRoutineTest, SampleOnlyGateAdvancesByTheDelay)
{
    Probe probe;
    Vruta::CrossRoutine routine(cross_loop(&probe));

    EXPECT_EQ(routine.next_execution(), DUE);
    EXPECT_FALSE(routine.try_resume_with_context(DUE - 1, Vruta::DelayContext::SAMPLE_BASED));
    EXPECT_EQ(probe.ticks, 1);

    EXPECT_TRUE(routine.try_resume_with_context(DUE, Vruta::DelayContext::SAMPLE_BASED));
    EXPECT_EQ(probe.ticks, 2);
    EXPECT_EQ(routine.next_execution(), 2U * DUE);
}

TEST(CrossRoutineTest, FrameOnlyGateNeedsNoSample)
{
    Probe probe;
    Vruta::CrossRoutine routine(cross_frame_once(&probe));

    EXPECT_FALSE(routine.try_resume_with_context(0, Vruta::DelayContext::SAMPLE_BASED));
    EXPECT_TRUE(routine.try_resume_with_context(1, Vruta::DelayContext::FRAME_BASED));
    EXPECT_EQ(probe.ticks, 2);
    EXPECT_FALSE(routine.is_active());
}

TEST(CrossRoutineTest, BothClocksMustBeSatisfiedBeforeTheGateOpens)
{
    Probe probe;
    Vruta::CrossRoutine routine(cross_both(&probe));

    EXPECT_FALSE(routine.try_resume_with_context(5, Vruta::DelayContext::FRAME_BASED));
    EXPECT_EQ(probe.ticks, 1);

    EXPECT_TRUE(routine.try_resume_with_context(DUE, Vruta::DelayContext::SAMPLE_BASED));
    EXPECT_EQ(probe.ticks, 2);
    EXPECT_FALSE(routine.is_active());
}

TEST(CrossRoutineTest, RejectsRoutinesNotSuspendedOnTheGate)
{
    Probe probe;
    Vruta::CrossRoutine routine(cross_bare(&probe));

    EXPECT_FALSE(routine.try_resume(100));
    EXPECT_FALSE(routine.try_resume_with_context(0, Vruta::DelayContext::NONE));
    EXPECT_EQ(probe.ticks, 1);
    EXPECT_TRUE(routine.is_active());
}

TEST(CrossRoutineTest, AwaitStateInitialisesTheRoutineOnTheNextResume)
{
    Probe probe;
    Vruta::CrossRoutine routine(cross_await(&probe));

    EXPECT_TRUE(routine.try_resume_with_context(7, Vruta::DelayContext::SAMPLE_BASED));
    EXPECT_EQ(probe.ticks, 2);
    EXPECT_FALSE(routine.is_active());
}

TEST(CrossRoutineTest, ClockAccessorsWriteTheAtomicFields)
{
    Probe probe;
    Vruta::CrossRoutine routine(cross_loop(&probe));

    routine.set_next_sample(55);
    EXPECT_EQ(routine.get_next_sample(), 55U);
    EXPECT_EQ(routine.next_execution(), 55U);

    routine.set_next_frame(66);
    EXPECT_EQ(routine.get_next_frame(), 66U);
}

TEST(CrossRoutineTest, MoveConstructorTransfersTheFrame)
{
    Probe probe;
    Vruta::CrossRoutine source(cross_loop(&probe));
    Vruta::CrossRoutine target(std::move(source));

    EXPECT_FALSE(source.is_active());
    EXPECT_TRUE(target.is_active());
    EXPECT_EQ(target.next_execution(), DUE);
}

TEST(CrossRoutineTest, MoveAssignmentReleasesTheOldFrame)
{
    Probe first;
    Probe second;
    Vruta::CrossRoutine source(cross_loop(&second));
    Vruta::CrossRoutine target(cross_loop(&first));

    target = std::move(source);

    EXPECT_EQ(first.destroyed, 1);
    EXPECT_EQ(second.destroyed, 0);
    EXPECT_FALSE(source.is_active());
    EXPECT_TRUE(target.is_active());
    EXPECT_EQ(target.next_execution(), DUE);
}

TEST(CrossRoutineTest, ConcurrentPumpsOpenTheGateExactlyOnce)
{
    for (int attempt = 0; attempt < 50; ++attempt) {
        Probe probe;
        Vruta::CrossRoutine routine(cross_both(&probe));
        std::atomic<int> opened { 0 };

        std::vector<std::thread> pumps;
        for (int i = 0; i < 4; ++i) {
            pumps.emplace_back([&routine, &opened] {
                for (int tries = 0; tries < 1000; ++tries) {
                    if (routine.try_resume_with_context(DUE, Vruta::DelayContext::SAMPLE_BASED)) {
                        opened.fetch_add(1);
                    }
                }
            });
            pumps.emplace_back([&routine, &opened] {
                for (int tries = 0; tries < 1000; ++tries) {
                    if (routine.try_resume_with_context(5, Vruta::DelayContext::FRAME_BASED)) {
                        opened.fetch_add(1);
                    }
                }
            });
        }
        for (auto& pump : pumps) {
            pump.join();
        }

        ASSERT_EQ(opened.load(), 1) << "attempt " << attempt;
        ASSERT_EQ(probe.ticks, 2) << "attempt " << attempt;
    }
}

TEST(FreeRoutineTest, PromiseHoldsItsDefaults)
{
    Probe probe;
    Vruta::conditional_promise* promise = nullptr;
    Vruta::FreeRoutine routine(free_capture(&promise, &probe));

    ASSERT_NE(promise, nullptr);
    EXPECT_EQ(promise->processing_token, Vruta::ProcessingToken::CONDITIONAL);
    EXPECT_TRUE(promise->auto_resume);
    EXPECT_FALSE(promise->armed.load());
    EXPECT_FALSE(static_cast<bool>(promise->condition));
}

TEST(FreeRoutineTest, ConditionGatesEachResume)
{
    Probe probe;
    Vruta::FreeRoutine routine(free_loop(&probe));

    EXPECT_FALSE(routine.try_resume(0));
    EXPECT_EQ(probe.ticks, 1);

    probe.ready = true;
    EXPECT_TRUE(routine.try_resume(0));
    EXPECT_EQ(probe.ticks, 2);
}

TEST(FreeRoutineTest, InitializeStateEnablesAutoResumeWithoutResuming)
{
    Probe probe;
    Vruta::FreeRoutine routine(free_once(&probe));

    routine.set_auto_resume(false);
    EXPECT_TRUE(routine.initialize_state(0));
    EXPECT_TRUE(routine.get_auto_resume());
    EXPECT_EQ(probe.ticks, 1);
}

TEST(FreeRoutineTest, ClockFieldsAreFixedAtZeroOrFalse)
{
    Probe probe;
    Vruta::FreeRoutine routine(free_loop(&probe));

    EXPECT_FALSE(routine.requires_clock_sync());
    EXPECT_FALSE(routine.get_sync_to_clock());
    EXPECT_EQ(routine.next_execution(), 0U);

    routine.set_next_sample(5);
    routine.set_next_frame(5);
    EXPECT_EQ(routine.get_next_sample(), 0U);
    EXPECT_EQ(routine.get_next_frame(), 0U);
}

TEST(FreeRoutineTest, MoveLeavesTheSourceInactiveAndReportsItTerminated)
{
    Probe probe;
    Vruta::FreeRoutine source(free_loop(&probe));
    Vruta::FreeRoutine target(std::move(source));

    EXPECT_FALSE(source.is_active());
    EXPECT_TRUE(source.get_should_terminate());
    EXPECT_TRUE(target.is_active());
}

TEST(FreeRoutineTest, MoveAssignmentReleasesTheOldFrame)
{
    Probe first;
    Probe second;
    Vruta::FreeRoutine source(free_loop(&second));
    Vruta::FreeRoutine target(free_loop(&first));

    target = std::move(source);

    EXPECT_EQ(first.destroyed, 1);
    EXPECT_EQ(second.destroyed, 0);
    EXPECT_FALSE(source.is_active());
    EXPECT_TRUE(target.is_active());
}

} // namespace MayaFlux::Test
