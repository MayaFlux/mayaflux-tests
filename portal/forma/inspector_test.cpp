#include "../../test_config.h"

#include "../../support/frame_scheduler.hpp"
#include "../../support/scripted_window.hpp"
#include "../../support/text_test_support.hpp"
#include "../../support/vulkan_test_support.hpp"

#include "MayaFlux/Buffers/BufferManager.hpp"
#include "MayaFlux/Buffers/BufferProcessingChain.hpp"
#include "MayaFlux/Buffers/Forma/FormaBuffer.hpp"
#include "MayaFlux/Buffers/Root/RootAudioBuffer.hpp"
#include "MayaFlux/Buffers/Shaders/RenderProcessor.hpp"
#include "MayaFlux/Kriya/Awaiters/BroadcastAwaiter.hpp"
#include "MayaFlux/Kriya/Awaiters/DelayAwaiters.hpp"
#include "MayaFlux/Kriya/Awaiters/GetPromise.hpp"
#include "MayaFlux/Nodes/Conduit/Constant.hpp"
#include "MayaFlux/Nodes/Generators/Sine.hpp"
#include "MayaFlux/Nodes/NodeGraphManager.hpp"
#include "MayaFlux/Portal/Forma/Context.hpp"
#include "MayaFlux/Portal/Forma/Inspect/Inspector.hpp"
#include "MayaFlux/Portal/Forma/Surface.hpp"
#include "MayaFlux/Portal/Graphics/ShaderFoundry.hpp"
#include "MayaFlux/Portal/Graphics/TextureLoom.hpp"
#include "MayaFlux/Portal/Text/TypeFaceFoundry.hpp"
#include "MayaFlux/Registry/BackendRegistry.hpp"
#include "MayaFlux/Registry/Service/BufferService.hpp"
#include "MayaFlux/Vruta/BroadcastSource.hpp"
#include "MayaFlux/Vruta/EventManager.hpp"

namespace MayaFlux::Test {

namespace Forma = Portal::Forma;
using Forma::InspectResult;
using Portal::Graphics::PrimitiveTopology;

namespace {

    Vruta::GraphicsRoutine idle_graphics()
    {
        auto& promise = co_await Kriya::GetGraphicsPromise {};
        while (!promise.should_terminate)
            co_await Kriya::FrameDelay { .frames_to_wait = 1 };
    }

    Vruta::Event idle_listener(std::shared_ptr<Vruta::BroadcastSource<int>> source)
    {
        auto& promise = co_await Kriya::GetEventPromise {};
        while (!promise.should_terminate)
            co_await source->next();
    }

    bool contains(const std::vector<uint32_t>& ids, uint32_t id)
    {
        return std::ranges::find(ids, id) != ids.end();
    }

}

class FormaInspectorTest : public VulkanDeviceTest {
protected:
    static void SetUpTestSuite()
    {
        VulkanDeviceTest::SetUpTestSuite();
        if (!vulkan_unavailable_reason().empty())
            return;

        auto borrowed = std::shared_ptr<Core::VulkanBackend>(&backend(), [](Core::VulkanBackend*) { });
        Portal::Graphics::get_shader_foundry().shutdown();
        Portal::Graphics::get_texture_manager().shutdown();
        s_ready = Portal::Graphics::get_shader_foundry().initialize(borrowed)
            && Portal::Graphics::get_texture_manager().initialize(borrowed);
    }

    static void TearDownTestSuite()
    {
        if (s_ready) {
            Portal::Graphics::get_texture_manager().shutdown();
            Portal::Graphics::get_shader_foundry().shutdown();
        }
        s_ready = false;
        VulkanDeviceTest::TearDownTestSuite();
    }

    void SetUp() override
    {
        VulkanDeviceTest::SetUp();
        if (IsSkipped())
            return;
        ASSERT_TRUE(s_ready);

        auto& foundry = Portal::Text::TypeFaceFoundry::instance();
        m_owns_text = !foundry.is_initialized();
        ASSERT_TRUE(foundry.initialize());
        const auto& path = system_font_path();
        ASSERT_TRUE(path.has_value()) << "no system font could be resolved on this host";
        ASSERT_TRUE(foundry.set_default_font(*path, 16, 512));
    }

    void TearDown() override
    {
        for (auto* result : persistent)
            inspector.destroy(*result);
        persistent.clear();

        auto* service = Registry::BackendRegistry::instance()
                            .get_service<Registry::Service::BufferService>();
        const auto buffers = rig_buffers->get_graphics_buffers(Buffers::ProcessingToken::GRAPHICS_BACKEND);
        for (const auto& buffer : buffers) {
            rig_buffers->remove_buffer(buffer, Buffers::ProcessingToken::GRAPHICS_BACKEND);
            if (auto render = buffer->get_render_processor(); render && buffer->get_processing_chain())
                buffer->get_processing_chain()->remove_processor_anywhere(render, buffer);
            buffer->set_default_processor(nullptr);
            if (service && buffer->is_initialized())
                service->destroy_buffer(buffer);
        }

        if (m_owns_text)
            Portal::Text::TypeFaceFoundry::instance().shutdown();
    }

    InspectResult& keep(InspectResult& result)
    {
        persistent.push_back(&result);
        return result;
    }

    std::shared_ptr<Buffers::FormaBuffer> add_forma_buffer()
    {
        auto buffer = std::make_shared<Buffers::FormaBuffer>(256, PrimitiveTopology::POINT_LIST);
        rig_buffers->add_buffer(buffer, Buffers::ProcessingToken::GRAPHICS_BACKEND);
        buffer->setup_rendering({ .target_window = window });
        return buffer;
    }

    void add_idle_tasks(size_t count)
    {
        for (size_t i = 0; i < count; ++i) {
            frames.scheduler->add_task(
                std::make_shared<Vruta::GraphicsRoutine>(idle_graphics()), "idle_" + std::to_string(i), false);
        }
        frames.tick();
    }

    FrameScheduler frames;
    std::shared_ptr<Nodes::NodeGraphManager> nodes = std::make_shared<Nodes::NodeGraphManager>();
    std::shared_ptr<Buffers::BufferManager> rig_buffers = std::make_shared<Buffers::BufferManager>();
    Vruta::EventManager events;
    std::shared_ptr<ScriptedWindow> window = std::make_shared<ScriptedWindow>(400U, 300U);
    std::shared_ptr<Forma::Layer> layer = std::make_shared<Forma::Layer>();
    std::shared_ptr<Forma::Context> ctx = std::make_shared<Forma::Context>(layer, window, events, "inspect");
    Forma::Surface surface { Forma::SurfaceConfig { .window = window, .layer = layer, .ctx = ctx, .detach_on_close = false } };
    Forma::LayoutCursor cursor;
    Forma::Inspector inspector { *nodes, *rig_buffers, *frames.scheduler, events };
    std::vector<InspectResult*> persistent;

private:
    bool m_owns_text {};
    static inline bool s_ready {};
};

// =============================================================================
// Nodes
// =============================================================================

TEST_F(FormaInspectorTest, ANodeShowsItsOutputRowAndRelatesTheRowToTheHeader)
{
    auto node = std::make_shared<Nodes::Constant>(1.0);

    auto result = inspector.node(node, surface, cursor);

    ASSERT_EQ(result.group.rows.size(), 1U);
    EXPECT_TRUE(result.children.empty());
    EXPECT_NE(layer->get(result.group.header.header_id), nullptr);
    EXPECT_NE(layer->get(result.group.rows.front().element_id), nullptr);
    EXPECT_TRUE(contains(layer->related_ids(result.group.header.header_id), result.group.rows.front().element_id));
}

TEST_F(FormaInspectorTest, ANodeExpandsEachModulatorAsARelatedChild)
{
    auto inner = std::make_shared<Nodes::Generator::Sine>(110.F);
    auto middle = std::make_shared<Nodes::Generator::Sine>(inner, 220.F);
    auto outer = std::make_shared<Nodes::Generator::Sine>(middle, 440.F);

    auto result = inspector.node(outer, surface, cursor);

    ASSERT_EQ(result.children.size(), 1U);
    ASSERT_EQ(result.children.front().children.size(), 1U);
    EXPECT_TRUE(result.children.front().children.front().children.empty());
    EXPECT_TRUE(contains(layer->related_ids(result.group.header.header_id),
        result.children.front().group.header.header_id));
    EXPECT_TRUE(contains(layer->related_ids(result.children.front().group.header.header_id),
        result.children.front().children.front().group.header.header_id));
}

TEST_F(FormaInspectorTest, ADepthIndentsTheHeaderByTheInspectIndentPerLevel)
{
    auto node = std::make_shared<Nodes::Constant>(1.0);
    Forma::LayoutCursor other;

    auto shallow = inspector.node(node, surface, cursor, -0.95F, 0.95F, 0.05F, 0);
    auto deep = inspector.node(node, surface, other, -0.95F, 0.95F, 0.05F, 2);

    EXPECT_NEAR(deep.group.header.bounds().min.x - shallow.group.header.bounds().min.x,
        2.F * Forma::k_inspect_indent, 1e-5F);
}

TEST_F(FormaInspectorTest, EachInspectedGroupAdvancesTheSharedCursor)
{
    auto node = std::make_shared<Nodes::Constant>(1.0);
    const float start = cursor.y();

    auto first = inspector.node(node, surface, cursor);
    const float after_first = cursor.y();
    auto second = inspector.node(node, surface, cursor);

    EXPECT_LT(after_first, start);
    EXPECT_LT(cursor.y(), after_first);
    EXPECT_LE(second.group.header.bounds().max.y, first.group.header.bounds().min.y);
}

TEST_F(FormaInspectorTest, ARootNodeListsEveryRegisteredNode)
{
    nodes->add_to_root(std::make_shared<Nodes::Constant>(1.0), Nodes::ProcessingToken::AUDIO_RATE, 0);
    nodes->add_to_root(std::make_shared<Nodes::Constant>(2.0), Nodes::ProcessingToken::AUDIO_RATE, 0);
    nodes->process_token(Nodes::ProcessingToken::AUDIO_RATE, 1);

    auto result = inspector.root_node(Nodes::ProcessingToken::AUDIO_RATE, 0, surface, cursor);

    EXPECT_EQ(result.group.rows.size(), 1U);
    EXPECT_EQ(result.children.size(), 2U);
}

TEST_F(FormaInspectorTest, AllAudioRootNodesExpandOneResultPerRegisteredChannel)
{
    nodes->add_to_root(std::make_shared<Nodes::Constant>(1.0), Nodes::ProcessingToken::AUDIO_RATE, 2);
    nodes->add_to_root(std::make_shared<Nodes::Constant>(2.0), Nodes::ProcessingToken::AUDIO_RATE, 0);
    nodes->process_token(Nodes::ProcessingToken::AUDIO_RATE, 1);

    auto result = inspector.root_node(Nodes::ProcessingToken::AUDIO_RATE, surface, cursor);

    EXPECT_EQ(result.group.rows.size(), 1U);
    ASSERT_EQ(result.children.size(), 2U);
    EXPECT_EQ(result.children[0].children.size(), 1U);
    EXPECT_EQ(result.children[1].children.size(), 1U);
}

TEST_F(FormaInspectorTest, ANullBufferYieldsAnEmptyResult)
{
    auto result = inspector.buffer(nullptr, surface, cursor);

    EXPECT_TRUE(result.children.empty());
    EXPECT_TRUE(result.group.rows.empty());
}

TEST_F(FormaInspectorTest, ANonAudioRootNodeDelegatesToChannelZero)
{
    nodes->add_to_root(std::make_shared<Nodes::Constant>(1.0), Nodes::ProcessingToken::VISUAL_RATE, 0);
    nodes->process_token(Nodes::ProcessingToken::VISUAL_RATE, 1);

    auto result = inspector.root_node(Nodes::ProcessingToken::VISUAL_RATE, surface, cursor);

    EXPECT_EQ(result.group.rows.size(), 1U);
    EXPECT_EQ(result.children.size(), 1U);
}

TEST_F(FormaInspectorTest, TheNodeGraphManagerResultIsBuiltOnceAndReused)
{
    nodes->add_to_root(std::make_shared<Nodes::Constant>(1.0), Nodes::ProcessingToken::AUDIO_RATE, 0);

    auto& first = keep(inspector.node_graph_manager(surface, cursor));
    const size_t built = layer->size();
    Forma::LayoutCursor other_cursor;
    auto& second = inspector.node_graph_manager(surface, other_cursor);

    EXPECT_EQ(&first, &second);
    EXPECT_EQ(layer->size(), built);
    EXPECT_EQ(first.children.size(), nodes->get_active_tokens().size());
    EXPECT_GT(built, 0U);
}

TEST_F(FormaInspectorTest, DestroyingAPersistentResultMakesTheNextCallRebuild)
{
    nodes->add_to_root(std::make_shared<Nodes::Constant>(1.0), Nodes::ProcessingToken::AUDIO_RATE, 0);
    auto& first = inspector.node_graph_manager(surface, cursor);
    const size_t built = layer->size();

    inspector.destroy(first);
    auto& rebuilt = keep(inspector.node_graph_manager(surface, cursor));

    EXPECT_GT(layer->size(), built);
    EXPECT_FALSE(rebuilt.children.empty());
}

TEST_F(FormaInspectorTest, DestroyingACallerOwnedResultResetsIt)
{
    auto result = inspector.node(std::make_shared<Nodes::Constant>(1.0), surface, cursor);
    ASSERT_FALSE(result.group.rows.empty());

    inspector.destroy(result);

    EXPECT_TRUE(result.group.rows.empty());
    EXPECT_TRUE(result.children.empty());
}

TEST_F(FormaInspectorTest, TappingEveryRowSurvivesAChangedValue)
{
    auto node = std::make_shared<Nodes::Constant>(1.0);
    auto outer = std::make_shared<Nodes::Generator::Sine>(node, 440.F);
    auto result = inspector.node(outer, surface, cursor);
    result.tap_all();

    node->set_constant(5.0);
    node->process_sample(0.0);

    EXPECT_NO_THROW(result.tap_all());
}

// =============================================================================
// Buffers
// =============================================================================

TEST_F(FormaInspectorTest, ABufferListsItsDefaultProcessorAndEveryChainSlot)
{
    auto buffer = add_forma_buffer();
    auto chain = buffer->get_processing_chain();
    ASSERT_NE(chain, nullptr);
    const size_t expected = 1 + 1 + chain->get_processors(buffer).size() + 1 + 1;

    auto result = inspector.buffer(buffer, surface, cursor);

    EXPECT_EQ(result.group.rows.size(), expected);
}

TEST_F(FormaInspectorTest, AnUnmanagedBufferShowsOnlyTheEmptyChainSlots)
{
    auto buffer = std::make_shared<Buffers::FormaBuffer>(256, PrimitiveTopology::POINT_LIST);
    ASSERT_NE(buffer->get_processing_chain(), nullptr);
    ASSERT_TRUE(buffer->get_processing_chain()->get_processors(buffer).empty());

    auto result = inspector.buffer(buffer, surface, cursor);

    EXPECT_EQ(result.group.rows.size(), 4U);
}

TEST_F(FormaInspectorTest, TheRootGraphicsBufferExpandsItselfAndEveryChild)
{
    add_forma_buffer();
    add_forma_buffer();

    auto result = inspector.root_graphics_buffer(Buffers::ProcessingToken::GRAPHICS_BACKEND, surface, cursor);

    EXPECT_EQ(result.group.rows.size(), 1U);
    EXPECT_EQ(result.children.size(), 3U);
}

TEST_F(FormaInspectorTest, ARootAudioBufferChannelShowsSamplesAndChildren)
{
    auto result = inspector.root_audio_buffer(Buffers::ProcessingToken::AUDIO_BACKEND, 0, surface, cursor);

    EXPECT_EQ(result.group.rows.size(), 2U);
    ASSERT_FALSE(result.children.empty());
    EXPECT_EQ(result.children.size(), 1U + rig_buffers->get_root_audio_buffer(Buffers::ProcessingToken::AUDIO_BACKEND, 0)->get_num_children());
}

TEST_F(FormaInspectorTest, AllRootAudioBuffersExpandOneResultPerChannel)
{
    const uint32_t channels = rig_buffers->get_num_channels(Buffers::ProcessingToken::AUDIO_BACKEND);
    ASSERT_GT(channels, 0U);

    auto result = inspector.root_audio_buffer(Buffers::ProcessingToken::AUDIO_BACKEND, surface, cursor);

    EXPECT_EQ(result.group.rows.size(), 1U);
    EXPECT_EQ(result.children.size(), channels);
}

TEST_F(FormaInspectorTest, TheBufferManagerResultCoversEveryActiveTokenAndIsReused)
{
    add_forma_buffer();

    auto& first = keep(inspector.buffer_manager(surface, cursor));
    const size_t built = layer->size();
    Forma::LayoutCursor other_cursor;
    auto& second = inspector.buffer_manager(surface, other_cursor);

    EXPECT_EQ(&first, &second);
    EXPECT_EQ(layer->size(), built);
    EXPECT_EQ(first.children.size(), rig_buffers->get_active_tokens().size());
}

// =============================================================================
// Scheduler
// =============================================================================

TEST_F(FormaInspectorTest, TheSchedulerResultListsEveryTaskAndIsReused)
{
    add_idle_tasks(2);

    auto& first = keep(inspector.scheduler(surface, cursor));
    const size_t built = layer->size();
    Forma::LayoutCursor other_cursor;
    auto& second = inspector.scheduler(surface, other_cursor);

    EXPECT_EQ(&first, &second);
    EXPECT_EQ(layer->size(), built);
    EXPECT_EQ(first.group.rows.size(), 1U);
    EXPECT_EQ(first.children.size(), frames.scheduler->get_all_tasks().size());
    for (const auto& child : first.children)
        EXPECT_EQ(child.group.rows.size(), 4U);
}

TEST_F(FormaInspectorTest, TasksAreFilteredByProcessingToken)
{
    add_idle_tasks(2);

    auto frame_tasks = inspector.tasks(Vruta::ProcessingToken::FRAME_ACCURATE, surface, cursor);
    auto sample_tasks = inspector.tasks(Vruta::ProcessingToken::SAMPLE_ACCURATE, surface, cursor);

    EXPECT_EQ(frame_tasks.children.size(), 2U);
    EXPECT_TRUE(sample_tasks.children.empty());
}

// =============================================================================
// Events
// =============================================================================

TEST_F(FormaInspectorTest, AnEventShowsItsTokenAndActiveRows)
{
    auto source = std::make_shared<Vruta::BroadcastSource<int>>();
    auto event = std::make_shared<Vruta::Event>(idle_listener(source));

    auto named = inspector.event(event, "listener", surface, cursor);
    auto unnamed = inspector.event(event, "", surface, cursor);

    EXPECT_EQ(named.group.rows.size(), 2U);
    EXPECT_EQ(unnamed.group.rows.size(), 2U);
}

TEST_F(FormaInspectorTest, TheEventManagerResultListsEveryEventAndIsReused)
{
    auto source = std::make_shared<Vruta::BroadcastSource<int>>();
    const size_t baseline = events.get_all_events().size();
    events.add_event(std::make_shared<Vruta::Event>(idle_listener(source)), "first");
    events.add_event(std::make_shared<Vruta::Event>(idle_listener(source)), "second");

    auto& first = keep(inspector.event_manager(surface, cursor));
    const size_t built = layer->size();
    Forma::LayoutCursor other_cursor;
    auto& second = inspector.event_manager(surface, other_cursor);

    EXPECT_EQ(&first, &second);
    EXPECT_EQ(layer->size(), built);
    EXPECT_EQ(first.children.size(), baseline + 2);
    for (const auto& child : first.children)
        EXPECT_EQ(child.group.rows.size(), 1U);
}

}
