#include "../../test_config.h"

#include "../../support/native_window_test_support.hpp"
#include "../../support/scripted_window.hpp"
#include "../../support/vulkan_test_support.hpp"

#include "MayaFlux/Buffers/BufferManager.hpp"
#include "MayaFlux/Buffers/BufferProcessingChain.hpp"
#include "MayaFlux/Buffers/Forma/FormaBuffer.hpp"
#include "MayaFlux/Buffers/Forma/FormaProcessor.hpp"
#include "MayaFlux/Buffers/Shaders/RenderProcessor.hpp"
#include "MayaFlux/Core/Windowing/WindowManager.hpp"
#include "MayaFlux/Kakshya/NDData/VertexFormats.hpp"
#include "MayaFlux/Nodes/NodeGraphManager.hpp"
#include "MayaFlux/Portal/Forma/Context.hpp"
#include "MayaFlux/Portal/Forma/Internal/Atelier.hpp"
#include "MayaFlux/Portal/Forma/Primitives/FormFactory.hpp"
#include "MayaFlux/Portal/Graphics/ShaderFoundry.hpp"
#include "MayaFlux/Registry/BackendRegistry.hpp"
#include "MayaFlux/Registry/Service/BufferService.hpp"
#include "MayaFlux/Transitive/Memory/Persist.hpp"
#include "MayaFlux/Vruta/EventManager.hpp"
#include "MayaFlux/Vruta/Scheduler.hpp"

namespace MayaFlux::Test {

namespace Forma = Portal::Forma;
using Kinesis::AABB2D;
using Portal::Graphics::PrimitiveTopology;

namespace {

    struct AtelierHarness {
        std::shared_ptr<Vruta::TaskScheduler> scheduler = std::make_shared<Vruta::TaskScheduler>(TestConfig::SAMPLE_RATE);
        std::shared_ptr<Nodes::NodeGraphManager> nodes = std::make_shared<Nodes::NodeGraphManager>();
        std::shared_ptr<Buffers::BufferManager> buffers = std::make_shared<Buffers::BufferManager>();
        std::shared_ptr<Vruta::EventManager> events = std::make_shared<Vruta::EventManager>();

        uint64_t frame {};

        bool up(std::shared_ptr<Core::WindowManager> windows = nullptr)
        {
            Forma::internal::atelier().shutdown();
            scheduler->register_token_processor(Vruta::ProcessingToken::FRAME_ACCURATE,
                [this](const std::vector<std::shared_ptr<Vruta::Routine>>& tasks, uint64_t units) {
                    for (uint64_t i = 0; i < units; ++i) {
                        for (const auto& routine : tasks) {
                            if (!routine || !routine->is_active())
                                continue;
                            if (!routine->requires_clock_sync() || frame + i >= routine->next_execution())
                                routine->try_resume(frame + i);
                        }
                    }
                });
            return Forma::internal::atelier().initialize(nodes, buffers, scheduler, events, std::move(windows));
        }

        void tick_frames(uint64_t count)
        {
            scheduler->process_token(Vruta::ProcessingToken::FRAME_ACCURATE, count);
            frame += count;
        }

        void down()
        {
            Forma::internal::atelier().shutdown();
            MayaFlux::internal::cleanup_persistent_store();
        }
    };

    Forma::GeometryFn<int> counting_geometry(std::shared_ptr<int> calls, std::optional<AABB2D> hint = {})
    {
        return [calls = std::move(calls), hint](int, std::vector<uint8_t>& bytes, Forma::Element& element) {
            ++*calls;
            bytes.assign(sizeof(Kakshya::MeshVertex) * 4, 0);
            if (hint)
                element.bounds_hint = hint;
        };
    }

    const AABB2D k_hint { .min = { -0.5F, -0.5F }, .max = { 0.5F, 0.5F } };

}

class FormaAtelierLifecycleTest : public ::testing::Test {
protected:
    void TearDown() override { harness.down(); }

    AtelierHarness harness;
    std::shared_ptr<ScriptedWindow> window = std::make_shared<ScriptedWindow>(200U, 100U);
};

// =============================================================================
// Lifecycle
// =============================================================================

TEST_F(FormaAtelierLifecycleTest, UninitializedAtelierRefusesEveryConstructionPath)
{
    Forma::internal::atelier().shutdown();

    EXPECT_FALSE(Forma::internal::atelier().is_initialized());
    EXPECT_THROW((void)Forma::internal::atelier().bridge(), std::runtime_error);
    EXPECT_THROW((void)Forma::internal::atelier().inspector(), std::runtime_error);
    EXPECT_THROW((void)Forma::internal::atelier().create_window({}), std::runtime_error);
    EXPECT_THROW((void)Forma::internal::atelier().create_buffer(window, 256, PrimitiveTopology::POINT_LIST),
        std::runtime_error);
}

TEST_F(FormaAtelierLifecycleTest, InitializeExposesAStableBridgeAndInspector)
{
    ASSERT_TRUE(harness.up());

    auto& atelier = Forma::internal::atelier();
    EXPECT_TRUE(atelier.is_initialized());
    EXPECT_EQ(&atelier.bridge(), &atelier.bridge());
    EXPECT_EQ(&atelier.inspector(), &atelier.inspector());
}

TEST_F(FormaAtelierLifecycleTest, SecondInitializeKeepsTheFirstSetOfManagers)
{
    ASSERT_TRUE(harness.up());
    auto* first_bridge = &Forma::internal::atelier().bridge();

    AtelierHarness other;
    EXPECT_TRUE(Forma::internal::atelier().initialize(
        other.nodes, other.buffers, other.scheduler, other.events, nullptr));

    EXPECT_EQ(&Forma::internal::atelier().bridge(), first_bridge);
    EXPECT_EQ(other.scheduler.use_count(), 1);
    EXPECT_EQ(other.buffers.use_count(), 1);
}

TEST_F(FormaAtelierLifecycleTest, ShutdownReleasesTheHeldManagers)
{
    ASSERT_TRUE(harness.up());
    EXPECT_GT(harness.scheduler.use_count(), 1);
    EXPECT_GT(harness.buffers.use_count(), 1);
    EXPECT_GT(harness.events.use_count(), 1);
    EXPECT_GT(harness.nodes.use_count(), 1);

    Forma::internal::atelier().shutdown();

    EXPECT_FALSE(Forma::internal::atelier().is_initialized());
    EXPECT_EQ(harness.scheduler.use_count(), 1);
    EXPECT_EQ(harness.buffers.use_count(), 1);
    EXPECT_EQ(harness.events.use_count(), 1);
    EXPECT_EQ(harness.nodes.use_count(), 1);
    EXPECT_THROW((void)Forma::internal::atelier().bridge(), std::runtime_error);
}

TEST_F(FormaAtelierLifecycleTest, ShutdownIsRepeatableAndInitializeWorksAgain)
{
    ASSERT_TRUE(harness.up());

    Forma::internal::atelier().shutdown();
    Forma::internal::atelier().shutdown();
    EXPECT_FALSE(Forma::internal::atelier().is_initialized());

    ASSERT_TRUE(harness.up());
    EXPECT_TRUE(Forma::internal::atelier().is_initialized());
    EXPECT_NO_THROW((void)Forma::internal::atelier().bridge());
}

// =============================================================================
// Layers and surfaces
// =============================================================================

TEST_F(FormaAtelierLifecycleTest, CreateLayerReturnsAnEmptyLayerWithAContext)
{
    ASSERT_TRUE(harness.up());

    auto [layer, ctx] = Forma::internal::atelier().create_layer(window, "atelier-layer");

    ASSERT_NE(layer, nullptr);
    ASSERT_NE(ctx, nullptr);
    EXPECT_TRUE(layer->empty());
}

TEST_F(FormaAtelierLifecycleTest, CreateLayerKeepsTheContextAliveForTheProcess)
{
    ASSERT_TRUE(harness.up());

    std::weak_ptr<Forma::Context> weak;
    {
        auto [layer, ctx] = Forma::internal::atelier().create_layer(window, "atelier-persist");
        weak = ctx;
    }

    EXPECT_FALSE(weak.expired());
}

TEST_F(FormaAtelierLifecycleTest, CreateSurfaceBuildsAMissingLayerAndContext)
{
    ASSERT_TRUE(harness.up());

    Forma::SurfaceConfig config;
    config.window = window;
    config.name = "atelier-built";
    config.detach_on_close = false;
    auto surface = Forma::internal::atelier().create_surface(std::move(config));

    EXPECT_NE(surface.layer_ptr(), nullptr);
    EXPECT_NE(surface.ctx_ptr(), nullptr);
    EXPECT_EQ(surface.window(), window);
    EXPECT_TRUE(surface.layer().empty());
}

TEST_F(FormaAtelierLifecycleTest, CreateSurfaceUsesASuppliedLayerAndContextAsIs)
{
    ASSERT_TRUE(harness.up());

    auto layer = std::make_shared<Forma::Layer>();
    auto ctx = std::make_shared<Forma::Context>(layer, window, *harness.events, "supplied");
    Forma::SurfaceConfig config;
    config.window = window;
    config.layer = layer;
    config.ctx = ctx;
    config.detach_on_close = false;
    auto surface = Forma::internal::atelier().create_surface(std::move(config));

    EXPECT_EQ(surface.layer_ptr(), layer);
    EXPECT_EQ(surface.ctx_ptr(), ctx);
}

TEST_F(FormaAtelierLifecycleTest, CreateSurfaceBuildsAContextAroundASuppliedLayer)
{
    ASSERT_TRUE(harness.up());

    auto layer = std::make_shared<Forma::Layer>();
    Forma::SurfaceConfig config;
    config.window = window;
    config.name = "atelier-lone-layer";
    config.layer = layer;
    config.detach_on_close = false;
    auto surface = Forma::internal::atelier().create_surface(std::move(config));

    EXPECT_EQ(surface.layer_ptr(), layer);
    ASSERT_NE(surface.ctx_ptr(), nullptr);
    EXPECT_EQ(surface.ctx_ptr()->layer(), layer);
}

TEST_F(FormaAtelierLifecycleTest, CreateSurfaceTakesTheLayerOfASuppliedContext)
{
    ASSERT_TRUE(harness.up());

    auto layer = std::make_shared<Forma::Layer>();
    auto ctx = std::make_shared<Forma::Context>(layer, window, *harness.events, "atelier-lone-ctx");
    Forma::SurfaceConfig config;
    config.window = window;
    config.ctx = ctx;
    config.detach_on_close = false;
    auto surface = Forma::internal::atelier().create_surface(std::move(config));

    EXPECT_EQ(surface.ctx_ptr(), ctx);
    EXPECT_EQ(surface.layer_ptr(), layer);
}

TEST_F(FormaAtelierLifecycleTest, PlacingAnEmptySeriesSpecAddsNoAdornments)
{
    ASSERT_TRUE(harness.up());

    Forma::SurfaceConfig config;
    config.window = window;
    config.name = "atelier-adornments";
    config.detach_on_close = false;
    auto surface = Forma::internal::atelier().create_surface(std::move(config));

    Forma::Plot::SeriesSpec spec;
    Forma::internal::atelier().place_adornments(surface, spec, 0);

    EXPECT_TRUE(surface.layer().empty());
}

// =============================================================================
// Buffers and elements (device)
// =============================================================================

class FormaAtelierBufferTest : public VulkanDeviceTest {
protected:
    static void SetUpTestSuite()
    {
        VulkanDeviceTest::SetUpTestSuite();
        if (!vulkan_unavailable_reason().empty())
            return;

        auto borrowed = std::shared_ptr<Core::VulkanBackend>(&backend(), [](Core::VulkanBackend*) {});
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
        ASSERT_TRUE(harness.up());
    }

    void TearDown() override
    {
        harness.down();

        auto* service = Registry::BackendRegistry::instance()
                            .get_service<Registry::Service::BufferService>();
        for (auto& buffer : created) {
            harness.buffers->remove_buffer(buffer, Buffers::ProcessingToken::GRAPHICS_BACKEND);
            if (auto render = buffer->get_render_processor())
                buffer->get_processing_chain()->remove_processor_anywhere(render, buffer);
            buffer->set_default_processor(nullptr);
            if (service && buffer->is_initialized())
                service->destroy_buffer(buffer);
        }
        created.clear();
    }

    std::shared_ptr<Buffers::FormaBuffer> make_buffer(
        size_t capacity,
        PrimitiveTopology topology,
        const std::string& texture_binding = {},
        std::vector<std::pair<std::string, std::shared_ptr<Core::VKImage>>> additional = {})
    {
        auto buffer = Forma::internal::atelier().create_buffer(
            window, capacity, topology, texture_binding, std::move(additional));
        created.push_back(buffer);
        return buffer;
    }

    AtelierHarness harness;
    std::shared_ptr<ScriptedWindow> window = std::make_shared<ScriptedWindow>(200U, 100U);
    std::vector<std::shared_ptr<Buffers::FormaBuffer>> created;

private:
    static inline bool s_foundry_ready {};
};

TEST_F(FormaAtelierBufferTest, CreateBufferRegistersWithTheGraphicsUnit)
{
    auto buffer = make_buffer(4096, PrimitiveTopology::TRIANGLE_STRIP);

    ASSERT_NE(buffer, nullptr);
    EXPECT_TRUE(buffer->is_initialized());
    EXPECT_EQ(buffer->get_size_bytes(), 4096U);
    EXPECT_EQ(buffer->topology(), PrimitiveTopology::TRIANGLE_STRIP);

    const auto& registered = harness.buffers->get_graphics_buffers(Buffers::ProcessingToken::GRAPHICS_BACKEND);
    EXPECT_NE(std::ranges::find(registered, buffer), registered.end());
}

TEST_F(FormaAtelierBufferTest, CreateBufferInstallsTheFormaProcessorAndTheRenderProcessor)
{
    auto buffer = make_buffer(4096, PrimitiveTopology::TRIANGLE_LIST);

    ASSERT_NE(buffer, nullptr);
    EXPECT_NE(std::dynamic_pointer_cast<Buffers::FormaProcessor>(buffer->get_default_processor()), nullptr);

    const auto render = buffer->get_render_processor();
    ASSERT_NE(render, nullptr);
    ASSERT_NE(buffer->get_processing_chain(), nullptr);
    EXPECT_EQ(buffer->get_processing_chain()->get_final_processor(buffer), render);
}

TEST_F(FormaAtelierBufferTest, CreateBufferWithoutTexturesBindsNoTextureSlot)
{
    auto buffer = make_buffer(4096, PrimitiveTopology::TRIANGLE_STRIP);

    ASSERT_NE(buffer->get_render_processor(), nullptr);
    EXPECT_TRUE(buffer->get_render_processor()->get_config().bindings.empty());
}

TEST_F(FormaAtelierBufferTest, CreateBufferWithATextureBindingDeclaresIt)
{
    auto buffer = make_buffer(4096, PrimitiveTopology::TRIANGLE_STRIP, "texSampler");

    ASSERT_NE(buffer->get_render_processor(), nullptr);
    const auto& bindings = buffer->get_render_processor()->get_config().bindings;
    ASSERT_TRUE(bindings.contains("texSampler"));
    EXPECT_EQ(bindings.at("texSampler").count, 1U);
}

TEST_F(FormaAtelierBufferTest, CreateBufferWithAdditionalTexturesDeclaresOneArrayBinding)
{
    auto buffer = make_buffer(4096, PrimitiveTopology::TRIANGLE_LIST, {},
        { { "first", nullptr }, { "second", nullptr } });

    ASSERT_NE(buffer->get_render_processor(), nullptr);
    const auto& bindings = buffer->get_render_processor()->get_config().bindings;
    ASSERT_TRUE(bindings.contains("textures"));
    EXPECT_EQ(bindings.at("textures").count, 2U);
}

TEST_F(FormaAtelierBufferTest, CreateElementOnALayerRegistersItAndSyncsOnTheFrameTick)
{
    auto calls = std::make_shared<int>(0);
    auto owner = std::make_shared<Forma::Layer>();
    Forma::Layer& layer = *owner;

    auto mapped = Forma::internal::atelier().create_element<int>(
        layer, window, counting_geometry(calls), 1);
    created.push_back(mapped.element.buffer.buffer());

    EXPECT_EQ(layer.size(), 1U);
    EXPECT_NE(layer.get(mapped.element.id), nullptr);
    EXPECT_EQ(mapped.state->value, 1);
    EXPECT_EQ(*calls, 0);

    harness.tick_frames(2);
    EXPECT_EQ(*calls, 1);

    mapped.state->write(7);
    harness.tick_frames(2);
    EXPECT_EQ(*calls, 2);
}

TEST_F(FormaAtelierBufferTest, CreateElementOnASurfaceSyncsOnceBeforeTheFirstFrame)
{
    Forma::SurfaceConfig config;
    config.window = window;
    config.name = "atelier-element";
    config.detach_on_close = false;
    auto surface = Forma::internal::atelier().create_surface(std::move(config));
    auto calls = std::make_shared<int>(0);

    auto mapped = Forma::internal::atelier().create_element<int>(
        surface, counting_geometry(calls, k_hint), 3);
    created.push_back(mapped.element.buffer.buffer());

    EXPECT_EQ(*calls, 1);
    EXPECT_EQ(surface.layer().hit_test({ 0.0F, 0.0F }), mapped.element.id);
    EXPECT_FALSE(surface.layer().hit_test({ 0.9F, 0.9F }).has_value());
}

TEST_F(FormaAtelierBufferTest, TrackReflowsTheMappedGeometryOnEveryScroll)
{
    Forma::SurfaceConfig config;
    config.window = window;
    config.name = "atelier-track";
    config.detach_on_close = false;
    auto surface = Forma::internal::atelier().create_surface(std::move(config));

    auto panel_buffer = std::make_shared<Buffers::FormaBuffer>(4096, PrimitiveTopology::TRIANGLE_STRIP);
    auto panel = Forma::make_scrollable(std::move(panel_buffer), surface, k_hint, glm::vec3(0.1F), 0.1F);

    const AABB2D content { .min = { -0.4F, -1.5F }, .max = { 0.4F, 1.5F } };
    auto calls = std::make_shared<int>(0);
    auto mapped = Forma::internal::atelier().create_element<int>(
        surface, counting_geometry(calls, content), 0);
    created.push_back(mapped.element.buffer.buffer());
    ASSERT_EQ(*calls, 1);

    Forma::internal::atelier().track(panel, surface.layer(), mapped, content);
    panel.scroll_by(surface.layer(), { 0.0F, 0.3F });

    EXPECT_EQ(*calls, 2);

    panel.scroll_by(surface.layer(), { 0.0F, -0.1F });

    EXPECT_EQ(*calls, 3);
}

// =============================================================================
// Windows (native display)
// =============================================================================

#if defined(MAYAFLUX_TEST_NATIVE_WINDOWS)

class FormaAtelierWindowTest : public PresentingVulkanTest {
protected:
    void SetUp() override
    {
        PresentingVulkanTest::SetUp();
        if (IsSkipped())
            return;
        ASSERT_TRUE(harness.up(std::shared_ptr<Core::WindowManager>(&manager(), [](Core::WindowManager*) { })));
    }

    void TearDown() override
    {
        harness.down();
        PresentingVulkanTest::TearDown();
    }

    AtelierHarness harness;
};

TEST_F(FormaAtelierWindowTest, CreateWindowRegistersAWindowWithTheManager)
{
    auto info = native_window_info("atelier-window");
    const size_t before = manager().window_count();

    auto window = Forma::internal::atelier().create_window(info);
    ASSERT_NE(window, nullptr);
    windows.push_back(window);

    EXPECT_EQ(manager().window_count(), before + 1);
    EXPECT_EQ(manager().find_window("atelier-window"), window);
}

TEST_F(FormaAtelierWindowTest, CreateSurfaceFromWindowInfoBuildsTheWindowAndTheSurface)
{
    auto info = native_window_info("atelier-surface-window");

    auto surface = Forma::internal::atelier().create_surface(info, "atelier-surface");
    ASSERT_NE(surface.window(), nullptr);
    windows.push_back(surface.window());

    EXPECT_EQ(manager().find_window("atelier-surface-window"), surface.window());
    EXPECT_NE(surface.layer_ptr(), nullptr);
    EXPECT_NE(surface.ctx_ptr(), nullptr);
}

#endif

}
