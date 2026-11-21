#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kriya/Awaiters/ConditionAwaiter.hpp"
#include "MayaFlux/Vruta/Routine.hpp"
#include "MayaFlux/Vruta/Scheduler.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

using namespace std::chrono_literals;
using Flag = std::shared_ptr<std::atomic<bool>>;
using Count = std::shared_ptr<std::atomic<int>>;

namespace {

    constexpr unsigned int SAMPLE_RATE = 48000;

    Flag make_flag()
    {
        return std::make_shared<std::atomic<bool>>(false);
    }

    Count make_count()
    {
        return std::make_shared<std::atomic<int>>(0);
    }

    Vruta::FreeRoutine resume_on_flag(Flag flag, Count resumed, Vruta::ConditionRecheck recheck)
    {
        co_await Kriya::ConditionAwaiter { [flag]() { return flag->load(); }, recheck };
        resumed->fetch_add(1);
    }

    Vruta::FreeRoutine resume_after_delay(double seconds, Count resumed)
    {
        co_await Kriya::SteadyDelay { seconds };
        resumed->fetch_add(1);
    }

    Vruta::FreeRoutine step_through(int steps, Count done)
    {
        for (int i = 0; i < steps; ++i) {
            co_await Kriya::ConditionAwaiter { [armed = false]() mutable {
                const bool met = armed;
                armed = true;
                return met;
            } };
        }
        done->fetch_add(1);
    }

    template <typename Predicate>
    bool eventually(Predicate&& predicate, std::chrono::milliseconds limit = 2000ms)
    {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < deadline) {
            if (predicate()) {
                return true;
            }
            std::this_thread::sleep_for(1ms);
        }
        return predicate();
    }

} // namespace

class ConditionalWaitTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        scheduler = std::make_shared<Vruta::TaskScheduler>(SAMPLE_RATE);
    }

    void TearDown() override
    {
        scheduler->terminate_all_tasks();
        scheduler.reset();
    }

    std::shared_ptr<Vruta::TaskScheduler> scheduler;
};

TEST_F(ConditionalWaitTest, ThePollIntervalDefaultsToOneMillisecond)
{
    EXPECT_EQ(scheduler->conditional_poll_interval(), 1000us);
}

TEST_F(ConditionalWaitTest, ThePollIntervalCanBeChangedAndIsAtLeastOneMicrosecond)
{
    scheduler->set_conditional_poll_interval(250us);
    EXPECT_EQ(scheduler->conditional_poll_interval(), 250us);

    scheduler->set_conditional_poll_interval(0us);
    EXPECT_EQ(scheduler->conditional_poll_interval(), 1us);
}

TEST_F(ConditionalWaitTest, APolledConditionResumesOnceItIsTrue)
{
    auto flag = make_flag();
    auto resumed = make_count();

    auto routine = std::make_shared<Vruta::FreeRoutine>(
        resume_on_flag(flag, resumed, Vruta::ConditionRecheck::POLL));
    scheduler->add_task(routine, "polled");

    std::this_thread::sleep_for(20ms);
    EXPECT_EQ(resumed->load(), 0);

    flag->store(true);

    EXPECT_TRUE(eventually([&] { return resumed->load() == 1; }));
}

TEST_F(ConditionalWaitTest, APolledConditionWaitsForTheIntervalUnlessWoken)
{
    scheduler->set_conditional_poll_interval(std::chrono::seconds(10));
    auto flag = make_flag();
    auto resumed = make_count();

    auto routine = std::make_shared<Vruta::FreeRoutine>(
        resume_on_flag(flag, resumed, Vruta::ConditionRecheck::POLL));
    scheduler->add_task(routine, "slow_poll");
    std::this_thread::sleep_for(20ms);

    flag->store(true);
    std::this_thread::sleep_for(200ms);
    EXPECT_EQ(resumed->load(), 0);

    scheduler->wake_conditional();
    EXPECT_TRUE(eventually([&] { return resumed->load() == 1; }));
}

TEST_F(ConditionalWaitTest, AWakeConditionResumesAfterTheWakeNotAfterTheInterval)
{
    scheduler->set_conditional_poll_interval(std::chrono::seconds(10));
    auto flag = make_flag();
    auto resumed = make_count();

    auto routine = std::make_shared<Vruta::FreeRoutine>(
        resume_on_flag(flag, resumed, Vruta::ConditionRecheck::ON_WAKE));
    scheduler->add_task(routine, "on_wake");
    std::this_thread::sleep_for(20ms);

    flag->store(true);
    scheduler->wake_conditional();

    EXPECT_TRUE(eventually([&] { return resumed->load() == 1; }, 500ms));
}

TEST_F(ConditionalWaitTest, AWakeConditionIsNotPolled)
{
    auto flag = make_flag();
    auto resumed = make_count();

    auto routine = std::make_shared<Vruta::FreeRoutine>(
        resume_on_flag(flag, resumed, Vruta::ConditionRecheck::ON_WAKE));
    scheduler->add_task(routine, "unpolled");
    std::this_thread::sleep_for(20ms);

    flag->store(true);
    std::this_thread::sleep_for(100ms);

    EXPECT_EQ(resumed->load(), 0);
}

TEST_F(ConditionalWaitTest, ASteadyDelayResumesAfterItsDeadline)
{
    auto resumed = make_count();
    const auto start = std::chrono::steady_clock::now();

    auto routine = std::make_shared<Vruta::FreeRoutine>(resume_after_delay(0.05, resumed));
    scheduler->add_task(routine, "delay");

    EXPECT_TRUE(eventually([&] { return resumed->load() == 1; }));
    EXPECT_GE(std::chrono::steady_clock::now() - start, 50ms);
}

TEST_F(ConditionalWaitTest, ASteadyDelayDoesNotDependOnThePollInterval)
{
    scheduler->set_conditional_poll_interval(std::chrono::seconds(10));
    auto resumed = make_count();

    auto routine = std::make_shared<Vruta::FreeRoutine>(resume_after_delay(0.05, resumed));
    scheduler->add_task(routine, "delay_slow_poll");

    EXPECT_TRUE(eventually([&] { return resumed->load() == 1; }, 1000ms));
}

TEST_F(ConditionalWaitTest, AZeroSteadyDelaySuspendsOnceAndThenResumes)
{
    auto resumed = make_count();

    auto routine = std::make_shared<Vruta::FreeRoutine>(resume_after_delay(0.0, resumed));
    scheduler->add_task(routine, "zero_delay");

    EXPECT_TRUE(eventually([&] { return resumed->load() == 1; }));
}

TEST_F(ConditionalWaitTest, ATaskThatKeepsMakingProgressDoesNotWaitBetweenSteps)
{
    auto done = make_count();
    const auto start = std::chrono::steady_clock::now();

    auto routine = std::make_shared<Vruta::FreeRoutine>(step_through(1000, done));
    scheduler->add_task(routine, "progress");

    EXPECT_TRUE(eventually([&] { return done->load() == 1; }, 5000ms));
    EXPECT_LT(std::chrono::steady_clock::now() - start, 500ms);
}

TEST_F(ConditionalWaitTest, CancellingATaskReachesAThreadThatIsAsleep)
{
    auto flag = make_flag();
    auto resumed = make_count();
    auto completed = std::make_shared<std::atomic<bool>>(false);

    auto routine = std::make_shared<Vruta::FreeRoutine>(
        resume_on_flag(flag, resumed, Vruta::ConditionRecheck::ON_WAKE));
    routine->on_complete([completed]() { completed->store(true); });
    scheduler->add_task(routine, "cancel_me");
    std::this_thread::sleep_for(20ms);

    EXPECT_TRUE(scheduler->cancel_task("cancel_me"));

    EXPECT_TRUE(eventually([&] { return completed->load(); }, 1000ms));
}

TEST_F(ConditionalWaitTest, AddingATaskReachesAThreadThatIsAsleep)
{
    auto first_flag = make_flag();
    auto first = make_count();
    auto second = make_count();

    auto idle = std::make_shared<Vruta::FreeRoutine>(
        resume_on_flag(first_flag, first, Vruta::ConditionRecheck::ON_WAKE));
    scheduler->add_task(idle, "idle");
    std::this_thread::sleep_for(20ms);

    auto added = std::make_shared<Vruta::FreeRoutine>(resume_after_delay(0.01, second));
    scheduler->add_task(added, "added_later");

    EXPECT_TRUE(eventually([&] { return second->load() == 1; }, 1000ms));
}

TEST_F(ConditionalWaitTest, ShutdownReleasesAThreadThatIsAsleep)
{
    auto flag = make_flag();
    auto resumed = make_count();

    auto routine = std::make_shared<Vruta::FreeRoutine>(
        resume_on_flag(flag, resumed, Vruta::ConditionRecheck::ON_WAKE));
    scheduler->add_task(routine, "asleep");
    std::this_thread::sleep_for(20ms);

    auto terminated = std::async(std::launch::async, [this]() { scheduler->terminate_all_tasks(); });

    EXPECT_EQ(terminated.wait_for(2s), std::future_status::ready);
}

}
