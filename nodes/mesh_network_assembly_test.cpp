#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kakshya/Semantic/MeshAssemblyContainer.hpp"
#include "MayaFlux/Nodes/Graphics/MeshWriterNode.hpp"
#include "MayaFlux/Nodes/Network/MeshNetwork.hpp"
#include "MayaFlux/Portal/Graphics/GraphicsUtils.hpp"

#include <glm/gtc/matrix_transform.hpp>

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

namespace {

    using Kakshya::MeshAssemblyContainer;
    using Nodes::Network::make_mesh_network;

    glm::mat4 shifted(float x, float y = 0.0F, float z = 0.0F)
    {
        return glm::translate(glm::mat4(1.0F), glm::vec3(x, y, z));
    }

    Kakshya::MeshData corner_triangle()
    {
        return Kakshya::MeshData::from_positions(
            std::vector<glm::vec3> { { 0.0F, 0.0F, 0.0F }, { 1.0F, 0.0F, 0.0F }, { 0.0F, 1.0F, 0.0F } },
            std::vector<uint32_t> { 0, 1, 2 })
            .value();
    }

    Kakshya::MeshData unit_quad()
    {
        return Kakshya::MeshData::from_positions(
            std::vector<glm::vec3> { { 0.0F, 0.0F, 0.0F }, { 1.0F, 0.0F, 0.0F }, { 1.0F, 1.0F, 0.0F }, { 0.0F, 1.0F, 0.0F } },
            Kakshya::FaceList { .sizes = { 4 }, .corners = { 0, 1, 2, 3 } })
            .value();
    }

    /** A joint at x = 10, two arms sharing one mesh under it, and a lone lamp. */
    struct Rig {
        std::shared_ptr<MeshAssemblyContainer> assembly = std::make_shared<MeshAssemblyContainer>();

        Rig()
        {
            const auto arm = assembly->add_source({ .geometry = corner_triangle(), .name = "arm" });
            const auto lamp = assembly->add_source({ .geometry = unit_quad(), .name = "lamp" });
            const auto base = assembly->add_placement({ .name = "base", .local = shifted(10.0F) }).value();
            std::ignore = assembly->add_placement({ .name = "left", .parent = base, .source = arm, .local = shifted(1.0F) });
            std::ignore = assembly->add_placement({ .name = "right", .parent = base, .source = arm, .local = shifted(-1.0F) });
            std::ignore = assembly->add_placement({ .name = "lamp", .source = lamp, .local = shifted(0.0F, 5.0F) });
        }
    };

}

TEST(MeshNetworkAssemblyTest, EveryRowBecomesTheSlotOfTheSameIndexWithItsNameAndParent)
{
    const Rig rig;

    const auto network = make_mesh_network(rig.assembly);

    ASSERT_NE(network, nullptr);
    ASSERT_EQ(network->slot_count(), 4U);
    EXPECT_EQ(network->get_slot(0).name, "base");
    EXPECT_EQ(network->get_slot(1).name, "left");
    EXPECT_EQ(network->get_slot(3).name, "lamp");
    EXPECT_FALSE(network->get_slot(0).parent_index.has_value());
    EXPECT_EQ(network->get_slot(1).parent_index, std::optional<uint32_t> { 0 });
    EXPECT_EQ(network->get_slot(2).parent_index, std::optional<uint32_t> { 0 });
    EXPECT_FALSE(network->get_slot(3).parent_index.has_value());
}

TEST(MeshNetworkAssemblyTest, AJointCarriesNoNodeAndEachMeshRowGetsACopyOfItsOwn)
{
    const Rig rig;

    const auto network = make_mesh_network(rig.assembly);

    ASSERT_NE(network, nullptr);
    EXPECT_EQ(network->get_slot(0).node, nullptr);
    ASSERT_NE(network->get_slot(1).node, nullptr);
    ASSERT_NE(network->get_slot(2).node, nullptr);
    EXPECT_NE(network->get_slot(1).node, network->get_slot(2).node);
    EXPECT_EQ(network->get_slot(1).node->get_mesh_vertex_count(), 3U);
    EXPECT_EQ(network->get_slot(3).node->get_mesh_vertex_count(), 4U);
}

TEST(MeshNetworkAssemblyTest, ASlotsTransformsAreItsRowsLocalAndWorldPlacement)
{
    const Rig rig;

    const auto network = make_mesh_network(rig.assembly);

    ASSERT_NE(network, nullptr);
    EXPECT_FLOAT_EQ(network->get_slot(0).local_transform[3].x, 10.0F);
    EXPECT_FLOAT_EQ(network->get_slot(1).local_transform[3].x, 1.0F);
    EXPECT_FLOAT_EQ(network->get_slot(1).world_transform[3].x, 11.0F);
    EXPECT_FLOAT_EQ(network->get_slot(2).world_transform[3].x, 9.0F);
    EXPECT_FLOAT_EQ(network->get_slot(3).world_transform[3].y, 5.0F);
}

TEST(MeshNetworkAssemblyTest, ASlotsNodeKeepsThePolygonFacesOfItsMesh)
{
    const Rig rig;

    const auto network = make_mesh_network(rig.assembly);

    ASSERT_NE(network, nullptr);
    const auto& faces = network->get_slot(3).node->get_mesh_faces();
    ASSERT_TRUE(faces.has_value());
    EXPECT_EQ(faces->sizes, (std::vector<uint32_t> { 4 }));
}

TEST(MeshNetworkAssemblyTest, ARowShowingPointsIsAJointBecauseAMeshNodeCannotHoldThem)
{
    std::shared_ptr<MeshAssemblyContainer> assembly = std::make_shared<MeshAssemblyContainer>();
    Kakshya::SpatialData cloud;
    cloud.topology = Portal::Graphics::PrimitiveTopology::POINT_LIST;
    cloud.positions = { { 0.0F, 0.0F, 0.0F }, { 1.0F, 0.0F, 0.0F } };
    const auto source = assembly->add_source({ .geometry = cloud, .name = "cloud" });
    std::ignore = assembly->add_placement({ .name = "cloud", .source = source, .local = shifted(2.0F) });

    const auto network = make_mesh_network(assembly);

    ASSERT_NE(network, nullptr);
    ASSERT_EQ(network->slot_count(), 1U);
    EXPECT_EQ(network->get_slot(0).node, nullptr);
    EXPECT_FLOAT_EQ(network->get_slot(0).local_transform[3].x, 2.0F);
}

TEST(MeshNetworkAssemblyTest, AnAssemblyWithNoRowsMakesNoNetwork)
{
    const auto assembly = std::make_shared<const MeshAssemblyContainer>();

    EXPECT_EQ(make_mesh_network(assembly), nullptr);
}

}
