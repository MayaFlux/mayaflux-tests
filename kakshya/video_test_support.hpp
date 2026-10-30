#pragma once

#include "MayaFlux/Kakshya/Source/DynamicVideoStream.hpp"
#include "MayaFlux/Kakshya/Source/VideoStreamContainer.hpp"
#include "MayaFlux/Registry/BackendRegistry.hpp"
#include "MayaFlux/Registry/Service/IOService.hpp"

namespace MayaFlux::Test::VideoSupport {

constexpr uint32_t k_width = 4;
constexpr uint32_t k_height = 2;
constexpr uint32_t k_rgba8_frame_bytes = k_width * k_height * 4;
constexpr double k_fps = 30.0;

inline std::vector<uint8_t> frame_bytes(uint64_t frame, size_t size = k_rgba8_frame_bytes)
{
    std::vector<uint8_t> bytes(size);
    for (size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<uint8_t>(10 * frame + i + 1);
    }
    return bytes;
}

inline std::vector<uint8_t> frames_bytes(uint64_t first, uint64_t count, size_t size = k_rgba8_frame_bytes)
{
    std::vector<uint8_t> bytes;
    for (uint64_t f = first; f < first + count; ++f) {
        const auto one = frame_bytes(f, size);
        bytes.insert(bytes.end(), one.begin(), one.end());
    }
    return bytes;
}

class VideoProbe : public Kakshya::VideoStreamContainer {
public:
    using Kakshya::VideoStreamContainer::VideoStreamContainer;

    template <typename T>
    void load(std::vector<T> values, uint64_t frames)
    {
        m_data.resize(1);
        m_data[0] = std::move(values);
        m_num_frames = frames;
        setup_dimensions();
    }

    template <typename T>
    [[nodiscard]] T read_value(const std::vector<uint64_t>& coords, T sentinel) const
    {
        get_value_impl(coords, &sentinel, typeid(T));
        return sentinel;
    }

    template <typename T>
    void write_value(const std::vector<uint64_t>& coords, T value)
    {
        set_value_impl(coords, &value, typeid(T));
    }

    template <typename T>
    void copy_frames(std::vector<T>& out, uint64_t start, uint64_t count) const
    {
        get_frames_impl(out.data(), out.size(), start, count, typeid(T));
    }
};

inline std::shared_ptr<VideoProbe> make_flat_rgba8(uint64_t frames)
{
    auto video = std::make_shared<VideoProbe>(k_width, k_height, Kakshya::ImageFormat::RGBA8, k_fps);
    video->load<uint8_t>(frames_bytes(0, frames), frames);
    return video;
}

inline std::shared_ptr<Kakshya::VideoStreamContainer> make_ring_rgba8(uint64_t total, uint32_t capacity)
{
    auto video = std::make_shared<Kakshya::VideoStreamContainer>();
    video->setup_ring(total, capacity, k_width, k_height, Kakshya::ImageFormat::RGBA8, k_fps, 0);
    return video;
}

inline void commit_pattern(Kakshya::VideoStreamContainer& video, uint64_t frame)
{
    auto* slot = video.mutable_slot_ptr(frame);
    if (!slot) {
        return;
    }
    const auto bytes = frame_bytes(frame, video.get_frame_byte_size());
    std::memcpy(slot, bytes.data(), bytes.size());
    video.commit_frame(frame);
}

inline std::vector<uint8_t> pixels_of(const Kakshya::VideoStreamContainer& video, uint64_t frame)
{
    const auto span = video.get_frame_pixels(frame);
    return { span.begin(), span.end() };
}

class RecordingIOService {
public:
    RecordingIOService()
    {
        m_service.request_decode = [this](uint64_t id) { decodes.push_back(id); };
        m_service.request_frame = [this](uint64_t id) { frames.push_back(id); };
        Registry::BackendRegistry::instance().register_service<Registry::Service::IOService>(
            [this]() -> void* { return &m_service; });
    }

    RecordingIOService(const RecordingIOService&) = delete;
    RecordingIOService& operator=(const RecordingIOService&) = delete;
    RecordingIOService(RecordingIOService&&) = delete;
    RecordingIOService& operator=(RecordingIOService&&) = delete;

    ~RecordingIOService()
    {
        Registry::BackendRegistry::instance().unregister_service<Registry::Service::IOService>();
    }

    [[nodiscard]] Registry::Service::IOService& service() { return m_service; }

    std::vector<uint64_t> decodes;
    std::vector<uint64_t> frames;

private:
    Registry::Service::IOService m_service;
};

inline std::vector<uint8_t> processed_bytes(const Kakshya::VideoStreamContainer& video)
{
    const auto& processed = video.get_processed_data();
    if (processed.empty()) {
        return {};
    }
    return std::get<std::vector<uint8_t>>(processed[0]);
}

inline Kakshya::VideoStreamSpec rgba8_spec(uint64_t ring_frames = 0, double frame_rate = k_fps)
{
    return { .width = k_width,
        .height = k_height,
        .format = Kakshya::ImageFormat::RGBA8,
        .frame_rate = frame_rate,
        .ring_frames = ring_frames };
}

inline uint64_t append_ramp(Kakshya::DynamicVideoStream& stream, uint64_t count, uint64_t first = 0)
{
    uint64_t stored = 0;
    for (uint64_t f = first; f < first + count; ++f) {
        const auto bytes = frame_bytes(f, stream.get_frame_byte_size());
        stored += stream.append_frame(std::span<const uint8_t>(bytes)) ? 1 : 0;
    }
    return stored;
}

}
