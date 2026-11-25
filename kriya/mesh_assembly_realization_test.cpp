#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Buffers/Network/InstanceNetworkBuffer.hpp"
#include "MayaFlux/Buffers/Network/MeshNetworkBuffer.hpp"
#include "MayaFlux/Buffers/Network/RecordedGeometryBuffer.hpp"
#include "MayaFlux/Core/Backends/Graphics/Vulkan/VKImage.hpp"
#include "MayaFlux/Nodes/Network/Operators/GpuFieldOperator.hpp"
#include "MayaFlux/Kriya/Realization/MeshAssemblyRealization.hpp"
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
        EXPECT_EQ(instanced, 3U);
    }

    TEST(MeshAssemblyRealizationTest, APointSetShownInSeveralPlacesIsOneInstancedPartThatAMotionPlaces)
    {
        auto assembly = std::make_shared<MeshAssemblyContainer>();
        const auto dust = assembly->add_source({ .geometry = level_points(3), .name = "dust" });
        const auto joint = assembly->add_placement({ .name = "joint", .local = level_shift(10.0F) }).value();
        std::ignore = assembly->add_placement({ .name = "left", .parent = joint, .source = dust, .local = level_shift(1.0F) });
        std::ignore = assembly->add_placement({ .name = "right", .parent = joint, .source = dust, .local = level_shift(-1.0F) });
        const auto times = *Kinesis::TimeSampling::acyclic({ 0.0, 2.0 });
        auto slide = Kinesis::TimedValues<glm::vec3>::create({ .times = times,
            .values = { glm::vec3(10.0F, 0.0F, 0.0F), glm::vec3(2.0F, 0.0F, 0.0F) } });
        std::ignore = assembly->add_motion({ .name = "slide", .channels = { { .row = joint, .translation = slide } } });
        MeshAssemblyRealizeOptions options;
        options.partition = Partitions::instanced(2);

        const auto realized = MeshAssemblyRealization::create(assembly, options);

        ASSERT_NE(realized, nullptr);
        ASSERT_EQ(realized->parts().size(), 1U);
        const auto& cloud = realized->parts()[0];
        EXPECT_EQ(cloud.kind, MeshAssemblyRowKind::Points);
        EXPECT_TRUE(cloud.instanced);
        ASSERT_NE(cloud.instances, nullptr);
        EXPECT_EQ(cloud.instances->slot_count(), 2U);
        EXPECT_NE(cloud.instance_buffer, nullptr);
        EXPECT_TRUE(cloud.spatial_buffers.empty());

        const auto frames = Kakshya::MeshAssemblyMotionFrames::create(assembly, 0);
        auto reflow = Kriya::MotionReflowBuilder::sampling(frames, realized->placement_sink(), k_level_rate).build();
        ASSERT_NE(reflow, nullptr);
        level_run(*reflow, 5);
        EXPECT_NEAR(cloud.instances->get_slot(0).transform[3].x, 9.0F, 1e-4F);
        EXPECT_NEAR(cloud.instances->get_slot(1).transform[3].x, 7.0F, 1e-4F);
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

    TEST(MeshAssemblyRealizationTest, AnAssemblyWithNoMotionHasOneFrameAtEveryTime)
    {
        const RealizedLevel level;

        const auto frames = Kakshya::MeshAssemblyRestFrames::create(level.assembly);

        ASSERT_NE(frames, nullptr);
        EXPECT_EQ(frames->extent(), (std::pair<double, double> { 0.0, 1.0 }));
        const auto frame = frames->sample(0.3);
        ASSERT_NE(frame, nullptr);
        EXPECT_EQ(frame, frames->sample(0.9));
        ASSERT_EQ(frame->world.size(), level.assembly->get_total_elements());
        EXPECT_FLOAT_EQ(frame->world[2][3].x, 11.0F);
        EXPECT_EQ(Kakshya::MeshAssemblyRestFrames::create(nullptr), nullptr);
        EXPECT_EQ(Kakshya::MeshAssemblyRestFrames::create(std::make_shared<MeshAssemblyContainer>()), nullptr);
    }

    TEST(MeshAssemblyRealizationTest, AnEditOfTheLocalPlacementsMovesEveryPartThroughTheHierarchy)
    {
        const RealizedLevel level;
        const auto realized = level.realize();
        ASSERT_NE(realized, nullptr);
        const auto rest = Kakshya::MeshAssemblyRestFrames::create(level.assembly);
        auto reflow = Kriya::MotionReflowBuilder::sampling(rest, realized->placement_sink(), k_level_rate)
                          .operation(Kriya::placement_edit(level.assembly, [](std::span<glm::mat4> locals, const Kriya::FrameContext&) {
                              locals[0] = level_shift(20.0F);
                          }))
                          .build();
        ASSERT_NE(reflow, nullptr);

        level_run(*reflow, 2);

        const auto& network = realized->parts()[0].network;
        EXPECT_NEAR(network->get_slot(0).local_transform[3].x, 20.0F, 1e-4F);
        EXPECT_NEAR(network->get_slot(1).local_transform[3].x, 21.0F, 1e-4F);
        EXPECT_NEAR(network->get_slot(2).local_transform[3].x, 19.0F, 1e-4F);
    }

    TEST(MeshAssemblyRealizationTest, AnEditMayReadTheReflowsTimeAndAnAdditiveLayerSitsOnAMotion)
    {
        const RealizedLevel level;
        const auto realized = level.realize();
        ASSERT_NE(realized, nullptr);
        const auto frames = Kakshya::MeshAssemblyMotionFrames::create(level.assembly, 0);
        auto reflow = Kriya::MotionReflowBuilder::sampling(frames, realized->placement_sink(), k_level_rate)
                          .operation(Kriya::placement_edit(level.assembly, [](std::span<glm::mat4> locals, const Kriya::FrameContext& context) {
                              locals[2][3].y += static_cast<float>(context.time) * 8.0F;
                          }))
                          .build();
        ASSERT_NE(reflow, nullptr);

        level_run(*reflow, 5);

        const auto& network = realized->parts()[0].network;
        EXPECT_NEAR(network->get_slot(1).local_transform[3].x, 9.0F, 1e-4F);
        EXPECT_NEAR(network->get_slot(1).local_transform[3].y, 4.0F, 1e-4F);
        EXPECT_NEAR(network->get_slot(2).local_transform[3].y, 0.0F, 1e-4F);
    }

    TEST(MeshAssemblyRealizationTest, AnEditWithNoCallableOrNoAssemblyLeavesTheFrameAsItIs)
    {
        const RealizedLevel level;
        const auto rest = Kakshya::MeshAssemblyRestFrames::create(level.assembly);
        const auto frame = rest->sample(0.0);

        const auto none = Kriya::placement_edit(level.assembly, {});
        const auto lost = Kriya::placement_edit(nullptr, [](std::span<glm::mat4>, const Kriya::FrameContext&) { });

        EXPECT_EQ(none.transform_fn()(Kriya::FrameContext {}, frame), frame);
        EXPECT_EQ(lost.transform_fn()(Kriya::FrameContext {}, frame), frame);
    }

    /** The level's slide played to one second, where the joint and the wall stand at x = 6. */
    struct SlidLevel {
        RealizedLevel level;
        std::shared_ptr<const Kakshya::MeshAssemblyMotionFrames> frames;
        std::shared_ptr<Kriya::Course> course;
        std::shared_ptr<const Kakshya::MeshAssemblyLocals> frame;

        explicit SlidLevel(CourseEdge edge = CourseEdge::Once)
            : frames(Kakshya::MeshAssemblyMotionFrames::create(level.assembly, 0))
        {
            Kriya::CourseSetting setting;
            setting.in = 0.0;
            setting.out = 2.0;
            setting.edge = edge;
            course = Kriya::Course::create(k_level_rate, setting);
            for (int i = 0; i < 8; ++i) {
                std::ignore = course->advance();
            }
            frame = frames->sample(course->time());
        }

        [[nodiscard]] Kriya::FrameContext context() const
        {
            Kriya::FrameContext made;
            made.course = course.get();
            return made;
        }

        /** Seconds for the rows standing at x = 6, nothing for the rest. */
        static Kinesis::SpatialField at_six(float seconds)
        {
            return { .fn = [seconds](const glm::vec3& p) { return std::abs(p.x - 6.0F) < 0.1F ? seconds : 0.0F; } };
        }
    };

    TEST(PlacementTimeOffsetTest, ARowIsTakenFromItsOwnTimeAndItsChildrenFollow)
    {
        const SlidLevel slid;
        ASSERT_NEAR(slid.frame->world[0][3].x, 6.0F, 1e-4F);

        const auto behind = Kriya::placement_time_offset(slid.level.assembly, slid.frames, SlidLevel::at_six(-0.5F))
                                .transform_fn()(slid.context(), slid.frame);
        const auto ahead = Kriya::placement_time_offset(slid.level.assembly, slid.frames, SlidLevel::at_six(0.5F))
                               .transform_fn()(slid.context(), slid.frame);

        ASSERT_NE(behind, nullptr);
        ASSERT_NE(ahead, nullptr);
        EXPECT_NEAR(behind->matrices[0][3].x, 8.0F, 1e-4F);
        EXPECT_NEAR(behind->world[2][3].x, 9.0F, 1e-4F);
        EXPECT_NEAR(behind->world[3][3].x, 7.0F, 1e-4F);
        EXPECT_NEAR(behind->world[4][3].x, 0.0F, 1e-4F);
        EXPECT_NEAR(ahead->matrices[0][3].x, 4.0F, 1e-4F);
        EXPECT_NEAR(ahead->world[2][3].x, 5.0F, 1e-4F);
        EXPECT_NEAR(slid.course->time(), 1.0, 1e-9);
    }

    TEST(PlacementTimeOffsetTest, TheCoursesEdgeAppliesToAnOffsetTime)
    {
        const SlidLevel slid(CourseEdge::Loop);

        const auto wrapped = Kriya::placement_time_offset(slid.level.assembly, slid.frames, SlidLevel::at_six(1.5F))
                                 .transform_fn()(slid.context(), slid.frame);

        ASSERT_NE(wrapped, nullptr);
        EXPECT_NEAR(wrapped->matrices[0][3].x, 8.0F, 1e-4F);
    }

    TEST(PlacementTimeOffsetTest, AFrameNoRowIsOffsetInIsHandedOnUnchanged)
    {
        const SlidLevel slid;
        const auto still = Kriya::placement_time_offset(slid.level.assembly, slid.frames, SlidLevel::at_six(0.0F));

        EXPECT_EQ(still.transform_fn()(slid.context(), slid.frame), slid.frame);
    }

    TEST(PlacementTimeOffsetTest, WithoutOneCourseOrItsArgumentsTheFrameIsUnchanged)
    {
        const SlidLevel slid;
        const auto field = SlidLevel::at_six(-0.5F);

        const auto mixed = Kriya::placement_time_offset(slid.level.assembly, slid.frames, field);
        const auto no_assembly = Kriya::placement_time_offset(nullptr, slid.frames, field);
        const auto no_source = Kriya::placement_time_offset(slid.level.assembly, nullptr, field);
        const auto no_field = Kriya::placement_time_offset(slid.level.assembly, slid.frames, {});

        EXPECT_EQ(mixed.transform_fn()(Kriya::FrameContext {}, slid.frame), slid.frame);
        EXPECT_EQ(no_assembly.transform_fn()(slid.context(), slid.frame), slid.frame);
        EXPECT_EQ(no_source.transform_fn()(slid.context(), slid.frame), slid.frame);
        EXPECT_EQ(no_field.transform_fn()(slid.context(), slid.frame), slid.frame);
    }

    /** Two wheels of rubber that a reader gave a colour, and one of stone it did not. */
    struct SurfaceLevel {
        std::shared_ptr<MeshAssemblyContainer> assembly = std::make_shared<MeshAssemblyContainer>();

        SurfaceLevel()
        {
            const auto wheel = assembly->add_source({ .geometry = level_triangle(), .name = "wheel" });
            const auto rock = assembly->add_source({ .geometry = level_triangle(), .name = "rock" });
            Facets colours;
            colours.set(Kakshya::MeshAssemblySurface { .base_color = glm::vec4(1.0F, 0.5F, 0.25F, 1.0F), .metallic = 0.3F, .roughness = 0.6F });
            const auto rubber = assembly->add_material({ .name = "rubber", .facets = colours });
            const auto stone = assembly->add_material({ .name = "stone" });
            std::ignore = assembly->add_placement({ .name = "left", .source = wheel, .material = rubber });
            std::ignore = assembly->add_placement({ .name = "right", .source = wheel, .material = rubber, .local = level_shift(2.0F) });
            std::ignore = assembly->add_placement({ .name = "rock", .source = rock, .material = stone });
        }
    };

    TEST(MeshAssemblyRealizationTest, InstancesAreTintedWithTheColourOfTheirMaterial)
    {
        const SurfaceLevel level;
        MeshAssemblyRealizeOptions options;
        options.partition = Partitions::instanced(2);
        const auto realized = MeshAssemblyRealization::create(level.assembly, options);
        ASSERT_NE(realized, nullptr);

        EXPECT_EQ(realized->apply_surfaces(), 2U);

        const auto& wheels = realized->parts()[0];
        ASSERT_NE(wheels.instances, nullptr);
        for (const auto& slot : wheels.instances->slots()) {
            EXPECT_EQ(slot.tint, glm::vec3(1.0F, 0.5F, 0.25F));
            EXPECT_TRUE(slot.dirty);
        }
    }

    TEST(MeshAssemblyRealizationTest, ANetworkAndAMaterialWithNoColourAreLeftAlone)
    {
        const SurfaceLevel level;

        const auto flat = MeshAssemblyRealization::create(level.assembly);
        ASSERT_NE(flat, nullptr);
        EXPECT_EQ(flat->apply_surfaces(), 0U);

        MeshAssemblyRealizeOptions options;
        options.partition = Partitions::instanced(1);
        const auto many = MeshAssemblyRealization::create(level.assembly, options);
        ASSERT_NE(many, nullptr);
        EXPECT_EQ(many->apply_surfaces(), 2U);
    }

    TEST(MeshAssemblyRealizationTest, ASkinnedMeshKeepsItsSkinOnItsSlotOfTheNetwork)
    {
        auto assembly = std::make_shared<MeshAssemblyContainer>();
        Kakshya::MeshSkin skin;
        skin.inverse_bind = { glm::mat4(1.0F) };
        skin.influence_joints.assign(3, glm::uvec4(0));
        skin.influence_weights.assign(3, glm::vec4(1.0F, 0.0F, 0.0F, 0.0F));
        Facets facets;
        facets.set(std::make_shared<const Kakshya::MeshSkin>(std::move(skin)));
        const auto skinned = assembly->add_source({ .geometry = level_triangle(), .name = "skinned", .facets = std::move(facets) });
        const auto plain = assembly->add_source({ .geometry = level_triangle(), .name = "plain" });
        const auto joint = assembly->add_placement({ .name = "joint" });
        ASSERT_TRUE(joint.has_value());
        const auto skeleton = assembly->add_skeleton({ *joint });
        ASSERT_TRUE(skeleton.has_value());
        const auto body = assembly->add_placement({ .name = "body", .parent = *joint, .source = skinned, .skeleton = *skeleton });
        const auto wall = assembly->add_placement({ .name = "wall", .source = plain });
        ASSERT_TRUE(body.has_value());
        ASSERT_TRUE(wall.has_value());

        const auto realized = MeshAssemblyRealization::create(assembly);

        ASSERT_NE(realized, nullptr);
        ASSERT_EQ(realized->parts().size(), 1U);
        const auto& part = realized->parts().front();
        ASSERT_EQ(part.rows.size(), 2U);
        ASSERT_EQ(part.rows[0], *body);
        EXPECT_EQ(part.network->get_slot(0).skin, assembly->skin(skinned));
        EXPECT_TRUE(part.network->get_slot(0).joints.empty());
        EXPECT_EQ(part.network->get_slot(1).skin, nullptr);
    }

    TEST(MeshAssemblyRealizationTest, ACameraRowMakesNoPartOfItsOwn)
    {
        auto assembly = std::make_shared<MeshAssemblyContainer>();
        const auto lens = assembly->add_source({ .geometry = Kakshya::MeshAssemblyLens {}, .name = "lens" });
        const auto mesh = assembly->add_source({ .geometry = level_triangle(), .name = "tri" });
        ASSERT_TRUE(assembly->add_placement({ .name = "camera", .source = lens }).has_value());
        ASSERT_TRUE(assembly->add_placement({ .name = "tri", .source = mesh }).has_value());

        const auto realized = MeshAssemblyRealization::create(assembly);

        ASSERT_NE(realized, nullptr);
        ASSERT_EQ(realized->parts().size(), 1U);
        EXPECT_EQ(realized->parts().front().kind, MeshAssemblyRowKind::Mesh);
    }

} // namespace
} // namespace MayaFlux::Test
