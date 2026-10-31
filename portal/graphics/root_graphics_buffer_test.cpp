#include "gtest/gtest.h"

#include <stdexcept>

#include "../../support/native_window_test_support.hpp"

#include "MayaFlux/Buffers/BufferProcessingChain.hpp"
#include "MayaFlux/Buffers/Root/RootGraphicsBuffer.hpp"
#include "MayaFlux/Buffers/Shaders/RenderProcessor.hpp"
#include "MayaFlux/Buffers/VKBuffer.hpp"
#include "MayaFlux/Kakshya/NDData/VertexLayout.hpp"
#include "MayaFlux/Portal/Graphics/RenderFlow.hpp"
#include "MayaFlux/Registry/BackendRegistry.hpp"
#include "MayaFlux/Registry/Service/BufferService.hpp"
#include "MayaFlux/Registry/Service/DisplayService.hpp"

namespace MayaFlux::Test {
namespace {

    using Buffers::Buffer;
    using Buffers::BufferProcessingChain;
    using Buffers::BufferProcessor;
    using Buffers::GraphicsBatchProcessor;
    using Buffers::PresentProcessor;
    using Buffers::ProcessingToken;
    using Buffers::RootGraphicsBuffer;
    using Buffers::VKBuffer;

    class GraphicsActionProcessor final : public BufferProcessor {
    public:
        explicit GraphicsActionProcessor(std::function<void(const std::shared_ptr<Buffer>&)> action = {})
            : m_action(std::move(action))
        {
            m_processing_token = ProcessingToken::GRAPHICS_BACKEND;
        }

        void processing_function(const std::shared_ptr<Buffer>& buffer) override
        {
            if (m_action)
                m_action(buffer);
        }

    private:
        std::function<void(const std::shared_ptr<Buffer>&)> m_action;
    };

    std::shared_ptr<VKBuffer> vertex_buffer()
    {
        return std::make_shared<VKBuffer>(64, VKBuffer::Usage::VERTEX,
            Kakshya::DataModality::VERTEX_POSITIONS_3D);
    }

    std::shared_ptr<VKBuffer> storage_buffer()
    {
        return std::make_shared<VKBuffer>(64, VKBuffer::Usage::COMPUTE,
            Kakshya::DataModality::UNKNOWN);
    }

    std::shared_ptr<RootGraphicsBuffer> graphics_root()
    {
        auto root = std::make_shared<RootGraphicsBuffer>();
        root->initialize();
        return root;
    }

    TEST(RootGraphicsBufferTest, ChildRegistrationKeepsOrderAndFiltersByUsage)
    {
        auto root = graphics_root();
        auto first = vertex_buffer();
        auto middle = storage_buffer();
        auto last = vertex_buffer();

        root->add_child_buffer(first);
        root->add_child_buffer(middle);
        root->add_child_buffer(last);

        EXPECT_EQ(root->get_buffer_count(), 3U);
        EXPECT_TRUE(root->has_buffer(middle));
        EXPECT_EQ(root->get_buffers_by_usage(VKBuffer::Usage::VERTEX),
            (std::vector<std::shared_ptr<VKBuffer>> { first, last }));
        EXPECT_EQ(root->get_buffers_by_usage(VKBuffer::Usage::COMPUTE),
            (std::vector<std::shared_ptr<VKBuffer>> { middle }));

        root->remove_child_buffer(middle);

        EXPECT_FALSE(root->has_buffer(middle));
        EXPECT_EQ(root->get_child_buffers(),
            (std::vector<std::shared_ptr<VKBuffer>> { first, last }));
    }

    TEST(RootGraphicsBufferTest, IncompatibleDefaultProcessorCannotEnterTheGraphicsRoot)
    {
        auto root = graphics_root();
        auto child = vertex_buffer();
        auto processor = std::make_shared<GraphicsActionProcessor>();
        processor->set_processing_token(ProcessingToken::AUDIO_BACKEND);
        child->set_default_processor(processor);

        std::string reason;
        EXPECT_FALSE(root->try_add_child_buffer(child, &reason));
        EXPECT_FALSE(reason.empty());
        EXPECT_FALSE(root->has_buffer(child));
    }

    TEST(RootGraphicsBufferTest, BatchRunsEachEligibleChildDefaultThenItsCompleteChain)
    {
        auto root = graphics_root();
        auto first = vertex_buffer();
        auto second = vertex_buffer();
        std::vector<std::string> events;

        for (const auto& [child, name] : {
                 std::pair { first, std::string("first") },
                 std::pair { second, std::string("second") } }) {
            child->set_default_processor(std::make_shared<GraphicsActionProcessor>(
                [&events, name](const auto&) { events.push_back(name + ":default"); }));

            auto chain = child->get_processing_chain();
            chain->set_preferred_token(ProcessingToken::GRAPHICS_BACKEND);
            chain->add_preprocessor(std::make_shared<GraphicsActionProcessor>(
                                        [&events, name](const auto&) { events.push_back(name + ":pre"); }),
                child);
            chain->add_processor(std::make_shared<GraphicsActionProcessor>(
                                     [&events, name](const auto&) { events.push_back(name + ":process"); }),
                child);
            chain->add_postprocessor(std::make_shared<GraphicsActionProcessor>(
                                         [&events, name](const auto&) { events.push_back(name + ":post"); }),
                child);
            chain->add_final_processor(std::make_shared<GraphicsActionProcessor>(
                                           [&events, name](const auto&) { events.push_back(name + ":final"); }),
                child);
            root->add_child_buffer(child);
        }

        root->process_default();

        EXPECT_EQ(events,
            (std::vector<std::string> {
                "first:default", "first:pre", "first:process", "first:post", "first:final",
                "second:default", "second:pre", "second:process", "second:post", "second:final" }));
    }

    TEST(RootGraphicsBufferTest, BatchSkipsPausedChildrenAndCanDisableOnlyDefaultProcessing)
    {
        auto root = graphics_root();
        auto paused = vertex_buffer();
        auto chain_only = vertex_buffer();
        std::vector<std::string> events;

        paused->set_default_processor(std::make_shared<GraphicsActionProcessor>(
            [&events](const auto&) { events.emplace_back("paused"); }));
        paused->mark_for_processing(false);
        root->add_child_buffer(paused);

        chain_only->set_default_processor(std::make_shared<GraphicsActionProcessor>(
            [&events](const auto&) { events.emplace_back("default"); }));
        chain_only->enforce_default_processing(false);
        auto chain = chain_only->get_processing_chain();
        chain->set_preferred_token(ProcessingToken::GRAPHICS_BACKEND);
        chain->add_final_processor(std::make_shared<GraphicsActionProcessor>(
                                       [&events](const auto&) { events.emplace_back("final"); }),
            chain_only);
        root->add_child_buffer(chain_only);

        root->process_default();

        EXPECT_EQ(events, (std::vector<std::string> { "final" }));
    }

    TEST(RootGraphicsBufferTest, BatchClearsPreviousCommandsAndCollectsOnlyCurrentCommands)
    {
        auto root = graphics_root();
        auto child = vertex_buffer();
        constexpr Buffers::RenderPipelineID pipeline = 17;
        constexpr Buffers::CommandBufferID first_command = 29;
        constexpr Buffers::CommandBufferID second_command = 31;
        auto next_command = first_command;

        child->set_pipeline_window(pipeline, nullptr);
        child->set_default_processor(std::make_shared<GraphicsActionProcessor>(
            [&next_command, pipeline](const auto& buffer) {
                std::static_pointer_cast<VKBuffer>(buffer)->set_pipeline_command(pipeline, next_command);
            }));
        root->add_child_buffer(child);

        root->process_default();
        ASSERT_EQ(root->get_renderable_buffers().size(), 1U);
        EXPECT_EQ(root->get_renderable_buffers()[0].command_buffer_id, first_command);

        next_command = second_command;
        root->process_default();
        ASSERT_EQ(root->get_renderable_buffers().size(), 1U);
        EXPECT_EQ(root->get_renderable_buffers()[0].command_buffer_id, second_command);

        child->enforce_default_processing(false);
        root->process_default();
        EXPECT_TRUE(root->get_renderable_buffers().empty());
        EXPECT_EQ(child->get_pipeline_command(pipeline), Portal::Graphics::INVALID_COMMAND_BUFFER);
    }

    TEST(RootGraphicsBufferTest, BatchOrdersDrawsByPriorityAndPreservesEqualPriorityOrder)
    {
        auto root = graphics_root();
        const std::vector<int64_t> priorities { 3, -2, 3, 0 };
        std::vector<std::shared_ptr<VKBuffer>> children;

        for (size_t i = 0; i < priorities.size(); ++i) {
            auto child = vertex_buffer();
            const auto pipeline = static_cast<Buffers::RenderPipelineID>(i + 1);
            auto config = child->get_render_config();
            config.draw_priority = priorities[i];
            child->set_render_config(config);
            child->set_pipeline_window(pipeline, nullptr);
            child->set_default_processor(std::make_shared<GraphicsActionProcessor>(
                [pipeline](const auto& buffer) {
                    std::static_pointer_cast<VKBuffer>(buffer)->set_pipeline_command(pipeline, 41);
                }));
            root->add_child_buffer(child);
            children.push_back(child);
        }

        root->process_default();

        const auto& draws = root->get_renderable_buffers();
        ASSERT_EQ(draws.size(), 4U);
        EXPECT_EQ(draws[0].buffer, children[1]);
        EXPECT_EQ(draws[1].buffer, children[3]);
        EXPECT_EQ(draws[2].buffer, children[0]);
        EXPECT_EQ(draws[3].buffer, children[2]);
    }

    TEST(RootGraphicsBufferTest, BatchCarriesDepthRequirementIntoTheDrawList)
    {
        auto root = graphics_root();
        auto child = vertex_buffer();
        child->set_needs_depth_attachment(true);
        child->set_pipeline_window(7, nullptr);
        child->set_default_processor(std::make_shared<GraphicsActionProcessor>(
            [](const auto& buffer) { std::static_pointer_cast<VKBuffer>(buffer)->set_pipeline_command(7, 13); }));
        root->add_child_buffer(child);

        root->process_default();

        ASSERT_EQ(root->get_renderable_buffers().size(), 1U);
        EXPECT_TRUE(root->get_renderable_buffers()[0].needs_depth);
        EXPECT_EQ(root->get_renderable_buffers()[0].pipeline_id, 7U);
    }

    TEST(RootGraphicsBufferTest, MarkedChildIsRemovedDuringTheNextBatch)
    {
        auto root = graphics_root();
        auto child = vertex_buffer();
        root->add_child_buffer(child);
        child->mark_for_removal();

        root->process_default();

        EXPECT_FALSE(root->has_buffer(child));
        EXPECT_EQ(root->get_buffer_count(), 0U);
    }

    TEST(RootGraphicsBufferTest, ExplicitCleanupRemovesEveryMarkedChildAndKeepsTheOthers)
    {
        auto root = graphics_root();
        auto first = vertex_buffer();
        auto kept = vertex_buffer();
        auto last = vertex_buffer();
        root->add_child_buffer(first);
        root->add_child_buffer(kept);
        root->add_child_buffer(last);
        first->mark_for_removal();
        last->mark_for_removal();

        root->cleanup_marked_buffers();
        root->cleanup_marked_buffers();

        EXPECT_EQ(root->get_child_buffers(),
            (std::vector<std::shared_ptr<VKBuffer>> { kept }));
    }

    TEST(RootGraphicsBufferTest, DefaultBatchProcessorDoesNotKeepTheRootAlive)
    {
        std::weak_ptr<RootGraphicsBuffer> weak;
        {
            auto root = graphics_root();
            weak = root;
        }

        EXPECT_TRUE(weak.expired());
    }

    TEST(RootGraphicsBufferTest, AnAttachedPresenterDoesNotKeepTheRootAlive)
    {
        std::weak_ptr<RootGraphicsBuffer> weak;
        {
            auto root = std::make_shared<RootGraphicsBuffer>();
            auto presenter = std::make_shared<PresentProcessor>();
            presenter->on_attach(root);
            root->set_final_processor(presenter);
            weak = root;
        }

        EXPECT_TRUE(weak.expired());
    }

    TEST(RootGraphicsBufferTest, PresentCallbackReceivesTheBatchResult)
    {
        auto root = graphics_root();
        auto child = vertex_buffer();
        child->set_pipeline_window(5, nullptr);
        child->set_default_processor(std::make_shared<GraphicsActionProcessor>(
            [](const auto& buffer) { std::static_pointer_cast<VKBuffer>(buffer)->set_pipeline_command(5, 19); }));
        root->add_child_buffer(child);
        root->process_default();

        size_t observed_draws = 0;
        PresentProcessor presenter([&observed_draws](const auto& presented_root) {
            observed_draws = presented_root->get_renderable_buffers().size();
        });
        presenter.processing_function(root);

        EXPECT_EQ(observed_draws, 1U);
        EXPECT_EQ(root->get_renderable_buffers().size(), 1U);
    }

    TEST(RootGraphicsBufferTest, InitializingInstallsABatchProcessorForTheGraphicsToken)
    {
        auto root = graphics_root();
        auto batch = std::dynamic_pointer_cast<GraphicsBatchProcessor>(root->get_default_processor());

        ASSERT_NE(batch, nullptr);
        EXPECT_EQ(batch->get_processing_token(), ProcessingToken::GRAPHICS_BACKEND);
        EXPECT_TRUE(batch->is_compatible_with(root));
        EXPECT_FALSE(batch->is_compatible_with(vertex_buffer()));
    }

    TEST(RootGraphicsBufferTest, BatchRejectsAnotherRootWithoutProcessingItsChildren)
    {
        auto owner = graphics_root();
        auto other = graphics_root();
        auto child = vertex_buffer();
        size_t processed = 0;
        child->set_default_processor(std::make_shared<GraphicsActionProcessor>(
            [&processed](const auto&) { ++processed; }));
        other->add_child_buffer(child);

        auto batch = std::dynamic_pointer_cast<GraphicsBatchProcessor>(owner->get_default_processor());
        ASSERT_NE(batch, nullptr);
        batch->processing_function(other);
        batch->processing_function(child);

        EXPECT_EQ(processed, 0U);
        EXPECT_TRUE(other->get_renderable_buffers().empty());
    }

    TEST(RootGraphicsBufferTest, BatchAttachmentRequiresARootGraphicsBuffer)
    {
        auto root = graphics_root();
        GraphicsBatchProcessor batch(root);

        EXPECT_NO_THROW(batch.on_attach(root));
        EXPECT_THROW(batch.on_attach(vertex_buffer()), std::invalid_argument);
    }

    TEST(RootGraphicsBufferTest, BatchAttachmentRejectsAnIncompatibleToken)
    {
        auto root = graphics_root();
        GraphicsBatchProcessor batch(root);
        batch.set_processing_token(ProcessingToken::AUDIO_BACKEND);

        EXPECT_THROW(batch.on_attach(root), std::runtime_error);
    }

    TEST(RootGraphicsBufferTest, AChildWithoutARecordedCommandDoesNotBecomeRenderable)
    {
        auto root = graphics_root();
        auto child = vertex_buffer();
        child->set_pipeline_window(9, nullptr);
        root->add_child_buffer(child);

        root->process_default();

        EXPECT_TRUE(root->get_renderable_buffers().empty());
    }

    TEST(RootGraphicsBufferTest, BatchKeepsOtherChildrenRunningAfterOneThrows)
    {
        auto root = graphics_root();
        auto failing = vertex_buffer();
        auto healthy = vertex_buffer();
        size_t processed = 0;
        failing->set_default_processor(std::make_shared<GraphicsActionProcessor>(
            [](const auto&) { throw std::runtime_error("test processor failure"); }));
        healthy->set_default_processor(std::make_shared<GraphicsActionProcessor>(
            [&processed](const auto&) { ++processed; }));
        root->add_child_buffer(failing);
        root->add_child_buffer(healthy);

        EXPECT_NO_THROW(root->process_default());
        EXPECT_EQ(processed, 1U);
    }

    TEST(RootGraphicsBufferTest, ExplicitChildRemovalPreservesTheOtherChildren)
    {
        auto root = graphics_root();
        auto first = vertex_buffer();
        auto second = vertex_buffer();
        auto third = vertex_buffer();
        root->add_child_buffer(first);
        root->add_child_buffer(second);
        root->add_child_buffer(third);

        root->remove_child_buffer(second);
        root->process_default();

        EXPECT_EQ(root->get_child_buffers(),
            (std::vector<std::shared_ptr<VKBuffer>> { first, third }));
    }

    TEST(RootGraphicsBufferTest, FinalProcessorAndTokenActivityCanBeReconfigured)
    {
        auto root = graphics_root();
        auto final = std::make_shared<PresentProcessor>([](const auto&) {});

        root->set_final_processor(final);
        root->set_token_active(true);
        EXPECT_EQ(root->get_final_processor(), final);
        EXPECT_TRUE(root->is_token_active());

        root->set_final_processor(nullptr);
        root->set_token_active(false);
        EXPECT_EQ(root->get_final_processor(), nullptr);
        EXPECT_FALSE(root->is_token_active());
    }

    TEST(RootGraphicsBufferTest, ExternalFinalProcessorObservesTheCompletedBatch)
    {
        auto root = graphics_root();
        auto child = vertex_buffer();
        std::vector<std::string> order;
        child->set_default_processor(std::make_shared<GraphicsActionProcessor>(
            [&order](const auto&) { order.emplace_back("child"); }));
        root->add_child_buffer(child);
        auto chain = std::make_shared<BufferProcessingChain>();
        chain->set_preferred_token(ProcessingToken::GRAPHICS_BACKEND);
        chain->add_final_processor(std::make_shared<GraphicsActionProcessor>(
                                       [&order](const auto&) { order.emplace_back("final"); }),
            root);

        root->process_default();
        EXPECT_EQ(order, (std::vector<std::string> { "child" }));
        chain->process_complete(root);

        EXPECT_EQ(order, (std::vector<std::string> { "child", "final" }));
    }

    TEST(RootGraphicsBufferTest, DirectCallerCanRunAnAttachedPresenterAfterBatching)
    {
        auto root = graphics_root();
        auto child = vertex_buffer();
        child->set_pipeline_window(23, nullptr);
        child->set_default_processor(std::make_shared<GraphicsActionProcessor>(
            [](const auto& buffer) { std::static_pointer_cast<VKBuffer>(buffer)->set_pipeline_command(23, 37); }));
        root->add_child_buffer(child);

        size_t observed_draws = 0;
        auto presenter = std::make_shared<PresentProcessor>([&observed_draws](const auto& ready) {
            observed_draws = ready->get_renderable_buffers().size();
        });
        presenter->on_attach(root);
        root->set_final_processor(presenter);

        root->process_default();
        ASSERT_EQ(root->get_renderable_buffers().size(), 1U);
        EXPECT_EQ(observed_draws, 0U);
        root->get_final_processor()->process(root);

        EXPECT_EQ(observed_draws, 1U);
        presenter->on_detach(root);
    }

    TEST(RootGraphicsBufferTest, ReplacingTheFinalPresenterReleasesThePreviousOne)
    {
        auto root = graphics_root();
        std::weak_ptr<PresentProcessor> previous;
        {
            auto first = std::make_shared<PresentProcessor>();
            previous = first;
            root->set_final_processor(first);
        }
        ASSERT_FALSE(previous.expired());

        auto second = std::make_shared<PresentProcessor>();
        root->set_final_processor(second);

        EXPECT_TRUE(previous.expired());
        EXPECT_EQ(root->get_final_processor(), second);
    }

    TEST(RootGraphicsBufferTest, PresentProcessorAcceptsOnlyARootGraphicsBuffer)
    {
        PresentProcessor presenter;
        auto root = graphics_root();
        auto child = vertex_buffer();

        EXPECT_TRUE(presenter.is_compatible_with(root));
        EXPECT_FALSE(presenter.is_compatible_with(child));
        EXPECT_THROW(presenter.on_attach(child), std::invalid_argument);
        EXPECT_NO_THROW(presenter.processing_function(child));
    }

    TEST(RootGraphicsBufferTest, PresenterAttachmentRejectsAnIncompatibleToken)
    {
        auto root = graphics_root();
        PresentProcessor presenter;
        presenter.set_processing_token(ProcessingToken::AUDIO_BACKEND);

        EXPECT_THROW(presenter.on_attach(root), std::runtime_error);
    }

    TEST(RootGraphicsBufferTest, AttachedPresenterRefusesADifferentRoot)
    {
        auto owner = graphics_root();
        auto other = graphics_root();
        size_t callbacks = 0;
        PresentProcessor presenter([&callbacks](const auto&) { ++callbacks; });
        presenter.on_attach(owner);

        presenter.processing_function(other);
        presenter.on_detach(other);
        presenter.processing_function(other);
        presenter.processing_function(owner);
        presenter.on_detach(owner);
        presenter.processing_function(other);

        EXPECT_EQ(callbacks, 2U);
    }

    TEST(RootGraphicsBufferTest, ExpiredPresenterAttachmentRejectsOtherRootsUntilReattached)
    {
        size_t callbacks = 0;
        PresentProcessor presenter([&callbacks](const auto&) { ++callbacks; });
        std::weak_ptr<RootGraphicsBuffer> former_owner;
        {
            auto owner = std::make_shared<RootGraphicsBuffer>();
            former_owner = owner;
            presenter.on_attach(owner);
        }
        ASSERT_TRUE(former_owner.expired());

        auto other = graphics_root();
        presenter.processing_function(other);
        EXPECT_EQ(callbacks, 0U);

        presenter.on_attach(other);
        presenter.processing_function(other);
        EXPECT_EQ(callbacks, 1U);
    }

    TEST(RootGraphicsBufferTest, PresentCallbackCanBeReplacedAndCleared)
    {
        auto root = graphics_root();
        size_t first = 0;
        size_t replacement = 0;
        PresentProcessor presenter([&first](const auto&) { ++first; });

        presenter.processing_function(root);
        presenter.set_callback([&replacement](const auto&) { ++replacement; });
        presenter.processing_function(root);
        presenter.clear_callback();
        EXPECT_FALSE(presenter.has_callback());
        presenter.processing_function(root);

        EXPECT_EQ(first, 1U);
        EXPECT_EQ(replacement, 1U);
    }

    TEST(RootGraphicsBufferTest, EmptyFallbackPresentationDoesNotNeedADisplayService)
    {
        auto root = graphics_root();
        PresentProcessor presenter;

        EXPECT_NO_THROW(presenter.processing_function(root));
        EXPECT_TRUE(root->get_renderable_buffers().empty());
    }

    TEST(RootGraphicsBufferTest, NullRefreshIsIgnored)
    {
        auto root = graphics_root();

        root->request_presentation_refresh(nullptr);

        EXPECT_TRUE(root->get_pending_presentation_refreshes().empty());
    }

    TEST(RootGraphicsBufferTest, UnroutableDrawIsDiscardedByTheFallbackPresenter)
    {
        auto root = graphics_root();
        auto child = vertex_buffer();
        child->set_pipeline_window(3, nullptr);
        child->set_default_processor(std::make_shared<GraphicsActionProcessor>(
            [](const auto& buffer) { std::static_pointer_cast<VKBuffer>(buffer)->set_pipeline_command(3, 11); }));
        root->add_child_buffer(child);
        root->process_default();
        ASSERT_EQ(root->get_renderable_buffers().size(), 1U);

        PresentProcessor presenter;
        presenter.processing_function(root);

        EXPECT_TRUE(root->get_renderable_buffers().empty());
    }

    TEST(RootGraphicsBufferTest, AThrowingPresentationCallbackPropagatesTheFailure)
    {
        auto root = graphics_root();
        PresentProcessor presenter([](const auto&) {
            throw std::runtime_error("presentation failed");
        });

        EXPECT_THROW(presenter.processing_function(root), std::runtime_error);
    }

#if defined(MAYAFLUX_TEST_NATIVE_WINDOWS)

    class RootGraphicsPresentationTest : public PresentingVulkanTest {
    protected:
        void SetUp() override
        {
            PresentingVulkanTest::SetUp();
            if (IsSkipped())
                return;

            Portal::Graphics::get_render_flow().shutdown();
            Portal::Graphics::get_shader_foundry().shutdown();
            auto borrowed_backend = std::shared_ptr<Core::VulkanBackend>(&backend(), [](Core::VulkanBackend*) {});
            ASSERT_TRUE(Portal::Graphics::get_shader_foundry().initialize(borrowed_backend));
            foundry_ready = true;
            ASSERT_TRUE(Portal::Graphics::get_render_flow().initialize());
            flow_ready = true;
        }

        void TearDown() override
        {
            if (chain && extra_render && child)
                chain->remove_processor_anywhere(extra_render, child);
            if (chain && render && child)
                chain->remove_processor_anywhere(render, child);
            extra_render.reset();
            render.reset();
            chain.reset();
            if (root)
                root->set_default_processor(nullptr);
            root.reset();

            if (child && child->is_initialized()) {
                auto* service = Registry::BackendRegistry::instance()
                                    .get_service<Registry::Service::BufferService>();
                if (service)
                    service->destroy_buffer(child);
            }
            child.reset();

            if (flow_ready)
                Portal::Graphics::get_render_flow().shutdown();
            if (foundry_ready)
                Portal::Graphics::get_shader_foundry().shutdown();
            PresentingVulkanTest::TearDown();
        }

        void prepare_draw()
        {
            window = make_window();
            ASSERT_NE(window, nullptr);
            ASSERT_TRUE(backend().register_window(window));

            child = vertex_buffer();
            auto* service = Registry::BackendRegistry::instance()
                                .get_service<Registry::Service::BufferService>();
            ASSERT_NE(service, nullptr);
            service->initialize_buffer(child);
            ASSERT_TRUE(child->is_initialized());

            child->set_vertex_layout(Kakshya::VertexLayout {});

            Buffers::ShaderConfig shader {
                R"(#version 460
void main() {
    vec2 p[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(p[gl_VertexIndex], 0.0, 1.0);
}
)"
            };
            shader.stage = Portal::Graphics::ShaderStage::VERTEX;
            render = std::make_shared<Buffers::RenderProcessor>(shader);
            render->set_fragment_shader(R"(#version 460
layout(location = 0) out vec4 color;
void main() { color = vec4(1.0, 0.0, 0.0, 1.0); }
)");
            render->set_target_window(window, child);
            render->set_primitive_topology(Portal::Graphics::PrimitiveTopology::TRIANGLE_LIST);
            render->disable_depth_test();
            render->set_vertex_range(0, 3);

            chain = child->get_processing_chain();
            chain->set_preferred_token(ProcessingToken::GRAPHICS_BACKEND);
            chain->add_final_processor(render, child);

            root = graphics_root();
            root->add_child_buffer(child);
        }

        std::shared_ptr<Core::Window> window;
        std::shared_ptr<RootGraphicsBuffer> root;
        std::shared_ptr<VKBuffer> child;
        std::shared_ptr<BufferProcessingChain> chain;
        std::shared_ptr<Buffers::RenderProcessor> render;
        std::shared_ptr<Buffers::RenderProcessor> extra_render;
        bool foundry_ready {};
        bool flow_ready {};
    };

    TEST_F(RootGraphicsPresentationTest, BatchRecordsARealDrawAndPresenterConsumesIt)
    {
        prepare_draw();
        ASSERT_NE(root, nullptr);

        root->process_default();

        const auto& draws = root->get_renderable_buffers();
        ASSERT_EQ(draws.size(), 1U);
        EXPECT_EQ(draws[0].buffer, child);
        EXPECT_EQ(draws[0].target_window, window);
        EXPECT_NE(draws[0].pipeline_id, Portal::Graphics::INVALID_RENDER_PIPELINE);
        EXPECT_NE(draws[0].command_buffer_id, Portal::Graphics::INVALID_COMMAND_BUFFER);

        PresentProcessor presenter;
        presenter.processing_function(root);

        EXPECT_TRUE(root->get_renderable_buffers().empty());
        EXPECT_EQ(child->get_pipeline_command(render->get_render_pipeline_id()),
            Portal::Graphics::INVALID_COMMAND_BUFFER);
    }

    TEST_F(RootGraphicsPresentationTest, CustomPresenterReceivesRealDrawWithoutFallbackConsumption)
    {
        prepare_draw();
        ASSERT_NE(root, nullptr);

        size_t callback_count = 0;
        auto presenter = std::make_shared<PresentProcessor>([&](const auto& ready) {
            ++callback_count;
            ASSERT_EQ(ready->get_renderable_buffers().size(), 1U);
            EXPECT_EQ(ready->get_renderable_buffers()[0].buffer, child);
            EXPECT_EQ(ready->get_renderable_buffers()[0].target_window, window);
        });
        auto root_chain = std::make_shared<BufferProcessingChain>();
        root_chain->set_preferred_token(ProcessingToken::GRAPHICS_BACKEND);
        root_chain->add_final_processor(presenter, root);

        root->process_default();
        const auto command = child->get_pipeline_command(render->get_render_pipeline_id());
        ASSERT_NE(command, Portal::Graphics::INVALID_COMMAND_BUFFER);

        root_chain->process_complete(root);

        EXPECT_EQ(callback_count, 1U);
        EXPECT_EQ(root->get_renderable_buffers().size(), 1U);
        EXPECT_EQ(child->get_pipeline_command(render->get_render_pipeline_id()), command);
        root_chain->remove_processor_anywhere(presenter, root);
    }

    TEST_F(RootGraphicsPresentationTest, ExplicitRefreshIsConsumedAfterASecondPresentation)
    {
        prepare_draw();
        ASSERT_NE(root, nullptr);
        PresentProcessor presenter;

        root->process_default();
        presenter.processing_function(root);

        child->mark_for_processing(false);
        root->request_presentation_refresh(window);
        root->process_default();
        ASSERT_FALSE(root->get_pending_presentation_refreshes().empty());
        ASSERT_EQ(root->get_renderable_buffers().size(), 1U);

        presenter.processing_function(root);

        EXPECT_TRUE(root->get_pending_presentation_refreshes().empty());
        EXPECT_TRUE(root->get_renderable_buffers().empty());
    }

    TEST_F(RootGraphicsPresentationTest, DetachedWindowStopsItsRenderProcessorAndLeavesTheChild)
    {
        prepare_draw();
        ASSERT_NE(root, nullptr);
        size_t processing_calls = 0;
        child->set_default_processor(std::make_shared<GraphicsActionProcessor>(
            [&processing_calls](const auto&) { ++processing_calls; }));
        root->process_default();
        ASSERT_EQ(root->get_renderable_buffers().size(), 1U);

        root->detach_window(window);

        EXPECT_TRUE(root->has_buffer(child));
        EXPECT_TRUE(root->get_renderable_buffers().empty());
        EXPECT_TRUE(root->get_pending_presentation_refreshes().empty());
        EXPECT_EQ(chain->get_final_processor(child), nullptr);
        EXPECT_TRUE(child->get_render_pipelines().empty());

        root->process_default();
        EXPECT_EQ(processing_calls, 2U);
    }

    TEST_F(RootGraphicsPresentationTest, NullWindowDetachmentLeavesTheCompositionAlone)
    {
        prepare_draw();
        ASSERT_NE(root, nullptr);
        root->process_default();

        root->detach_window(nullptr);

        ASSERT_EQ(root->get_renderable_buffers().size(), 1U);
        EXPECT_EQ(root->get_renderable_buffers()[0].target_window, window);
        EXPECT_EQ(chain->get_final_processor(child), render);
    }

    TEST_F(RootGraphicsPresentationTest, RemovedChildRequestsAWindowClear)
    {
        prepare_draw();
        ASSERT_NE(root, nullptr);
        root->process_default();
        PresentProcessor presenter;
        presenter.processing_function(root);

        child->mark_for_removal();
        root->process_default();

        EXPECT_FALSE(root->has_buffer(child));
        EXPECT_TRUE(root->get_renderable_buffers().empty());
        EXPECT_TRUE(root->get_pending_presentation_refreshes().contains(window));

        presenter.processing_function(root);
        EXPECT_TRUE(root->get_pending_presentation_refreshes().empty());
    }

    TEST_F(RootGraphicsPresentationTest, RefreshWithoutDrawsPresentsAClearFrame)
    {
        window = make_window();
        ASSERT_NE(window, nullptr);
        ASSERT_TRUE(backend().register_window(window));
        root = graphics_root();
        root->request_presentation_refresh(window);

        root->process_default();
        ASSERT_TRUE(root->get_renderable_buffers().empty());
        ASSERT_TRUE(root->get_pending_presentation_refreshes().contains(window));

        PresentProcessor presenter;
        presenter.processing_function(root);

        EXPECT_TRUE(root->get_pending_presentation_refreshes().empty());
    }

    TEST_F(RootGraphicsPresentationTest, RefreshForAnUnregisteredWindowIsDiscarded)
    {
        window = make_window();
        ASSERT_NE(window, nullptr);
        root = graphics_root();
        root->request_presentation_refresh(window);

        PresentProcessor presenter;
        presenter.processing_function(root);

        EXPECT_TRUE(root->get_pending_presentation_refreshes().empty());
    }

    TEST_F(RootGraphicsPresentationTest, DepthDrawRequestsAWindowDepthAttachment)
    {
        prepare_draw();
        ASSERT_NE(root, nullptr);
        render->enable_depth_test();

        root->process_default();
        ASSERT_EQ(root->get_renderable_buffers().size(), 1U);
        EXPECT_TRUE(root->get_renderable_buffers()[0].needs_depth);

        PresentProcessor presenter;
        presenter.processing_function(root);

        auto* display = Registry::BackendRegistry::instance()
                            .get_service<Registry::Service::DisplayService>();
        ASSERT_NE(display, nullptr);
        EXPECT_NE(display->get_depth_format(window),
            static_cast<uint32_t>(vk::Format::eUndefined));
    }

    TEST_F(RootGraphicsPresentationTest, ChildRefreshRebuildsTheWindowComposition)
    {
        prepare_draw();
        ASSERT_NE(root, nullptr);
        root->process_default();
        PresentProcessor presenter;
        presenter.processing_function(root);

        child->mark_for_processing(false);
        child->request_presentation_refresh(window);
        root->process_default();

        EXPECT_TRUE(root->get_pending_presentation_refreshes().contains(window));
        ASSERT_EQ(root->get_renderable_buffers().size(), 1U);
        EXPECT_EQ(root->get_renderable_buffers()[0].buffer, child);
    }

    TEST_F(RootGraphicsPresentationTest, HiddenDrawClearsTheWindowAndShowingItRestoresTheDraw)
    {
        prepare_draw();
        ASSERT_NE(root, nullptr);
        PresentProcessor presenter;
        root->process_default();
        presenter.processing_function(root);
        child->mark_for_processing(false);

        render->set_visible(false, child);
        root->process_default();
        EXPECT_TRUE(root->get_renderable_buffers().empty());
        EXPECT_TRUE(root->get_pending_presentation_refreshes().contains(window));
        presenter.processing_function(root);

        render->set_visible(true, child);
        root->process_default();
        ASSERT_EQ(root->get_renderable_buffers().size(), 1U);
        EXPECT_EQ(root->get_renderable_buffers()[0].buffer, child);
    }

    TEST_F(RootGraphicsPresentationTest, PresenterConsumesDrawsForTwoWindows)
    {
        prepare_draw();
        ASSERT_NE(root, nullptr);
        auto other_window = make_window();
        ASSERT_NE(other_window, nullptr);
        ASSERT_TRUE(backend().register_window(other_window));

        Buffers::ShaderConfig shader {
            R"(#version 460
void main() {
    vec2 p[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(p[gl_VertexIndex], 0.0, 1.0);
}
)"
        };
        shader.stage = Portal::Graphics::ShaderStage::VERTEX;
        extra_render = std::make_shared<Buffers::RenderProcessor>(shader);
        extra_render->set_fragment_shader(R"(#version 460
layout(location = 0) out vec4 color;
void main() { color = vec4(0.0, 1.0, 0.0, 1.0); }
)");
        extra_render->set_target_window(other_window, child);
        extra_render->set_primitive_topology(Portal::Graphics::PrimitiveTopology::TRIANGLE_LIST);
        extra_render->disable_depth_test();
        extra_render->set_vertex_range(0, 3);
        ASSERT_TRUE(chain->add_processor(extra_render, child));

        root->process_default();

        const auto& draws = root->get_renderable_buffers();
        ASSERT_EQ(draws.size(), 2U);
        EXPECT_EQ(std::ranges::count_if(draws, [&](const auto& draw) {
                      return draw.target_window == window;
                  }),
            1);
        EXPECT_EQ(std::ranges::count_if(draws, [&](const auto& draw) {
                      return draw.target_window == other_window;
                  }),
            1);

        PresentProcessor presenter;
        presenter.processing_function(root);

        EXPECT_TRUE(root->get_renderable_buffers().empty());
        EXPECT_EQ(child->get_pipeline_command(render->get_render_pipeline_id()),
            Portal::Graphics::INVALID_COMMAND_BUFFER);
        EXPECT_EQ(child->get_pipeline_command(extra_render->get_render_pipeline_id()),
            Portal::Graphics::INVALID_COMMAND_BUFFER);

        root->detach_window(window);
        root->process_default();

        ASSERT_EQ(root->get_renderable_buffers().size(), 1U);
        EXPECT_EQ(root->get_renderable_buffers()[0].target_window, other_window);
    }

#endif

}
}
