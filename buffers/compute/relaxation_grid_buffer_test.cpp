#include "gtest/gtest.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "../../support/scripted_window.hpp"
#include "../../support/native_window_test_support.hpp"
#include "../../support/vulkan_test_support.hpp"

#include "MayaFlux/Buffers/BufferProcessingChain.hpp"
#include "MayaFlux/Buffers/Root/RootGraphicsBuffer.hpp"
#include "MayaFlux/Buffers/Shaders/RenderProcessor.hpp"
#include "MayaFlux/Buffers/Staging/StagingUtils.hpp"
#include "MayaFlux/Buffers/State/RelaxationEmitProcessor.hpp"
#include "MayaFlux/Buffers/State/RelaxationGridBuffer.hpp"
#include "MayaFlux/Buffers/State/RelaxationSpecs.hpp"
#include "MayaFlux/Buffers/State/RelaxationStepProcessor.hpp"
#include "MayaFlux/Kakshya/NDData/VertexFormats.hpp"
#include "MayaFlux/Kriya/Awaiters/BroadcastAwaiter.hpp"
#include "MayaFlux/Portal/Graphics/ComputePress.hpp"
#include "MayaFlux/Portal/Graphics/RenderFlow.hpp"
#include "MayaFlux/Portal/Graphics/ShaderFoundry.hpp"
#include "MayaFlux/Registry/BackendRegistry.hpp"
#include "MayaFlux/Registry/Service/BufferService.hpp"
#include "MayaFlux/Registry/Service/DisplayService.hpp"

namespace MayaFlux::Test {
namespace {

    using Buffers::RelaxationGridBuffer;
    using Buffers::ProcessingToken;

    RelaxationGridBuffer::GridConfig conway_config(uint32_t width = 5U, uint32_t height = 5U)
    {
        return {
            .width = width,
            .height = height,
            .rule = {
                .state = Kakshya::GpuDataFormat::UINT32,
                .shader = std::string { "relax_conway_rule.comp" },
            },
            .emit = {
                .state = Kakshya::GpuDataFormat::UINT32,
                .shader = std::string { "relax_vertex_emit.comp" },
            },
        };
    }

    std::vector<uint32_t> blinker(uint32_t width = 5U, uint32_t height = 5U)
    {
        std::vector<uint32_t> cells(static_cast<size_t>(width) * height);
        const uint32_t center_x = width / 2U;
        const uint32_t center_y = height / 2U;
        for (uint32_t x = center_x - 1U; x <= center_x + 1U; ++x)
            cells[static_cast<size_t>(center_y) * width + x] = 1U;
        return cells;
    }

    TEST(RelaxationGridConfigTest, PathStagesRequireAnExplicitStateFormat)
    {
        auto config = conway_config();
        config.rule.state.reset();
        EXPECT_THROW(static_cast<void>(RelaxationGridBuffer { config }), std::invalid_argument);
    }

    TEST(RelaxationGridConfigTest, RuleAndEmitMustUseTheSameCellFormat)
    {
        auto config = conway_config();
        config.emit.state = Kakshya::GpuDataFormat::FLOAT32;
        EXPECT_THROW(static_cast<void>(RelaxationGridBuffer { config }), std::invalid_argument);
    }

    TEST(RelaxationGridConfigTest, ThreeComponentCellFormatsAreRejected)
    {
        auto config = conway_config();
        config.rule.state = Kakshya::GpuDataFormat::VEC3_F32;
        config.emit.state = Kakshya::GpuDataFormat::VEC3_F32;
        EXPECT_THROW(static_cast<void>(RelaxationGridBuffer { config }), std::invalid_argument);
    }

    class RelaxationGridDeviceTest : public VulkanDeviceTest {
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
            for (auto& owned : grids) {
                if (auto render = owned->get_render_processor()) {
                    if (auto chain = owned->get_processing_chain())
                        chain->remove_processor_anywhere(render, owned);
                }
                owned->set_default_processor(nullptr);
                if (owned->is_initialized() && service)
                    service->destroy_buffer(owned);
            }
            grids.clear();
            grid.reset();
            windows.clear();
        }

        std::shared_ptr<RelaxationGridBuffer> attach(RelaxationGridBuffer::GridConfig config = conway_config())
        {
            grid = std::make_shared<RelaxationGridBuffer>(config);
            service->initialize_buffer(grid);
            EXPECT_TRUE(grid->is_initialized());
            grids.push_back(grid);
            grid->setup_processors(ProcessingToken::GRAPHICS_BACKEND);
            return grid;
        }

        std::shared_ptr<RelaxationGridBuffer> attach_positional(
            uint32_t width, uint32_t height, size_t stride)
        {
            grid = std::make_shared<RelaxationGridBuffer>(
                width, height, stride,
                std::string { "relax_conway_rule.comp" },
                std::string { "relax_vertex_emit.comp" });
            service->initialize_buffer(grid);
            EXPECT_TRUE(grid->is_initialized());
            grids.push_back(grid);
            grid->setup_processors(ProcessingToken::GRAPHICS_BACKEND);
            return grid;
        }

        template <typename T>
        void seed(const std::vector<T>& cells)
        {
            grid->seed_state(cells.data(), cells.size() * sizeof(T));
        }

        template <typename T>
        void seed_back(const std::vector<T>& cells)
        {
            auto& generation = grid->get_buffer_resources().back_buffers[grid->back_index()];
            std::memcpy(generation.mapped_ptr, cells.data(), cells.size() * sizeof(T));
        }

        template <typename T>
        std::vector<T> front_state(const std::shared_ptr<RelaxationGridBuffer>& target)
        {
            const auto& generation = target->get_buffer_resources().back_buffers[target->front_index()];
            std::vector<T> cells(target->get_cell_count());
            std::memcpy(cells.data(), generation.mapped_ptr, cells.size() * sizeof(T));
            return cells;
        }

        template <typename T>
        std::vector<T> back_state(const std::shared_ptr<RelaxationGridBuffer>& target)
        {
            const auto& generation = target->get_buffer_resources().back_buffers[target->back_index()];
            std::vector<T> cells(target->get_cell_count());
            std::memcpy(cells.data(), generation.mapped_ptr, cells.size() * sizeof(T));
            return cells;
        }

        void step() { grid->process_default(); }
        void emit() { grid->get_processing_chain()->process(grid); }
        void cycle()
        {
            step();
            emit();
        }

        std::vector<Kakshya::Vertex> vertices(const std::shared_ptr<RelaxationGridBuffer>& target)
        {
            std::vector<uint8_t> bytes(target->get_size_bytes());
            Buffers::download_from_gpu(target, bytes.data(), bytes.size());
            std::vector<Kakshya::Vertex> result(target->get_cell_count());
            std::memcpy(result.data(), bytes.data(), bytes.size());
            return result;
        }

        static inline bool s_foundry_ready {};
        static inline bool s_compute_ready {};
        Registry::Service::BufferService* service {};
        std::shared_ptr<RelaxationGridBuffer> grid;
        std::vector<std::shared_ptr<RelaxationGridBuffer>> grids;
        std::vector<std::shared_ptr<ScriptedWindow>> windows;
    };

    TEST_F(RelaxationGridDeviceTest, ConstructionAllocatesTwoMappedGenerationsAndOneVertexPerCell)
    {
        attach(conway_config(17U, 19U));

        EXPECT_EQ(grid->get_grid_width(), 17U);
        EXPECT_EQ(grid->get_grid_height(), 19U);
        EXPECT_EQ(grid->get_cell_count(), 323U);
        EXPECT_EQ(grid->get_cell_stride_bytes(), sizeof(uint32_t));
        EXPECT_EQ(grid->get_state_bytes(), 323U * sizeof(uint32_t));
        EXPECT_EQ(grid->get_size_bytes(), 323U * sizeof(Kakshya::Vertex));
        const auto& generations = grid->get_buffer_resources().back_buffers;
        ASSERT_EQ(generations.size(), 2U);
        EXPECT_NE(generations[0].mapped_ptr, nullptr);
        EXPECT_NE(generations[1].mapped_ptr, nullptr);
        EXPECT_NE(generations[0].buffer, generations[1].buffer);
        EXPECT_EQ(grid->front_index(), 0U);
        EXPECT_EQ(grid->back_index(), 1U);

        const auto layout = grid->get_vertex_layout();
        ASSERT_TRUE(layout.has_value());
        EXPECT_EQ(layout->vertex_count, 323U);
    }

    TEST_F(RelaxationGridDeviceTest, PositionalConstructorRetainsItsShaderSourcesAndCellStride)
    {
        attach_positional(5U, 5U, sizeof(uint32_t));

        EXPECT_EQ(grid->get_cell_stride_bytes(), sizeof(uint32_t));
        EXPECT_EQ(std::get<std::string>(grid->rule_source()), "relax_conway_rule.comp");
        EXPECT_EQ(std::get<std::string>(grid->emit_source()), "relax_vertex_emit.comp");
        EXPECT_EQ(grid->step_processor()->get_config().shader_path, "relax_conway_rule.comp");
        EXPECT_EQ(grid->emit_processor()->get_config().shader_path, "relax_vertex_emit.comp");
    }

    TEST_F(RelaxationGridDeviceTest, ProcessorChainAndDispatchCoverPartialWorkgroups)
    {
        attach(conway_config(17U, 19U));
        auto step_processor = grid->step_processor();
        auto emit_processor = grid->emit_processor();
        ASSERT_NE(step_processor, nullptr);
        ASSERT_NE(emit_processor, nullptr);
        EXPECT_EQ(grid->get_default_processor(), step_processor);
        const auto chain = grid->get_processing_chain();
        ASSERT_NE(chain, nullptr);
        ASSERT_EQ(chain->get_processors(grid).size(), 1U);
        EXPECT_EQ(chain->get_processors(grid).front(), emit_processor);
        EXPECT_EQ(step_processor->get_dispatch_config().group_count_x, 2U);
        EXPECT_EQ(step_processor->get_dispatch_config().group_count_y, 2U);
        EXPECT_EQ(emit_processor->get_dispatch_config().group_count_x, 2U);

        auto window = std::make_shared<ScriptedWindow>(320, 240);
        windows.push_back(window);
        grid->setup_rendering({ .target_window = window });
        const auto render = grid->get_render_processor();
        ASSERT_NE(render, nullptr);
        EXPECT_EQ(chain->get_final_processor(grid), render);
        EXPECT_EQ(render->get_config().shader_path, "point.vert.spv");
    }

    TEST_F(RelaxationGridDeviceTest, PartialWorkgroupsProcessTheLastCellInBothStages)
    {
        attach(conway_config(17U, 19U));
        seed(std::vector<uint32_t>(323U, 1U));
        seed_back(std::vector<uint32_t>(323U, 1U));

        cycle();

        const auto cells = front_state<uint32_t>(grid);
        EXPECT_EQ(cells.front(), 0U);
        EXPECT_EQ(cells[16U * 17U + 16U], 0U);
        EXPECT_EQ(cells.back(), 0U);
        const auto emitted = vertices(grid);
        EXPECT_NEAR(emitted.front().position.z, 2.0F, 1e-5F);
        EXPECT_NEAR(emitted.back().position.z, 2.0F, 1e-5F);
        EXPECT_NEAR(emitted.back().uv.x, 16.5F / 17.0F, 1e-5F);
        EXPECT_NEAR(emitted.back().uv.y, 18.5F / 19.0F, 1e-5F);
    }

    TEST_F(RelaxationGridDeviceTest, SeedWritesOnlyTheFrontGeneration)
    {
        attach();
        const std::vector<uint32_t> previous_back(25U, 7U);
        seed_back(previous_back);
        const auto cells = blinker();
        seed(cells);

        EXPECT_EQ(front_state<uint32_t>(grid), cells);
        EXPECT_EQ(back_state<uint32_t>(grid), previous_back);
        EXPECT_FALSE(grid->snapshot_source()->has_pending());
    }

    TEST_F(RelaxationGridDeviceTest, IncorrectSeedSizeLeavesTheFrontGenerationUntouched)
    {
        attach();
        const auto initial = blinker();
        seed(initial);
        const std::vector<uint32_t> short_seed(24U, 1U);

        grid->seed_state(short_seed.data(), short_seed.size() * sizeof(uint32_t));

        EXPECT_EQ(front_state<uint32_t>(grid), initial);
        EXPECT_EQ(grid->front_index(), 0U);
    }

    TEST_F(RelaxationGridDeviceTest, ConwayStepsSwapGenerationsAndEmitTheNewFront)
    {
        attach();
        const auto initial = blinker();
        seed(initial);

        cycle();
        EXPECT_EQ(grid->front_index(), 1U);
        const auto first = front_state<uint32_t>(grid);
        EXPECT_EQ(first[1U * 5U + 2U], 1U);
        EXPECT_EQ(first[2U * 5U + 2U], 1U);
        EXPECT_EQ(first[3U * 5U + 2U], 1U);
        EXPECT_EQ(first[2U * 5U + 1U], 0U);
        EXPECT_EQ(first[2U * 5U + 3U], 0U);
        EXPECT_EQ(back_state<uint32_t>(grid), initial);

        const auto emitted = vertices(grid);
        EXPECT_NEAR(emitted[1U * 5U + 2U].position.z, 0.0F, 1e-5F);
        EXPECT_NEAR(emitted[2U * 5U + 1U].position.z, 2.0F, 1e-5F);
        EXPECT_NEAR(emitted[1U * 5U + 2U].scalar, 2.0F, 1e-5F);
        EXPECT_NEAR(emitted[1U * 5U + 2U].uv.x, 0.5F, 1e-5F);

        cycle();
        EXPECT_EQ(grid->front_index(), 0U);
        EXPECT_EQ(front_state<uint32_t>(grid), initial);
    }

    TEST_F(RelaxationGridDeviceTest, StepPredicateSkipsTheDispatchSwapAndSnapshot)
    {
        attach();
        const auto initial = blinker();
        seed(initial);
        bool advance = false;
        grid->step_processor()->set_step_predicate([&advance] { return advance; });
        grid->request_snapshot();

        step();
        EXPECT_EQ(grid->front_index(), 0U);
        EXPECT_EQ(front_state<uint32_t>(grid), initial);
        EXPECT_FALSE(grid->snapshot_source()->has_pending());

        advance = true;
        step();
        EXPECT_EQ(grid->front_index(), 1U);
        EXPECT_TRUE(grid->snapshot_source()->has_pending());
    }

    TEST_F(RelaxationGridDeviceTest, SnapshotContainsTheCompletedGenerationAndIsOneShot)
    {
        attach();
        seed(blinker());
        grid->request_snapshot();

        step();
        auto snapshot = grid->snapshot_source()->next();
        ASSERT_TRUE(snapshot.await_ready());
        const auto bytes = snapshot.await_resume();
        ASSERT_EQ(bytes.size(), grid->get_state_bytes());
        std::vector<uint32_t> cells(grid->get_cell_count());
        std::memcpy(cells.data(), bytes.data(), bytes.size());
        EXPECT_EQ(cells, front_state<uint32_t>(grid));

        step();
        EXPECT_FALSE(grid->snapshot_source()->has_pending());
    }

    TEST_F(RelaxationGridDeviceTest, EmitParametersChangeVerticesWithoutSteppingState)
    {
        auto config = conway_config();
        config.extent = 0.5F;
        config.point_size = 3.0F;
        attach(config);
        seed(blinker());
        emit();
        const auto first = vertices(grid);
        EXPECT_NEAR(first[0].position.x, -0.4F, 1e-5F);
        EXPECT_NEAR(first[0].scalar, 3.0F, 1e-5F);
        EXPECT_EQ(grid->front_index(), 0U);

        grid->emit_processor()->set_extent(1.0F);
        grid->emit_processor()->set_point_size(7.0F);
        emit();
        const auto second = vertices(grid);
        EXPECT_NEAR(second[0].position.x, -0.8F, 1e-5F);
        EXPECT_NEAR(second[0].scalar, 7.0F, 1e-5F);
        EXPECT_EQ(front_state<uint32_t>(grid), blinker());
    }

    TEST_F(RelaxationGridDeviceTest, GeneratedJacobiRuleUsesConfiguredConstantsAndFloatState)
    {
        auto config = conway_config();
        config.rule = {
            .shader = Buffers::RelaxationSpecs::jacobi_diffusion(),
            .constants = { 1.0F, 0.125F },
        };
        config.emit = {
            .state = Kakshya::GpuDataFormat::FLOAT32,
            .shader = std::string { "relax_scalar_ramp_emit.comp" },
        };
        attach(config);
        std::vector<float> cells(25U, 0.0F);
        cells[12] = 1.0F;
        seed(cells);

        cycle();
        const auto result = front_state<float>(grid);
        EXPECT_EQ(grid->get_cell_stride_bytes(), sizeof(float));
        EXPECT_NEAR(result[12], 0.0F, 1e-5F);
        EXPECT_NEAR(result[11], 0.125F, 1e-5F);
        EXPECT_NEAR(result[6], 0.125F, 1e-5F);
        EXPECT_TRUE(grid->step_processor()->is_pipeline_ready());
        EXPECT_TRUE(grid->emit_processor()->is_pipeline_ready());
        const auto emitted = vertices(grid);
        EXPECT_GT(emitted[11].color.g, emitted[12].color.g);

        grid->step_processor()->feed("rate", [] { return Buffers::ShaderProcessor::FeedValue { 0.0 }; },
            sizeof(Buffers::RelaxationStepProcessor::GridExtent), sizeof(float), false);
        step();
        EXPECT_EQ(front_state<float>(grid), result);
    }

    TEST_F(RelaxationGridDeviceTest, PathRuleUsesTrailingConstantsWithTwoFloatCellState)
    {
        auto config = conway_config();
        config.rule = {
            .state = Kakshya::GpuDataFormat::VEC2_F32,
            .shader = std::string { "relax_gray_scott_rule.comp" },
            .constants = { 0.06F, 0.062F },
        };
        config.emit = {
            .state = Kakshya::GpuDataFormat::VEC2_F32,
            .shader = std::string { "relax_scalar_emit.comp" },
        };
        attach(config);
        seed(std::vector<glm::vec2>(25U, glm::vec2(0.5F, 0.0F)));

        cycle();

        EXPECT_EQ(grid->get_cell_stride_bytes(), sizeof(glm::vec2));
        const auto cells = front_state<glm::vec2>(grid);
        EXPECT_NEAR(cells[12].x, 0.53F, 1e-5F);
        EXPECT_NEAR(cells[12].y, 0.0F, 1e-5F);
        EXPECT_TRUE(grid->step_processor()->is_pipeline_ready());
        EXPECT_TRUE(grid->emit_processor()->is_pipeline_ready());
        EXPECT_NEAR(vertices(grid)[12].position.z, 0.0F, 1e-5F);
    }

    TEST_F(RelaxationGridDeviceTest, InterleavedGridsKeepTheirOwnStateBindings)
    {
        auto first = attach();
        seed(blinker());
        auto second = attach();
        std::vector<uint32_t> empty(25U, 0U);
        seed(empty);

        first->process_default();
        second->process_default();
        first->process_default();
        second->process_default();

        EXPECT_EQ(front_state<uint32_t>(first), blinker());
        EXPECT_EQ(front_state<uint32_t>(second), empty);
        first->get_processing_chain()->process(first);
        second->get_processing_chain()->process(second);
        EXPECT_NEAR(vertices(first)[11].position.z, 0.0F, 1e-5F);
        EXPECT_NEAR(vertices(second)[11].position.z, 2.0F, 1e-5F);
    }

    TEST_F(RelaxationGridDeviceTest, DeferredStepDeliversSnapshotAfterDispatchCompletion)
    {
        attach();
        seed(blinker());
        auto step_processor = grid->step_processor();
        step_processor->set_deferred_submission(true);
        grid->request_snapshot();

        step();
        EXPECT_EQ(grid->front_index(), 1U);
        EXPECT_FALSE(grid->snapshot_source()->has_pending());
        EXPECT_TRUE(step_processor->resolve_pending_dispatch(true));
        EXPECT_TRUE(grid->snapshot_source()->has_pending());
        auto snapshot = grid->snapshot_source()->next();
        ASSERT_TRUE(snapshot.await_ready());
        const auto bytes = snapshot.await_resume();
        ASSERT_EQ(bytes.size(), grid->get_state_bytes());
        std::vector<uint32_t> cells(grid->get_cell_count());
        std::memcpy(cells.data(), bytes.data(), bytes.size());
        EXPECT_EQ(cells, front_state<uint32_t>(grid));
    }

#if defined(MAYAFLUX_TEST_NATIVE_WINDOWS)

    class RelaxationGridPresentationTest : public PresentingVulkanTest {
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

            auto config = conway_config();
            config.point_size = 24.0F;
            grid = std::make_shared<RelaxationGridBuffer>(config);
            service->initialize_buffer(grid);
            ASSERT_TRUE(grid->is_initialized());
            grid->setup_processors(ProcessingToken::GRAPHICS_BACKEND);
            grid->setup_rendering({ .target_window = window });
            grid->step_processor()->set_step_predicate([] { return false; });
            std::vector<uint32_t> cells(25U, 0U);
            cells[12] = 1U;
            grid->seed_state(cells.data(), cells.size() * sizeof(uint32_t));

            root = std::make_shared<Buffers::RootGraphicsBuffer>();
            root->initialize();
            root->add_child_buffer(grid);
        }

        void TearDown() override
        {
            if (grid) {
                if (auto render = grid->get_render_processor()) {
                    if (auto chain = grid->get_processing_chain())
                        chain->remove_processor_anywhere(render, grid);
                }
                grid->set_default_processor(nullptr);
            }
            root.reset();
            if (grid && grid->is_initialized() && service)
                service->destroy_buffer(grid);
            grid.reset();
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
        std::shared_ptr<RelaxationGridBuffer> grid;
        std::shared_ptr<Buffers::RootGraphicsBuffer> root;
        Registry::Service::BufferService* service {};
        bool foundry_ready {};
        bool compute_ready {};
        bool flow_ready {};
    };

    TEST_F(RelaxationGridPresentationTest, SeededPointIsVisibleAfterPresentation)
    {
        root->process_default();
        Buffers::PresentProcessor presenter;
        presenter.processing_function(root);

        const auto pixel = center_pixel();
        EXPECT_GT(pixel[0], 20U);
        EXPECT_GT(pixel[1], 20U);
        EXPECT_GT(pixel[2], 20U);
    }

#endif

}
}
