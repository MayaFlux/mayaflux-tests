#include "gtest/gtest.h"

#include "../../support/native_window_test_support.hpp"

#if defined(MAYAFLUX_TEST_NATIVE_WINDOWS)

#include "../../support/frame_scheduler.hpp"
#include "../../support/scripted_window.hpp"
#include "../../support/text_test_support.hpp"
#include "../../support/vulkan_test_support.hpp"

#include "MayaFlux/Buffers/BufferManager.hpp"
#include "MayaFlux/Buffers/BufferProcessingChain.hpp"
#include "MayaFlux/Buffers/Forma/FormaBuffer.hpp"
#include "MayaFlux/Buffers/Root/RootGraphicsBuffer.hpp"
#include "MayaFlux/Buffers/Shaders/RenderProcessor.hpp"
#include "MayaFlux/Core/Windowing/WindowManager.hpp"
#include "MayaFlux/Kriya/Awaiters/BroadcastAwaiter.hpp"
#include "MayaFlux/Kriya/Awaiters/GetPromise.hpp"
#include "MayaFlux/Nodes/Conduit/Constant.hpp"
#include "MayaFlux/Nodes/NodeGraphManager.hpp"
#include "MayaFlux/Portal/Forma/Forma.hpp"
#include "MayaFlux/Portal/Forma/Primitives/Collapsible.hpp"
#include "MayaFlux/Portal/Graphics/RenderFlow.hpp"
#include "MayaFlux/Portal/Graphics/ShaderFoundry.hpp"
#include "MayaFlux/Portal/Graphics/TextureLoom.hpp"
#include "MayaFlux/Portal/Text/TypeFaceFoundry.hpp"
#include "MayaFlux/Registry/BackendRegistry.hpp"
#include "MayaFlux/Registry/Service/BufferService.hpp"
#include "MayaFlux/Registry/Service/DisplayService.hpp"
#include "MayaFlux/Transitive/Memory/Persist.hpp"
#include "MayaFlux/Vruta/BroadcastSource.hpp"
#include "MayaFlux/Vruta/EventManager.hpp"

namespace MayaFlux::Test {

namespace Forma = Portal::Forma;

namespace {

    Vruta::Event idle_listener(std::shared_ptr<Vruta::BroadcastSource<int>> source)
    {
        auto& promise = co_await Kriya::GetEventPromise {};
        while (!promise.should_terminate)
            co_await source->next();
    }

}

class FormaWindowApiTest : public PresentingVulkanTest {
protected:
    static void SetUpTestSuite()
    {
        PresentingVulkanTest::SetUpTestSuite();
        if (!available())
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
        PresentingVulkanTest::TearDownTestSuite();
    }

    void SetUp() override
    {
        PresentingVulkanTest::SetUp();
        if (IsSkipped())
            return;
        ASSERT_TRUE(s_ready);

        auto& foundry = Portal::Text::TypeFaceFoundry::instance();
        m_owns_text = !foundry.is_initialized();
        ASSERT_TRUE(foundry.initialize());
        const auto& path = system_font_path();
        ASSERT_TRUE(path.has_value()) << "no system font could be resolved on this host";
        ASSERT_TRUE(foundry.set_default_font(*path, 16, 512));

        Forma::shutdown();
        ASSERT_TRUE(Forma::initialize(nodes, buffer_manager, frames.scheduler, events,
            std::shared_ptr<Core::WindowManager>(&manager(), [](Core::WindowManager*) { })));
    }

    void TearDown() override
    {
        if (!available()) {
            return;
        }
        Forma::shutdown();
        MayaFlux::internal::cleanup_persistent_store();

        for (const auto& title : opened)
            manager().destroy_window_by_title(title);
        opened.clear();

        auto* service = Registry::BackendRegistry::instance()
                            .get_service<Registry::Service::BufferService>();
        const auto buffers = buffer_manager->get_graphics_buffers(Buffers::ProcessingToken::GRAPHICS_BACKEND);
        for (const auto& buffer : buffers) {
            buffer_manager->remove_buffer(buffer, Buffers::ProcessingToken::GRAPHICS_BACKEND);
            if (auto render = buffer->get_render_processor(); render && buffer->get_processing_chain())
                buffer->get_processing_chain()->remove_processor_anywhere(render, buffer);
            buffer->set_default_processor(nullptr);
            if (service && buffer->is_initialized())
                service->destroy_buffer(buffer);
        }

        if (m_owns_text)
            Portal::Text::TypeFaceFoundry::instance().shutdown();
        PresentingVulkanTest::TearDown();
    }

    std::shared_ptr<Core::Window> expect_window(const std::string& title)
    {
        opened.push_back(title);
        auto found = manager().find_window(title);
        EXPECT_NE(found, nullptr) << title;
        return found;
    }

    uint32_t row_px() const
    {
        const auto atlas = Portal::Text::TypeFaceFoundry::instance().get_default_glyph_atlas();
        return std::max(48U, atlas->pixel_size() + 16U);
    }

    FrameScheduler frames;
    std::shared_ptr<Nodes::NodeGraphManager> nodes = std::make_shared<Nodes::NodeGraphManager>();
    std::shared_ptr<Buffers::BufferManager> buffer_manager = std::make_shared<Buffers::BufferManager>();
    std::shared_ptr<Vruta::EventManager> events = std::make_shared<Vruta::EventManager>();
    std::vector<std::string> opened;

private:
    bool m_owns_text {};
    static inline bool s_ready {};
};

// =============================================================================
// Inspect windows
// =============================================================================

TEST_F(FormaWindowApiTest, InspectNodeGraphOpensOneWindowAndReusesIt)
{
    Forma::inspect_node_graph();
    const size_t opened_count = manager().window_count();
    auto window = expect_window("NodeGraphManager");

    Forma::inspect_node_graph();

    EXPECT_EQ(manager().window_count(), opened_count);
    EXPECT_EQ(manager().find_window("NodeGraphManager"), window);
}

TEST_F(FormaWindowApiTest, EachManagerInspectionOpensItsOwnTitledWindow)
{
    Forma::inspect_buffers();
    Forma::inspect_scheduler();
    Forma::inspect_events();

    EXPECT_NE(expect_window("BufferManager"), nullptr);
    EXPECT_NE(expect_window("TaskScheduler"), nullptr);
    EXPECT_NE(expect_window("EventManager"), nullptr);
}

TEST_F(FormaWindowApiTest, InspectingANodeTitlesTheWindowWithItsType)
{
    Forma::inspect(std::static_pointer_cast<Nodes::Node>(std::make_shared<Nodes::Constant>(1.0)));

    EXPECT_NE(expect_window("Constant"), nullptr);
}

TEST_F(FormaWindowApiTest, InspectingAnEventTitlesTheWindowWithItsName)
{
    auto source = std::make_shared<Vruta::BroadcastSource<int>>();
    auto event = std::make_shared<Vruta::Event>(idle_listener(source));

    Forma::inspect(event, "listener");

    EXPECT_NE(expect_window("Event: listener"), nullptr);
}

TEST_F(FormaWindowApiTest, InspectingAnUnnamedEventTitlesTheWindowEvent)
{
    auto source = std::make_shared<Vruta::BroadcastSource<int>>();
    auto event = std::make_shared<Vruta::Event>(idle_listener(source));

    Forma::inspect(event, "");

    EXPECT_NE(expect_window("Event"), nullptr);
}

// =============================================================================
// Picker
// =============================================================================

TEST_F(FormaWindowApiTest, PickingFromNothingReportsNoChoiceWithoutOpeningAWindow)
{
    const size_t before = manager().window_count();
    int calls = 0;
    std::optional<size_t> choice { 99 };

    Forma::pick("empty-pick", {}, [&](std::optional<size_t> index) {
        ++calls;
        choice = index;
    });

    EXPECT_EQ(calls, 1);
    EXPECT_FALSE(choice.has_value());
    EXPECT_EQ(manager().window_count(), before);
}

TEST_F(FormaWindowApiTest, PickingOpensAWindowSizedToOneRowPerLabel)
{
    int calls = 0;

    Forma::pick("short-pick", { "alpha", "beta", "gamma" }, [&](std::optional<size_t>) { ++calls; });
    auto window = expect_window("short-pick");

    ASSERT_NE(window, nullptr);
    EXPECT_EQ(window->get_create_info().width, 520U);
    EXPECT_EQ(window->get_create_info().height, row_px() * 3U);
    EXPECT_EQ(calls, 0);
}

TEST_F(FormaWindowApiTest, PickingFromALongListShowsTenRowsAndScrolls)
{
    std::vector<std::string> labels;
    for (int i = 0; i < 12; ++i)
        labels.push_back("row " + std::to_string(i));

    Forma::pick("long-pick", std::move(labels), [](std::optional<size_t>) { });
    auto window = expect_window("long-pick");

    ASSERT_NE(window, nullptr);
    EXPECT_EQ(window->get_create_info().height, row_px() * 10U + row_px() / 2U);
}

class FormaCollapsiblePresentationTest : public FormaWindowApiTest {
protected:
    void SetUp() override
    {
        FormaWindowApiTest::SetUp();
        if (IsSkipped())
            return;

        ASSERT_TRUE(Portal::Graphics::get_render_flow().initialize());
        flow_ready = true;

        native = make_window();
        ASSERT_NE(native, nullptr);
        ASSERT_TRUE(backend().register_window(native));
    }

    void TearDown() override
    {
        if (root)
            root->set_default_processor(nullptr);
        root.reset();
        FormaWindowApiTest::TearDown();
        if (flow_ready)
            Portal::Graphics::get_render_flow().shutdown();
        flow_ready = false;
    }

    void present()
    {
        root->process_default();
        Buffers::PresentProcessor presenter;
        presenter.processing_function(root);
    }

    [[nodiscard]] std::array<uint8_t, 4> corner_pixel() const
    {
        auto* display = Registry::BackendRegistry::instance()
                            .get_service<Registry::Service::DisplayService>();
        EXPECT_NE(display, nullptr);
        if (!display)
            return {};
        uint32_t width = 0;
        uint32_t height = 0;
        display->get_swapchain_extent(native, width, height);
        EXPECT_GT(width, 8U);
        EXPECT_GT(height, 8U);
        std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
        if (pixels.empty())
            return {};
        const bool read = display->readback_swapchain_region(
            native, pixels.data(), 0, 0, width, height, pixels.size());
        EXPECT_TRUE(read);
        if (!read)
            return {};
        const size_t corner = (static_cast<size_t>(height - 4) * width + (width - 4)) * 4;
        return { pixels[corner], pixels[corner + 1], pixels[corner + 2], pixels[corner + 3] };
    }

    std::shared_ptr<Core::Window> native;
    std::shared_ptr<Buffers::RootGraphicsBuffer> root;
    bool flow_ready {};
};

TEST_F(FormaCollapsiblePresentationTest, PressingALabelledHeaderRepaintsItsBackgroundColour)
{
    auto scripted = std::make_shared<ScriptedWindow>(400U, 300U);
    Forma::SurfaceConfig config;
    config.window = scripted;
    config.name = "collapsible-presentation";
    config.detach_on_close = false;
    auto surface = Forma::create_surface(std::move(config));

    Forma::LayoutCursor cursor;
    auto header = Forma::Collapsible {}
                      .initially_open(false)
                      .closed_color({ 0.1F, 0.1F, 0.1F })
                      .open_color({ 0.8F, 0.8F, 0.8F })
                      .label("section")
                      .place(Forma::create_buffer(native, Portal::Graphics::PrimitiveTopology::TRIANGLE_LIST,
                                 std::vector<std::pair<std::string, std::shared_ptr<Core::VKImage>>> { { "text", nullptr } }),
                          surface, cursor, -1.F, 1.F, 2.F);
    ASSERT_NE(header.buf, nullptr);

    root = std::make_shared<Buffers::RootGraphicsBuffer>();
    root->initialize();
    root->add_child_buffer(header.buf);

    present();
    const auto closed = corner_pixel();

    scripted->move_to_ndc({ 0.F, 0.F });
    scripted->press(IO::MouseButtons::Left);
    scripted->release(IO::MouseButtons::Left);
    ASSERT_TRUE(header.open->value);
    present();
    const auto opened = corner_pixel();

    EXPECT_GT(opened[0], closed[0] + 64U);

    scripted->press(IO::MouseButtons::Left);
    scripted->release(IO::MouseButtons::Left);
    ASSERT_FALSE(header.open->value);
    present();
    const auto closed_again = corner_pixel();

    EXPECT_LT(closed_again[0], opened[0] - 64U);
}

}

#endif
