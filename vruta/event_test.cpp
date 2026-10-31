#include "gtest/gtest.h"

#include "MayaFlux/IO/Keys.hpp"
#include "MayaFlux/Kriya/Awaiters/EventAwaiter.hpp"
#include "MayaFlux/Kriya/Awaiters/GetPromise.hpp"
#include "MayaFlux/Vruta/Event.hpp"
#include "MayaFlux/Vruta/EventManager.hpp"
#include "MayaFlux/Vruta/WindowEventSource.hpp"

namespace MayaFlux::Test {

namespace {

    struct Log {
        std::vector<double> seen;
        std::vector<std::string> order;
    };

    Core::WindowEvent make_event(Core::WindowEventType type, double timestamp)
    {
        Core::WindowEvent event;
        event.type = type;
        event.timestamp = timestamp;
        return event;
    }

    Core::WindowEvent key_event(Core::WindowEventType type, IO::Keys key, double timestamp)
    {
        auto event = make_event(type, timestamp);
        event.data = Core::WindowEvent::KeyData { static_cast<int16_t>(key), 0, 0 };
        return event;
    }

    Core::WindowEvent button_event(Core::WindowEventType type, int8_t button, double timestamp)
    {
        auto event = make_event(type, timestamp);
        event.data = Core::WindowEvent::MouseButtonData { button, 0 };
        return event;
    }

    Core::WindowEvent position_event(double x, double y)
    {
        auto event = make_event(Core::WindowEventType::MOUSE_MOTION, 0.0);
        event.data = Core::WindowEvent::MousePosData { x, y };
        return event;
    }

    Vruta::Event one_shot(Vruta::WindowEventSource& source, Log* log)
    {
        auto event = co_await source.next_event();
        log->seen.push_back(event.timestamp);
    }

    Vruta::Event loop_handler(Vruta::WindowEventSource& source, Log* log)
    {
        for (;;) {
            auto event = co_await source.next_event();
            log->seen.push_back(event.timestamp);
        }
    }

    Vruta::Event typed_handler(Vruta::WindowEventSource& source, Core::WindowEventType type, Log* log)
    {
        auto event = co_await source.await_event(type);
        log->seen.push_back(event.timestamp);
    }

    Vruta::Event key_handler(Vruta::WindowEventSource& source, IO::Keys key, Log* log)
    {
        auto event = co_await Kriya::WindowEventAwaiter { source, Vruta::WindowEventFilter { key } };
        log->seen.push_back(event.timestamp);
    }

    Vruta::Event button_handler(Vruta::WindowEventSource& source, IO::MouseButtons button, Log* log)
    {
        Vruta::WindowEventFilter filter;
        filter.button = button;
        auto event = co_await Kriya::WindowEventAwaiter { source, filter };
        log->seen.push_back(event.timestamp);
    }

    Vruta::Event kicked_handler(Vruta::WindowEventSource& source, Log* log)
    {
        [[maybe_unused]] auto& promise = co_await Kriya::GetEventPromise {};
        auto event = co_await source.next_event();
        log->seen.push_back(event.timestamp);
    }

    struct Sentinel {
        int* destroyed;

        ~Sentinel() { ++*destroyed; }
    };

    Vruta::Event sentinel_handler(Vruta::WindowEventSource& source, int* destroyed)
    {
        Sentinel sentinel { destroyed };
        co_await source.next_event();
    }

    Vruta::Event deferring_handler(Vruta::WindowEventSource& source, Log* log)
    {
        auto event = co_await source.next_event();
        log->seen.push_back(event.timestamp);
        log->order.push_back("handler");
        source.defer([log] { log->order.push_back("deferred"); });
    }

} // namespace

TEST(EventCoroutineTest, SuspendsUntilTheSignalAndCompletes)
{
    Vruta::WindowEventSource source;
    Log log;
    auto handler = one_shot(source, &log);

    EXPECT_TRUE(handler.is_active());
    EXPECT_EQ(handler.get_processing_token(), Vruta::ProcessingToken::EVENT_DRIVEN);
    EXPECT_TRUE(log.seen.empty());

    source.signal(make_event(Core::WindowEventType::WINDOW_CREATED, 1.0));

    EXPECT_EQ(log.seen, std::vector<double> { 1.0 });
    EXPECT_FALSE(handler.is_active());
    EXPECT_TRUE(handler.done());
}

TEST(EventCoroutineTest, OnCompleteFiresOnceOnNaturalCompletion)
{
    Vruta::WindowEventSource source;
    Log log;
    int completions = 0;
    auto handler = one_shot(source, &log);
    handler.on_complete([&completions] { ++completions; });

    source.signal(make_event(Core::WindowEventType::WINDOW_CREATED, 1.0));
    EXPECT_EQ(completions, 1);

    source.signal(make_event(Core::WindowEventType::WINDOW_CREATED, 2.0));
    EXPECT_EQ(completions, 1);
}

TEST(EventCoroutineTest, OnCompleteRegisteredAfterCompletionRunsImmediately)
{
    Vruta::WindowEventSource source;
    Log log;
    int completions = 0;
    auto handler = one_shot(source, &log);

    source.signal(make_event(Core::WindowEventType::WINDOW_CREATED, 1.0));
    handler.on_complete([&completions] { ++completions; });

    EXPECT_EQ(completions, 1);
}

TEST(EventCoroutineTest, DestroyingASuspendedHandlerUnregistersItsWaiter)
{
    Vruta::WindowEventSource source;
    Log log;
    {
        auto handler = loop_handler(source, &log);
        EXPECT_TRUE(handler.is_active());
    }

    source.signal(make_event(Core::WindowEventType::WINDOW_CREATED, 1.0));

    EXPECT_TRUE(log.seen.empty());
    EXPECT_EQ(source.pending_count(), 1U);
}

TEST(EventCoroutineTest, CancelledHandlerRunsOnceMoreOnTheNextMatchingInput)
{
    Vruta::WindowEventSource source;
    Log log;
    auto handler = loop_handler(source, &log);

    handler.set_should_terminate(true);
    source.signal(make_event(Core::WindowEventType::WINDOW_CREATED, 1.0));

    EXPECT_EQ(log.seen, std::vector<double> { 1.0 });
    EXPECT_TRUE(handler.is_active());
}

TEST(EventCoroutineTest, StateDictionaryStoresTypedValues)
{
    Vruta::WindowEventSource source;
    Log log;
    auto handler = one_shot(source, &log);

    handler.set_state("count", 3);

    const int* count = handler.get_state<int>("count");
    ASSERT_NE(count, nullptr);
    EXPECT_EQ(*count, 3);
    EXPECT_EQ(handler.get_state<float>("count"), nullptr);
}

TEST(EventCoroutineTest, UnaddedHandlerStaysAtItsKickAndDoesNotReceiveEvents)
{
    Vruta::WindowEventSource source;
    Log log;
    auto handler = kicked_handler(source, &log);

    EXPECT_TRUE(handler.is_active());
    source.signal(make_event(Core::WindowEventType::WINDOW_CREATED, 1.0));

    EXPECT_TRUE(log.seen.empty());
    EXPECT_TRUE(handler.is_active());
    EXPECT_EQ(source.pending_count(), 1U);
}

TEST(EventManagerKickTest, AddPassesTheKickAndTheHandlerWaitsForItsEvent)
{
    Vruta::WindowEventSource source;
    Vruta::EventManager manager;
    Log log;
    auto handler = std::make_shared<Vruta::Event>(kicked_handler(source, &log));

    manager.add_event(handler, "kicked");

    EXPECT_TRUE(handler->is_active());
    EXPECT_TRUE(log.seen.empty());
    EXPECT_EQ(source.pending_count(), 0U);

    source.signal(make_event(Core::WindowEventType::WINDOW_CREATED, 4.0));

    EXPECT_EQ(log.seen, std::vector<double> { 4.0 });
    EXPECT_FALSE(handler->is_active());
}

TEST(EventManagerKickTest, PendingEventIsDeliveredWhenTheHandlerIsAdded)
{
    Vruta::WindowEventSource source;
    Vruta::EventManager manager;
    Log log;
    source.signal(make_event(Core::WindowEventType::WINDOW_CREATED, 1.0));
    auto handler = std::make_shared<Vruta::Event>(kicked_handler(source, &log));

    manager.add_event(handler, "kicked");

    EXPECT_EQ(log.seen, std::vector<double> { 1.0 });
    EXPECT_FALSE(handler->is_active());
    EXPECT_FALSE(source.has_pending());
}

TEST(EventCoroutineTest, CopyIsDeleted)
{
    EXPECT_FALSE(std::is_copy_constructible_v<Vruta::Event>);
    EXPECT_FALSE(std::is_copy_assignable_v<Vruta::Event>);
}

TEST(EventCoroutineTest, MovedFromEventReportsTerminatedAndIgnoresSetTerminate)
{
    Vruta::WindowEventSource source;
    Log log;
    Vruta::Event origin = one_shot(source, &log);
    Vruta::Event target(std::move(origin));

    EXPECT_TRUE(origin.get_should_terminate());
    origin.set_should_terminate(false);
    EXPECT_TRUE(origin.get_should_terminate());
    EXPECT_FALSE(target.get_should_terminate());
    EXPECT_TRUE(target.is_active());
}

TEST(EventCoroutineTest, MoveAssignmentDestroysTheOldFrame)
{
    Vruta::WindowEventSource source;
    int first = 0;
    int second = 0;
    Vruta::Event replacement = sentinel_handler(source, &second);
    Vruta::Event target = sentinel_handler(source, &first);

    target = std::move(replacement);

    EXPECT_EQ(first, 1);
    EXPECT_EQ(second, 0);
    EXPECT_TRUE(target.is_active());
    EXPECT_FALSE(replacement.is_active());
}

TEST(WindowEventSourceTest, SignalWithoutWaiterIsKeptPending)
{
    Vruta::WindowEventSource source;

    source.signal(make_event(Core::WindowEventType::WINDOW_CREATED, 1.0));

    EXPECT_TRUE(source.has_pending());
    EXPECT_EQ(source.pending_count(), 1U);
}

TEST(WindowEventSourceTest, PendingEventIsDeliveredWithoutSuspending)
{
    Vruta::WindowEventSource source;
    Log log;
    source.signal(make_event(Core::WindowEventType::WINDOW_CREATED, 1.0));

    auto handler = one_shot(source, &log);

    EXPECT_EQ(log.seen, std::vector<double> { 1.0 });
    EXPECT_FALSE(handler.is_active());
    EXPECT_FALSE(source.has_pending());
}

TEST(WindowEventSourceTest, ResumedEventIsNotKeptPending)
{
    Vruta::WindowEventSource source;
    Log log;
    auto handler = one_shot(source, &log);

    source.signal(make_event(Core::WindowEventType::WINDOW_CREATED, 1.0));

    EXPECT_FALSE(source.has_pending());
    EXPECT_EQ(source.pending_count(), 0U);
}

TEST(WindowEventSourceTest, TypedAwaiterSkipsOtherTypesAndLeavesThemPending)
{
    Vruta::WindowEventSource source;
    Log log;
    source.signal(make_event(Core::WindowEventType::MOUSE_MOTION, 1.0));
    source.signal(make_event(Core::WindowEventType::KEY_PRESSED, 2.0));

    auto handler = typed_handler(source, Core::WindowEventType::KEY_PRESSED, &log);

    EXPECT_EQ(log.seen, std::vector<double> { 2.0 });
    EXPECT_FALSE(handler.is_active());
    EXPECT_EQ(source.pending_count(), 1U);
}

TEST(WindowEventSourceTest, TypedWaiterResumesOnlyOnItsType)
{
    Vruta::WindowEventSource source;
    Log log;
    auto handler = typed_handler(source, Core::WindowEventType::KEY_PRESSED, &log);

    source.signal(make_event(Core::WindowEventType::MOUSE_MOTION, 1.0));
    EXPECT_TRUE(log.seen.empty());
    EXPECT_TRUE(handler.is_active());
    EXPECT_EQ(source.pending_count(), 1U);

    source.signal(make_event(Core::WindowEventType::KEY_PRESSED, 5.0));

    EXPECT_EQ(log.seen, std::vector<double> { 5.0 });
    EXPECT_FALSE(handler.is_active());
    EXPECT_EQ(source.pending_count(), 1U);
}

TEST(WindowEventSourceTest, KeyFilterSelectsTheMatchingKeyOnly)
{
    Vruta::WindowEventSource source;
    Log log;
    auto handler = key_handler(source, IO::Keys::A, &log);

    source.signal(key_event(Core::WindowEventType::KEY_PRESSED, IO::Keys::B, 1.0));
    EXPECT_TRUE(log.seen.empty());
    EXPECT_TRUE(handler.is_active());

    source.signal(key_event(Core::WindowEventType::KEY_PRESSED, IO::Keys::A, 2.0));

    EXPECT_EQ(log.seen, std::vector<double> { 2.0 });
    EXPECT_FALSE(handler.is_active());
    EXPECT_EQ(source.pending_count(), 1U);
}

TEST(WindowEventSourceTest, KeyFilterDeliversAMatchingPendingEventWithoutSuspending)
{
    Vruta::WindowEventSource source;
    Log log;
    source.signal(key_event(Core::WindowEventType::KEY_PRESSED, IO::Keys::B, 1.0));
    source.signal(key_event(Core::WindowEventType::KEY_PRESSED, IO::Keys::A, 2.0));

    auto handler = key_handler(source, IO::Keys::A, &log);

    EXPECT_EQ(log.seen, std::vector<double> { 2.0 });
    EXPECT_FALSE(handler.is_active());
    EXPECT_EQ(source.pending_count(), 1U);
}

TEST(WindowEventSourceTest, ButtonFilterSelectsTheMatchingButtonOnly)
{
    Vruta::WindowEventSource source;
    Log log;
    auto handler = button_handler(source, IO::MouseButtons::Left, &log);

    source.signal(button_event(Core::WindowEventType::MOUSE_BUTTON_PRESSED, 1, 1.0));
    EXPECT_TRUE(log.seen.empty());

    source.signal(button_event(Core::WindowEventType::MOUSE_BUTTON_PRESSED, 0, 2.0));

    EXPECT_EQ(log.seen, std::vector<double> { 2.0 });
    EXPECT_FALSE(handler.is_active());
}

TEST(WindowEventSourceTest, EveryMatchingWaiterReceivesTheEvent)
{
    Vruta::WindowEventSource source;
    Log first;
    Log second;
    auto a = one_shot(source, &first);
    auto b = one_shot(source, &second);

    source.signal(make_event(Core::WindowEventType::WINDOW_CREATED, 3.0));

    EXPECT_EQ(first.seen, std::vector<double> { 3.0 });
    EXPECT_EQ(second.seen, std::vector<double> { 3.0 });
    EXPECT_FALSE(source.has_pending());
}

TEST(WindowEventSourceTest, PendingCapDropsTheOldestOfThatTypeOnly)
{
    Vruta::WindowEventSource source;
    Log key_log;
    Log motion_log;
    source.signal(key_event(Core::WindowEventType::KEY_PRESSED, IO::Keys::A, 0.0));

    const auto cap = Vruta::WindowEventSource::k_max_pending_events;
    for (size_t i = 1; i <= cap + 1; ++i) {
        source.signal(make_event(Core::WindowEventType::MOUSE_MOTION, static_cast<double>(i)));
    }

    EXPECT_EQ(source.pending_count(), cap + 1);

    auto motion = typed_handler(source, Core::WindowEventType::MOUSE_MOTION, &motion_log);
    EXPECT_EQ(motion_log.seen, std::vector<double> { 2.0 });

    auto key = typed_handler(source, Core::WindowEventType::KEY_PRESSED, &key_log);
    EXPECT_EQ(key_log.seen, std::vector<double> { 0.0 });
}

TEST(WindowEventSourceTest, ClearEmptiesPendingEvents)
{
    Vruta::WindowEventSource source;
    Log log;
    source.signal(make_event(Core::WindowEventType::WINDOW_CREATED, 1.0));
    source.signal(make_event(Core::WindowEventType::MOUSE_MOTION, 2.0));

    source.clear();

    EXPECT_FALSE(source.has_pending());
    EXPECT_EQ(source.pending_count(), 0U);

    auto handler = typed_handler(source, Core::WindowEventType::MOUSE_MOTION, &log);
    EXPECT_TRUE(log.seen.empty());
    EXPECT_TRUE(handler.is_active());
}

TEST(WindowEventSourceTest, DeferRunsImmediatelyOutsideASignal)
{
    Vruta::WindowEventSource source;
    bool ran = false;

    source.defer([&ran] { ran = true; });

    EXPECT_TRUE(ran);
}

TEST(WindowEventSourceTest, DeferredActionRunsAfterTheSignalDispatchFinishes)
{
    Vruta::WindowEventSource source;
    Log log;
    auto handler = deferring_handler(source, &log);

    source.signal(make_event(Core::WindowEventType::WINDOW_CREATED, 1.0));
    log.order.push_back("returned");

    EXPECT_EQ(log.order, (std::vector<std::string> { "handler", "deferred", "returned" }));
}

TEST(WindowEventSourceTest, KeyStateTracksPressAndRelease)
{
    Vruta::WindowEventSource source;

    EXPECT_FALSE(source.is_key_pressed(IO::Keys::A));

    source.signal(key_event(Core::WindowEventType::KEY_PRESSED, IO::Keys::A, 1.0));
    EXPECT_TRUE(source.is_key_pressed(IO::Keys::A));

    source.signal(key_event(Core::WindowEventType::KEY_RELEASED, IO::Keys::A, 2.0));
    EXPECT_FALSE(source.is_key_pressed(IO::Keys::A));
}

TEST(WindowEventSourceTest, MouseButtonStateTracksPressAndRelease)
{
    Vruta::WindowEventSource source;

    EXPECT_FALSE(source.is_mouse_pressed(0));

    source.signal(button_event(Core::WindowEventType::MOUSE_BUTTON_PRESSED, 0, 1.0));
    EXPECT_TRUE(source.is_mouse_pressed(0));

    source.signal(button_event(Core::WindowEventType::MOUSE_BUTTON_RELEASED, 0, 2.0));
    EXPECT_FALSE(source.is_mouse_pressed(0));
}

TEST(WindowEventSourceTest, MousePositionTracksTheLatestMotion)
{
    Vruta::WindowEventSource source;

    source.signal(position_event(3.5, -2.0));
    source.signal(position_event(7.0, 4.25));

    const auto [x, y] = source.get_mouse_position();
    EXPECT_EQ(x, 7.0);
    EXPECT_EQ(y, 4.25);
}

} // namespace MayaFlux::Test
