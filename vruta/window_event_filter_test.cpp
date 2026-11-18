#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/IO/Keys.hpp"
#include "MayaFlux/Kriya/Awaiters/EventAwaiter.hpp"
#include "MayaFlux/Vruta/Event.hpp"
#include "MayaFlux/Vruta/WindowEventSource.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

namespace {

    using Core::WindowEvent;
    using Core::WindowEventType;
    using IO::Keys;
    using IO::Modifiers;
    using Vruta::WindowEventFilter;

    WindowEvent key_event(WindowEventType type, Keys key, Modifiers mods)
    {
        WindowEvent event;
        event.type = type;
        event.data = WindowEvent::KeyData { static_cast<int16_t>(key), 0, mods };
        return event;
    }

    WindowEvent button_event(IO::MouseButtons button, Modifiers mods)
    {
        WindowEvent event;
        event.type = WindowEventType::MOUSE_BUTTON_PRESSED;
        event.data = WindowEvent::MouseButtonData { static_cast<int8_t>(button), mods };
        return event;
    }

    Vruta::Event take_key_mods(Vruta::WindowEventSource& source, WindowEventFilter filter, std::vector<Modifiers>* seen)
    {
        auto event = co_await Kriya::WindowEventAwaiter { source, filter };
        if (const auto* data = std::get_if<WindowEvent::KeyData>(&event.data))
            seen->push_back(data->mods);
    }

} // namespace

TEST(WindowEventFilterTest, ModifiedKeyConstructorCopiesKeyAndModifiers)
{
    const WindowEventFilter filter { Keys::V | Modifiers::Ctrl };

    EXPECT_EQ(filter.key_code, Keys::V);
    EXPECT_EQ(filter.modifiers, Modifiers::Ctrl);
}

TEST(WindowEventFilterTest, BareKeyLeavesModifiersUnset)
{
    const WindowEventFilter from_key { Keys::V };
    const WindowEventFilter from_modified_key { IO::ModifiedKey { Keys::V } };

    EXPECT_FALSE(from_key.modifiers.has_value());
    EXPECT_FALSE(from_modified_key.modifiers.has_value());
}

TEST(WindowEventFilterTest, ModifiersMustEqualTheHeldModifierBits)
{
    WindowEventFilter filter { Keys::V | Modifiers::Ctrl };

    EXPECT_TRUE(filter.matches(key_event(WindowEventType::KEY_PRESSED, Keys::V, Modifiers::Ctrl)));
    EXPECT_FALSE(filter.matches(key_event(WindowEventType::KEY_PRESSED, Keys::V, Modifiers::None)));
    EXPECT_FALSE(filter.matches(key_event(WindowEventType::KEY_PRESSED, Keys::V, Modifiers::Shift)));
    EXPECT_FALSE(filter.matches(key_event(WindowEventType::KEY_PRESSED, Keys::V,
        Modifiers::Ctrl | Modifiers::Shift)));
    EXPECT_FALSE(filter.matches(key_event(WindowEventType::KEY_PRESSED, Keys::C, Modifiers::Ctrl)));
}

TEST(WindowEventFilterTest, LockBitsAreIgnored)
{
    const WindowEventFilter filter { Keys::V | Modifiers::Ctrl };
    const WindowEventFilter none { Keys::V | Modifiers::None };

    const Modifiers locks = Modifiers::CapsLock | Modifiers::NumLock;
    EXPECT_TRUE(filter.matches(key_event(WindowEventType::KEY_PRESSED, Keys::V, Modifiers::Ctrl | locks)));
    EXPECT_TRUE(none.matches(key_event(WindowEventType::KEY_PRESSED, Keys::V, locks)));
}

TEST(WindowEventFilterTest, UnsetModifiersAcceptAnyModifierState)
{
    const WindowEventFilter filter { Keys::V };

    EXPECT_TRUE(filter.matches(key_event(WindowEventType::KEY_PRESSED, Keys::V, Modifiers::None)));
    EXPECT_TRUE(filter.matches(key_event(WindowEventType::KEY_PRESSED, Keys::V,
        Modifiers::Ctrl | Modifiers::Alt)));
}

TEST(WindowEventFilterTest, ModifiersApplyToMouseButtonEvents)
{
    WindowEventFilter filter;
    filter.button = IO::MouseButtons::Left;
    filter.modifiers = Modifiers::Shift;

    EXPECT_TRUE(filter.matches(button_event(IO::MouseButtons::Left, Modifiers::Shift)));
    EXPECT_FALSE(filter.matches(button_event(IO::MouseButtons::Left, Modifiers::None)));
    EXPECT_FALSE(filter.matches(button_event(IO::MouseButtons::Right, Modifiers::Shift)));
}

TEST(WindowEventFilterTest, ModifiersNeverMatchAnEventThatCarriesNone)
{
    WindowEventFilter filter;
    filter.modifiers = Modifiers::None;

    WindowEvent text;
    text.type = WindowEventType::TEXT_INPUT;
    text.data = WindowEvent::TextData { 0x41 };
    WindowEvent motion;
    motion.type = WindowEventType::MOUSE_MOTION;
    motion.data = WindowEvent::MousePosData { 1.0, 2.0 };

    EXPECT_FALSE(filter.matches(text));
    EXPECT_FALSE(filter.matches(motion));
    EXPECT_TRUE(WindowEventFilter {}.matches(text));
    EXPECT_TRUE(WindowEventFilter {}.matches(motion));
}

TEST(WindowEventFilterTest, EventTypeAndKeyAndModifiersAreAnded)
{
    WindowEventFilter filter { Keys::V | Modifiers::Ctrl };
    filter.event_type = WindowEventType::KEY_PRESSED;

    EXPECT_TRUE(filter.matches(key_event(WindowEventType::KEY_PRESSED, Keys::V, Modifiers::Ctrl)));
    EXPECT_FALSE(filter.matches(key_event(WindowEventType::KEY_RELEASED, Keys::V, Modifiers::Ctrl)));
}

TEST(WindowEventSourceModifiersTest, StartsWithNoModifiers)
{
    Vruta::WindowEventSource source;

    EXPECT_EQ(source.modifiers(), Modifiers::None);
}

TEST(WindowEventSourceModifiersTest, FollowsTheModifiersOfKeyEvents)
{
    Vruta::WindowEventSource source;

    source.signal(key_event(WindowEventType::KEY_PRESSED, Keys::LShift, Modifiers::Shift));
    EXPECT_EQ(source.modifiers(), Modifiers::Shift);

    source.signal(key_event(WindowEventType::KEY_PRESSED, Keys::LCtrl, Modifiers::Shift | Modifiers::Ctrl));
    EXPECT_EQ(source.modifiers(), Modifiers::Shift | Modifiers::Ctrl);

    source.signal(key_event(WindowEventType::KEY_RELEASED, Keys::LShift, Modifiers::Ctrl));
    EXPECT_EQ(source.modifiers(), Modifiers::Ctrl);

    source.signal(key_event(WindowEventType::KEY_RELEASED, Keys::LCtrl, Modifiers::None));
    EXPECT_EQ(source.modifiers(), Modifiers::None);
}

TEST(WindowEventSourceModifiersTest, FollowsTheModifiersOfMouseButtonEvents)
{
    Vruta::WindowEventSource source;

    source.signal(button_event(IO::MouseButtons::Middle, Modifiers::Alt));

    EXPECT_EQ(source.modifiers(), Modifiers::Alt);
}

TEST(WindowEventSourceModifiersTest, EventsWithoutModifiersLeaveTheStateAlone)
{
    Vruta::WindowEventSource source;
    source.signal(key_event(WindowEventType::KEY_PRESSED, Keys::LShift, Modifiers::Shift));

    WindowEvent motion;
    motion.type = WindowEventType::MOUSE_MOTION;
    motion.data = WindowEvent::MousePosData { 3.0, 4.0 };
    source.signal(motion);

    EXPECT_EQ(source.modifiers(), Modifiers::Shift);
}

TEST(WindowEventFilterTest, QueuedEventsAreFilteredByModifiers)
{
    Vruta::WindowEventSource source;
    std::vector<Modifiers> seen;

    source.signal(key_event(WindowEventType::KEY_PRESSED, Keys::V, Modifiers::None));
    source.signal(key_event(WindowEventType::KEY_PRESSED, Keys::V, Modifiers::Ctrl));
    ASSERT_EQ(source.pending_count(), 2U);

    auto handler = take_key_mods(source, WindowEventFilter { Keys::V | Modifiers::Ctrl }, &seen);

    EXPECT_EQ(seen, std::vector<Modifiers> { Modifiers::Ctrl });
    EXPECT_EQ(source.pending_count(), 1U);
}

TEST(WindowEventFilterTest, SuspendedHandlerSkipsEventsWithOtherModifiers)
{
    Vruta::WindowEventSource source;
    std::vector<Modifiers> seen;
    auto handler = take_key_mods(source, WindowEventFilter { Keys::V | Modifiers::Ctrl }, &seen);

    source.signal(key_event(WindowEventType::KEY_PRESSED, Keys::V, Modifiers::Shift));
    EXPECT_TRUE(seen.empty());
    EXPECT_TRUE(handler.is_active());

    source.signal(key_event(WindowEventType::KEY_PRESSED, Keys::V, Modifiers::Ctrl));
    EXPECT_EQ(seen, std::vector<Modifiers> { Modifiers::Ctrl });
    EXPECT_FALSE(handler.is_active());
}

}
