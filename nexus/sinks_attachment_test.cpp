#include "../support/test_metadata.hpp"
#include "gtest/gtest.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "../support/nexus_support.hpp"

#include "../support/scoped_working_dir.hpp"
#include "MayaFlux/IO/Image/STBImageWriter.hpp"
#include "MayaFlux/Buffers/Textures/TextureBuffer.hpp"
#include "MayaFlux/Buffers/AudioBuffer.hpp"
#include "MayaFlux/Buffers/BufferManager.hpp"
#include "MayaFlux/Buffers/BufferProcessingChain.hpp"
#include "MayaFlux/Buffers/Geometry/MeshBuffer.hpp"
#include "MayaFlux/Buffers/Shaders/RenderProcessor.hpp"
#include "MayaFlux/Buffers/VKBuffer.hpp"
#include "MayaFlux/Core/Backends/Graphics/Vulkan/VKImage.hpp"
#include "MayaFlux/IO/Detail/IOTransitions.hpp"
#include "MayaFlux/IO/Image/EXRWriter.hpp"
#include "MayaFlux/Kakshya/NDData/MeshAssemblyData.hpp"
#include "MayaFlux/Kakshya/NDData/MeshData.hpp"
#include "MayaFlux/Kakshya/NDData/VertexFormats.hpp"
#include "MayaFlux/Nexus/Expanses/Hull.hpp"
#include "MayaFlux/Nexus/Expanses/Mantle.hpp"
#include "MayaFlux/Nexus/Fabric.hpp"
#include "MayaFlux/Nexus/Pheme/Attachment.hpp"
#include "MayaFlux/Nexus/Pheme/Sinks.hpp"
#include "MayaFlux/Nexus/Pheme/Survey.hpp"
#include "MayaFlux/Nexus/Principals/Agent.hpp"
#include "MayaFlux/Nexus/Principals/Locus.hpp"
#include "MayaFlux/Nexus/State/Decoder.hpp"
#include "MayaFlux/Nexus/State/Encoder.hpp"
#include "MayaFlux/Nexus/Tapestry.hpp"
#include "MayaFlux/Portal/Graphics/GraphicsUtils.hpp"
#include "MayaFlux/Portal/Graphics/ImageTarget.hpp"
#include "MayaFlux/Portal/Graphics/ShaderFoundry.hpp"
#include "MayaFlux/Portal/Graphics/TextureLoom.hpp"
#include "MayaFlux/Transitive/IO/JSONSerializer.hpp"
#include "MayaFlux/Vruta/EventManager.hpp"
#include "MayaFlux/Vruta/Scheduler.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Nexus::AttachConfig;
    using Nexus::InfluenceContext;
    using Nexus::PerceptionContext;

    // =========================================================================
    // Audio sinks
    // =========================================================================

    class NexusAudioSinkTest : public ::testing::Test {
    protected:
        [[nodiscard]] size_t count(uint32_t channel) const
        {
            return manager.get_buffers(Buffers::ProcessingToken::AUDIO_BACKEND, channel).size();
        }

        [[nodiscard]] bool registered(const std::shared_ptr<Buffers::AudioBuffer>& buffer, uint32_t channel) const
        {
            return contains(manager.get_buffers(Buffers::ProcessingToken::AUDIO_BACKEND, channel), buffer);
        }

        std::shared_ptr<Buffers::BufferManager> manager_handle = std::make_shared<Buffers::BufferManager>();
        Buffers::BufferManager& manager = *manager_handle;
        std::shared_ptr<Vruta::TaskScheduler> scheduler = std::make_shared<Vruta::TaskScheduler>(48000);
        std::shared_ptr<Vruta::EventManager> events = std::make_shared<Vruta::EventManager>();
        std::vector<Nexus::AudioSink> sinks;
    };

    TEST_F(NexusAudioSinkTest, AddingASinkRegistersItsBufferOnTheChannel)
    {
        const size_t before = count(1);
        Nexus::add_audio_sink(sinks, manager, 1);

        ASSERT_EQ(sinks.size(), 1U);
        EXPECT_EQ(count(1), before + 1);
        EXPECT_TRUE(registered(sinks.front().buf, 1));
        EXPECT_EQ(sinks.front().channel, 1U);
    }

    TEST_F(NexusAudioSinkTest, RemovingASinkDeregistersItsBuffer)
    {
        const size_t before = count(1);
        Nexus::add_audio_sink(sinks, manager, 1);
        const auto buffer = sinks.front().buf;

        Nexus::remove_audio_sink(sinks, manager, 1);

        EXPECT_TRUE(sinks.empty());
        EXPECT_EQ(count(1), before);
        EXPECT_FALSE(registered(buffer, 1));
    }

    TEST_F(NexusAudioSinkTest, RemovingAnUnknownChannelChangesNothing)
    {
        Nexus::add_audio_sink(sinks, manager, 1);
        const size_t before = count(1);

        Nexus::remove_audio_sink(sinks, manager, 0);

        EXPECT_EQ(sinks.size(), 1U);
        EXPECT_EQ(count(1), before);
    }

    TEST_F(NexusAudioSinkTest, SinksOnDifferentChannelsAreIndependent)
    {
        Nexus::add_audio_sink(sinks, manager, 0);
        Nexus::add_audio_sink(sinks, manager, 1);
        const auto first = sinks.at(0).buf;
        const auto second = sinks.at(1).buf;

        Nexus::remove_audio_sink(sinks, manager, 0);

        ASSERT_EQ(sinks.size(), 1U);
        EXPECT_FALSE(registered(first, 0));
        EXPECT_TRUE(registered(second, 1));
    }

    TEST_F(NexusAudioSinkTest, DispatchCallsEachProducerOnce)
    {
        int calls = 0;
        Nexus::add_audio_sink(sinks, manager, 0, [&calls](const InfluenceContext&) {
            ++calls;
            return Kakshya::DataVariant { std::vector<double> { 0.1, 0.2, 0.3 } };
        });
        Nexus::add_audio_sink(sinks, manager, 1);

        Nexus::dispatch_audio_sinks(sinks, InfluenceContext {});

        EXPECT_EQ(calls, 1);
    }

    TEST_F(NexusAudioSinkTest, DispatchAcceptsAProducerThatReturnsAnotherSampleType)
    {
        int calls = 0;
        Nexus::add_audio_sink(sinks, manager, 0, [&calls](const InfluenceContext&) {
            ++calls;
            return Kakshya::DataVariant { std::vector<float> { 0.5F, -0.5F } };
        });

        EXPECT_NO_THROW(Nexus::dispatch_audio_sinks(sinks, InfluenceContext {}));
        EXPECT_EQ(calls, 1);
    }

    TEST_F(NexusAudioSinkTest, TheProducerReceivesTheContext)
    {
        glm::vec3 seen {};
        Nexus::add_audio_sink(sinks, manager, 0, [&seen](const InfluenceContext& ctx) {
            seen = ctx.position;
            return Kakshya::DataVariant { std::vector<double> { 0.0 } };
        });

        InfluenceContext ctx;
        ctx.position = { 1.0F, 2.0F, 3.0F };
        Nexus::dispatch_audio_sinks(sinks, ctx);

        expect_near(seen, ctx.position);
    }

    TEST_F(NexusAudioSinkTest, ASinkRegistersItsProducerName)
    {
        Nexus::add_audio_sink(
            sinks, manager, 0,
            [](const InfluenceContext&) { return Kakshya::DataVariant { std::vector<double> {} }; },
            "tone");

        EXPECT_EQ(sinks.front().fn_name, "tone");
    }

    TEST_F(NexusAudioSinkTest, PushingSamplesNeedsNoProducer)
    {
        Nexus::add_audio_sink(sinks, manager, 0);
        const std::vector<double> samples { 0.25, 0.5 };

        EXPECT_NO_THROW(Nexus::push_audio_data(sinks, samples));
        EXPECT_NO_THROW(Nexus::dispatch_audio_sinks(sinks, InfluenceContext {}));
    }

    TEST_F(NexusAudioSinkTest, AnAgentRegistersAndRemovesItsAudioSink)
    {
        Nexus::Agent agent(1.0F);
        const size_t before = count(1);

        agent.sink_audio(manager, 1);
        ASSERT_EQ(agent.audio_sinks().size(), 1U);
        EXPECT_EQ(count(1), before + 1);

        agent.remove_audio_sink(manager, 1);
        EXPECT_TRUE(agent.audio_sinks().empty());
        EXPECT_EQ(count(1), before);
    }

    TEST_F(NexusAudioSinkTest, AnAgentDroppedOutsideAFabricLeavesItsAudioSinkRegistered)
    {
        const size_t before = count(1);
        {
            Nexus::Agent agent(1.0F);
            agent.sink_audio(manager, 1);
        }

        EXPECT_EQ(count(1), before + 1);
    }

    TEST_F(NexusAudioSinkTest, RemovingAnAgentFromItsFabricUnregistersItsAudioSink)
    {
        Nexus::Fabric fabric(scheduler, events, manager_handle, 1.0F);
        const size_t before = count(1);
        auto agent = std::make_shared<Nexus::Agent>(1.0F);
        agent->sink_audio(manager, 1);
        fabric.wire(agent).finalise();
        ASSERT_EQ(count(1), before + 1);

        fabric.remove(fabric.all_ids().front());

        EXPECT_EQ(count(1), before);
        EXPECT_TRUE(agent->audio_sinks().empty());
    }

    TEST_F(NexusAudioSinkTest, RemovingAnEmitterFromItsFabricUnregistersItsAudioSink)
    {
        Nexus::Fabric fabric(scheduler, events, manager_handle, 1.0F);
        const size_t before = count(0);
        auto emitter = std::make_shared<Nexus::Emitter>();
        emitter->sink_audio(manager, 0);
        fabric.wire(emitter).finalise();

        fabric.remove(fabric.all_ids().front());

        EXPECT_EQ(count(0), before);
        EXPECT_TRUE(emitter->audio_sinks().empty());
    }

    TEST_F(NexusAudioSinkTest, DestroyingAFabricUnregistersTheAudioSinksOfItsEntities)
    {
        const size_t before = count(1);
        auto agent = std::make_shared<Nexus::Agent>(1.0F);
        agent->sink_audio(manager, 1);
        {
            Nexus::Fabric fabric(scheduler, events, manager_handle, 1.0F);
            fabric.wire(agent).finalise();
        }

        EXPECT_EQ(count(1), before);
        EXPECT_TRUE(agent->audio_sinks().empty());
    }

    TEST_F(NexusAudioSinkTest, RemovingAFabricFromItsTapestryUnregistersTheAudioSinksOfItsEntities)
    {
        const auto shared = std::make_shared<Buffers::BufferManager>();
        auto tapestry = std::make_shared<Nexus::Tapestry>(
            std::make_shared<Vruta::TaskScheduler>(48000), std::make_shared<Vruta::EventManager>(), shared);
        const auto fabric = tapestry->create_fabric();
        const size_t before = shared->get_buffers(Buffers::ProcessingToken::AUDIO_BACKEND, 1).size();
        auto agent = std::make_shared<Nexus::Agent>(1.0F);
        agent->sink_audio(*shared, 1);
        fabric->wire(agent).finalise();

        tapestry->remove_fabric(fabric);

        EXPECT_EQ(shared->get_buffers(Buffers::ProcessingToken::AUDIO_BACKEND, 1).size(), before);
        EXPECT_TRUE(fabric->all_ids().empty());
    }

    // =========================================================================
    // Render sinks
    // =========================================================================

    class NexusRenderSinkTest : public NexusDeviceTest {
    protected:
        std::shared_ptr<Vruta::TaskScheduler> scheduler = std::make_shared<Vruta::TaskScheduler>(48000);
        std::shared_ptr<Vruta::EventManager> events = std::make_shared<Vruta::EventManager>();
        std::vector<Nexus::RenderSink> sinks;
    };

    MAYAFLUX_TEST_WITH(TEST_F, NexusRenderSinkTest, AddingASinkRegistersItsBufferWithTheGraphicsToken, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        Nexus::add_render_sink(sinks, manager, render_config());

        ASSERT_EQ(sinks.size(), 1U);
        EXPECT_TRUE(registered(sinks.front().buf));
        EXPECT_EQ(sinks.front().window, window);
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusRenderSinkTest, TheSinkRendererIsThePrimaryProcessorOfItsBuffer, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        Nexus::add_render_sink(sinks, manager, render_config());

        ASSERT_NE(sinks.front().renderer, nullptr);
        EXPECT_EQ(sinks.front().buf->get_render_processor(), sinks.front().renderer);
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusRenderSinkTest, RemovingASinkDeregistersItsBuffer, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        Nexus::add_render_sink(sinks, manager, render_config());
        const auto buffer = sinks.front().buf;

        Nexus::remove_render_sink(sinks, manager, window);

        EXPECT_TRUE(sinks.empty());
        EXPECT_FALSE(registered(buffer));
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusRenderSinkTest, RemovingAnUnknownWindowChangesNothing, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        Nexus::add_render_sink(sinks, manager, render_config());
        const auto other = std::make_shared<ScriptedWindow>(100U, 100U);

        Nexus::remove_render_sink(sinks, manager, other);

        EXPECT_EQ(sinks.size(), 1U);
        EXPECT_TRUE(registered(sinks.front().buf));
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusRenderSinkTest, AnImageTargetAloneIsADestinationOfTheSink, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        auto& loom = Portal::Graphics::get_texture_manager();
        loom.shutdown();
        ASSERT_TRUE(loom.initialize(std::shared_ptr<Core::VulkanBackend>(&backend(), [](Core::VulkanBackend*) { })));
        {
            const auto target = loom.create_image_target({ .width = 16, .height = 16 });
            ASSERT_NE(target, nullptr);
            auto config = render_config();
            config.target_window = nullptr;
            config.target_images = { target };

            Nexus::add_render_sink(sinks, manager, config);

            ASSERT_EQ(sinks.size(), 1U);
            EXPECT_EQ(sinks.front().window, nullptr);
            EXPECT_EQ(sinks.front().renderer->get_target_window(), nullptr);
            EXPECT_EQ(sinks.front().renderer->get_target_image(), target);
            EXPECT_EQ(sinks.front().renderer->destination_count(), 1U);
            EXPECT_TRUE(registered(sinks.front().buf));

            Nexus::release_render_sinks(sinks, manager);
            EXPECT_TRUE(sinks.empty());
        }
        loom.shutdown();
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusRenderSinkTest, AWindowAndAnImageTargetAreBothDestinationsOfTheSink, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        auto& loom = Portal::Graphics::get_texture_manager();
        loom.shutdown();
        ASSERT_TRUE(loom.initialize(std::shared_ptr<Core::VulkanBackend>(&backend(), [](Core::VulkanBackend*) { })));
        {
            const auto target = loom.create_image_target({ .width = 16, .height = 16 });
            ASSERT_NE(target, nullptr);
            auto config = render_config();
            config.target_images = { target };

            Nexus::add_render_sink(sinks, manager, config);

            ASSERT_EQ(sinks.size(), 1U);
            EXPECT_EQ(sinks.front().window, window);
            EXPECT_EQ(sinks.front().renderer->get_target_window(), window);
            EXPECT_EQ(sinks.front().renderer->get_target_image(), target);
            EXPECT_EQ(sinks.front().renderer->destination_count(), 2U);

            Nexus::remove_render_sink(sinks, manager, window);
            EXPECT_TRUE(sinks.empty());
        }
        loom.shutdown();
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusRenderSinkTest, TheSinkKeepsTheImageTargetsItDrawsInto, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        auto& loom = Portal::Graphics::get_texture_manager();
        loom.shutdown();
        ASSERT_TRUE(loom.initialize(std::shared_ptr<Core::VulkanBackend>(&backend(), [](Core::VulkanBackend*) { })));
        {
            const auto first = loom.create_image_target({ .width = 16, .height = 16 });
            const auto second = loom.create_image_target({ .width = 16, .height = 16 });
            ASSERT_NE(first, nullptr);
            ASSERT_NE(second, nullptr);
            auto config = render_config();
            config.target_window = nullptr;
            config.target_images = { first, second };

            Nexus::add_render_sink(sinks, manager, config);

            ASSERT_EQ(sinks.size(), 1U);
            ASSERT_EQ(sinks.front().images.size(), 2U);
            EXPECT_EQ(sinks.front().images.front(), first);
            EXPECT_EQ(sinks.front().images.back(), second);

            Nexus::release_render_sinks(sinks, manager);
        }
        loom.shutdown();
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusRenderSinkTest, ASinkWithOnlyAnImageTargetIsRemovedThroughThatTarget, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        auto& loom = Portal::Graphics::get_texture_manager();
        loom.shutdown();
        ASSERT_TRUE(loom.initialize(std::shared_ptr<Core::VulkanBackend>(&backend(), [](Core::VulkanBackend*) { })));
        {
            const auto target = loom.create_image_target({ .width = 16, .height = 16 });
            ASSERT_NE(target, nullptr);
            auto config = render_config();
            config.target_window = nullptr;
            config.target_images = { target };
            Nexus::add_render_sink(sinks, manager, config);
            const auto buffer = sinks.front().buf;

            Nexus::remove_render_sink(sinks, manager, target);

            EXPECT_TRUE(sinks.empty());
            EXPECT_FALSE(registered(buffer));
        }
        loom.shutdown();
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusRenderSinkTest, RemovingAnUnknownImageTargetChangesNothing, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        auto& loom = Portal::Graphics::get_texture_manager();
        loom.shutdown();
        ASSERT_TRUE(loom.initialize(std::shared_ptr<Core::VulkanBackend>(&backend(), [](Core::VulkanBackend*) { })));
        {
            const auto target = loom.create_image_target({ .width = 16, .height = 16 });
            const auto other = loom.create_image_target({ .width = 16, .height = 16 });
            ASSERT_NE(target, nullptr);
            ASSERT_NE(other, nullptr);
            auto config = render_config();
            config.target_window = nullptr;
            config.target_images = { target };
            Nexus::add_render_sink(sinks, manager, config);

            Nexus::remove_render_sink(sinks, manager, other);
            Nexus::remove_render_sink(sinks, manager, std::shared_ptr<Portal::Graphics::ImageTarget> {});

            EXPECT_EQ(sinks.size(), 1U);
            EXPECT_TRUE(registered(sinks.front().buf));

            Nexus::release_render_sinks(sinks, manager);
        }
        loom.shutdown();
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusRenderSinkTest, OnlyTheSinkDrawingIntoTheTargetIsRemovedWhenSeveralExist, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        auto& loom = Portal::Graphics::get_texture_manager();
        loom.shutdown();
        ASSERT_TRUE(loom.initialize(std::shared_ptr<Core::VulkanBackend>(&backend(), [](Core::VulkanBackend*) { })));
        {
            const auto first = loom.create_image_target({ .width = 16, .height = 16 });
            const auto second = loom.create_image_target({ .width = 16, .height = 16 });
            ASSERT_NE(first, nullptr);
            ASSERT_NE(second, nullptr);
            auto config = render_config();
            config.target_window = nullptr;
            config.target_images = { first };
            Nexus::add_render_sink(sinks, manager, config);
            config.target_images = { second };
            Nexus::add_render_sink(sinks, manager, config);
            const auto kept = sinks.back().buf;

            Nexus::remove_render_sink(sinks, manager, first);

            ASSERT_EQ(sinks.size(), 1U);
            EXPECT_EQ(sinks.front().buf, kept);
            EXPECT_TRUE(registered(kept));

            Nexus::release_render_sinks(sinks, manager);
        }
        loom.shutdown();
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusRenderSinkTest, ASinkWithAWindowAndAnImageIsRemovedThroughTheImageToo, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        auto& loom = Portal::Graphics::get_texture_manager();
        loom.shutdown();
        ASSERT_TRUE(loom.initialize(std::shared_ptr<Core::VulkanBackend>(&backend(), [](Core::VulkanBackend*) { })));
        {
            const auto target = loom.create_image_target({ .width = 16, .height = 16 });
            ASSERT_NE(target, nullptr);
            auto config = render_config();
            config.target_images = { target };
            Nexus::add_render_sink(sinks, manager, config);

            Nexus::remove_render_sink(sinks, manager, target);

            EXPECT_TRUE(sinks.empty());
        }
        loom.shutdown();
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusRenderSinkTest, AnAgentFindsAndRemovesAnImageOnlySinkByItsTarget, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        auto& loom = Portal::Graphics::get_texture_manager();
        loom.shutdown();
        ASSERT_TRUE(loom.initialize(std::shared_ptr<Core::VulkanBackend>(&backend(), [](Core::VulkanBackend*) { })));
        {
            const auto target = loom.create_image_target({ .width = 16, .height = 16 });
            const auto other = loom.create_image_target({ .width = 16, .height = 16 });
            ASSERT_NE(target, nullptr);
            ASSERT_NE(other, nullptr);
            auto config = render_config();
            config.target_window = nullptr;
            config.target_images = { target };

            Nexus::Agent agent(1.0F);
            agent.render(manager, config);

            ASSERT_EQ(agent.render_sinks().size(), 1U);
            EXPECT_EQ(agent.get_render_processor(target), agent.render_sinks().front().renderer);
            EXPECT_EQ(agent.get_render_processor(other), nullptr);

            agent.remove_render(manager, other);
            EXPECT_EQ(agent.render_sinks().size(), 1U);

            agent.remove_render(manager, target);
            EXPECT_TRUE(agent.render_sinks().empty());
            EXPECT_EQ(agent.get_render_processor(target), nullptr);
        }
        loom.shutdown();
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusRenderSinkTest, AnEmitterFindsAndRemovesAnImageOnlySinkByItsTarget, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        auto& loom = Portal::Graphics::get_texture_manager();
        loom.shutdown();
        ASSERT_TRUE(loom.initialize(std::shared_ptr<Core::VulkanBackend>(&backend(), [](Core::VulkanBackend*) { })));
        {
            const auto target = loom.create_image_target({ .width = 16, .height = 16 });
            ASSERT_NE(target, nullptr);
            auto config = render_config();
            config.target_window = nullptr;
            config.target_images = { target };

            Nexus::Emitter emitter;
            emitter.render(manager, config, "image_sink", {});

            ASSERT_EQ(emitter.render_sinks().size(), 1U);
            EXPECT_EQ(emitter.get_render_processor(target), emitter.render_sinks().front().renderer);

            emitter.remove_render(manager, target);
            EXPECT_TRUE(emitter.render_sinks().empty());
        }
        loom.shutdown();
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusRenderSinkTest, EveryTopologyBuildsASink, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        for (const auto topology : {
                 Portal::Graphics::PrimitiveTopology::POINT_LIST,
                 Portal::Graphics::PrimitiveTopology::LINE_LIST,
                 Portal::Graphics::PrimitiveTopology::LINE_STRIP,
                 Portal::Graphics::PrimitiveTopology::TRIANGLE_LIST,
                 Portal::Graphics::PrimitiveTopology::TRIANGLE_STRIP,
             }) {
            auto config = render_config();
            config.topology = topology;
            std::vector<Nexus::RenderSink> local;

            Nexus::add_render_sink(local, manager, config);

            ASSERT_EQ(local.size(), 1U);
            EXPECT_NE(local.front().renderer, nullptr);
            Nexus::remove_render_sink(local, manager, window);
        }
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusRenderSinkTest, DispatchCallsEachProducerWithTheContext, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        glm::vec3 seen {};
        int calls = 0;
        Nexus::add_render_sink(sinks, manager, render_config(), [&](const InfluenceContext& ctx) {
            ++calls;
            seen = ctx.position;
        });

        InfluenceContext ctx;
        ctx.position = { 4.0F, 5.0F, 6.0F };
        Nexus::dispatch_render_sinks(sinks, ctx);

        EXPECT_EQ(calls, 1);
        expect_near(seen, ctx.position);
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusRenderSinkTest, DispatchSkipsASinkWithoutAProducer, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        Nexus::add_render_sink(sinks, manager, render_config(), {}, {}, glm::vec3(1.0F));

        EXPECT_NO_THROW(Nexus::dispatch_render_sinks(sinks, InfluenceContext {}));
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusRenderSinkTest, AnAgentFindsTheRendererOfItsSinkByWindow, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        Nexus::Agent agent(1.0F);
        agent.render(manager, render_config());

        ASSERT_EQ(agent.render_sinks().size(), 1U);
        EXPECT_EQ(agent.get_render_processor(window), agent.render_sinks().front().renderer);
        EXPECT_EQ(agent.get_render_processor(std::make_shared<ScriptedWindow>(10U, 10U)), nullptr);

        agent.remove_render(manager, window);
        EXPECT_TRUE(agent.render_sinks().empty());
        EXPECT_EQ(agent.get_render_processor(window), nullptr);
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusRenderSinkTest, AnAgentDroppedOutsideAFabricLeavesItsRenderSinkRegistered, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        std::shared_ptr<Buffers::VKBuffer> buffer;
        {
            Nexus::Agent agent(1.0F);
            agent.render(manager, render_config());
            buffer = agent.render_sinks().front().buf;
        }

        EXPECT_TRUE(registered(buffer));
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusRenderSinkTest, RemovingAnAgentFromItsFabricUnregistersItsRenderSink, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        Nexus::Fabric fabric(scheduler, events, manager_handle, 1.0F);
        auto agent = std::make_shared<Nexus::Agent>(1.0F);
        agent->render(manager, render_config());
        const auto buffer = agent->render_sinks().front().buf;
        fabric.wire(agent).finalise();
        ASSERT_TRUE(registered(buffer));

        fabric.remove(fabric.all_ids().front());

        EXPECT_FALSE(registered(buffer));
        EXPECT_TRUE(agent->render_sinks().empty());
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusRenderSinkTest, RemovingAnEmitterFromItsFabricUnregistersItsRenderSink, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        Nexus::Fabric fabric(scheduler, events, manager_handle, 1.0F);
        auto emitter = std::make_shared<Nexus::Emitter>();
        emitter->render(manager, render_config());
        const auto buffer = emitter->render_sinks().front().buf;
        fabric.wire(emitter).finalise();

        fabric.remove(fabric.all_ids().front());

        EXPECT_FALSE(registered(buffer));
        EXPECT_TRUE(emitter->render_sinks().empty());
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusRenderSinkTest, DestroyingAFabricUnregistersTheRenderSinksOfItsEntities, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        auto agent = std::make_shared<Nexus::Agent>(1.0F);
        agent->render(manager, render_config());
        const auto buffer = agent->render_sinks().front().buf;
        {
            Nexus::Fabric fabric(scheduler, events, manager_handle, 1.0F);
            fabric.wire(agent).finalise();
        }

        EXPECT_FALSE(registered(buffer));
    }

    // =========================================================================
    // Survey
    // =========================================================================

    class NexusSurveyTest : public ::testing::Test {
    protected:
        static std::shared_ptr<Buffers::VKBuffer> mesh_buffer(Kakshya::MeshData data)
        {
            return std::make_shared<Buffers::MeshBuffer>(std::move(data));
        }
    };

    TEST_F(NexusSurveyTest, AnchorOfAMeshWithFacesIsItsSurfaceCentroid)
    {
        const auto anchor = Nexus::read_anchor(mesh_buffer(two_triangle_mesh()), std::nullopt);

        ASSERT_TRUE(anchor.has_value());
        expect_near(*anchor, { 1.862745F, 1.274510F, 0.0F });
    }

    TEST_F(NexusSurveyTest, AnchorOfAMeshWithoutFacesIsTheVertexMean)
    {
        const auto data = make_mesh(
            { { 0.0F, 0.0F, 0.0F }, { 2.0F, 0.0F, 0.0F }, { 0.0F, 2.0F, 0.0F }, { 0.0F, 0.0F, 2.0F } }, {});
        const auto anchor = Nexus::read_anchor(mesh_buffer(data), std::nullopt);

        ASSERT_TRUE(anchor.has_value());
        expect_near(*anchor, { 0.5F, 0.5F, 0.5F });
    }

    TEST_F(NexusSurveyTest, AnchorAtAnIndexIsThatVertex)
    {
        const auto anchor = Nexus::read_anchor(mesh_buffer(triangle_mesh()), 1U);

        ASSERT_TRUE(anchor.has_value());
        expect_near(*anchor, { 4.0F, 0.0F, 0.0F });
    }

    TEST_F(NexusSurveyTest, AnchorAtAnIndexBeyondTheVerticesCannotBeRead)
    {
        EXPECT_FALSE(Nexus::read_anchor(mesh_buffer(triangle_mesh()), 3U).has_value());
    }

    TEST_F(NexusSurveyTest, ABufferThatHoldsNoMeshCannotBeRead)
    {
        const auto buffer = std::make_shared<Buffers::VKBuffer>(
            256, Buffers::VKBuffer::Usage::VERTEX, Kakshya::DataModality::VERTICES_3D);

        EXPECT_FALSE(Nexus::read_anchor(buffer, std::nullopt).has_value());
        EXPECT_FALSE(Nexus::read_bounds(buffer).has_value());
    }

    TEST_F(NexusSurveyTest, BoundsOfAMeshCoverEveryVertex)
    {
        const auto bounds = Nexus::read_bounds(mesh_buffer(two_triangle_mesh()));

        ASSERT_TRUE(bounds.has_value());
        expect_near(bounds->min, { 0.0F, 0.0F, 0.0F });
        expect_near(bounds->max, { 11.0F, 4.0F, 0.0F });
    }

    TEST_F(NexusSurveyTest, PlacementOfABufferWithoutARendererIsIdentity)
    {
        const auto buffer = mesh_buffer(triangle_mesh());

        EXPECT_EQ(Nexus::read_placement(buffer), glm::mat4(1.0F));
    }

    // =========================================================================
    // Attachment
    // =========================================================================

    class NexusAttachmentTest : public NexusDeviceTest {
    protected:
        void SetUp() override
        {
            NexusDeviceTest::SetUp();
            if (IsSkipped())
                return;

            buffer = std::make_shared<Buffers::MeshBuffer>(triangle_mesh());
            renderer = std::make_shared<Buffers::RenderProcessor>(Buffers::ShaderConfig { "triangle.vert.spv" });
            buffer->set_render_processor(renderer);
        }

        Nexus::Attachment attach(const AttachConfig& config = {})
        {
            return Nexus::make_attachment(buffer, config, std::nullopt);
        }

        std::shared_ptr<Vruta::TaskScheduler> scheduler = std::make_shared<Vruta::TaskScheduler>(48000);
        std::shared_ptr<Vruta::EventManager> events = std::make_shared<Vruta::EventManager>();
        std::shared_ptr<Buffers::MeshBuffer> buffer;
        std::shared_ptr<Buffers::RenderProcessor> renderer;
    };

    MAYAFLUX_TEST_WITH(TEST_F, NexusAttachmentTest, AnAttachmentAnchorsAtTheSurfaceCentroid, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        const auto attachment = attach();

        expect_near(attachment.anchor, { 4.0F / 3.0F, 4.0F / 3.0F, 0.0F });
        expect_near(attachment.applied, attachment.anchor);
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusAttachmentTest, AnAttachmentAnchorsAtTheIndexedVertex, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        AttachConfig config;
        config.index = 1U;

        expect_near(attach(config).anchor, { 4.0F, 0.0F, 0.0F });
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusAttachmentTest, AnAttachmentToAnUnreadableBufferAnchorsAtTheOrigin, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        const auto empty = std::make_shared<Buffers::VKBuffer>(
            256, Buffers::VKBuffer::Usage::VERTEX, Kakshya::DataModality::VERTICES_3D);

        const auto attachment = Nexus::make_attachment(empty, {}, std::nullopt);

        expect_near(attachment.anchor, { 0.0F, 0.0F, 0.0F });
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusAttachmentTest, TheOffsetIsAddedToTheAppliedPosition, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        AttachConfig config;
        config.offset = { 0.0F, 0.0F, 1.0F };

        const auto attachment = attach(config);

        expect_near(attachment.applied, attachment.anchor + glm::vec3(0.0F, 0.0F, 1.0F));
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusAttachmentTest, TheOffsetTurnsWithTheOrientation, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        AttachConfig config;
        config.offset = { 0.0F, 0.0F, 1.0F };
        const auto turn = glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0F, 1.0F, 0.0F));

        const auto attachment = Nexus::make_attachment(buffer, config, turn);

        expect_near(attachment.applied, attachment.anchor + glm::vec3(1.0F, 0.0F, 0.0F));
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusAttachmentTest, PlacingInstallsASourceThatReadsTheTransform, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        auto attachment = attach();
        EXPECT_FALSE(static_cast<bool>(renderer->get_geometry_transform_source()));

        Nexus::place_attachment(attachment);
        *attachment.transform = glm::translate(glm::mat4(1.0F), { 1.0F, 2.0F, 3.0F });

        EXPECT_TRUE(static_cast<bool>(renderer->get_geometry_transform_source()));
        expect_near(placed(buffer, { 0.0F, 0.0F, 0.0F }), { 1.0F, 2.0F, 3.0F });
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusAttachmentTest, PlacingTwiceFollowsTheProcessorOnce, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        auto attachment = attach();

        Nexus::place_attachment(attachment);
        Nexus::place_attachment(attachment);

        EXPECT_EQ(attachment.followed.size(), 1U);
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusAttachmentTest, ReleasingRestoresAFixedTransform, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        const auto fixed = glm::scale(glm::mat4(1.0F), { 2.0F, 2.0F, 2.0F });
        renderer->set_geometry_transform(fixed);
        auto attachment = attach();
        Nexus::place_attachment(attachment);
        *attachment.transform = glm::translate(glm::mat4(1.0F), { 5.0F, 0.0F, 0.0F });
        expect_near(placed(buffer, { 1.0F, 0.0F, 0.0F }), { 7.0F, 0.0F, 0.0F });

        Nexus::release_attachment(attachment);

        EXPECT_EQ(Nexus::read_placement(buffer), fixed);
        EXPECT_TRUE(attachment.followed.empty());
        EXPECT_EQ(*attachment.transform, glm::mat4(1.0F));
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusAttachmentTest, ReleasingRestoresTheAbsenceOfATransform, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        auto attachment = attach();
        Nexus::place_attachment(attachment);

        Nexus::release_attachment(attachment);

        EXPECT_FALSE(static_cast<bool>(renderer->get_geometry_transform_source()));
        EXPECT_EQ(Nexus::read_placement(buffer), glm::mat4(1.0F));
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusAttachmentTest, ReleasingRestoresATransformSource, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        renderer->set_geometry_transform_source([] { return glm::translate(glm::mat4(1.0F), { 0.0F, 9.0F, 0.0F }); });
        auto attachment = attach();
        Nexus::place_attachment(attachment);

        Nexus::release_attachment(attachment);

        expect_near(placed(buffer, { 0.0F, 0.0F, 0.0F }), { 0.0F, 9.0F, 0.0F });
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusAttachmentTest, LaterAttachmentsStackOutsideEarlierOnes, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        auto first = attach();
        auto second = attach();
        Nexus::place_attachment(first);
        Nexus::place_attachment(second);
        *first.transform = glm::translate(glm::mat4(1.0F), { 1.0F, 0.0F, 0.0F });
        *second.transform = glm::scale(glm::mat4(1.0F), { 2.0F, 2.0F, 2.0F });

        expect_near(placed(buffer, { 0.0F, 0.0F, 0.0F }), { 2.0F, 0.0F, 0.0F });
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusAttachmentTest, EachAttachmentIsReleasedOnItsOwn, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        auto first = attach();
        auto second = attach();
        Nexus::place_attachment(first);
        Nexus::place_attachment(second);
        *first.transform = glm::translate(glm::mat4(1.0F), { 1.0F, 0.0F, 0.0F });
        *second.transform = glm::scale(glm::mat4(1.0F), { 2.0F, 2.0F, 2.0F });

        Nexus::release_attachment(first);
        expect_near(placed(buffer, { 1.0F, 0.0F, 0.0F }), { 2.0F, 0.0F, 0.0F });
        EXPECT_TRUE(static_cast<bool>(renderer->get_geometry_transform_source()));

        Nexus::release_attachment(second);
        EXPECT_FALSE(static_cast<bool>(renderer->get_geometry_transform_source()));
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusAttachmentTest, AnInfluenceMovesTheAnchorToTheContextPosition, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        auto attachment = attach();
        InfluenceContext ctx;
        ctx.position = { 5.0F, 0.0F, 0.0F };

        Nexus::apply_attachment(attachment, ctx);

        expect_near(placed(buffer, attachment.anchor), ctx.position);
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusAttachmentTest, TheOffsetIsLeftBehindTheContextPosition, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        AttachConfig config;
        config.offset = { 0.0F, 1.0F, 0.0F };
        auto attachment = attach(config);
        InfluenceContext ctx;
        ctx.position = { 5.0F, 0.0F, 0.0F };

        Nexus::apply_attachment(attachment, ctx);

        expect_near(placed(buffer, attachment.anchor), { 5.0F, -1.0F, 0.0F });
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusAttachmentTest, AMoveFromOutsideBecomesTheOffset, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        auto attachment = attach();
        std::optional<glm::vec3> position = attachment.applied + glm::vec3(2.0F, 0.0F, 0.0F);

        Nexus::follow_attachment(attachment, position, std::nullopt);

        expect_near(attachment.config.offset, { 2.0F, 0.0F, 0.0F });
        EXPECT_EQ(*attachment.transform, glm::mat4(1.0F));
        expect_near(*position, attachment.anchor + glm::vec3(2.0F, 0.0F, 0.0F));
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusAttachmentTest, AMoveFromOutsideCanCarryTheBuffer, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        AttachConfig config;
        config.on_move = AttachConfig::OnMove::Carry;
        auto attachment = attach(config);
        std::optional<glm::vec3> position = attachment.applied + glm::vec3(2.0F, 0.0F, 0.0F);

        Nexus::follow_attachment(attachment, position, std::nullopt);

        expect_near(attachment.config.offset, { 0.0F, 0.0F, 0.0F });
        expect_near(glm::vec3(*attachment.transform * glm::vec4(0.0F, 0.0F, 0.0F, 1.0F)), { 2.0F, 0.0F, 0.0F });
        expect_near(*position, attachment.anchor + glm::vec3(2.0F, 0.0F, 0.0F));
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusAttachmentTest, FollowingWithoutAPositionSetsItFromTheBuffer, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        AttachConfig config;
        config.offset = { 0.0F, 0.0F, 1.0F };
        auto attachment = attach(config);
        std::optional<glm::vec3> position;

        Nexus::follow_attachment(attachment, position, std::nullopt);

        ASSERT_TRUE(position.has_value());
        expect_near(*position, attachment.anchor + glm::vec3(0.0F, 0.0F, 1.0F));
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusAttachmentTest, RecenteringMovesTheAnchorAndKeepsThePosition, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        auto attachment = attach();
        std::optional<glm::vec3> position = attachment.applied;
        const glm::vec3 kept = *position;
        buffer->set_mesh_data(make_mesh(
            { { 10.0F, 0.0F, 0.0F }, { 14.0F, 0.0F, 0.0F }, { 10.0F, 4.0F, 0.0F } }, { 0, 1, 2 }));

        Nexus::recenter_attachment(attachment, position, std::nullopt);

        expect_near(attachment.anchor, { 10.0F + 4.0F / 3.0F, 4.0F / 3.0F, 0.0F });
        expect_near(*position, kept);
        expect_near(attachment.config.offset, kept - attachment.anchor);
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusAttachmentTest, AnAgentAttachesToABufferAndTakesItsPosition, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        Nexus::Agent agent(1.0F);

        agent.attach(buffer);

        EXPECT_TRUE(agent.attached());
        ASSERT_TRUE(agent.position().has_value());
        expect_near(*agent.position(), { 4.0F / 3.0F, 4.0F / 3.0F, 0.0F });
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusAttachmentTest, AnAgentIgnoresANullBuffer, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        Nexus::Agent agent(1.0F);

        agent.attach(nullptr);

        EXPECT_FALSE(agent.attached());
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusAttachmentTest, AnAgentInfluencePlacesTheAttachedBuffer, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        Nexus::Agent agent(1.0F);
        agent.attach(buffer);
        InfluenceContext ctx;
        ctx.position = { 5.0F, 0.0F, 0.0F };

        agent.invoke_influence(ctx);

        expect_near(placed(buffer, { 4.0F / 3.0F, 4.0F / 3.0F, 0.0F }), ctx.position);
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusAttachmentTest, DetachingGivesTheBufferItsPlacementBack, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        Nexus::Agent agent(1.0F);
        agent.attach(buffer);
        InfluenceContext ctx;
        ctx.position = { 5.0F, 0.0F, 0.0F };
        agent.invoke_influence(ctx);

        agent.detach();

        EXPECT_FALSE(agent.attached());
        EXPECT_FALSE(static_cast<bool>(renderer->get_geometry_transform_source()));
        EXPECT_EQ(Nexus::read_placement(buffer), glm::mat4(1.0F));
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusAttachmentTest, AttachingAgainReplacesThePreviousAttachment, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        Nexus::Agent agent(1.0F);
        agent.attach(buffer);
        const auto other = std::make_shared<Buffers::MeshBuffer>(triangle_mesh());
        const auto other_renderer = std::make_shared<Buffers::RenderProcessor>(Buffers::ShaderConfig { "triangle.vert.spv" });
        other->set_render_processor(other_renderer);
        InfluenceContext ctx;
        agent.invoke_influence(ctx);
        ASSERT_TRUE(static_cast<bool>(renderer->get_geometry_transform_source()));

        agent.attach(other);

        EXPECT_FALSE(static_cast<bool>(renderer->get_geometry_transform_source()));
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusAttachmentTest, ADroppedAgentGivesTheBufferItsPlacementBack, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        {
            Nexus::Agent agent(1.0F);
            agent.attach(buffer);
            InfluenceContext ctx;
            ctx.position = { 5.0F, 0.0F, 0.0F };
            agent.invoke_influence(ctx);
            ASSERT_NE(Nexus::read_placement(buffer), glm::mat4(1.0F));
        }

        EXPECT_FALSE(static_cast<bool>(renderer->get_geometry_transform_source()));
        EXPECT_EQ(Nexus::read_placement(buffer), glm::mat4(1.0F));
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusAttachmentTest, ADroppedEmitterGivesTheBufferItsPlacementBack, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        {
            Nexus::Emitter emitter;
            emitter.attach(buffer);
            InfluenceContext ctx;
            ctx.position = { 5.0F, 0.0F, 0.0F };
            emitter.invoke(ctx);
            ASSERT_NE(Nexus::read_placement(buffer), glm::mat4(1.0F));
        }

        EXPECT_FALSE(static_cast<bool>(renderer->get_geometry_transform_source()));
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusAttachmentTest, RemovingAnAgentFromItsFabricGivesTheBufferItsPlacementBack, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        Nexus::Fabric fabric(scheduler, events, manager_handle, 1.0F);
        auto agent = std::make_shared<Nexus::Agent>(1.0F);
        agent->attach(buffer);
        fabric.wire(agent).finalise();
        fabric.commit();
        ASSERT_TRUE(static_cast<bool>(renderer->get_geometry_transform_source()));

        fabric.remove(fabric.all_ids().front());

        EXPECT_FALSE(agent->attached());
        EXPECT_FALSE(static_cast<bool>(renderer->get_geometry_transform_source()));
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusAttachmentTest, DestroyingAFabricGivesTheBuffersOfItsEntitiesTheirPlacementBack, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        auto agent = std::make_shared<Nexus::Agent>(1.0F);
        agent->attach(buffer);
        {
            Nexus::Fabric fabric(scheduler, events, manager_handle, 1.0F);
            fabric.wire(agent).finalise();
            fabric.commit();
            ASSERT_TRUE(static_cast<bool>(renderer->get_geometry_transform_source()));
        }

        EXPECT_FALSE(agent->attached());
        EXPECT_FALSE(static_cast<bool>(renderer->get_geometry_transform_source()));
    }

    // =========================================================================
    // Locus view targets
    // =========================================================================

    class NexusLocusViewTest : public NexusDeviceTest {
    protected:
        void SetUp() override
        {
            NexusDeviceTest::SetUp();
            if (IsSkipped())
                return;

            first = std::make_shared<Buffers::RenderProcessor>(Buffers::ShaderConfig { "triangle.vert.spv" });
            second = std::make_shared<Buffers::RenderProcessor>(Buffers::ShaderConfig { "triangle.vert.spv" });
            locus = std::make_unique<Nexus::Locus>(
                Kinesis::NavigationConfig {}, 4.0F,
                [](const PerceptionContext&) { },
                [this](const InfluenceContext&) { seen_at_influence = first->get_view_cell() != nullptr; });
        }

        std::shared_ptr<Buffers::RenderProcessor> first;
        std::shared_ptr<Buffers::RenderProcessor> second;
        std::unique_ptr<Nexus::Locus> locus;
        bool seen_at_influence {};
    };

    MAYAFLUX_TEST_WITH(TEST_F, NexusLocusViewTest, ANullTargetIsIgnored, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        locus->add_view_target(nullptr);

        EXPECT_TRUE(locus->view_targets().empty());
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusLocusViewTest, AddingTheSameTargetTwiceKeepsOne, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        locus->add_view_target(first);
        locus->add_view_target(first);

        EXPECT_EQ(locus->view_targets().size(), 1U);
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusLocusViewTest, RemovingATargetLeavesTheOthers, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        locus->add_view_target(first);
        locus->add_view_target(second);

        locus->remove_view_target(first);

        ASSERT_EQ(locus->view_targets().size(), 1U);
        EXPECT_EQ(locus->view_targets().front(), second);
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusLocusViewTest, EveryTargetDrawsFromTheLocusViewpoint, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        locus->add_view_target(first);
        locus->add_view_target(second);

        for (const auto& target : { first, second }) {
            EXPECT_EQ(target->get_view_cell(), locus->view());
        }
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusLocusViewTest, ARemovedTargetIsLeftAlone, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        locus->add_view_target(first);
        locus->add_view_target(second);
        locus->remove_view_target(second);

        EXPECT_EQ(first->get_view_cell(), locus->view());
        EXPECT_EQ(second->get_view_cell(), nullptr);
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusLocusViewTest, TheViewIsBoundWhenTheInfluenceFunctionRuns, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        locus->add_view_target(first);

        locus->invoke_influence(InfluenceContext {});

        EXPECT_TRUE(seen_at_influence);
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusLocusViewTest, ATurnedLocusMovesTheViewpoint, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        locus->add_view_target(first);
        const auto before = locus->view()->read().first;

        locus->nav().yaw += 0.5F;
        locus->invoke_perception(PerceptionContext {});

        EXPECT_NE(locus->view()->read().first, before);
    }

    class NexusStateDeviceTest : public NexusDeviceTest {
    protected:
        void SetUp() override
        {
            NexusDeviceTest::SetUp();
            if (IsSkipped())
                return;
            IO::EXRWriter::register_with_registry();
        }

        [[nodiscard]] std::shared_ptr<Buffers::MeshBuffer> drawn_mesh() const
        {
            auto buffer = std::make_shared<Buffers::MeshBuffer>(solid_mesh());
            buffer->set_render_processor(
                std::make_shared<Buffers::RenderProcessor>(Buffers::ShaderConfig { "triangle.vert.spv" }));
            return buffer;
        }

        ScopedWorkingDir cwd;
        std::shared_ptr<Vruta::TaskScheduler> scheduler = std::make_shared<Vruta::TaskScheduler>(48000);
        std::shared_ptr<Vruta::EventManager> events = std::make_shared<Vruta::EventManager>();
    };

    MAYAFLUX_TEST_WITH(TEST_F, NexusStateDeviceTest, AttachmentsSinksViewsAndBodiesAreRestored, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        auto bindings = std::make_shared<Nexus::State::Bindings>();
        bindings->windows["main"] = window;
        for (const char* name : { "held", "viewed", "hulled", "worn" }) {
            bindings->buffers[name] = drawn_mesh();
            bindings->assets[name] = { .file = "unused.glb" };
        }

        Nexus::Fabric source { scheduler, events, manager_handle, 1.0F };
        auto agent = std::make_shared<Nexus::Agent>(1.0F);
        source.wire(agent).finalise();
        agent->attach(bindings->buffers["held"], { .index = 1U, .slot = 0U, .on_move = AttachConfig::OnMove::Carry });
        auto locus = std::make_shared<Nexus::Locus>(
            Kinesis::NavigationConfig {}, 4.0F, [](const PerceptionContext&) { }, [](const InfluenceContext&) { });
        source.wire(std::shared_ptr<Nexus::Agent>(locus)).finalise();
        locus->add_view_target(bindings->buffers["viewed"]->get_render_processor());
        auto lamp = std::make_shared<Nexus::Emitter>();
        source.wire(lamp).finalise();
        lamp->render(manager, render_config());
        source.add_expanse(std::make_shared<Nexus::Hull>(bindings->buffers["hulled"]));
        source.add_expanse(std::make_shared<Nexus::Mantle>(
            Kinesis::AABB3D::from_center(glm::vec3(0.0F), glm::vec3(1.0F)), bindings->buffers["worn"]));

        const auto first = (std::filesystem::current_path() / "first").string();
        const auto second = (std::filesystem::current_path() / "second").string();
        Nexus::StateEncoder encoder(bindings);
        ASSERT_TRUE(encoder.encode(source, first)) << encoder.last_error();

        Nexus::Fabric target { scheduler, events, manager_handle, 1.0F };
        Nexus::StateDecoder decoder(bindings);
        static_cast<void>(decoder.reconstruct(target, first));
        ASSERT_TRUE(encoder.encode(target, second)) << encoder.last_error();

        IO::JSONSerializer serializer;
        const auto saved = serializer.read<Nexus::State::FabricSchema>(first + ".json");
        const auto again = serializer.read<Nexus::State::FabricSchema>(second + ".json");
        ASSERT_TRUE(saved && again);
        ASSERT_EQ(again->entities.size(), saved->entities.size());
        for (size_t i = 0; i < saved->entities.size(); ++i) {
            const auto& before = saved->entities[i];
            const auto& after = again->entities[i];
            ASSERT_EQ(after.attach.has_value(), before.attach.has_value());
            if (before.attach) {
                EXPECT_EQ(after.attach->buffer, "held");
                EXPECT_EQ(after.attach->index, 1U);
                EXPECT_EQ(after.attach->slot, 0U);
                EXPECT_EQ(after.attach->on_move, AttachConfig::OnMove::Carry);
            }
            EXPECT_EQ(after.view_targets, before.view_targets);
            EXPECT_EQ(after.render_sinks.size(), before.render_sinks.size());
        }
        ASSERT_EQ(saved->expanses.size(), 2U);
        ASSERT_EQ(again->expanses.size(), 2U);
        for (size_t i = 0; i < saved->expanses.size(); ++i) {
            EXPECT_EQ(again->expanses[i].subkind, saved->expanses[i].subkind);
            EXPECT_EQ(again->expanses[i].buffer, saved->expanses[i].buffer);
        }
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusStateDeviceTest, AnEmbeddedTextureIsBoundThroughTheMaterialResolver, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        auto& loom = Portal::Graphics::get_texture_manager();
        loom.shutdown();
        ASSERT_TRUE(loom.initialize(std::shared_ptr<Core::VulkanBackend>(&backend(), [](Core::VulkanBackend*) { })));
        {
            auto pixels = std::make_shared<Kakshya::ImageData>();
            pixels->width = 2;
            pixels->height = 1;
            pixels->channels = 4;
            pixels->format = Kakshya::ImageFormat::RGBA8;
            pixels->pixels = std::vector<uint8_t> { 255, 0, 0, 255, 0, 0, 255, 255 };

            Kakshya::MeshAssemblyMaps maps;
            maps.base_color = Kakshya::MeshAssemblyTexture { .path = "*0", .image = pixels };
            Kakshya::MeshAssemblyMaterial material { .name = "skin", .diffuse_path = "*0", .diffuse_embedded = true };
            material.facets.set(std::move(maps));

            const auto resolver = IO::Detail::make_material_resolver("scene.glb");
            const auto image = resolver(material);

            ASSERT_NE(image, nullptr);
            EXPECT_EQ(image->get_width(), 2U);
            EXPECT_EQ(image->get_height(), 1U);
            EXPECT_EQ(resolver(material), image);
        }
        loom.shutdown();
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusStateDeviceTest, ARenderSinkKeepsItsConfigAndAViewOfItComesBack, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        auto bindings = std::make_shared<Nexus::State::Bindings>();
        bindings->windows["main"] = window;
        Nexus::Fabric source { scheduler, events, manager_handle, 1.0F };
        auto lamp = std::make_shared<Nexus::Emitter>();
        source.wire(lamp).finalise();
        auto config = render_config();
        config.topology = Portal::Graphics::PrimitiveTopology::LINE_LIST;
        lamp->render(manager, config);
        auto locus = std::make_shared<Nexus::Locus>(
            Kinesis::NavigationConfig {}, 4.0F, [](const PerceptionContext&) { }, [](const InfluenceContext&) { });
        source.wire(std::shared_ptr<Nexus::Agent>(locus)).finalise();
        locus->add_view_target(lamp->render_sinks().front().renderer);

        const auto first = (std::filesystem::current_path() / "sinks").string();
        const auto second = (std::filesystem::current_path() / "sinks_again").string();
        Nexus::StateEncoder encoder(bindings);
        ASSERT_TRUE(encoder.encode(source, first)) << encoder.last_error();
        Nexus::Fabric target { scheduler, events, manager_handle, 1.0F };
        Nexus::StateDecoder decoder(bindings);
        static_cast<void>(decoder.reconstruct(target, first));
        ASSERT_TRUE(encoder.encode(target, second)) << encoder.last_error();

        IO::JSONSerializer serializer;
        const auto again = serializer.read<Nexus::State::FabricSchema>(second + ".json");
        ASSERT_TRUE(again.has_value());
        ASSERT_EQ(again->entities.size(), 2U);
        ASSERT_EQ(again->entities[0].render_sinks.size(), 1U);
        EXPECT_EQ(again->entities[0].render_sinks[0].topology, Portal::Graphics::PrimitiveTopology::LINE_LIST);
        EXPECT_EQ(again->entities[0].render_sinks[0].vertex_shader, "line.vert.spv");
        EXPECT_EQ(again->entities[1].view_sinks.size(), 1U);
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusStateDeviceTest, SinkArgumentsAndImageTargetsAreSavedAndRestored, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        auto& loom = Portal::Graphics::get_texture_manager();
        loom.shutdown();
        ASSERT_TRUE(loom.initialize(std::shared_ptr<Core::VulkanBackend>(&backend(), [](Core::VulkanBackend*) { })));
        {
            const auto off = loom.create_image_target({ .width = 16, .height = 16 });
            ASSERT_NE(off, nullptr);
            auto bindings = std::make_shared<Nexus::State::Bindings>();
            bindings->targets["off"] = off;
            Nexus::Fabric source { scheduler, events, manager_handle, 1.0F };
            auto lamp = std::make_shared<Nexus::Emitter>();
            source.wire(lamp).finalise();
            auto config = render_config();
            config.target_window = nullptr;
            config.target_images = { off };
            lamp->render(manager, config, Reflect::FunctionCall { "paint", { { "amount", "3" } } },
                [](const InfluenceContext&) { });

            const auto base = (std::filesystem::current_path() / "targets").string();
            Nexus::StateEncoder encoder(bindings);
            ASSERT_TRUE(encoder.encode(source, base)) << encoder.last_error();

            IO::JSONSerializer serializer;
            const auto saved = serializer.read<Nexus::State::FabricSchema>(base + ".json");
            ASSERT_TRUE(saved.has_value());
            const auto& sink = saved->entities.at(0).render_sinks.at(0);
            EXPECT_EQ(sink.fn_name, "paint");
            EXPECT_EQ(sink.arguments.at("amount"), "3");
            EXPECT_EQ(sink.targets, (std::vector<std::string> { "off" }));

            Nexus::Fabric target { scheduler, events, manager_handle, 1.0F };
            Nexus::StateDecoder decoder(bindings);
            static_cast<void>(decoder.reconstruct(target, base));
            const auto restored = target.get_emitter(target.all_ids().at(0));
            ASSERT_NE(restored, nullptr);
            ASSERT_EQ(restored->render_sinks().size(), 1U);
            EXPECT_EQ(restored->render_sinks()[0].images, config.target_images);
        }
        loom.shutdown();
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusStateDeviceTest, PatchingReplacesSinksThatDiffer, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        auto bindings = std::make_shared<Nexus::State::Bindings>();
        bindings->windows["main"] = window;
        Nexus::Fabric source { scheduler, events, manager_handle, 1.0F };
        auto saved = std::make_shared<Nexus::Emitter>();
        source.wire(saved).finalise();
        auto lines = render_config();
        lines.topology = Portal::Graphics::PrimitiveTopology::LINE_LIST;
        saved->render(manager, lines);

        Nexus::Fabric target { scheduler, events, manager_handle, 1.0F };
        auto live = std::make_shared<Nexus::Emitter>();
        target.wire(live).finalise();
        live->render(manager, render_config());
        ASSERT_EQ(live->id(), saved->id());

        const auto base = (std::filesystem::current_path() / "patched").string();
        Nexus::StateEncoder encoder(bindings);
        ASSERT_TRUE(encoder.encode(source, base)) << encoder.last_error();
        Nexus::StateDecoder decoder(bindings);
        static_cast<void>(decoder.reconstruct(target, base));

        ASSERT_EQ(live->render_sinks().size(), 1U);
        EXPECT_EQ(live->render_sinks()[0].buf->get_render_config().topology,
            Portal::Graphics::PrimitiveTopology::LINE_LIST);
    }

    MAYAFLUX_TEST_WITH(TEST_F, NexusStateDeviceTest, ATextureSnapshotLoadsBackWithItsSize, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        IO::STBImageWriter::register_with_registry();
        auto& loom = Portal::Graphics::get_texture_manager();
        loom.shutdown();
        ASSERT_TRUE(loom.initialize(std::shared_ptr<Core::VulkanBackend>(&backend(), [](Core::VulkanBackend*) { })));
        {
            const std::vector<uint8_t> pixels(4U * 2U * 4U, 128U);
            auto texture = std::make_shared<Buffers::TextureBuffer>(
                4U, 2U, Portal::Graphics::ImageFormat::RGBA8, pixels.data());
            manager.add_buffer(texture, Buffers::ProcessingToken::GRAPHICS_BACKEND);
            ASSERT_NE(texture->get_gpu_texture(), nullptr);
            auto bindings = std::make_shared<Nexus::State::Bindings>();
            bindings->buffers["skin"] = texture;
            Nexus::Fabric source { scheduler, events, manager_handle, 1.0F };
            source.wire(std::make_shared<Nexus::Emitter>()).finalise();
            const auto base = (std::filesystem::current_path() / "painted").string();
            Nexus::StateEncoder encoder(bindings);
            ASSERT_TRUE(encoder.encode(source, base)) << encoder.last_error();

            Nexus::Fabric target { scheduler, events, manager_handle, 1.0F };
            Nexus::StateDecoder decoder;
            const auto result = decoder.reconstruct(target, base);

            ASSERT_TRUE(result.buffers.contains("skin"))
                << (result.warnings.empty() ? std::string {} : result.warnings.front());
            const auto loaded = std::dynamic_pointer_cast<Buffers::TextureBuffer>(result.buffers.at("skin"));
            ASSERT_NE(loaded, nullptr);
            EXPECT_EQ(loaded->get_width(), 4U);
            EXPECT_EQ(loaded->get_height(), 2U);
        }
        loom.shutdown();
    }
}
}
