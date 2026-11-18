#pragma once

#include <glm/vec2.hpp>

#include "MayaFlux/Core/Backends/Windowing/Window.hpp"
#include "MayaFlux/IO/Keys.hpp"
#include "MayaFlux/Vruta/WindowEventSource.hpp"

namespace MayaFlux::Test {

/**
 * @brief Headless Core::Window whose input events are injected by the test.
 *
 * Owns a real Vruta::WindowEventSource, so Kriya input coroutines registered
 * against it resume exactly as they do under a native backend. Nothing is
 * created on screen and no device is touched.
 */
class ScriptedWindow final : public Core::Window {
public:
    ScriptedWindow(uint32_t width, uint32_t height)
    {
        m_state.current_width = width;
        m_state.current_height = height;
        m_info.width = width;
        m_info.height = height;
    }

    void move_to_pixel(double x, double y)
    {
        signal(Core::WindowEventType::MOUSE_MOTION, Core::WindowEvent::MousePosData { x, y });
    }

    void move_to_ndc(glm::vec2 ndc)
    {
        move_to_pixel(
            (static_cast<double>(ndc.x) + 1.0) * 0.5 * m_state.current_width,
            (1.0 - static_cast<double>(ndc.y)) * 0.5 * m_state.current_height);
    }

    void press(IO::MouseButtons button, IO::Modifiers mods = IO::Modifiers::None)
    {
        signal(Core::WindowEventType::MOUSE_BUTTON_PRESSED,
            Core::WindowEvent::MouseButtonData { static_cast<int8_t>(button), mods });
    }

    void release(IO::MouseButtons button, IO::Modifiers mods = IO::Modifiers::None)
    {
        signal(Core::WindowEventType::MOUSE_BUTTON_RELEASED,
            Core::WindowEvent::MouseButtonData { static_cast<int8_t>(button), mods });
    }

    void scroll(double dx, double dy)
    {
        signal(Core::WindowEventType::MOUSE_SCROLLED, Core::WindowEvent::ScrollData { dx, dy });
    }

    void key_down(IO::Keys key, IO::Modifiers mods = IO::Modifiers::None)
    {
        signal(Core::WindowEventType::KEY_PRESSED,
            Core::WindowEvent::KeyData { static_cast<int16_t>(key), 0, mods });
    }

    void key_up(IO::Keys key, IO::Modifiers mods = IO::Modifiers::None)
    {
        signal(Core::WindowEventType::KEY_RELEASED,
            Core::WindowEvent::KeyData { static_cast<int16_t>(key), 0, mods });
    }

    void key_repeat(IO::Keys key, IO::Modifiers mods = IO::Modifiers::None)
    {
        signal(Core::WindowEventType::KEY_REPEAT,
            Core::WindowEvent::KeyData { static_cast<int16_t>(key), 0, mods });
    }

    void type(uint32_t codepoint)
    {
        signal(Core::WindowEventType::TEXT_INPUT, Core::WindowEvent::TextData { codepoint });
    }

    void resize(uint32_t width, uint32_t height)
    {
        m_state.current_width = width;
        m_state.current_height = height;
        signal(Core::WindowEventType::WINDOW_RESIZED, Core::WindowEvent::ResizeData { width, height });
    }

    /// @brief Change the reported size without signalling a resize, as a
    ///        compositor that answers the requested size with another can.
    void change_size_silently(uint32_t width, uint32_t height)
    {
        m_state.current_width = width;
        m_state.current_height = height;
    }

    void close()
    {
        signal(Core::WindowEventType::WINDOW_CLOSED, std::monostate {});
    }

    /// @brief Text the scripted clipboard holds; nullopt when it holds none.
    void set_clipboard(std::optional<std::string> text) { m_clipboard = std::move(text); }

    [[nodiscard]] const std::optional<std::string>& clipboard() const noexcept { return m_clipboard; }

    /// @brief Paste requests wait for poll(), as on every native backend.
    void request_clipboard_text(Core::ClipboardTextCallback callback) override
    {
        m_clipboard_requests.push_back(std::move(callback));
    }

    void set_clipboard_text(const std::string& text) override { m_clipboard = text; }

    /// @brief Deliver the waiting paste requests with the clipboard as it is now.
    void poll() override
    {
        auto requests = std::move(m_clipboard_requests);
        m_clipboard_requests.clear();
        for (auto& callback : requests) {
            if (callback)
                callback(m_clipboard);
        }
    }

    void show() override { m_state.is_visible = true; }
    void hide() override { m_state.is_visible = false; }
    void destroy() override { }
    [[nodiscard]] bool should_close() const override { return m_close_requested; }
    void request_close() override { m_close_requested = true; }
    [[nodiscard]] const Core::WindowState& get_state() const override { return m_state; }
    [[nodiscard]] const Core::WindowCreateInfo& get_create_info() const override { return m_info; }
    void set_input_config(const Core::InputConfig& config) override { m_input = config; }
    [[nodiscard]] const Core::InputConfig& get_input_config() const override { return m_input; }
    void set_event_callback(Core::WindowEventCallback) override { }
    [[nodiscard]] void* get_native_handle() const override { return nullptr; }
    [[nodiscard]] void* get_native_display() const override { return nullptr; }
    void set_title(const std::string& title) override { m_info.title = title; }
    void set_size(uint32_t width, uint32_t height) override
    {
        m_state.current_width = width;
        m_state.current_height = height;
    }
    void set_position(uint32_t, uint32_t) override { }
    void set_color(const std::array<float, 4>&) override { }
    [[nodiscard]] Vruta::WindowEventSource& get_event_source() override { return m_source; }
    [[nodiscard]] const Vruta::WindowEventSource& get_event_source() const override { return m_source; }
    [[nodiscard]] bool is_graphics_registered() const override { return m_graphics_registered; }
    void set_graphics_registered(bool registered) override { m_graphics_registered = registered; }
    void register_rendering_buffer(std::shared_ptr<Buffers::VKBuffer>) override { }
    void unregister_rendering_buffer(std::shared_ptr<Buffers::VKBuffer>) override { }
    void track_frame_command(uint64_t id) override { m_frame_commands.push_back(id); }
    [[nodiscard]] const std::vector<uint64_t>& get_frame_commands() const override { return m_frame_commands; }
    void clear_frame_commands() override { m_frame_commands.clear(); }
    [[nodiscard]] bool is_capture_enabled() const override { return m_capture; }
    void set_capture_enabled(bool enabled) override { m_capture = enabled; }
    [[nodiscard]] std::vector<std::shared_ptr<Buffers::VKBuffer>> get_rendering_buffers() const override { return {}; }

private:
    void signal(Core::WindowEventType type, Core::WindowEvent::EventData data)
    {
        Core::WindowEvent event;
        event.type = type;
        event.timestamp = 0.0;
        event.data = std::move(data);
        m_source.signal(std::move(event));
    }

    Vruta::WindowEventSource m_source;
    Core::WindowState m_state;
    Core::WindowCreateInfo m_info;
    Core::InputConfig m_input;
    std::vector<uint64_t> m_frame_commands;
    std::optional<std::string> m_clipboard;
    std::vector<Core::ClipboardTextCallback> m_clipboard_requests;
    bool m_close_requested {};
    bool m_graphics_registered {};
    bool m_capture {};
};

} // namespace MayaFlux::Test
