#include "gtest/gtest.h"

#include "MayaFlux/Kriya/BroadcastEvents.hpp"
#include "MayaFlux/Vruta/BroadcastSource.hpp"
#include "MayaFlux/Vruta/Event.hpp"
#include "MayaFlux/Vruta/EventManager.hpp"

namespace MayaFlux::Test {

namespace {

    Vruta::Event one_value(Vruta::BroadcastSource<int>& source, std::vector<int>* seen)
    {
        auto value = co_await source.next();
        seen->push_back(value);
    }

    Vruta::Event each_value(Vruta::BroadcastSource<int>& source, std::vector<int>* seen)
    {
        for (;;) {
            auto value = co_await source.next();
            seen->push_back(value);
        }
    }

} // namespace

TEST(BroadcastAwaiterTest, SuspendedHandlerReceivesTheNextSignalledValue)
{
    Vruta::BroadcastSource<int> source;
    std::vector<int> seen;
    auto handler = one_value(source, &seen);

    EXPECT_TRUE(handler.is_active());
    EXPECT_TRUE(seen.empty());

    source.signal(7);

    EXPECT_EQ(seen, std::vector<int> { 7 });
    EXPECT_FALSE(handler.is_active());
    EXPECT_FALSE(source.has_pending());
}

TEST(BroadcastAwaiterTest, DestroyedSuspendedAwaiterLeavesTheValuePending)
{
    Vruta::BroadcastSource<int> source;
    std::vector<int> first;
    std::vector<int> second;
    {
        auto handler = one_value(source, &first);
        EXPECT_TRUE(handler.is_active());
    }

    source.signal(3);

    EXPECT_TRUE(first.empty());
    EXPECT_TRUE(source.has_pending());

    auto next = one_value(source, &second);

    EXPECT_EQ(second, std::vector<int> { 3 });
    EXPECT_FALSE(next.is_active());
    EXPECT_FALSE(source.has_pending());
}

TEST(BroadcastAwaiterTest, ReawaitingHandlerReceivesEachValueOnce)
{
    Vruta::BroadcastSource<int> source;
    std::vector<int> seen;
    auto handler = each_value(source, &seen);

    source.signal(1);
    source.signal(2);
    source.signal(3);

    EXPECT_EQ(seen, (std::vector<int> { 1, 2, 3 }));
    EXPECT_TRUE(handler.is_active());
    EXPECT_FALSE(source.has_pending());
}

TEST(BroadcastAwaiterTest, ValueDeliveredToAWaiterIsNotRedeliveredToALaterOne)
{
    Vruta::BroadcastSource<int> source;
    std::vector<int> first;
    std::vector<int> second;
    auto a = one_value(source, &first);
    source.signal(5);

    auto b = one_value(source, &second);

    EXPECT_EQ(first, std::vector<int> { 5 });
    EXPECT_TRUE(second.empty());
    EXPECT_TRUE(b.is_active());
}

TEST(BroadcastOnSignalTest, CallbackRunsForEachSignalledValue)
{
    Vruta::EventManager manager;
    auto source = std::make_shared<Vruta::BroadcastSource<int>>();
    std::vector<int> seen;
    auto handler = std::make_shared<Vruta::Event>(
        Kriya::on_signal(source, [&seen](int value) { seen.push_back(value); }));
    manager.add_event(handler, "each");

    source->signal(4);
    source->signal(5);

    EXPECT_EQ(seen, (std::vector<int> { 4, 5 }));
    EXPECT_TRUE(handler->is_active());
}

TEST(BroadcastOnSignalTest, OnSignalMatchingFiresOnlyForAcceptedValues)
{
    Vruta::EventManager manager;
    auto source = std::make_shared<Vruta::BroadcastSource<int>>();
    std::vector<int> seen;
    auto handler = std::make_shared<Vruta::Event>(Kriya::on_signal_matching(
        source,
        [](int value) { return value % 2 == 0; },
        [&seen](int value) { seen.push_back(value); }));
    manager.add_event(handler, "matching");

    source->signal(1);
    source->signal(2);
    source->signal(3);
    source->signal(4);

    EXPECT_EQ(seen, (std::vector<int> { 2, 4 }));
    EXPECT_TRUE(handler->is_active());
}

TEST(BroadcastOnSignalTest, StopRequestSuppressesTheCallbackForTheWakingValue)
{
    Vruta::EventManager manager;
    auto source = std::make_shared<Vruta::BroadcastSource<int>>();
    std::vector<int> seen;
    auto handler = std::make_shared<Vruta::Event>(
        Kriya::on_signal(source, [&seen](int value) { seen.push_back(value); }));
    manager.add_event(handler, "stopping");

    handler->set_should_terminate(true);
    source->signal(8);

    EXPECT_TRUE(seen.empty());
    EXPECT_FALSE(handler->is_active());
}

TEST(BroadcastOnSignalTest, StopRequestSuppressesTheCallbackOnMatchingSignals)
{
    Vruta::EventManager manager;
    auto source = std::make_shared<Vruta::BroadcastSource<int>>();
    std::vector<int> seen;
    auto handler = std::make_shared<Vruta::Event>(Kriya::on_signal_matching(
        source,
        [](int value) { return value % 2 == 0; },
        [&seen](int value) { seen.push_back(value); }));
    manager.add_event(handler, "stopping_matching");

    handler->set_should_terminate(true);
    source->signal(4);

    EXPECT_TRUE(seen.empty());
    EXPECT_FALSE(handler->is_active());
}

TEST(BroadcastSubscribeTest, CancelledHeldHandlerDoesNotRunItsCallbackOnTheNextValue)
{
    Vruta::EventManager manager;
    auto source = std::make_shared<Vruta::BroadcastSource<int>>();
    std::vector<int> seen;
    auto held = Kriya::subscribe(manager, source, [&seen](int value) { seen.push_back(value); }, "held");

    source->signal(1);
    EXPECT_EQ(seen, std::vector<int> { 1 });

    EXPECT_TRUE(manager.cancel_event(std::string("held")));
    source->signal(2);

    EXPECT_EQ(seen, std::vector<int> { 1 });
    EXPECT_FALSE(held->is_active());
    EXPECT_FALSE(source->has_pending());
}

TEST(BroadcastSubscribeTest, SubscribeRegistersTheHandlerAndDeliversSignals)
{
    Vruta::EventManager manager;
    auto source = std::make_shared<Vruta::BroadcastSource<int>>();
    std::vector<int> seen;

    auto handler = Kriya::subscribe(manager, source, [&seen](int value) { seen.push_back(value); }, "listener");

    EXPECT_NE(manager.get_event("listener"), nullptr);
    source->signal(6);

    EXPECT_EQ(seen, std::vector<int> { 6 });
    EXPECT_TRUE(handler->is_active());
}

TEST(BroadcastSubscribeTest, ValueSignalledBeforeSubscribeIsDeliveredToTheNewListener)
{
    Vruta::EventManager manager;
    auto source = std::make_shared<Vruta::BroadcastSource<int>>();
    std::vector<int> seen;
    source->signal(9);

    auto handler = Kriya::subscribe(manager, source, [&seen](int value) { seen.push_back(value); }, "early");

    EXPECT_EQ(seen, std::vector<int> { 9 });
    EXPECT_FALSE(source->has_pending());
}

} // namespace MayaFlux::Test
