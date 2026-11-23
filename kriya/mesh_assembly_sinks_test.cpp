#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Buffers/Network/RecordedGeometryBuffer.hpp"
#include "MayaFlux/Kriya/MeshReflow.hpp"
#include "MayaFlux/Kriya/MotionReflow.hpp"
#include "MayaFlux/Nodes/Graphics/MeshWriterNode.hpp"
#include "MayaFlux/Nodes/Network/InstanceNetwork.hpp"
#include "MayaFlux/Nodes/Network/MeshNetwork.hpp"

#include <glm/gtc/matrix_transform.hpp>

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Kakshya::MeshAssemblyContainer;
    using Kakshya::MeshAssemblyLocals;
    using Kakshya::MeshData;
    using Kriya::FrameContext;
    using Nodes::Network::make_instance_network;
    using Nodes::Network::make_mesh_network;
    using Nodes::Network::MeshPartPlacement;

    glm::mat4 sink_shift(float x)
    {
        return glm::translate(glm::mat4(1.0F), glm::vec3(x, 0.0F, 0.0F));
    }

    MeshData sink_triangle()
    {
        return MeshData::from_positions(
            std::vector<glm::vec3> { { 0.0F, 0.0F, 0.0F }, { 1.0F, 0.0F, 0.0F }, { 0.0F, 1.0F, 0.0F } },
            std::vector<uint32_t> { 0, 1, 2 })
            .value();
    }

    MeshData sink_quad()
    {
        return MeshData::from_positions(
            std::vector<glm::vec3> { { 0.0F, 0.0F, 0.0F }, { 1.0F, 0.0F, 0.0F }, { 1.0F, 1.0F, 0.0F }, { 0.0F, 1.0F, 0.0F } },
            std::vector<uint32_t> { 0, 1, 2, 0, 2, 3 })
            .value();
    }

    using Rows = std::vector<uint64_t>;

    /** Row 0 a joint at x = 10, row 1 a wall, rows 2 and 3 two wheels of one mesh at x = 1 and -1 under it. */
    struct SinkLevel {
        std::shared_ptr<MeshAssemblyContainer> assembly = std::make_shared<MeshAssemblyContainer>();

        SinkLevel()
        {
            const auto wall = assembly->add_source({ .geometry = sink_triangle(), .name = "wall" });
            const auto wheel = assembly->add_source({ .geometry = sink_triangle(), .name = "wheel" });
            const auto level = assembly->add_placement({ .name = "level", .local = sink_shift(10.0F) }).value();
            std::ignore = assembly->add_placement({ .name = "wall", .parent = level, .source = wall });
            std::ignore = assembly->add_placement({ .name = "left", .parent = level, .source = wheel, .local = sink_shift(1.0F) });
            std::ignore = assembly->add_placement({ .name = "right", .parent = level, .source = wheel, .local = sink_shift(-1.0F) });
        }

        /** A frame whose joint is at the given x, the other rows at their rest placements below it. */
        [[nodiscard]] std::shared_ptr<const MeshAssemblyLocals> frame_with_joint_at(float x) const
        {
            auto locals = assembly->local_transforms();
            locals[0] = sink_shift(x);
            return std::make_shared<const MeshAssemblyLocals>(Kakshya::resolve_world(*assembly, std::move(locals)));
        }
    };

    TEST(PartSinkTest, AFlatPartTakesTheWorldPlacementOfEachRow)
    {
        const SinkLevel level;
        const auto part = make_mesh_network(level.assembly, Rows { 1, 2, 3 });
        ASSERT_TRUE(part.has_value());

        Kriya::slot_sink(part->network, part->rows, MeshPartPlacement::Flat)(level.frame_with_joint_at(20.0F), FrameContext {});

        EXPECT_FLOAT_EQ(part->network->get_slot(0).local_transform[3].x, 20.0F);
        EXPECT_FLOAT_EQ(part->network->get_slot(1).local_transform[3].x, 21.0F);
        EXPECT_FLOAT_EQ(part->network->get_slot(2).local_transform[3].x, 19.0F);
    }

    TEST(PartSinkTest, ASlotIsMarkedDirtyOnlyWhenItsPlacementChanges)
    {
        const SinkLevel level;
        const auto part = make_mesh_network(level.assembly, Rows { 1, 2 });
        ASSERT_TRUE(part.has_value());
        const auto sink = Kriya::slot_sink(part->network, part->rows, MeshPartPlacement::Flat);
        const auto frame = level.frame_with_joint_at(20.0F);

        sink(frame, FrameContext {});
        for (auto& slot : part->network->slots()) {
            slot.dirty = false;
        }

        sink(frame, FrameContext {});
        EXPECT_FALSE(part->network->get_slot(0).dirty);
        EXPECT_FALSE(part->network->get_slot(1).dirty);

        sink(level.frame_with_joint_at(21.0F), FrameContext {});
        EXPECT_TRUE(part->network->get_slot(0).dirty);
        EXPECT_TRUE(part->network->get_slot(1).dirty);
    }

    TEST(PartSinkTest, AFlatPartNeedsTheWorldPlacementsAndWritesNothingWithoutThem)
    {
        const SinkLevel level;
        const auto part = make_mesh_network(level.assembly, Rows { 1, 2 });
        ASSERT_TRUE(part.has_value());
        MeshAssemblyLocals locals;
        locals.matrices = level.assembly->local_transforms();
        locals.matrices[1] = sink_shift(99.0F);

        Kriya::slot_sink(part->network, part->rows, MeshPartPlacement::Flat)(
            std::make_shared<const MeshAssemblyLocals>(std::move(locals)), FrameContext {});

        EXPECT_FLOAT_EQ(part->network->get_slot(0).local_transform[3].x, 10.0F);
        EXPECT_FLOAT_EQ(part->network->get_slot(1).local_transform[3].x, 11.0F);
    }

    TEST(PartSinkTest, AChainedPartTakesTheLocalPlacementAndLeavesTheHierarchyToTheNetwork)
    {
        const SinkLevel level;
        const auto part = make_mesh_network(level.assembly, Rows { 2 }, MeshPartPlacement::Chained);
        ASSERT_TRUE(part.has_value());
        ASSERT_EQ(part->rows, (Rows { 0, 2 }));
        auto locals = level.assembly->local_transforms();
        locals[0] = sink_shift(30.0F);
        locals[2] = sink_shift(5.0F);

        Kriya::slot_sink(part->network, part->rows, MeshPartPlacement::Chained)(
            std::make_shared<const MeshAssemblyLocals>(Kakshya::resolve_world(*level.assembly, std::move(locals))), FrameContext {});

        EXPECT_FLOAT_EQ(part->network->get_slot(0).local_transform[3].x, 30.0F);
        EXPECT_FLOAT_EQ(part->network->get_slot(1).local_transform[3].x, 5.0F);
    }

    TEST(PartSinkTest, ANetworkThatIsGoneAndANullFrameEndTheWritesQuietly)
    {
        const SinkLevel level;
        auto part = make_mesh_network(level.assembly, Rows { 1, 2 });
        ASSERT_TRUE(part.has_value());
        const auto sink = Kriya::slot_sink(part->network, part->rows, MeshPartPlacement::Flat);

        EXPECT_NO_THROW(sink(nullptr, FrameContext {}));
        EXPECT_FLOAT_EQ(part->network->get_slot(0).local_transform[3].x, 10.0F);

        part->network.reset();
        EXPECT_NO_THROW(sink(level.frame_with_joint_at(1.0F), FrameContext {}));
    }

    TEST(PartSinkTest, InstancesTakeTheWorldPlacementOfTheirRowsAndAreDirtyOnlyOnChange)
    {
        const SinkLevel level;
        const auto groups = level.assembly->groups();
        const auto wheels = std::ranges::find_if(groups, [](const auto& group) { return group.rows.size() == 2; });
        ASSERT_NE(wheels, groups.end());
        const auto instances = make_instance_network(level.assembly, *wheels);
        ASSERT_NE(instances, nullptr);
        const auto sink = Kriya::instance_sink(instances, wheels->rows);
        for (auto& slot : instances->slots()) {
            slot.dirty = false;
        }

        sink(level.frame_with_joint_at(20.0F), FrameContext {});

        EXPECT_FLOAT_EQ(instances->get_slot(0).transform[3].x, 21.0F);
        EXPECT_FLOAT_EQ(instances->get_slot(1).transform[3].x, 19.0F);
        EXPECT_TRUE(instances->get_slot(0).dirty);

        for (auto& slot : instances->slots()) {
            slot.dirty = false;
        }
        sink(level.frame_with_joint_at(20.0F), FrameContext {});
        EXPECT_FALSE(instances->get_slot(0).dirty);
    }

    TEST(PartSinkTest, ABufferThatShowsAPointSetIsPlacedWithoutARenderProcessorYet)
    {
        const auto buffer = std::make_shared<Buffers::RecordedGeometryBuffer>(
            8, Portal::Graphics::PrimitiveTopology::POINT_LIST, Kinesis::SamplerBounds {});
        const SinkLevel level;
        const auto sink = Kriya::placement_sink(buffer, 2);

        EXPECT_NO_THROW(sink(level.frame_with_joint_at(5.0F), FrameContext {}));
        EXPECT_NO_THROW(sink(std::make_shared<const MeshAssemblyLocals>(), FrameContext {}));
        EXPECT_NO_THROW(sink(nullptr, FrameContext {}));
    }

    TEST(PartGeometrySinkTest, ASlotTakesEachFrameWhateverItsSize)
    {
        const SinkLevel level;
        const auto part = make_mesh_network(level.assembly, Rows { 1, 2 });
        ASSERT_TRUE(part.has_value());
        ASSERT_EQ(part->network->get_slot(1).node->get_mesh_vertex_count(), 3U);

        Kriya::slot_geometry_sink(part->network, 1)(std::make_shared<const MeshData>(sink_quad()), FrameContext {});

        EXPECT_EQ(part->network->get_slot(1).node->get_mesh_vertex_count(), 4U);
        EXPECT_EQ(part->network->get_slot(0).node->get_mesh_vertex_count(), 3U);
    }

    TEST(PartGeometrySinkTest, ASlotWithNoNodeAOneOutOfRangeANullFrameAndAGoneNetworkTakeNothing)
    {
        const SinkLevel level;
        auto part = make_mesh_network(level.assembly, Rows { 2 }, MeshPartPlacement::Chained);
        ASSERT_TRUE(part.has_value());
        const auto frame = std::make_shared<const MeshData>(sink_quad());

        EXPECT_NO_THROW(Kriya::slot_geometry_sink(part->network, 0)(frame, FrameContext {}));
        EXPECT_NO_THROW(Kriya::slot_geometry_sink(part->network, 42)(frame, FrameContext {}));
        EXPECT_NO_THROW(Kriya::slot_geometry_sink(part->network, 1)(nullptr, FrameContext {}));
        EXPECT_EQ(part->network->get_slot(1).node->get_mesh_vertex_count(), 3U);

        const auto sink = Kriya::slot_geometry_sink(part->network, 1);
        part->network.reset();
        EXPECT_NO_THROW(sink(frame, FrameContext {}));
    }

    TEST(PartGeometrySinkTest, EveryInstanceDrawsTheTemplateTheFrameReplaces)
    {
        const SinkLevel level;
        const auto groups = level.assembly->groups();
        const auto wheels = std::ranges::find_if(groups, [](const auto& group) { return group.rows.size() == 2; });
        ASSERT_NE(wheels, groups.end());
        const auto instances = make_instance_network(level.assembly, *wheels);
        ASSERT_NE(instances, nullptr);

        Kriya::instance_geometry_sink(instances)(std::make_shared<const MeshData>(sink_quad()), FrameContext {});

        for (const auto& slot : instances->slots()) {
            const auto node = std::dynamic_pointer_cast<Nodes::GpuSync::MeshWriterNode>(slot.node);
            ASSERT_NE(node, nullptr);
            EXPECT_EQ(node->get_mesh_vertex_count(), 4U);
        }
    }

    TEST(FanOutSinkTest, EverySinkGetsTheFrameInOrderAndAnEmptyOneIsSkipped)
    {
        std::vector<int> order;
        std::vector<Kriya::FrameSink<int>> sinks;
        sinks.emplace_back([&order](const std::shared_ptr<const int>& frame, const FrameContext&) { order.push_back(*frame + 1); });
        sinks.emplace_back();
        sinks.emplace_back([&order](const std::shared_ptr<const int>& frame, const FrameContext&) { order.push_back(*frame + 2); });

        Kriya::fan_out<int>(std::move(sinks))(std::make_shared<const int>(10), FrameContext {});

        EXPECT_EQ(order, (std::vector<int> { 11, 12 }));
    }

    TEST(FanOutSinkTest, NoSinksIsASinkThatDoesNothing)
    {
        const auto sink = Kriya::fan_out<int>({});

        EXPECT_TRUE(static_cast<bool>(sink));
        EXPECT_NO_THROW(sink(std::make_shared<const int>(1), FrameContext {}));
    }

} // namespace
} // namespace MayaFlux::Test
