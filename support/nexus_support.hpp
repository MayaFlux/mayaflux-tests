#pragma once

#include "gtest/gtest.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

#include "scripted_window.hpp"
#include "vulkan_test_support.hpp"

#include "MayaFlux/Buffers/BufferManager.hpp"
#include "MayaFlux/Buffers/BufferProcessingChain.hpp"
#include "MayaFlux/Buffers/Shaders/RenderProcessor.hpp"
#include "MayaFlux/Buffers/VKBuffer.hpp"
#include "MayaFlux/Kakshya/NDData/MeshData.hpp"
#include "MayaFlux/Kakshya/NDData/VertexFormats.hpp"
#include "MayaFlux/Nexus/Pheme/Survey.hpp"
#include "MayaFlux/Portal/Graphics/GraphicsUtils.hpp"
#include "MayaFlux/Portal/Graphics/ShaderFoundry.hpp"

namespace MayaFlux::Test {

constexpr float k_tolerance = 1e-4F;

inline void expect_near(const glm::vec3& actual, const glm::vec3& expected)
{
    EXPECT_NEAR(actual.x, expected.x, k_tolerance);
    EXPECT_NEAR(actual.y, expected.y, k_tolerance);
    EXPECT_NEAR(actual.z, expected.z, k_tolerance);
}

inline Kakshya::MeshData make_mesh(std::initializer_list<glm::vec3> positions, std::vector<uint32_t> indices)
{
    std::vector<Kakshya::MeshVertex> vertices;
    for (const auto& position : positions) {
        vertices.push_back({ .position = position });
    }

    auto data = Kakshya::MeshData::empty();
    std::vector<uint8_t> bytes(vertices.size() * sizeof(Kakshya::MeshVertex));
    std::memcpy(bytes.data(), vertices.data(), bytes.size());
    data.vertex_variant = std::move(bytes);
    data.index_variant = std::move(indices);
    data.layout.vertex_count = static_cast<uint32_t>(vertices.size());
    return data;
}

inline Kakshya::MeshData triangle_mesh()
{
    return make_mesh({ { 0.0F, 0.0F, 0.0F }, { 4.0F, 0.0F, 0.0F }, { 0.0F, 4.0F, 0.0F } }, { 0, 1, 2 });
}

inline Kakshya::MeshData two_triangle_mesh()
{
    return make_mesh(
        { { 0.0F, 0.0F, 0.0F }, { 4.0F, 0.0F, 0.0F }, { 0.0F, 4.0F, 0.0F },
            { 10.0F, 0.0F, 0.0F }, { 11.0F, 0.0F, 0.0F }, { 10.0F, 1.0F, 0.0F } },
        { 0, 1, 2, 3, 4, 5 });
}

inline Kakshya::MeshData solid_mesh()
{
    return make_mesh(
        { { 0.0F, 0.0F, 0.0F }, { 4.0F, 0.0F, 0.0F }, { 0.0F, 4.0F, 0.0F }, { 0.0F, 0.0F, 4.0F } },
        { 0, 1, 2, 0, 1, 3, 0, 2, 3, 1, 2, 3 });
}

template <typename Buffer>
bool contains(const std::vector<std::shared_ptr<Buffer>>& buffers, const std::shared_ptr<Buffer>& buffer)
{
    return std::ranges::find(buffers, buffer) != buffers.end();
}

inline glm::vec3 placed(const std::shared_ptr<Buffers::VKBuffer>& buffer, const glm::vec3& point)
{
    return glm::vec3(Nexus::read_placement(buffer) * glm::vec4(point, 1.0F));
}

/**
 * @brief A Vulkan device fixture for Nexus tests that build a RenderProcessor.
 *
 * Brings up ShaderFoundry once per suite and owns a BufferManager and a
 * ScriptedWindow. TearDown removes every graphics buffer left in the manager.
 */
class NexusDeviceTest : public VulkanDeviceTest {
protected:
    static void SetUpTestSuite()
    {
        VulkanDeviceTest::SetUpTestSuite();
        if (!vulkan_unavailable_reason().empty())
            return;

        auto borrowed = std::shared_ptr<Core::VulkanBackend>(&backend(), [](Core::VulkanBackend*) { });
        Portal::Graphics::get_shader_foundry().shutdown();
        s_foundry_ready = Portal::Graphics::get_shader_foundry().initialize(borrowed);
    }

    static void TearDownTestSuite()
    {
        if (s_foundry_ready)
            Portal::Graphics::get_shader_foundry().shutdown();
        s_foundry_ready = false;
        VulkanDeviceTest::TearDownTestSuite();
    }

    void SetUp() override
    {
        VulkanDeviceTest::SetUp();
        if (IsSkipped())
            return;
        ASSERT_TRUE(s_foundry_ready);
    }

    void TearDown() override
    {
        const auto buffers = manager.get_graphics_buffers(Buffers::ProcessingToken::GRAPHICS_BACKEND);
        for (const auto& buffer : buffers) {
            manager.remove_buffer(buffer, Buffers::ProcessingToken::GRAPHICS_BACKEND);
            if (auto render = buffer->get_render_processor(); render && buffer->get_processing_chain())
                buffer->get_processing_chain()->remove_processor_anywhere(render, buffer);
            buffer->set_default_processor(nullptr);
        }
    }

    [[nodiscard]] Portal::Graphics::RenderConfig render_config() const
    {
        Portal::Graphics::RenderConfig config;
        config.target_window = window;
        return config;
    }

    [[nodiscard]] bool registered(const std::shared_ptr<Buffers::VKBuffer>& buffer) const
    {
        return contains(manager.get_graphics_buffers(Buffers::ProcessingToken::GRAPHICS_BACKEND), buffer);
    }

    Buffers::BufferManager manager;
    std::shared_ptr<ScriptedWindow> window = std::make_shared<ScriptedWindow>(400U, 300U);

private:
    static inline bool s_foundry_ready {};
};

}
