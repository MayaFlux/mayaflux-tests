#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kriya/Chain.hpp"
#include "MayaFlux/Vruta/BroadcastSource.hpp"
#include "MayaFlux/Vruta/Scheduler.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

using SchedToken = Vruta::ProcessingToken;

namespace {

    constexpr unsigned int SAMPLE_RATE = 48000;

} // namespace

class ChainStepsTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        scheduler = std::make_shared<Vruta::TaskScheduler>(SAMPLE_RATE);
    }

    void TearDown() override
    {
        scheduler.reset();
    }

    void run_samples(uint64_t samples)
    {
        for (uint64_t i = 0; i < samples; ++i) {
            scheduler->process_token(SchedToken::SAMPLE_ACCURATE, 1);
        }
    }

    void run_seconds(double seconds)
    {
        run_samples(scheduler->seconds_to_samples(seconds));
    }

    std::shared_ptr<Vruta::TaskScheduler> scheduler;
};

//-----------------------------------------------------------------------------
// on
//-----------------------------------------------------------------------------

TEST_F(ChainStepsTest, OnRunsTheActionWithTheSignalledValue)
{
    auto source = std::make_shared<Vruta::BroadcastSource<int>>();
    std::vector<int> values;

    Kriya::EventChain chain(scheduler);
    chain.on<int>(source, [&values](const int& value) { values.push_back(value); }).start();

    run_samples(5);
    EXPECT_TRUE(values.empty());
    EXPECT_TRUE(chain.is_active());

    source->signal(7);
    run_samples(2);

    EXPECT_EQ(values, std::vector<int> { 7 });
    EXPECT_FALSE(chain.is_active());
}

TEST_F(ChainStepsTest, OnHoldsTheLaterStepsUntilItsSignal)
{
    auto source = std::make_shared<Vruta::BroadcastSource<int>>();
    std::vector<std::string> order;

    Kriya::EventChain chain(scheduler);
    chain.then([&order]() { order.emplace_back("first"); })
        .on<int>(source, [&order](const int&) { order.emplace_back("signal"); })
        .then([&order]() { order.emplace_back("after"); }, 0.01)
        .start();

    run_seconds(0.02);
    EXPECT_EQ(order, std::vector<std::string> { "first" });

    source->signal(1);
    run_samples(2);
    EXPECT_EQ(order, (std::vector<std::string> { "first", "signal" }));

    run_seconds(0.015);
    EXPECT_EQ(order, (std::vector<std::string> { "first", "signal", "after" }));
}

TEST_F(ChainStepsTest, OnCollapsesSignalsBetweenChecksToTheLast)
{
    auto source = std::make_shared<Vruta::BroadcastSource<int>>();
    std::vector<int> values;

    Kriya::EventChain chain(scheduler);
    chain.on<int>(source, [&values](const int& value) { values.push_back(value); }).start();

    source->signal(1);
    source->signal(2);
    run_samples(2);

    EXPECT_EQ(values, std::vector<int> { 2 });
}

TEST_F(ChainStepsTest, OnRunsWhereTheChainRunsNotWhereTheSignalCame)
{
    auto source = std::make_shared<Vruta::BroadcastSource<int>>();
    std::thread::id acted_on;

    Kriya::EventChain chain(scheduler);
    chain.on<int>(source, [&acted_on](const int&) { acted_on = std::this_thread::get_id(); }).start();

    std::thread signaller([&source]() { source->signal(1); });
    signaller.join();
    run_samples(2);

    EXPECT_EQ(acted_on, std::this_thread::get_id());
}

//-----------------------------------------------------------------------------
// when
//-----------------------------------------------------------------------------

TEST_F(ChainStepsTest, WhenRunsOnceTheConditionHolds)
{
    bool ready = false;
    int count = 0;

    Kriya::EventChain chain(scheduler);
    chain.when([&ready]() { return ready; }, [&count]() { ++count; }).start();

    run_samples(5);
    EXPECT_EQ(count, 0);

    ready = true;
    run_samples(2);

    EXPECT_EQ(count, 1);
    EXPECT_FALSE(chain.is_active());
}

TEST_F(ChainStepsTest, WhenWithAThrowingConditionEndsTheStepAndContinues)
{
    int acted = 0;
    int after = 0;

    Kriya::EventChain chain(scheduler);
    chain.when([]() -> bool { throw std::runtime_error("condition"); }, [&acted]() { ++acted; })
        .then([&after]() { ++after; })
        .start();

    run_samples(2);

    EXPECT_EQ(acted, 0);
    EXPECT_EQ(after, 1);
}

TEST_F(ChainStepsTest, WhenWithAnEmptyConditionAddsNoStep)
{
    Kriya::EventChain chain(scheduler);
    chain.when({}, []() { });

    EXPECT_EQ(chain.event_count(), 0U);
}

//-----------------------------------------------------------------------------
// forever and until
//-----------------------------------------------------------------------------

TEST_F(ChainStepsTest, ForeverKeepsRepeatingUntilCancelled)
{
    int count = 0;

    Kriya::EventChain chain(scheduler);
    chain.then([&count]() { ++count; }, 0.001).forever().start();

    run_seconds(0.02);
    EXPECT_GT(count, 5);
    EXPECT_TRUE(chain.is_active());

    chain.cancel();
    const int stopped = count;
    run_seconds(0.02);

    EXPECT_EQ(count, stopped);
}

TEST_F(ChainStepsTest, ForeverWithZeroDelaysDoesNotHoldTheScheduler)
{
    int count = 0;

    Kriya::EventChain chain(scheduler);
    chain.then([&count]() { ++count; }).forever().start();

    run_samples(10);

    EXPECT_GT(count, 1);
    EXPECT_LE(count, 12);

    chain.cancel();
}

TEST_F(ChainStepsTest, UntilStopsAfterThePassInWhichTheConditionHolds)
{
    int count = 0;

    Kriya::EventChain chain(scheduler);
    chain.then([&count]() { ++count; }, 0.001)
        .until([&count]() { return count >= 3; })
        .start();

    run_seconds(0.05);

    EXPECT_EQ(count, 3);
    EXPECT_FALSE(chain.is_active());
}

TEST_F(ChainStepsTest, TimesAfterForeverMakesTheChainFiniteAgain)
{
    int count = 0;

    Kriya::EventChain chain(scheduler);
    chain.then([&count]() { ++count; }, 0.001).forever().times(2).start();

    run_seconds(0.05);

    EXPECT_EQ(count, 2);
    EXPECT_FALSE(chain.is_active());
}

TEST_F(ChainStepsTest, ForeverAfterUntilDropsTheCondition)
{
    int count = 0;

    Kriya::EventChain chain(scheduler);
    chain.then([&count]() { ++count; }, 0.001)
        .until([]() { return true; })
        .forever()
        .start();

    run_seconds(0.02);

    EXPECT_GT(count, 5);
    chain.cancel();
}

//-----------------------------------------------------------------------------
// parallel and fork
//-----------------------------------------------------------------------------

TEST_F(ChainStepsTest, ParallelRunsTheLanesSideBySideAndWaitsForAll)
{
    std::vector<std::string> order;

    Kriya::EventChain chain(scheduler);
    chain.then([&order]() { order.emplace_back("first"); })
        .parallel({ [&order](Kriya::EventChain& lane) {
                       lane.then([&order]() { order.emplace_back("a1"); }, 0.01)
                           .then([&order]() { order.emplace_back("a2"); }, 0.01);
                   },
            [&order](Kriya::EventChain& lane) {
                lane.then([&order]() { order.emplace_back("b1"); }, 0.015);
            } })
        .then([&order]() { order.emplace_back("after"); })
        .start();

    run_seconds(0.012);
    EXPECT_EQ(order, (std::vector<std::string> { "first", "a1" }));

    run_seconds(0.006);
    EXPECT_EQ(order, (std::vector<std::string> { "first", "a1", "b1" }));

    run_seconds(0.006);
    EXPECT_EQ(order, (std::vector<std::string> { "first", "a1", "b1", "a2", "after" }));
    EXPECT_FALSE(chain.is_active());
}

TEST_F(ChainStepsTest, ParallelWithLanesThatFinishAtOnceContinuesAtOnce)
{
    int lane_ran = 0;
    int after = 0;

    Kriya::EventChain chain(scheduler);
    chain.parallel({ [&lane_ran](Kriya::EventChain& lane) { lane.then([&lane_ran]() { ++lane_ran; }); } })
        .then([&after]() { ++after; })
        .start();

    run_samples(3);

    EXPECT_EQ(lane_ran, 1);
    EXPECT_EQ(after, 1);
}

TEST_F(ChainStepsTest, ParallelWithNoUsableLanesAddsNoStep)
{
    Kriya::EventChain chain(scheduler);

    chain.parallel({});
    chain.parallel({ nullptr });
    chain.fork({});

    EXPECT_EQ(chain.event_count(), 0U);
}

TEST_F(ChainStepsTest, ParallelLanesMayAddLanesOfTheirOwn)
{
    std::vector<std::string> order;

    Kriya::EventChain chain(scheduler);
    chain.parallel({ [&order](Kriya::EventChain& lane) {
                        lane.parallel({ [&order](Kriya::EventChain& inner) {
                                           inner.then([&order]() { order.emplace_back("x"); }, 0.005);
                                       },
                                  [&order](Kriya::EventChain& inner) {
                                      inner.then([&order]() { order.emplace_back("y"); }, 0.01);
                                  } })
                            .then([&order]() { order.emplace_back("lane done"); });
                    } })
        .then([&order]() { order.emplace_back("after"); })
        .start();

    run_seconds(0.02);

    EXPECT_EQ(order, (std::vector<std::string> { "x", "y", "lane done", "after" }));
}

TEST_F(ChainStepsTest, ParallelInsideForeverStartsTheLanesAgainEachPass)
{
    int count = 0;

    Kriya::EventChain chain(scheduler);
    chain.parallel({ [&count](Kriya::EventChain& lane) { lane.then([&count]() { ++count; }, 0.002); } })
        .forever()
        .start();

    run_seconds(0.03);
    EXPECT_GE(count, 4);

    chain.cancel();
}

TEST_F(ChainStepsTest, CancelStopsTheLanesOfAParallelStep)
{
    int fired = 0;

    Kriya::EventChain chain(scheduler);
    chain.parallel({ [&fired](Kriya::EventChain& lane) { lane.then([&fired]() { ++fired; }, 0.05); } }).start();

    run_seconds(0.01);
    chain.cancel();
    run_seconds(0.1);

    EXPECT_EQ(fired, 0);
}

TEST_F(ChainStepsTest, ForkContinuesAtOnceWhileTheLanesRun)
{
    std::vector<std::string> order;

    Kriya::EventChain chain(scheduler);
    chain.fork({ [&order](Kriya::EventChain& lane) { lane.then([&order]() { order.emplace_back("lane"); }, 0.01); } })
        .then([&order]() { order.emplace_back("after"); })
        .start();

    run_samples(3);
    EXPECT_EQ(order, std::vector<std::string> { "after" });
    EXPECT_FALSE(chain.is_active());

    run_seconds(0.015);
    EXPECT_EQ(order, (std::vector<std::string> { "after", "lane" }));
}

TEST_F(ChainStepsTest, CancelStopsForkedLanes)
{
    int fired = 0;

    Kriya::EventChain chain(scheduler);
    chain.fork({ [&fired](Kriya::EventChain& lane) { lane.then([&fired]() { ++fired; }, 0.05); } })
        .wait(0.2)
        .start();

    run_seconds(0.01);
    chain.cancel();
    run_seconds(0.1);

    EXPECT_EQ(fired, 0);
}

}
