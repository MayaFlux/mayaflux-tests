#include "gtest/gtest.h"

#include "MayaFlux/Nodes/Graphics/GeometryLeafNode.hpp"
#include "MayaFlux/Nodes/Graphics/MeshWriterNode.hpp"
#include "MayaFlux/Nodes/Network/Operators/FieldOperator.hpp"
#include "MayaFlux/Nodes/Network/Operators/InstanceFieldOperator.hpp"
#include "MayaFlux/Nodes/Network/Operators/MeshFieldOperator.hpp"
#include "MayaFlux/Nodes/Network/Operators/PathOperator.hpp"
#include "MayaFlux/Nodes/Network/Operators/PhysicsOperator.hpp"

namespace MayaFlux::Test {

namespace {

    using Kinesis::FieldMode;
    using Kinesis::FieldTarget;
    using Nodes::LineVertex;
    using Nodes::MeshVertex;
    using Nodes::PointVertex;
    using Nodes::GpuSync::GeometryLeafNode;
    using Nodes::GpuSync::MeshWriterNode;
    using Nodes::Network::FieldOperator;
    using Nodes::Network::GeometrySlot;
    using Nodes::Network::InstanceFieldOperator;
    using Nodes::Network::MeshFieldOperator;
    using Nodes::Network::MeshSlot;
    using Nodes::Network::PathOperator;
    using Nodes::Network::PhysicsOperator;

    constexpr float k_eps = 1e-5F;

    ::testing::AssertionResult near_vec(const glm::vec3& a, const glm::vec3& b)
    {
        if (std::abs(a.x - b.x) < k_eps && std::abs(a.y - b.y) < k_eps && std::abs(a.z - b.z) < k_eps) {
            return ::testing::AssertionSuccess();
        }
        return ::testing::AssertionFailure() << "(" << a.x << ", " << a.y << ", " << a.z << ") != ("
                                             << b.x << ", " << b.y << ", " << b.z << ")";
    }

    Kinesis::VectorField constant_vector(const glm::vec3& value)
    {
        return Kinesis::VectorField { .fn = [value](const glm::vec3&) { return value; } };
    }

    Kinesis::SpatialField constant_scalar(float value)
    {
        return Kinesis::SpatialField { .fn = [value](const glm::vec3&) { return value; } };
    }

    std::vector<PointVertex> point_vertices(uint32_t count)
    {
        std::vector<PointVertex> out;
        for (uint32_t i = 0; i < count; ++i) {
            PointVertex v;
            const auto f = static_cast<float>(i);
            v.position = { f, f + 1.0F, f + 2.0F };
            v.color = { 0.1F * f, 0.2F, 0.3F, 1.0F };
            v.size = 3.0F + f;
            out.push_back(v);
        }
        return out;
    }

    std::vector<LineVertex> line_vertices(uint32_t count)
    {
        std::vector<LineVertex> out;
        for (uint32_t i = 0; i < count; ++i) {
            LineVertex v;
            const auto f = static_cast<float>(i);
            v.position = { f, f + 1.0F, f + 2.0F };
            v.thickness = 3.0F + f;
            out.push_back(v);
        }
        return out;
    }

    std::vector<MeshVertex> mesh_vertices(uint32_t count, float first = 0.0F)
    {
        std::vector<MeshVertex> out;
        for (uint32_t i = 0; i < count; ++i) {
            MeshVertex v;
            const auto f = first + static_cast<float>(i);
            v.position = { f, f + 1.0F, f + 2.0F };
            v.weight = 3.0F + f;
            out.push_back(v);
        }
        return out;
    }

    glm::mat4 placed_at(const glm::vec3& at)
    {
        glm::mat4 m(1.0F);
        m[3] = glm::vec4(at, 1.0F);
        return m;
    }

    template <typename Vertex>
    std::vector<uint8_t> raw_bytes(const std::vector<Vertex>& v)
    {
        const auto* p = reinterpret_cast<const uint8_t*>(v.data());
        return { p, p + v.size() * sizeof(Vertex) };
    }

    struct VertexKind {
        const char* name;
        const char* type_name;
        std::function<std::unique_ptr<FieldOperator>(uint32_t)> make;
        std::function<bool(const FieldOperator&, uint32_t)> round_trips;
        std::function<float(const FieldOperator&, size_t)> scalar_at;
    };

    std::vector<VertexKind> vertex_kinds()
    {
        return {
            { .name = "Point",
                .type_name = "PointVertex",
                .make = [](uint32_t n) {
                    auto op = std::make_unique<FieldOperator>();
                    op->initialize(point_vertices(n));
                    return op; },
                .round_trips = [](const FieldOperator& op, uint32_t n) {
                    return raw_bytes(op.extract_point_vertices()) == raw_bytes(point_vertices(n)); },
                .scalar_at = [](const FieldOperator& op, size_t i) { return op.extract_point_vertices()[i].size; } },
            { .name = "Line",
                .type_name = "LineVertex",
                .make = [](uint32_t n) {
                    auto op = std::make_unique<FieldOperator>();
                    op->initialize(line_vertices(n));
                    return op; },
                .round_trips = [](const FieldOperator& op, uint32_t n) {
                    return raw_bytes(op.extract_line_vertices()) == raw_bytes(line_vertices(n)); },
                .scalar_at = [](const FieldOperator& op, size_t i) { return op.extract_line_vertices()[i].thickness; } },
            { .name = "Mesh",
                .type_name = "MeshVertex",
                .make = [](uint32_t n) {
                    auto op = std::make_unique<FieldOperator>();
                    op->initialize(mesh_vertices(n));
                    return op; },
                .round_trips = [](const FieldOperator& op, uint32_t n) {
                    return raw_bytes(op.extract_mesh_vertices()) == raw_bytes(mesh_vertices(n)); },
                .scalar_at = [](const FieldOperator& op, size_t i) { return op.extract_mesh_vertices()[i].weight; } },
        };
    }

    class FieldOperatorVertexKindTest : public ::testing::TestWithParam<VertexKind> { };

    TEST_P(FieldOperatorVertexKindTest, InitializeReportsTypeCountAndLayout)
    {
        auto op = GetParam().make(5);

        EXPECT_STREQ(op->get_vertex_type_name(), GetParam().type_name);
        EXPECT_EQ(op->get_vertex_count(), 5U);
        EXPECT_EQ(op->get_point_count(), 5U);
        EXPECT_EQ(op->get_vertex_layout().stride_bytes, 68U);
        EXPECT_EQ(op->get_vertex_layout().vertex_count, 5U);
        EXPECT_EQ(op->get_vertex_data().size(), 5U * 68U);
    }

    TEST_P(FieldOperatorVertexKindTest, InitializeMarksDirtyAndKeepsTheSourceBytes)
    {
        auto op = GetParam().make(4);

        EXPECT_TRUE(op->is_vertex_data_dirty());
        EXPECT_TRUE(GetParam().round_trips(*op, 4));
    }

    TEST_P(FieldOperatorVertexKindTest, ScalarFieldWritesTheWeightSlotOfEveryKind)
    {
        auto op = GetParam().make(3);
        op->bind(FieldTarget::SCALAR, constant_scalar(7.5F));

        op->process(0.0F);

        for (size_t i = 0; i < 3; ++i) {
            EXPECT_FLOAT_EQ(GetParam().scalar_at(*op, i), 7.5F) << "vertex " << i;
        }
    }

    INSTANTIATE_TEST_SUITE_P(
        FieldOperators,
        FieldOperatorVertexKindTest,
        ::testing::ValuesIn(vertex_kinds()),
        [](const ::testing::TestParamInfo<VertexKind>& info) { return std::string(info.param.name); });

    class FieldOperatorTest : public ::testing::Test {
    protected:
        FieldOperator op;

        void SetUp() override { op.initialize(point_vertices(4)); }

        [[nodiscard]] PointVertex vertex(size_t i) const { return op.extract_point_vertices()[i]; }
    };

    TEST(FieldOperatorEmptyTest, UninitializedOperatorHasNothingToProcess)
    {
        FieldOperator op;
        op.bind(FieldTarget::POSITION, constant_vector({ 0.0F, 1.0F, 0.0F }));

        EXPECT_NO_THROW(op.process(0.0F));

        EXPECT_EQ(op.get_vertex_count(), 0U);
        EXPECT_FALSE(op.is_vertex_data_dirty());
        EXPECT_TRUE(op.get_vertex_data().empty());
        EXPECT_STREQ(op.get_vertex_type_name(), "Unknown");
        EXPECT_TRUE(op.extract_point_vertices().empty());
    }

    TEST_F(FieldOperatorTest, AbsoluteModeAppliesTheDisplacementFromTheReferenceEveryFrame)
    {
        op.bind(FieldTarget::POSITION, constant_vector({ 0.0F, 10.0F, 0.0F }));

        op.process(0.0F);
        op.process(0.0F);
        op.process(0.0F);

        EXPECT_TRUE(near_vec(vertex(0).position, { 0.0F, 11.0F, 2.0F }));
        EXPECT_TRUE(near_vec(vertex(3).position, { 3.0F, 14.0F, 5.0F }));
    }

    TEST_F(FieldOperatorTest, AccumulateModeCompoundsTheDisplacement)
    {
        op.set_mode(FieldMode::ACCUMULATE);
        op.bind(FieldTarget::POSITION, constant_vector({ 0.0F, 10.0F, 0.0F }));

        op.process(0.0F);
        op.process(0.0F);
        op.process(0.0F);

        EXPECT_TRUE(near_vec(vertex(0).position, { 0.0F, 31.0F, 2.0F }));
    }

    TEST_F(FieldOperatorTest, ModeParameterSwitchesBetweenAbsoluteAndAccumulate)
    {
        EXPECT_EQ(op.get_mode(), FieldMode::ABSOLUTE);

        op.set_parameter("mode", 1.0);
        EXPECT_EQ(op.get_mode(), FieldMode::ACCUMULATE);

        op.set_parameter("mode", 0.0);
        EXPECT_EQ(op.get_mode(), FieldMode::ABSOLUTE);
    }

    TEST_F(FieldOperatorTest, SwitchingBackToAbsoluteRestoresTheReference)
    {
        op.set_mode(FieldMode::ACCUMULATE);
        op.bind(FieldTarget::POSITION, constant_vector({ 0.0F, 10.0F, 0.0F }));
        op.process(0.0F);
        op.process(0.0F);

        op.set_mode(FieldMode::ABSOLUTE);
        op.process(0.0F);

        EXPECT_TRUE(near_vec(vertex(0).position, { 0.0F, 11.0F, 2.0F }));
    }

    TEST_F(FieldOperatorTest, SeveralPositionFieldsSum)
    {
        op.bind(FieldTarget::POSITION, constant_vector({ 1.0F, 0.0F, 0.0F }));
        op.bind(FieldTarget::POSITION, constant_vector({ 0.0F, 2.0F, 0.0F }));

        op.process(0.0F);

        EXPECT_TRUE(near_vec(vertex(0).position, { 1.0F, 3.0F, 2.0F }));
    }

    TEST_F(FieldOperatorTest, ColorFieldReplacesTheColor)
    {
        op.bind(FieldTarget::COLOR, constant_vector({ 0.25F, 0.5F, 0.75F }));
        op.bind(FieldTarget::COLOR, constant_vector({ 0.25F, 0.0F, 0.0F }));

        op.process(0.0F);

        EXPECT_TRUE(near_vec(vertex(2).color, { 0.5F, 0.5F, 0.75F }));
    }

    TEST_F(FieldOperatorTest, OneFieldOnSeveralTargetsWritesAll)
    {
        op.bind(FieldTarget::POSITION | FieldTarget::COLOR, constant_vector({ 0.0F, 0.5F, 0.0F }));

        op.process(0.0F);

        EXPECT_TRUE(near_vec(vertex(0).position, { 0.0F, 1.5F, 2.0F }));
        EXPECT_TRUE(near_vec(vertex(0).color, { 0.0F, 0.5F, 0.0F }));
        EXPECT_EQ(op.query_state("field_count"), 2.0);
    }

    TEST_F(FieldOperatorTest, NormalFieldIsNormalised)
    {
        op.bind(FieldTarget::NORMAL, constant_vector({ 0.0F, 3.0F, 0.0F }));

        op.process(0.0F);

        EXPECT_TRUE(near_vec(vertex(1).normal, { 0.0F, 1.0F, 0.0F }));
    }

    TEST_F(FieldOperatorTest, DegenerateNormalAndTangentFallBackToAxes)
    {
        op.bind(FieldTarget::NORMAL, constant_vector(glm::vec3(0.0F)));
        op.bind(FieldTarget::TANGENT, constant_vector(glm::vec3(0.0F)));

        op.process(0.0F);

        EXPECT_TRUE(near_vec(vertex(0).normal, { 0.0F, 0.0F, 1.0F }));
        EXPECT_TRUE(near_vec(vertex(0).tangent, { 1.0F, 0.0F, 0.0F }));
    }

    TEST_F(FieldOperatorTest, UvFieldIsEvaluatedAtTheReferencePosition)
    {
        op.bind(FieldTarget::POSITION, constant_vector({ 0.0F, 100.0F, 0.0F }));
        op.bind(FieldTarget::UV, Kinesis::UVField { .fn = [](const glm::vec3& p) { return glm::vec2(p.x, p.y); } });

        op.process(0.0F);

        EXPECT_NEAR(vertex(2).uv.x, 2.0F, k_eps);
        EXPECT_NEAR(vertex(2).uv.y, 3.0F, k_eps);
    }

    TEST_F(FieldOperatorTest, FieldsOnlyBindToTheirOwnTargets)
    {
        op.bind(FieldTarget::SCALAR, constant_vector({ 1.0F, 1.0F, 1.0F }));
        op.bind(FieldTarget::UV, constant_vector({ 1.0F, 1.0F, 1.0F }));
        op.bind(FieldTarget::POSITION | FieldTarget::SCALAR, constant_vector({ 1.0F, 1.0F, 1.0F }));
        op.bind(FieldTarget::POSITION, constant_scalar(1.0F));
        op.bind(FieldTarget::SCALAR | FieldTarget::COLOR, constant_scalar(1.0F));
        op.bind(FieldTarget::SCALAR, Kinesis::UVField { .fn = [](const glm::vec3&) { return glm::vec2(0.0F); } });
        op.bind(FieldTarget::NONE, constant_vector({ 1.0F, 1.0F, 1.0F }));

        EXPECT_EQ(op.query_state("field_count"), 0.0);
    }

    TEST_F(FieldOperatorTest, UnbindClearsOnlyTheNamedTargets)
    {
        op.bind(FieldTarget::POSITION, constant_vector({ 0.0F, 1.0F, 0.0F }));
        op.bind(FieldTarget::COLOR, constant_vector({ 1.0F, 1.0F, 1.0F }));

        op.unbind(FieldTarget::POSITION);

        EXPECT_EQ(op.query_state("field_count"), 1.0);
        op.process(0.0F);
        EXPECT_TRUE(near_vec(vertex(0).position, { 0.0F, 1.0F, 2.0F }));
        EXPECT_TRUE(near_vec(vertex(0).color, { 1.0F, 1.0F, 1.0F }));
    }

    TEST_F(FieldOperatorTest, UnbindAllLeavesTheReferenceGeometry)
    {
        op.bind(FieldTarget::POSITION, constant_vector({ 0.0F, 1.0F, 0.0F }));
        op.process(0.0F);

        op.unbind_all();
        op.process(0.0F);

        EXPECT_EQ(op.query_state("field_count"), 0.0);
        EXPECT_TRUE(near_vec(vertex(0).position, { 0.0F, 1.0F, 2.0F }));
    }

    TEST_F(FieldOperatorTest, ProcessMarksDirtyAndCleanClearsIt)
    {
        op.mark_vertex_data_clean();
        EXPECT_FALSE(op.is_vertex_data_dirty());

        op.process(0.0F);

        EXPECT_TRUE(op.is_vertex_data_dirty());
    }

    TEST_F(FieldOperatorTest, QueryStateReportsPointAndFieldCounts)
    {
        EXPECT_EQ(op.query_state("point_count"), 4.0);
        EXPECT_EQ(op.query_state("field_count"), 0.0);
        EXPECT_FALSE(op.query_state("no_such_state").has_value());
    }

    TEST(FieldOperatorSeedTest, EmptyOperatorSeedsFromPhysicsAsPointVertices)
    {
        PhysicsOperator upstream;
        upstream.initialize(point_vertices(5));
        FieldOperator op;

        op.seed_from_upstream(&upstream);

        EXPECT_EQ(op.get_vertex_count(), 5U);
        EXPECT_STREQ(op.get_vertex_type_name(), "PointVertex");
        EXPECT_TRUE(raw_bytes(op.extract_point_vertices()) == raw_bytes(point_vertices(5)));
    }

    TEST(FieldOperatorSeedTest, UnrecognisedUpstreamTypeSeedsAsLineVertices)
    {
        PathOperator upstream;
        upstream.initialize_paths({ line_vertices(6) }, Kinesis::InterpolationMode::CATMULL_ROM);
        FieldOperator op;

        op.seed_from_upstream(&upstream);

        EXPECT_STREQ(op.get_vertex_type_name(), "LineVertex");
        EXPECT_EQ(op.get_vertex_count(), upstream.get_vertex_count());
    }

    TEST(FieldOperatorSeedTest, PopulatedOperatorIgnoresUpstream)
    {
        PhysicsOperator upstream;
        upstream.initialize(point_vertices(5));
        FieldOperator op;
        op.initialize(point_vertices(3));

        op.seed_from_upstream(&upstream);

        EXPECT_EQ(op.get_vertex_count(), 3U);
    }

    TEST(FieldOperatorSeedTest, NullUpstreamIsIgnored)
    {
        FieldOperator op;

        EXPECT_NO_THROW(op.seed_from_upstream(nullptr));
        EXPECT_EQ(op.get_vertex_count(), 0U);
    }

    class MeshFieldOperatorTest : public ::testing::Test {
    protected:
        std::vector<MeshSlot> slots;
        std::vector<uint32_t> order;
        MeshFieldOperator op;

        void SetUp() override
        {
            for (uint32_t i = 0; i < 2; ++i) {
                MeshSlot slot;
                slot.index = i;
                slot.node = std::make_shared<MeshWriterNode>(16);
                slot.node->set_mesh(mesh_vertices(4, static_cast<float>(i) * 10.0F), std::vector<uint32_t> { 0, 1, 2, 2, 3, 0 });
                slots.push_back(std::move(slot));
                order.push_back(i);
            }
            op.set_slots(slots, order);
        }

        [[nodiscard]] glm::vec3 position(uint32_t slot, size_t vertex) const
        {
            return slots[slot].node->get_mesh_vertices()[vertex].position;
        }
    };

    TEST_F(MeshFieldOperatorTest, BoundFieldDisplacesOnlyItsSlot)
    {
        op.bind(0, FieldTarget::POSITION, constant_vector({ 0.0F, 5.0F, 0.0F }));

        op.process(0.0F);

        EXPECT_TRUE(near_vec(position(0, 0), { 0.0F, 6.0F, 2.0F }));
        EXPECT_TRUE(near_vec(position(1, 0), { 10.0F, 11.0F, 12.0F }));
    }

    TEST_F(MeshFieldOperatorTest, SlotsKeepIndependentFields)
    {
        op.bind(0, FieldTarget::POSITION, constant_vector({ 1.0F, 0.0F, 0.0F }));
        op.bind(1, FieldTarget::POSITION, constant_vector({ 0.0F, 0.0F, 7.0F }));

        op.process(0.0F);

        EXPECT_TRUE(near_vec(position(0, 0), { 1.0F, 1.0F, 2.0F }));
        EXPECT_TRUE(near_vec(position(1, 0), { 10.0F, 11.0F, 19.0F }));
    }

    TEST_F(MeshFieldOperatorTest, DefaultsToAbsoluteSoRepeatedFramesDoNotCompound)
    {
        op.bind(0, FieldTarget::POSITION, constant_vector({ 0.0F, 5.0F, 0.0F }));

        op.process(0.0F);
        op.process(0.0F);
        op.process(0.0F);

        EXPECT_TRUE(near_vec(position(0, 0), { 0.0F, 6.0F, 2.0F }));
    }

    TEST_F(MeshFieldOperatorTest, AccumulateModeCompoundsPerSlot)
    {
        op.bind(0, FieldTarget::POSITION, constant_vector({ 0.0F, 5.0F, 0.0F }));
        op.set_mode(0, FieldMode::ACCUMULATE);

        op.process(0.0F);
        op.process(0.0F);

        EXPECT_TRUE(near_vec(position(0, 0), { 0.0F, 11.0F, 2.0F }));
    }

    TEST_F(MeshFieldOperatorTest, ScalarAndUvFieldsWriteTheMeshAttributes)
    {
        op.bind(0, FieldTarget::SCALAR, constant_scalar(9.0F));
        op.bind(0, FieldTarget::UV, Kinesis::UVField { .fn = [](const glm::vec3&) { return glm::vec2(0.25F, 0.75F); } });

        op.process(0.0F);

        const auto& v = slots[0].node->get_mesh_vertices()[2];
        EXPECT_FLOAT_EQ(v.weight, 9.0F);
        EXPECT_NEAR(v.uv.x, 0.25F, k_eps);
        EXPECT_NEAR(v.uv.y, 0.75F, k_eps);
    }

    TEST_F(MeshFieldOperatorTest, ProcessingKeepsTheVertexCount)
    {
        op.bind(0, FieldTarget::POSITION, constant_vector({ 0.0F, 5.0F, 0.0F }));

        op.process(0.0F);

        EXPECT_EQ(slots[0].node->get_mesh_vertex_count(), 4U);
    }

    TEST_F(MeshFieldOperatorTest, BindingBeforeSlotsAreSetStillTakesEffect)
    {
        MeshFieldOperator late;
        late.bind(0, FieldTarget::POSITION, constant_vector({ 0.0F, 5.0F, 0.0F }));
        late.set_slots(slots, order);

        late.process(0.0F);

        EXPECT_TRUE(near_vec(position(0, 0), { 0.0F, 6.0F, 2.0F }));
    }

    TEST_F(MeshFieldOperatorTest, OutOfRangeSlotBindingIsHarmless)
    {
        op.bind(9, FieldTarget::POSITION, constant_vector({ 0.0F, 5.0F, 0.0F }));

        EXPECT_NO_THROW(op.process(0.0F));
        EXPECT_TRUE(near_vec(position(0, 0), { 0.0F, 1.0F, 2.0F }));
    }

    TEST_F(MeshFieldOperatorTest, UnbindingATargetRestoresTheReferenceOnTheNextFrame)
    {
        op.bind(0, FieldTarget::POSITION, constant_vector({ 0.0F, 5.0F, 0.0F }));
        op.process(0.0F);

        op.unbind(0, FieldTarget::POSITION);
        op.process(0.0F);

        EXPECT_TRUE(near_vec(position(0, 0), { 0.0F, 1.0F, 2.0F }));
    }

    TEST_F(MeshFieldOperatorTest, UnbindingASlotStopsProcessingAndLeavesTheLastResult)
    {
        op.bind(0, FieldTarget::POSITION, constant_vector({ 0.0F, 5.0F, 0.0F }));
        op.process(0.0F);

        op.unbind_slot(0);
        op.process(0.0F);

        EXPECT_TRUE(near_vec(position(0, 0), { 0.0F, 6.0F, 2.0F }));
    }

    TEST_F(MeshFieldOperatorTest, UnbindAllStopsEverySlot)
    {
        op.bind(0, FieldTarget::POSITION, constant_vector({ 0.0F, 5.0F, 0.0F }));
        op.bind(1, FieldTarget::POSITION, constant_vector({ 0.0F, 5.0F, 0.0F }));
        op.process(0.0F);

        op.unbind_all();
        slots[0].node->set_mesh_vertices(mesh_vertices(4));
        op.process(0.0F);

        EXPECT_TRUE(near_vec(position(0, 0), { 0.0F, 1.0F, 2.0F }));
    }

    TEST_F(MeshFieldOperatorTest, SlotWithoutANodeIsSkipped)
    {
        slots[1].node.reset();
        op.bind(1, FieldTarget::POSITION, constant_vector({ 0.0F, 5.0F, 0.0F }));
        op.bind(0, FieldTarget::POSITION, constant_vector({ 0.0F, 5.0F, 0.0F }));

        EXPECT_NO_THROW(op.process(0.0F));
        EXPECT_TRUE(near_vec(position(0, 0), { 0.0F, 6.0F, 2.0F }));
    }

    TEST_F(MeshFieldOperatorTest, MeshReplacedAfterFirstFrameIsWhatTheFieldActsOn)
    {
        op.bind(0, FieldTarget::POSITION, constant_vector({ 0.0F, 5.0F, 0.0F }));
        op.process(0.0F);

        slots[0].node->set_mesh_vertices(mesh_vertices(6, 100.0F));
        op.process(0.0F);

        ASSERT_EQ(slots[0].node->get_mesh_vertex_count(), 6U);
        EXPECT_TRUE(near_vec(position(0, 0), { 100.0F, 106.0F, 102.0F }));
    }

    TEST(MeshFieldOperatorIdentityTest, ReportsItsTypeAndHasNoGpuExecutorByDefault)
    {
        MeshFieldOperator op;

        EXPECT_EQ(op.get_type_name(), "MeshField");
        EXPECT_FALSE(op.has_gpu_executor());
        EXPECT_NO_THROW(op.set_gpu_executor(nullptr));
        EXPECT_FALSE(op.has_gpu_executor());
        EXPECT_FALSE(op.query_state("anything").has_value());
        EXPECT_NO_THROW(op.process(0.0F));
    }

    class InstanceFieldOperatorTest : public ::testing::Test {
    protected:
        std::vector<GeometrySlot> slots;
        InstanceFieldOperator op;

        void SetUp() override
        {
            for (uint32_t i = 0; i < 2; ++i) {
                GeometrySlot slot;
                slot.index = i;
                slot.node = std::make_shared<GeometryLeafNode>(size_t { 4 });
                slot.transform = placed_at({ static_cast<float>(i) * 10.0F, 1.0F, 2.0F });
                slot.dirty = false;
                slots.push_back(std::move(slot));
            }
            op.set_slots(slots);
        }

        [[nodiscard]] glm::vec3 origin(uint32_t slot) const { return glm::vec3(slots[slot].transform[3]); }
    };

    TEST_F(InstanceFieldOperatorTest, PositionFieldMapsTheSlotOriginToItsResult)
    {
        op.bind_position(0, constant_vector({ 4.0F, 5.0F, 6.0F }));

        op.process(0.0F);

        EXPECT_TRUE(near_vec(origin(0), { 4.0F, 5.0F, 6.0F }));
    }

    TEST_F(InstanceFieldOperatorTest, PositionFieldIsFedItsOwnPreviousResult)
    {
        op.bind_position(0, Kinesis::VectorField { .fn = [](const glm::vec3& p) { return p + glm::vec3(0.0F, 1.0F, 0.0F); } });

        op.process(0.0F);
        op.process(0.0F);
        op.process(0.0F);

        EXPECT_TRUE(near_vec(origin(0), { 0.0F, 4.0F, 2.0F }));
    }

    TEST_F(InstanceFieldOperatorTest, PositionFieldKeepsRotationAndScale)
    {
        slots[0].transform = glm::mat4(1.0F);
        slots[0].transform[0][0] = 2.0F;
        slots[0].transform[1][1] = 3.0F;
        slots[0].transform[2][2] = 4.0F;
        op.bind_position(0, constant_vector({ 1.0F, 1.0F, 1.0F }));

        op.process(0.0F);

        EXPECT_FLOAT_EQ(slots[0].transform[0][0], 2.0F);
        EXPECT_FLOAT_EQ(slots[0].transform[1][1], 3.0F);
        EXPECT_FLOAT_EQ(slots[0].transform[2][2], 4.0F);
    }

    TEST_F(InstanceFieldOperatorTest, TransformFieldRewritesTheWholeMatrix)
    {
        op.bind_transform(1, [](const glm::mat4& m) {
            glm::mat4 out = m;
            out[0][0] = 2.0F;
            return out;
        });

        op.process(0.0F);

        EXPECT_FLOAT_EQ(slots[1].transform[0][0], 2.0F);
        EXPECT_TRUE(near_vec(origin(1), { 10.0F, 1.0F, 2.0F }));
    }

    TEST_F(InstanceFieldOperatorTest, OnlyBoundSlotsAreTouchedAndMarkedDirty)
    {
        op.bind_position(0, constant_vector({ 4.0F, 5.0F, 6.0F }));

        op.process(0.0F);

        EXPECT_TRUE(slots[0].dirty);
        EXPECT_FALSE(slots[1].dirty);
        EXPECT_TRUE(near_vec(origin(1), { 10.0F, 1.0F, 2.0F }));
    }

    TEST_F(InstanceFieldOperatorTest, TransformBindingReplacesPositionBindingAndViceVersa)
    {
        op.bind_position(0, constant_vector({ 4.0F, 5.0F, 6.0F }));
        op.bind_transform(0, [](const glm::mat4& m) { return m; });
        op.process(0.0F);
        EXPECT_TRUE(near_vec(origin(0), { 0.0F, 1.0F, 2.0F }));

        op.bind_position(0, constant_vector({ 4.0F, 5.0F, 6.0F }));
        op.process(0.0F);
        EXPECT_TRUE(near_vec(origin(0), { 4.0F, 5.0F, 6.0F }));
    }

    TEST_F(InstanceFieldOperatorTest, UnbindStopsASlotAndUnbindAllStopsEverySlot)
    {
        op.bind_position(0, constant_vector({ 4.0F, 5.0F, 6.0F }));
        op.bind_position(1, constant_vector({ 7.0F, 8.0F, 9.0F }));

        op.unbind(0);
        op.process(0.0F);
        EXPECT_TRUE(near_vec(origin(0), { 0.0F, 1.0F, 2.0F }));
        EXPECT_TRUE(near_vec(origin(1), { 7.0F, 8.0F, 9.0F }));

        op.unbind_all();
        slots[1].transform = glm::mat4(1.0F);
        op.process(0.0F);
        EXPECT_TRUE(near_vec(origin(1), { 0.0F, 0.0F, 0.0F }));
    }

    TEST(InstanceFieldOperatorIdentityTest, WithoutSlotsProcessIsSafeAndTypeIsReported)
    {
        InstanceFieldOperator op;

        EXPECT_NO_THROW(op.process(0.0F));
        EXPECT_EQ(op.get_type_name(), "InstanceField");
        EXPECT_FALSE(op.has_gpu_executor());
        EXPECT_NO_THROW(op.set_gpu_executor(nullptr));
        EXPECT_FALSE(op.has_gpu_executor());
    }

} // namespace

} // namespace MayaFlux::Test
