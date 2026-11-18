#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#if defined(MAYAFLUX_PLATFORM_WINDOWS)

#include <chrono>

#include "MayaFlux/Core/Backends/Windowing/Win32/KeyMapping.hpp"
#include "MayaFlux/Core/Backends/Windowing/Win32/Win32Window.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::display>() }))

namespace MayaFlux::Test {
namespace {

    using namespace std::chrono_literals;
    using Core::WindowEvent;
    using Core::WindowEventType;

    constexpr uint32_t k_scancode_a = 0x1E;

    template <typename Predicate>
    bool eventually(Predicate predicate, std::chrono::milliseconds timeout = 2s)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (predicate())
                return true;
            std::this_thread::sleep_for(2ms);
        }
        return predicate();
    }

    Core::WindowCreateInfo small_window(const std::string& title = "MayaFlux Win32 Test")
    {
        Core::WindowCreateInfo info;
        info.title = title;
        info.width = 320;
        info.height = 240;
        return info;
    }

    class WindowHarness {
    public:
        explicit WindowHarness(
            const Core::WindowCreateInfo& info = small_window(),
            const Core::GlobalGraphicsConfig& config = {})
            : m_window(std::make_unique<Core::Win32Window>(info, config))
        {
            m_window->set_event_callback([this](const WindowEvent& event) {
                m_events.push_back(event);
            });
        }

        WindowHarness(const WindowHarness&) = delete;
        WindowHarness& operator=(const WindowHarness&) = delete;
        WindowHarness(WindowHarness&&) = delete;
        WindowHarness& operator=(WindowHarness&&) = delete;

        Core::Win32Window& window() { return *m_window; }
        [[nodiscard]] HWND hwnd() const { return static_cast<HWND>(m_window->get_native_handle()); }

        LRESULT send(UINT message, WPARAM wparam = 0, LPARAM lparam = 0) const
        {
            return SendMessageW(hwnd(), message, wparam, lparam);
        }

        void poll() { m_window->poll(); }

        [[nodiscard]] std::vector<WindowEvent> events_of(WindowEventType type) const
        {
            std::vector<WindowEvent> matching;
            std::ranges::copy_if(m_events, std::back_inserter(matching), [type](const WindowEvent& event) {
                return event.type == type;
            });
            return matching;
        }

        [[nodiscard]] const std::vector<WindowEvent>& events() const { return m_events; }
        void clear_events() { m_events.clear(); }

    private:
        std::vector<WindowEvent> m_events;
        std::unique_ptr<Core::Win32Window> m_window;
    };

    template <typename Data>
    const Data& data_of(const WindowEvent& event)
    {
        const auto* data = std::get_if<Data>(&event.data);
        EXPECT_NE(data, nullptr);
        static const Data fallback {};
        return data ? *data : fallback;
    }

    LPARAM key_lparam(uint32_t scancode, bool repeat = false)
    {
        return static_cast<LPARAM>((scancode << 16) | 1U | (repeat ? (1U << 30) : 0U));
    }

    std::wstring window_text(HWND hwnd)
    {
        wchar_t buffer[256] {};
        GetWindowTextW(hwnd, buffer, 256);
        return buffer;
    }

    // ------------------------------------------------------------------
    // Lifecycle
    // ------------------------------------------------------------------

    TEST(Win32WindowTest, ConstructorCreatesNativeWindowWithRequestedTitle)
    {
        WindowHarness harness(small_window("MayaFlux Title Test"));

        ASSERT_NE(harness.hwnd(), nullptr);
        EXPECT_TRUE(IsWindow(harness.hwnd()));
        EXPECT_EQ(harness.window().get_native_display(), nullptr);
        EXPECT_EQ(window_text(harness.hwnd()), L"MayaFlux Title Test");
        EXPECT_EQ(harness.window().get_create_info().title, "MayaFlux Title Test");
        EXPECT_FALSE(harness.window().should_close());
    }

    TEST(Win32WindowTest, DestroyClosesNativeWindowAndIsIdempotent)
    {
        WindowHarness harness;
        const auto hwnd = harness.hwnd();
        ASSERT_TRUE(IsWindow(hwnd));

        harness.window().destroy();
        EXPECT_FALSE(IsWindow(hwnd));
        EXPECT_EQ(harness.window().get_native_handle(), nullptr);
        EXPECT_TRUE(harness.window().should_close());

        EXPECT_NO_THROW(harness.window().destroy());
        EXPECT_NO_THROW(harness.window().show());
        EXPECT_NO_THROW(harness.window().hide());
        EXPECT_NO_THROW(harness.window().set_title("after destroy"));
    }

    TEST(Win32WindowTest, PollAfterDestroyDeliversClosedEventOnce)
    {
        WindowHarness harness;
        harness.window().destroy();

        harness.poll();
        EXPECT_EQ(harness.events_of(WindowEventType::WINDOW_CLOSED).size(), 1U);
        harness.poll();
        EXPECT_EQ(harness.events_of(WindowEventType::WINDOW_CLOSED).size(), 1U);
    }

    TEST(Win32WindowTest, DestructionWithoutDestroyReleasesNativeWindow)
    {
        HWND hwnd = nullptr;
        {
            WindowHarness harness;
            hwnd = harness.hwnd();
            ASSERT_TRUE(IsWindow(hwnd));
        }
        EXPECT_FALSE(IsWindow(hwnd));
    }

    TEST(Win32WindowTest, RepeatedCreationAndDestructionInOneProcess)
    {
        for (int round = 0; round < 5; ++round) {
            HWND hwnd = nullptr;
            {
                WindowHarness harness;
                hwnd = harness.hwnd();
                ASSERT_NE(hwnd, nullptr);
                EXPECT_TRUE(IsWindow(hwnd));
            }
            EXPECT_FALSE(IsWindow(hwnd));
        }
    }

    TEST(Win32WindowTest, ConcurrentWindowsAreIndependent)
    {
        WindowHarness first(small_window("MayaFlux First"));
        WindowHarness second(small_window("MayaFlux Second"));
        ASSERT_NE(first.hwnd(), nullptr);
        ASSERT_NE(second.hwnd(), nullptr);
        EXPECT_NE(first.hwnd(), second.hwnd());

        first.send(WM_MOUSEMOVE, 0, MAKELPARAM(10, 20));
        first.poll();
        second.poll();
        EXPECT_EQ(first.events_of(WindowEventType::MOUSE_MOTION).size(), 1U);
        EXPECT_TRUE(second.events_of(WindowEventType::MOUSE_MOTION).empty());

        first.window().destroy();
        EXPECT_TRUE(IsWindow(second.hwnd()));
        EXPECT_FALSE(second.window().should_close());
    }

    // ------------------------------------------------------------------
    // Window state
    // ------------------------------------------------------------------

    TEST(Win32WindowTest, ShowAndHideToggleNativeVisibility)
    {
        WindowHarness harness;
        EXPECT_FALSE(IsWindowVisible(harness.hwnd()));

        harness.window().show();
        EXPECT_TRUE(IsWindowVisible(harness.hwnd()));

        harness.window().hide();
        EXPECT_FALSE(IsWindowVisible(harness.hwnd()));
    }

    TEST(Win32WindowTest, SetTitleUpdatesNativeTextAndCreateInfo)
    {
        WindowHarness harness;
        harness.window().set_title("Renamed Window");

        EXPECT_EQ(window_text(harness.hwnd()), L"Renamed Window");
        EXPECT_EQ(harness.window().get_create_info().title, "Renamed Window");
    }

    TEST(Win32WindowTest, SetSizeUpdatesStateAndEmitsResizeMatchingClientArea)
    {
        WindowHarness harness;
        harness.window().set_size(400, 300);
        harness.poll();

        RECT client {};
        ASSERT_TRUE(GetClientRect(harness.hwnd(), &client));
        const auto width = static_cast<uint32_t>(client.right - client.left);
        const auto height = static_cast<uint32_t>(client.bottom - client.top);
        EXPECT_GT(width, 0U);
        EXPECT_GT(height, 0U);
        EXPECT_LE(width, 400U);
        EXPECT_LE(height, 300U);

        const auto& state = harness.window().get_state();
        EXPECT_EQ(state.current_width, width);
        EXPECT_EQ(state.current_height, height);
        EXPECT_EQ(harness.window().get_create_info().width, 400U);
        EXPECT_EQ(harness.window().get_create_info().height, 300U);

        const auto resized = harness.events_of(WindowEventType::WINDOW_RESIZED);
        ASSERT_FALSE(resized.empty());
        const auto& last = data_of<WindowEvent::ResizeData>(resized.back());
        EXPECT_EQ(last.width, width);
        EXPECT_EQ(last.height, height);
    }

    TEST(Win32WindowTest, SetPositionMovesNativeWindow)
    {
        WindowHarness harness;
        harness.window().set_position(100, 120);

        RECT bounds {};
        ASSERT_TRUE(GetWindowRect(harness.hwnd(), &bounds));
        EXPECT_EQ(bounds.left, 100);
        EXPECT_EQ(bounds.top, 120);
    }

    TEST(Win32WindowTest, FocusMessagesUpdateStateAndEmitEvents)
    {
        WindowHarness harness;
        EXPECT_FALSE(harness.window().get_state().is_focused);

        harness.send(WM_SETFOCUS);
        EXPECT_TRUE(harness.window().get_state().is_focused);
        harness.send(WM_KILLFOCUS);
        EXPECT_FALSE(harness.window().get_state().is_focused);

        harness.poll();
        EXPECT_EQ(harness.events_of(WindowEventType::WINDOW_FOCUS_GAINED).size(), 1U);
        EXPECT_EQ(harness.events_of(WindowEventType::WINDOW_FOCUS_LOST).size(), 1U);
    }

    TEST(Win32WindowTest, RequestCloseFlagsWindowAndLeavesDestructionToOwner)
    {
        WindowHarness harness;
        const auto hwnd = harness.hwnd();
        EXPECT_FALSE(harness.window().should_close());

        harness.window().request_close();
        harness.window().request_close();
        EXPECT_TRUE(harness.window().should_close());
        EXPECT_TRUE(IsWindow(hwnd));

        harness.poll();
        EXPECT_EQ(harness.events_of(WindowEventType::WINDOW_CLOSED).size(), 1U);
        harness.poll();
        EXPECT_EQ(harness.events_of(WindowEventType::WINDOW_CLOSED).size(), 1U);
        EXPECT_NO_THROW(harness.window().destroy());
    }

    TEST(Win32WindowTest, RequestCloseFromAnotherThreadIsDeliveredByPoll)
    {
        WindowHarness harness;

        std::thread other([&harness] { harness.window().request_close(); });
        other.join();
        EXPECT_TRUE(harness.window().should_close());

        harness.poll();
        EXPECT_EQ(harness.events_of(WindowEventType::WINDOW_CLOSED).size(), 1U);
        EXPECT_NO_THROW(harness.window().destroy());
    }

    TEST(Win32WindowTest, CloseMessageRequestsCloseAndDestroysNativeWindow)
    {
        WindowHarness harness;
        const auto hwnd = harness.hwnd();
        EXPECT_FALSE(harness.window().should_close());

        harness.send(WM_CLOSE);
        EXPECT_TRUE(harness.window().should_close());
        EXPECT_FALSE(IsWindow(hwnd));

        harness.poll();
        EXPECT_EQ(harness.events_of(WindowEventType::WINDOW_CLOSED).size(), 1U);
        EXPECT_NO_THROW(harness.window().destroy());
    }

    TEST(Win32WindowTest, InteractiveResizeSuppressesSourceSignalsUntilExit)
    {
        WindowHarness harness;
        auto& source = harness.window().get_event_source();
        source.clear();

        harness.send(WM_ENTERSIZEMOVE);
        harness.window().set_size(380, 280);
        harness.poll();
        EXPECT_FALSE(harness.events_of(WindowEventType::WINDOW_RESIZED).empty());
        EXPECT_EQ(source.pending_count(), 0U);

        harness.send(WM_EXITSIZEMOVE);
        harness.poll();
        EXPECT_EQ(source.pending_count(), 1U);
    }

    TEST(Win32WindowTest, StoresRenderTrackingAndFlagState)
    {
        WindowHarness harness;
        auto& window = harness.window();

        EXPECT_FALSE(window.is_graphics_registered());
        window.set_graphics_registered(true);
        EXPECT_TRUE(window.is_graphics_registered());

        EXPECT_FALSE(window.is_capture_enabled());
        window.set_capture_enabled(true);
        EXPECT_TRUE(window.is_capture_enabled());

        EXPECT_TRUE(window.get_frame_commands().empty());
        window.track_frame_command(7);
        window.track_frame_command(9);
        EXPECT_EQ(window.get_frame_commands(), (std::vector<uint64_t> { 7, 9 }));
        window.clear_frame_commands();
        EXPECT_TRUE(window.get_frame_commands().empty());

        EXPECT_TRUE(window.get_rendering_buffers().empty());

        Core::InputConfig input;
        input.keyboard_enabled = false;
        input.cursor_mode = Core::CursorMode::HIDDEN;
        window.set_input_config(input);
        EXPECT_FALSE(window.get_input_config().keyboard_enabled);
        EXPECT_EQ(window.get_input_config().cursor_mode, Core::CursorMode::HIDDEN);

        window.set_color({ 0.25F, 0.5F, 0.75F, 1.0F });
        EXPECT_EQ(window.get_create_info().clear_color, (std::array<float, 4> { 0.25F, 0.5F, 0.75F, 1.0F }));
    }

    // ------------------------------------------------------------------
    // Keyboard
    // ------------------------------------------------------------------

    TEST(Win32WindowTest, KeyDownAndUpEmitEventsAndTrackSourceState)
    {
        WindowHarness harness;
        auto& source = harness.window().get_event_source();

        harness.send(WM_KEYDOWN, 'A', key_lparam(k_scancode_a));
        harness.poll();
        auto pressed = harness.events_of(WindowEventType::KEY_PRESSED);
        ASSERT_EQ(pressed.size(), 1U);
        const auto& down = data_of<WindowEvent::KeyData>(pressed.front());
        EXPECT_EQ(down.key, static_cast<int16_t>(IO::Keys::A));
        EXPECT_EQ(down.scancode, static_cast<int32_t>(k_scancode_a));
        EXPECT_TRUE(source.is_key_pressed(IO::Keys::A));

        harness.send(WM_KEYUP, 'A', key_lparam(k_scancode_a));
        harness.poll();
        auto released = harness.events_of(WindowEventType::KEY_RELEASED);
        ASSERT_EQ(released.size(), 1U);
        EXPECT_EQ(data_of<WindowEvent::KeyData>(released.front()).key, static_cast<int16_t>(IO::Keys::A));
        EXPECT_FALSE(source.is_key_pressed(IO::Keys::A));
    }

    TEST(Win32WindowTest, OsAutoRepeatKeyDownIsIgnored)
    {
        WindowHarness harness;

        harness.send(WM_KEYDOWN, 'A', key_lparam(k_scancode_a));
        harness.send(WM_KEYDOWN, 'A', key_lparam(k_scancode_a, true));
        harness.send(WM_KEYDOWN, 'A', key_lparam(k_scancode_a, true));
        harness.poll();

        EXPECT_EQ(harness.events_of(WindowEventType::KEY_PRESSED).size(), 1U);
    }

    TEST(Win32WindowTest, HeldKeyRepeatsAfterInitialDelayAndStopsOnRelease)
    {
        Core::GlobalGraphicsConfig config;
        config.key_repeat_config.initial_delay_ms = 300;
        config.key_repeat_config.interval_ms = 5;
        WindowHarness harness(small_window(), config);

        harness.send(WM_KEYDOWN, 'A', key_lparam(k_scancode_a));
        harness.poll();
        EXPECT_TRUE(harness.events_of(WindowEventType::KEY_REPEAT).empty());

        ASSERT_TRUE(eventually([&] {
            harness.poll();
            return harness.events_of(WindowEventType::KEY_REPEAT).size() >= 2;
        }, 3s));
        const auto repeat = harness.events_of(WindowEventType::KEY_REPEAT).front();
        EXPECT_EQ(data_of<WindowEvent::KeyData>(repeat).key, static_cast<int16_t>(IO::Keys::A));

        harness.send(WM_KEYUP, 'A', key_lparam(k_scancode_a));
        harness.poll();
        const auto after_release = harness.events_of(WindowEventType::KEY_REPEAT).size();
        std::this_thread::sleep_for(50ms);
        harness.poll();
        EXPECT_EQ(harness.events_of(WindowEventType::KEY_REPEAT).size(), after_release);
    }

    // ------------------------------------------------------------------
    // Modifiers
    // ------------------------------------------------------------------

    constexpr IO::Modifiers k_held_modifier_bits
        = IO::Modifiers::Shift | IO::Modifiers::Ctrl | IO::Modifiers::Alt | IO::Modifiers::Super;

    IO::Modifiers held_modifiers(const WindowEvent& event)
    {
        return data_of<WindowEvent::KeyData>(event).mods & k_held_modifier_bits;
    }

    IO::Modifiers held_modifiers(IO::Modifiers mods)
    {
        return mods & k_held_modifier_bits;
    }

    struct ModifierKeyCase {
        const char* name;
        WPARAM vk;
        IO::Keys key;
        IO::Modifiers flag;
    };

    class Win32ModifierKeyTest : public ::testing::TestWithParam<ModifierKeyCase> { };

    MAYAFLUX_TEST_WITH(TEST_P, Win32ModifierKeyTest, ModifierKeyPressCarriesItsOwnBitAndReleaseClearsIt, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        const auto& param = GetParam();
        WindowHarness harness;

        harness.send(WM_KEYDOWN, param.vk, key_lparam(0));
        harness.send(WM_KEYUP, param.vk, key_lparam(0));
        harness.poll();

        const auto pressed = harness.events_of(WindowEventType::KEY_PRESSED);
        const auto released = harness.events_of(WindowEventType::KEY_RELEASED);
        ASSERT_EQ(pressed.size(), 1U);
        ASSERT_EQ(released.size(), 1U);
        EXPECT_EQ(data_of<WindowEvent::KeyData>(pressed.front()).key, static_cast<int16_t>(param.key));
        EXPECT_EQ(held_modifiers(pressed.front()), param.flag);
        EXPECT_EQ(held_modifiers(released.front()), IO::Modifiers::None);
    }

    INSTANTIATE_TEST_SUITE_P(
        Win32Window,
        Win32ModifierKeyTest,
        ::testing::Values(
            ModifierKeyCase { "LeftShift", VK_LSHIFT, IO::Keys::LShift, IO::Modifiers::Shift },
            ModifierKeyCase { "RightShift", VK_RSHIFT, IO::Keys::RShift, IO::Modifiers::Shift },
            ModifierKeyCase { "LeftControl", VK_LCONTROL, IO::Keys::LCtrl, IO::Modifiers::Ctrl },
            ModifierKeyCase { "RightControl", VK_RCONTROL, IO::Keys::RCtrl, IO::Modifiers::Ctrl },
            ModifierKeyCase { "LeftAlt", VK_LMENU, IO::Keys::LAlt, IO::Modifiers::Alt },
            ModifierKeyCase { "RightAlt", VK_RMENU, IO::Keys::RAlt, IO::Modifiers::Alt },
            ModifierKeyCase { "LeftWin", VK_LWIN, IO::Keys::LSuper, IO::Modifiers::Super },
            ModifierKeyCase { "RightWin", VK_RWIN, IO::Keys::RSuper, IO::Modifiers::Super }),
        [](const ::testing::TestParamInfo<ModifierKeyCase>& info) { return std::string(info.param.name); });

    TEST(Win32WindowTest, ModifierStaysSetUntilBothSidesAreReleased)
    {
        WindowHarness harness;

        harness.send(WM_KEYDOWN, VK_LSHIFT, key_lparam(0));
        harness.send(WM_KEYDOWN, VK_RSHIFT, key_lparam(0));
        harness.send(WM_KEYUP, VK_LSHIFT, key_lparam(0));
        harness.send(WM_KEYDOWN, 'A', key_lparam(k_scancode_a));
        harness.send(WM_KEYUP, VK_RSHIFT, key_lparam(0));
        harness.send(WM_KEYUP, 'A', key_lparam(k_scancode_a));
        harness.poll();

        const auto pressed = harness.events_of(WindowEventType::KEY_PRESSED);
        const auto released = harness.events_of(WindowEventType::KEY_RELEASED);
        ASSERT_EQ(pressed.size(), 3U);
        ASSERT_EQ(released.size(), 3U);
        EXPECT_EQ(held_modifiers(pressed[2]), IO::Modifiers::Shift);
        EXPECT_EQ(held_modifiers(released[0]), IO::Modifiers::Shift);
        EXPECT_EQ(held_modifiers(released[1]), IO::Modifiers::None);
        EXPECT_EQ(held_modifiers(released[2]), IO::Modifiers::None);
    }

    TEST(Win32WindowTest, KeyEventsCarryHeldModifiers)
    {
        WindowHarness harness;

        harness.send(WM_KEYDOWN, VK_LCONTROL, key_lparam(0));
        harness.send(WM_KEYDOWN, VK_LSHIFT, key_lparam(0));
        harness.send(WM_KEYDOWN, 'A', key_lparam(k_scancode_a));
        harness.send(WM_KEYUP, 'A', key_lparam(k_scancode_a));
        harness.poll();

        const auto expected = IO::Modifiers::Ctrl | IO::Modifiers::Shift;
        const auto pressed = harness.events_of(WindowEventType::KEY_PRESSED);
        ASSERT_EQ(pressed.size(), 3U);
        EXPECT_EQ(held_modifiers(pressed[2]), expected);
        EXPECT_TRUE(has_flag(
            data_of<WindowEvent::KeyData>(pressed[2]).mods,
            IO::Modifiers::Ctrl | IO::Modifiers::Shift));
        EXPECT_FALSE(has_flag(
            data_of<WindowEvent::KeyData>(pressed[2]).mods, IO::Modifiers::Alt));

        const auto released = harness.events_of(WindowEventType::KEY_RELEASED);
        ASSERT_EQ(released.size(), 1U);
        EXPECT_EQ(held_modifiers(released.front()), expected);
    }

    TEST(Win32WindowTest, MouseButtonsCarryHeldModifiers)
    {
        WindowHarness harness;

        harness.send(WM_KEYDOWN, VK_LMENU, key_lparam(0));
        harness.send(WM_LBUTTONDOWN, 0, MAKELPARAM(5, 5));
        harness.send(WM_LBUTTONUP, 0, MAKELPARAM(5, 5));
        harness.send(WM_KEYUP, VK_LMENU, key_lparam(0));
        harness.send(WM_LBUTTONDOWN, 0, MAKELPARAM(5, 5));
        harness.poll();

        const auto pressed = harness.events_of(WindowEventType::MOUSE_BUTTON_PRESSED);
        const auto released = harness.events_of(WindowEventType::MOUSE_BUTTON_RELEASED);
        ASSERT_EQ(pressed.size(), 2U);
        ASSERT_EQ(released.size(), 1U);
        EXPECT_EQ(held_modifiers(data_of<WindowEvent::MouseButtonData>(pressed[0]).mods), IO::Modifiers::Alt);
        EXPECT_EQ(held_modifiers(data_of<WindowEvent::MouseButtonData>(released[0]).mods), IO::Modifiers::Alt);
        EXPECT_EQ(held_modifiers(data_of<WindowEvent::MouseButtonData>(pressed[1]).mods), IO::Modifiers::None);
    }

    TEST(Win32WindowTest, FocusLossReleasesHeldKeysAndModifiers)
    {
        WindowHarness harness;
        auto& source = harness.window().get_event_source();

        harness.send(WM_SETFOCUS);
        harness.send(WM_KEYDOWN, VK_LSHIFT, key_lparam(0));
        harness.send(WM_KEYDOWN, 'A', key_lparam(k_scancode_a));
        harness.poll();
        ASSERT_TRUE(source.is_key_pressed(IO::Keys::A));
        ASSERT_TRUE(source.is_key_pressed(IO::Keys::LShift));

        harness.send(WM_KILLFOCUS);
        harness.poll();

        const auto released = harness.events_of(WindowEventType::KEY_RELEASED);
        ASSERT_EQ(released.size(), 2U);
        for (const auto& event : released)
            EXPECT_EQ(held_modifiers(event), IO::Modifiers::None);
        EXPECT_FALSE(source.is_key_pressed(IO::Keys::A));
        EXPECT_FALSE(source.is_key_pressed(IO::Keys::LShift));

        harness.send(WM_SETFOCUS);
        harness.send(WM_KEYDOWN, 'A', key_lparam(k_scancode_a));
        harness.poll();
        const auto pressed = harness.events_of(WindowEventType::KEY_PRESSED);
        ASSERT_EQ(pressed.size(), 3U);
        EXPECT_EQ(held_modifiers(pressed.back()), IO::Modifiers::None);
    }

    TEST(Win32WindowTest, HeldKeyRepeatCarriesCurrentModifiers)
    {
        Core::GlobalGraphicsConfig config;
        config.key_repeat_config.initial_delay_ms = 300;
        config.key_repeat_config.interval_ms = 5;
        WindowHarness harness(small_window(), config);

        harness.send(WM_KEYDOWN, 'A', key_lparam(k_scancode_a));
        harness.send(WM_KEYDOWN, VK_LCONTROL, key_lparam(0));

        const auto repeats_of_a = [&] {
            std::vector<WindowEvent> matching;
            for (const auto& event : harness.events_of(WindowEventType::KEY_REPEAT)) {
                if (data_of<WindowEvent::KeyData>(event).key == static_cast<int16_t>(IO::Keys::A))
                    matching.push_back(event);
            }
            return matching;
        };

        ASSERT_TRUE(eventually([&] {
            harness.poll();
            return !repeats_of_a().empty();
        }, 3s));
        for (const auto& event : repeats_of_a())
            EXPECT_EQ(held_modifiers(event), IO::Modifiers::Ctrl);
    }

    // ------------------------------------------------------------------
    // Text input
    // ------------------------------------------------------------------

    TEST(Win32WindowTest, CharMessageDeliversBasicMultilingualPlaneCodepoint)
    {
        WindowHarness harness;

        harness.send(WM_CHAR, L'a');
        harness.send(WM_CHAR, 0x00E9);
        harness.poll();

        const auto text = harness.events_of(WindowEventType::TEXT_INPUT);
        ASSERT_EQ(text.size(), 2U);
        EXPECT_EQ(data_of<WindowEvent::TextData>(text[0]).codepoint, 0x61U);
        EXPECT_EQ(data_of<WindowEvent::TextData>(text[1]).codepoint, 0xE9U);
    }

    TEST(Win32WindowTest, SurrogatePairIsCombinedIntoSingleCodepoint)
    {
        WindowHarness harness;

        harness.send(WM_CHAR, 0xD83D);
        harness.poll();
        EXPECT_TRUE(harness.events_of(WindowEventType::TEXT_INPUT).empty());

        harness.send(WM_CHAR, 0xDE00);
        harness.poll();
        const auto text = harness.events_of(WindowEventType::TEXT_INPUT);
        ASSERT_EQ(text.size(), 1U);
        EXPECT_EQ(data_of<WindowEvent::TextData>(text.front()).codepoint, 0x1F600U);
    }

    struct RejectedTextCase {
        const char* name;
        uint16_t unit;
    };

    class Win32RejectedTextTest : public ::testing::TestWithParam<RejectedTextCase> { };

    MAYAFLUX_TEST_WITH(TEST_P, Win32RejectedTextTest, CharMessageProducesNoTextEvent, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        WindowHarness harness;

        harness.send(WM_CHAR, GetParam().unit);
        harness.poll();

        EXPECT_TRUE(harness.events_of(WindowEventType::TEXT_INPUT).empty());
    }

    INSTANTIATE_TEST_SUITE_P(
        Win32Window,
        Win32RejectedTextTest,
        ::testing::Values(
            RejectedTextCase { "Backspace", 0x08 },
            RejectedTextCase { "Delete", 0x7F },
            RejectedTextCase { "OrphanLowSurrogate", 0xDC00 }),
        [](const ::testing::TestParamInfo<RejectedTextCase>& info) { return std::string(info.param.name); });

    // ------------------------------------------------------------------
    // Mouse
    // ------------------------------------------------------------------

    TEST(Win32WindowTest, MouseMoveEmitsPositionAndUpdatesSource)
    {
        WindowHarness harness;

        harness.send(WM_MOUSEMOVE, 0, MAKELPARAM(42, 77));
        harness.poll();

        const auto motion = harness.events_of(WindowEventType::MOUSE_MOTION);
        ASSERT_EQ(motion.size(), 1U);
        const auto& position = data_of<WindowEvent::MousePosData>(motion.front());
        EXPECT_EQ(position.x, 42.0);
        EXPECT_EQ(position.y, 77.0);
        const auto [x, y] = harness.window().get_event_source().get_mouse_position();
        EXPECT_EQ(x, 42.0);
        EXPECT_EQ(y, 77.0);
    }

    struct MouseButtonCase {
        const char* name;
        UINT down_message;
        UINT up_message;
        IO::MouseButtons button;
    };

    class Win32MouseButtonTest : public ::testing::TestWithParam<MouseButtonCase> { };

    MAYAFLUX_TEST_WITH(TEST_P, Win32MouseButtonTest, PressAndReleaseEmitButtonEventsAndTrackSourceState, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        const auto& param = GetParam();
        WindowHarness harness;
        auto& source = harness.window().get_event_source();

        harness.send(param.down_message, 0, MAKELPARAM(5, 5));
        harness.poll();
        const auto pressed = harness.events_of(WindowEventType::MOUSE_BUTTON_PRESSED);
        ASSERT_EQ(pressed.size(), 1U);
        EXPECT_EQ(data_of<WindowEvent::MouseButtonData>(pressed.front()).button, static_cast<int8_t>(param.button));
        EXPECT_TRUE(source.is_mouse_pressed(static_cast<int>(param.button)));

        harness.send(param.up_message, 0, MAKELPARAM(5, 5));
        harness.poll();
        const auto released = harness.events_of(WindowEventType::MOUSE_BUTTON_RELEASED);
        ASSERT_EQ(released.size(), 1U);
        EXPECT_EQ(data_of<WindowEvent::MouseButtonData>(released.front()).button, static_cast<int8_t>(param.button));
        EXPECT_FALSE(source.is_mouse_pressed(static_cast<int>(param.button)));
    }

    INSTANTIATE_TEST_SUITE_P(
        Win32Window,
        Win32MouseButtonTest,
        ::testing::Values(
            MouseButtonCase { "Left", WM_LBUTTONDOWN, WM_LBUTTONUP, IO::MouseButtons::Left },
            MouseButtonCase { "Right", WM_RBUTTONDOWN, WM_RBUTTONUP, IO::MouseButtons::Right },
            MouseButtonCase { "Middle", WM_MBUTTONDOWN, WM_MBUTTONUP, IO::MouseButtons::Middle }),
        [](const ::testing::TestParamInfo<MouseButtonCase>& info) { return std::string(info.param.name); });

    struct ScrollCase {
        const char* name;
        int16_t delta;
        double expected;
    };

    class Win32ScrollTest : public ::testing::TestWithParam<ScrollCase> { };

    MAYAFLUX_TEST_WITH(TEST_P, Win32ScrollTest, WheelDeltaIsReportedInNotches, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        WindowHarness harness;

        harness.send(WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(GetParam().delta)), MAKELPARAM(0, 0));
        harness.poll();

        const auto scrolled = harness.events_of(WindowEventType::MOUSE_SCROLLED);
        ASSERT_EQ(scrolled.size(), 1U);
        const auto& scroll = data_of<WindowEvent::ScrollData>(scrolled.front());
        EXPECT_EQ(scroll.x_offset, 0.0);
        EXPECT_EQ(scroll.y_offset, GetParam().expected);
    }

    INSTANTIATE_TEST_SUITE_P(
        Win32Window,
        Win32ScrollTest,
        ::testing::Values(
            ScrollCase { "OneNotchUp", 120, 1.0 },
            ScrollCase { "OneNotchDown", -120, -1.0 },
            ScrollCase { "TwoNotchesUp", 240, 2.0 },
            ScrollCase { "HalfNotchDown", -60, -0.5 }),
        [](const ::testing::TestParamInfo<ScrollCase>& info) { return std::string(info.param.name); });

    // ------------------------------------------------------------------
    // Event queue
    // ------------------------------------------------------------------

    TEST(Win32WindowTest, EventQueueDropsOverflowWithoutBlockingAndRecovers)
    {
        WindowHarness harness;

        for (int i = 0; i < 400; ++i)
            harness.send(WM_MOUSEMOVE, 0, MAKELPARAM(i, 0));
        harness.poll();

        const auto delivered = harness.events_of(WindowEventType::MOUSE_MOTION).size();
        EXPECT_GT(delivered, 0U);
        EXPECT_LE(delivered, 256U);

        harness.clear_events();
        harness.send(WM_MOUSEMOVE, 0, MAKELPARAM(7, 0));
        harness.poll();
        EXPECT_EQ(harness.events_of(WindowEventType::MOUSE_MOTION).size(), 1U);
    }

    TEST(Win32WindowTest, PollPreservesEventOrder)
    {
        WindowHarness harness;

        harness.send(WM_KEYDOWN, 'A', key_lparam(k_scancode_a));
        harness.send(WM_MOUSEMOVE, 0, MAKELPARAM(1, 2));
        harness.send(WM_KEYUP, 'A', key_lparam(k_scancode_a));
        harness.poll();

        const auto& events = harness.events();
        ASSERT_EQ(events.size(), 3U);
        EXPECT_EQ(events[0].type, WindowEventType::KEY_PRESSED);
        EXPECT_EQ(events[1].type, WindowEventType::MOUSE_MOTION);
        EXPECT_EQ(events[2].type, WindowEventType::KEY_RELEASED);
    }

    // ------------------------------------------------------------------
    // Key mapping
    // ------------------------------------------------------------------

    MAYAFLUX_TEST_WITH(TEST, Win32KeyMappingTest, LettersAndDigitsMapToMatchingKeys, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        EXPECT_EQ(Core::from_win32_key('A'), IO::Keys::A);
        EXPECT_EQ(Core::to_win32_key(IO::Keys::A), static_cast<int>('A'));

        for (WPARAM vk = '0'; vk <= '9'; ++vk) {
            EXPECT_TRUE(Core::is_valid_win32_key(vk));
            EXPECT_EQ(Core::to_win32_key(Core::from_win32_key(vk)), static_cast<int>(vk));
        }
        for (WPARAM vk = 'A'; vk <= 'Z'; ++vk) {
            EXPECT_TRUE(Core::is_valid_win32_key(vk));
            EXPECT_EQ(Core::to_win32_key(Core::from_win32_key(vk)), static_cast<int>(vk));
        }
    }

    MAYAFLUX_TEST_WITH(TEST, Win32KeyMappingTest, NavigationKeysRoundTripThroughTable, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        EXPECT_EQ(Core::from_win32_key(VK_LEFT), IO::Keys::Left);
        EXPECT_EQ(Core::from_win32_key(VK_RIGHT), IO::Keys::Right);

        for (const WPARAM vk : { static_cast<WPARAM>(VK_RETURN), static_cast<WPARAM>(VK_SPACE),
                 static_cast<WPARAM>(VK_LEFT), static_cast<WPARAM>(VK_RIGHT) }) {
            const auto key = Core::from_win32_key(vk);
            EXPECT_NE(key, IO::Keys::Unknown);
            EXPECT_EQ(Core::to_win32_key(key), static_cast<int>(vk));
        }
    }

    MAYAFLUX_TEST_WITH(TEST, Win32KeyMappingTest, UnmappedKeysReportUnknownAndZero, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        EXPECT_EQ(Core::to_win32_key(IO::Keys::Unknown), 0);
        EXPECT_EQ(Core::from_win32_key(0), IO::Keys::Unknown);
        EXPECT_FALSE(Core::is_valid_win32_key(0));
        EXPECT_EQ(Core::from_win32_scancode(0), IO::Keys::Unknown);
        EXPECT_EQ(Core::from_win32_scancode(0x1E), IO::Keys::Unknown);
    }

    // ------------------------------------------------------------------
    // Cursor modes
    // ------------------------------------------------------------------

    class ClipRestore {
    public:
        ClipRestore() { GetClipCursor(&m_baseline); }
        ClipRestore(const ClipRestore&) = delete;
        ClipRestore& operator=(const ClipRestore&) = delete;
        ~ClipRestore() { ClipCursor(nullptr); }

        [[nodiscard]] bool is_baseline() const
        {
            RECT now {};
            GetClipCursor(&now);
            return EqualRect(&now, &m_baseline) != FALSE;
        }

    private:
        RECT m_baseline {};
    };

    RECT client_in_screen(HWND hwnd)
    {
        RECT client {};
        GetClientRect(hwnd, &client);
        POINT top_left { client.left, client.top };
        POINT bottom_right { client.right, client.bottom };
        ClientToScreen(hwnd, &top_left);
        ClientToScreen(hwnd, &bottom_right);
        return { top_left.x, top_left.y, bottom_right.x, bottom_right.y };
    }

    bool clip_matches_client(HWND hwnd)
    {
        RECT clip {};
        GetClipCursor(&clip);
        const RECT client = client_in_screen(hwnd);
        return EqualRect(&clip, &client) != FALSE;
    }

    HWND registered_mouse_target()
    {
        UINT count = 0;
        GetRegisteredRawInputDevices(nullptr, &count, sizeof(RAWINPUTDEVICE));
        std::vector<RAWINPUTDEVICE> devices(count);
        if (count == 0 || GetRegisteredRawInputDevices(devices.data(), &count, sizeof(RAWINPUTDEVICE)) == static_cast<UINT>(-1))
            return nullptr;
        for (const auto& device : devices) {
            if (device.usUsagePage == 0x01 && device.usUsage == 0x02)
                return device.hwndTarget;
        }
        return nullptr;
    }

    Core::InputConfig cursor_config(Core::CursorMode mode, bool raw = false)
    {
        Core::InputConfig input;
        input.cursor_mode = mode;
        input.raw_mouse_motion = raw;
        return input;
    }

    TEST(Win32RawMouseTest, RelativeRecordsReportTheirDeltas)
    {
        RAWMOUSE mouse {};
        mouse.usFlags = MOUSE_MOVE_RELATIVE;
        mouse.lLastX = 5;
        mouse.lLastY = -3;
        Core::RawMouseState state;

        const auto delta = Core::raw_mouse_delta(mouse, state, SIZE { 1920, 1080 }, SIZE { 3840, 1080 });

        ASSERT_TRUE(delta.has_value());
        EXPECT_DOUBLE_EQ(delta->first, 5.0);
        EXPECT_DOUBLE_EQ(delta->second, -3.0);
    }

    TEST(Win32RawMouseTest, ARecordWithoutMotionReportsNothing)
    {
        RAWMOUSE mouse {};
        mouse.usFlags = MOUSE_MOVE_RELATIVE;
        Core::RawMouseState state;

        EXPECT_FALSE(Core::raw_mouse_delta(mouse, state, SIZE { 1920, 1080 }, SIZE { 3840, 1080 }).has_value());
    }

    TEST(Win32RawMouseTest, AbsoluteRecordsAreDifferencedAndScaledToPixels)
    {
        RAWMOUSE mouse {};
        mouse.usFlags = MOUSE_MOVE_ABSOLUTE;
        Core::RawMouseState state;
        const SIZE primary { 1920, 1080 };
        const SIZE desktop { 3840, 1080 };

        mouse.lLastX = 0;
        mouse.lLastY = 0;
        EXPECT_FALSE(Core::raw_mouse_delta(mouse, state, primary, desktop).has_value());

        mouse.lLastX = 65535;
        mouse.lLastY = 32768;
        const auto delta = Core::raw_mouse_delta(mouse, state, primary, desktop);

        ASSERT_TRUE(delta.has_value());
        EXPECT_NEAR(delta->first, 1920.0, 0.01);
        EXPECT_NEAR(delta->second, 540.0, 0.1);
    }

    TEST(Win32RawMouseTest, TheVirtualDesktopFlagScalesByTheWholeDesktop)
    {
        RAWMOUSE mouse {};
        mouse.usFlags = MOUSE_MOVE_ABSOLUTE | MOUSE_VIRTUAL_DESKTOP;
        Core::RawMouseState state;
        const SIZE primary { 1920, 1080 };
        const SIZE desktop { 3840, 1080 };

        (void)Core::raw_mouse_delta(mouse, state, primary, desktop);
        mouse.lLastX = 65535;
        const auto delta = Core::raw_mouse_delta(mouse, state, primary, desktop);

        ASSERT_TRUE(delta.has_value());
        EXPECT_NEAR(delta->first, 3840.0, 0.01);
    }

    TEST(Win32WindowTest, AFocusedWindowInALockedModeConfinesTheCursorToItsClientArea)
    {
        ClipRestore restore;
        WindowHarness harness;

        harness.window().set_input_config(cursor_config(Core::CursorMode::DISABLED));
        harness.send(WM_SETFOCUS);
        ASSERT_TRUE(eventually([&] { return clip_matches_client(harness.hwnd()); }));

        harness.send(WM_KILLFOCUS);
        EXPECT_TRUE(eventually([&] { return restore.is_baseline(); }));

        harness.send(WM_SETFOCUS);
        ASSERT_TRUE(eventually([&] { return clip_matches_client(harness.hwnd()); }));

        harness.window().set_input_config(cursor_config(Core::CursorMode::NORMAL));
        EXPECT_TRUE(eventually([&] { return restore.is_baseline(); }));
    }

    TEST(Win32WindowTest, AnUnfocusedWindowDoesNotConfineTheCursor)
    {
        ClipRestore restore;
        WindowHarness harness;

        harness.window().set_input_config(cursor_config(Core::CursorMode::CAPTURED));
        harness.send(WM_KILLFOCUS);
        std::this_thread::sleep_for(100ms);

        EXPECT_TRUE(restore.is_baseline());
    }

    TEST(Win32WindowTest, ADestroyedWindowReleasesItsLock)
    {
        ClipRestore restore;
        {
            WindowHarness harness;
            harness.window().set_input_config(cursor_config(Core::CursorMode::DISABLED));
            harness.send(WM_SETFOCUS);
            ASSERT_TRUE(eventually([&] { return clip_matches_client(harness.hwnd()); }));

            harness.window().destroy();
        }

        EXPECT_TRUE(restore.is_baseline());
    }

    TEST(Win32WindowTest, AHiddenModeHandlesTheClientCursorRequest)
    {
        WindowHarness harness;
        const auto set_cursor = [&] {
            return harness.send(WM_SETCURSOR, reinterpret_cast<WPARAM>(harness.hwnd()), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
        };

        harness.window().set_input_config(cursor_config(Core::CursorMode::HIDDEN));

        EXPECT_TRUE(eventually([&] { return set_cursor() == TRUE; }));
    }

    TEST(Win32WindowTest, LockedMotionReportsAVirtualPositionFromTheClientCentre)
    {
        ClipRestore restore;
        WindowHarness harness;
        harness.window().set_input_config(cursor_config(Core::CursorMode::DISABLED));
        harness.send(WM_SETFOCUS);
        ASSERT_TRUE(eventually([&] { return clip_matches_client(harness.hwnd()); }));

        RECT client {};
        GetClientRect(harness.hwnd(), &client);
        const int cx = client.right / 2;
        const int cy = client.bottom / 2;
        harness.poll();
        harness.clear_events();

        harness.send(WM_MOUSEMOVE, 0, MAKELPARAM(cx + 5, cy + 3));
        harness.poll();
        auto moves = harness.events_of(WindowEventType::MOUSE_MOTION);
        ASSERT_FALSE(moves.empty());
        EXPECT_DOUBLE_EQ(data_of<WindowEvent::MousePosData>(moves.back()).x, cx + 5.0);
        EXPECT_DOUBLE_EQ(data_of<WindowEvent::MousePosData>(moves.back()).y, cy + 3.0);

        const size_t before = moves.size();
        harness.send(WM_MOUSEMOVE, 0, MAKELPARAM(cx, cy));
        harness.send(WM_MOUSEMOVE, 0, MAKELPARAM(cx + 5, cy + 3));
        harness.poll();
        moves = harness.events_of(WindowEventType::MOUSE_MOTION);
        ASSERT_EQ(moves.size(), before + 1);
        EXPECT_DOUBLE_EQ(data_of<WindowEvent::MousePosData>(moves.back()).x, cx + 10.0);
        EXPECT_DOUBLE_EQ(data_of<WindowEvent::MousePosData>(moves.back()).y, cy + 6.0);
    }

    TEST(Win32WindowTest, ACapturedWindowRegistersRawMouseInputOnlyWhileFocused)
    {
        ClipRestore restore;
        WindowHarness harness;
        const HWND before = registered_mouse_target();

        harness.window().set_input_config(cursor_config(Core::CursorMode::CAPTURED));
        harness.send(WM_SETFOCUS);
        ASSERT_TRUE(eventually([&] { return registered_mouse_target() == harness.hwnd(); }));

        harness.send(WM_KILLFOCUS);
        EXPECT_TRUE(eventually([&] { return registered_mouse_target() == before; }));
    }

    // ------------------------------------------------------------------
    // Clipboard
    // ------------------------------------------------------------------

    std::optional<std::string> read_clipboard(WindowHarness& harness)
    {
        int calls = 0;
        std::optional<std::string> got;
        harness.window().request_clipboard_text([&](std::optional<std::string> text) {
            got = std::move(text);
            ++calls;
        });
        EXPECT_TRUE(eventually([&] {
            harness.poll();
            return calls > 0;
        }));
        EXPECT_EQ(calls, 1);
        return got;
    }

    std::optional<std::wstring> raw_clipboard_text()
    {
        for (int attempt = 0; attempt < 50; ++attempt) {
            if (OpenClipboard(nullptr)) {
                std::optional<std::wstring> text;
                if (HANDLE handle = GetClipboardData(CF_UNICODETEXT)) {
                    if (const auto* wide = static_cast<const wchar_t*>(GlobalLock(handle))) {
                        text = std::wstring(wide);
                        GlobalUnlock(handle);
                    }
                }
                CloseClipboard();
                return text;
            }
            std::this_thread::sleep_for(10ms);
        }
        return std::nullopt;
    }

    class ClipboardRestore {
    public:
        explicit ClipboardRestore(WindowHarness& harness)
            : m_harness(harness)
            , m_saved(read_clipboard(harness))
        {
        }

        ClipboardRestore(const ClipboardRestore&) = delete;
        ClipboardRestore& operator=(const ClipboardRestore&) = delete;

        ~ClipboardRestore()
        {
            if (m_saved) {
                m_harness.window().set_clipboard_text(*m_saved);
                (void)read_clipboard(m_harness);
            }
        }

    private:
        WindowHarness& m_harness;
        std::optional<std::string> m_saved;
    };

    TEST(Win32WindowTest, ACopyIsReadBackFromTheClipboard)
    {
        WindowHarness harness;
        ClipboardRestore restore(harness);

        harness.window().set_clipboard_text("h\xC3\xA9llo \xF0\x9F\x8E\xB5");
        const auto got = read_clipboard(harness);

        ASSERT_TRUE(got.has_value());
        EXPECT_EQ(*got, "h\xC3\xA9llo \xF0\x9F\x8E\xB5");
    }

    TEST(Win32WindowTest, NewlinesAreStoredAsCarriageReturnLineFeedAndReadBackAsLineFeed)
    {
        WindowHarness harness;
        ClipboardRestore restore(harness);

        harness.window().set_clipboard_text("a\nb");
        const auto got = read_clipboard(harness);

        ASSERT_TRUE(got.has_value());
        EXPECT_EQ(*got, "a\nb");
        const auto stored = raw_clipboard_text();
        ASSERT_TRUE(stored.has_value());
        EXPECT_EQ(*stored, L"a\r\nb");
    }

    TEST(Win32WindowTest, TextThatIsNotValidUtf8LeavesTheClipboardAlone)
    {
        WindowHarness harness;
        ClipboardRestore restore(harness);

        harness.window().set_clipboard_text("keep");
        harness.window().set_clipboard_text("\xFF\xFE");
        const auto got = read_clipboard(harness);

        ASSERT_TRUE(got.has_value());
        EXPECT_EQ(*got, "keep");
    }

    TEST(Win32WindowTest, AClipboardRequestIsDeliveredFromPollOnce)
    {
        WindowHarness harness;
        ClipboardRestore restore(harness);
        int calls = 0;

        harness.window().request_clipboard_text([&](std::optional<std::string>) { ++calls; });
        EXPECT_EQ(calls, 0);

        EXPECT_TRUE(eventually([&] {
            harness.poll();
            return calls > 0;
        }));
        harness.poll();
        harness.poll();
        EXPECT_EQ(calls, 1);
    }

    TEST(Win32WindowTest, ARequestStillPendingWhenTheWindowIsDestroyedIsDeliveredOnce)
    {
        WindowHarness harness;
        ClipboardRestore restore(harness);
        int calls = 0;

        harness.window().request_clipboard_text([&](std::optional<std::string>) { ++calls; });
        harness.window().destroy();
        harness.poll();

        EXPECT_EQ(calls, 1);
    }

    TEST(Win32WindowTest, ARequestAfterDestroyDeliversNothingOnce)
    {
        WindowHarness harness;
        harness.window().destroy();
        int calls = 0;
        std::optional<std::string> got = "unset";

        harness.window().request_clipboard_text([&](std::optional<std::string> text) {
            got = std::move(text);
            ++calls;
        });
        harness.poll();

        EXPECT_EQ(calls, 1);
        EXPECT_FALSE(got.has_value());
    }

    TEST(Win32WindowTest, AnEmptyClipboardCallbackIsIgnored)
    {
        WindowHarness harness;

        EXPECT_NO_THROW(harness.window().request_clipboard_text({}));
        EXPECT_NO_THROW(harness.poll());
    }

}
}

#endif
