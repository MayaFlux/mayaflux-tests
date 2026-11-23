#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kakshya/Semantic/MeshAssemblyContainer.hpp"
#include "MayaFlux/Nodes/Graphics/MeshWriterNode.hpp"
#include "MayaFlux/Nodes/Network/MeshNetwork.hpp"

#include <glm/gtc/matrix_transform.hpp>

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Kakshya::MeshAssemblyContainer;
    using Nodes::Network::make_mesh_network;
    using Nodes::Network::MeshPartPlacement;

    glm::mat4 shifted(float x)
    {
        return glm::translate(glm::mat4(1.0F), glm::vec3(x, 0.0F, 0.0F));
    }

    Kakshya::MeshData network_triangle()
    {
        return Kakshya::MeshData::from_positions(
            std::vector<glm::vec3> { { 0.0F, 0.0F, 0.0F }, { 1.0F, 0.0F, 0.0F }, { 0.0F, 1.0F, 0.0F } },
            std::vector<uint32_t> { 0, 1, 2 })
            .value();
    }

    /**
     * Rows: 0 a joint at x = 10, 1 a wall, 2 and 3 wheels at x = 1 and -1 under it, 4 a deeper
     * joint under row 2 at y = 5, 5 a lamp under that joint, 6 a set of points.
     */
    struct NetworkLevel {
        std::shared_ptr<MeshAssemblyContainer> assembly = std::make_shared<MeshAssemblyContainer>();

        NetworkLevel()
        {
            const auto mesh = assembly->add_source({ .geometry = network_triangle(), .name = "mesh" });
            Kakshya::SpatialData points;
            points.topology = Portal::Graphics::PrimitiveTopology::POINT_LIST;
            points.positions = { glm::vec3(0.0F) };
            const auto dust = assembly->add_source({ .geometry = points, .name = "dust" });

            const auto level = assembly->add_placement({ .name = "level", .local = shifted(10.0F) }).value();
            std::ignore = assembly->add_placement({ .name = "wall", .parent = level, .source = mesh });
            const auto left = assembly->add_placement({ .name = "left", .parent = level, .source = mesh, .local = shifted(1.0F) }).value();
            std::ignore = assembly->add_placement({ .name = "right", .parent = level, .source = mesh, .local = shifted(-1.0F) });
            const auto mount = assembly->add_placement({ .name = "mount", .parent = left, .local = glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 5.0F, 0.0F)) }).value();
            std::ignore = assembly->add_placement({ .name = "lamp", .parent = mount, .source = mesh });
            std::ignore = assembly->add_placement({ .name = "dust", .source = dust });
        }
    };

    using Rows = std::vector<uint64_t>;

    TEST(MeshNetworkPartTest, AFlatPartHasOneRootSlotForEveryRowThatShowsAMesh)
    {
        const NetworkLevel level;

        const auto part = make_mesh_network(level.assembly, Rows { 1, 2, 3 });

        ASSERT_TRUE(part.has_value());
        EXPECT_EQ(part->rows, (Rows { 1, 2, 3 }));
        ASSERT_EQ(part->network->slot_count(), 3U);
        for (uint32_t slot = 0; slot < 3; ++slot) {
            EXPECT_FALSE(part->network->get_slot(slot).parent_index.has_value());
            ASSERT_NE(part->network->get_slot(slot).node, nullptr);
            EXPECT_EQ(part->network->get_slot(slot).node->get_mesh_vertex_count(), 3U);
        }
        EXPECT_EQ(part->network->get_slot(1).name, "left");
    }

    TEST(MeshNetworkPartTest, AFlatSlotStartsAtTheWorldPlacementOfItsRow)
    {
        const NetworkLevel level;

        const auto part = make_mesh_network(level.assembly, Rows { 1, 2, 3 });

        ASSERT_TRUE(part.has_value());
        EXPECT_FLOAT_EQ(part->network->get_slot(0).local_transform[3].x, 10.0F);
        EXPECT_FLOAT_EQ(part->network->get_slot(1).local_transform[3].x, 11.0F);
        EXPECT_FLOAT_EQ(part->network->get_slot(2).local_transform[3].x, 9.0F);
        EXPECT_FLOAT_EQ(part->network->get_slot(1).world_transform[3].x, 11.0F);
    }

    TEST(MeshNetworkPartTest, ASlotIsFoundByItsRow)
    {
        const NetworkLevel level;

        const auto part = make_mesh_network(level.assembly, Rows { 1, 3 });

        ASSERT_TRUE(part.has_value());
        EXPECT_EQ(part->slot_of(1), std::optional<uint32_t> { 0 });
        EXPECT_EQ(part->slot_of(3), std::optional<uint32_t> { 1 });
        EXPECT_EQ(part->slot_of(2), std::nullopt);
        EXPECT_EQ(part->slot_of(0), std::nullopt);
    }

    TEST(MeshNetworkPartTest, RowsInAnyOrderAndRepeatedAreTakenOnceInRowOrder)
    {
        const NetworkLevel level;

        const auto part = make_mesh_network(level.assembly, Rows { 3, 1, 3, 2 });

        ASSERT_TRUE(part.has_value());
        EXPECT_EQ(part->rows, (Rows { 1, 2, 3 }));
    }

    TEST(MeshNetworkPartTest, AFlatPartLeavesOutJointsAndRowsThatShowNoMesh)
    {
        const NetworkLevel level;

        const auto part = make_mesh_network(level.assembly, Rows { 0, 1, 4, 6 });

        ASSERT_TRUE(part.has_value());
        EXPECT_EQ(part->rows, (Rows { 1 }));
        EXPECT_EQ(part->network->slot_count(), 1U);
    }

    TEST(MeshNetworkPartTest, NothingToShowMakesNoPart)
    {
        const NetworkLevel level;

        EXPECT_EQ(make_mesh_network(level.assembly, Rows {}), std::nullopt);
        EXPECT_EQ(make_mesh_network(level.assembly, Rows { 0, 4 }), std::nullopt);
        EXPECT_EQ(make_mesh_network(level.assembly, Rows { 1, 99 }), std::nullopt);
        EXPECT_EQ(make_mesh_network(nullptr, Rows { 1 }), std::nullopt);
        EXPECT_EQ(make_mesh_network(level.assembly, Rows { 0, 4 }, MeshPartPlacement::Chained), std::nullopt);
    }

    TEST(MeshNetworkPartTest, AChainedPartBringsTheAncestorsOfItsRowsAsJoints)
    {
        const NetworkLevel level;

        const auto part = make_mesh_network(level.assembly, Rows { 5 }, MeshPartPlacement::Chained);

        ASSERT_TRUE(part.has_value());
        EXPECT_EQ(part->rows, (Rows { 0, 2, 4, 5 }));
        ASSERT_EQ(part->network->slot_count(), 4U);
        EXPECT_EQ(part->network->get_slot(0).node, nullptr);
        EXPECT_EQ(part->network->get_slot(1).node, nullptr);
        EXPECT_EQ(part->network->get_slot(2).node, nullptr);
        ASSERT_NE(part->network->get_slot(3).node, nullptr);
    }

    TEST(MeshNetworkPartTest, AChainedPartKeepsTheParentLinksAndLocalPlacements)
    {
        const NetworkLevel level;

        const auto part = make_mesh_network(level.assembly, Rows { 5 }, MeshPartPlacement::Chained);

        ASSERT_TRUE(part.has_value());
        EXPECT_FALSE(part->network->get_slot(0).parent_index.has_value());
        EXPECT_EQ(part->network->get_slot(1).parent_index, std::optional<uint32_t> { 0 });
        EXPECT_EQ(part->network->get_slot(2).parent_index, std::optional<uint32_t> { 1 });
        EXPECT_EQ(part->network->get_slot(3).parent_index, std::optional<uint32_t> { 2 });
        EXPECT_FLOAT_EQ(part->network->get_slot(1).local_transform[3].x, 1.0F);
        EXPECT_FLOAT_EQ(part->network->get_slot(2).local_transform[3].y, 5.0F);
        EXPECT_FLOAT_EQ(part->network->get_slot(3).world_transform[3].x, 11.0F);
        EXPECT_FLOAT_EQ(part->network->get_slot(3).world_transform[3].y, 5.0F);
    }

    TEST(MeshNetworkPartTest, SiblingsShareTheirAncestorsInAChainedPart)
    {
        const NetworkLevel level;

        const auto part = make_mesh_network(level.assembly, Rows { 2, 3 }, MeshPartPlacement::Chained);

        ASSERT_TRUE(part.has_value());
        EXPECT_EQ(part->rows, (Rows { 0, 2, 3 }));
        EXPECT_EQ(part->network->get_slot(1).parent_index, std::optional<uint32_t> { 0 });
        EXPECT_EQ(part->network->get_slot(2).parent_index, std::optional<uint32_t> { 0 });
        EXPECT_EQ(part->slot_of(0), std::optional<uint32_t> { 0 });
    }

    TEST(MeshNetworkPartTest, OnlyTheChosenRowsOfAChainedPartShowMeshes)
    {
        const NetworkLevel level;

        const auto part = make_mesh_network(level.assembly, Rows { 3 }, MeshPartPlacement::Chained);

        ASSERT_TRUE(part.has_value());
        EXPECT_EQ(part->rows, (Rows { 0, 3 }));
        EXPECT_EQ(part->network->get_slot(0).node, nullptr);
        ASSERT_NE(part->network->get_slot(1).node, nullptr);
    }

    TEST(MeshNetworkPartTest, ARowThatIsBothChosenAndAnAncestorKeepsItsMesh)
    {
        const NetworkLevel level;

        const auto part = make_mesh_network(level.assembly, Rows { 2, 5 }, MeshPartPlacement::Chained);

        ASSERT_TRUE(part.has_value());
        EXPECT_EQ(part->rows, (Rows { 0, 2, 4, 5 }));
        EXPECT_NE(part->network->get_slot(1).node, nullptr);
        EXPECT_EQ(part->network->get_slot(2).node, nullptr);
        EXPECT_NE(part->network->get_slot(3).node, nullptr);
    }

    TEST(MeshNetworkPartTest, TheMeshOfARowIsCopiedIntoItsSlot)
    {
        const NetworkLevel level;

        const auto part = make_mesh_network(level.assembly, Rows { 1, 2 });

        ASSERT_TRUE(part.has_value());
        EXPECT_NE(part->network->get_slot(0).node, part->network->get_slot(1).node);
    }

} // namespace
} // namespace MayaFlux::Test
