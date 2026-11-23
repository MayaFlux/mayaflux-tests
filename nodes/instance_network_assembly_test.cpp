#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kakshya/Semantic/MeshAssemblyContainer.hpp"
#include "MayaFlux/Nodes/Graphics/MeshWriterNode.hpp"
#include "MayaFlux/Nodes/Graphics/PointCollectionNode.hpp"
#include "MayaFlux/Nodes/Network/InstanceNetwork.hpp"
#include "MayaFlux/Portal/Graphics/GraphicsUtils.hpp"

#include <glm/gtc/matrix_transform.hpp>

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

namespace {

    using Kakshya::MeshAssemblyContainer;
    using Kakshya::MeshAssemblySource;
    using Nodes::Network::make_instance_network;

    glm::mat4 moved(float x, float y = 0.0F, float z = 0.0F)
    {
        return glm::translate(glm::mat4(1.0F), glm::vec3(x, y, z));
    }

    Kakshya::MeshData triangle()
    {
        return Kakshya::MeshData::from_positions(
            std::vector<glm::vec3> { { 0.0F, 0.0F, 0.0F }, { 1.0F, 0.0F, 0.0F }, { 0.0F, 1.0F, 0.0F } },
            std::vector<uint32_t> { 0, 1, 2 })
            .value();
    }

    /** a parent at x = 10 with two wheels sharing a mesh, and a lamp with another. */
    struct Wheels {
        std::shared_ptr<MeshAssemblyContainer> assembly = std::make_shared<MeshAssemblyContainer>();
        size_t wheel {};
        size_t lamp {};

        Wheels()
        {
            wheel = assembly->add_source({ .geometry = triangle(), .name = "wheel" });
            lamp = assembly->add_source({ .geometry = triangle(), .name = "lamp" });
            const auto car = assembly->add_placement({ .name = "car", .local = moved(10.0F) }).value();
            std::ignore = assembly->add_placement({ .name = "front_left", .parent = car, .source = wheel, .local = moved(1.0F) });
            std::ignore = assembly->add_placement({ .name = "front_right", .parent = car, .source = wheel, .local = moved(-1.0F) });
            std::ignore = assembly->add_placement({ .name = "lamp", .source = lamp, .local = moved(0.0F, 5.0F) });
        }
    };

}

TEST(InstanceNetworkAssemblyTest, ARowGroupBecomesOneSlotPerRowSharingOneTemplate)
{
    const Wheels wheels;
    const auto groups = wheels.assembly->groups();
    ASSERT_EQ(groups.size(), 2U);

    const auto network = make_instance_network(wheels.assembly, groups[0]);

    ASSERT_NE(network, nullptr);
    ASSERT_EQ(network->slot_count(), 2U);
    EXPECT_EQ(network->get_slot(0).name, "front_left");
    EXPECT_EQ(network->get_slot(1).name, "front_right");
    EXPECT_EQ(network->get_slot(0).node, network->get_slot(1).node);
    const auto mesh_node = std::dynamic_pointer_cast<Nodes::GpuSync::MeshWriterNode>(network->get_slot(0).node);
    ASSERT_NE(mesh_node, nullptr);
    EXPECT_EQ(mesh_node->get_mesh_vertex_count(), 3U);
}

TEST(InstanceNetworkAssemblyTest, EachSlotTakesItsRowsWorldPlacement)
{
    const Wheels wheels;
    const auto groups = wheels.assembly->groups();

    const auto network = make_instance_network(wheels.assembly, groups[0]);

    ASSERT_NE(network, nullptr);
    EXPECT_EQ(glm::vec3(network->get_slot(0).transform[3]), glm::vec3(11.0F, 0.0F, 0.0F));
    EXPECT_EQ(glm::vec3(network->get_slot(1).transform[3]), glm::vec3(9.0F, 0.0F, 0.0F));
}

TEST(InstanceNetworkAssemblyTest, ASingleRowGroupIsANetworkOfOne)
{
    const Wheels wheels;
    const auto groups = wheels.assembly->groups();

    const auto network = make_instance_network(wheels.assembly, groups[1]);

    ASSERT_NE(network, nullptr);
    EXPECT_EQ(network->slot_count(), 1U);
    EXPECT_EQ(glm::vec3(network->get_slot(0).transform[3]), glm::vec3(0.0F, 5.0F, 0.0F));
}

TEST(InstanceNetworkAssemblyTest, EverySlotKeepsTheDefaultPayload)
{
    const Wheels wheels;

    const auto network = make_instance_network(wheels.assembly, wheels.assembly->groups()[0]);

    ASSERT_NE(network, nullptr);
    for (const auto& slot : network->slots()) {
        EXPECT_EQ(slot.tint, glm::vec3(1.0F));
        EXPECT_EQ(slot.scalar, 1.0F);
        EXPECT_EQ(slot.texture, 0U);
    }
}

TEST(InstanceNetworkAssemblyTest, ASourceOfCurvesMakesNoNetwork)
{
    std::shared_ptr<MeshAssemblyContainer> assembly = std::make_shared<MeshAssemblyContainer>();
    const auto curve = assembly->add_source({
        .geometry = Kakshya::SpatialData { .topology = Portal::Graphics::PrimitiveTopology::LINE_STRIP,
            .positions = { { 0.0F, 0.0F, 0.0F }, { 1.0F, 0.0F, 0.0F } },
            .vertex_counts_per_curve = { 2 } },
        .name = "curve" });
    ASSERT_TRUE(assembly->add_placement({ .name = "line", .source = curve }).has_value());

    EXPECT_EQ(make_instance_network(assembly, assembly->groups()[0]), nullptr);
}

TEST(InstanceNetworkAssemblyTest, ASetOfPointsIsInstancedAroundOneTemplateOfItsPoints)
{
    std::shared_ptr<MeshAssemblyContainer> assembly = std::make_shared<MeshAssemblyContainer>();
    const auto points = assembly->add_source({
        .geometry = Kakshya::SpatialData { .topology = Portal::Graphics::PrimitiveTopology::POINT_LIST,
            .positions = { { 0.0F, 0.0F, 0.0F }, { 1.0F, 0.0F, 0.0F }, { 2.0F, 0.0F, 0.0F } } },
        .name = "points" });
    ASSERT_TRUE(assembly->add_placement({ .name = "first", .source = points }).has_value());
    ASSERT_TRUE(assembly->add_placement({ .name = "second", .source = points, .local = moved(5.0F) }).has_value());

    const auto network = make_instance_network(assembly, assembly->groups()[0]);

    ASSERT_NE(network, nullptr);
    ASSERT_EQ(network->slot_count(), 2U);
    EXPECT_EQ(network->get_slot(0).node, network->get_slot(1).node);
    const auto cloud = std::dynamic_pointer_cast<Nodes::GpuSync::PointCollectionNode>(network->get_slot(0).node);
    ASSERT_NE(cloud, nullptr);
    EXPECT_EQ(cloud->get_point_count(), 3U);
    EXPECT_FLOAT_EQ(network->get_slot(1).transform[3].x, 5.0F);
}

TEST(InstanceNetworkAssemblyTest, AGroupWithARowOutOfRangeMakesNoNetwork)
{
    const Wheels wheels;
    auto group = wheels.assembly->groups()[0];
    group.rows.push_back(99);

    EXPECT_EQ(make_instance_network(wheels.assembly, group), nullptr);
}

TEST(InstanceNetworkAssemblyTest, DifferentMaterialsGiveSeparateNetworksForTheSameMesh)
{
    Wheels wheels;
    const auto chrome = wheels.assembly->add_material({ .name = "chrome" });
    ASSERT_TRUE(wheels.assembly->add_placement({ .name = "spare", .parent = 0, .source = wheels.wheel, .material = chrome }).has_value());

    const auto groups = wheels.assembly->groups();

    ASSERT_EQ(groups.size(), 3U);
    const auto plain = make_instance_network(wheels.assembly, groups[0]);
    const auto shiny = make_instance_network(wheels.assembly, groups[2]);
    ASSERT_NE(plain, nullptr);
    ASSERT_NE(shiny, nullptr);
    EXPECT_EQ(plain->slot_count(), 2U);
    EXPECT_EQ(shiny->slot_count(), 1U);
    EXPECT_NE(plain->get_slot(0).node, shiny->get_slot(0).node);
}

}
