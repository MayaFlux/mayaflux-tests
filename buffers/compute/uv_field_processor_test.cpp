#include "gtest/gtest.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "../../support/vulkan_test_support.hpp"

#include "MayaFlux/Buffers/Shaders/UVFieldProcessor.hpp"
#include "MayaFlux/Buffers/VKBuffer.hpp"
#include "MayaFlux/Kakshya/NDData/VertexFormats.hpp"
#include "MayaFlux/Portal/Graphics/ComputePress.hpp"
#include "MayaFlux/Portal/Graphics/SamplerForge.hpp"
#include "MayaFlux/Portal/Graphics/ShaderFoundry.hpp"
#include "MayaFlux/Portal/Graphics/TextureLoom.hpp"
#include "MayaFlux/Registry/BackendRegistry.hpp"
#include "MayaFlux/Registry/Service/BufferService.hpp"

namespace MayaFlux::Test {
namespace {

    using Buffers::UVFieldProcessor;
    using Buffers::UVProjectionMode;
    using Kakshya::MeshVertex;

    TEST(UVFieldProcessorConfigTest, DeclaresTheVertexAndSamplerBindings)
    {
        UVFieldProcessor processor;
        const auto& config = processor.get_config();

        EXPECT_EQ(processor.get_type_name(), "UVField");
        EXPECT_EQ(config.shader_path, "uv_field.comp.spv");
        EXPECT_EQ(config.push_constant_size, 48U);
        ASSERT_TRUE(config.bindings.contains("vertices"));
        ASSERT_TRUE(config.bindings.contains("tex_sampler"));
        EXPECT_EQ(config.bindings.at("vertices").binding, 0U);
        EXPECT_EQ(config.bindings.at("tex_sampler").binding, 1U);
    }

    class UVFieldProcessorDeviceTest : public VulkanDeviceTest {
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
            if (!s_foundry_ready)
                return;

            auto& press = Portal::Graphics::get_compute_press();
            press.shutdown();
            s_compute_ready = press.initialize();
            auto& loom = Portal::Graphics::get_texture_manager();
            loom.shutdown();
            s_texture_ready = loom.initialize(borrowed);
            auto& forge = Portal::Graphics::SamplerForge::instance();
            forge.shutdown();
            s_sampler_ready = forge.initialize(borrowed);
        }

        static void TearDownTestSuite()
        {
            if (s_compute_ready)
                Portal::Graphics::get_compute_press().shutdown();
            if (s_foundry_ready)
                Portal::Graphics::get_shader_foundry().shutdown();
            if (s_sampler_ready)
                Portal::Graphics::SamplerForge::instance().shutdown();
            if (s_texture_ready)
                Portal::Graphics::get_texture_manager().shutdown();
            s_foundry_ready = false;
            s_compute_ready = false;
            s_texture_ready = false;
            s_sampler_ready = false;
            VulkanDeviceTest::TearDownTestSuite();
        }

        void SetUp() override
        {
            VulkanDeviceTest::SetUp();
            if (IsSkipped())
                return;
            ASSERT_TRUE(s_foundry_ready);
            ASSERT_TRUE(s_compute_ready);
            ASSERT_TRUE(s_texture_ready);
            ASSERT_TRUE(s_sampler_ready);
            service = Registry::BackendRegistry::instance()
                          .get_service<Registry::Service::BufferService>();
            ASSERT_NE(service, nullptr);
            processor = std::make_shared<UVFieldProcessor>();
        }

        void TearDown() override
        {
            if (buffer) {
                buffer->set_default_processor(nullptr);
                if (buffer->is_initialized() && service)
                    service->destroy_buffer(buffer);
            }
            buffer.reset();
            processor.reset();
            texture.reset();
        }

        void attach(std::vector<MeshVertex> vertices)
        {
            buffer = std::make_shared<Buffers::VKBuffer>(
                vertices.size() * sizeof(MeshVertex),
                Buffers::VKBuffer::Usage::HOST_STORAGE,
                Kakshya::DataModality::VERTEX_POSITIONS_3D);
            service->initialize_buffer(buffer);
            ASSERT_TRUE(buffer->is_initialized());
            seed(vertices);
            buffer->set_default_processor(processor);
        }

        void seed(const std::vector<MeshVertex>& vertices)
        {
            std::vector<uint8_t> bytes(vertices.size() * sizeof(MeshVertex));
            std::memcpy(bytes.data(), vertices.data(), bytes.size());
            buffer->set_data({ std::move(bytes) });
        }

        MeshVertex read_vertex(size_t index = 0)
        {
            const auto data = buffer->get_data();
            const auto& bytes = std::get<std::vector<uint8_t>>(data.front());
            MeshVertex vertex;
            std::memcpy(&vertex, bytes.data() + index * sizeof(MeshVertex), sizeof(vertex));
            return vertex;
        }

        void project() { buffer->process_default(); }

        static inline bool s_foundry_ready {};
        static inline bool s_compute_ready {};
        static inline bool s_texture_ready {};
        static inline bool s_sampler_ready {};
        Registry::Service::BufferService* service {};
        std::shared_ptr<Buffers::VKBuffer> buffer;
        std::shared_ptr<UVFieldProcessor> processor;
        std::shared_ptr<Core::VKImage> texture;
    };

    TEST_F(UVFieldProcessorDeviceTest, CartesianProjectionUsesOriginAxisAndScaleWithoutChangingOtherAttributes)
    {
        MeshVertex input;
        input.position = { 2.0F, 3.0F, 4.0F };
        input.color = { 0.2F, 0.4F, 0.6F, 1.0F };
        input.normal = { 0.0F, 1.0F, 0.0F };
        attach({ input });

        processor->set_origin({ 1.0F, 1.0F, 0.0F });
        processor->set_axis({ 0.0F, 0.0F, 1.0F });
        processor->set_scale(2.0F);
        project();

        const auto vertex = read_vertex();
        EXPECT_NEAR(vertex.uv.x, -2.0F, 1e-5F);
        EXPECT_NEAR(vertex.uv.y, -4.0F, 1e-5F);
        EXPECT_EQ(vertex.position, input.position);
        EXPECT_EQ(vertex.color, input.color);
        EXPECT_EQ(vertex.normal, input.normal);
    }

    TEST_F(UVFieldProcessorDeviceTest, CylindricalProjectionUsesAxisOriginAndHeight)
    {
        MeshVertex input;
        input.position = { 1.0F, 3.0F, 0.0F };
        attach({ input });

        processor->set_projection(UVProjectionMode::CYLINDRICAL);
        processor->set_origin({ 0.0F, 1.0F, 0.0F });
        processor->set_axis({ 0.0F, 1.0F, 0.0F });
        processor->set_aux(2.0F);
        project();

        const auto vertex = read_vertex();
        EXPECT_NEAR(vertex.uv.x, 0.25F, 1e-5F);
        EXPECT_NEAR(vertex.uv.y, 1.0F, 1e-5F);
    }

    TEST_F(UVFieldProcessorDeviceTest, SphericalProjectionHandlesTheOriginAndUpdatesOnTheNextDispatch)
    {
        MeshVertex input;
        input.position = { 1.0F, 0.0F, 0.0F };
        attach({ input });

        processor->set_projection(UVProjectionMode::SPHERICAL);
        project();
        EXPECT_NEAR(read_vertex().uv.x, 0.5F, 1e-5F);
        EXPECT_NEAR(read_vertex().uv.y, 0.5F, 1e-5F);

        processor->set_origin(input.position);
        project();
        EXPECT_EQ(read_vertex().uv, glm::vec2(0.0F));
    }

    TEST_F(UVFieldProcessorDeviceTest, AxialBlendUsesScaleAndExponent)
    {
        MeshVertex input;
        input.position = { 1.0F, 2.0F, 0.0F };
        attach({ input });

        processor->set_projection(UVProjectionMode::AXIAL_BLEND);
        processor->set_aux(2.0F);
        processor->set_scale(1.0F);
        project();
        EXPECT_NEAR(read_vertex().uv.x, 1.2F, 1e-5F);
        EXPECT_NEAR(read_vertex().uv.y, 0.0F, 1e-5F);

        processor->set_scale(2.0F);
        project();
        EXPECT_NEAR(read_vertex().uv.x, 2.4F, 1e-5F);
    }

    TEST_F(UVFieldProcessorDeviceTest, ProjectionReachesVerticesBeyondTheFirstWorkgroup)
    {
        std::vector<MeshVertex> vertices(65);
        vertices.front().position = { 1.0F, 0.0F, 0.0F };
        vertices.back().position = { 0.0F, 2.0F, 0.0F };
        attach(std::move(vertices));

        project();

        EXPECT_NEAR(read_vertex(0).uv.x, -1.0F, 1e-5F);
        EXPECT_NEAR(read_vertex(64).uv.y, -2.0F, 1e-5F);
    }

    TEST_F(UVFieldProcessorDeviceTest, TextureSamplingWritesColourAndClearingRestoresUvOnlyProcessing)
    {
        MeshVertex input;
        input.position = { 1.0F, 0.0F, 0.0F };
        input.color = { 0.0F, 0.0F, 1.0F, 1.0F };
        attach({ input });

        const std::array<uint8_t, 4> red { 255U, 0U, 0U, 255U };
        texture = Portal::Graphics::get_texture_manager().create_2d(
            1U, 1U, Portal::Graphics::ImageFormat::RGBA8, red.data(), 1U);
        ASSERT_NE(texture, nullptr);

        processor->set_texture(texture);
        project();
        const auto sampled = read_vertex();
        EXPECT_NEAR(sampled.color.r, 1.0F, 1e-5F);
        EXPECT_NEAR(sampled.color.g, 0.0F, 1e-5F);
        EXPECT_NEAR(sampled.color.b, 0.0F, 1e-5F);

        const std::array<uint8_t, 4> green { 0U, 255U, 0U, 255U };
        texture = Portal::Graphics::get_texture_manager().create_2d(
            1U, 1U, Portal::Graphics::ImageFormat::RGBA8, green.data(), 1U);
        ASSERT_NE(texture, nullptr);
        processor->set_texture(texture);
        project();
        const auto replaced = read_vertex();
        EXPECT_NEAR(replaced.color.r, 0.0F, 1e-5F);
        EXPECT_NEAR(replaced.color.g, 1.0F, 1e-5F);
        EXPECT_NEAR(replaced.color.b, 0.0F, 1e-5F);

        processor->clear_texture();
        seed({ input });
        project();
        const auto uv_only = read_vertex();
        EXPECT_EQ(uv_only.color, input.color);
        EXPECT_NEAR(uv_only.uv.x, -1.0F, 1e-5F);
    }

} // namespace
} // namespace MayaFlux::Test
