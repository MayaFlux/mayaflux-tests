#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Buffers/Network/InstanceNetworkBuffer.hpp"
#include "MayaFlux/Buffers/Network/MeshNetworkBuffer.hpp"
#include "MayaFlux/Buffers/Network/RecordedGeometryBuffer.hpp"
#include "MayaFlux/Core/Backends/Graphics/Vulkan/VKImage.hpp"
#include "MayaFlux/Nodes/Network/Operators/GpuFieldOperator.hpp"
#include "MayaFlux/Kriya/MeshAssemblyRealization.hpp"
#include "MayaFlux/Nodes/Graphics/MeshWriterNode.hpp"
#include "MayaFlux/Nodes/Network/InstanceNetwork.hpp"

#include <glm/gtc/matrix_transform.hpp>

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Kakshya::MeshAssemblyContainer;
    using Kakshya::MeshAssemblyRowKind;
    using Kakshya::MeshData;
    using Kakshya::ResidentFrames;
    using Kakshya::SpatialData;
    using Kriya::CourseEdge;
    using Kriya::MeshAssemblyRealization;
    using Kriya::MeshAssemblyRealizeOptions;
    using Nodes::Network::MeshPartPlacement;
    using Portal::Graphics::PrimitiveTopology;
    namespace Partitions = Kakshya::Partitions;

    constexpr double k_level_rate = 8.0;

    glm::mat4 level_shift(float x)
    {
        return glm::translate(glm::mat4(1.0F), glm::vec3(x, 0.0F, 0.0F));
    }

    MeshData level_triangle()
    {
        return MeshData::from_positions(
            std::vector<glm::vec3> { { 0.0F, 0.0F, 0.0F }, { 1.0F, 0.0F, 0.0F }, { 0.0F, 1.0F, 0.0F } },
            std::vector<uint32_t> { 0, 1, 2 })
            .value();
    }

    MeshData level_quad()
    {
        return MeshData::from_positions(
            std::vector<glm::vec3> { { 0.0F, 0.0F, 0.0F }, { 1.0F, 0.0F, 0.0F }, { 1.0F, 1.0F, 0.0F }, { 0.0F, 1.0F, 0.0F } },
            std::vector<uint32_t> { 0, 1, 2, 0, 2, 3 })
            .value();
    }

    SpatialData level_points(size_t count)
    {
        SpatialData shot;
        shot.topology = PrimitiveTopology::POINT_LIST;
        for (size_t i = 0; i < count; ++i) {
            shot.positions.emplace_back(static_cast<float>(i), 0.0F, 0.0F);
        }
        return shot;
    }

    Kinesis::TimeSampling level_times()
    {
        return *Kinesis::TimeSampling::uniform(0.125);
    }

    template <typename Frame>
    std::shared_ptr<const ResidentFrames<Frame>> level_frames(std::vector<Frame> frames)
    {
        return ResidentFrames<Frame>::create(level_times(), std::move(frames));
    }

    /**
     * Row 0 a joint at x = 10 that slides to x = 2 over two seconds, row 1 a wall, rows 2 and 3
     * two wheels of one mesh at x = 1 and -1 under the joint, row 4 a set of three points and
     * row 5 a curve.
     */
    struct RealizedLevel {
        std::shared_ptr<MeshAssemblyContainer> assembly = std::make_shared<MeshAssemblyContainer>();
        size_t wall {};
        size_t wheel {};
        size_t dust {};
        size_t rail {};

        RealizedLevel()
        {
            wall = assembly->add_source({ .geometry = level_triangle(), .name = "wall" });
            wheel = assembly->add_source({ .geometry = level_triangle(), .name = "wheel" });
            dust = assembly->add_source({ .geometry = level_points(3), .name = "dust" });

            SpatialData curve;
            curve.topology = PrimitiveTopology::LINE_STRIP;
            curve.positions = { glm::vec3(0.0F), glm::vec3(1.0F), glm::vec3(2.0F) };
            curve.vertex_counts_per_curve = { 3 };
            rail = assembly->add_source({ .geometry = curve, .name = "rail" });

            const auto level = assembly->add_placement({ .name = "level", .local = level_shift(10.0F) }).value();
            std::ignore = assembly->add_placement({ .name = "wall", .parent = level, .source = wall });
            std::ignore = assembly->add_placement({ .name = "left", .parent = level, .source = wheel, .local = level_shift(1.0F) });
            std::ignore = assembly->add_placement({ .name = "right", .parent = level, .source = wheel, .local = level_shift(-1.0F) });
            std::ignore = assembly->add_placement({ .name = "dust", .source = dust });
            std::ignore = assembly->add_placement({ .name = "rail", .source = rail });

            const auto times = *Kinesis::TimeSampling::acyclic({ 0.0, 2.0 });
            auto slide = Kinesis::TimedValues<glm::vec3>::create({ .times = times,
                .values = { glm::vec3(10.0F, 0.0F, 0.0F), glm::vec3(2.0F, 0.0F, 0.0F) } });
            std::ignore = assembly->add_motion({ .name = "slide", .channels = { { .row = level, .translation = slide } } });
        }

        [[nodiscard]] std::shared_ptr<MeshAssemblyRealization> realize(const MeshAssemblyRealizeOptions& options = {}) const
        {
            return MeshAssemblyRealization::create(assembly, options);
        }
    };

    void level_run(Kriya::MotionReflow& reflow, int cycles)
    {
        for (int i = 0; i < cycles; ++i) {
            reflow.advance();
        }
    }

    TEST(MeshAssemblyRealizationTest, NothingToRealizeMakesNoRealization)
    {
        EXPECT_EQ(MeshAssemblyRealization::create(nullptr), nullptr);
        EXPECT_EQ(MeshAssemblyRealization::create(std::make_shared<MeshAssemblyContainer>()), nullptr);
    }

    TEST(MeshAssemblyRealizationTest, WithNoPartitionEachKindGetsOneTargetOfItsKind)
    {
        const RealizedLevel level;

        const auto realized = level.realize();

        ASSERT_NE(realized, nullptr);
        ASSERT_EQ(realized->parts().size(), 3U);

        const auto& meshes = realized->parts()[0];
        EXPECT_EQ(meshes.kind, MeshAssemblyRowKind::Mesh);
        EXPECT_EQ(meshes.rows, (std::vector<uint64_t> { 1, 2, 3 }));
        EXPECT_FALSE(meshes.instanced);
        EXPECT_EQ(meshes.placement, MeshPartPlacement::Flat);
        ASSERT_NE(meshes.network, nullptr);
        EXPECT_EQ(meshes.network->slot_count(), 3U);
        EXPECT_NE(meshes.network_buffer, nullptr);
        EXPECT_EQ(meshes.instances, nullptr);

        const auto& points = realized->parts()[1];
        EXPECT_EQ(points.kind, MeshAssemblyRowKind::Points);
        ASSERT_EQ(points.spatial_buffers.size(), 1U);
        ASSERT_NE(points.spatial_buffers[0], nullptr);
        EXPECT_EQ(points.spatial_buffers[0]->topology(), PrimitiveTopology::POINT_LIST);
        EXPECT_EQ(points.spatial_buffers[0]->capacity(), 3U);

        const auto& curves = realized->parts()[2];
        EXPECT_EQ(curves.kind, MeshAssemblyRowKind::Curves);
        ASSERT_EQ(curves.spatial_buffers.size(), 1U);
        ASSERT_NE(curves.spatial_buffers[0], nullptr);
        EXPECT_EQ(curves.spatial_buffers[0]->topology(), PrimitiveTopology::LINE_STRIP);
    }

    TEST(MeshAssemblyRealizationTest, ASourceShownManyTimesCanBeAnInstancedPartAndTheRestAreNetworks)
    {
        const RealizedLevel level;
        MeshAssemblyRealizeOptions options;
        options.partition = Partitions::instanced(2);

        const auto realized = level.realize(options);

        ASSERT_NE(realized, nullptr);
        ASSERT_EQ(realized->parts().size(), 4U);
        EXPECT_NE(realized->parts()[0].network, nullptr);
        EXPECT_EQ(realized->parts()[0].rows, (std::vector<uint64_t> { 1 }));

        const auto& wheels = realized->parts()[1];
        EXPECT_TRUE(wheels.instanced);
        EXPECT_EQ(wheels.network, nullptr);
        ASSERT_NE(wheels.instances, nullptr);
        EXPECT_EQ(wheels.instances->slot_count(), 2U);
        EXPECT_NE(wheels.instance_buffer, nullptr);
        EXPECT_EQ(wheels.rows, (std::vector<uint64_t> { 2, 3 }));
    }

    TEST(MeshAssemblyRealizationTest, AnInstancedPartIsSplitBySourceAndMaterialWhenAPartitionMixesThem)
    {
        const RealizedLevel level;
        MeshAssemblyRealizeOptions options;
        options.partition = [](const Kakshya::MeshAssemblyPartContext&, size_t) {
            return Kakshya::MeshAssemblyPartKey { .key = 1, .instanced = true };
        };

        const auto realized = level.realize(options);

        ASSERT_NE(realized, nullptr);
        size_t instanced = 0;
        for (const auto& part : realized->parts()) {
            if (part.instances) {
                ++instanced;
            }
        }
        EXPECT_EQ(instanced, 2U);
    }

    TEST(MeshAssemblyRealizationTest, EverySourceCanHaveATargetOfItsOwn)
    {
        const RealizedLevel level;
        MeshAssemblyRealizeOptions options;
        options.partition = Partitions::by_source();

        const auto realized = level.realize(options);

        ASSERT_NE(realized, nullptr);
        EXPECT_EQ(realized->parts().size(), 4U);
    }

    TEST(MeshAssemblyRealizationTest, APartMayKeepTheHierarchyWhenAskedTo)
    {
        const RealizedLevel level;
        MeshAssemblyRealizeOptions options;
        options.placement = [](const Kakshya::MeshAssemblyPart&) { return MeshPartPlacement::Chained; };

        const auto realized = level.realize(options);

        ASSERT_NE(realized, nullptr);
        const auto& meshes = realized->parts()[0];
        EXPECT_EQ(meshes.placement, MeshPartPlacement::Chained);
        EXPECT_EQ(meshes.rows, (std::vector<uint64_t> { 0, 1, 2, 3 }));
        EXPECT_EQ(meshes.network->get_slot(0).node, nullptr);
    }

    TEST(MeshAssemblyRealizationTest, ASourceOfPointsWithNothingInItGetsNoBuffer)
    {
        auto assembly = std::make_shared<MeshAssemblyContainer>();
        const auto mesh = assembly->add_source({ .geometry = level_triangle(), .name = "mesh" });
        const auto empty = assembly->add_source({ .geometry = level_points(0), .name = "empty" });
        std::ignore = assembly->add_placement({ .name = "mesh", .source = mesh });
        std::ignore = assembly->add_placement({ .name = "empty", .source = empty });

        const auto realized = MeshAssemblyRealization::create(assembly);

        ASSERT_NE(realized, nullptr);
        ASSERT_EQ(realized->parts().size(), 1U);
        EXPECT_EQ(realized->parts()[0].kind, MeshAssemblyRowKind::Mesh);
    }

    TEST(MeshAssemblyRealizationTest, ABufferForFramesIsSizedForTheLargestTheSourceWasToldOf)
    {
        const RealizedLevel level;
        ASSERT_TRUE(level.assembly->set_spatial_frames(
            level.dust, level_frames(std::vector<SpatialData> { level_points(2), level_points(10) }), 10U));

        const auto realized = level.realize();

        ASSERT_NE(realized, nullptr);
        EXPECT_EQ(realized->parts()[1].spatial_buffers[0]->capacity(), 10U);
    }

    TEST(MeshAssemblyRealizationTest, OneMotionPlacesEveryPartThroughOneSink)
    {
        const RealizedLevel level;
        const auto realized = level.realize();
        ASSERT_NE(realized, nullptr);
        const auto frames = Kakshya::MeshAssemblyMotionFrames::create(level.assembly, 0);
        ASSERT_NE(frames, nullptr);
        auto reflow = Kriya::MotionReflowBuilder::sampling(frames, realized->placement_sink(), k_level_rate).build();
        ASSERT_NE(reflow, nullptr);

        level_run(*reflow, 5);

        const auto& network = realized->parts()[0].network;
        EXPECT_NEAR(network->get_slot(0).local_transform[3].x, 8.0F, 1e-4F);
        EXPECT_NEAR(network->get_slot(1).local_transform[3].x, 9.0F, 1e-4F);
        EXPECT_NEAR(network->get_slot(2).local_transform[3].x, 7.0F, 1e-4F);
    }

    TEST(MeshAssemblyRealizationTest, InstancesAreMovedByTheSameMotion)
    {
        const RealizedLevel level;
        MeshAssemblyRealizeOptions options;
        options.partition = Partitions::instanced(2);
        const auto realized = level.realize(options);
        ASSERT_NE(realized, nullptr);
        const auto frames = Kakshya::MeshAssemblyMotionFrames::create(level.assembly, 0);
        auto reflow = Kriya::MotionReflowBuilder::sampling(frames, realized->placement_sink(), k_level_rate).build();
        ASSERT_NE(reflow, nullptr);

        level_run(*reflow, 5);

        const auto& instances = realized->parts()[1].instances;
        EXPECT_NEAR(instances->get_slot(0).transform[3].x, 9.0F, 1e-4F);
        EXPECT_NEAR(instances->get_slot(1).transform[3].x, 7.0F, 1e-4F);
    }

    TEST(MeshAssemblyRealizationTest, APartCanBePlacedAloneAndAPartThatDoesNotExistHasNoSink)
    {
        const RealizedLevel level;
        const auto realized = level.realize();
        ASSERT_NE(realized, nullptr);
        const auto frames = Kakshya::MeshAssemblyMotionFrames::create(level.assembly, 0);
        auto reflow = Kriya::MotionReflowBuilder::sampling(frames, realized->placement_sink(0), k_level_rate).build();
        ASSERT_NE(reflow, nullptr);

        level_run(*reflow, 5);

        EXPECT_NEAR(realized->parts()[0].network->get_slot(1).local_transform[3].x, 9.0F, 1e-4F);
        EXPECT_FALSE(static_cast<bool>(realized->placement_sink(99)));
    }

    TEST(MeshAssemblyRealizationTest, PointsAndCurvesAreGivenTheirPlacementWithoutBeingDrawnYet)
    {
        const RealizedLevel level;
        const auto realized = level.realize();
        ASSERT_NE(realized, nullptr);
        const auto frames = Kakshya::MeshAssemblyMotionFrames::create(level.assembly, 0);
        auto reflow = Kriya::MotionReflowBuilder::sampling(frames, realized->placement_sink(), k_level_rate).build();

        ASSERT_NE(reflow, nullptr);
        level_run(*reflow, 3);

        EXPECT_EQ(reflow->failures(), 0U);
    }

    TEST(MeshAssemblyRealizationTest, ASourceWithNoFramesGetsNoLane)
    {
        const RealizedLevel level;
        const auto realized = level.realize();
        ASSERT_NE(realized, nullptr);

        EXPECT_TRUE(realized->mesh_routes().empty());
        EXPECT_TRUE(realized->spatial_routes().empty());
    }

    TEST(MeshAssemblyRealizationTest, ASourceWithFramesGetsALaneOverItsOwnTimes)
    {
        const RealizedLevel level;
        ASSERT_TRUE(level.assembly->set_mesh_frames(level.wall, level_frames(std::vector<MeshData> { level_triangle(), level_quad() })));
        const auto realized = level.realize();
        ASSERT_NE(realized, nullptr);

        const auto routes = realized->mesh_routes(CourseEdge::PingPong);

        ASSERT_EQ(routes.size(), 1U);
        EXPECT_NE(routes[0].source, nullptr);
        EXPECT_TRUE(static_cast<bool>(routes[0].sink));
        EXPECT_DOUBLE_EQ(routes[0].course.in, 0.0);
        EXPECT_DOUBLE_EQ(routes[0].course.out, 0.25);
        EXPECT_EQ(routes[0].course.edge, CourseEdge::PingPong);
    }

    TEST(MeshAssemblyRealizationTest, ASourceOfOneFrameHoldsInsteadOfLooping)
    {
        const RealizedLevel level;
        ASSERT_TRUE(level.assembly->set_mesh_frames(level.wall, level_frames(std::vector<MeshData> { level_quad() })));
        const auto realized = level.realize();
        ASSERT_NE(realized, nullptr);

        const auto routes = realized->mesh_routes();

        ASSERT_EQ(routes.size(), 1U);
        EXPECT_EQ(routes[0].course.edge, CourseEdge::Hold);
        EXPECT_GT(routes[0].course.out, routes[0].course.in);
    }

    TEST(MeshAssemblyRealizationTest, ALaneWritesItsFramesIntoTheSlotsThatShowItsSource)
    {
        const RealizedLevel level;
        ASSERT_TRUE(level.assembly->set_mesh_frames(level.wall, level_frames(std::vector<MeshData> { level_triangle(), level_quad() })));
        const auto realized = level.realize();
        ASSERT_NE(realized, nullptr);
        const auto routes = realized->mesh_routes();
        ASSERT_EQ(routes.size(), 1U);
        auto reflow = Kriya::MeshReflowBuilder(nullptr, {}, k_level_rate).route(routes[0]).build();
        ASSERT_NE(reflow, nullptr);
        const auto& network = realized->parts()[0].network;
        ASSERT_EQ(network->get_slot(0).node->get_mesh_vertex_count(), 3U);

        reflow->advance();
        reflow->advance();

        EXPECT_EQ(network->get_slot(0).node->get_mesh_vertex_count(), 4U);
        EXPECT_EQ(network->get_slot(1).node->get_mesh_vertex_count(), 3U);
    }

    TEST(MeshAssemblyRealizationTest, OneLaneFeedsEveryRowThatShowsTheSourceAndOnlyThose)
    {
        const RealizedLevel level;
        ASSERT_TRUE(level.assembly->set_mesh_frames(level.wheel, level_frames(std::vector<MeshData> { level_triangle(), level_quad() })));
        const auto realized = level.realize();
        ASSERT_NE(realized, nullptr);
        const auto routes = realized->mesh_routes();
        ASSERT_EQ(routes.size(), 1U);
        auto reflow = Kriya::MeshReflowBuilder(nullptr, {}, k_level_rate).route(routes[0]).build();
        ASSERT_NE(reflow, nullptr);

        reflow->advance();
        reflow->advance();

        const auto& network = realized->parts()[0].network;
        EXPECT_EQ(network->get_slot(0).node->get_mesh_vertex_count(), 3U);
        EXPECT_EQ(network->get_slot(1).node->get_mesh_vertex_count(), 4U);
        EXPECT_EQ(network->get_slot(2).node->get_mesh_vertex_count(), 4U);
    }

    TEST(MeshAssemblyRealizationTest, AnInstancedSourceIsWrittenOnceIntoItsTemplate)
    {
        const RealizedLevel level;
        ASSERT_TRUE(level.assembly->set_mesh_frames(level.wheel, level_frames(std::vector<MeshData> { level_triangle(), level_quad() })));
        MeshAssemblyRealizeOptions options;
        options.partition = Partitions::instanced(2);
        const auto realized = level.realize(options);
        ASSERT_NE(realized, nullptr);
        const auto routes = realized->mesh_routes();
        ASSERT_EQ(routes.size(), 1U);
        auto reflow = Kriya::MeshReflowBuilder(nullptr, {}, k_level_rate).route(routes[0]).build();
        ASSERT_NE(reflow, nullptr);

        reflow->advance();
        reflow->advance();

        const auto node = std::dynamic_pointer_cast<Nodes::GpuSync::MeshWriterNode>(realized->parts()[1].instances->get_slot(0).node);
        ASSERT_NE(node, nullptr);
        EXPECT_EQ(node->get_mesh_vertex_count(), 4U);
    }

    TEST(MeshAssemblyRealizationTest, APointSourceWithFramesGetsALaneIntoTheBufferOfItsRow)
    {
        const RealizedLevel level;
        ASSERT_TRUE(level.assembly->set_spatial_frames(
            level.dust, level_frames(std::vector<SpatialData> { level_points(2), level_points(5) }), 5U));
        const auto realized = level.realize();
        ASSERT_NE(realized, nullptr);
        const auto routes = realized->spatial_routes();
        ASSERT_EQ(routes.size(), 1U);
        auto reflow = Kriya::SpatialReflowBuilder(nullptr, {}, k_level_rate).route(routes[0]).build();
        ASSERT_NE(reflow, nullptr);
        const auto& buffer = realized->parts()[1].spatial_buffers[0];

        reflow->advance();
        reflow->advance();

        ASSERT_NE(buffer->current_frame(), nullptr);
        EXPECT_EQ(buffer->current_frame()->positions.size(), 5U);
    }

    TEST(MeshAssemblyRealizationTest, ShowingTheRestWritesTheFirstShotOfEveryPointAndCurveSource)
    {
        const RealizedLevel level;
        const auto realized = level.realize();
        ASSERT_NE(realized, nullptr);
        EXPECT_EQ(realized->parts()[1].spatial_buffers[0]->current_frame(), nullptr);

        realized->show_rest();

        ASSERT_NE(realized->parts()[1].spatial_buffers[0]->current_frame(), nullptr);
        EXPECT_EQ(realized->parts()[1].spatial_buffers[0]->current_frame()->positions.size(), 3U);
        ASSERT_NE(realized->parts()[2].spatial_buffers[0]->current_frame(), nullptr);
        EXPECT_EQ(realized->parts()[2].spatial_buffers[0]->current_frame()->vertex_counts_per_curve, (std::vector<int32_t> { 3 }));
    }

    TEST(MeshAssemblyRealizationTest, RowsOfOneSourceShareTheShotShowRestWrites)
    {
        auto assembly = std::make_shared<MeshAssemblyContainer>();
        const auto dust = assembly->add_source({ .geometry = level_points(3), .name = "dust" });
        std::ignore = assembly->add_placement({ .name = "first", .source = dust });
        std::ignore = assembly->add_placement({ .name = "second", .source = dust, .local = level_shift(4.0F) });
        const auto realized = MeshAssemblyRealization::create(assembly);
        ASSERT_NE(realized, nullptr);
        ASSERT_EQ(realized->parts().size(), 1U);

        realized->show_rest();

        const auto& buffers = realized->parts()[0].spatial_buffers;
        ASSERT_EQ(buffers.size(), 2U);
        ASSERT_NE(buffers[0]->current_frame(), nullptr);
        EXPECT_EQ(buffers[0]->current_frame(), buffers[1]->current_frame());
    }

    TEST(MeshAssemblyRealizationTest, ABufferOfPointsOrCurvesAsksForItsOwnOperatorByPartAndRow)
    {
        const RealizedLevel level;
        std::vector<std::pair<MeshAssemblyRowKind, uint64_t>> asked;
        MeshAssemblyRealizeOptions options;
        options.spatial_field = [&asked](const Kakshya::MeshAssemblyPart& part, uint64_t row) {
            asked.emplace_back(part.kind, row);
            return std::shared_ptr<Nodes::Network::GpuFieldOperator> {};
        };

        const auto realized = level.realize(options);

        ASSERT_NE(realized, nullptr);
        ASSERT_EQ(asked.size(), 2U);
        EXPECT_EQ(asked[0], (std::pair<MeshAssemblyRowKind, uint64_t> { MeshAssemblyRowKind::Points, 4 }));
        EXPECT_EQ(asked[1], (std::pair<MeshAssemblyRowKind, uint64_t> { MeshAssemblyRowKind::Curves, 5 }));
    }

    TEST(MeshAssemblyRealizationTest, BoundsMayBeGivenOrLeftToTheGeometry)
    {
        const RealizedLevel level;
        MeshAssemblyRealizeOptions given;
        given.bounds = Kinesis::SamplerBounds { .min = glm::vec3(-9.0F), .max = glm::vec3(9.0F) };

        EXPECT_NE(level.realize(given), nullptr);
        EXPECT_NE(level.realize(), nullptr);
    }

    /** A wall of stone and two wheels of rubber under a joint. */
    struct MaterialLevel {
        std::shared_ptr<MeshAssemblyContainer> assembly = std::make_shared<MeshAssemblyContainer>();

        MaterialLevel()
        {
            const auto wall = assembly->add_source({ .geometry = level_triangle(), .name = "wall" });
            const auto wheel = assembly->add_source({ .geometry = level_triangle(), .name = "wheel" });
            const auto stone = assembly->add_material({ .name = "stone", .diffuse_path = "stone.png" });
            const auto rubber = assembly->add_material({ .name = "rubber", .diffuse_path = "*0", .diffuse_embedded = true });
            const auto level = assembly->add_placement({ .name = "level" }).value();
            std::ignore = assembly->add_placement({ .name = "wall", .parent = level, .source = wall, .material = stone });
            std::ignore = assembly->add_placement({ .name = "left", .parent = level, .source = wheel, .material = rubber, .local = level_shift(1.0F) });
            std::ignore = assembly->add_placement({ .name = "right", .parent = level, .source = wheel, .material = rubber, .local = level_shift(-1.0F) });
        }
    };

    TEST(MeshAssemblyRealizationTest, EveryMaterialAPartUsesIsResolvedOnceWithTheMaterialAsTheFileWroteIt)
    {
        const MaterialLevel level;
        const auto realized = MeshAssemblyRealization::create(level.assembly);
        ASSERT_NE(realized, nullptr);
        std::vector<std::string> asked;
        std::vector<bool> embedded;

        const auto bound = realized->bind_materials([&](const Kakshya::MeshAssemblyMaterial& material) {
            asked.push_back(material.name);
            embedded.push_back(material.diffuse_embedded);
            return std::shared_ptr<Core::VKImage> {};
        });

        EXPECT_EQ(bound, 0U);
        ASSERT_EQ(asked.size(), 2U);
        EXPECT_EQ(asked[0], "stone");
        EXPECT_EQ(asked[1], "rubber");
        EXPECT_FALSE(embedded[0]);
        EXPECT_TRUE(embedded[1]);
    }

    TEST(MeshAssemblyRealizationTest, AnInstancedPartResolvesTheOneMaterialItsGroupShares)
    {
        const MaterialLevel level;
        MeshAssemblyRealizeOptions options;
        options.partition = Partitions::instanced(2);
        const auto realized = MeshAssemblyRealization::create(level.assembly, options);
        ASSERT_NE(realized, nullptr);
        int asked = 0;

        std::ignore = realized->bind_materials([&asked](const Kakshya::MeshAssemblyMaterial&) {
            ++asked;
            return std::shared_ptr<Core::VKImage> {};
        });

        EXPECT_EQ(asked, 2);
    }

    TEST(MeshAssemblyRealizationTest, NoResolverBindsNothing)
    {
        const MaterialLevel level;
        const auto realized = MeshAssemblyRealization::create(level.assembly);
        ASSERT_NE(realized, nullptr);

        EXPECT_EQ(realized->bind_materials({}), 0U);
    }

    TEST(MeshAssemblyRealizationTest, RowsWithNoMaterialAreNeverAsked)
    {
        const RealizedLevel level;
        const auto realized = level.realize();
        ASSERT_NE(realized, nullptr);
        int asked = 0;

        std::ignore = realized->bind_materials([&asked](const Kakshya::MeshAssemblyMaterial&) {
            ++asked;
            return std::shared_ptr<Core::VKImage> {};
        });

        EXPECT_EQ(asked, 0);
    }

} // namespace
} // namespace MayaFlux::Test
