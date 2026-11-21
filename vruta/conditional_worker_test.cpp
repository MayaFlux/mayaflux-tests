#include "../support/test_metadata.hpp"
#include "gtest/gtest.h"

#include "MayaFlux/Kriya/Awaiters/ConditionAwaiter.hpp"
#include "MayaFlux/Kriya/Awaiters/GetPromise.hpp"
#include "MayaFlux/Vruta/Routine.hpp"
#include "MayaFlux/Vruta/Scheduler.hpp"

#include <future>

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

using namespace std::chrono_literals;

namespace {

    constexpr unsigned int SAMPLE_RATE = 48000;

    struct Visit {
        std::thread::id before;
        std::thread::id after;
        std::atomic<bool> started { false };
        std::atomic<bool> done { false };
    };

    using VisitPtr = std::shared_ptr<Visit>;

    VisitPtr make_visit()
    {
        return std::make_shared<Visit>();
    }

    Vruta::FreeRoutine hop(Vruta::WorkerId worker, VisitPtr visit)
    {
        visit->before = std::this_thread::get_id();
        static_cast<void>(co_await Kriya::OnWorker { worker });
        visit->after = std::this_thread::get_id();
        visit->done.store(true);
    }

    Vruta::FreeRoutine handshake(VisitPtr visit)
    {
        visit->before = std::this_thread::get_id();
        static_cast<void>(co_await Kriya::GetFreePromise {});
        visit->after = std::this_thread::get_id();
        visit->done.store(true);
    }

    Vruta::FreeRoutine work_for(Vruta::WorkerId worker, std::chrono::milliseconds busy, VisitPtr visit)
    {
        static_cast<void>(co_await Kriya::OnWorker { worker });
        visit->started.store(true);
        std::this_thread::sleep_for(busy);
        visit->done.store(true);
    }

    Vruta::FreeRoutine delayed(Vruta::WorkerId worker, double seconds, VisitPtr visit)
    {
        static_cast<void>(co_await Kriya::OnWorker { worker });
        co_await Kriya::SteadyDelay { seconds };
        visit->done.store(true);
    }

    Vruta::FreeRoutine wake_gated(Vruta::WorkerId worker, std::shared_ptr<std::atomic<bool>> flag, VisitPtr visit)
    {
        static_cast<void>(co_await Kriya::OnWorker { worker });
        co_await Kriya::ConditionAwaiter { [flag]() { return flag->load(); }, true };
        visit->done.store(true);
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

class ConditionalWorkerTest : public ::testing::Test {
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

    void add(const std::shared_ptr<Vruta::FreeRoutine>& routine, const std::string& name)
    {
        scheduler->add_task(routine, name);
    }

    std::shared_ptr<Vruta::TaskScheduler> scheduler;
};

TEST_F(ConditionalWorkerTest, AddedWorkersGetDistinctHandlesAfterTheDefault)
{
    const auto first = scheduler->add_worker("first");
    const auto second = scheduler->add_worker("second");

    EXPECT_EQ(first.index, 1U);
    EXPECT_EQ(second.index, 2U);
    EXPECT_EQ(Vruta::WorkerId {}.index, 0U);
}

TEST_F(ConditionalWorkerTest, AddingWorkersStopsAtTheLimitAndReturnsTheDefault)
{
    for (uint32_t i = 1; i < Vruta::TaskScheduler::max_workers; ++i) {
        EXPECT_EQ(scheduler->add_worker("w").index, i);
    }

    EXPECT_EQ(scheduler->add_worker("one too many"), Vruta::WorkerId {});
}

TEST_F(ConditionalWorkerTest, OnWorkerRunsTheRestOfTheRoutineOnTheWorkerThread)
{
    const auto worker = scheduler->add_worker("rewrite");
    auto visit = make_visit();

    add(std::make_shared<Vruta::FreeRoutine>(hop(worker, visit)), "hop");

    ASSERT_TRUE(eventually([&] { return visit->done.load(); }));
    EXPECT_EQ(visit->before, std::this_thread::get_id());
    EXPECT_NE(visit->after, std::this_thread::get_id());
}

TEST_F(ConditionalWorkerTest, RoutinesOnTheSameWorkerShareAThread)
{
    const auto worker = scheduler->add_worker("shared");
    auto one = make_visit();
    auto two = make_visit();

    add(std::make_shared<Vruta::FreeRoutine>(hop(worker, one)), "one");
    add(std::make_shared<Vruta::FreeRoutine>(hop(worker, two)), "two");

    ASSERT_TRUE(eventually([&] { return one->done.load() && two->done.load(); }));
    EXPECT_EQ(one->after, two->after);
}

TEST_F(ConditionalWorkerTest, RoutinesOnDifferentWorkersRunOnDifferentThreads)
{
    const auto left = scheduler->add_worker("left");
    const auto right = scheduler->add_worker("right");
    auto on_left = make_visit();
    auto on_right = make_visit();
    auto on_default = make_visit();

    add(std::make_shared<Vruta::FreeRoutine>(hop(left, on_left)), "left");
    add(std::make_shared<Vruta::FreeRoutine>(hop(right, on_right)), "right");
    add(std::make_shared<Vruta::FreeRoutine>(handshake(on_default)), "default");

    ASSERT_TRUE(eventually([&] { return on_left->done.load() && on_right->done.load() && on_default->done.load(); }));
    EXPECT_NE(on_left->after, on_right->after);
    EXPECT_NE(on_left->after, on_default->after);
    EXPECT_NE(on_right->after, on_default->after);
}

TEST_F(ConditionalWorkerTest, GetFreePromiseHandsTheRoutineToTheDefaultWorker)
{
    auto visit = make_visit();

    add(std::make_shared<Vruta::FreeRoutine>(handshake(visit)), "handshake");

    ASSERT_TRUE(eventually([&] { return visit->done.load(); }));
    EXPECT_EQ(visit->before, std::this_thread::get_id());
    EXPECT_NE(visit->after, std::this_thread::get_id());
}

TEST_F(ConditionalWorkerTest, AnUnknownWorkerFallsBackToTheDefaultWorker)
{
    auto stray = make_visit();
    auto home = make_visit();

    add(std::make_shared<Vruta::FreeRoutine>(hop(Vruta::WorkerId { 99 }, stray)), "stray");
    add(std::make_shared<Vruta::FreeRoutine>(handshake(home)), "home");

    ASSERT_TRUE(eventually([&] { return stray->done.load() && home->done.load(); }));
    EXPECT_EQ(stray->after, home->after);
}

TEST_F(ConditionalWorkerTest, ALongRoutineDelaysOthersOnItsWorker)
{
    const auto worker = scheduler->add_worker("crowded");
    auto heavy = make_visit();
    auto light = make_visit();

    add(std::make_shared<Vruta::FreeRoutine>(work_for(worker, 250ms, heavy)), "heavy");
    ASSERT_TRUE(eventually([&] { return heavy->started.load(); }));
    add(std::make_shared<Vruta::FreeRoutine>(delayed(worker, 0.02, light)), "light");

    ASSERT_TRUE(eventually([&] { return light->done.load(); }));
    EXPECT_TRUE(heavy->done.load());
}

TEST_F(ConditionalWorkerTest, ALongRoutineDoesNotDelayAnotherWorker)
{
    const auto busy = scheduler->add_worker("busy");
    const auto free = scheduler->add_worker("free");
    auto heavy = make_visit();
    auto light = make_visit();

    add(std::make_shared<Vruta::FreeRoutine>(work_for(busy, 400ms, heavy)), "heavy");
    ASSERT_TRUE(eventually([&] { return heavy->started.load(); }));
    add(std::make_shared<Vruta::FreeRoutine>(delayed(free, 0.02, light)), "light");

    ASSERT_TRUE(eventually([&] { return light->done.load(); }, 300ms));
    EXPECT_FALSE(heavy->done.load());
}

TEST_F(ConditionalWorkerTest, WakeConditionalWithNoArgumentWakesEveryWorker)
{
    const auto left = scheduler->add_worker("left");
    const auto right = scheduler->add_worker("right");
    auto flag = std::make_shared<std::atomic<bool>>(false);
    auto on_left = make_visit();
    auto on_right = make_visit();

    add(std::make_shared<Vruta::FreeRoutine>(wake_gated(left, flag, on_left)), "left");
    add(std::make_shared<Vruta::FreeRoutine>(wake_gated(right, flag, on_right)), "right");
    std::this_thread::sleep_for(30ms);

    flag->store(true);
    scheduler->wake_conditional();

    EXPECT_TRUE(eventually([&] { return on_left->done.load() && on_right->done.load(); }));
}

TEST_F(ConditionalWorkerTest, WakeConditionalWithAWorkerWakesOnlyThatWorker)
{
    const auto left = scheduler->add_worker("left");
    const auto right = scheduler->add_worker("right");
    auto flag = std::make_shared<std::atomic<bool>>(false);
    auto on_left = make_visit();
    auto on_right = make_visit();

    add(std::make_shared<Vruta::FreeRoutine>(wake_gated(left, flag, on_left)), "left");
    add(std::make_shared<Vruta::FreeRoutine>(wake_gated(right, flag, on_right)), "right");
    std::this_thread::sleep_for(30ms);

    flag->store(true);
    scheduler->wake_conditional(left);

    EXPECT_TRUE(eventually([&] { return on_left->done.load(); }));
    std::this_thread::sleep_for(100ms);
    EXPECT_FALSE(on_right->done.load());

    scheduler->wake_conditional();
    EXPECT_TRUE(eventually([&] { return on_right->done.load(); }));
}

TEST_F(ConditionalWorkerTest, CancellingByNameReachesARoutineOnAnotherWorker)
{
    const auto worker = scheduler->add_worker("cancel");
    auto flag = std::make_shared<std::atomic<bool>>(false);
    auto visit = make_visit();
    auto completed = std::make_shared<std::atomic<bool>>(false);

    auto routine = std::make_shared<Vruta::FreeRoutine>(wake_gated(worker, flag, visit));
    routine->on_complete([completed]() { completed->store(true); });
    add(routine, "cancel_by_name");
    std::this_thread::sleep_for(30ms);

    EXPECT_TRUE(scheduler->cancel_task("cancel_by_name"));

    EXPECT_TRUE(eventually([&] { return completed->load(); }, 1000ms));
}

TEST_F(ConditionalWorkerTest, CancellingByRoutineReachesARoutineOnAnotherWorker)
{
    const auto worker = scheduler->add_worker("cancel");
    auto flag = std::make_shared<std::atomic<bool>>(false);
    auto visit = make_visit();
    auto completed = std::make_shared<std::atomic<bool>>(false);

    auto routine = std::make_shared<Vruta::FreeRoutine>(wake_gated(worker, flag, visit));
    routine->on_complete([completed]() { completed->store(true); });
    add(routine, "cancel_by_routine");
    ASSERT_TRUE(eventually([&] { return !scheduler->get_tasks_for_token(Vruta::ProcessingToken::CONDITIONAL).empty(); }));

    EXPECT_TRUE(scheduler->cancel_task(routine));

    EXPECT_TRUE(eventually([&] { return completed->load(); }, 1000ms));
}

TEST_F(ConditionalWorkerTest, ShutdownStopsEveryWorker)
{
    auto flag = std::make_shared<std::atomic<bool>>(false);
    std::vector<VisitPtr> visits;
    for (const char* name : { "a", "b", "c" }) {
        const auto worker = scheduler->add_worker(name);
        visits.push_back(make_visit());
        add(std::make_shared<Vruta::FreeRoutine>(wake_gated(worker, flag, visits.back())), name);
    }
    std::this_thread::sleep_for(30ms);

    auto terminated = std::async(std::launch::async, [this]() { scheduler->terminate_all_tasks(); });

    EXPECT_EQ(terminated.wait_for(2s), std::future_status::ready);
}

}
