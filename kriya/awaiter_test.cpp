#include "gtest/gtest.h"

#include "MayaFlux/Kriya/Awaiters/ConditionAwaiter.hpp"
#include "MayaFlux/Kriya/Awaiters/DelayAwaiters.hpp"
#include "MayaFlux/Kriya/Awaiters/GetPromise.hpp"
#include "MayaFlux/Vruta/Routine.hpp"

namespace MayaFlux::Test {

namespace {

    constexpr uint64_t DUE = 10U;

    using SoundHandle = std::coroutine_handle<Vruta::audio_promise>;
    using GraphicsHandle = std::coroutine_handle<Vruta::graphics_promise>;
    using CrossHandle = std::coroutine_handle<Vruta::cross_promise>;
    using FreeHandle = std::coroutine_handle<Vruta::conditional_promise>;

    Vruta::SoundRoutine sample_zero(int* ticks)
    {
        ++*ticks;
        co_await Kriya::SampleDelay { 0 };
        ++*ticks;
    }

    Vruta::SoundRoutine sample_chain(int* ticks)
    {
        ++*ticks;
        co_await Kriya::SampleDelay { 5 };
        ++*ticks;
        co_await Kriya::SampleDelay { 7 };
        ++*ticks;
    }

    Vruta::SoundRoutine sample_zero_between(int* ticks)
    {
        ++*ticks;
        co_await Kriya::SampleDelay { 5 };
        ++*ticks;
        co_await Kriya::SampleDelay { 0 };
        ++*ticks;
        co_await Kriya::SampleDelay { 7 };
        ++*ticks;
    }

    Vruta::SoundRoutine sample_loop(int* ticks)
    {
        for (;;) {
            ++*ticks;
            co_await Kriya::SampleDelay { 3 };
        }
    }

    Vruta::SoundRoutine buffer_zero(int* ticks)
    {
        ++*ticks;
        co_await Kriya::BufferDelay { 0 };
        ++*ticks;
    }

    Vruta::SoundRoutine buffer_chain(int* ticks)
    {
        ++*ticks;
        co_await Kriya::BufferDelay { 2 };
        ++*ticks;
        co_await Kriya::BufferDelay { 3 };
        ++*ticks;
    }

    Vruta::SoundRoutine buffer_loop(int* ticks)
    {
        for (;;) {
            ++*ticks;
            co_await Kriya::BufferDelay { 2 };
        }
    }

    Vruta::GraphicsRoutine frame_zero(int* ticks)
    {
        ++*ticks;
        co_await Kriya::FrameDelay { .frames_to_wait = 0 };
        ++*ticks;
    }

    Vruta::GraphicsRoutine frame_chain(int* ticks)
    {
        ++*ticks;
        co_await Kriya::FrameDelay { .frames_to_wait = 3 };
        ++*ticks;
        co_await Kriya::FrameDelay { .frames_to_wait = 4 };
        ++*ticks;
    }

    Vruta::GraphicsRoutine frame_zero_between(int* ticks)
    {
        ++*ticks;
        co_await Kriya::FrameDelay { .frames_to_wait = 3 };
        ++*ticks;
        co_await Kriya::FrameDelay { .frames_to_wait = 0 };
        ++*ticks;
        co_await Kriya::FrameDelay { .frames_to_wait = 4 };
        ++*ticks;
    }

    Vruta::GraphicsRoutine frame_loop(int* ticks)
    {
        for (;;) {
            ++*ticks;
            co_await Kriya::FrameDelay { .frames_to_wait = 2 };
        }
    }

    Vruta::CrossRoutine multi_zero(int* ticks)
    {
        ++*ticks;
        co_await Kriya::MultiRateDelay { 0, 0 };
        ++*ticks;
    }

    Vruta::CrossRoutine multi_sample(int* ticks)
    {
        ++*ticks;
        co_await Kriya::MultiRateDelay { 5, 0 };
        ++*ticks;
        co_await Kriya::MultiRateDelay { 5, 0 };
        ++*ticks;
    }

    Vruta::CrossRoutine multi_sample_late(int* ticks)
    {
        ++*ticks;
        co_await Kriya::MultiRateDelay { DUE, 0 };
        ++*ticks;
        co_await Kriya::MultiRateDelay { 5, 0 };
        ++*ticks;
    }

    Vruta::CrossRoutine multi_one_sample(int* ticks)
    {
        ++*ticks;
        co_await Kriya::MultiRateDelay { DUE, 0 };
        ++*ticks;
    }

    Vruta::CrossRoutine multi_frame(int* ticks)
    {
        ++*ticks;
        co_await Kriya::MultiRateDelay { 0, 4 };
        ++*ticks;
    }

    Vruta::CrossRoutine multi_both(int* ticks)
    {
        ++*ticks;
        co_await Kriya::MultiRateDelay { DUE, 4 };
        ++*ticks;
    }

    Vruta::CrossRoutine multi_sample_then_frame(int* ticks)
    {
        ++*ticks;
        co_await Kriya::MultiRateDelay { DUE, 0 };
        ++*ticks;
        co_await Kriya::MultiRateDelay { 0, 4 };
        ++*ticks;
    }

    Vruta::FreeRoutine condition_true(int* ticks)
    {
        co_await Kriya::ConditionAwaiter { [] { return true; } };
        ++*ticks;
    }

    Vruta::FreeRoutine condition_gate(int* ticks, const bool* ready, int* polls)
    {
        co_await Kriya::ConditionAwaiter { [ready, polls] {
            ++*polls;
            return *ready;
        } };
        ++*ticks;
    }

    Vruta::FreeRoutine condition_two(int* ticks, const bool* first, const bool* second)
    {
        co_await Kriya::ConditionAwaiter { [first] { return *first; } };
        ++*ticks;
        co_await Kriya::ConditionAwaiter { [second] { return *second; } };
        ++*ticks;
    }

    Vruta::SoundRoutine get_audio(uint64_t* seen_next)
    {
        auto& promise = co_await Kriya::GetAudioPromise {};
        *seen_next = promise.next_sample;
        promise.set_state<int>("tag", 42);
    }

    Vruta::GraphicsRoutine get_graphics(uint64_t* seen_frame)
    {
        auto& promise = co_await Kriya::GetGraphicsPromise {};
        *seen_frame = promise.next_frame;
        promise.set_state<int>("tag", 42);
    }

    Vruta::CrossRoutine get_cross(uint64_t* seen_sample, uint64_t* seen_frame)
    {
        auto& promise = co_await Kriya::GetCrossPromise {};
        *seen_sample = promise.next_sample.load();
        *seen_frame = promise.next_frame.load();
        promise.set_state<int>("tag", 42);
    }

    template <typename Awaitable, typename Handle>
    concept SuspendableOn = requires(Awaitable& awaitable, Handle handle) {
        awaitable.await_suspend(handle);
    };

    static_assert(SuspendableOn<Kriya::SampleDelay, SoundHandle>);
    static_assert(!SuspendableOn<Kriya::SampleDelay, GraphicsHandle>);
    static_assert(!SuspendableOn<Kriya::SampleDelay, CrossHandle>);

    static_assert(SuspendableOn<Kriya::BufferDelay, SoundHandle>);
    static_assert(!SuspendableOn<Kriya::BufferDelay, GraphicsHandle>);
    static_assert(!SuspendableOn<Kriya::BufferDelay, CrossHandle>);

    static_assert(SuspendableOn<Kriya::FrameDelay, GraphicsHandle>);
    static_assert(!SuspendableOn<Kriya::FrameDelay, SoundHandle>);
    static_assert(!SuspendableOn<Kriya::FrameDelay, CrossHandle>);

    static_assert(SuspendableOn<Kriya::MultiRateDelay, CrossHandle>);
    static_assert(!SuspendableOn<Kriya::MultiRateDelay, SoundHandle>);
    static_assert(!SuspendableOn<Kriya::MultiRateDelay, GraphicsHandle>);

    static_assert(SuspendableOn<Kriya::ConditionAwaiter, FreeHandle>);
    static_assert(!SuspendableOn<Kriya::ConditionAwaiter, SoundHandle>);
    static_assert(!SuspendableOn<Kriya::ConditionAwaiter, GraphicsHandle>);
    static_assert(!SuspendableOn<Kriya::ConditionAwaiter, CrossHandle>);

    static_assert(SuspendableOn<Kriya::GetAudioPromise, SoundHandle>);
    static_assert(!SuspendableOn<Kriya::GetAudioPromise, GraphicsHandle>);
    static_assert(!SuspendableOn<Kriya::GetAudioPromise, CrossHandle>);

    static_assert(SuspendableOn<Kriya::GetGraphicsPromise, GraphicsHandle>);
    static_assert(!SuspendableOn<Kriya::GetGraphicsPromise, SoundHandle>);
    static_assert(!SuspendableOn<Kriya::GetGraphicsPromise, CrossHandle>);

    static_assert(SuspendableOn<Kriya::GetCrossPromise, CrossHandle>);
    static_assert(!SuspendableOn<Kriya::GetCrossPromise, SoundHandle>);
    static_assert(!SuspendableOn<Kriya::GetCrossPromise, GraphicsHandle>);

} // namespace

TEST(SampleDelayAwaiterTest, ZeroDelayNeverSuspends)
{
    int ticks = 0;
    Vruta::SoundRoutine routine(sample_zero(&ticks));

    EXPECT_EQ(ticks, 2);
    EXPECT_FALSE(routine.is_active());
}

TEST(SampleDelayAwaiterTest, NonZeroDelaySuspendsWithTheSampleContext)
{
    int ticks = 0;
    Vruta::SoundRoutine routine(sample_chain(&ticks));

    EXPECT_EQ(ticks, 1);
    EXPECT_TRUE(routine.is_active());
    EXPECT_EQ(routine.next_execution(), 5U);
    EXPECT_EQ(routine.get_delay_context(), Vruta::DelayContext::SAMPLE_BASED);
}

TEST(SampleDelayAwaiterTest, DelayIsAddedToTheCurrentNextSample)
{
    int ticks = 0;
    Vruta::SoundRoutine routine(sample_loop(&ticks));

    EXPECT_EQ(routine.next_execution(), 3U);
    EXPECT_TRUE(routine.initialize_state(50));
    EXPECT_EQ(ticks, 2);
    EXPECT_EQ(routine.next_execution(), 53U);
}

TEST(SampleDelayAwaiterTest, ResumeAtTheDueSampleSchedulesTheNextDelayFromThere)
{
    int ticks = 0;
    Vruta::SoundRoutine routine(sample_chain(&ticks));

    EXPECT_FALSE(routine.try_resume(4));
    EXPECT_TRUE(routine.try_resume(5));
    EXPECT_EQ(ticks, 2);
    EXPECT_EQ(routine.next_execution(), 12U);

    EXPECT_FALSE(routine.try_resume(11));
    EXPECT_TRUE(routine.try_resume(12));
    EXPECT_EQ(ticks, 3);
    EXPECT_FALSE(routine.is_active());
}

TEST(SampleDelayAwaiterTest, LateResumeSchedulesFromTheResumeSample)
{
    int ticks = 0;
    Vruta::SoundRoutine routine(sample_chain(&ticks));

    EXPECT_TRUE(routine.try_resume(9));
    EXPECT_EQ(ticks, 2);
    EXPECT_EQ(routine.next_execution(), 16U);
}

TEST(SampleDelayAwaiterTest, ZeroDelayBetweenDelaysLeavesTheSchedule)
{
    int ticks = 0;
    Vruta::SoundRoutine routine(sample_zero_between(&ticks));

    EXPECT_TRUE(routine.try_resume(5));
    EXPECT_EQ(ticks, 3);
    EXPECT_EQ(routine.next_execution(), 12U);
}

TEST(SampleDelayAwaiterTest, OnlyTheSampleContextResumes)
{
    int ticks = 0;
    Vruta::SoundRoutine routine(sample_chain(&ticks));

    EXPECT_FALSE(routine.try_resume_with_context(100, Vruta::DelayContext::BUFFER_BASED));
    EXPECT_FALSE(routine.try_resume_with_context(100, Vruta::DelayContext::FRAME_BASED));
    EXPECT_FALSE(routine.try_resume_with_context(100, Vruta::DelayContext::NONE));
    EXPECT_EQ(ticks, 1);
    EXPECT_TRUE(routine.is_active());
}

TEST(SampleDelayAwaiterTest, ManySuspensionsAccumulateExactly)
{
    int ticks = 0;
    Vruta::SoundRoutine routine(sample_loop(&ticks));

    for (uint64_t i = 0; i < 50; ++i) {
        const uint64_t due = routine.next_execution();
        ASSERT_TRUE(routine.try_resume(due)) << "resume " << i;
        ASSERT_EQ(routine.next_execution(), due + 3U) << "resume " << i;
    }
    EXPECT_EQ(ticks, 51);
    EXPECT_TRUE(routine.is_active());
}

TEST(BufferDelayAwaiterTest, ZeroDelayNeverSuspends)
{
    int ticks = 0;
    Vruta::SoundRoutine routine(buffer_zero(&ticks));

    EXPECT_EQ(ticks, 2);
    EXPECT_FALSE(routine.is_active());
}

TEST(BufferDelayAwaiterTest, NonZeroDelaySuspendsWithTheBufferContext)
{
    int ticks = 0;
    Vruta::SoundRoutine routine(buffer_chain(&ticks));

    EXPECT_EQ(ticks, 1);
    EXPECT_EQ(routine.get_next_frame(), 2U);
    EXPECT_EQ(routine.get_delay_context(), Vruta::DelayContext::BUFFER_BASED);
}

TEST(BufferDelayAwaiterTest, ResumeOnTheDueCycleAdvancesToTheNextCycle)
{
    int ticks = 0;
    Vruta::SoundRoutine routine(buffer_chain(&ticks));

    EXPECT_FALSE(routine.try_resume_with_context(1, Vruta::DelayContext::BUFFER_BASED));
    EXPECT_TRUE(routine.try_resume_with_context(2, Vruta::DelayContext::BUFFER_BASED));
    EXPECT_EQ(ticks, 2);
    EXPECT_EQ(routine.get_next_frame(), 5U);

    EXPECT_FALSE(routine.try_resume_with_context(4, Vruta::DelayContext::BUFFER_BASED));
    EXPECT_TRUE(routine.try_resume_with_context(5, Vruta::DelayContext::BUFFER_BASED));
    EXPECT_EQ(ticks, 3);
    EXPECT_FALSE(routine.is_active());
}

TEST(BufferDelayAwaiterTest, LateResumeKeepsTheScheduledCycleAndCatchesUp)
{
    int ticks = 0;
    Vruta::SoundRoutine routine(buffer_chain(&ticks));

    EXPECT_TRUE(routine.try_resume_with_context(6, Vruta::DelayContext::BUFFER_BASED));
    EXPECT_EQ(ticks, 2);
    EXPECT_EQ(routine.get_next_frame(), 5U);

    EXPECT_TRUE(routine.try_resume_with_context(6, Vruta::DelayContext::BUFFER_BASED));
    EXPECT_EQ(ticks, 3);
    EXPECT_FALSE(routine.is_active());
}

TEST(BufferDelayAwaiterTest, OnlyTheBufferContextResumes)
{
    int ticks = 0;
    Vruta::SoundRoutine routine(buffer_chain(&ticks));

    EXPECT_FALSE(routine.try_resume_with_context(100, Vruta::DelayContext::SAMPLE_BASED));
    EXPECT_FALSE(routine.try_resume_with_context(100, Vruta::DelayContext::FRAME_BASED));
    EXPECT_FALSE(routine.try_resume_with_context(100, Vruta::DelayContext::NONE));
    EXPECT_EQ(ticks, 1);
}

TEST(BufferDelayAwaiterTest, ManySuspensionsAccumulateExactly)
{
    int ticks = 0;
    Vruta::SoundRoutine routine(buffer_loop(&ticks));

    for (uint64_t i = 0; i < 50; ++i) {
        const uint64_t due = routine.get_next_frame();
        ASSERT_TRUE(routine.try_resume_with_context(due, Vruta::DelayContext::BUFFER_BASED)) << "resume " << i;
        ASSERT_EQ(routine.get_next_frame(), due + 2U) << "resume " << i;
    }
    EXPECT_EQ(ticks, 51);
}

TEST(FrameDelayAwaiterTest, ZeroDelayNeverSuspends)
{
    int ticks = 0;
    Vruta::GraphicsRoutine routine(frame_zero(&ticks));

    EXPECT_EQ(ticks, 2);
    EXPECT_FALSE(routine.is_active());
}

TEST(FrameDelayAwaiterTest, NonZeroDelaySuspendsWithTheFrameContext)
{
    int ticks = 0;
    Vruta::GraphicsRoutine routine(frame_chain(&ticks));

    EXPECT_EQ(ticks, 1);
    EXPECT_EQ(routine.next_execution(), 3U);
    EXPECT_EQ(routine.get_delay_context(), Vruta::DelayContext::FRAME_BASED);
}

TEST(FrameDelayAwaiterTest, ResumeOnTheDueFrameSchedulesTheNextDelay)
{
    int ticks = 0;
    Vruta::GraphicsRoutine routine(frame_chain(&ticks));

    EXPECT_FALSE(routine.try_resume(2));
    EXPECT_TRUE(routine.try_resume(3));
    EXPECT_EQ(ticks, 2);
    EXPECT_EQ(routine.next_execution(), 7U);

    EXPECT_FALSE(routine.try_resume(6));
    EXPECT_TRUE(routine.try_resume(7));
    EXPECT_EQ(ticks, 3);
    EXPECT_FALSE(routine.is_active());
}

TEST(FrameDelayAwaiterTest, LateResumeSchedulesFromTheResumeFrame)
{
    int ticks = 0;
    Vruta::GraphicsRoutine routine(frame_chain(&ticks));

    EXPECT_TRUE(routine.try_resume(5));
    EXPECT_EQ(ticks, 2);
    EXPECT_EQ(routine.next_execution(), 9U);
}

TEST(FrameDelayAwaiterTest, ZeroDelayBetweenDelaysLeavesTheSchedule)
{
    int ticks = 0;
    Vruta::GraphicsRoutine routine(frame_zero_between(&ticks));

    EXPECT_TRUE(routine.try_resume(3));
    EXPECT_EQ(ticks, 3);
    EXPECT_EQ(routine.next_execution(), 7U);
}

TEST(FrameDelayAwaiterTest, OnlyTheFrameContextResumes)
{
    int ticks = 0;
    Vruta::GraphicsRoutine routine(frame_chain(&ticks));

    EXPECT_FALSE(routine.try_resume_with_context(100, Vruta::DelayContext::SAMPLE_BASED));
    EXPECT_FALSE(routine.try_resume_with_context(100, Vruta::DelayContext::BUFFER_BASED));
    EXPECT_FALSE(routine.try_resume_with_context(100, Vruta::DelayContext::NONE));
    EXPECT_EQ(ticks, 1);
}

TEST(FrameDelayAwaiterTest, InitializeStateSetsTheBaseFrame)
{
    int ticks = 0;
    Vruta::GraphicsRoutine routine(frame_loop(&ticks));

    EXPECT_TRUE(routine.initialize_state(30));
    EXPECT_EQ(ticks, 2);
    EXPECT_EQ(routine.next_execution(), 32U);
}

TEST(FrameDelayAwaiterTest, ManySuspensionsAccumulateExactly)
{
    int ticks = 0;
    Vruta::GraphicsRoutine routine(frame_loop(&ticks));

    for (uint64_t i = 0; i < 50; ++i) {
        const uint64_t due = routine.next_execution();
        ASSERT_TRUE(routine.try_resume(due)) << "resume " << i;
        ASSERT_EQ(routine.next_execution(), due + 2U) << "resume " << i;
    }
    EXPECT_EQ(ticks, 51);
}

TEST(MultiRateDelayAwaiterTest, ZeroDelayOnBothClocksNeverSuspends)
{
    int ticks = 0;
    Vruta::CrossRoutine routine(multi_zero(&ticks));

    EXPECT_EQ(ticks, 2);
    EXPECT_FALSE(routine.is_active());
}

TEST(MultiRateDelayAwaiterTest, SuspendsWithTheMultipleContext)
{
    int ticks = 0;
    Vruta::CrossRoutine routine(multi_sample(&ticks));

    EXPECT_EQ(ticks, 1);
    EXPECT_EQ(routine.get_next_sample(), 5U);
    EXPECT_EQ(routine.get_delay_context(), Vruta::DelayContext::MULTIPLE);
}

TEST(MultiRateDelayAwaiterTest, SampleOnlyDelayAccumulatesOnTheSampleClock)
{
    int ticks = 0;
    Vruta::CrossRoutine routine(multi_sample(&ticks));

    EXPECT_FALSE(routine.try_resume_with_context(4, Vruta::DelayContext::SAMPLE_BASED));
    EXPECT_TRUE(routine.try_resume_with_context(5, Vruta::DelayContext::SAMPLE_BASED));
    EXPECT_EQ(ticks, 2);
    EXPECT_EQ(routine.get_next_sample(), 10U);

    EXPECT_FALSE(routine.try_resume_with_context(9, Vruta::DelayContext::SAMPLE_BASED));
    EXPECT_TRUE(routine.try_resume_with_context(10, Vruta::DelayContext::SAMPLE_BASED));
    EXPECT_EQ(ticks, 3);
    EXPECT_FALSE(routine.is_active());
}

TEST(MultiRateDelayAwaiterTest, LateSampleResumeDoesNotRebaseTheClock)
{
    int ticks = 0;
    Vruta::CrossRoutine routine(multi_sample_late(&ticks));

    EXPECT_TRUE(routine.try_resume_with_context(12, Vruta::DelayContext::SAMPLE_BASED));
    EXPECT_EQ(ticks, 2);
    EXPECT_EQ(routine.get_next_sample(), 15U);

    EXPECT_FALSE(routine.try_resume_with_context(14, Vruta::DelayContext::SAMPLE_BASED));
    EXPECT_TRUE(routine.try_resume_with_context(15, Vruta::DelayContext::SAMPLE_BASED));
    EXPECT_EQ(ticks, 3);
}

TEST(MultiRateDelayAwaiterTest, FrameOnlyDelayNeedsNoSample)
{
    int ticks = 0;
    Vruta::CrossRoutine routine(multi_frame(&ticks));

    EXPECT_EQ(routine.get_next_frame(), 4U);
    EXPECT_FALSE(routine.try_resume_with_context(3, Vruta::DelayContext::FRAME_BASED));
    EXPECT_TRUE(routine.try_resume_with_context(4, Vruta::DelayContext::FRAME_BASED));
    EXPECT_EQ(ticks, 2);
    EXPECT_FALSE(routine.is_active());
}

TEST(MultiRateDelayAwaiterTest, FrameOnlyDelayIgnoresTheSampleClock)
{
    int ticks = 0;
    Vruta::CrossRoutine routine(multi_frame(&ticks));

    EXPECT_FALSE(routine.try_resume_with_context(1000, Vruta::DelayContext::SAMPLE_BASED));
    EXPECT_EQ(ticks, 1);
    EXPECT_TRUE(routine.is_active());
}

TEST(MultiRateDelayAwaiterTest, BothClocksMustBeDueFrameFirst)
{
    int ticks = 0;
    Vruta::CrossRoutine routine(multi_both(&ticks));

    EXPECT_EQ(routine.get_next_sample(), DUE);
    EXPECT_EQ(routine.get_next_frame(), 4U);

    EXPECT_FALSE(routine.try_resume_with_context(4, Vruta::DelayContext::FRAME_BASED));
    EXPECT_EQ(ticks, 1);
    EXPECT_TRUE(routine.try_resume_with_context(DUE, Vruta::DelayContext::SAMPLE_BASED));
    EXPECT_EQ(ticks, 2);
    EXPECT_FALSE(routine.is_active());
}

TEST(MultiRateDelayAwaiterTest, BothClocksMustBeDueSampleFirst)
{
    int ticks = 0;
    Vruta::CrossRoutine routine(multi_both(&ticks));

    EXPECT_FALSE(routine.try_resume_with_context(DUE, Vruta::DelayContext::SAMPLE_BASED));
    EXPECT_EQ(ticks, 1);
    EXPECT_TRUE(routine.try_resume_with_context(4, Vruta::DelayContext::FRAME_BASED));
    EXPECT_EQ(ticks, 2);
    EXPECT_FALSE(routine.is_active());
}

TEST(MultiRateDelayAwaiterTest, NewSuspensionReplacesTheOldRequirement)
{
    int ticks = 0;
    Vruta::CrossRoutine routine(multi_sample_then_frame(&ticks));

    EXPECT_TRUE(routine.try_resume_with_context(DUE, Vruta::DelayContext::SAMPLE_BASED));
    EXPECT_EQ(ticks, 2);
    EXPECT_EQ(routine.get_next_frame(), 4U);

    EXPECT_FALSE(routine.try_resume_with_context(1000, Vruta::DelayContext::SAMPLE_BASED));
    EXPECT_EQ(ticks, 2);
    EXPECT_TRUE(routine.try_resume_with_context(4, Vruta::DelayContext::FRAME_BASED));
    EXPECT_EQ(ticks, 3);
    EXPECT_FALSE(routine.is_active());
}

TEST(MultiRateDelayAwaiterTest, ContextReturnsToNoneOnceTheGateOpens)
{
    int ticks = 0;
    Vruta::CrossRoutine routine(multi_one_sample(&ticks));

    EXPECT_EQ(routine.get_delay_context(), Vruta::DelayContext::MULTIPLE);
    EXPECT_TRUE(routine.try_resume_with_context(DUE, Vruta::DelayContext::SAMPLE_BASED));
    EXPECT_EQ(routine.get_delay_context(), Vruta::DelayContext::NONE);
    EXPECT_FALSE(routine.is_active());
}

TEST(MultiRateDelayAwaiterTest, ConcurrentSamplePumpsOpenTheGateExactlyOnce)
{
    for (int attempt = 0; attempt < 50; ++attempt) {
        int ticks = 0;
        Vruta::CrossRoutine routine(multi_one_sample(&ticks));
        std::atomic<int> opened { 0 };

        std::vector<std::thread> pumps;
        for (int i = 0; i < 8; ++i) {
            pumps.emplace_back([&routine, &opened] {
                for (int tries = 0; tries < 1000; ++tries) {
                    if (routine.try_resume_with_context(DUE, Vruta::DelayContext::SAMPLE_BASED)) {
                        opened.fetch_add(1);
                    }
                }
            });
        }
        for (auto& pump : pumps) {
            pump.join();
        }

        ASSERT_EQ(opened.load(), 1) << "attempt " << attempt;
        ASSERT_EQ(ticks, 2) << "attempt " << attempt;
    }
}

TEST(MultiRateDelayAwaiterTest, ConcurrentSampleAndFramePumpsOpenTheGateExactlyOnce)
{
    for (int attempt = 0; attempt < 50; ++attempt) {
        int ticks = 0;
        Vruta::CrossRoutine routine(multi_both(&ticks));
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
                    if (routine.try_resume_with_context(4, Vruta::DelayContext::FRAME_BASED)) {
                        opened.fetch_add(1);
                    }
                }
            });
        }
        for (auto& pump : pumps) {
            pump.join();
        }

        ASSERT_EQ(opened.load(), 1) << "attempt " << attempt;
        ASSERT_EQ(ticks, 2) << "attempt " << attempt;
    }
}

TEST(ConditionAwaiterTest, TrueConditionNeverSuspends)
{
    int ticks = 0;
    Vruta::FreeRoutine routine(condition_true(&ticks));

    EXPECT_EQ(ticks, 1);
    EXPECT_FALSE(routine.is_active());
}

TEST(ConditionAwaiterTest, FalseConditionSuspendsAndIsPolledOncePerTry)
{
    bool ready = false;
    int polls = 0;
    int ticks = 0;
    Vruta::FreeRoutine routine(condition_gate(&ticks, &ready, &polls));

    EXPECT_EQ(polls, 1);
    EXPECT_EQ(ticks, 0);
    EXPECT_TRUE(routine.is_active());

    EXPECT_FALSE(routine.try_resume(0));
    EXPECT_EQ(polls, 2);
    EXPECT_EQ(ticks, 0);

    ready = true;
    EXPECT_TRUE(routine.try_resume(0));
    EXPECT_EQ(polls, 3);
    EXPECT_EQ(ticks, 1);
    EXPECT_FALSE(routine.is_active());
}

TEST(ConditionAwaiterTest, ConditionIsNotPolledWhileAutoResumeIsOff)
{
    bool ready = false;
    int polls = 0;
    int ticks = 0;
    Vruta::FreeRoutine routine(condition_gate(&ticks, &ready, &polls));
    const int polls_at_suspend = polls;

    routine.set_auto_resume(false);
    ready = true;
    EXPECT_FALSE(routine.try_resume(0));
    EXPECT_FALSE(routine.try_resume(0));
    EXPECT_EQ(polls, polls_at_suspend);
    EXPECT_EQ(ticks, 0);
    EXPECT_TRUE(routine.is_active());
}

TEST(ConditionAwaiterTest, ConditionIsNotPolledAfterTermination)
{
    bool ready = false;
    int polls = 0;
    int ticks = 0;
    Vruta::FreeRoutine routine(condition_gate(&ticks, &ready, &polls));
    const int polls_at_suspend = polls;

    routine.set_should_terminate(true);
    ready = true;
    EXPECT_FALSE(routine.try_resume(0));
    EXPECT_EQ(polls, polls_at_suspend);
    EXPECT_EQ(ticks, 0);
}

TEST(ConditionAwaiterTest, ConditionIsNotPolledAfterCompletion)
{
    bool ready = false;
    int polls = 0;
    int ticks = 0;
    Vruta::FreeRoutine routine(condition_gate(&ticks, &ready, &polls));

    ready = true;
    ASSERT_TRUE(routine.try_resume(0));
    ASSERT_FALSE(routine.is_active());
    const int polls_at_finish = polls;

    EXPECT_FALSE(routine.try_resume(0));
    EXPECT_EQ(polls, polls_at_finish);
}

TEST(ConditionAwaiterTest, EachWaitUsesItsOwnCondition)
{
    bool first = false;
    bool second = false;
    int ticks = 0;
    Vruta::FreeRoutine routine(condition_two(&ticks, &first, &second));

    first = true;
    EXPECT_TRUE(routine.try_resume(0));
    EXPECT_EQ(ticks, 1);
    EXPECT_TRUE(routine.is_active());

    EXPECT_FALSE(routine.try_resume(0));
    EXPECT_EQ(ticks, 1);

    second = true;
    EXPECT_TRUE(routine.try_resume(0));
    EXPECT_EQ(ticks, 2);
    EXPECT_FALSE(routine.is_active());
}

TEST(ConditionAwaiterTest, ForcedResumeRunsTheBodyWithoutPollingTheCondition)
{
    bool ready = false;
    int polls = 0;
    int ticks = 0;
    Vruta::FreeRoutine routine(condition_gate(&ticks, &ready, &polls));
    const int polls_at_suspend = polls;

    EXPECT_TRUE(routine.force_resume());
    EXPECT_EQ(ticks, 1);
    EXPECT_EQ(polls, polls_at_suspend);
}

TEST(GetPromiseAwaiterTest, AudioAccessorSuspendsWithTheAwaitContext)
{
    uint64_t seen = 999;
    Vruta::SoundRoutine routine(get_audio(&seen));

    EXPECT_TRUE(routine.is_active());
    EXPECT_EQ(routine.get_delay_context(), Vruta::DelayContext::AWAIT);
    EXPECT_EQ(seen, 999U);
}

TEST(GetPromiseAwaiterTest, AudioResumeHandsOverTheRoutinesOwnPromise)
{
    uint64_t seen = 999;
    Vruta::SoundRoutine routine(get_audio(&seen));

    EXPECT_TRUE(routine.try_resume_with_context(7, Vruta::DelayContext::SAMPLE_BASED));
    EXPECT_EQ(seen, 7U);
    EXPECT_FALSE(routine.is_active());

    const int* tag = routine.get_state<int>("tag");
    ASSERT_NE(tag, nullptr);
    EXPECT_EQ(*tag, 42);
}

TEST(GetPromiseAwaiterTest, AudioResumesOnAnyNonNoneContext)
{
    uint64_t seen = 999;
    Vruta::SoundRoutine routine(get_audio(&seen));

    EXPECT_TRUE(routine.try_resume_with_context(3, Vruta::DelayContext::BUFFER_BASED));
    EXPECT_EQ(seen, 3U);
}

TEST(GetPromiseAwaiterTest, AudioNoneContextDoesNotRelease)
{
    uint64_t seen = 999;
    Vruta::SoundRoutine routine(get_audio(&seen));

    EXPECT_FALSE(routine.try_resume_with_context(7, Vruta::DelayContext::NONE));
    EXPECT_TRUE(routine.is_active());
    EXPECT_EQ(routine.get_delay_context(), Vruta::DelayContext::AWAIT);
    EXPECT_EQ(seen, 999U);
    EXPECT_EQ(routine.get_state<int>("tag"), nullptr);
}

TEST(GetPromiseAwaiterTest, GraphicsResumeHandsOverTheFrameAndPromise)
{
    uint64_t seen = 999;
    Vruta::GraphicsRoutine routine(get_graphics(&seen));

    EXPECT_EQ(routine.get_delay_context(), Vruta::DelayContext::AWAIT);
    EXPECT_TRUE(routine.try_resume(9));
    EXPECT_EQ(seen, 9U);
    EXPECT_FALSE(routine.is_active());

    const int* tag = routine.get_state<int>("tag");
    ASSERT_NE(tag, nullptr);
    EXPECT_EQ(*tag, 42);
}

TEST(GetPromiseAwaiterTest, GraphicsNoneContextDoesNotRelease)
{
    uint64_t seen = 999;
    Vruta::GraphicsRoutine routine(get_graphics(&seen));

    EXPECT_FALSE(routine.try_resume_with_context(9, Vruta::DelayContext::NONE));
    EXPECT_EQ(routine.get_delay_context(), Vruta::DelayContext::AWAIT);
    EXPECT_EQ(seen, 999U);
}

TEST(GetPromiseAwaiterTest, CrossResumeHandsOverBothClocksAndPromise)
{
    uint64_t seen_sample = 999;
    uint64_t seen_frame = 999;
    Vruta::CrossRoutine routine(get_cross(&seen_sample, &seen_frame));

    EXPECT_EQ(routine.get_delay_context(), Vruta::DelayContext::AWAIT);
    EXPECT_TRUE(routine.try_resume_with_context(6, Vruta::DelayContext::SAMPLE_BASED));
    EXPECT_EQ(seen_sample, 6U);
    EXPECT_EQ(seen_frame, 6U);
    EXPECT_FALSE(routine.is_active());

    const int* tag = routine.get_state<int>("tag");
    ASSERT_NE(tag, nullptr);
    EXPECT_EQ(*tag, 42);
}

TEST(GetPromiseAwaiterTest, CrossNoneContextDoesNotRelease)
{
    uint64_t seen_sample = 999;
    uint64_t seen_frame = 999;
    Vruta::CrossRoutine routine(get_cross(&seen_sample, &seen_frame));

    EXPECT_FALSE(routine.try_resume_with_context(6, Vruta::DelayContext::NONE));
    EXPECT_EQ(routine.get_delay_context(), Vruta::DelayContext::AWAIT);
    EXPECT_EQ(seen_sample, 999U);
}

} // namespace MayaFlux::Test
