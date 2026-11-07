#pragma once

#include "gtest/gtest.h"

#include "log_capture.hpp"
#include "vulkan_test_support.hpp"

#include "MayaFlux/Portal/Graphics/ComputePress.hpp"
#include "MayaFlux/Yantra/Executors/GpuResourceManager.hpp"

namespace MayaFlux::Test::GpuDispatch {

inline Portal::Graphics::ShaderFoundry& foundry()
{
    return Portal::Graphics::get_shader_foundry();
}

inline Portal::Graphics::ComputePress& press()
{
    return Portal::Graphics::get_compute_press();
}

inline Portal::Graphics::ShaderID load_compute(std::string_view source)
{
    return foundry().load_shader(std::string(source), Portal::Graphics::ShaderStage::COMPUTE);
}

inline Yantra::GpuComputeConfig compute_config(Portal::Graphics::ShaderID shader, size_t push_constant_bytes)
{
    Yantra::GpuComputeConfig config;
    config.shader_id = shader;
    config.push_constant_size = push_constant_bytes;
    return config;
}

inline Portal::Graphics::GpuBufferBinding storage_binding(uint32_t set, uint32_t binding)
{
    Portal::Graphics::GpuBufferBinding out;
    out.set = set;
    out.binding = binding;
    out.element_type = Portal::Graphics::GpuBufferBinding::ElementType::UINT32;
    return out;
}

inline std::vector<uint32_t> ascending(size_t count, uint32_t first = 1)
{
    std::vector<uint32_t> out(count);
    std::iota(out.begin(), out.end(), first);
    return out;
}

inline std::vector<uint32_t> filled(size_t count, uint32_t value)
{
    return std::vector<uint32_t>(count, value);
}

inline std::vector<uint32_t> scaled(std::vector<uint32_t> words, uint32_t factor)
{
    for (auto& word : words) {
        word *= factor;
    }
    return words;
}

inline std::vector<uint32_t> summed(const std::vector<uint32_t>& lhs, const std::vector<uint32_t>& rhs)
{
    std::vector<uint32_t> out(lhs);
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] += rhs[i];
    }
    return out;
}

inline std::vector<uint8_t> push_bytes(uint32_t value)
{
    std::vector<uint8_t> out(sizeof(value));
    std::memcpy(out.data(), &value, sizeof(value));
    return out;
}

/**
 * @brief Run retired-resource releases whose submissions have completed.
 *
 * Releases run from the command manager's collect step, which only runs when a
 * command buffer is allocated. This waits for all submitted work, then allocates
 * one command buffer so that step runs.
 */
inline void flush_retired_resources()
{
    foundry().wait_submitted();
    Yantra::GpuDispatchRecorder flush(true);
    static_cast<void>(flush.command());
    flush.wait();
}

inline void upload_words(Yantra::GpuResourceManager& manager, const std::string& key, size_t index,
    const std::vector<uint32_t>& words)
{
    manager.upload_raw(key, index, reinterpret_cast<const uint8_t*>(words.data()), words.size() * sizeof(uint32_t));
}

inline std::vector<uint32_t> download_words(Yantra::GpuResourceManager& manager, const std::string& key,
    size_t index, size_t count)
{
    std::vector<uint32_t> out(count);
    manager.download(key, index, reinterpret_cast<float*>(out.data()), count * sizeof(uint32_t));
    return out;
}

inline void upload_shared_words(Yantra::GpuResourceManager& manager, uint32_t set, size_t binding,
    const std::vector<uint32_t>& words)
{
    manager.upload_shared_raw(set, binding, reinterpret_cast<const uint8_t*>(words.data()),
        words.size() * sizeof(uint32_t));
}

inline std::vector<uint32_t> download_shared_words(Yantra::GpuResourceManager& manager, uint32_t set,
    size_t binding, size_t count)
{
    std::vector<uint32_t> out(count);
    manager.download_shared(set, binding, out.data(), count * sizeof(uint32_t));
    return out;
}

/**
 * @brief Fixture that brings up one Vulkan backend, ShaderFoundry and ComputePress.
 *
 * DedicatedComputeQueue is the hardware option: it sets enable_compute_queue on the
 * backend, so isolated work runs on a separate compute queue where the device has one
 * and on the graphics queue otherwise. Every test body is instantiated for both values
 * through a typed test suite. Validation layer messages logged during a test fail it.
 */
template <bool DedicatedComputeQueue>
class GpuDispatchFixture : public ::testing::Test {
protected:
    static void SetUpTestSuite()
    {
        if (!vulkan_unavailable_reason().empty()) {
            return;
        }
        auto config = headless_graphics_config();
        config.backend_info.enable_compute_queue = DedicatedComputeQueue;
        auto backend = std::make_shared<Core::VulkanBackend>();
        if (backend->initialize(config)) {
            s_backend = std::move(backend);
        }
    }

    static void TearDownTestSuite()
    {
        s_backend.reset();
    }

    void SetUp() override
    {
        if (!s_backend) {
            GTEST_SKIP() << "No usable Vulkan device: " << vulkan_unavailable_reason();
        }
        LogCapture::install();
        LogCapture::clear();
        teardown_portal();
        ASSERT_TRUE(foundry().initialize(s_backend));
        ASSERT_TRUE(press().initialize());
    }

    void TearDown() override
    {
        teardown_portal();
        for (const auto& message : LogCapture::messages()) {
            EXPECT_EQ(message.find("Validation layer"), std::string::npos) << message;
        }
    }

    static void teardown_portal()
    {
        press().shutdown();
        foundry().shutdown();
    }

    static const std::shared_ptr<Core::VulkanBackend>& backend()
    {
        return s_backend;
    }

private:
    static inline std::shared_ptr<Core::VulkanBackend> s_backend;
};

} // namespace MayaFlux::Test::GpuDispatch
