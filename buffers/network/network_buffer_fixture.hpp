#pragma once

#include "gtest/gtest.h"


#include "../../support/scripted_window.hpp"
#include "../../support/vulkan_test_support.hpp"

#include "MayaFlux/Buffers/BufferProcessingChain.hpp"
#include "MayaFlux/Buffers/Network/NetworkGeometryBuffer.hpp"
#include "MayaFlux/Buffers/Shaders/RenderProcessor.hpp"
#include "MayaFlux/Buffers/Staging/StagingUtils.hpp"
#include "MayaFlux/Nodes/Network/Operators/PhysicsOperator.hpp"
#include "MayaFlux/Nodes/Network/ParticleNetwork.hpp"
#include "MayaFlux/Portal/Graphics/ComputePress.hpp"
#include "MayaFlux/Portal/Graphics/ShaderFoundry.hpp"
#include "MayaFlux/Registry/BackendRegistry.hpp"
#include "MayaFlux/Registry/Service/BufferService.hpp"

namespace MayaFlux::Test {

/**
 * @brief Device fixture shared by the NetworkGeometryBuffer test files.
 *
 * Brings up ShaderFoundry and ComputePress once per suite, owns the buffer
 * under test and tears it down in the order the render and compute
 * processors need. Readback goes through the same staging helpers the
 * library uses, so a test never touches Vulkan directly.
 */
class NetworkBufferDeviceTest : public VulkanDeviceTest {
protected:
    static void SetUpTestSuite()
    {
        VulkanDeviceTest::SetUpTestSuite();
        if (!vulkan_unavailable_reason().empty())
            return;

        auto borrowed = std::shared_ptr<Core::VulkanBackend>(&backend(), [](Core::VulkanBackend*) { });
        auto& foundry = Portal::Graphics::get_shader_foundry();
        foundry.shutdown();
        s_foundry_ready = foundry.initialize(borrowed);
        if (s_foundry_ready) {
            Portal::Graphics::get_compute_press().shutdown();
            s_compute_ready = Portal::Graphics::get_compute_press().initialize();
        }
    }

    static void TearDownTestSuite()
    {
        if (s_compute_ready)
            Portal::Graphics::get_compute_press().shutdown();
        if (s_foundry_ready)
            Portal::Graphics::get_shader_foundry().shutdown();
        s_compute_ready = false;
        s_foundry_ready = false;
        VulkanDeviceTest::TearDownTestSuite();
    }

    void SetUp() override
    {
        VulkanDeviceTest::SetUp();
        if (IsSkipped())
            return;
        ASSERT_TRUE(s_foundry_ready);
        ASSERT_TRUE(s_compute_ready);

        service = Registry::BackendRegistry::instance()
                      .get_service<Registry::Service::BufferService>();
        ASSERT_NE(service, nullptr);
    }

    void TearDown() override
    {
        if (buffer) {
            if (auto chain = buffer->get_processing_chain()) {
                if (auto render = buffer->get_render_processor())
                    chain->remove_processor_anywhere(render, buffer);
            }
            buffer->set_default_processor(nullptr);
            if (buffer->is_initialized() && service)
                service->destroy_buffer(buffer);
        }
        buffer.reset();
        network.reset();
        window.reset();
    }

    static std::shared_ptr<Nodes::Network::ParticleNetwork> make_particles(
        size_t count,
        glm::vec3 bounds_min = glm::vec3(-1.0F),
        glm::vec3 bounds_max = glm::vec3(1.0F))
    {
        return std::make_shared<Nodes::Network::ParticleNetwork>(
            count, bounds_min, bounds_max, Kinesis::SpatialDistribution::RANDOM_VOLUME);
    }

    void attach(
        std::shared_ptr<Nodes::Network::NodeNetwork> value,
        float over_allocate_factor = 2.0F)
    {
        network = std::move(value);
        buffer = std::make_shared<Buffers::NetworkGeometryBuffer>(
            network, "network_geometry", over_allocate_factor);
        service->initialize_buffer(buffer);
        ASSERT_TRUE(buffer->is_initialized());
        buffer->setup_processors(Buffers::ProcessingToken::GRAPHICS_BACKEND);
    }

    void render(Portal::Graphics::RenderConfig config = {})
    {
        window = std::make_shared<ScriptedWindow>(320, 240);
        config.target_window = window;
        buffer->setup_rendering(config);
    }

    std::vector<uint8_t> read_vertices(size_t byte_count)
    {
        std::vector<uint8_t> bytes(buffer->get_size_bytes());
        Buffers::download_from_gpu(buffer, bytes.data(), bytes.size());
        bytes.resize(byte_count);
        return bytes;
    }

    template <typename T>
    std::vector<T> read_state(const std::string& name)
    {
        std::vector<T> values(buffer->get_state_bytes(name) / sizeof(T));
        std::shared_ptr<Buffers::VKBuffer> staging;
        Buffers::download_back_buffer(
            buffer->read_state_slot(name), values.data(), values.size() * sizeof(T), staging);
        return values;
    }

    template <typename T>
    void write_state(const std::string& name, const std::vector<T>& values)
    {
        std::shared_ptr<Buffers::VKBuffer> staging;
        Buffers::upload_back_buffer(
            buffer->write_state_slot(name), values.data(), values.size() * sizeof(T), staging);
    }

    static inline bool s_foundry_ready {};
    static inline bool s_compute_ready {};
    Registry::Service::BufferService* service {};
    std::shared_ptr<Nodes::Network::NodeNetwork> network;
    std::shared_ptr<Buffers::NetworkGeometryBuffer> buffer;
    std::shared_ptr<ScriptedWindow> window;
};

} // namespace MayaFlux::Test
