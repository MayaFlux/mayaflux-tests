#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kriya/Chain.hpp"
#include "MayaFlux/Vruta/BroadcastSource.hpp"
#include "MayaFlux/Vruta/Scheduler.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

using namespace std::chrono_literals;
using SchedToken = Vruta::ProcessingToken;

namespace {

    constexpr unsigned int SAMPLE_RATE = 48000;

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

class WorkerChainTest : public ::testing::Test {
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

    Kriya::EventChain make_chain(const std::string& name)
    {
        return Kriya::EventChain(scheduler, name, SchedToken::CONDITIONAL);
    }

    std::shared_ptr<Vruta::TaskScheduler> scheduler;
};

TEST_F(WorkerChainTest, ARunsItsStepsOffTheCreatingThread)
{
    std::atomic<bool> done { false };
    std::thread::id ran_on;

    auto chain = make_chain("off_thread");
    chain.then([&]() {
        ran_on = std::this_thread::get_id();
        done.store(true);
    }).start();

    ASSERT_TRUE(eventually([&] { return done.load(); }));
    EXPECT_NE(ran_on, std::this_thread::get_id());
}

TEST_F(WorkerChainTest, OnWorkerPutsTheChainOnThatWorkersThread)
{
    const auto worker = scheduler->add_worker("chains");
    std::atomic<int> done { 0 };
    std::thread::id on_default;
    std::thread::id on_worker;

    auto home = make_chain("home");
    home.then([&]() {
            on_default = std::this_thread::get_id();
            done.fetch_add(1);
        })
        .start();

    auto away = make_chain("away");
    away.on_worker(worker)
        .then([&]() {
            on_worker = std::this_thread::get_id();
            done.fetch_add(1);
        })
        .start();

    ASSERT_TRUE(eventually([&] { return done.load() == 2; }));
    EXPECT_NE(on_default, on_worker);
}

TEST_F(WorkerChainTest, DelaysAreWallClockSeconds)
{
    std::atomic<bool> done { false };
    const auto start = std::chrono::steady_clock::now();

    auto chain = make_chain("wall_clock");
    chain.then([&]() { done.store(true); }, 0.05).start();

    ASSERT_TRUE(eventually([&] { return done.load(); }));
    EXPECT_GE(std::chrono::steady_clock::now() - start, 50ms);
}

TEST_F(WorkerChainTest, OnRunsTheActionWithTheSignalledValueOnTheWorker)
{
    auto source = std::make_shared<Vruta::BroadcastSource<int>>();
    std::atomic<int> value { 0 };
    std::thread::id ran_on;

    auto chain = make_chain("signal");
    chain.on<int>(source, [&](const int& signalled) {
             ran_on = std::this_thread::get_id();
             value.store(signalled);
         })
        .start();
    std::this_thread::sleep_for(20ms);

    source->signal(7);

    ASSERT_TRUE(eventually([&] { return value.load() == 7; }));
    EXPECT_NE(ran_on, std::this_thread::get_id());
}

TEST_F(WorkerChainTest, WhenRunsOnceTheConditionHolds)
{
    std::atomic<bool> ready { false };
    std::atomic<int> count { 0 };

    auto chain = make_chain("condition");
    chain.when([&]() { return ready.load(); }, [&]() { count.fetch_add(1); }).start();
    std::this_thread::sleep_for(20ms);
    EXPECT_EQ(count.load(), 0);

    ready.store(true);

    EXPECT_TRUE(eventually([&] { return count.load() == 1; }));
}

TEST_F(WorkerChainTest, ForeverWithZeroDelaysPacesItselfAtThePollInterval)
{
    std::atomic<int> count { 0 };

    auto chain = make_chain("paced");
    chain.then([&]() { count.fetch_add(1); }).forever().start();

    std::this_thread::sleep_for(100ms);
    const int seen = count.load();
    chain.cancel();

    EXPECT_GT(seen, 10);
    EXPECT_LT(seen, 200);

    std::this_thread::sleep_for(50ms);
    const int stopped = count.load();
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(count.load(), stopped);
}

TEST_F(WorkerChainTest, UntilEndsTheChain)
{
    std::atomic<int> count { 0 };

    auto chain = make_chain("until");
    chain.then([&]() { count.fetch_add(1); }, 0.005)
        .until([&]() { return count.load() >= 3; })
        .start();

    ASSERT_TRUE(eventually([&] { return !chain.is_active(); }));
    EXPECT_EQ(count.load(), 3);
}

TEST_F(WorkerChainTest, ParallelLanesStartOnTheParentsWorkerAndTheChainWaitsForThem)
{
    const auto worker = scheduler->add_worker("lanes");
    std::mutex guard;
    std::vector<std::thread::id> threads;
    std::atomic<bool> after { false };

    const auto record = [&]() {
        const std::lock_guard lock(guard);
        threads.push_back(std::this_thread::get_id());
    };

    auto chain = make_chain("lanes");
    chain.on_worker(worker)
        .parallel({ [&](Kriya::EventChain& lane) { lane.then(record, 0.01); },
            [&](Kriya::EventChain& lane) { lane.then(record, 0.02); } })
        .then([&]() {
            record();
            after.store(true);
        })
        .start();

    ASSERT_TRUE(eventually([&] { return after.load(); }));

    const std::lock_guard lock(guard);
    ASSERT_EQ(threads.size(), 3U);
    EXPECT_EQ(threads[0], threads[1]);
    EXPECT_EQ(threads[1], threads[2]);
    EXPECT_NE(threads[0], std::this_thread::get_id());
}

TEST_F(WorkerChainTest, ALaneMayChooseAnotherWorker)
{
    const auto parent_worker = scheduler->add_worker("parent");
    const auto lane_worker = scheduler->add_worker("lane");
    std::atomic<int> done { 0 };
    std::thread::id parent_thread;
    std::thread::id lane_thread;

    auto chain = make_chain("lane_elsewhere");
    chain.on_worker(parent_worker)
        .parallel({ [&](Kriya::EventChain& lane) {
            lane.on_worker(lane_worker).then([&]() {
                lane_thread = std::this_thread::get_id();
                done.fetch_add(1);
            });
        } })
        .then([&]() {
            parent_thread = std::this_thread::get_id();
            done.fetch_add(1);
        })
        .start();

    ASSERT_TRUE(eventually([&] { return done.load() == 2; }));
    EXPECT_NE(parent_thread, lane_thread);
}

}
