#include "gtest/gtest.h"


#include <glm/gtc/matrix_transform.hpp>

#include "../buffers/compute/volume_grid_fixture.hpp"
#include "../support/nexus_support.hpp"

#include "MayaFlux/Buffers/Geometry/GeometryBuffer.hpp"
#include "MayaFlux/Buffers/Network/InstanceNetworkBuffer.hpp"
#include "MayaFlux/Buffers/Network/MeshNetworkBuffer.hpp"
#include "MayaFlux/Buffers/Network/NetworkGeometryBuffer.hpp"
#include "MayaFlux/Kakshya/NDData/VertexFormats.hpp"
#include "MayaFlux/Nexus/Pheme/Survey.hpp"
#include "MayaFlux/Nodes/Graphics/MeshWriterNode.hpp"
#include "MayaFlux/Nodes/Graphics/PointCollectionNode.hpp"
#include "MayaFlux/Nodes/Network/InstanceNetwork.hpp"
#include "MayaFlux/Nodes/Network/MeshNetwork.hpp"
#include "MayaFlux/Nodes/Network/Operators/PhysicsOperator.hpp"
#include "MayaFlux/Nodes/Network/ParticleNetwork.hpp"

namespace MayaFlux::Test {
namespace {

    using Nodes::Network::InstanceNetwork;
    using Nodes::Network::MeshNetwork;
    using Nodes::Network::ParticleNetwork;
    using Nodes::Network::PhysicsOperator;

    std::shared_ptr<Nodes::GpuSync::MeshWriterNode> quad_node(const glm::vec3& centre)
    {
        std::vector<Nodes::MeshVertex> vertices(4);
        vertices[0].position = centre + glm::vec3(-0.2F, -0.2F, 0.0F);
        vertices[1].position = centre + glm::vec3(0.2F, -0.2F, 0.0F);
        vertices[2].position = centre + glm::vec3(0.2F, 0.2F, 0.0F);
        vertices[3].position = centre + glm::vec3(-0.2F, 0.2F, 0.0F);
        const std::vector<uint32_t> indices { 0, 1, 2, 0, 2, 3 };
        auto node = std::make_shared<Nodes::GpuSync::MeshWriterNode>(vertices.size());
        node->set_mesh(vertices, indices);
        node->compute_frame();
        return node;
    }

    // =========================================================================
    // Geometry nodes
    // =========================================================================

    class NexusSurveyNodeBufferTest : public ::testing::Test {
    protected:
        void SetUp() override
        {
            node = std::make_shared<Nodes::GpuSync::PointCollectionNode>(std::vector<Kakshya::PointVertex> {
                { .position = { 0.0F, 0.0F, 0.0F } },
                { .position = { 4.0F, 0.0F, 0.0F } },
                { .position = { 0.0F, 4.0F, 0.0F } },
                { .position = { 0.0F, 0.0F, 4.0F } },
            });
            node->compute_frame();
            buffer = std::make_shared<Buffers::GeometryBuffer>(node);
        }

        std::shared_ptr<Nodes::GpuSync::PointCollectionNode> node;
        std::shared_ptr<Buffers::GeometryBuffer> buffer;
    };

    TEST_F(NexusSurveyNodeBufferTest, AnchorOfAPointNodeIsTheVertexMean)
    {
        const auto anchor = Nexus::read_anchor(buffer, std::nullopt);

        ASSERT_TRUE(anchor.has_value());
        expect_near(*anchor, { 1.0F, 1.0F, 1.0F });
    }

    TEST_F(NexusSurveyNodeBufferTest, AnchorAtAnIndexIsThatPoint)
    {
        const auto anchor = Nexus::read_anchor(buffer, 2U);

        ASSERT_TRUE(anchor.has_value());
        expect_near(*anchor, { 0.0F, 4.0F, 0.0F });
    }

    TEST_F(NexusSurveyNodeBufferTest, AnchorAtAnIndexBeyondThePointsCannotBeRead)
    {
        EXPECT_FALSE(Nexus::read_anchor(buffer, 4U).has_value());
    }

    TEST_F(NexusSurveyNodeBufferTest, BoundsOfAPointNodeCoverEveryPoint)
    {
        const auto bounds = Nexus::read_bounds(buffer);

        ASSERT_TRUE(bounds.has_value());
        expect_near(bounds->min, { 0.0F, 0.0F, 0.0F });
        expect_near(bounds->max, { 4.0F, 4.0F, 4.0F });
    }

    TEST_F(NexusSurveyNodeBufferTest, ACollectionIsIgnoredForAPointNode)
    {
        const auto bounds = Nexus::read_bounds(buffer, 7U);

        ASSERT_TRUE(bounds.has_value());
        expect_near(bounds->max, { 4.0F, 4.0F, 4.0F });
    }

    TEST_F(NexusSurveyNodeBufferTest, AnEmptyPointNodeCannotBeRead)
    {
        const auto empty = std::make_shared<Buffers::GeometryBuffer>(
            std::make_shared<Nodes::GpuSync::PointCollectionNode>(std::vector<Kakshya::PointVertex> {}));

        EXPECT_FALSE(Nexus::read_anchor(empty, std::nullopt).has_value());
        EXPECT_FALSE(Nexus::read_bounds(empty).has_value());
    }

    TEST_F(NexusSurveyNodeBufferTest, AMeshWriterNodeIsReadAsAMesh)
    {
        const auto mesh = std::make_shared<Buffers::GeometryBuffer>(quad_node({ 1.0F, 2.0F, 3.0F }));

        const auto anchor = Nexus::read_anchor(mesh, std::nullopt);
        const auto bounds = Nexus::read_bounds(mesh);

        ASSERT_TRUE(anchor.has_value());
        ASSERT_TRUE(bounds.has_value());
        expect_near(*anchor, { 1.0F, 2.0F, 3.0F });
        expect_near(bounds->min, { 0.8F, 1.8F, 3.0F });
        expect_near(bounds->max, { 1.2F, 2.2F, 3.0F });
    }

    // =========================================================================
    // Network geometry
    // =========================================================================

    class NexusSurveyNetworkBufferTest : public ::testing::Test {
    protected:
        void SetUp() override
        {
            auto particles = std::make_shared<ParticleNetwork>(
                1, glm::vec3(100.0F, 0.0F, 0.0F), glm::vec3(100.0F, 0.0F, 0.0F), Kinesis::SpatialDistribution::RANDOM_VOLUME);
            auto* physics = particles->create_operator<PhysicsOperator>();
            physics->initialize_collections({
                { { .position = { 0.0F, 0.0F, 0.0F } }, { .position = { 2.0F, 0.0F, 0.0F } } },
                { { .position = { 10.0F, 0.0F, 0.0F } }, { .position = { 12.0F, 4.0F, 0.0F } } },
            });
            buffer = std::make_shared<Buffers::NetworkGeometryBuffer>(particles);
        }

        std::shared_ptr<Buffers::NetworkGeometryBuffer> buffer;
    };

    TEST_F(NexusSurveyNetworkBufferTest, BoundsOfANetworkCoverEveryCollection)
    {
        const auto bounds = Nexus::read_bounds(buffer);

        ASSERT_TRUE(bounds.has_value());
        expect_near(bounds->min, { 0.0F, 0.0F, 0.0F });
        expect_near(bounds->max, { 100.0F, 4.0F, 0.0F });
    }

    TEST_F(NexusSurveyNetworkBufferTest, BoundsOfACollectionCoverOnlyItsVertices)
    {
        const auto seeded = Nexus::read_bounds(buffer, 0U);
        const auto first = Nexus::read_bounds(buffer, 1U);
        const auto second = Nexus::read_bounds(buffer, 2U);

        ASSERT_TRUE(seeded.has_value());
        ASSERT_TRUE(first.has_value());
        ASSERT_TRUE(second.has_value());
        expect_near(seeded->min, { 100.0F, 0.0F, 0.0F });
        expect_near(seeded->max, { 100.0F, 0.0F, 0.0F });
        expect_near(first->min, { 0.0F, 0.0F, 0.0F });
        expect_near(first->max, { 2.0F, 0.0F, 0.0F });
        expect_near(second->min, { 10.0F, 0.0F, 0.0F });
        expect_near(second->max, { 12.0F, 4.0F, 0.0F });
    }

    TEST_F(NexusSurveyNetworkBufferTest, ACollectionThatDoesNotExistHasNoBounds)
    {
        EXPECT_FALSE(Nexus::read_bounds(buffer, 3U).has_value());
    }

    TEST_F(NexusSurveyNetworkBufferTest, AnchorOfANetworkIsTheMeanOfEveryVertex)
    {
        const auto anchor = Nexus::read_anchor(buffer, std::nullopt);

        ASSERT_TRUE(anchor.has_value());
        expect_near(*anchor, { 24.8F, 0.8F, 0.0F });
    }

    TEST_F(NexusSurveyNetworkBufferTest, AnchorAtAnIndexFollowsTheCollectionOrder)
    {
        const auto first = Nexus::read_anchor(buffer, 2U);
        const auto second = Nexus::read_anchor(buffer, 3U);

        ASSERT_TRUE(first.has_value());
        ASSERT_TRUE(second.has_value());
        expect_near(*first, { 2.0F, 0.0F, 0.0F });
        expect_near(*second, { 10.0F, 0.0F, 0.0F });
    }

    TEST_F(NexusSurveyNetworkBufferTest, AnchorAtAnIndexBeyondTheVerticesCannotBeRead)
    {
        EXPECT_FALSE(Nexus::read_anchor(buffer, 5U).has_value());
    }

    // =========================================================================
    // Mesh network
    // =========================================================================

    TEST(NexusSurveyMeshNetworkTest, AMeshNetworkIsReadAsOneMesh)
    {
        auto network = std::make_shared<MeshNetwork>();
        network->add_slot("near", quad_node({ 0.0F, 0.0F, 0.0F }));
        network->add_slot("far", quad_node({ 10.0F, 0.0F, 0.0F }));
        const auto buffer = std::make_shared<Buffers::MeshNetworkBuffer>(network);

        const auto bounds = Nexus::read_bounds(buffer);
        const auto anchor = Nexus::read_anchor(buffer, std::nullopt);

        ASSERT_TRUE(bounds.has_value());
        ASSERT_TRUE(anchor.has_value());
        expect_near(bounds->min, { -0.2F, -0.2F, 0.0F });
        expect_near(bounds->max, { 10.2F, 0.2F, 0.0F });
        expect_near(*anchor, { 5.0F, 0.0F, 0.0F });
    }

    // =========================================================================
    // Instance network
    // =========================================================================

    class NexusSurveyInstanceBufferTest : public ::testing::Test {
    protected:
        uint32_t add(const glm::mat4& transform)
        {
            const uint32_t slot = network->add_slot("slot_" + std::to_string(network->slot_count()), quad_node({ 0.0F, 0.0F, 0.0F }));
            network->get_slot(slot).transform = transform;
            return slot;
        }

        [[nodiscard]] std::shared_ptr<Buffers::InstanceNetworkBuffer> buffer() const
        {
            return std::make_shared<Buffers::InstanceNetworkBuffer>(network);
        }

        std::shared_ptr<InstanceNetwork> network = std::make_shared<InstanceNetwork>();
    };

    TEST_F(NexusSurveyInstanceBufferTest, BoundsOfOneInstanceFollowItsTransform)
    {
        add(glm::translate(glm::mat4(1.0F), { 10.0F, 0.0F, 0.0F }));

        const auto bounds = Nexus::read_bounds(buffer());

        ASSERT_TRUE(bounds.has_value());
        expect_near(bounds->min, { 9.8F, -0.2F, 0.0F });
        expect_near(bounds->max, { 10.2F, 0.2F, 0.0F });
    }

    TEST_F(NexusSurveyInstanceBufferTest, BoundsOfSeveralInstancesAreTheirUnion)
    {
        add(glm::translate(glm::mat4(1.0F), { 10.0F, 0.0F, 0.0F }));
        add(glm::translate(glm::mat4(1.0F), { -10.0F, 5.0F, 0.0F }));

        const auto bounds = Nexus::read_bounds(buffer());

        ASSERT_TRUE(bounds.has_value());
        expect_near(bounds->min, { -10.2F, -0.2F, 0.0F });
        expect_near(bounds->max, { 10.2F, 5.2F, 0.0F });
    }

    TEST_F(NexusSurveyInstanceBufferTest, AScaledInstanceIsBoundedByItsScaledCorners)
    {
        add(glm::scale(glm::mat4(1.0F), { 5.0F, 5.0F, 5.0F }));

        const auto bounds = Nexus::read_bounds(buffer());

        ASSERT_TRUE(bounds.has_value());
        expect_near(bounds->min, { -1.0F, -1.0F, 0.0F });
        expect_near(bounds->max, { 1.0F, 1.0F, 0.0F });
    }

    TEST_F(NexusSurveyInstanceBufferTest, AnchorOfInstancesIsTheCentreOfTheirBounds)
    {
        add(glm::translate(glm::mat4(1.0F), { 10.0F, 0.0F, 0.0F }));
        add(glm::translate(glm::mat4(1.0F), { 0.0F, 4.0F, 0.0F }));

        const auto anchor = Nexus::read_anchor(buffer(), std::nullopt);

        ASSERT_TRUE(anchor.has_value());
        expect_near(*anchor, { 5.0F, 2.0F, 0.0F });
    }

    TEST_F(NexusSurveyInstanceBufferTest, AnIndexedAnchorOfInstancesCannotBeRead)
    {
        add(glm::mat4(1.0F));

        EXPECT_FALSE(Nexus::read_anchor(buffer(), 0U).has_value());
    }

    TEST_F(NexusSurveyInstanceBufferTest, AnInstanceNetworkWithoutSlotsCannotBeRead)
    {
        EXPECT_FALSE(Nexus::read_bounds(buffer()).has_value());
        EXPECT_FALSE(Nexus::read_anchor(buffer(), std::nullopt).has_value());
    }

    // =========================================================================
    // Volume grid
    // =========================================================================

    class NexusSurveyVolumeTest : public VolumeGridDeviceFixture { };

    TEST_F(NexusSurveyVolumeTest, BoundsOfAVolumeAreItsLatticeBounds)
    {
        const auto volume = attach({ scalar_decl("density") }, unit_cell_lattice());

        const auto bounds = Nexus::read_bounds(volume);

        ASSERT_TRUE(bounds.has_value());
        expect_near(bounds->min, { 0.0F, 0.0F, 0.0F });
        expect_near(bounds->max, { 4.0F, 4.0F, 4.0F });
    }

    TEST_F(NexusSurveyVolumeTest, AVolumeHasNoAnchor)
    {
        const auto volume = attach({ scalar_decl("density") }, unit_cell_lattice());

        EXPECT_FALSE(Nexus::read_anchor(volume, std::nullopt).has_value());
    }

}
}
