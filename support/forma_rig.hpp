#pragma once

#include "frame_scheduler.hpp"
#include "scripted_window.hpp"
#include "text_test_support.hpp"
#include "vulkan_test_support.hpp"

#include "MayaFlux/Buffers/BufferManager.hpp"
#include "MayaFlux/Buffers/BufferProcessingChain.hpp"
#include "MayaFlux/Buffers/Forma/FormaBuffer.hpp"
#include "MayaFlux/Buffers/Shaders/RenderProcessor.hpp"
#include "MayaFlux/Nodes/NodeGraphManager.hpp"
#include "MayaFlux/Portal/Forma/Context.hpp"
#include "MayaFlux/Portal/Forma/Forma.hpp"
#include "MayaFlux/Portal/Graphics/ShaderFoundry.hpp"
#include "MayaFlux/Portal/Graphics/TextureLoom.hpp"
#include "MayaFlux/Portal/Text/TypeFaceFoundry.hpp"
#include "MayaFlux/Registry/BackendRegistry.hpp"
#include "MayaFlux/Registry/Service/BufferService.hpp"
#include "MayaFlux/Transitive/Memory/Persist.hpp"
#include "MayaFlux/Vruta/EventManager.hpp"

namespace MayaFlux::Test {

/**
 * @brief A Vulkan device fixture with Portal::Forma initialised over managers it owns.
 *
 * Brings up ShaderFoundry, TextureLoom and the default font, owns a frame
 * driven scheduler, node graph, buffer manager and event manager, and
 * initialises Portal::Forma over them in SetUp. TearDown shuts Forma down,
 * clears the persistent store and destroys every graphics buffer left in
 * the buffer manager.
 */
class FormaRigTest : public VulkanDeviceTest {
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

        Portal::Forma::shutdown();
        ASSERT_TRUE(start_forma());
    }

    void TearDown() override
    {
        Portal::Forma::shutdown();
        MayaFlux::internal::cleanup_persistent_store();

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
    }

    bool start_forma(std::shared_ptr<Core::WindowManager> windows = nullptr)
    {
        frames.scheduler->register_token_processor(Vruta::ProcessingToken::FRAME_ACCURATE,
            [this](const std::vector<std::shared_ptr<Vruta::Routine>>& tasks, uint64_t units) {
                for (uint64_t i = 0; i < units; ++i) {
                    for (const auto& routine : tasks) {
                        if (!routine || !routine->is_active())
                            continue;
                        if (!routine->requires_clock_sync() || frames.frame + i >= routine->next_execution())
                            routine->try_resume(frames.frame + i);
                    }
                }
            });
        return Portal::Forma::initialize(nodes, buffer_manager, frames.scheduler, events, std::move(windows));
    }

    Portal::Forma::Surface make_surface(const std::string& name)
    {
        Portal::Forma::SurfaceConfig config;
        config.window = window;
        config.name = name;
        config.detach_on_close = false;
        return Portal::Forma::create_surface(std::move(config));
    }

    FrameScheduler frames;
    std::shared_ptr<Nodes::NodeGraphManager> nodes = std::make_shared<Nodes::NodeGraphManager>();
    std::shared_ptr<Buffers::BufferManager> buffer_manager = std::make_shared<Buffers::BufferManager>();
    std::shared_ptr<Vruta::EventManager> events = std::make_shared<Vruta::EventManager>();
    std::shared_ptr<ScriptedWindow> window = std::make_shared<ScriptedWindow>(400U, 300U);

private:
    bool m_owns_text {};
    static inline bool s_ready {};
};

}
