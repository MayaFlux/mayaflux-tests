#include "gtest/gtest.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "../support/nexus_support.hpp"

#include "MayaFlux/Buffers/AudioBuffer.hpp"
#include "MayaFlux/Buffers/BufferManager.hpp"
#include "MayaFlux/Buffers/BufferProcessingChain.hpp"
#include "MayaFlux/Buffers/Geometry/MeshBuffer.hpp"
#include "MayaFlux/Buffers/Shaders/RenderProcessor.hpp"
#include "MayaFlux/Buffers/VKBuffer.hpp"
#include "MayaFlux/Kakshya/NDData/MeshData.hpp"
#include "MayaFlux/Kakshya/NDData/VertexFormats.hpp"
#include "MayaFlux/Nexus/Fabric.hpp"
#include "MayaFlux/Nexus/Pheme/Attachment.hpp"
#include "MayaFlux/Nexus/Pheme/Sinks.hpp"
#include "MayaFlux/Nexus/Pheme/Survey.hpp"
#include "MayaFlux/Nexus/Principals/Agent.hpp"
#include "MayaFlux/Nexus/Principals/Locus.hpp"
#include "MayaFlux/Nexus/Tapestry.hpp"
#include "MayaFlux/Portal/Graphics/GraphicsUtils.hpp"
#include "MayaFlux/Portal/Graphics/ShaderFoundry.hpp"
#include "MayaFlux/Vruta/EventManager.hpp"
#include "MayaFlux/Vruta/Scheduler.hpp"

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

        Buffers::BufferManager manager;
        Vruta::TaskScheduler scheduler { 48000 };
        Vruta::EventManager events;
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
        Nexus::Fabric fabric(scheduler, events, manager, 1.0F);
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
        Nexus::Fabric fabric(scheduler, events, manager, 1.0F);
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
            Nexus::Fabric fabric(scheduler, events, manager, 1.0F);
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
        Vruta::TaskScheduler scheduler { 48000 };
        Vruta::EventManager events;
        std::vector<Nexus::RenderSink> sinks;
    };

    TEST_F(NexusRenderSinkTest, AddingASinkRegistersItsBufferWithTheGraphicsToken)
    {
        Nexus::add_render_sink(sinks, manager, render_config());

        ASSERT_EQ(sinks.size(), 1U);
        EXPECT_TRUE(registered(sinks.front().buf));
        EXPECT_EQ(sinks.front().window, window);
    }

    TEST_F(NexusRenderSinkTest, TheSinkRendererIsThePrimaryProcessorOfItsBuffer)
    {
        Nexus::add_render_sink(sinks, manager, render_config());

        ASSERT_NE(sinks.front().renderer, nullptr);
        EXPECT_EQ(sinks.front().buf->get_render_processor(), sinks.front().renderer);
    }

    TEST_F(NexusRenderSinkTest, RemovingASinkDeregistersItsBuffer)
    {
        Nexus::add_render_sink(sinks, manager, render_config());
        const auto buffer = sinks.front().buf;

        Nexus::remove_render_sink(sinks, manager, window);

        EXPECT_TRUE(sinks.empty());
        EXPECT_FALSE(registered(buffer));
    }

    TEST_F(NexusRenderSinkTest, RemovingAnUnknownWindowChangesNothing)
    {
        Nexus::add_render_sink(sinks, manager, render_config());
        const auto other = std::make_shared<ScriptedWindow>(100U, 100U);

        Nexus::remove_render_sink(sinks, manager, other);

        EXPECT_EQ(sinks.size(), 1U);
        EXPECT_TRUE(registered(sinks.front().buf));
    }

    TEST_F(NexusRenderSinkTest, EveryTopologyBuildsASink)
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

    TEST_F(NexusRenderSinkTest, DispatchCallsEachProducerWithTheContext)
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

    TEST_F(NexusRenderSinkTest, DispatchSkipsASinkWithoutAProducer)
    {
        Nexus::add_render_sink(sinks, manager, render_config(), {}, {}, glm::vec3(1.0F));

        EXPECT_NO_THROW(Nexus::dispatch_render_sinks(sinks, InfluenceContext {}));
    }

    TEST_F(NexusRenderSinkTest, AnAgentFindsTheRendererOfItsSinkByWindow)
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

    TEST_F(NexusRenderSinkTest, AnAgentDroppedOutsideAFabricLeavesItsRenderSinkRegistered)
    {
        std::shared_ptr<Buffers::VKBuffer> buffer;
        {
            Nexus::Agent agent(1.0F);
            agent.render(manager, render_config());
            buffer = agent.render_sinks().front().buf;
        }

        EXPECT_TRUE(registered(buffer));
    }

    TEST_F(NexusRenderSinkTest, RemovingAnAgentFromItsFabricUnregistersItsRenderSink)
    {
        Nexus::Fabric fabric(scheduler, events, manager, 1.0F);
        auto agent = std::make_shared<Nexus::Agent>(1.0F);
        agent->render(manager, render_config());
        const auto buffer = agent->render_sinks().front().buf;
        fabric.wire(agent).finalise();
        ASSERT_TRUE(registered(buffer));

        fabric.remove(fabric.all_ids().front());

        EXPECT_FALSE(registered(buffer));
        EXPECT_TRUE(agent->render_sinks().empty());
    }

    TEST_F(NexusRenderSinkTest, RemovingAnEmitterFromItsFabricUnregistersItsRenderSink)
    {
        Nexus::Fabric fabric(scheduler, events, manager, 1.0F);
        auto emitter = std::make_shared<Nexus::Emitter>();
        emitter->render(manager, render_config());
        const auto buffer = emitter->render_sinks().front().buf;
        fabric.wire(emitter).finalise();

        fabric.remove(fabric.all_ids().front());

        EXPECT_FALSE(registered(buffer));
        EXPECT_TRUE(emitter->render_sinks().empty());
    }

    TEST_F(NexusRenderSinkTest, DestroyingAFabricUnregistersTheRenderSinksOfItsEntities)
    {
        auto agent = std::make_shared<Nexus::Agent>(1.0F);
        agent->render(manager, render_config());
        const auto buffer = agent->render_sinks().front().buf;
        {
            Nexus::Fabric fabric(scheduler, events, manager, 1.0F);
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

        Vruta::TaskScheduler scheduler { 48000 };
        Vruta::EventManager events;
        std::shared_ptr<Buffers::MeshBuffer> buffer;
        std::shared_ptr<Buffers::RenderProcessor> renderer;
    };

    TEST_F(NexusAttachmentTest, AnAttachmentAnchorsAtTheSurfaceCentroid)
    {
        const auto attachment = attach();

        expect_near(attachment.anchor, { 4.0F / 3.0F, 4.0F / 3.0F, 0.0F });
        expect_near(attachment.applied, attachment.anchor);
    }

    TEST_F(NexusAttachmentTest, AnAttachmentAnchorsAtTheIndexedVertex)
    {
        AttachConfig config;
        config.index = 1U;

        expect_near(attach(config).anchor, { 4.0F, 0.0F, 0.0F });
    }

    TEST_F(NexusAttachmentTest, AnAttachmentToAnUnreadableBufferAnchorsAtTheOrigin)
    {
        const auto empty = std::make_shared<Buffers::VKBuffer>(
            256, Buffers::VKBuffer::Usage::VERTEX, Kakshya::DataModality::VERTICES_3D);

        const auto attachment = Nexus::make_attachment(empty, {}, std::nullopt);

        expect_near(attachment.anchor, { 0.0F, 0.0F, 0.0F });
    }

    TEST_F(NexusAttachmentTest, TheOffsetIsAddedToTheAppliedPosition)
    {
        AttachConfig config;
        config.offset = { 0.0F, 0.0F, 1.0F };

        const auto attachment = attach(config);

        expect_near(attachment.applied, attachment.anchor + glm::vec3(0.0F, 0.0F, 1.0F));
    }

    TEST_F(NexusAttachmentTest, TheOffsetTurnsWithTheOrientation)
    {
        AttachConfig config;
        config.offset = { 0.0F, 0.0F, 1.0F };
        const auto turn = glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0F, 1.0F, 0.0F));

        const auto attachment = Nexus::make_attachment(buffer, config, turn);

        expect_near(attachment.applied, attachment.anchor + glm::vec3(1.0F, 0.0F, 0.0F));
    }

    TEST_F(NexusAttachmentTest, PlacingInstallsASourceThatReadsTheTransform)
    {
        auto attachment = attach();
        EXPECT_FALSE(static_cast<bool>(renderer->get_geometry_transform_source()));

        Nexus::place_attachment(attachment);
        *attachment.transform = glm::translate(glm::mat4(1.0F), { 1.0F, 2.0F, 3.0F });

        EXPECT_TRUE(static_cast<bool>(renderer->get_geometry_transform_source()));
        expect_near(placed(buffer, { 0.0F, 0.0F, 0.0F }), { 1.0F, 2.0F, 3.0F });
    }

    TEST_F(NexusAttachmentTest, PlacingTwiceFollowsTheProcessorOnce)
    {
        auto attachment = attach();

        Nexus::place_attachment(attachment);
        Nexus::place_attachment(attachment);

        EXPECT_EQ(attachment.followed.size(), 1U);
    }

    TEST_F(NexusAttachmentTest, ReleasingRestoresAFixedTransform)
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

    TEST_F(NexusAttachmentTest, ReleasingRestoresTheAbsenceOfATransform)
    {
        auto attachment = attach();
        Nexus::place_attachment(attachment);

        Nexus::release_attachment(attachment);

        EXPECT_FALSE(static_cast<bool>(renderer->get_geometry_transform_source()));
        EXPECT_EQ(Nexus::read_placement(buffer), glm::mat4(1.0F));
    }

    TEST_F(NexusAttachmentTest, ReleasingRestoresATransformSource)
    {
        renderer->set_geometry_transform_source([] { return glm::translate(glm::mat4(1.0F), { 0.0F, 9.0F, 0.0F }); });
        auto attachment = attach();
        Nexus::place_attachment(attachment);

        Nexus::release_attachment(attachment);

        expect_near(placed(buffer, { 0.0F, 0.0F, 0.0F }), { 0.0F, 9.0F, 0.0F });
    }

    TEST_F(NexusAttachmentTest, LaterAttachmentsStackOutsideEarlierOnes)
    {
        auto first = attach();
        auto second = attach();
        Nexus::place_attachment(first);
        Nexus::place_attachment(second);
        *first.transform = glm::translate(glm::mat4(1.0F), { 1.0F, 0.0F, 0.0F });
        *second.transform = glm::scale(glm::mat4(1.0F), { 2.0F, 2.0F, 2.0F });

        expect_near(placed(buffer, { 0.0F, 0.0F, 0.0F }), { 2.0F, 0.0F, 0.0F });
    }

    TEST_F(NexusAttachmentTest, EachAttachmentIsReleasedOnItsOwn)
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

    TEST_F(NexusAttachmentTest, AnInfluenceMovesTheAnchorToTheContextPosition)
    {
        auto attachment = attach();
        InfluenceContext ctx;
        ctx.position = { 5.0F, 0.0F, 0.0F };

        Nexus::apply_attachment(attachment, ctx);

        expect_near(placed(buffer, attachment.anchor), ctx.position);
    }

    TEST_F(NexusAttachmentTest, TheOffsetIsLeftBehindTheContextPosition)
    {
        AttachConfig config;
        config.offset = { 0.0F, 1.0F, 0.0F };
        auto attachment = attach(config);
        InfluenceContext ctx;
        ctx.position = { 5.0F, 0.0F, 0.0F };

        Nexus::apply_attachment(attachment, ctx);

        expect_near(placed(buffer, attachment.anchor), { 5.0F, -1.0F, 0.0F });
    }

    TEST_F(NexusAttachmentTest, AMoveFromOutsideBecomesTheOffset)
    {
        auto attachment = attach();
        std::optional<glm::vec3> position = attachment.applied + glm::vec3(2.0F, 0.0F, 0.0F);

        Nexus::follow_attachment(attachment, position, std::nullopt);

        expect_near(attachment.config.offset, { 2.0F, 0.0F, 0.0F });
        EXPECT_EQ(*attachment.transform, glm::mat4(1.0F));
        expect_near(*position, attachment.anchor + glm::vec3(2.0F, 0.0F, 0.0F));
    }

    TEST_F(NexusAttachmentTest, AMoveFromOutsideCanCarryTheBuffer)
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

    TEST_F(NexusAttachmentTest, FollowingWithoutAPositionSetsItFromTheBuffer)
    {
        AttachConfig config;
        config.offset = { 0.0F, 0.0F, 1.0F };
        auto attachment = attach(config);
        std::optional<glm::vec3> position;

        Nexus::follow_attachment(attachment, position, std::nullopt);

        ASSERT_TRUE(position.has_value());
        expect_near(*position, attachment.anchor + glm::vec3(0.0F, 0.0F, 1.0F));
    }

    TEST_F(NexusAttachmentTest, RecenteringMovesTheAnchorAndKeepsThePosition)
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

    TEST_F(NexusAttachmentTest, AnAgentAttachesToABufferAndTakesItsPosition)
    {
        Nexus::Agent agent(1.0F);

        agent.attach(buffer);

        EXPECT_TRUE(agent.attached());
        ASSERT_TRUE(agent.position().has_value());
        expect_near(*agent.position(), { 4.0F / 3.0F, 4.0F / 3.0F, 0.0F });
    }

    TEST_F(NexusAttachmentTest, AnAgentIgnoresANullBuffer)
    {
        Nexus::Agent agent(1.0F);

        agent.attach(nullptr);

        EXPECT_FALSE(agent.attached());
    }

    TEST_F(NexusAttachmentTest, AnAgentInfluencePlacesTheAttachedBuffer)
    {
        Nexus::Agent agent(1.0F);
        agent.attach(buffer);
        InfluenceContext ctx;
        ctx.position = { 5.0F, 0.0F, 0.0F };

        agent.invoke_influence(ctx);

        expect_near(placed(buffer, { 4.0F / 3.0F, 4.0F / 3.0F, 0.0F }), ctx.position);
    }

    TEST_F(NexusAttachmentTest, DetachingGivesTheBufferItsPlacementBack)
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

    TEST_F(NexusAttachmentTest, AttachingAgainReplacesThePreviousAttachment)
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

    TEST_F(NexusAttachmentTest, ADroppedAgentGivesTheBufferItsPlacementBack)
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

    TEST_F(NexusAttachmentTest, ADroppedEmitterGivesTheBufferItsPlacementBack)
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

    TEST_F(NexusAttachmentTest, RemovingAnAgentFromItsFabricGivesTheBufferItsPlacementBack)
    {
        Nexus::Fabric fabric(scheduler, events, manager, 1.0F);
        auto agent = std::make_shared<Nexus::Agent>(1.0F);
        agent->attach(buffer);
        fabric.wire(agent).finalise();
        fabric.commit();
        ASSERT_TRUE(static_cast<bool>(renderer->get_geometry_transform_source()));

        fabric.remove(fabric.all_ids().front());

        EXPECT_FALSE(agent->attached());
        EXPECT_FALSE(static_cast<bool>(renderer->get_geometry_transform_source()));
    }

    TEST_F(NexusAttachmentTest, DestroyingAFabricGivesTheBuffersOfItsEntitiesTheirPlacementBack)
    {
        auto agent = std::make_shared<Nexus::Agent>(1.0F);
        agent->attach(buffer);
        {
            Nexus::Fabric fabric(scheduler, events, manager, 1.0F);
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
                [this](const InfluenceContext&) { seen_at_influence = first->get_view_transform().has_value(); });
        }

        std::shared_ptr<Buffers::RenderProcessor> first;
        std::shared_ptr<Buffers::RenderProcessor> second;
        std::unique_ptr<Nexus::Locus> locus;
        bool seen_at_influence {};
    };

    TEST_F(NexusLocusViewTest, ANullTargetIsIgnored)
    {
        locus->add_view_target(nullptr);

        EXPECT_TRUE(locus->view_targets().empty());
    }

    TEST_F(NexusLocusViewTest, AddingTheSameTargetTwiceKeepsOne)
    {
        locus->add_view_target(first);
        locus->add_view_target(first);

        EXPECT_EQ(locus->view_targets().size(), 1U);
    }

    TEST_F(NexusLocusViewTest, RemovingATargetLeavesTheOthers)
    {
        locus->add_view_target(first);
        locus->add_view_target(second);

        locus->remove_view_target(first);

        ASSERT_EQ(locus->view_targets().size(), 1U);
        EXPECT_EQ(locus->view_targets().front(), second);
    }

    TEST_F(NexusLocusViewTest, InfluenceGivesEveryTargetTheCurrentView)
    {
        locus->add_view_target(first);
        locus->add_view_target(second);

        locus->invoke_influence(InfluenceContext {});

        for (const auto& target : { first, second }) {
            ASSERT_TRUE(target->get_view_transform().has_value());
            EXPECT_EQ(target->get_view_transform()->view, locus->current_view().view);
            EXPECT_EQ(target->get_view_transform()->projection, locus->current_view().projection);
        }
    }

    TEST_F(NexusLocusViewTest, ARemovedTargetIsLeftAlone)
    {
        locus->add_view_target(first);
        locus->add_view_target(second);
        locus->remove_view_target(second);

        locus->invoke_influence(InfluenceContext {});

        EXPECT_TRUE(first->get_view_transform().has_value());
        EXPECT_FALSE(second->get_view_transform().has_value());
    }

    TEST_F(NexusLocusViewTest, TheViewIsPushedBeforeTheInfluenceFunctionRuns)
    {
        locus->add_view_target(first);

        locus->invoke_influence(InfluenceContext {});

        EXPECT_TRUE(seen_at_influence);
    }

    TEST_F(NexusLocusViewTest, ATurnedLocusPushesTheNewView)
    {
        locus->add_view_target(first);
        locus->invoke_influence(InfluenceContext {});
        const auto before = first->get_view_transform()->view;

        locus->nav().yaw += 0.5F;
        locus->invoke_perception(PerceptionContext {});
        locus->invoke_influence(InfluenceContext {});

        EXPECT_NE(first->get_view_transform()->view, before);
        EXPECT_EQ(first->get_view_transform()->view, locus->current_view().view);
    }

}
}
