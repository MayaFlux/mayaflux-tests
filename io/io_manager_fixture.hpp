#pragma once

#include "gtest/gtest.h"

#include "file_contract_fixture.hpp"
#include "ffmpeg/ffmpeg_fixture.hpp"

#include "MayaFlux/Buffers/BufferManager.hpp"
#include "MayaFlux/Core/GlobalStreamInfo.hpp"
#include "MayaFlux/IO/Audio/SoundFileReader.hpp"
#include "MayaFlux/IO/Audio/SoundFileWriter.hpp"
#include "MayaFlux/IO/Camera/CameraSource.hpp"
#include "MayaFlux/IO/IOManager.hpp"
#include "MayaFlux/IO/Image/ImageReader.hpp"
#include "MayaFlux/IO/Model/ModelReader.hpp"
#include "MayaFlux/IO/Video/VideoFileWriter.hpp"
#include "MayaFlux/Kakshya/Source/CameraContainer.hpp"
#include "MayaFlux/Kakshya/Source/DynamicSoundStream.hpp"
#include "MayaFlux/Kakshya/Source/SoundFileContainer.hpp"
#include "MayaFlux/Kakshya/Source/VideoFileContainer.hpp"
#include "MayaFlux/Registry/BackendRegistry.hpp"
#include "MayaFlux/Registry/Service/AudioBackendService.hpp"
#include "MayaFlux/Registry/Service/IOService.hpp"
#include "MayaFlux/Vruta/Scheduler.hpp"

namespace MayaFlux::Test {

/**
 * @brief A CameraSource with no device behind it, counting what IOManager asks of it.
 */
class CountingCameraSource final : public IO::CameraSource {
public:
    [[nodiscard]] std::vector<IO::CameraConfig> enumerate_configs() const override { return {}; }
    [[nodiscard]] std::vector<IO::CameraMode> preset_modes() const override { return {}; }
    [[nodiscard]] bool open(const IO::CameraConfig&) override { return true; }
    void close() override { }
    [[nodiscard]] bool is_open() const override { return true; }
    [[nodiscard]] std::shared_ptr<Kakshya::CameraContainer> create_container() const override { return nullptr; }
    bool pull_frame(const std::shared_ptr<Kakshya::CameraContainer>&) override { return true; }
    [[nodiscard]] uint32_t width() const override { return 8; }
    [[nodiscard]] uint32_t height() const override { return 4; }
    [[nodiscard]] double frame_rate() const override { return 30.0; }
    void set_container(const std::shared_ptr<Kakshya::CameraContainer>&) override { }
    void pull_frame_all() override { pulls.fetch_add(1, std::memory_order_relaxed); }
    [[nodiscard]] const std::string& last_error() const override { return error; }
    void setup_io_service(uint64_t) override { }

    std::atomic<int> pulls { 0 };
    std::string error;
};

/**
 * @brief An AudioBackendService backed by plain bookkeeping, with no audio device.
 *
 * Registers itself in BackendRegistry on construction and removes the
 * registration on destruction. Observers are recorded, never invoked.
 */
class FakeAudioBackend {
public:
    FakeAudioBackend()
    {
        service.register_output_observer = [this](std::function<void(const double*, uint32_t)> observer) {
            const std::lock_guard lock(mutex);
            const uint32_t id = next_id++;
            observers.emplace(id, std::move(observer));
            return id;
        };
        service.unregister_output_observer = [this](uint32_t id) {
            const std::lock_guard lock(mutex);
            observers.erase(id);
            unregistered.push_back(id);
        };
        service.get_output_snapshot = [] { return std::span<const double> {}; };

        Registry::BackendRegistry::instance().register_service<Registry::Service::AudioBackendService>(
            [this]() -> void* { return &service; });
    }

    ~FakeAudioBackend()
    {
        Registry::BackendRegistry::instance().unregister_service<Registry::Service::AudioBackendService>();
    }

    FakeAudioBackend(const FakeAudioBackend&) = delete;
    FakeAudioBackend& operator=(const FakeAudioBackend&) = delete;
    FakeAudioBackend(FakeAudioBackend&&) = delete;
    FakeAudioBackend& operator=(FakeAudioBackend&&) = delete;

    [[nodiscard]] size_t observer_count() const
    {
        const std::lock_guard lock(mutex);
        return observers.size();
    }

    [[nodiscard]] std::vector<uint32_t> unregistered_ids() const
    {
        const std::lock_guard lock(mutex);
        return unregistered;
    }

    Registry::Service::AudioBackendService service;

private:
    mutable std::mutex mutex;
    uint32_t next_id { 100 };
    std::map<uint32_t, std::function<void(const double*, uint32_t)>> observers;
    std::vector<uint32_t> unregistered;
};

/**
 * @brief An IOManager over a real BufferManager and TaskScheduler, in a scratch directory.
 *
 * The IOService and AudioBackendService registrations are cleared before and
 * after every test so no case sees another's services. The manager is built in
 * SetUp and destroyed in TearDown; a test that needs a different one calls
 * rebuild().
 */
class IOManagerFixture : public FileContractFixture {
protected:
    static constexpr uint32_t k_rate = 48000;
    static constexpr uint32_t k_block = 512;
    static constexpr uint32_t k_fps = 30;

    void SetUp() override
    {
        clear_services();
        stream_info.sample_rate = k_rate;
        stream_info.buffer_size = k_block;
        stream_info.output.channels = 2;
        buffers = std::make_shared<Buffers::BufferManager>(2, 2, k_rate, k_block);
        scheduler = std::make_shared<Vruta::TaskScheduler>(k_rate);
        manager = std::make_unique<IO::IOManager>(stream_info, k_fps, buffers, scheduler);
    }

    void TearDown() override
    {
        manager.reset();
        clear_services();
    }

    void rebuild()
    {
        manager.reset();
        manager = std::make_unique<IO::IOManager>(stream_info, k_fps, buffers, scheduler);
    }

    static void clear_services()
    {
        Registry::BackendRegistry::instance().unregister_service<Registry::Service::IOService>();
        Registry::BackendRegistry::instance().unregister_service<Registry::Service::AudioBackendService>();
    }

    [[nodiscard]] static Registry::Service::IOService* io_service()
    {
        return Registry::BackendRegistry::instance().get_service<Registry::Service::IOService>();
    }

    [[nodiscard]] std::string path_of(const std::string& name) const
    {
        return (root() / name).string();
    }

    [[nodiscard]] bool exists(const std::string& name) const
    {
        std::error_code ec;
        return std::filesystem::is_regular_file(root() / name, ec);
    }

    [[nodiscard]] uintmax_t size_of(const std::string& name) const
    {
        std::error_code ec;
        const auto size = std::filesystem::file_size(root() / name, ec);
        return ec ? 0 : size;
    }

    void write_wav(const std::string& name, uint16_t channels = 2, uint32_t frames = 4800)
    {
        FFmpegFixture::write_bytes(root() / name, FFmpegFixture::make_wav(channels, k_rate, frames));
    }

    [[nodiscard]] bool write_clip(const std::string& name, uint32_t frames = 10)
    {
        return FFmpegFixture::write_mpeg4_clip(root() / name, 64, 48, frames, 30.0);
    }

    [[nodiscard]] static IO::VideoLoadConfig small_video_config()
    {
        IO::VideoLoadConfig config;
        config.target_width = 64;
        config.target_height = 48;
        return config;
    }

    Core::GlobalStreamInfo stream_info;
    std::shared_ptr<Buffers::BufferManager> buffers;
    std::shared_ptr<Vruta::TaskScheduler> scheduler;
    std::unique_ptr<IO::IOManager> manager;
};

}
