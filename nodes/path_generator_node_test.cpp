#include "gtest/gtest.h"

#include "MayaFlux/Nodes/Graphics/PathGeneratorNode.hpp"

namespace MayaFlux::Test {

namespace {

    using Kinesis::InterpolationMode;
    using Nodes::LineVertex;
    using Nodes::GpuSync::PathGeneratorNode;

    LineVertex make_vertex(const glm::vec3& position, float thickness = 2.0F, const glm::vec3& color = glm::vec3(1.0F))
    {
        LineVertex v;
        v.position = position;
        v.color = color;
        v.thickness = thickness;
        return v;
    }

    std::vector<LineVertex> collinear_points(uint32_t count)
    {
        std::vector<LineVertex> pts;
        pts.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            const auto f = static_cast<float>(i);
            pts.push_back(make_vertex({ f, 0.0F, 0.0F }, 1.0F + f, { 0.1F * (f + 1.0F), 0.0F, 0.0F }));
        }
        return pts;
    }

    std::vector<LineVertex> zigzag_points(uint32_t count)
    {
        std::vector<LineVertex> pts;
        pts.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            const auto f = static_cast<float>(i);
            pts.push_back(make_vertex({ f, (i % 2 == 0) ? 0.0F : 1.0F, 0.0F }));
        }
        return pts;
    }

    void add_in_order(PathGeneratorNode& node, const std::vector<LineVertex>& pts)
    {
        for (const auto& p : pts) {
            node.add_control_point(p);
        }
    }

    std::vector<LineVertex> uploaded_vertices(const PathGeneratorNode& node)
    {
        std::vector<LineVertex> out;
        out.reserve(node.get_vertex_count());
        for (uint32_t i = 0; i < node.get_vertex_count(); ++i) {
            out.push_back(node.get_vertex_typed<LineVertex>(i));
        }
        return out;
    }

    ::testing::AssertionResult near_vec(const glm::vec3& a, const glm::vec3& b, float tol = 1e-4F)
    {
        if (glm::length(a - b) <= tol) {
            return ::testing::AssertionSuccess();
        }
        return ::testing::AssertionFailure()
            << "(" << a.x << ", " << a.y << ", " << a.z << ") != ("
            << b.x << ", " << b.y << ", " << b.z << ")";
    }

    bool contains_in_order(const std::vector<LineVertex>& uploaded, const std::vector<glm::vec3>& points)
    {
        size_t next = 0;
        for (const auto& v : uploaded) {
            if (next < points.size() && glm::length(v.position - points[next]) < 1e-4F) {
                ++next;
            }
        }
        return next == points.size();
    }

    class PathGeneratorNodeTest : public ::testing::Test {
    protected:
        static PathGeneratorNode linear()
        {
            return PathGeneratorNode(InterpolationMode::LINEAR, 4, 16);
        }

        static PathGeneratorNode catmull(Eigen::Index samples = 4, size_t capacity = 16)
        {
            return PathGeneratorNode(InterpolationMode::CATMULL_ROM, samples, capacity);
        }

        static std::vector<LineVertex> rebuilt(InterpolationMode mode, Eigen::Index samples, const std::vector<LineVertex>& pts_in_order)
        {
            PathGeneratorNode fresh(mode, samples, 16);
            add_in_order(fresh, pts_in_order);
            fresh.compute_frame();
            return uploaded_vertices(fresh);
        }

        static void expect_same_geometry(const std::vector<LineVertex>& actual, const std::vector<LineVertex>& expected)
        {
            ASSERT_EQ(actual.size(), expected.size());
            for (size_t i = 0; i < actual.size(); ++i) {
                EXPECT_TRUE(near_vec(actual[i].position, expected[i].position)) << "vertex " << i;
            }
        }
    };

    TEST_F(PathGeneratorNodeTest, ConstructionStartsEmptyWithLineStripTopology)
    {
        PathGeneratorNode node;

        EXPECT_EQ(node.get_control_point_count(), 0U);
        EXPECT_EQ(node.get_vertex_count(), 0U);
        EXPECT_EQ(node.get_vertex_stride(), sizeof(LineVertex));
        EXPECT_EQ(node.get_primitive_topology(), Portal::Graphics::PrimitiveTopology::LINE_STRIP);
    }

    TEST_F(PathGeneratorNodeTest, ConstructionLayoutDescribesNoVerticesYet)
    {
        PathGeneratorNode node;
        auto layout = node.get_vertex_layout();

        ASSERT_TRUE(layout.has_value());
        EXPECT_EQ(layout->stride_bytes, sizeof(LineVertex));
        EXPECT_EQ(layout->vertex_count, 0U);
    }

    TEST_F(PathGeneratorNodeTest, ConstructionDefaults)
    {
        PathGeneratorNode node;

        EXPECT_EQ(node.get_control_point_capacity(), 64U);
        EXPECT_EQ(node.get_path_color(), glm::vec3(1.0F));
        EXPECT_FLOAT_EQ(node.get_path_thickness(), 2.0F);
        EXPECT_FALSE(node.should_force_uniform_color());
    }

    TEST_F(PathGeneratorNodeTest, CapacityIsTheRequestedControlPointLimit)
    {
        PathGeneratorNode node(InterpolationMode::LINEAR, 4, 7);
        EXPECT_EQ(node.get_control_point_capacity(), 7U);
    }

    TEST_F(PathGeneratorNodeTest, ControlPointsAreHeldNewestFirst)
    {
        auto node = linear();
        add_in_order(node, collinear_points(3));

        ASSERT_EQ(node.get_control_point_count(), 3U);
        EXPECT_TRUE(near_vec(node.get_control_point(0).position, { 2, 0, 0 }));
        EXPECT_TRUE(near_vec(node.get_control_point(1).position, { 1, 0, 0 }));
        EXPECT_TRUE(near_vec(node.get_control_point(2).position, { 0, 0, 0 }));
    }

    TEST_F(PathGeneratorNodeTest, GetControlPointsReturnsEveryPointInStoredOrder)
    {
        auto node = linear();
        add_in_order(node, collinear_points(3));

        auto all = node.get_control_points();
        ASSERT_EQ(all.size(), 3U);
        EXPECT_TRUE(near_vec(all[0].position, { 2, 0, 0 }));
        EXPECT_TRUE(near_vec(all[2].position, { 0, 0, 0 }));
    }

    TEST_F(PathGeneratorNodeTest, ExceedingCapacityDropsTheOldestPoints)
    {
        PathGeneratorNode node(InterpolationMode::LINEAR, 4, 3);
        add_in_order(node, collinear_points(5));

        ASSERT_EQ(node.get_control_point_count(), 3U);
        EXPECT_TRUE(near_vec(node.get_control_point(0).position, { 4, 0, 0 }));
        EXPECT_TRUE(near_vec(node.get_control_point(2).position, { 2, 0, 0 }));
    }

    TEST_F(PathGeneratorNodeTest, SetControlPointsMatchesAddingInOrder)
    {
        auto added = linear();
        add_in_order(added, collinear_points(4));

        auto set = linear();
        set.set_control_points(collinear_points(4));

        ASSERT_EQ(set.get_control_point_count(), 4U);
        for (size_t i = 0; i < 4; ++i) {
            EXPECT_TRUE(near_vec(set.get_control_point(i).position, added.get_control_point(i).position));
        }
    }

    TEST_F(PathGeneratorNodeTest, SetControlPointsBeyondCapacityKeepsTheNewest)
    {
        PathGeneratorNode node(InterpolationMode::LINEAR, 4, 3);
        node.set_control_points(collinear_points(6));

        ASSERT_EQ(node.get_control_point_count(), 3U);
        EXPECT_TRUE(near_vec(node.get_control_point(0).position, { 5, 0, 0 }));
        EXPECT_TRUE(near_vec(node.get_control_point(2).position, { 3, 0, 0 }));
    }

    TEST_F(PathGeneratorNodeTest, SetControlPointsReplacesWhatWasThere)
    {
        auto node = linear();
        add_in_order(node, collinear_points(5));
        node.set_control_points(collinear_points(2));

        EXPECT_EQ(node.get_control_point_count(), 2U);
    }

    TEST_F(PathGeneratorNodeTest, GetControlPointOutOfRangeReturnsAnOriginVertex)
    {
        auto node = linear();
        add_in_order(node, collinear_points(2));

        EXPECT_TRUE(near_vec(node.get_control_point(5).position, { 0, 0, 0 }));
    }

    TEST_F(PathGeneratorNodeTest, UpdateControlPointOutOfRangeChangesNothing)
    {
        auto node = linear();
        add_in_order(node, collinear_points(2));
        node.compute_frame();
        node.clear_gpu_update_flag();

        node.update_control_point(2, make_vertex({ 9, 9, 9 }));

        EXPECT_EQ(node.get_control_point_count(), 2U);
        EXPECT_FALSE(node.needs_gpu_update());
    }

    TEST_F(PathGeneratorNodeTest, LinearUploadsEveryControlPointInStoredOrder)
    {
        auto node = linear();
        add_in_order(node, collinear_points(3));
        node.compute_frame();

        ASSERT_EQ(node.get_vertex_count(), 3U);
        const auto up = uploaded_vertices(node);
        EXPECT_TRUE(near_vec(up[0].position, { 2, 0, 0 }));
        EXPECT_TRUE(near_vec(up[1].position, { 1, 0, 0 }));
        EXPECT_TRUE(near_vec(up[2].position, { 0, 0, 0 }));
    }

    TEST_F(PathGeneratorNodeTest, LinearKeepsPerPointColourAndThickness)
    {
        auto node = linear();
        add_in_order(node, collinear_points(3));
        node.compute_frame();

        const auto up = uploaded_vertices(node);
        EXPECT_TRUE(near_vec(up[0].color, { 0.3F, 0, 0 }));
        EXPECT_FLOAT_EQ(up[0].thickness, 3.0F);
        EXPECT_FLOAT_EQ(up[2].thickness, 1.0F);
    }

    TEST_F(PathGeneratorNodeTest, ComputeFrameUpdatesLayoutVertexCount)
    {
        auto node = linear();
        add_in_order(node, collinear_points(3));
        node.compute_frame();

        ASSERT_TRUE(node.get_vertex_layout().has_value());
        EXPECT_EQ(node.get_vertex_layout()->vertex_count, 3U);
    }

    TEST_F(PathGeneratorNodeTest, ComputeFrameRequestsGpuUpload)
    {
        auto node = linear();
        add_in_order(node, collinear_points(3));
        node.clear_gpu_update_flag();
        node.add_control_point(make_vertex({ 3, 0, 0 }));

        node.compute_frame();

        EXPECT_TRUE(node.needs_gpu_update());
    }

    TEST_F(PathGeneratorNodeTest, ComputeFrameMarksVertexDataAsNeedingUpdate)
    {
        auto node = linear();
        add_in_order(node, collinear_points(3));
        node.clear_gpu_update_flag();
        node.add_control_point(make_vertex({ 3, 0, 0 }));

        node.compute_frame();

        EXPECT_TRUE(node.needs_vertex_data_update());
    }

    TEST_F(PathGeneratorNodeTest, UnchangedPathIsNotReuploaded)
    {
        auto node = linear();
        add_in_order(node, collinear_points(3));
        node.compute_frame();
        node.clear_gpu_update_flag();

        node.compute_frame();

        EXPECT_FALSE(node.needs_gpu_update());
        EXPECT_EQ(node.get_vertex_count(), 3U);
    }

    TEST_F(PathGeneratorNodeTest, PointsAddedInteractivelyAppearInTheNextFrame)
    {
        auto node = linear();
        node.compute_frame();

        const auto pts = collinear_points(4);
        const std::array<uint32_t, 4> expected { 0, 2, 3, 4 };
        for (size_t i = 0; i < pts.size(); ++i) {
            node.add_control_point(pts[i]);
            node.compute_frame();
            EXPECT_EQ(node.get_vertex_count(), expected[i]) << "after point " << i;
        }
    }

    TEST_F(PathGeneratorNodeTest, ProcessSampleUploadsAFrameAndReturnsZero)
    {
        auto node = linear();
        add_in_order(node, collinear_points(3));

        EXPECT_DOUBLE_EQ(node.process_sample(0.0), 0.0);
        EXPECT_EQ(node.get_vertex_count(), 3U);
    }

    TEST_F(PathGeneratorNodeTest, EmptyNodeFramesAreHarmlessAndStaySettled)
    {
        auto node = linear();
        node.compute_frame();
        EXPECT_EQ(node.get_vertex_count(), 0U);

        node.clear_gpu_update_flag();
        node.compute_frame();
        node.compute_frame();

        EXPECT_FALSE(node.needs_gpu_update());
    }

    TEST_F(PathGeneratorNodeTest, ClearPathEmptiesTheNode)
    {
        auto node = linear();
        add_in_order(node, collinear_points(3));
        node.compute_frame();

        node.clear_path();
        node.compute_frame();

        EXPECT_EQ(node.get_control_point_count(), 0U);
        EXPECT_EQ(node.get_vertex_count(), 0U);
    }

    TEST_F(PathGeneratorNodeTest, ClearPathResetsLayoutVertexCount)
    {
        auto node = linear();
        add_in_order(node, collinear_points(3));
        node.compute_frame();

        node.clear_path();
        node.compute_frame();

        ASSERT_TRUE(node.get_vertex_layout().has_value());
        EXPECT_EQ(node.get_vertex_layout()->vertex_count, 0U);
    }

    TEST_F(PathGeneratorNodeTest, SingleControlPointDrawsNothing)
    {
        auto node = linear();
        node.add_control_point(make_vertex({ 1, 1, 1 }));
        node.compute_frame();

        EXPECT_EQ(node.get_vertex_count(), 0U);
    }

    TEST_F(PathGeneratorNodeTest, SingleControlPointLeavesLayoutAtZero)
    {
        auto node = linear();
        add_in_order(node, collinear_points(3));
        node.compute_frame();

        node.set_control_points({ make_vertex({ 1, 1, 1 }) });
        node.compute_frame();

        EXPECT_EQ(node.get_vertex_layout()->vertex_count, 0U);
    }

    TEST_F(PathGeneratorNodeTest, UpdatingALinearControlPointMovesItsVertex)
    {
        auto node = linear();
        add_in_order(node, collinear_points(4));
        node.compute_frame();

        node.update_control_point(1, make_vertex({ 7, 7, 0 }));
        node.compute_frame();

        EXPECT_TRUE(near_vec(uploaded_vertices(node)[1].position, { 7, 7, 0 }));
    }

    TEST_F(PathGeneratorNodeTest, UpdatingAnyLinearControlPointMatchesRebuildingFromScratch)
    {
        const auto pts = zigzag_points(5);

        for (size_t stored = 0; stored < pts.size(); ++stored) {
            SCOPED_TRACE(stored);

            auto node = linear();
            add_in_order(node, pts);
            node.compute_frame();
            node.update_control_point(stored, make_vertex({ 9, 9, 9 }));
            node.compute_frame();

            auto modified = pts;
            modified[pts.size() - 1 - stored] = make_vertex({ 9, 9, 9 });
            expect_same_geometry(uploaded_vertices(node), rebuilt(InterpolationMode::LINEAR, 4, modified));
        }
    }

    TEST_F(PathGeneratorNodeTest, UpdatingAnyCatmullRomControlPointMatchesRebuildingFromScratch)
    {
        const auto pts = zigzag_points(8);

        for (size_t stored = 0; stored < pts.size(); ++stored) {
            SCOPED_TRACE(stored);

            auto node = catmull(4);
            add_in_order(node, pts);
            node.compute_frame();
            node.update_control_point(stored, make_vertex({ 9, 9, 9 }));
            node.compute_frame();

            auto modified = pts;
            modified[pts.size() - 1 - stored] = make_vertex({ 9, 9, 9 });
            expect_same_geometry(uploaded_vertices(node), rebuilt(InterpolationMode::CATMULL_ROM, 4, modified));
        }
    }

    TEST_F(PathGeneratorNodeTest, UpdatingTheNewestCatmullRomPointOfAFourPointPathChangesTheCurve)
    {
        const auto pts = zigzag_points(4);

        auto node = catmull(4);
        add_in_order(node, pts);
        node.compute_frame();
        const auto before = uploaded_vertices(node);

        node.update_control_point(0, make_vertex({ 3, 8, 0 }));
        node.compute_frame();
        const auto after = uploaded_vertices(node);

        ASSERT_EQ(before.size(), after.size());
        bool changed = false;
        for (size_t i = 0; i < before.size(); ++i) {
            changed = changed || glm::length(before[i].position - after[i].position) > 1e-6F;
        }
        EXPECT_TRUE(changed);
    }

    TEST_F(PathGeneratorNodeTest, CatmullRomWithFewerThanFourPointsDrawsNothing)
    {
        auto node = catmull();
        add_in_order(node, collinear_points(3));
        node.compute_frame();

        EXPECT_EQ(node.get_vertex_count(), 0U);
        EXPECT_EQ(node.get_vertex_layout()->vertex_count, 0U);
    }

    TEST_F(PathGeneratorNodeTest, CatmullRomEmitsOneLineStripRunPerWindow)
    {
        auto node = catmull(5);
        add_in_order(node, collinear_points(5));
        node.compute_frame();

        EXPECT_EQ(node.get_vertex_count(), 16U);
        EXPECT_EQ(node.get_generated_vertex_count(), node.get_vertex_count());
    }

    TEST_F(PathGeneratorNodeTest, CatmullRomWindowsJoinEndToEnd)
    {
        auto node = catmull(5);
        add_in_order(node, collinear_points(4));
        node.compute_frame();

        const auto up = uploaded_vertices(node);
        ASSERT_EQ(up.size(), 8U);
        for (size_t i = 1; i + 1 < up.size(); i += 2) {
            EXPECT_TRUE(near_vec(up[i].position, up[i + 1].position)) << "junction " << i;
        }
    }

    TEST_F(PathGeneratorNodeTest, SamplesPerSegmentChangesTheVertexCount)
    {
        auto node = catmull(5);
        add_in_order(node, collinear_points(4));
        node.compute_frame();
        ASSERT_EQ(node.get_vertex_count(), 8U);

        node.set_samples_per_segment(3);
        node.compute_frame();

        EXPECT_EQ(node.get_vertex_count(), 4U);
    }

    TEST_F(PathGeneratorNodeTest, SwitchingToLinearDrawsTheControlPolyline)
    {
        auto node = catmull(5);
        add_in_order(node, collinear_points(4));
        node.compute_frame();

        node.set_interpolation_mode(InterpolationMode::LINEAR);
        node.compute_frame();

        EXPECT_EQ(node.get_vertex_count(), 4U);
    }

    TEST_F(PathGeneratorNodeTest, ArcLengthParameterizationKeepsTheVertexCountAndStaysFinite)
    {
        auto node = catmull(6);
        add_in_order(node, zigzag_points(5));
        node.compute_frame();
        const auto count = node.get_vertex_count();

        node.parameterize_arc_length(true);
        node.compute_frame();

        ASSERT_EQ(node.get_vertex_count(), count);
        for (const auto& v : uploaded_vertices(node)) {
            EXPECT_TRUE(std::isfinite(v.position.x) && std::isfinite(v.position.y) && std::isfinite(v.position.z));
        }
    }

    TEST_F(PathGeneratorNodeTest, UniformColourOverridesEveryLinearVertex)
    {
        auto node = linear();
        add_in_order(node, collinear_points(3));
        node.compute_frame();

        node.set_path_color({ 0, 1, 0 });
        node.compute_frame();

        for (const auto& v : uploaded_vertices(node)) {
            EXPECT_TRUE(near_vec(v.color, { 0, 1, 0 }));
        }
        EXPECT_TRUE(node.should_force_uniform_color());
        EXPECT_EQ(node.get_path_color(), glm::vec3(0, 1, 0));
    }

    TEST_F(PathGeneratorNodeTest, UniformColourSetBeforeTheFirstFrameApplies)
    {
        auto node = linear();
        add_in_order(node, collinear_points(3));
        node.set_path_color({ 0, 1, 0 });
        node.compute_frame();

        for (const auto& v : uploaded_vertices(node)) {
            EXPECT_TRUE(near_vec(v.color, { 0, 1, 0 }));
        }
    }

    TEST_F(PathGeneratorNodeTest, DisablingUniformColourRestoresPerPointColour)
    {
        auto node = linear();
        add_in_order(node, collinear_points(3));
        node.set_path_color({ 0, 1, 0 });
        node.compute_frame();

        node.force_uniform_color(false);
        node.compute_frame();

        const auto up = uploaded_vertices(node);
        EXPECT_TRUE(near_vec(up[0].color, { 0.3F, 0, 0 }));
        EXPECT_TRUE(near_vec(up[2].color, { 0.1F, 0, 0 }));
        EXPECT_FALSE(node.should_force_uniform_color());
    }

    TEST_F(PathGeneratorNodeTest, SettingColourWithoutForcingOnlyStoresIt)
    {
        auto node = linear();
        add_in_order(node, collinear_points(3));
        node.set_path_color({ 0, 0, 1 }, false);
        node.compute_frame();

        EXPECT_EQ(node.get_path_color(), glm::vec3(0, 0, 1));
        EXPECT_FALSE(node.should_force_uniform_color());
        EXPECT_TRUE(near_vec(uploaded_vertices(node)[0].color, { 0.3F, 0, 0 }));
    }

    TEST_F(PathGeneratorNodeTest, UniformThicknessOverridesEveryLinearVertex)
    {
        auto node = linear();
        add_in_order(node, collinear_points(3));
        node.compute_frame();

        node.set_path_thickness(6.0F);
        node.compute_frame();

        for (const auto& v : uploaded_vertices(node)) {
            EXPECT_FLOAT_EQ(v.thickness, 6.0F);
        }
        EXPECT_FLOAT_EQ(node.get_path_thickness(), 6.0F);
    }

    TEST_F(PathGeneratorNodeTest, DisablingUniformThicknessRestoresPerPointThickness)
    {
        auto node = linear();
        add_in_order(node, collinear_points(3));
        node.set_path_thickness(6.0F);
        node.compute_frame();

        node.force_uniform_thickness(false);
        node.compute_frame();

        const auto up = uploaded_vertices(node);
        EXPECT_FLOAT_EQ(up[0].thickness, 3.0F);
        EXPECT_FLOAT_EQ(up[2].thickness, 1.0F);
    }

    TEST_F(PathGeneratorNodeTest, UniformColourOverridesEveryCatmullRomVertex)
    {
        auto node = catmull(5);
        add_in_order(node, collinear_points(5));
        node.compute_frame();

        node.set_path_color({ 0, 1, 0 });
        node.compute_frame();

        ASSERT_GT(node.get_vertex_count(), 0U);
        for (const auto& v : uploaded_vertices(node)) {
            EXPECT_TRUE(near_vec(v.color, { 0, 1, 0 }));
        }
    }

    TEST_F(PathGeneratorNodeTest, UniformThicknessOverridesEveryCatmullRomVertex)
    {
        auto node = catmull(5);
        add_in_order(node, collinear_points(5));
        node.set_path_thickness(5.0F);
        node.compute_frame();

        ASSERT_GT(node.get_vertex_count(), 0U);
        for (const auto& v : uploaded_vertices(node)) {
            EXPECT_FLOAT_EQ(v.thickness, 5.0F);
        }
    }

    TEST_F(PathGeneratorNodeTest, DrawnStrokePassesThroughItsPointsInOrder)
    {
        auto node = linear();
        node.draw_to(make_vertex({ 0, 0, 0 }));
        node.draw_to(make_vertex({ 1, 1, 0 }));
        node.draw_to(make_vertex({ 2, 0, 0 }));
        node.compute_frame();

        const auto up = uploaded_vertices(node);
        ASSERT_FALSE(up.empty());
        EXPECT_TRUE(contains_in_order(up, { { 0, 0, 0 }, { 1, 1, 0 }, { 2, 0, 0 } }));
        EXPECT_TRUE(near_vec(up.front().position, { 0, 0, 0 }));
        EXPECT_TRUE(near_vec(up.back().position, { 2, 0, 0 }));
    }

    TEST_F(PathGeneratorNodeTest, DrawnStrokeGrowsOnEachFrame)
    {
        auto node = linear();
        node.draw_to(make_vertex({ 0, 0, 0 }));
        node.draw_to(make_vertex({ 1, 0, 0 }));
        node.compute_frame();
        const auto first = node.get_vertex_count();

        node.draw_to(make_vertex({ 2, 0, 0 }));
        node.compute_frame();

        EXPECT_GT(node.get_vertex_count(), first);
        EXPECT_TRUE(near_vec(uploaded_vertices(node).back().position, { 2, 0, 0 }));
    }

    TEST_F(PathGeneratorNodeTest, DrawToAfterAnUploadDoesNotLoseTheStroke)
    {
        auto node = linear();
        add_in_order(node, collinear_points(2));
        node.compute_frame();

        node.draw_to(make_vertex({ 5, 5, 0 }));
        node.draw_to(make_vertex({ 6, 5, 0 }));
        node.clear_gpu_update_flag();
        node.compute_frame();

        EXPECT_TRUE(near_vec(uploaded_vertices(node).back().position, { 6, 5, 0 }));
    }

    TEST_F(PathGeneratorNodeTest, UniformColourAppliesToEveryVertexOfADrawnStroke)
    {
        auto node = linear();
        node.set_path_color({ 0, 1, 0 });
        node.draw_to(make_vertex({ 0, 0, 0 }, 2.0F, { 1, 0, 0 }));
        node.draw_to(make_vertex({ 1, 0, 0 }, 2.0F, { 1, 0, 0 }));
        node.compute_frame();

        for (const auto& v : uploaded_vertices(node)) {
            EXPECT_TRUE(near_vec(v.color, { 0, 1, 0 }));
        }
    }

    TEST_F(PathGeneratorNodeTest, CompletingAShortStrokeKeepsItVisible)
    {
        auto node = linear();
        node.draw_to(make_vertex({ 0, 0, 0 }));
        node.draw_to(make_vertex({ 1, 0, 0 }));
        node.draw_to(make_vertex({ 2, 0, 0 }));
        node.compute_frame();
        const auto before = uploaded_vertices(node);

        node.complete();
        node.compute_frame();

        expect_same_geometry(uploaded_vertices(node), before);
    }

    TEST_F(PathGeneratorNodeTest, ANewStrokeAfterCompleteDoesNotEraseTheFinishedOne)
    {
        auto node = linear();
        node.draw_to(make_vertex({ 0, 0, 0 }));
        node.draw_to(make_vertex({ 1, 0, 0 }));
        node.complete();
        node.compute_frame();
        const auto finished = node.get_vertex_count();

        node.draw_to(make_vertex({ 5, 5, 0 }));
        node.draw_to(make_vertex({ 6, 5, 0 }));
        node.compute_frame();

        EXPECT_GT(node.get_vertex_count(), finished);
        EXPECT_TRUE(contains_in_order(uploaded_vertices(node), { { 0, 0, 0 }, { 1, 0, 0 }, { 5, 5, 0 }, { 6, 5, 0 } }));
    }

    TEST_F(PathGeneratorNodeTest, CompletingALongStrokeFitsACurveEndingAtTheLastPoint)
    {
        auto node = catmull(4);
        for (int i = 0; i < 6; ++i) {
            node.draw_to(make_vertex({ static_cast<float>(i), (i % 2 == 0) ? 0.0F : 1.0F, 0.0F }));
        }

        node.complete();
        node.compute_frame();

        const auto up = uploaded_vertices(node);
        ASSERT_FALSE(up.empty());
        EXPECT_TRUE(near_vec(up.back().position, { 5, 1, 0 }));
    }

    TEST_F(PathGeneratorNodeTest, ClearPathAlsoDiscardsDrawnStrokes)
    {
        auto node = linear();
        node.draw_to(make_vertex({ 0, 0, 0 }));
        node.draw_to(make_vertex({ 1, 0, 0 }));
        node.complete();
        node.draw_to(make_vertex({ 2, 0, 0 }));
        node.compute_frame();
        ASSERT_GT(node.get_vertex_count(), 0U);

        node.clear_path();
        node.compute_frame();

        EXPECT_EQ(node.get_vertex_count(), 0U);
    }

    TEST_F(PathGeneratorNodeTest, CustomFunctionReceivesControlPointsAndParameterRange)
    {
        std::vector<size_t> sizes;
        std::vector<double> params;
        PathGeneratorNode node(
            [&](std::span<const LineVertex> pts, double t) {
                sizes.push_back(pts.size());
                params.push_back(t);
                return glm::vec3(static_cast<float>(t) * 9.0F, 0.0F, 0.0F);
            },
            5, 8);
        add_in_order(node, collinear_points(3));

        node.compute_frame();

        ASSERT_EQ(node.get_vertex_count(), 10U);
        ASSERT_EQ(params.size(), 10U);
        EXPECT_DOUBLE_EQ(params.front(), 0.0);
        EXPECT_DOUBLE_EQ(params.back(), 1.0);
        for (size_t s : sizes) {
            EXPECT_EQ(s, 3U);
        }
    }

    TEST_F(PathGeneratorNodeTest, CustomFunctionPositionsAreUploadedInParameterOrder)
    {
        PathGeneratorNode node(
            [](std::span<const LineVertex>, double t) {
                return glm::vec3(static_cast<float>(t) * 9.0F, 0.0F, 0.0F);
            },
            5, 8);
        add_in_order(node, collinear_points(3));
        node.compute_frame();

        const auto up = uploaded_vertices(node);
        ASSERT_EQ(up.size(), 10U);
        for (size_t i = 0; i < up.size(); ++i) {
            EXPECT_TRUE(near_vec(up[i].position, { static_cast<float>(i), 0, 0 })) << "vertex " << i;
        }
    }

    TEST_F(PathGeneratorNodeTest, CustomFunctionWithOnePointDrawsNothing)
    {
        PathGeneratorNode node(
            [](std::span<const LineVertex>, double) { return glm::vec3(0.0F); }, 5, 8);
        node.add_control_point(make_vertex({ 1, 1, 1 }));
        node.compute_frame();

        EXPECT_EQ(node.get_vertex_count(), 0U);
    }

    TEST_F(PathGeneratorNodeTest, CustomPathUsesLineStripTopology)
    {
        PathGeneratorNode node(
            [](std::span<const LineVertex>, double) { return glm::vec3(0.0F); }, 5, 8);

        EXPECT_EQ(node.get_primitive_topology(), Portal::Graphics::PrimitiveTopology::LINE_STRIP);
    }

    class PathGeneratorModeTest : public ::testing::TestWithParam<InterpolationMode> {
    };

    TEST_P(PathGeneratorModeTest, FourPointsMakeOneWindowOfSamplesMinusOnePairs)
    {
        PathGeneratorNode node(GetParam(), 6, 16);
        add_in_order(node, collinear_points(4));
        node.compute_frame();

        EXPECT_EQ(node.get_vertex_count(), 10U);
    }

    TEST_P(PathGeneratorModeTest, EachExtraPointAddsAWindow)
    {
        PathGeneratorNode node(GetParam(), 6, 16);
        add_in_order(node, collinear_points(6));
        node.compute_frame();

        EXPECT_EQ(node.get_vertex_count(), 30U);
    }

    TEST_P(PathGeneratorModeTest, ThreePointsDrawNothing)
    {
        PathGeneratorNode node(GetParam(), 6, 16);
        add_in_order(node, collinear_points(3));
        node.compute_frame();

        EXPECT_EQ(node.get_vertex_count(), 0U);
    }

    TEST_P(PathGeneratorModeTest, CollinearControlPointsGiveAFiniteCurveOnTheirLine)
    {
        PathGeneratorNode node(GetParam(), 6, 16);
        add_in_order(node, collinear_points(5));
        node.compute_frame();

        ASSERT_GT(node.get_vertex_count(), 0U);
        for (const auto& v : uploaded_vertices(node)) {
            ASSERT_TRUE(std::isfinite(v.position.x));
            EXPECT_NEAR(v.position.y, 0.0F, 1e-4F);
            EXPECT_NEAR(v.position.z, 0.0F, 1e-4F);
            EXPECT_GE(v.position.x, -0.5F);
            EXPECT_LE(v.position.x, 4.5F);
        }
    }

    INSTANTIATE_TEST_SUITE_P(
        PathGeneratorModes,
        PathGeneratorModeTest,
        ::testing::Values(
            InterpolationMode::CATMULL_ROM,
            InterpolationMode::CUBIC_HERMITE,
            InterpolationMode::CUBIC_BEZIER,
            InterpolationMode::QUADRATIC_BEZIER,
            InterpolationMode::BSPLINE));

} // namespace

}
