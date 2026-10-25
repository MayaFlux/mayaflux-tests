#include "gtest/gtest.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "volume_grid_fixture.hpp"
#include "../../support/native_window_test_support.hpp"

#include "MayaFlux/Buffers/Root/RootGraphicsBuffer.hpp"
#include "MayaFlux/Buffers/Shaders/RenderProcessor.hpp"
#include "MayaFlux/Buffers/Shaders/SDFMeshProcessor.hpp"
#include "MayaFlux/Buffers/Staging/StagingUtils.hpp"
#include "MayaFlux/Buffers/State/VolumeSurfaceProcessor.hpp"
#include "MayaFlux/Kakshya/NDData/VertexFormats.hpp"
#include "MayaFlux/Portal/Graphics/RenderFlow.hpp"
#include "MayaFlux/Registry/Service/DisplayService.hpp"

namespace MayaFlux::Test {
namespace {

    using Buffers::VolumeGridBuffer;

    VolumeGridBuffer::SurfaceConfig surface_config(
        glm::uvec3 resolution = glm::uvec3(4U), float threshold = 0.25F)
    {
        return {
            .field_name = "density",
            .resolution = resolution,
            .threshold = threshold,
        };
    }

    class VolumeSurfaceProcessorDeviceTest : public VolumeGridDeviceFixture {
    protected:
        std::shared_ptr<VolumeGridBuffer> plane(
            glm::uvec3 resolution = glm::uvec3(4U), float threshold = 0.25F)
        {
            auto volume = attach_surface({ { "density", sizeof(float) } },
                surface_config(resolution, threshold));
            volume->seed("density", Kinesis::SpatialField { [](const glm::vec3& p) {
                return p.x;
            } });
            settle(volume, "density");
            return volume;
        }

        static std::vector<float> corners(const std::shared_ptr<VolumeGridBuffer>& volume)
        {
            const auto surface = volume->surface_processor();
            const auto* data = static_cast<const float*>(surface->grid_buf()->get_mapped_ptr());
            EXPECT_NE(data, nullptr);
            if (!data)
                return {};
            return { data, data + surface->corner_count() };
        }

        static void expect_plane_corners(const std::shared_ptr<VolumeGridBuffer>& volume,
            float threshold)
        {
            const auto values = corners(volume);
            ASSERT_EQ(values.size(), 125U);
            for (size_t i = 0; i < values.size(); ++i) {
                const size_t x = i % 5U;
                const float expected = threshold + (x < 2U ? 1.0F : x == 2U ? 0.0F : -1.0F);
                EXPECT_NEAR(values[i], expected, 1e-5F) << "corner " << i;
            }
        }

        static void resample(const std::shared_ptr<VolumeGridBuffer>& volume)
        {
            volume->process_default();
        }

        static void extract(const std::shared_ptr<VolumeGridBuffer>& volume)
        {
            volume->process_default();
            volume->get_processing_chain()->process(volume);
        }

        static std::vector<Kakshya::MeshVertex> vertices(
            const std::shared_ptr<VolumeGridBuffer>& volume)
        {
            const uint32_t count = volume->get_live_vertex_count().value_or(0U);
            std::vector<uint8_t> bytes(volume->get_size_bytes());
            Buffers::download_from_gpu(volume, bytes.data(), bytes.size());
            std::vector<Kakshya::MeshVertex> result(count);
            std::memcpy(result.data(), bytes.data(), result.size() * sizeof(result.front()));
            return result;
        }
    };

    TEST_F(VolumeSurfaceProcessorDeviceTest, NoSurfaceCreatesNoExtractionStages)
    {
        auto volume = attach({ { "density", sizeof(float) } });
        volume->setup_processors(Buffers::ProcessingToken::GRAPHICS_BACKEND);
        volume->setup_rendering({});

        ASSERT_NE(volume->get_processing_chain(), nullptr);
        EXPECT_EQ(volume->get_default_processor(), nullptr);
        EXPECT_TRUE(volume->get_processing_chain()->get_processors(volume).empty());
        EXPECT_EQ(volume->surface_processor(), nullptr);
        EXPECT_EQ(volume->mesh_processor(), nullptr);
        EXPECT_EQ(volume->get_render_processor(), nullptr);
        EXPECT_FALSE(volume->get_live_vertex_count().has_value());
    }

    TEST_F(VolumeSurfaceProcessorDeviceTest, SurfaceWiresResamplerMeshAndRenderAtItsOwnResolution)
    {
        auto volume = attach_surface({ { "density", sizeof(float) } },
            surface_config({ 4U, 3U, 2U }));
        const auto surface = volume->surface_processor();
        const auto mesh = volume->mesh_processor();
        ASSERT_NE(surface, nullptr);
        ASSERT_NE(mesh, nullptr);

        EXPECT_EQ(volume->get_default_processor(), surface);
        ASSERT_EQ(volume->get_processing_chain()->get_processors(volume).size(), 1U);
        EXPECT_EQ(volume->get_processing_chain()->get_processors(volume).front(), mesh);
        EXPECT_EQ(volume->get_processing_chain()->get_final_processor(volume),
            volume->get_render_processor());
        EXPECT_EQ(surface->get_res_x(), 4U);
        EXPECT_EQ(surface->get_res_y(), 3U);
        EXPECT_EQ(surface->get_res_z(), 2U);
        EXPECT_EQ(surface->corner_count(), 60U);
        EXPECT_EQ(surface->worst_case_vertices(), 360U);
        EXPECT_EQ(surface->grid_buf()->get_size_bytes(), 60U * sizeof(float));
        EXPECT_EQ(volume->get_size_bytes(), 360U * sizeof(Kakshya::MeshVertex));
        ASSERT_TRUE(volume->get_vertex_layout().has_value());
        EXPECT_EQ(volume->get_vertex_layout()->vertex_count, 360U);
        EXPECT_EQ(mesh->counter_buf()->get_size_bytes(), sizeof(uint32_t));
        EXPECT_TRUE(volume->get_live_vertex_count().has_value());
    }

    TEST_F(VolumeSurfaceProcessorDeviceTest, ResampleDispatchCoversPartialWorkgroups)
    {
        auto volume = plane();
        const auto surface = volume->surface_processor();
        const auto& dispatch = surface->get_dispatch_config();

        EXPECT_EQ(dispatch.mode, Buffers::ShaderDispatchConfig::DispatchMode::MANUAL);
        EXPECT_EQ(dispatch.workgroup_x, 64U);
        EXPECT_EQ(dispatch.group_count_x, 2U);
        EXPECT_EQ(dispatch.group_count_y, 1U);
        EXPECT_EQ(dispatch.group_count_z, 1U);

        resample(volume);
        const auto values = corners(volume);
        ASSERT_EQ(values.size(), 125U);
        EXPECT_NEAR(values.front(), 1.25F, 1e-5F);
        EXPECT_NEAR(values.back(), -0.75F, 1e-5F);
    }

    TEST_F(VolumeSurfaceProcessorDeviceTest, UniformFieldWritesEveryCornerWithThresholdMinusDensity)
    {
        auto volume = attach_surface({ { "density", sizeof(float) } },
            surface_config({ 3U, 2U, 4U }, 1.0F));
        seed_raw(volume, "density", std::vector<float>(8U, 2.0F));

        resample(volume);
        const auto values = corners(volume);
        ASSERT_EQ(values.size(), 60U);
        EXPECT_TRUE(std::all_of(values.begin(), values.end(), [](float value) {
            return value == -1.0F;
        }));
    }

    TEST_F(VolumeSurfaceProcessorDeviceTest, TrilinearResampleClampsBoundaryCells)
    {
        auto volume = plane({ 4U, 2U, 2U }, 0.0F);
        resample(volume);
        const auto values = corners(volume);
        ASSERT_EQ(values.size(), 45U);

        for (uint32_t z = 0; z < 3U; ++z) {
            for (uint32_t y = 0; y < 3U; ++y) {
                const size_t base = (static_cast<size_t>(z) * 3U + y) * 5U;
                EXPECT_NEAR(values[base + 0U], 1.0F, 1e-5F);
                EXPECT_NEAR(values[base + 1U], 1.0F, 1e-5F);
                EXPECT_NEAR(values[base + 2U], 0.0F, 1e-5F);
                EXPECT_NEAR(values[base + 3U], -1.0F, 1e-5F);
                EXPECT_NEAR(values[base + 4U], -1.0F, 1e-5F);
            }
        }
    }

    TEST_F(VolumeSurfaceProcessorDeviceTest, ThresholdChangeUpdatesTheNextGrid)
    {
        auto volume = plane({ 2U, 2U, 2U }, 0.0F);
        resample(volume);
        const auto initial = corners(volume);
        volume->surface_processor()->set_threshold(0.5F);

        resample(volume);
        const auto shifted = corners(volume);
        ASSERT_EQ(shifted.size(), initial.size());
        EXPECT_EQ(volume->surface_processor()->get_threshold(), 0.5F);
        for (size_t i = 0; i < shifted.size(); ++i)
            EXPECT_NEAR(shifted[i], initial[i] + 0.5F, 1e-5F);
    }

    TEST_F(VolumeSurfaceProcessorDeviceTest, ResampleReadsTheCurrentFieldGeneration)
    {
        auto volume = attach_surface({ { "density", sizeof(float) } },
            surface_config({ 2U, 2U, 2U }, 0.0F));
        seed_raw(volume, "density", std::vector<float>(8U, 1.0F));
        resample(volume);
        EXPECT_EQ(corners(volume), std::vector<float>(27U, -1.0F));

        volume->swap_field("density");
        seed_raw(volume, "density", std::vector<float>(8U, 3.0F));
        resample(volume);
        EXPECT_EQ(corners(volume), std::vector<float>(27U, -3.0F));
    }

    TEST_F(VolumeSurfaceProcessorDeviceTest, InterleavedVolumesKeepTheirOwnCornerGrids)
    {
        auto first = attach_surface({ { "density", sizeof(float) } },
            surface_config({ 2U, 2U, 2U }, 0.0F));
        auto second = attach_surface({ { "density", sizeof(float) } },
            surface_config({ 2U, 2U, 2U }, 0.0F));
        seed_raw(first, "density", std::vector<float>(8U, 1.0F));
        seed_raw(second, "density", std::vector<float>(8U, 4.0F));

        resample(first);
        resample(second);
        EXPECT_EQ(corners(first), std::vector<float>(27U, -1.0F));
        EXPECT_EQ(corners(second), std::vector<float>(27U, -4.0F));
        EXPECT_NE(first->surface_processor()->grid_buf(), second->surface_processor()->grid_buf());

        seed_raw(first, "density", std::vector<float>(8U, 2.0F));
        resample(first);
        EXPECT_EQ(corners(first), std::vector<float>(27U, -2.0F));
        EXPECT_EQ(corners(second), std::vector<float>(27U, -4.0F));
    }

    TEST_F(VolumeSurfaceProcessorDeviceTest, ExtractedPlaneHasFiniteVerticesAtTheThreshold)
    {
        auto volume = plane();
        extract(volume);
        const auto count = volume->get_live_vertex_count();
        ASSERT_TRUE(count.has_value());
        ASSERT_GT(*count, 0U);
        EXPECT_EQ(*count % 3U, 0U);
        EXPECT_LE(*count, volume->surface_processor()->worst_case_vertices());

        const auto mesh = vertices(volume);
        ASSERT_EQ(mesh.size(), *count);
        for (const auto& vertex : mesh) {
            EXPECT_NEAR(vertex.position.x, 0.25F, 1e-4F);
            EXPECT_TRUE(std::isfinite(vertex.normal.x));
            EXPECT_TRUE(std::isfinite(vertex.normal.y));
            EXPECT_TRUE(std::isfinite(vertex.normal.z));
        }
    }

    TEST_F(VolumeSurfaceProcessorDeviceTest, UpdatedThresholdMovesTheExtractedPlane)
    {
        auto volume = plane();
        extract(volume);
        const auto first_count = volume->get_live_vertex_count();
        EXPECT_EQ(read<float>(volume, "density"),
            (std::vector<float> { -1.0F, 1.0F, -1.0F, 1.0F,
                -1.0F, 1.0F, -1.0F, 1.0F }));
        expect_plane_corners(volume, 0.25F);
        ASSERT_TRUE(first_count.has_value());
        EXPECT_FALSE(volume->mesh_processor()->is_dirty());
        ASSERT_GT(*first_count, 0U);

        volume->surface_processor()->set_threshold(0.75F);
        volume->mesh_processor()->set_dirty();
        extract(volume);
        const auto count = volume->get_live_vertex_count();
        expect_plane_corners(volume, 0.75F);
        ASSERT_TRUE(count.has_value());
        EXPECT_FALSE(volume->mesh_processor()->is_dirty());
        ASSERT_GT(*count, 0U);
        for (const auto& vertex : vertices(volume))
            EXPECT_NEAR(vertex.position.x, 0.75F, 1e-4F);
    }

    TEST_F(VolumeSurfaceProcessorDeviceTest, ExtractionCounterClearsThePreviousMesh)
    {
        auto volume = plane();
        extract(volume);
        const auto first_count = volume->get_live_vertex_count();
        EXPECT_EQ(read<float>(volume, "density"),
            (std::vector<float> { -1.0F, 1.0F, -1.0F, 1.0F,
                -1.0F, 1.0F, -1.0F, 1.0F }));
        expect_plane_corners(volume, 0.25F);
        ASSERT_TRUE(first_count.has_value());
        EXPECT_FALSE(volume->mesh_processor()->is_dirty());
        ASSERT_GT(*first_count, 0U);

        seed_raw(volume, "density", std::vector<float>(8U, -4.0F));
        volume->mesh_processor()->set_dirty();
        extract(volume);
        EXPECT_EQ(corners(volume), std::vector<float>(125U, 4.25F));
        EXPECT_FALSE(volume->mesh_processor()->is_dirty());
        EXPECT_EQ(volume->get_live_vertex_count(), 0U);
    }

#if defined(MAYAFLUX_TEST_NATIVE_WINDOWS)

    class VolumeSurfacePresentationTest : public PresentingVulkanTest {
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

            auto lattice = small_volume_lattice();
            lattice.resolution = glm::uvec3(8U);
            lattice.bounds = { .min = glm::vec3(-1.0F), .max = glm::vec3(1.0F) };
            volume = std::make_shared<VolumeGridBuffer>(
                lattice,
                std::vector<VolumeGridBuffer::FieldDecl> { { "density", sizeof(float) } },
                surface_config(glm::uvec3(16U), 0.2F));
            service->initialize_buffer(volume);
            ASSERT_TRUE(volume->is_initialized());
            volume->setup_processors(Buffers::ProcessingToken::GRAPHICS_BACKEND);
            volume->setup_rendering({ .target_window = window });
            volume->seed("density", Kinesis::SpatialField { [](const glm::vec3& p) {
                return 1.0F - glm::length(p);
            } });

            root = std::make_shared<Buffers::RootGraphicsBuffer>();
            root->initialize();
            root->add_child_buffer(volume);
        }

        void TearDown() override
        {
            if (volume) {
                if (auto chain = volume->get_processing_chain()) {
                    const auto processors = chain->get_processors(volume);
                    for (const auto& processor : processors)
                        chain->remove_processor_anywhere(processor, volume);
                    if (auto render = chain->get_final_processor(volume))
                        chain->remove_processor_anywhere(render, volume);
                }
                volume->set_default_processor(nullptr);
                volume->set_processing_chain(nullptr, true);
            }
            root.reset();
            if (volume && volume->is_initialized() && service)
                service->destroy_buffer(volume);
            volume.reset();
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
            const size_t offset = (static_cast<size_t>(height / 2U) * width + width / 2U) * 4U;
            return { pixels[offset], pixels[offset + 1U], pixels[offset + 2U], pixels[offset + 3U] };
        }

        std::shared_ptr<Core::Window> window;
        std::shared_ptr<VolumeGridBuffer> volume;
        std::shared_ptr<Buffers::RootGraphicsBuffer> root;
        Registry::Service::BufferService* service {};
        bool foundry_ready {};
        bool compute_ready {};
        bool flow_ready {};
    };

    TEST_F(VolumeSurfacePresentationTest, ExtractedSphereIsVisibleAfterPresentation)
    {
        root->process_default();
        ASSERT_GT(volume->get_live_vertex_count().value_or(0U), 0U);

        Buffers::PresentProcessor presenter;
        presenter.processing_function(root);

        const auto pixel = center_pixel();
        EXPECT_GT(pixel[0], 100U);
        EXPECT_GT(pixel[1], 100U);
        EXPECT_GT(pixel[2], 100U);
    }

#endif

} // namespace
} // namespace MayaFlux::Test
