#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"


#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "../support/nexus_support.hpp"

#include "MayaFlux/Buffers/Geometry/MeshBuffer.hpp"
#include "MayaFlux/Buffers/Network/InstanceNetworkBuffer.hpp"
#include "MayaFlux/Buffers/Network/MeshNetworkBuffer.hpp"
#include "MayaFlux/Buffers/Network/NetworkGeometryBuffer.hpp"
#include "MayaFlux/Buffers/Shaders/RenderProcessor.hpp"
#include "MayaFlux/Nexus/Expanses/Hull.hpp"
#include "MayaFlux/Nexus/Expanses/Mantle.hpp"
#include "MayaFlux/Nexus/Pheme/Attachment.hpp"
#include "MayaFlux/Nexus/Principals/Agent.hpp"
#include "MayaFlux/Nodes/Graphics/MeshWriterNode.hpp"
#include "MayaFlux/Nodes/Network/InstanceNetwork.hpp"
#include "MayaFlux/Nodes/Network/MeshNetwork.hpp"
#include "MayaFlux/Nodes/Network/Operators/PhysicsOperator.hpp"
#include "MayaFlux/Nodes/Network/ParticleNetwork.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))

namespace MayaFlux::Test {
namespace {

    using Kinesis::AABB3D;
    using Nexus::Hull;
    using Nexus::InfluenceContext;
    using Nexus::Mantle;
    using Portal::Graphics::FitMode;

    class NexusBodyTest : public NexusDeviceTest {
    protected:
        void SetUp() override
        {
            NexusDeviceTest::SetUp();
            if (IsSkipped())
                return;

            buffer = std::make_shared<Buffers::MeshBuffer>(solid_mesh());
            renderer = std::make_shared<Buffers::RenderProcessor>(Buffers::ShaderConfig { "triangle.vert.spv" });
            buffer->set_render_processor(renderer);
        }

        void place(const glm::mat4& transform)
        {
            renderer->set_geometry_transform(transform);
        }

        std::shared_ptr<Buffers::MeshBuffer> buffer;
        std::shared_ptr<Buffers::RenderProcessor> renderer;
    };

    // =========================================================================
    // Hull
    // =========================================================================

    class NexusHullTest : public NexusBodyTest { };

    TEST_F(NexusHullTest, ContainsThePointsInsideTheBuffersBounds)
    {
        const Hull hull(buffer);

        EXPECT_TRUE(hull.contains({ 2.0F, 2.0F, 2.0F }));
        EXPECT_TRUE(hull.contains({ 4.0F, 4.0F, 4.0F }));
        EXPECT_FALSE(hull.contains({ 5.0F, 2.0F, 2.0F }));
        EXPECT_FALSE(hull.contains({ 2.0F, -0.1F, 2.0F }));
    }

    TEST_F(NexusHullTest, FollowsATranslatedBuffer)
    {
        place(glm::translate(glm::mat4(1.0F), { 10.0F, 0.0F, 0.0F }));
        const Hull hull(buffer);

        EXPECT_TRUE(hull.contains({ 12.0F, 2.0F, 2.0F }));
        EXPECT_FALSE(hull.contains({ 2.0F, 2.0F, 2.0F }));
    }

    TEST_F(NexusHullTest, StaysAnExactBoxUnderRotation)
    {
        place(glm::rotate(glm::mat4(1.0F), glm::half_pi<float>(), { 0.0F, 0.0F, 1.0F }));
        const Hull hull(buffer);

        EXPECT_TRUE(hull.contains({ -2.0F, 2.0F, 2.0F }));
        EXPECT_TRUE(hull.contains({ -0.1F, 3.9F, 3.9F }));
        EXPECT_FALSE(hull.contains({ 2.0F, 2.0F, 2.0F }));
        EXPECT_FALSE(hull.contains({ -4.2F, 2.0F, 2.0F }));
    }

    TEST_F(NexusHullTest, FollowsAScaledBuffer)
    {
        place(glm::scale(glm::mat4(1.0F), { 2.0F, 2.0F, 2.0F }));
        const Hull hull(buffer);

        EXPECT_TRUE(hull.contains({ 7.0F, 7.0F, 7.0F }));
        EXPECT_FALSE(hull.contains({ 9.0F, 2.0F, 2.0F }));
    }

    TEST_F(NexusHullTest, AFlattenedBufferContainsNothing)
    {
        place(glm::scale(glm::mat4(1.0F), { 1.0F, 1.0F, 0.0F }));
        const Hull hull(buffer);

        EXPECT_FALSE(hull.contains({ 2.0F, 2.0F, 0.0F }));
    }

    TEST_F(NexusHullTest, FollowsABufferAnAgentIsAttachedTo)
    {
        const Hull hull(buffer);
        Nexus::Agent agent(1.0F);
        agent.attach(buffer);
        InfluenceContext ctx;
        ctx.position = { 20.0F, 0.0F, 0.0F };
        ASSERT_FALSE(hull.contains(ctx.position));

        agent.invoke_influence(ctx);

        EXPECT_TRUE(hull.contains(ctx.position));
        EXPECT_FALSE(hull.contains({ 2.0F, 2.0F, 2.0F }));
    }

    TEST_F(NexusHullTest, ReadsTheBufferOnceUntilRefitted)
    {
        const Hull hull(buffer);
        buffer->set_mesh_data(make_mesh(
            { { 20.0F, 0.0F, 0.0F }, { 24.0F, 0.0F, 0.0F }, { 20.0F, 4.0F, 0.0F }, { 20.0F, 0.0F, 4.0F } },
            { 0, 1, 2, 0, 1, 3, 0, 2, 3, 1, 2, 3 }));

        EXPECT_TRUE(hull.contains({ 2.0F, 2.0F, 2.0F }));
        EXPECT_FALSE(hull.contains({ 22.0F, 2.0F, 2.0F }));
    }

    TEST_F(NexusHullTest, RefitTakesTheBoundsTheBufferHasNow)
    {
        Hull hull(buffer);
        buffer->set_mesh_data(make_mesh(
            { { 20.0F, 0.0F, 0.0F }, { 24.0F, 0.0F, 0.0F }, { 20.0F, 4.0F, 0.0F }, { 20.0F, 0.0F, 4.0F } },
            { 0, 1, 2, 0, 1, 3, 0, 2, 3, 1, 2, 3 }));

        hull.refit();

        EXPECT_TRUE(hull.contains({ 22.0F, 2.0F, 2.0F }));
        EXPECT_FALSE(hull.contains({ 2.0F, 2.0F, 2.0F }));
    }

    TEST_F(NexusHullTest, RefitKeepsThePreviousBoundsWhenNothingCanBeRead)
    {
        Hull hull(buffer);
        buffer->set_mesh_data(Kakshya::MeshData::empty());

        hull.refit();

        EXPECT_TRUE(hull.contains({ 2.0F, 2.0F, 2.0F }));
    }

    TEST_F(NexusHullTest, ABufferThatCannotBeReadContainsNothing)
    {
        const auto unreadable = std::make_shared<Buffers::VKBuffer>(
            256, Buffers::VKBuffer::Usage::VERTEX, Kakshya::DataModality::VERTICES_3D);
        const Hull hull(unreadable);

        EXPECT_FALSE(hull.contains({ 0.0F, 0.0F, 0.0F }));
    }

    TEST_F(NexusHullTest, ANullBufferContainsNothing)
    {
        const Hull hull(nullptr);

        EXPECT_FALSE(hull.contains({ 0.0F, 0.0F, 0.0F }));
    }

    TEST_F(NexusHullTest, WrapsOneCollectionOfANetworkBuffer)
    {
        auto particles = std::make_shared<Nodes::Network::ParticleNetwork>(
            1, glm::vec3(100.0F, 0.0F, 0.0F), glm::vec3(100.0F, 0.0F, 0.0F), Kinesis::SpatialDistribution::RANDOM_VOLUME);
        auto* physics = particles->create_operator<Nodes::Network::PhysicsOperator>();
        physics->initialize_collections({
            { { .position = { 0.0F, 0.0F, 0.0F } }, { .position = { 2.0F, 0.0F, 0.0F } } },
            { { .position = { 10.0F, 0.0F, 0.0F } }, { .position = { 12.0F, 4.0F, 0.0F } } },
        });
        const auto network = std::make_shared<Buffers::NetworkGeometryBuffer>(particles);

        const Hull whole(network);
        const Hull second(network, {}, {}, 2U);

        EXPECT_TRUE(whole.contains({ 1.0F, 0.0F, 0.0F }));
        EXPECT_TRUE(second.contains({ 11.0F, 2.0F, 0.0F }));
        EXPECT_FALSE(second.contains({ 1.0F, 0.0F, 0.0F }));
    }

    TEST_F(NexusHullTest, TheLocalBoxIsAControlThatCanBeSetDirectly)
    {
        Hull hull(buffer);
        ASSERT_TRUE(hull.contains({ 2.0F, 2.0F, 2.0F }));

        hull.set(Nexus::HullControls { .local = AABB3D { .min = { 100.0F, 0.0F, 0.0F }, .max = { 104.0F, 4.0F, 4.0F } } });

        EXPECT_FALSE(hull.contains({ 2.0F, 2.0F, 2.0F }));
        EXPECT_TRUE(hull.contains({ 102.0F, 2.0F, 2.0F }));
    }

    TEST_F(NexusHullTest, CrossingsFollowTheBufferWhenItMoves)
    {
        std::vector<uint32_t> entered;
        std::vector<uint32_t> exited;
        Hull hull(
            buffer,
            [&entered](uint32_t id) { entered.push_back(id); },
            [&exited](uint32_t id) { exited.push_back(id); });
        const std::vector<std::pair<uint32_t, glm::vec3>> snapshot { { 7U, { 12.0F, 2.0F, 2.0F } } };

        hull.evaluate(1U, snapshot);
        EXPECT_TRUE(entered.empty());

        place(glm::translate(glm::mat4(1.0F), { 10.0F, 0.0F, 0.0F }));
        hull.evaluate(1U, snapshot);
        EXPECT_EQ(entered, (std::vector<uint32_t> { 7U }));

        place(glm::mat4(1.0F));
        hull.evaluate(1U, snapshot);
        EXPECT_EQ(exited, (std::vector<uint32_t> { 7U }));
    }

    // =========================================================================
    // Hull around one slot
    // =========================================================================

    std::shared_ptr<Nodes::GpuSync::MeshWriterNode> cube_corner_node()
    {
        std::vector<Nodes::MeshVertex> vertices(4);
        vertices[0].position = { 0.0F, 0.0F, 0.0F };
        vertices[1].position = { 4.0F, 0.0F, 0.0F };
        vertices[2].position = { 0.0F, 4.0F, 0.0F };
        vertices[3].position = { 0.0F, 0.0F, 4.0F };
        const std::vector<uint32_t> indices { 0, 1, 2, 0, 1, 3, 0, 2, 3, 1, 2, 3 };
        auto node = std::make_shared<Nodes::GpuSync::MeshWriterNode>(vertices.size());
        node->set_mesh(vertices, indices);
        node->compute_frame();
        return node;
    }

    std::shared_ptr<Nodes::Network::MeshNetwork> two_bodies()
    {
        auto network = std::make_shared<Nodes::Network::MeshNetwork>();
        network->add_slot("first", cube_corner_node());
        network->add_slot("second", cube_corner_node());
        network->get_slot(1).local_transform = glm::translate(glm::mat4(1.0F), { 20.0F, 0.0F, 0.0F });
        network->process_batch(1);
        return network;
    }

    TEST_F(NexusHullTest, WrapsOneSlotOfAMeshNetwork)
    {
        const auto meshes = std::make_shared<Buffers::MeshNetworkBuffer>(two_bodies());

        const Hull second(meshes, {}, {}, 1U);

        EXPECT_TRUE(second.contains({ 22.0F, 2.0F, 2.0F }));
        EXPECT_FALSE(second.contains({ 2.0F, 2.0F, 2.0F }));
    }

    TEST_F(NexusHullTest, FollowsItsSlotWithoutARefit)
    {
        const auto network = two_bodies();
        const auto meshes = std::make_shared<Buffers::MeshNetworkBuffer>(network);
        const Hull second(meshes, {}, {}, 1U);

        network->get_slot(1).local_transform = glm::translate(glm::mat4(1.0F), { 40.0F, 0.0F, 0.0F });
        network->process_batch(1);

        EXPECT_TRUE(second.contains({ 42.0F, 2.0F, 2.0F }));
        EXPECT_FALSE(second.contains({ 22.0F, 2.0F, 2.0F }));
    }

    TEST_F(NexusHullTest, ASlotIsPlacedInsideTheBuffersOwnTransform)
    {
        const auto meshes = std::make_shared<Buffers::MeshNetworkBuffer>(two_bodies());
        meshes->set_render_processor(renderer);
        place(glm::translate(glm::mat4(1.0F), { 0.0F, 100.0F, 0.0F }));

        const Hull second(meshes, {}, {}, 1U);

        EXPECT_TRUE(second.contains({ 22.0F, 102.0F, 2.0F }));
        EXPECT_FALSE(second.contains({ 22.0F, 2.0F, 2.0F }));
        meshes->set_render_processor(nullptr);
    }

    TEST_F(NexusHullTest, WrapsOneInstanceAndFollowsIt)
    {
        auto network = std::make_shared<Nodes::Network::InstanceNetwork>();
        network->add_slot("first", cube_corner_node());
        network->add_slot("second", cube_corner_node());
        network->get_slot(1).transform = glm::translate(glm::mat4(1.0F), { 20.0F, 0.0F, 0.0F });
        const auto instances = std::make_shared<Buffers::InstanceNetworkBuffer>(network);
        const Hull second(instances, {}, {}, 1U);
        ASSERT_TRUE(second.contains({ 22.0F, 2.0F, 2.0F }));
        ASSERT_FALSE(second.contains({ 2.0F, 2.0F, 2.0F }));

        network->get_slot(1).transform = glm::translate(glm::mat4(1.0F), { 0.0F, 30.0F, 0.0F });

        EXPECT_TRUE(second.contains({ 2.0F, 32.0F, 2.0F }));
        EXPECT_FALSE(second.contains({ 22.0F, 2.0F, 2.0F }));
    }

    TEST_F(NexusHullTest, ASlotOutOfRangeContainsNothing)
    {
        const auto meshes = std::make_shared<Buffers::MeshNetworkBuffer>(two_bodies());

        const Hull missing(meshes, {}, {}, 5U);

        EXPECT_FALSE(missing.contains({ 2.0F, 2.0F, 2.0F }));
        EXPECT_FALSE(missing.intersect({ .origin = { -5.0F, 1.0F, 1.0F }, .direction = { 1.0F, 0.0F, 0.0F } }).has_value());
    }

    TEST_F(NexusHullTest, ARayDistanceIsMeasuredWhereTheBufferIsDrawn)
    {
        place(glm::translate(glm::mat4(1.0F), { 10.0F, 0.0F, 0.0F }) * glm::scale(glm::mat4(1.0F), glm::vec3(2.0F)));
        const Hull hull(buffer);

        const auto hit = hull.intersect({ .origin = { 0.0F, 2.0F, 2.0F }, .direction = { 1.0F, 0.0F, 0.0F } });

        ASSERT_TRUE(hit.has_value());
        EXPECT_NEAR(*hit, 10.0F, k_tolerance);
    }

    TEST_F(NexusHullTest, ARayMeetsARotatedBuffersOwnBox)
    {
        place(glm::rotate(glm::mat4(1.0F), glm::half_pi<float>(), { 0.0F, 0.0F, 1.0F }));
        const Hull hull(buffer);

        const auto hit = hull.intersect({ .origin = { -10.0F, 2.0F, 2.0F }, .direction = { 1.0F, 0.0F, 0.0F } });

        ASSERT_TRUE(hit.has_value());
        EXPECT_NEAR(*hit, 6.0F, k_tolerance);
        EXPECT_FALSE(hull.intersect({ .origin = { 2.0F, 2.0F, -10.0F }, .direction = { 0.0F, 0.0F, 1.0F } }).has_value());
    }

    TEST_F(NexusHullTest, ARayFindsNothingInAFlattenedBuffer)
    {
        place(glm::scale(glm::mat4(1.0F), { 1.0F, 1.0F, 0.0F }));
        const Hull hull(buffer);

        EXPECT_FALSE(hull.intersect({ .origin = { 2.0F, 2.0F, -5.0F }, .direction = { 0.0F, 0.0F, 1.0F } }).has_value());
    }

    // =========================================================================
    // Mantle
    // =========================================================================

    class NexusMantleTest : public NexusBodyTest {
    protected:
        static AABB3D box(const glm::vec3& min, const glm::vec3& max)
        {
            return { .min = min, .max = max };
        }

        [[nodiscard]] glm::vec3 low() const { return placed(buffer, { 0.0F, 0.0F, 0.0F }); }
        [[nodiscard]] glm::vec3 high() const { return placed(buffer, { 4.0F, 4.0F, 4.0F }); }
    };

    TEST_F(NexusMantleTest, StretchFillsTheBoxOnEveryAxis)
    {
        const Mantle mantle(box({ 10.0F, 0.0F, -5.0F }, { 20.0F, 5.0F, 5.0F }), buffer, FitMode::STRETCH);

        expect_near(low(), { 10.0F, 0.0F, -5.0F });
        expect_near(high(), { 20.0F, 5.0F, 5.0F });
    }

    TEST_F(NexusMantleTest, ContainKeepsTheAspectAndFitsInsideTheBox)
    {
        const Mantle mantle(box({ 0.0F, 0.0F, 0.0F }, { 10.0F, 5.0F, 10.0F }), buffer, FitMode::CONTAIN);

        expect_near(high() - low(), { 5.0F, 5.0F, 5.0F });
        expect_near((high() + low()) * 0.5F, { 5.0F, 2.5F, 5.0F });
    }

    TEST_F(NexusMantleTest, CoverKeepsTheAspectAndCoversTheBox)
    {
        const Mantle mantle(box({ 0.0F, 0.0F, 0.0F }, { 10.0F, 5.0F, 10.0F }), buffer, FitMode::COVER);

        expect_near(high() - low(), { 10.0F, 10.0F, 10.0F });
        expect_near((high() + low()) * 0.5F, { 5.0F, 2.5F, 5.0F });
    }

    TEST_F(NexusMantleTest, CenterDoesNotScale)
    {
        const Mantle mantle(box({ 0.0F, 0.0F, 0.0F }, { 10.0F, 10.0F, 10.0F }), buffer, FitMode::CENTER);

        expect_near(high() - low(), { 4.0F, 4.0F, 4.0F });
        expect_near((high() + low()) * 0.5F, { 5.0F, 5.0F, 5.0F });
    }

    TEST_F(NexusMantleTest, TilingModesFitAsStretch)
    {
        const AABB3D target = box({ 10.0F, 0.0F, -5.0F }, { 20.0F, 5.0F, 5.0F });
        const Mantle tiled(target, buffer, FitMode::TILE);

        expect_near(low(), { 10.0F, 0.0F, -5.0F });
        expect_near(high(), { 20.0F, 5.0F, 5.0F });
    }

    TEST_F(NexusMantleTest, TheBoundsControlPlacesTheBufferAtTheNextEvaluation)
    {
        Mantle mantle(box({ 0.0F, 0.0F, 0.0F }, { 4.0F, 4.0F, 4.0F }), buffer);

        mantle.set({ .bounds = box({ 100.0F, 0.0F, 0.0F }, { 108.0F, 8.0F, 8.0F }) });
        expect_near(low(), { 0.0F, 0.0F, 0.0F });

        mantle.evaluate(1U, {});

        expect_near(low(), { 100.0F, 0.0F, 0.0F });
        expect_near(high(), { 108.0F, 8.0F, 8.0F });
    }

    TEST_F(NexusMantleTest, ABoxSetOnceIsPlacedOnceAndTheLookStaysAttachedAfter)
    {
        Mantle mantle(box({ 0.0F, 0.0F, 0.0F }, { 4.0F, 4.0F, 4.0F }), buffer);
        mantle.set({ .bounds = box({ 10.0F, 0.0F, 0.0F }, { 14.0F, 4.0F, 4.0F }) });

        mantle.evaluate(1U, {});
        mantle.evaluate(1U, {});

        expect_near(low(), { 10.0F, 0.0F, 0.0F });
        expect_near(high(), { 14.0F, 4.0F, 4.0F });
    }

    TEST_F(NexusMantleTest, TheFitControlChangesHowTheGeometryFillsTheBox)
    {
        Mantle mantle(box({ 0.0F, 0.0F, 0.0F }, { 10.0F, 10.0F, 10.0F }), buffer, FitMode::STRETCH);
        expect_near(high() - low(), { 10.0F, 10.0F, 10.0F });

        mantle.set(Nexus::MantleControls { .fit = FitMode::CENTER });
        mantle.evaluate(1U, {});

        expect_near(high() - low(), { 4.0F, 4.0F, 4.0F });
        expect_near((high() + low()) * 0.5F, { 5.0F, 5.0F, 5.0F });
    }

    TEST_F(NexusMantleTest, RefitPlacesTheGeometryTheBufferHasNowAtTheNextEvaluation)
    {
        Mantle mantle(box({ 0.0F, 0.0F, 0.0F }, { 8.0F, 8.0F, 8.0F }), buffer);
        buffer->set_mesh_data(make_mesh(
            { { 0.0F, 0.0F, 0.0F }, { 8.0F, 0.0F, 0.0F }, { 0.0F, 8.0F, 0.0F }, { 0.0F, 0.0F, 8.0F } },
            { 0, 1, 2, 0, 1, 3, 0, 2, 3, 1, 2, 3 }));

        mantle.refit();
        mantle.evaluate(1U, {});

        expect_near(placed(buffer, { 8.0F, 8.0F, 8.0F }), { 8.0F, 8.0F, 8.0F });
        expect_near(placed(buffer, { 0.0F, 0.0F, 0.0F }), { 0.0F, 0.0F, 0.0F });
    }

    TEST_F(NexusMantleTest, ItsRegionIsTheBoxNotTheBuffer)
    {
        const Mantle mantle(box({ 10.0F, 0.0F, 0.0F }, { 20.0F, 5.0F, 5.0F }), buffer, FitMode::CENTER);

        EXPECT_TRUE(mantle.contains({ 11.0F, 1.0F, 1.0F }));
        EXPECT_FALSE(mantle.contains({ 2.0F, 2.0F, 2.0F }));
    }

    TEST_F(NexusMantleTest, DestroyingItGivesTheBufferItsFixedTransformBack)
    {
        const auto fixed = glm::scale(glm::mat4(1.0F), { 2.0F, 2.0F, 2.0F });
        place(fixed);
        {
            const Mantle mantle(box({ 10.0F, 0.0F, 0.0F }, { 20.0F, 10.0F, 10.0F }), buffer);
            ASSERT_NE(Nexus::read_placement(buffer), fixed);
        }

        EXPECT_EQ(Nexus::read_placement(buffer), fixed);
    }

    TEST_F(NexusMantleTest, DestroyingItLeavesNoTransformWhereThereWasNone)
    {
        {
            const Mantle mantle(box({ 10.0F, 0.0F, 0.0F }, { 20.0F, 10.0F, 10.0F }), buffer);
            ASSERT_TRUE(static_cast<bool>(renderer->get_geometry_transform_source()));
        }

        EXPECT_FALSE(static_cast<bool>(renderer->get_geometry_transform_source()));
    }

    TEST_F(NexusMantleTest, StacksWithAnAgentAttachedToTheSameBuffer)
    {
        const Mantle mantle(box({ 10.0F, 0.0F, 0.0F }, { 20.0F, 10.0F, 10.0F }), buffer);
        const glm::vec3 fitted = low();
        Nexus::Agent agent(1.0F);
        agent.attach(buffer);
        InfluenceContext ctx;
        ctx.position = { 50.0F, 0.0F, 0.0F };

        agent.invoke_influence(ctx);
        ASSERT_GT(glm::length(low() - fitted), 1.0F);

        agent.detach();

        expect_near(low(), fitted);
        EXPECT_TRUE(static_cast<bool>(renderer->get_geometry_transform_source()));
    }

    TEST_F(NexusMantleTest, ABufferThatCannotBeReadIsLeftWhereItIs)
    {
        const auto unreadable = std::make_shared<Buffers::VKBuffer>(
            256, Buffers::VKBuffer::Usage::VERTEX, Kakshya::DataModality::VERTICES_3D);
        const auto other = std::make_shared<Buffers::RenderProcessor>(Buffers::ShaderConfig { "triangle.vert.spv" });
        unreadable->set_render_processor(other);

        const Mantle mantle(box({ 0.0F, 0.0F, 0.0F }, { 4.0F, 4.0F, 4.0F }), unreadable);

        EXPECT_FALSE(static_cast<bool>(other->get_geometry_transform_source()));
    }

    TEST_F(NexusMantleTest, ANullBufferStillHasItsBox)
    {
        const Mantle mantle(box({ 0.0F, 0.0F, 0.0F }, { 4.0F, 4.0F, 4.0F }), nullptr);

        EXPECT_TRUE(mantle.contains({ 1.0F, 1.0F, 1.0F }));
        EXPECT_FALSE(mantle.contains({ 5.0F, 1.0F, 1.0F }));
    }

}
}
