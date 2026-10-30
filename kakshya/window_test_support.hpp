#pragma once

#include "../support/scripted_window.hpp"

#include "MayaFlux/Core/Backends/Graphics/Vulkan/VKEnumUtils.hpp"
#include "MayaFlux/Kakshya/Source/WindowContainer.hpp"
#include "MayaFlux/Registry/BackendRegistry.hpp"
#include "MayaFlux/Registry/Service/DisplayService.hpp"

namespace MayaFlux::Test::WindowSupport {

using SurfaceFormat = Core::GraphicsSurfaceInfo::SurfaceFormat;

constexpr uint32_t k_surface_width = 4;
constexpr uint32_t k_surface_height = 3;
constexpr uint32_t k_surface_channels = 4;
constexpr uint32_t k_history = 4;

inline uint32_t surface_bytes_per_pixel(SurfaceFormat format)
{
    return Core::vk_format_bytes_per_pixel(Core::to_vk_format(format));
}

inline std::vector<uint8_t> surface_bytes(uint32_t width, uint32_t height, SurfaceFormat format, uint64_t seed)
{
    std::vector<uint8_t> bytes(size_t { width } * height * surface_bytes_per_pixel(format));
    for (size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<uint8_t>(seed * 37 + i * 11 + 1);
    }
    return bytes;
}

class FakeDisplay {
public:
    FakeDisplay(uint32_t width, uint32_t height, SurfaceFormat format = SurfaceFormat::R8G8B8A8_UNORM)
        : m_width(width)
        , m_height(height)
        , m_format(format)
    {
        m_service.get_swapchain_format = [this](const std::shared_ptr<void>&) {
            return static_cast<int>(Core::to_vk_format(m_format));
        };
        m_service.get_swapchain_extent = [this](const std::shared_ptr<void>&, uint32_t& w, uint32_t& h) {
            w = m_width;
            h = m_height;
        };
        m_service.get_last_frame = [this](const std::shared_ptr<void>&) { return m_frame; };
        Registry::BackendRegistry::instance().register_service<Registry::Service::DisplayService>(
            [this]() -> void* { return &m_service; });
    }

    FakeDisplay(const FakeDisplay&) = delete;
    FakeDisplay& operator=(const FakeDisplay&) = delete;
    FakeDisplay(FakeDisplay&&) = delete;
    FakeDisplay& operator=(FakeDisplay&&) = delete;

    ~FakeDisplay()
    {
        Registry::BackendRegistry::instance().unregister_service<Registry::Service::DisplayService>();
    }

    std::vector<uint8_t> present(uint64_t seed)
    {
        auto bytes = surface_bytes(m_width, m_height, m_format, seed);
        m_frame = std::make_shared<std::vector<uint8_t>>(bytes);
        return bytes;
    }

    void withdraw_frame() { m_frame.reset(); }

    void resize(uint32_t width, uint32_t height)
    {
        m_width = width;
        m_height = height;
    }

    void set_format(SurfaceFormat format) { m_format = format; }

    [[nodiscard]] uint32_t width() const { return m_width; }
    [[nodiscard]] uint32_t height() const { return m_height; }
    [[nodiscard]] SurfaceFormat format() const { return m_format; }

private:
    uint32_t m_width;
    uint32_t m_height;
    SurfaceFormat m_format;
    std::shared_ptr<std::vector<uint8_t>> m_frame;
    Registry::Service::DisplayService m_service;
};

class WindowProbe : public Kakshya::WindowContainer {
public:
    using Kakshya::WindowContainer::WindowContainer;

    template <typename T>
    void copy_frames(std::vector<T>& out, uint64_t start, uint64_t count) const
    {
        get_frames_impl(out.data(), out.size(), start, count, typeid(T));
    }

    template <typename T>
    [[nodiscard]] T read_value(const std::vector<uint64_t>& coords, T sentinel) const
    {
        get_value_impl(coords, &sentinel, typeid(T));
        return sentinel;
    }
};

class WindowRig {
public:
    explicit WindowRig(uint32_t history = k_history, SurfaceFormat format = SurfaceFormat::R8G8B8A8_UNORM)
        : window(std::make_shared<ScriptedWindow>(k_surface_width, k_surface_height))
        , display(k_surface_width, k_surface_height, format)
        , container(std::make_shared<WindowProbe>(window, history))
    {
    }

    std::shared_ptr<ScriptedWindow> window;
    FakeDisplay display;
    std::shared_ptr<WindowProbe> container;
};

inline std::vector<uint8_t> raw_bytes(const Kakshya::DataVariant& variant)
{
    return std::visit(
        [](const auto& v) {
            const auto* first = reinterpret_cast<const uint8_t*>(v.data());
            return std::vector<uint8_t>(first, first + v.size() * sizeof(typename std::decay_t<decltype(v)>::value_type));
        },
        variant);
}

inline std::type_index element_type(const Kakshya::DataVariant& variant)
{
    return std::visit(
        [](const auto& v) { return std::type_index(typeid(typename std::decay_t<decltype(v)>::value_type)); },
        variant);
}

inline std::vector<uint8_t> processed_pixels(const Kakshya::WindowContainer& container)
{
    const auto& processed = container.get_processed_data();
    return processed.empty() ? std::vector<uint8_t> {} : raw_bytes(processed[0]);
}

inline std::vector<uint8_t> history_slot(Kakshya::WindowContainer& container, uint32_t index)
{
    const auto& data = container.get_data();
    return index < data.size() ? raw_bytes(data[index]) : std::vector<uint8_t> {};
}

}
