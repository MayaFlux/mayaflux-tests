#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kriya/Timers.hpp"
#include "MayaFlux/Vruta/Scheduler.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

using SchedToken = Vruta::ProcessingToken;

namespace {

    constexpr unsigned int TIMER_LIFETIME_SAMPLE_RATE = 48000;

} // namespace

class TimerLifetimeTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        scheduler = std::make_shared<Vruta::TaskScheduler>(TIMER_LIFETIME_SAMPLE_RATE);
    }

    void TearDown() override
    {
        scheduler.reset();
    }

    void run_seconds(double seconds)
    {
        scheduler->process_token(SchedToken::SAMPLE_ACCURATE, scheduler->seconds_to_samples(seconds));
    }

    std::shared_ptr<Vruta::TaskScheduler> scheduler;
};

TEST_F(TimerLifetimeTest, DestroyedBeforeItFiresRunsNothing)
{
    bool triggered = false;

    {
        Kriya::Timer timer(scheduler);
        timer.schedule(0.01, [&triggered]() { triggered = true; });
    }

    run_seconds(0.05);
    EXPECT_FALSE(triggered);
}

TEST_F(TimerLifetimeTest, CallbackMayScheduleItsTimerAgain)
{
    Kriya::Timer timer(scheduler);
    int fired = 0;

    timer.schedule(0.01, [&timer, &fired]() {
        if (++fired == 1) {
            timer.schedule(0.01, [&fired]() { ++fired; });
        }
    });

    run_seconds(0.015);
    EXPECT_EQ(fired, 1);
    EXPECT_TRUE(timer.is_active());

    run_seconds(0.02);
    EXPECT_EQ(fired, 2);
    EXPECT_FALSE(timer.is_active());
}

TEST_F(TimerLifetimeTest, ReleasesItsCallbackAfterFiring)
{
    auto token = std::make_shared<int>(0);
    const std::weak_ptr<int> watch = token;
    Kriya::Timer timer(scheduler);

    timer.schedule(0.01, [token]() { });
    token.reset();
    EXPECT_FALSE(watch.expired());

    run_seconds(0.02);
    EXPECT_TRUE(watch.expired());
}

TEST_F(TimerLifetimeTest, HeldOnlyByItsOwnCallbackFiresAndIsReleased)
{
    auto timer = std::make_shared<Kriya::Timer>(scheduler);
    const std::weak_ptr<Kriya::Timer> watch = timer;
    bool triggered = false;

    timer->schedule(0.01, [timer, &triggered]() { triggered = true; });
    timer.reset();

    run_seconds(0.005);
    EXPECT_FALSE(triggered);
    EXPECT_FALSE(watch.expired());

    run_seconds(0.02);
    EXPECT_TRUE(triggered);
    EXPECT_TRUE(watch.expired());
}

TEST_F(TimerLifetimeTest, CancelledThenDestroyedRunsNothing)
{
    bool triggered = false;

    {
        Kriya::Timer timer(scheduler);
        timer.schedule(0.01, [&triggered]() { triggered = true; });
        timer.cancel();
    }

    run_seconds(0.05);
    EXPECT_FALSE(triggered);
}

TEST_F(TimerLifetimeTest, RescheduledThenDestroyedRunsNothing)
{
    int fired = 0;

    {
        Kriya::Timer timer(scheduler);
        timer.schedule(0.01, [&fired]() { ++fired; });
        timer.schedule(0.02, [&fired]() { ++fired; });
    }

    run_seconds(0.05);
    EXPECT_EQ(fired, 0);
}

TEST_F(TimerLifetimeTest, TimedActionDestroyedBeforeTheEndRunsNoEnd)
{
    bool started = false;
    bool ended = false;

    {
        Kriya::TimedAction action(scheduler);
        action.execute([&started]() { started = true; }, [&ended]() { ended = true; }, 0.01);
    }

    run_seconds(0.05);
    EXPECT_TRUE(started);
    EXPECT_FALSE(ended);
}

}
