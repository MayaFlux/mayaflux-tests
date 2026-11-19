#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#if defined(MAYAFLUX_PLATFORM_WINDOWS) && defined(WASAPI_BACKEND)

#include <chrono>
#include <stdexcept>

#include "MayaFlux/Core/Backends/Audio/WasapiBackend.hpp"
#include "MayaFlux/Core/SubsystemManager.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::wasapi>() }))

namespace MayaFlux::Test {
namespace {

    using namespace std::chrono_literals;

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

    Core::GlobalStreamInfo output_config(uint32_t channels = 2)
    {
        Core::GlobalStreamInfo config;
        config.sample_rate = 48000;
        config.buffer_size = 512;
        config.output.enabled = true;
        config.output.channels = channels;
        config.input.enabled = false;
        config.input.channels = 0;
        return config;
    }

    std::function<int(void*, void*, unsigned int)> silence_callback(
        const Core::GlobalStreamInfo& config,
        std::shared_ptr<std::atomic<uint32_t>> counter)
    {
        return [&config, counter](void* output, void*, unsigned int frames) {
            if (output && frames > 0) {
                std::memset(output, 0,
                    static_cast<std::size_t>(frames) * config.output.channels * sizeof(double));
                counter->fetch_add(1, std::memory_order_release);
            }
            return 0;
        };
    }

    std::optional<Core::DeviceInfo> default_info(
        const std::vector<Core::DeviceInfo>& devices, unsigned int id)
    {
        if (id == 0 || id > devices.size())
            return std::nullopt;
        return devices[id - 1];
    }

    TEST(WasapiBackendTest, CreatesBackendAndReportsVersion)
    {
        Core::WasapiBackend backend;
        EXPECT_EQ(backend.get_version_string(), "WASAPI (Windows native shared mode)");
        EXPECT_NE(backend.enumerator(), nullptr);
        EXPECT_NE(backend.create_device_manager(), nullptr);
    }

    TEST(WasapiBackendTest, EnumeratesDevicesWithUsableIdsAndDefaults)
    {
        Core::WasapiBackend backend;
        auto devices = backend.create_device_manager();
        ASSERT_NE(devices, nullptr);
        auto* wasapi_devices = dynamic_cast<Core::WasapiDevice*>(devices.get());
        ASSERT_NE(wasapi_devices, nullptr);

        const auto outputs = devices->get_output_devices();
        const auto inputs = devices->get_input_devices();

        for (const auto& output : outputs) {
            EXPECT_FALSE(output.name.empty());
            EXPECT_GT(output.output_channels, 0U);
            EXPECT_GT(output.preferred_sample_rate, 0U);
        }
        for (const auto& input : inputs) {
            EXPECT_FALSE(input.name.empty());
            EXPECT_GT(input.input_channels, 0U);
            EXPECT_GT(input.preferred_sample_rate, 0U);
        }

        if (!outputs.empty()) {
            const auto default_output = devices->get_default_output_device();
            ASSERT_LE(default_output, outputs.size());
            if (default_output != 0)
                EXPECT_TRUE(outputs[default_output - 1].is_default_output);

            for (std::size_t id = 0; id <= outputs.size(); ++id) {
                auto* endpoint = wasapi_devices->resolve_device(static_cast<unsigned int>(id), eRender);
                EXPECT_NE(endpoint, nullptr);
                if (endpoint)
                    endpoint->Release();
            }
            EXPECT_EQ(wasapi_devices->resolve_device(static_cast<unsigned int>(outputs.size() + 1), eRender), nullptr);
        }

        if (!inputs.empty()) {
            const auto default_input = devices->get_default_input_device();
            ASSERT_LE(default_input, inputs.size());
            if (default_input != 0)
                EXPECT_TRUE(inputs[default_input - 1].is_default_input);

            for (std::size_t id = 0; id <= inputs.size(); ++id) {
                auto* endpoint = wasapi_devices->resolve_device(static_cast<unsigned int>(id), eCapture);
                EXPECT_NE(endpoint, nullptr);
                if (endpoint)
                    endpoint->Release();
            }
            EXPECT_EQ(wasapi_devices->resolve_device(static_cast<unsigned int>(inputs.size() + 1), eCapture), nullptr);
        }
    }

    MAYAFLUX_TEST_WITH(TEST, WasapiBackendTest, OpensDefaultOutputAndReportsStreamState, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::audio_output, ::MayaFlux::Test::Meta::Capability::wasapi>() }))
    {
        Core::WasapiBackend backend;
        auto devices = backend.create_device_manager();
        ASSERT_NE(devices, nullptr);
        if (devices->get_output_devices().empty())
            GTEST_SKIP() << "WASAPI has no active render endpoint";

        auto config = output_config();
        auto stream = backend.create_stream(0, 0, config, nullptr);
        ASSERT_NE(stream, nullptr);
        EXPECT_FALSE(stream->is_open());
        EXPECT_FALSE(stream->is_running());

        ASSERT_NO_THROW(stream->open());
        EXPECT_TRUE(stream->is_open());
        EXPECT_FALSE(stream->is_running());
        stream->close();
        EXPECT_FALSE(stream->is_open());
        EXPECT_FALSE(stream->is_running());
    }

    TEST(WasapiBackendTest, StartRequiresOpenStream)
    {
        Core::WasapiBackend backend;
        auto devices = backend.create_device_manager();
        ASSERT_NE(devices, nullptr);
        if (devices->get_output_devices().empty())
            GTEST_SKIP() << "WASAPI has no active render endpoint";

        auto config = output_config();
        auto stream = backend.create_stream(devices->get_default_output_device(),
            devices->get_default_input_device(), config, nullptr);
        ASSERT_NE(stream, nullptr);
        EXPECT_ANY_THROW(stream->start());
        EXPECT_FALSE(stream->is_open());
        EXPECT_FALSE(stream->is_running());
    }

    TEST(WasapiBackendTest, RejectsOutputDeviceIdBeyondEnumeratedRange)
    {
        Core::WasapiBackend backend;
        auto devices = backend.create_device_manager();
        ASSERT_NE(devices, nullptr);
        const auto outputs = devices->get_output_devices();

        auto config = output_config();
        EXPECT_ANY_THROW(backend.create_stream(static_cast<unsigned int>(outputs.size() + 1), 0, config, nullptr));
    }

    MAYAFLUX_TEST_WITH(TEST, WasapiBackendTest, DeliversOutputCallbacksThatWriteSilence, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::audio_output, ::MayaFlux::Test::Meta::Capability::wasapi>() }))
    {
        Core::WasapiBackend backend;
        auto devices = backend.create_device_manager();
        ASSERT_NE(devices, nullptr);
        if (devices->get_output_devices().empty())
            GTEST_SKIP() << "WASAPI has no active render endpoint";

        auto config = output_config();
        auto stream = backend.create_stream(devices->get_default_output_device(),
            devices->get_default_input_device(), config, nullptr);
        ASSERT_NE(stream, nullptr);

        auto callback_count = std::make_shared<std::atomic<uint32_t>>(0);
        stream->set_process_callback(silence_callback(config, callback_count));

        stream->open();
        stream->start();
        EXPECT_TRUE(eventually([&] { return callback_count->load(std::memory_order_acquire) >= 2; }));
        EXPECT_TRUE(stream->is_open());
        EXPECT_TRUE(stream->is_running());
        stream->stop();
        EXPECT_FALSE(stream->is_running());
        stream->close();
        EXPECT_FALSE(stream->is_open());
    }

    MAYAFLUX_TEST_WITH(TEST, WasapiBackendTest, RestartsPausesAndResumesOutputStream, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::audio_output, ::MayaFlux::Test::Meta::Capability::wasapi>() }))
    {
        Core::WasapiBackend backend;
        auto devices = backend.create_device_manager();
        ASSERT_NE(devices, nullptr);
        if (devices->get_output_devices().empty())
            GTEST_SKIP() << "WASAPI has no active render endpoint";

        auto config = output_config();
        auto stream = backend.create_stream(devices->get_default_output_device(),
            devices->get_default_input_device(), config, nullptr);
        ASSERT_NE(stream, nullptr);
        auto callback_count = std::make_shared<std::atomic<uint32_t>>(0);
        stream->set_process_callback(silence_callback(config, callback_count));

        stream->open();
        stream->start();
        ASSERT_TRUE(eventually([&] { return callback_count->load(std::memory_order_acquire) >= 2; }));
        stream->stop();
        EXPECT_FALSE(stream->is_running());
        EXPECT_TRUE(stream->is_open());

        const auto before_restart = callback_count->load(std::memory_order_acquire);
        stream->start();
        ASSERT_TRUE(eventually([&] {
            return callback_count->load(std::memory_order_acquire) >= before_restart + 2;
        }));

        stream->pause();
        EXPECT_FALSE(stream->is_running());
        EXPECT_TRUE(stream->is_open());
        const auto before_resume = callback_count->load(std::memory_order_acquire);
        stream->resume();
        EXPECT_TRUE(eventually([&] {
            return callback_count->load(std::memory_order_acquire) >= before_resume + 2;
        }));
        EXPECT_TRUE(stream->is_running());
        stream->stop();
        stream->close();
    }

    MAYAFLUX_TEST_WITH(TEST, WasapiBackendTest, DeliversFullDuplexCallbackWhenInputAndOutputExist, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::audio_input, ::MayaFlux::Test::Meta::Capability::audio_output, ::MayaFlux::Test::Meta::Capability::wasapi>() }))
    {
        Core::WasapiBackend backend;
        auto devices = backend.create_device_manager();
        ASSERT_NE(devices, nullptr);
        if (devices->get_output_devices().empty() || devices->get_input_devices().empty())
            GTEST_SKIP() << "WASAPI full duplex requires both render and capture endpoints";

        auto config = output_config();
        config.input.enabled = true;
        config.input.channels = 1;
        backend.resolve_stream_info(devices->get_default_output_device(),
            devices->get_default_input_device(), config);
        if (!config.input.enabled)
            GTEST_SKIP() << "WASAPI capture endpoint cannot be negotiated";

        auto stream = backend.create_stream(devices->get_default_output_device(),
            devices->get_default_input_device(), config, nullptr);
        ASSERT_NE(stream, nullptr);

        auto output_callbacks = std::make_shared<std::atomic<uint32_t>>(0);
        auto input_callbacks = std::make_shared<std::atomic<uint32_t>>(0);
        stream->set_process_callback([&config, output_callbacks, input_callbacks](void* output, void* input, unsigned int frames) {
            if (output && frames > 0) {
                std::memset(output, 0,
                    static_cast<std::size_t>(frames) * config.output.channels * sizeof(double));
                output_callbacks->fetch_add(1, std::memory_order_release);
                if (input)
                    input_callbacks->fetch_add(1, std::memory_order_release);
            }
            return 0;
        });

        stream->open();
        stream->start();
        EXPECT_TRUE(eventually([&] { return input_callbacks->load(std::memory_order_acquire) > 0; }, 4s));
        EXPECT_GT(output_callbacks->load(std::memory_order_acquire), 0U);
        stream->stop();
        stream->close();
    }

    TEST(WasapiBackendTest, ResolveKeepsAConfigTheDefaultOutputAlreadyProvides)
    {
        Core::WasapiBackend backend;
        auto devices = backend.create_device_manager();
        ASSERT_NE(devices, nullptr);
        const auto output = default_info(devices->get_output_devices(), devices->get_default_output_device());
        if (!output)
            GTEST_SKIP() << "WASAPI has no default render endpoint";

        auto config = output_config(output->output_channels);
        config.sample_rate = output->preferred_sample_rate;
        backend.resolve_stream_info(devices->get_default_output_device(),
            devices->get_default_input_device(), config);

        EXPECT_EQ(config.output.channels, output->output_channels);
        EXPECT_EQ(config.sample_rate, output->preferred_sample_rate);
    }

    TEST(WasapiBackendTest, ResolveNeverInventsAnOutputLayoutTheDeviceLacks)
    {
        Core::WasapiBackend backend;
        auto devices = backend.create_device_manager();
        ASSERT_NE(devices, nullptr);
        const auto output = default_info(devices->get_output_devices(), devices->get_default_output_device());
        if (!output)
            GTEST_SKIP() << "WASAPI has no default render endpoint";

        for (const uint32_t channels : { 1U, 2U, 4U, 8U }) {
            for (const uint32_t rate : { 44100U, 48000U, 96000U }) {
                auto config = output_config(channels);
                config.sample_rate = rate;
                backend.resolve_stream_info(devices->get_default_output_device(),
                    devices->get_default_input_device(), config);

                EXPECT_TRUE(config.output.channels == channels || config.output.channels == output->output_channels)
                    << channels << " channels at " << rate << " Hz resolved to " << config.output.channels;
                EXPECT_TRUE(config.sample_rate == rate || config.sample_rate == output->preferred_sample_rate)
                    << channels << " channels at " << rate << " Hz resolved to " << config.sample_rate << " Hz";
            }
        }
    }

    TEST(WasapiBackendTest, ResolveIsStableWhenAppliedTwice)
    {
        Core::WasapiBackend backend;
        auto devices = backend.create_device_manager();
        ASSERT_NE(devices, nullptr);
        if (devices->get_output_devices().empty())
            GTEST_SKIP() << "WASAPI has no active render endpoint";

        auto config = output_config(1);
        config.sample_rate = 44100;
        backend.resolve_stream_info(devices->get_default_output_device(),
            devices->get_default_input_device(), config);
        const auto first = config;
        backend.resolve_stream_info(devices->get_default_output_device(),
            devices->get_default_input_device(), config);

        EXPECT_EQ(config.output.channels, first.output.channels);
        EXPECT_EQ(config.sample_rate, first.sample_rate);
    }

    TEST(WasapiBackendTest, ResolveLeavesDisabledInputAlone)
    {
        Core::WasapiBackend backend;
        auto devices = backend.create_device_manager();
        ASSERT_NE(devices, nullptr);

        auto config = output_config();
        backend.resolve_stream_info(devices->get_default_output_device(),
            devices->get_default_input_device(), config);

        EXPECT_FALSE(config.input.enabled);
        EXPECT_EQ(config.input.channels, 0U);
    }

    TEST(WasapiBackendTest, ResolveLeavesOutputAloneForADeviceBeyondEnumeratedRange)
    {
        Core::WasapiBackend backend;
        auto devices = backend.create_device_manager();
        ASSERT_NE(devices, nullptr);
        const auto beyond = static_cast<unsigned int>(devices->get_output_devices().size() + 1);

        auto config = output_config(4);
        config.sample_rate = 44100;
        backend.resolve_stream_info(beyond, 0, config);

        EXPECT_EQ(config.output.channels, 4U);
        EXPECT_EQ(config.sample_rate, 44100U);
    }

    TEST(WasapiBackendTest, ResolveDisablesInputForACaptureDeviceBeyondEnumeratedRange)
    {
        Core::WasapiBackend backend;
        auto devices = backend.create_device_manager();
        ASSERT_NE(devices, nullptr);
        const auto beyond = static_cast<unsigned int>(devices->get_input_devices().size() + 1);

        auto config = output_config();
        config.input.enabled = true;
        config.input.channels = 2;
        backend.resolve_stream_info(devices->get_default_output_device(), beyond, config);

        EXPECT_FALSE(config.input.enabled);
        EXPECT_EQ(config.input.channels, 0U);
    }

    MAYAFLUX_TEST_WITH(TEST, WasapiBackendTest, ResolveFitsInputToTheDefaultCaptureDevice, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::audio_input, ::MayaFlux::Test::Meta::Capability::wasapi>() }))
    {
        Core::WasapiBackend backend;
        auto devices = backend.create_device_manager();
        ASSERT_NE(devices, nullptr);
        const auto input = default_info(devices->get_input_devices(), devices->get_default_input_device());
        if (!input)
            GTEST_SKIP() << "WASAPI has no default capture endpoint";

        auto config = output_config();
        config.input.enabled = true;
        config.input.channels = input->input_channels + 1;
        backend.resolve_stream_info(devices->get_default_output_device(),
            devices->get_default_input_device(), config);

        EXPECT_TRUE(config.input.enabled);
        EXPECT_EQ(config.input.channels, input->input_channels);
    }

    TEST(WasapiBackendTest, SubsystemManagerResolvesTheSameConfigAsTheBackend)
    {
        Core::WasapiBackend backend;
        auto devices = backend.create_device_manager();
        ASSERT_NE(devices, nullptr);
        if (devices->get_output_devices().empty())
            GTEST_SKIP() << "WASAPI has no active render endpoint";

        auto via_backend = output_config(1);
        via_backend.sample_rate = 44100;
        auto via_manager = via_backend;

        backend.resolve_stream_info(devices->get_default_output_device(),
            devices->get_default_input_device(), via_backend);
        Core::SubsystemManager::resolve_configs(via_manager);

        EXPECT_EQ(via_manager.output.channels, via_backend.output.channels);
        EXPECT_EQ(via_manager.sample_rate, via_backend.sample_rate);
        EXPECT_EQ(via_manager.input.enabled, via_backend.input.enabled);
        EXPECT_EQ(via_manager.input.channels, via_backend.input.channels);
    }

    MAYAFLUX_TEST_WITH(TEST, WasapiBackendTest, ResolvedConfigOpensWithoutFurtherAdjustment, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::audio_output, ::MayaFlux::Test::Meta::Capability::wasapi>() }))
    {
        Core::WasapiBackend backend;
        auto devices = backend.create_device_manager();
        ASSERT_NE(devices, nullptr);
        if (devices->get_output_devices().empty())
            GTEST_SKIP() << "WASAPI has no active render endpoint";

        auto config = output_config(1);
        config.sample_rate = 44100;
        backend.resolve_stream_info(devices->get_default_output_device(),
            devices->get_default_input_device(), config);
        const auto resolved = config;

        auto stream = backend.create_stream(devices->get_default_output_device(),
            devices->get_default_input_device(), config, nullptr);
        ASSERT_NE(stream, nullptr);
        ASSERT_NO_THROW(stream->open());

        EXPECT_EQ(config.output.channels, resolved.output.channels);
        EXPECT_EQ(config.sample_rate, resolved.sample_rate);
        stream->close();
    }

    MAYAFLUX_TEST_WITH(TEST, WasapiBackendTest, OpenAdjustsAnUnresolvedOutputConfigToTheDevice, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::audio_output, ::MayaFlux::Test::Meta::Capability::wasapi>() }))
    {
        Core::WasapiBackend backend;
        auto devices = backend.create_device_manager();
        ASSERT_NE(devices, nullptr);
        if (devices->get_output_devices().empty())
            GTEST_SKIP() << "WASAPI has no active render endpoint";

        auto expected = output_config(1);
        expected.sample_rate = 44100;
        backend.resolve_stream_info(devices->get_default_output_device(),
            devices->get_default_input_device(), expected);

        auto config = output_config(1);
        config.sample_rate = 44100;
        auto stream = backend.create_stream(devices->get_default_output_device(),
            devices->get_default_input_device(), config, nullptr);
        ASSERT_NE(stream, nullptr);
        ASSERT_NO_THROW(stream->open());

        EXPECT_EQ(config.output.channels, expected.output.channels);
        EXPECT_EQ(config.sample_rate, expected.sample_rate);
        stream->close();
    }

    MAYAFLUX_TEST_WITH(TEST, WasapiBackendTest, DropsInputInsteadOfPaddingWhenTheChannelCountDiffers, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::audio_input, ::MayaFlux::Test::Meta::Capability::audio_output, ::MayaFlux::Test::Meta::Capability::wasapi>() }))
    {
        Core::WasapiBackend backend;
        auto devices = backend.create_device_manager();
        ASSERT_NE(devices, nullptr);
        const auto input = default_info(devices->get_input_devices(), devices->get_default_input_device());
        if (devices->get_output_devices().empty() || !input)
            GTEST_SKIP() << "WASAPI full duplex requires both render and capture endpoints";

        auto config = output_config();
        config.input.enabled = true;
        config.input.channels = input->input_channels + 1;
        auto stream = backend.create_stream(devices->get_default_output_device(),
            devices->get_default_input_device(), config, nullptr);
        ASSERT_NE(stream, nullptr);

        auto output_callbacks = std::make_shared<std::atomic<uint32_t>>(0);
        auto input_callbacks = std::make_shared<std::atomic<uint32_t>>(0);
        stream->set_process_callback([&config, output_callbacks, input_callbacks](void* output, void* input, unsigned int frames) {
            if (output && frames > 0) {
                std::memset(output, 0,
                    static_cast<std::size_t>(frames) * config.output.channels * sizeof(double));
                output_callbacks->fetch_add(1, std::memory_order_release);
                if (input)
                    input_callbacks->fetch_add(1, std::memory_order_release);
            }
            return 0;
        });

        stream->open();
        stream->start();
        EXPECT_TRUE(eventually([&] { return output_callbacks->load(std::memory_order_acquire) >= 4; }));
        EXPECT_EQ(input_callbacks->load(std::memory_order_acquire), 0U);
        stream->stop();
        stream->close();
    }

    TEST(WasapiBackendTest, RepeatedBackendCreationAndDestructionOnOneThread)
    {
        for (int round = 0; round < 3; ++round) {
            Core::WasapiBackend backend;
            auto devices = backend.create_device_manager();
            ASSERT_NE(devices, nullptr);
            EXPECT_NE(backend.enumerator(), nullptr);
        }

        Core::WasapiBackend restarted;
        auto devices = restarted.create_device_manager();
        ASSERT_NE(devices, nullptr);
        if (devices->get_output_devices().empty())
            GTEST_SKIP() << "WASAPI has no active render endpoint to exercise after reinitialization";

        auto config = output_config();
        auto stream = restarted.create_stream(devices->get_default_output_device(),
            devices->get_default_input_device(), config, nullptr);
        ASSERT_NE(stream, nullptr);
        auto callback_count = std::make_shared<std::atomic<uint32_t>>(0);
        stream->set_process_callback(silence_callback(config, callback_count));

        stream->open();
        stream->start();
        EXPECT_TRUE(eventually([&] { return callback_count->load(std::memory_order_acquire) > 0; }));
        stream->stop();
        stream->close();
    }

    TEST(WasapiBackendTest, RepeatedCleanupAllowsDestructionAndReinitialization)
    {
        {
            Core::WasapiBackend backend;
            ASSERT_NE(backend.create_device_manager(), nullptr);
            EXPECT_NO_THROW(backend.cleanup());
            EXPECT_EQ(backend.enumerator(), nullptr);
            EXPECT_NO_THROW(backend.cleanup());
        }

        Core::WasapiBackend restarted;
        EXPECT_NE(restarted.enumerator(), nullptr);
        EXPECT_NE(restarted.create_device_manager(), nullptr);
    }

}
}

#endif
