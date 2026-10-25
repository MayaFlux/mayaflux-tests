#include "gtest/gtest.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <utility>
#include <vector>

#include "../../support/native_window_test_support.hpp"
#include "../../support/scripted_window.hpp"
#include "../../support/vulkan_test_support.hpp"

#include "MayaFlux/Buffers/BufferProcessingChain.hpp"
#include "MayaFlux/Buffers/Geometry/ComputeMeshBuffer.hpp"
#include "MayaFlux/Buffers/Root/RootGraphicsBuffer.hpp"
#include "MayaFlux/Buffers/Shaders/RenderProcessor.hpp"
#include "MayaFlux/Buffers/Shaders/SDFFieldProcessor.hpp"
#include "MayaFlux/Buffers/Shaders/SDFMeshProcessor.hpp"
#include "MayaFlux/Buffers/Shaders/SDFPrepProcessor.hpp"
#include "MayaFlux/Buffers/Staging/StagingUtils.hpp"
#include "MayaFlux/Kinesis/Tendency/ShaderCompat.hpp"
#include "MayaFlux/Portal/Graphics/ComputePress.hpp"
#include "MayaFlux/Portal/Graphics/RenderFlow.hpp"
#include "MayaFlux/Portal/Graphics/ShaderFoundry.hpp"
#include "MayaFlux/Registry/BackendRegistry.hpp"
#include "MayaFlux/Registry/Service/BufferService.hpp"
#include "MayaFlux/Registry/Service/DisplayService.hpp"

namespace MayaFlux::Test {
namespace {

    namespace fields {

        using namespace MayaFlux::ShaderCompat;

        const auto plane = MF_FIELD(plane, [](vec3 p) -> float {
            return p.x - 0.25f;
        });

        const auto moving_plane = MF_FIELD_P(moving_plane, [](vec3 p, float t, vec4 params) -> float {
            return p.x - params.x + t;
        });

        const auto time_plane = MF_FIELD_T(time_plane, [](vec3 p, float t) -> float {
            return p.x - t;
        });

        const auto sphere = MF_FIELD(sphere, [](vec3 p) -> float {
            return length(p) - 0.65f;
        });

    }

    using Buffers::ComputeMeshBuffer;
    using Buffers::ProcessingToken;

    constexpr uint32_t k_resolution = 4;
    constexpr uint32_t k_plane_vertices = 6 * k_resolution * k_resolution;

    ComputeMeshBuffer::Config config_for(ComputeMeshBuffer::FieldSource source)
    {
        return {
            .source = std::move(source),
            .bounds_min = glm::vec3(-1.0F),
            .bounds_max = glm::vec3(1.0F),
            .resolution_x = k_resolution,
            .resolution_y = k_resolution,
            .resolution_z = k_resolution,
        };
    }

    TEST(ComputeMeshBufferSizingTest, CapacityUsesTheMeshVertexStride)
    {
        ComputeMeshBuffer buffer(config_for(fields::plane.cpu));

        EXPECT_EQ(buffer.get_size_bytes(),
            static_cast<size_t>(k_resolution) * k_resolution * k_resolution
                * 15U * sizeof(Kakshya::MeshVertex));
        EXPECT_FALSE(buffer.get_live_vertex_count().has_value());
        EXPECT_EQ(buffer.get_field_processor(), nullptr);
    }

    class ComputeMeshBufferDeviceTest : public VulkanDeviceTest {
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
                auto& press = Portal::Graphics::get_compute_press();
                press.shutdown();
                s_compute_ready = press.initialize();
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
            for (auto& owned : buffers) {
                if (auto render = owned->get_render_processor()) {
                    if (auto chain = owned->get_processing_chain())
                        chain->remove_processor_anywhere(render, owned);
                }
                owned->set_default_processor(nullptr);
                if (owned->is_initialized() && service)
                    service->destroy_buffer(owned);
            }
            buffers.clear();
            buffer.reset();
            windows.clear();
        }

        void attach(ComputeMeshBuffer::FieldSource source,
            Portal::Graphics::RenderConfig config = {})
        {
            buffer = std::make_shared<ComputeMeshBuffer>(config_for(std::move(source)));
            service->initialize_buffer(buffer);
            ASSERT_TRUE(buffer->is_initialized());
            buffers.push_back(buffer);
            buffer->setup_processors(ProcessingToken::GRAPHICS_BACKEND);
            ASSERT_NE(buffer->get_mesh_processor(), nullptr);

            window = std::make_shared<ScriptedWindow>(320, 240);
            windows.push_back(window);
            config.target_window = window;
            buffer->setup_rendering(config);
            ASSERT_NE(buffer->get_render_processor(), nullptr);
        }

        void run_field()
        {
            buffer->process_default();
            buffer->get_processing_chain()->preprocess(buffer);
        }

        void run_mesh()
        {
            run_mesh_for(buffer);
        }

        void run_mesh_for(const std::shared_ptr<ComputeMeshBuffer>& target)
        {
            target->process_default();
            if (target->get_field_processor())
                target->get_processing_chain()->preprocess(target);
            target->get_processing_chain()->process(target);
        }

        std::vector<Kakshya::MeshVertex> read_vertices(
            const std::shared_ptr<ComputeMeshBuffer>& target, uint32_t count)
        {
            std::vector<uint8_t> bytes(target->get_size_bytes());
            Buffers::download_from_gpu(target, bytes.data(), bytes.size());
            std::vector<Kakshya::MeshVertex> vertices(count);
            std::memcpy(vertices.data(), bytes.data(), vertices.size() * sizeof(vertices.front()));
            return vertices;
        }

        std::vector<Kakshya::MeshVertex> read_vertices(uint32_t count)
        {
            return read_vertices(buffer, count);
        }

        static inline bool s_foundry_ready {};
        static inline bool s_compute_ready {};
        Registry::Service::BufferService* service {};
        std::shared_ptr<ComputeMeshBuffer> buffer;
        std::shared_ptr<ScriptedWindow> window;
        std::vector<std::shared_ptr<ComputeMeshBuffer>> buffers;
        std::vector<std::shared_ptr<ScriptedWindow>> windows;
    };

    TEST_F(ComputeMeshBufferDeviceTest, CpuFieldEmitsPlaneAndReplacesItWhenTheFieldChanges)
    {
        attach(fields::plane.cpu);
        ASSERT_EQ(buffer->get_field_processor(), nullptr);

        run_mesh();
        ASSERT_EQ(buffer->get_live_vertex_count(), k_plane_vertices);
        for (const auto& vertex : read_vertices(k_plane_vertices)) {
            EXPECT_NEAR(vertex.position.x, 0.25F, 1e-5F);
            EXPECT_TRUE(std::isfinite(vertex.normal.x));
        }

        run_mesh();
        EXPECT_EQ(buffer->get_live_vertex_count(), k_plane_vertices);

        buffer->set_field(Kinesis::SpatialField { .fn = [](const glm::vec3&) { return 5.0F; } });
        run_mesh();
        EXPECT_EQ(buffer->get_live_vertex_count(), 0U);

        buffer->set_field(fields::plane.cpu);
        run_mesh();
        EXPECT_EQ(buffer->get_live_vertex_count(), k_plane_vertices);
    }

    TEST_F(ComputeMeshBufferDeviceTest, CpuModeWiresOnlyTheMeshStage)
    {
        attach(fields::plane.cpu);

        const auto chain = buffer->get_processing_chain();
        ASSERT_NE(chain, nullptr);
        EXPECT_EQ(buffer->get_default_processor(), buffer->get_mesh_processor());
        EXPECT_EQ(buffer->get_field_processor(), nullptr);
        EXPECT_EQ(chain->get_preprocessor(buffer), nullptr);
        EXPECT_TRUE(chain->get_processors(buffer).empty());
        EXPECT_EQ(chain->get_final_processor(buffer), buffer->get_render_processor());

        const auto layout = buffer->get_vertex_layout();
        ASSERT_TRUE(layout.has_value());
        EXPECT_EQ(layout->stride_bytes, sizeof(Kakshya::MeshVertex));
        EXPECT_EQ(layout->vertex_count, k_resolution * k_resolution * k_resolution * 15U);
    }

    TEST_F(ComputeMeshBufferDeviceTest, GpuModeWiresPrepFieldMeshAndRenderInOrder)
    {
        attach(fields::plane);

        const auto chain = buffer->get_processing_chain();
        ASSERT_NE(chain, nullptr);
        EXPECT_NE(std::dynamic_pointer_cast<Buffers::SDFPrepProcessor>(buffer->get_default_processor()), nullptr);
        EXPECT_EQ(chain->get_preprocessor(buffer), buffer->get_field_processor());
        ASSERT_EQ(chain->get_processors(buffer).size(), 1U);
        EXPECT_EQ(chain->get_processors(buffer).front(), buffer->get_mesh_processor());
        EXPECT_EQ(chain->get_final_processor(buffer), buffer->get_render_processor());

        auto prep = std::dynamic_pointer_cast<Buffers::SDFPrepProcessor>(buffer->get_default_processor());
        ASSERT_NE(prep, nullptr);
        EXPECT_EQ(buffer->get_mesh_processor()->counter_buf(), prep->counter_buf());
        EXPECT_EQ(prep->grid_buf()->get_size_bytes(),
            static_cast<size_t>(k_resolution + 1) * (k_resolution + 1)
                * (k_resolution + 1) * sizeof(float));
    }

    TEST_F(ComputeMeshBufferDeviceTest, DefaultRenderingUsesTheTriangleVertexShader)
    {
        attach(fields::plane.cpu);

        const auto render = buffer->get_render_processor();
        ASSERT_NE(render, nullptr);
        EXPECT_EQ(render->get_config().shader_path, "triangle.vert.spv");
    }

    TEST_F(ComputeMeshBufferDeviceTest, RequestedTextureBindingRegistersTheSamplerSlot)
    {
        Portal::Graphics::RenderConfig config;
        config.default_texture_binding = "albedoMap";
        attach(fields::plane.cpu, config);

        const auto render = buffer->get_render_processor();
        ASSERT_NE(render, nullptr);
        const auto& bindings = render->get_config().bindings;
        const auto sampler = bindings.find("albedoMap");
        ASSERT_NE(sampler, bindings.end());
        EXPECT_EQ(sampler->second.set, 0U);
        EXPECT_EQ(sampler->second.binding, 1U);
        EXPECT_EQ(sampler->second.type, vk::DescriptorType::eCombinedImageSampler);
        EXPECT_FALSE(buffer->has_diffuse_texture());
    }

    TEST_F(ComputeMeshBufferDeviceTest, DirtyStateChangesOnlyWhenTheMeshIsInvalidatedOrDispatched)
    {
        attach(fields::plane.cpu);
        auto mesh = buffer->get_mesh_processor();
        ASSERT_TRUE(mesh->is_dirty());

        run_mesh();
        EXPECT_FALSE(mesh->is_dirty());

        buffer->set_dirty();
        EXPECT_TRUE(mesh->is_dirty());
        run_mesh();
        EXPECT_FALSE(mesh->is_dirty());

        buffer->set_iso_level(0.5F);
        EXPECT_TRUE(mesh->is_dirty());
    }

    TEST_F(ComputeMeshBufferDeviceTest, CpuFieldRebuildsWhenBoundsMoveAwayAndBack)
    {
        attach(fields::plane.cpu);

        run_mesh();
        ASSERT_EQ(buffer->get_live_vertex_count(), k_plane_vertices);

        buffer->set_bounds(glm::vec3(1.0F), glm::vec3(3.0F));
        run_mesh();
        EXPECT_EQ(buffer->get_live_vertex_count(), 0U);

        buffer->set_bounds(glm::vec3(-1.0F), glm::vec3(1.0F));
        run_mesh();
        EXPECT_EQ(buffer->get_live_vertex_count(), k_plane_vertices);
    }

    TEST_F(ComputeMeshBufferDeviceTest, CpuFieldUsesTheCurrentIsoLevel)
    {
        attach(fields::plane.cpu);

        buffer->set_iso_level(0.5F);
        run_mesh();
        ASSERT_EQ(buffer->get_live_vertex_count(), k_plane_vertices);
        for (const auto& vertex : read_vertices(k_plane_vertices))
            EXPECT_NEAR(vertex.position.x, 0.75F, 1e-5F);

        buffer->set_iso_level(-5.0F);
        run_mesh();
        EXPECT_EQ(buffer->get_live_vertex_count(), 0U);
    }

    TEST_F(ComputeMeshBufferDeviceTest, PrepClearsBothOwnedBuffersBeforeGpuEvaluation)
    {
        attach(fields::plane);
        auto prep = std::dynamic_pointer_cast<Buffers::SDFPrepProcessor>(buffer->get_default_processor());
        ASSERT_NE(prep, nullptr);

        auto* grid = static_cast<float*>(prep->grid_buf()->get_mapped_ptr());
        auto* counter = static_cast<uint32_t*>(prep->counter_buf()->get_mapped_ptr());
        ASSERT_NE(grid, nullptr);
        ASSERT_NE(counter, nullptr);

        constexpr size_t corners = static_cast<size_t>(k_resolution + 1)
            * (k_resolution + 1) * (k_resolution + 1);
        std::fill_n(grid, corners, 17.0F);
        *counter = 93U;

        buffer->process_default();

        EXPECT_TRUE(std::all_of(grid, grid + corners, [](float value) { return value == 0.0F; }));
        EXPECT_EQ(*counter, 0U);
    }

    TEST_F(ComputeMeshBufferDeviceTest, PrepClearsThePreviousGpuDispatchResult)
    {
        attach(fields::plane);
        run_mesh();
        ASSERT_EQ(buffer->get_live_vertex_count(), k_plane_vertices);

        buffer->process_default();
        EXPECT_EQ(buffer->get_live_vertex_count(), 0U);

        auto prep = std::dynamic_pointer_cast<Buffers::SDFPrepProcessor>(buffer->get_default_processor());
        ASSERT_NE(prep, nullptr);
        const auto* grid = static_cast<const float*>(prep->grid_buf()->get_mapped_ptr());
        ASSERT_NE(grid, nullptr);
        constexpr size_t corners = static_cast<size_t>(k_resolution + 1)
            * (k_resolution + 1) * (k_resolution + 1);
        EXPECT_TRUE(std::all_of(grid, grid + corners, [](float value) { return value == 0.0F; }));
    }

    TEST_F(ComputeMeshBufferDeviceTest, PrepResizesItsGridAndCounterTogether)
    {
        attach(fields::plane);
        auto prep = std::dynamic_pointer_cast<Buffers::SDFPrepProcessor>(buffer->get_default_processor());
        ASSERT_NE(prep, nullptr);
        const auto original_grid = prep->grid_buf();
        const auto original_counter = prep->counter_buf();

        prep->set_resolution(2U, 3U, 1U);

        ASSERT_NE(prep->grid_buf(), nullptr);
        ASSERT_NE(prep->counter_buf(), nullptr);
        EXPECT_NE(prep->grid_buf(), original_grid);
        EXPECT_NE(prep->counter_buf(), original_counter);
        EXPECT_EQ(prep->grid_buf()->get_size_bytes(), 3U * 4U * 2U * sizeof(float));
        EXPECT_EQ(prep->counter_buf()->get_size_bytes(), sizeof(uint32_t));

        auto* grid = static_cast<float*>(prep->grid_buf()->get_mapped_ptr());
        auto* counter = static_cast<uint32_t*>(prep->counter_buf()->get_mapped_ptr());
        ASSERT_NE(grid, nullptr);
        ASSERT_NE(counter, nullptr);
        std::fill_n(grid, 24U, 1.0F);
        *counter = 7U;
        buffer->process_default();
        EXPECT_TRUE(std::all_of(grid, grid + 24U, [](float value) { return value == 0.0F; }));
        EXPECT_EQ(*counter, 0U);
    }

    TEST_F(ComputeMeshBufferDeviceTest, GeneratedFieldWritesEveryCornerAndTracksBounds)
    {
        attach(fields::plane);
        auto prep = std::dynamic_pointer_cast<Buffers::SDFPrepProcessor>(buffer->get_default_processor());
        ASSERT_NE(prep, nullptr);
        ASSERT_NE(buffer->get_field_processor(), nullptr);

        run_field();
        const auto* grid = static_cast<const float*>(prep->grid_buf()->get_mapped_ptr());
        ASSERT_NE(grid, nullptr);
        for (uint32_t z = 0; z <= k_resolution; ++z) {
            for (uint32_t y = 0; y <= k_resolution; ++y) {
                for (uint32_t x = 0; x <= k_resolution; ++x) {
                    const size_t index = x + (k_resolution + 1) * (y + (k_resolution + 1) * z);
                    EXPECT_NEAR(grid[index], -1.25F + 0.5F * static_cast<float>(x), 1e-5F)
                        << "corner " << x << ", " << y << ", " << z;
                }
            }
        }

        buffer->set_bounds(glm::vec3(-2.0F), glm::vec3(2.0F));
        run_field();
        EXPECT_NEAR(grid[0], -2.25F, 1e-5F);
        EXPECT_NEAR(grid[k_resolution], 1.75F, 1e-5F);
    }

    TEST_F(ComputeMeshBufferDeviceTest, ParametricFieldUsesTheCurrentTimeAndParameterBlock)
    {
        attach(fields::moving_plane);
        auto prep = std::dynamic_pointer_cast<Buffers::SDFPrepProcessor>(buffer->get_default_processor());
        ASSERT_NE(prep, nullptr);
        auto field = buffer->get_field_processor();
        ASSERT_NE(field, nullptr);
        const auto* grid = static_cast<const float*>(prep->grid_buf()->get_mapped_ptr());
        ASSERT_NE(grid, nullptr);

        field->set_params(glm::vec4(0.25F, 0.0F, 0.0F, 0.0F));
        field->set_time(0.5F);
        run_field();
        EXPECT_NEAR(grid[0], -0.75F, 1e-5F);
        EXPECT_NEAR(grid[k_resolution], 1.25F, 1e-5F);

        field->set_time(-0.5F);
        run_field();
        EXPECT_NEAR(grid[0], -1.75F, 1e-5F);
    }

    TEST_F(ComputeMeshBufferDeviceTest, TemporalFieldReadsUpdatedTimeAcrossDispatches)
    {
        attach(fields::time_plane);
        auto prep = std::dynamic_pointer_cast<Buffers::SDFPrepProcessor>(buffer->get_default_processor());
        ASSERT_NE(prep, nullptr);
        auto field = buffer->get_field_processor();
        ASSERT_NE(field, nullptr);
        const auto* grid = static_cast<const float*>(prep->grid_buf()->get_mapped_ptr());
        ASSERT_NE(grid, nullptr);

        field->set_time(0.25F);
        run_field();
        EXPECT_NEAR(grid[0], -1.25F, 1e-5F);
        EXPECT_NEAR(grid[k_resolution], 0.75F, 1e-5F);
        ASSERT_TRUE(field->is_pipeline_ready());

        field->set_time(-0.25F);
        run_field();
        EXPECT_NEAR(grid[0], -0.75F, 1e-5F);
        EXPECT_NEAR(grid[k_resolution], 1.25F, 1e-5F);
        EXPECT_TRUE(field->is_pipeline_ready());
    }

    TEST_F(ComputeMeshBufferDeviceTest, GpuFieldMeshingTracksIsoLevelAcrossDispatches)
    {
        attach(fields::plane);

        run_mesh();
        ASSERT_EQ(buffer->get_live_vertex_count(), k_plane_vertices);
        for (const auto& vertex : read_vertices(k_plane_vertices))
            EXPECT_NEAR(vertex.position.x, 0.25F, 1e-5F);

        buffer->set_iso_level(5.0F);
        run_mesh();
        EXPECT_EQ(buffer->get_live_vertex_count(), 0U);

        buffer->set_iso_level(0.0F);
        run_mesh();
        EXPECT_EQ(buffer->get_live_vertex_count(), k_plane_vertices);
    }

    TEST_F(ComputeMeshBufferDeviceTest, GpuMeshVerticesKeepTheirPackedAttributes)
    {
        attach(fields::plane);
        run_mesh();
        ASSERT_EQ(buffer->get_live_vertex_count(), k_plane_vertices);

        for (const auto& vertex : read_vertices(k_plane_vertices)) {
            EXPECT_NEAR(vertex.position.x, 0.25F, 1e-5F);
            EXPECT_GE(vertex.position.y, -1.0F);
            EXPECT_LE(vertex.position.y, 1.0F);
            EXPECT_GE(vertex.position.z, -1.0F);
            EXPECT_LE(vertex.position.z, 1.0F);
            EXPECT_NEAR(vertex.color.a, 1.0F, 1e-5F);
            EXPECT_NEAR(vertex.uv.x, 0.625F, 1e-5F);
            EXPECT_NEAR(vertex.normal.x, 1.0F, 1e-3F);
            EXPECT_NEAR(vertex.tangent.w, 1.0F, 1e-5F);
        }
    }

    TEST_F(ComputeMeshBufferDeviceTest, AxisPaletteIsWrittenIntoTheGeneratedVertices)
    {
        attach(fields::plane);
        auto mesh = buffer->get_mesh_processor();
        const glm::vec3 x { 0.2F, 0.4F, 0.6F };
        mesh->set_axis_palette(x, { 0.8F, 0.1F, 0.1F }, { 0.1F, 0.8F, 0.1F });

        run_mesh();
        ASSERT_EQ(buffer->get_live_vertex_count(), k_plane_vertices);
        for (const auto& vertex : read_vertices(k_plane_vertices)) {
            EXPECT_NEAR(vertex.color.r, x.r, 1e-5F);
            EXPECT_NEAR(vertex.color.g, x.g, 1e-5F);
            EXPECT_NEAR(vertex.color.b, x.b, 1e-5F);
        }
    }

    TEST_F(ComputeMeshBufferDeviceTest, DeferredMeshDispatchRetainsItsBindingsUntilCompletion)
    {
        attach(fields::plane);
        auto mesh = buffer->get_mesh_processor();
        mesh->set_deferred_submission(true);

        run_mesh();
        ASSERT_TRUE(mesh->is_dispatch_pending());
        EXPECT_TRUE(mesh->resolve_pending_dispatch(true));
        EXPECT_FALSE(mesh->is_dispatch_pending());
        EXPECT_EQ(buffer->get_live_vertex_count(), k_plane_vertices);
        mesh->set_deferred_submission(false);
    }

    TEST_F(ComputeMeshBufferDeviceTest, InterleavedBuffersKeepTheirOwnFieldAndMeshBindings)
    {
        attach(fields::plane);
        auto first = buffer;
        run_mesh();
        ASSERT_EQ(first->get_live_vertex_count(), k_plane_vertices);

        attach(fields::plane);
        auto second = buffer;
        second->set_iso_level(0.5F);
        run_mesh();
        ASSERT_EQ(second->get_live_vertex_count(), k_plane_vertices);

        first->set_dirty();
        run_mesh_for(first);
        ASSERT_EQ(first->get_live_vertex_count(), k_plane_vertices);
        for (const auto& vertex : read_vertices(first, k_plane_vertices))
            EXPECT_NEAR(vertex.position.x, 0.25F, 1e-5F);
        for (const auto& vertex : read_vertices(second, k_plane_vertices))
            EXPECT_NEAR(vertex.position.x, 0.75F, 1e-5F);
    }

    TEST_F(ComputeMeshBufferDeviceTest, CpuAndGpuSphereFieldsProduceTheSameTriangleCount)
    {
        attach(fields::sphere.cpu);
        auto cpu = buffer;
        run_mesh();
        const auto cpu_count = cpu->get_live_vertex_count();
        ASSERT_TRUE(cpu_count.has_value());
        ASSERT_GT(*cpu_count, 0U);
        ASSERT_EQ(*cpu_count % 3U, 0U);

        attach(fields::sphere);
        auto gpu = buffer;
        run_mesh();
        const auto gpu_count = gpu->get_live_vertex_count();
        ASSERT_TRUE(gpu_count.has_value());
        EXPECT_EQ(*gpu_count, *cpu_count);

        for (const auto& vertex : read_vertices(gpu, *gpu_count)) {
            const float radius = glm::length(vertex.position);
            EXPECT_TRUE(std::isfinite(radius));
            EXPECT_NEAR(radius, 0.65F, 0.15F);
        }
    }

    TEST_F(ComputeMeshBufferDeviceTest, CpuResolutionChangeRebindsTheMeshGridAndCounter)
    {
        attach(fields::plane.cpu);
        run_mesh();
        ASSERT_EQ(buffer->get_live_vertex_count(), k_plane_vertices);

        buffer->set_resolution(2U, 2U, 2U);
        run_mesh();

        ASSERT_EQ(buffer->get_live_vertex_count(), 24U);
        for (const auto& vertex : read_vertices(24U))
            EXPECT_NEAR(vertex.position.x, 0.25F, 1e-5F);

        buffer->set_resolution(6U, 6U, 6U);
        EXPECT_EQ(buffer->get_size_bytes(),
            static_cast<size_t>(6U * 6U * 6U * 15U) * sizeof(Kakshya::MeshVertex));
        run_mesh();
        ASSERT_EQ(buffer->get_live_vertex_count(), 216U);
        for (const auto& vertex : read_vertices(216U))
            EXPECT_NEAR(vertex.position.x, 0.25F, 1e-5F);
    }

    TEST_F(ComputeMeshBufferDeviceTest, GpuResolutionChangeKeepsAllThreeProcessorsOnTheSameBuffers)
    {
        attach(fields::plane);
        auto prep = std::dynamic_pointer_cast<Buffers::SDFPrepProcessor>(buffer->get_default_processor());
        auto field = buffer->get_field_processor();
        auto mesh = buffer->get_mesh_processor();
        ASSERT_NE(prep, nullptr);
        ASSERT_NE(field, nullptr);
        ASSERT_NE(mesh, nullptr);
        const auto original_grid = prep->grid_buf();
        const auto original_counter = prep->counter_buf();

        buffer->set_resolution(2U, 2U, 2U);

        EXPECT_NE(prep->grid_buf(), original_grid);
        EXPECT_NE(prep->counter_buf(), original_counter);
        EXPECT_EQ(prep->grid_buf()->get_size_bytes(), 27U * sizeof(float));
        EXPECT_EQ(mesh->counter_buf(), prep->counter_buf());

        run_mesh();
        ASSERT_EQ(buffer->get_live_vertex_count(), 24U);
        for (const auto& vertex : read_vertices(24U))
            EXPECT_NEAR(vertex.position.x, 0.25F, 1e-5F);

        buffer->set_resolution(6U, 6U, 6U);
        EXPECT_EQ(prep->grid_buf()->get_size_bytes(), 343U * sizeof(float));
        EXPECT_EQ(mesh->counter_buf(), prep->counter_buf());
        EXPECT_EQ(buffer->get_size_bytes(),
            static_cast<size_t>(6U * 6U * 6U * 15U) * sizeof(Kakshya::MeshVertex));
        run_mesh();
        ASSERT_EQ(buffer->get_live_vertex_count(), 216U);
        for (const auto& vertex : read_vertices(216U))
            EXPECT_NEAR(vertex.position.x, 0.25F, 1e-5F);
    }

    TEST_F(ComputeMeshBufferDeviceTest, ResolutionChangePreservesGpuFieldBoundsTimeAndParameters)
    {
        attach(fields::moving_plane);
        auto field = buffer->get_field_processor();
        auto prep = std::dynamic_pointer_cast<Buffers::SDFPrepProcessor>(buffer->get_default_processor());
        ASSERT_NE(field, nullptr);
        ASSERT_NE(prep, nullptr);

        field->set_time(0.25F);
        field->set_params(glm::vec4(0.75F, 0.0F, 0.0F, 0.0F));
        buffer->set_bounds(glm::vec3(0.0F, -1.0F, -1.0F), glm::vec3(2.0F, 1.0F, 1.0F));
        buffer->set_resolution(2U, 2U, 2U);
        run_mesh();

        const auto* grid = static_cast<const float*>(prep->grid_buf()->get_mapped_ptr());
        ASSERT_NE(grid, nullptr);
        EXPECT_FLOAT_EQ(field->get_time(), 0.25F);
        EXPECT_NEAR(grid[0], -0.5F, 1e-5F);
        EXPECT_NEAR(grid[2], 1.5F, 1e-5F);
        ASSERT_EQ(buffer->get_live_vertex_count(), 24U);
        for (const auto& vertex : read_vertices(24U))
            EXPECT_NEAR(vertex.position.x, 0.5F, 1e-5F);
    }

    TEST_F(ComputeMeshBufferDeviceTest, ShrinkingResolutionRetainsCapacityAndUpdatesVertexLayout)
    {
        attach(fields::plane.cpu);
        buffer->set_resolution(6U, 6U, 6U);
        const size_t grown_capacity = buffer->get_size_bytes();
        buffer->set_resolution(2U, 2U, 2U);

        EXPECT_EQ(buffer->get_size_bytes(), grown_capacity);
        const auto layout = buffer->get_vertex_layout();
        ASSERT_TRUE(layout.has_value());
        EXPECT_EQ(layout->vertex_count, 2U * 2U * 2U * 15U);

        run_mesh();
        EXPECT_EQ(buffer->get_live_vertex_count(), 24U);
    }

    TEST_F(ComputeMeshBufferDeviceTest, ShaderFileFieldUsesTheResizedGrid)
    {
        attach(std::filesystem::path { "sdf_field_gyroid.comp.spv" });
        auto prep = std::dynamic_pointer_cast<Buffers::SDFPrepProcessor>(buffer->get_default_processor());
        ASSERT_NE(prep, nullptr);

        buffer->set_resolution(6U, 6U, 6U);
        run_mesh();

        EXPECT_EQ(prep->grid_buf()->get_size_bytes(), 343U * sizeof(float));
        const auto* grid = static_cast<const float*>(prep->grid_buf()->get_mapped_ptr());
        ASSERT_NE(grid, nullptr);
        EXPECT_TRUE(std::all_of(grid, grid + 343U, [](float value) { return std::isfinite(value); }));
        EXPECT_TRUE(std::any_of(grid, grid + 343U, [](float value) { return value < 0.0F; }));
        EXPECT_TRUE(std::any_of(grid, grid + 343U, [](float value) { return value > 0.0F; }));
        const auto count = buffer->get_live_vertex_count();
        ASSERT_TRUE(count.has_value());
        EXPECT_GT(*count, 0U);
        EXPECT_EQ(*count % 3U, 0U);
    }

    TEST_F(ComputeMeshBufferDeviceTest, ShaderFileFieldProducesFiniteGridAndAVisibleSurface)
    {
        attach(std::filesystem::path { "sdf_field_gyroid.comp.spv" });

        run_mesh();
        const auto count = buffer->get_live_vertex_count();
        ASSERT_TRUE(count.has_value());
        EXPECT_GT(*count, 0U);
        EXPECT_EQ(*count % 3U, 0U);

        auto prep = std::dynamic_pointer_cast<Buffers::SDFPrepProcessor>(buffer->get_default_processor());
        ASSERT_NE(prep, nullptr);
        const auto* grid = static_cast<const float*>(prep->grid_buf()->get_mapped_ptr());
        ASSERT_NE(grid, nullptr);
        constexpr size_t corners = static_cast<size_t>(k_resolution + 1)
            * (k_resolution + 1) * (k_resolution + 1);
        EXPECT_TRUE(std::all_of(grid, grid + corners, [](float value) { return std::isfinite(value); }));
        EXPECT_TRUE(std::any_of(grid, grid + corners, [](float value) { return value < 0.0F; }));
        EXPECT_TRUE(std::any_of(grid, grid + corners, [](float value) { return value > 0.0F; }));
    }

#if defined(MAYAFLUX_TEST_NATIVE_WINDOWS)

    class ComputeMeshBufferPresentationTest : public PresentingVulkanTest {
    protected:
        void SetUp() override
        {
            PresentingVulkanTest::SetUp();
            if (IsSkipped())
                return;

            Portal::Graphics::get_render_flow().shutdown();
            Portal::Graphics::get_compute_press().shutdown();
            Portal::Graphics::get_shader_foundry().shutdown();
            auto borrowed = std::shared_ptr<Core::VulkanBackend>(&backend(), [](Core::VulkanBackend*) { });
            ASSERT_TRUE(Portal::Graphics::get_shader_foundry().initialize(borrowed));
            foundry_ready = true;
            ASSERT_TRUE(Portal::Graphics::get_compute_press().initialize());
            compute_ready = true;
            ASSERT_TRUE(Portal::Graphics::get_render_flow().initialize());
            flow_ready = true;

            window = make_window({ 0.0F, 0.0F, 0.0F, 1.0F });
            ASSERT_NE(window, nullptr);
            ASSERT_TRUE(backend().register_window(window));
            service = Registry::BackendRegistry::instance()
                          .get_service<Registry::Service::BufferService>();
            ASSERT_NE(service, nullptr);

            auto config = config_for(fields::sphere);
            config.resolution_x = 16U;
            config.resolution_y = 16U;
            config.resolution_z = 16U;
            buffer = std::make_shared<ComputeMeshBuffer>(std::move(config));
            service->initialize_buffer(buffer);
            ASSERT_TRUE(buffer->is_initialized());
            buffer->setup_processors(ProcessingToken::GRAPHICS_BACKEND);
            buffer->setup_rendering({ .target_window = window });

            root = std::make_shared<Buffers::RootGraphicsBuffer>();
            root->initialize();
            root->add_child_buffer(buffer);
        }

        void TearDown() override
        {
            if (buffer) {
                if (auto render = buffer->get_render_processor()) {
                    if (auto chain = buffer->get_processing_chain())
                        chain->remove_processor_anywhere(render, buffer);
                }
                buffer->set_default_processor(nullptr);
            }
            root.reset();
            if (buffer && buffer->is_initialized() && service)
                service->destroy_buffer(buffer);
            buffer.reset();
            if (flow_ready)
                Portal::Graphics::get_render_flow().shutdown();
            if (compute_ready)
                Portal::Graphics::get_compute_press().shutdown();
            if (foundry_ready)
                Portal::Graphics::get_shader_foundry().shutdown();
            PresentingVulkanTest::TearDown();
        }

        std::array<uint8_t, 4> center_pixel()
        {
            auto* display = Registry::BackendRegistry::instance()
                                .get_service<Registry::Service::DisplayService>();
            EXPECT_NE(display, nullptr);
            if (!display)
                return {};

            uint32_t width = 0;
            uint32_t height = 0;
            display->get_swapchain_extent(window, width, height);
            std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
            if (pixels.empty())
                return {};
            EXPECT_TRUE(display->readback_swapchain_region(
                window, pixels.data(), 0, 0, width, height, pixels.size()));
            const size_t offset = (static_cast<size_t>(height / 2) * width + width / 2) * 4;
            return { pixels[offset], pixels[offset + 1], pixels[offset + 2], pixels[offset + 3] };
        }

        std::shared_ptr<Core::Window> window;
        std::shared_ptr<ComputeMeshBuffer> buffer;
        std::shared_ptr<Buffers::RootGraphicsBuffer> root;
        Registry::Service::BufferService* service {};
        bool foundry_ready {};
        bool compute_ready {};
        bool flow_ready {};
    };

    TEST_F(ComputeMeshBufferPresentationTest, GpuGeneratedSphereIsVisibleAfterPresentation)
    {
        root->process_default();
        ASSERT_TRUE(buffer->get_live_vertex_count().has_value());
        ASSERT_GT(*buffer->get_live_vertex_count(), 0U);

        Buffers::PresentProcessor presenter;
        presenter.processing_function(root);

        const auto pixel = center_pixel();
        EXPECT_GT(pixel[0], 100U);
        EXPECT_GT(pixel[1], 100U);
        EXPECT_GT(pixel[2], 100U);
    }

#endif

}
}
