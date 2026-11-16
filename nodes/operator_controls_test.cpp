#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Nodes/Generators/Polynomial.hpp"
#include "MayaFlux/Nodes/Network/ModalNetwork.hpp"
#include "MayaFlux/Nodes/Network/Operators/FieldOperator.hpp"
#include "MayaFlux/Nodes/Network/Operators/PathOperator.hpp"
#include "MayaFlux/Nodes/Network/Operators/PhysicsOperator.hpp"
#include "MayaFlux/Nodes/Network/Operators/TopologyOperator.hpp"
#include "MayaFlux/Nodes/Network/Operators/TerminationOperators.hpp"
#include "MayaFlux/Nodes/Network/Operators/WalkOperator.hpp"
#include "MayaFlux/Nodes/Network/ParticleNetwork.hpp"
#include "MayaFlux/Nodes/Network/PointCloudNetwork.hpp"
#include "MayaFlux/Nodes/Network/RelationNetwork.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

using Nodes::LineVertex;
using Nodes::PointVertex;
using Nodes::Generator::Polynomial;
using Nodes::Network::FieldMode;
using Nodes::Network::FieldOperator;
using Nodes::Network::ModalDampingOperator;
using Nodes::Network::ModalNetwork;
using Nodes::Network::ParticleNetwork;
using Nodes::Network::PathOperator;
using Nodes::Network::PhysicsOperator;
using Nodes::Network::DuctEnd;
using Nodes::Network::JunctionOperator;
using Nodes::Network::PointCloudNetwork;
using Nodes::Network::ReedOperator;
using Nodes::Network::RelationNetwork;
using Nodes::Network::TopologyOperator;
using Nodes::Network::WalkOperator;
using BoundsMode = PhysicsOperator::BoundsMode;
namespace Acoustics = Kinesis::Acoustics;

namespace {

    constexpr float k_eps = 1e-4F;

    std::shared_ptr<Polynomial> constant_source(double value)
    {
        auto source = std::make_shared<Polynomial>([value](double) { return value; });
        source->process_sample(0.0);
        return source;
    }

    std::vector<PointVertex> particles(uint32_t count)
    {
        std::vector<PointVertex> out;
        for (uint32_t i = 0; i < count; ++i) {
            PointVertex vertex;
            vertex.position = { static_cast<float>(i), 0.0F, 0.0F };
            out.push_back(vertex);
        }
        return out;
    }

    std::vector<LineVertex> line_points(uint32_t count)
    {
        std::vector<LineVertex> out;
        for (uint32_t i = 0; i < count; ++i) {
            LineVertex vertex;
            vertex.position = { static_cast<float>(i), static_cast<float>(i % 2), 0.0F };
            out.push_back(vertex);
        }
        return out;
    }

    std::shared_ptr<PointCloudNetwork> index_source(size_t count)
    {
        auto source = std::make_shared<PointCloudNetwork>(count);
        source->initialize();
        return source;
    }

    ::testing::AssertionResult near_vec(const glm::vec3& a, const glm::vec3& b)
    {
        if (std::abs(a.x - b.x) < k_eps && std::abs(a.y - b.y) < k_eps && std::abs(a.z - b.z) < k_eps) {
            return ::testing::AssertionSuccess();
        }
        return ::testing::AssertionFailure() << "(" << a.x << ", " << a.y << ", " << a.z << ") != ("
                                             << b.x << ", " << b.y << ", " << b.z << ")";
    }

    class PhysicsControlsTest : public ::testing::Test {
    protected:
        PhysicsOperator op;

        void SetUp() override
        {
            op.set_gravity(glm::vec3(0.0F));
            op.set_drag(0.0F);
            op.set_bounds_mode(BoundsMode::NONE);
        }

        [[nodiscard]] glm::vec3 velocity(size_t i) const { return op.extract_vertex_velocities()[i]; }
        [[nodiscard]] glm::vec3 position(size_t i) const { return op.extract_vertices()[i].position; }
    };

    std::vector<double> render(ModalNetwork& network, unsigned int samples)
    {
        network.process_batch(samples);
        return network.get_audio_buffer().value_or(std::vector<double> {});
    }

}

TEST_F(PhysicsControlsTest, ANumberGravityIsAppliedOnce)
{
    op.initialize(particles(1));

    op.set({ .gravity = glm::vec3(0.0F, -10.0F, 0.0F) });
    op.process(0.1F);
    EXPECT_TRUE(near_vec(velocity(0), { 0.0F, -1.0F, 0.0F }));

    op.set_gravity(glm::vec3(0.0F));
    op.process(0.1F);
    EXPECT_TRUE(near_vec(velocity(0), { 0.0F, -1.0F, 0.0F }));
}

TEST_F(PhysicsControlsTest, ANumberDragScalesTheVelocityEachStep)
{
    op.initialize(particles(1));
    op.apply_impulse(0, { 1.0F, 0.0F, 0.0F });

    op.set({ .drag = 0.5 });
    op.process(0.1F);

    EXPECT_TRUE(near_vec(velocity(0), { 0.5F, 0.0F, 0.0F }));
    EXPECT_NEAR(position(0).x, 0.05F, k_eps);
}

TEST_F(PhysicsControlsTest, AMemberLeftOutLeavesTheOthersAlone)
{
    op.set_gravity({ 0.0F, -10.0F, 0.0F });
    op.initialize(particles(1));

    op.set({ .drag = 0.0 });
    op.process(0.1F);

    EXPECT_TRUE(near_vec(velocity(0), { 0.0F, -1.0F, 0.0F }));
}

TEST_F(PhysicsControlsTest, TheSameNameThroughTheStringPathGivesTheSameResult)
{
    PhysicsOperator by_string;
    by_string.set_bounds_mode(BoundsMode::NONE);
    by_string.initialize(particles(2));
    by_string.set_parameter("gravity_y", -4.0);
    by_string.set_parameter("gravity_x", 2.0);
    by_string.set_parameter("drag", 0.25);

    op.initialize(particles(2));
    op.set({ .gravity = glm::vec3(2.0F, -4.0F, 0.0F), .drag = 0.25 });

    by_string.process(0.1F);
    op.process(0.1F);

    for (size_t i = 0; i < 2; ++i) {
        EXPECT_TRUE(near_vec(by_string.extract_vertex_velocities()[i], velocity(i)));
        EXPECT_TRUE(near_vec(by_string.extract_vertices()[i].position, position(i)));
    }
}

TEST_F(PhysicsControlsTest, AGravityVectorIsHeldAsOneGlmVector)
{
    op.initialize(particles(1));

    op.set({ .gravity = glm::vec3(1.0F, -4.0F, 2.0F) });
    op.process(0.1F);

    EXPECT_TRUE(near_vec(velocity(0), { 0.1F, -0.4F, 0.2F }));
    ASSERT_TRUE(op.get().gravity.has_value());
    EXPECT_TRUE(near_vec(op.get().gravity.value<glm::vec3>(), { 1.0F, -4.0F, 2.0F }));
}

TEST_F(PhysicsControlsTest, AVectorIsAppliedOnceAndALaterEditStays)
{
    op.initialize(particles(1));

    op.set({ .gravity = glm::vec3(0.0F, -10.0F, 0.0F) });
    op.process(0.1F);
    op.set_parameter("gravity_y", 0.0);
    op.process(0.1F);

    EXPECT_TRUE(near_vec(velocity(0), { 0.0F, -1.0F, 0.0F }));
}

TEST_F(PhysicsControlsTest, AnUnsetVectorLeavesGravityAlone)
{
    op.set_parameter("gravity_y", -10.0);
    op.initialize(particles(1));

    op.set({ .drag = 0.0 });
    op.process(0.1F);

    EXPECT_TRUE(near_vec(velocity(0), { 0.0F, -1.0F, 0.0F }));
}

TEST_F(PhysicsControlsTest, ASecondVectorReplacesTheFirst)
{
    op.initialize(particles(1));

    op.set({ .gravity = glm::vec3(0.0F, -10.0F, 0.0F) });
    op.set({ .gravity = glm::vec3(0.0F, 10.0F, 0.0F) });
    op.process(0.1F);

    EXPECT_TRUE(near_vec(velocity(0), { 0.0F, 1.0F, 0.0F }));
}

TEST_F(PhysicsControlsTest, AForceVectorPushesEveryParticleOnce)
{
    op.initialize(particles(3));

    op.set({ .force = glm::vec3(0.0F, 20.0F, 0.0F) });
    op.process(0.1F);
    op.process(0.1F);

    for (size_t i = 0; i < 3; ++i) {
        EXPECT_NEAR(velocity(i).y, 2.0F, k_eps) << "particle " << i;
    }
}

TEST_F(PhysicsControlsTest, AVectorSetFromAnotherThreadIsNeverSeenTorn)
{
    op.initialize(particles(1));
    constexpr int last = 4000;
    std::atomic<bool> done { false };

    std::thread writer([&] {
        for (int i = 1; i <= last; ++i) {
            const auto value = static_cast<float>(i);
            op.set({ .gravity = glm::vec3(value, value, value) });
        }
        done.store(true, std::memory_order_release);
    });

    bool whole = true;
    for (;;) {
        const bool finished = done.load(std::memory_order_acquire);
        op.process(0.0F);
        const auto gravity = op.get().gravity.value<glm::vec3>();
        whole = whole && gravity.x == gravity.y && gravity.y == gravity.z;
        if (finished) {
            break;
        }
    }
    writer.join();

    EXPECT_TRUE(whole);
    EXPECT_FLOAT_EQ(op.get().gravity.value<glm::vec3>().x, static_cast<float>(last));
}

TEST_F(PhysicsControlsTest, ANumberForceIsAnImpulseOnEveryParticleOnce)
{
    op.initialize(particles(3));

    op.set({ .force = glm::vec3(10.0F, 0.0F, 0.0F) });
    op.process(0.1F);
    for (size_t i = 0; i < 3; ++i) {
        EXPECT_NEAR(velocity(i).x, 1.0F, k_eps) << "particle " << i;
    }

    op.process(0.1F);
    for (size_t i = 0; i < 3; ++i) {
        EXPECT_NEAR(velocity(i).x, 1.0F, k_eps) << "particle " << i;
    }
}

TEST_F(PhysicsControlsTest, ANetworkMassSetsEachParticlesMassWithAFloor)
{
    op.initialize(particles(4));

    op.set({ .mass = index_source(4) });
    op.process(0.0F);
    op.apply_global_impulse({ 1.0F, 0.0F, 0.0F });

    const auto v = op.extract_vertex_velocities();
    EXPECT_NEAR(v[0].x, 10.0F, k_eps);
    EXPECT_NEAR(v[1].x, 1.0F, k_eps);
    EXPECT_NEAR(v[2].x, 0.5F, k_eps);
    EXPECT_NEAR(v[3].x, 1.0F / 3.0F, k_eps);
}

TEST_F(PhysicsControlsTest, ANumberMassSetsEveryParticle)
{
    op.initialize(particles(2));

    op.set({ .mass = 4.0 });
    op.process(0.0F);
    op.apply_global_impulse({ 1.0F, 0.0F, 0.0F });

    EXPECT_NEAR(velocity(0).x, 0.25F, k_eps);
    EXPECT_NEAR(velocity(1).x, 0.25F, k_eps);
}

TEST_F(PhysicsControlsTest, ASourceOfTheWrongSizeIsIgnored)
{
    op.initialize(particles(4));

    op.set({ .mass = index_source(2) });
    op.process(0.0F);
    op.apply_global_impulse({ 1.0F, 0.0F, 0.0F });

    EXPECT_NEAR(velocity(0).x, 1.0F, k_eps);
}

TEST_F(PhysicsControlsTest, GetReportsWhatTheOperatorHolds)
{
    op.set_drag(0.3F);
    op.set_gravity({ 1.0F, 2.0F, 3.0F });

    const auto controls = op.get();

    EXPECT_NEAR(controls.drag.value(), 0.3, 1e-6);
    ASSERT_TRUE(controls.gravity.has_value());
    EXPECT_TRUE(near_vec(controls.gravity.value<glm::vec3>(), { 1.0F, 2.0F, 3.0F }));
    EXPECT_FALSE(controls.force.has_value());
}

TEST_F(PhysicsControlsTest, ADragOutsideTheUnitIntervalIsClampedLikeTheString)
{
    op.initialize(particles(1));
    op.apply_impulse(0, { 1.0F, 0.0F, 0.0F });

    op.set({ .drag = 5.0 });
    op.process(0.1F);

    EXPECT_NEAR(velocity(0).x, 0.0F, k_eps);
}

TEST(PathControlsTest, ANumberThicknessReachesEveryPath)
{
    PathOperator op;
    op.initialize(line_points(4));

    op.set({ .thickness = 7.0 });
    op.process(0.0F);

    EXPECT_FLOAT_EQ(op.get_path(0)->get_path_thickness(), 7.0F);
}

TEST(PathControlsTest, ANodeThicknessIsAppliedEveryBlock)
{
    PathOperator op;
    op.initialize(line_points(4));

    op.set({ .thickness = constant_source(6.0) });
    op.process(0.0F);
    op.set_global_thickness(2.0F);
    op.process(0.0F);

    EXPECT_FLOAT_EQ(op.get_path(0)->get_path_thickness(), 6.0F);
}

TEST(PathControlsTest, ANumberIsAppliedOnceAndLaterEditsStay)
{
    PathOperator op;
    op.initialize(line_points(4));

    op.set({ .thickness = 7.0 });
    op.process(0.0F);
    op.set_global_thickness(2.0F);
    op.process(0.0F);

    EXPECT_FLOAT_EQ(op.get_path(0)->get_path_thickness(), 2.0F);
}

TEST(PathControlsTest, TheSameNamesThroughTheStringPathGiveTheSameVertices)
{
    PathOperator by_string;
    PathOperator by_control;
    by_string.initialize(line_points(5));
    by_control.initialize(line_points(5));

    by_string.set_parameter("tension", 0.2);
    by_string.set_parameter("samples_per_segment", 8.0);
    by_string.set_parameter("thickness", 5.0);
    by_control.set({ .tension = 0.2, .samples_per_segment = 8.0, .thickness = 5.0 });

    by_string.process(0.0F);
    by_control.process(0.0F);

    ASSERT_EQ(by_control.get_vertex_count(), by_string.get_vertex_count());
    const auto expected = by_string.get_vertex_data();
    const auto actual = by_control.get_vertex_data();
    ASSERT_EQ(actual.size(), expected.size());
    EXPECT_TRUE(std::equal(actual.begin(), actual.end(), expected.begin()));
}

TEST(TopologyControlsTest, TheSameNamesThroughTheStringPathGiveTheSameVertices)
{
    TopologyOperator by_string;
    TopologyOperator by_control;
    by_string.initialize(line_points(6));
    by_control.initialize(line_points(6));

    by_string.set_parameter("k_neighbors", 2.0);
    by_string.set_parameter("line_thickness", 3.0);
    by_control.set({ .k_neighbors = 2.0, .line_thickness = 3.0 });

    by_string.process(0.0F);
    by_control.process(0.0F);

    ASSERT_EQ(by_control.get_vertex_count(), by_string.get_vertex_count());
    const auto expected = by_string.get_vertex_data();
    const auto actual = by_control.get_vertex_data();
    ASSERT_EQ(actual.size(), expected.size());
    EXPECT_TRUE(std::equal(actual.begin(), actual.end(), expected.begin()));
}

TEST(TopologyControlsTest, ARadiusThroughTheStringPathAndTheControlAgree)
{
    TopologyOperator by_string { Kinesis::ProximityMode::RADIUS_THRESHOLD };
    TopologyOperator by_control { Kinesis::ProximityMode::RADIUS_THRESHOLD };
    by_string.initialize(line_points(6));
    by_control.initialize(line_points(6));

    by_string.set_parameter("connection_radius", 1.2);
    by_control.set({ .connection_radius = constant_source(1.2) });

    by_string.process(0.0F);
    by_control.process(0.0F);

    EXPECT_EQ(by_control.get_vertex_count(), by_string.get_vertex_count());
}

TEST(FieldControlsTest, AModeAboveHalfAccumulates)
{
    FieldOperator op;
    ASSERT_EQ(op.get_mode(), FieldMode::ABSOLUTE);

    op.set({ .mode = 1.0 });
    op.process(0.0F);

    EXPECT_EQ(op.get_mode(), FieldMode::ACCUMULATE);
}

TEST(FieldControlsTest, AModeOfHalfOrBelowIsAbsolute)
{
    FieldOperator op(FieldMode::ACCUMULATE);

    op.set({ .mode = 0.5 });
    op.process(0.0F);

    EXPECT_EQ(op.get_mode(), FieldMode::ABSOLUTE);
}

TEST(FieldControlsTest, ANodeModeFollowsTheNode)
{
    FieldOperator op;

    op.set({ .mode = constant_source(1.0) });
    op.process(0.0F);

    EXPECT_EQ(op.get_mode(), FieldMode::ACCUMULATE);
}

TEST(FieldControlsTest, GetReportsTheModeTheOperatorHolds)
{
    FieldOperator op(FieldMode::ACCUMULATE);

    EXPECT_DOUBLE_EQ(op.get().mode.value(), 1.0);

    op.set_mode(FieldMode::ABSOLUTE);
    EXPECT_DOUBLE_EQ(op.get().mode.value(), 0.0);
}

TEST(FieldControlsTest, TheStringModeStillWorks)
{
    FieldOperator op;

    op.set_parameter("mode", 1.0);

    EXPECT_EQ(op.get_mode(), FieldMode::ACCUMULATE);
}

TEST(ModalDampingControlsTest, ANumberFieldSetsThatFieldOnEveryMode)
{
    auto network = std::make_shared<ModalNetwork>(2, 440.0);
    auto damping = network->add<ModalDampingOperator>();

    damping->set({ .quiet_seconds = 1.0, .loud_seconds = 0.25, .reference_rms = 0.05, .smoothing_seconds = 0.0 });
    network->excite_mode(0);
    network->excite_mode(1);
    render(*network, 256);
    render(*network, 256);

    EXPECT_EQ(network->get_mode_decay_time(0), 0.25);
    EXPECT_EQ(network->get_mode_decay_time(1), 0.25);
}

TEST(ModalDampingControlsTest, AFieldLeavesTheOtherFieldsOfEachModeAlone)
{
    auto network = std::make_shared<ModalNetwork>(2, 440.0);
    auto damping = network->add<ModalDampingOperator>();
    damping->set({ .quiet_seconds = 1.0, .loud_seconds = 0.5, .reference_rms = 0.05, .smoothing_seconds = 0.0 });
    damping->set_mode(1, { .quiet_seconds = 3.0, .loud_seconds = 0.3 });

    damping->set({ .reference_rms = 0.1 });
    render(*network, 256);

    EXPECT_EQ(network->get_mode_decay_time(0), 1.0);
    EXPECT_EQ(network->get_mode_decay_time(1), 3.0);
}

TEST(ModalDampingControlsTest, AnInvalidValueIsRefusedAndTheResponseKept)
{
    auto network = std::make_shared<ModalNetwork>(1, 440.0);
    auto damping = network->add<ModalDampingOperator>();

    damping->set({ .quiet_seconds = 0.0, .reference_rms = -1.0 });
    render(*network, 64);

    EXPECT_DOUBLE_EQ(damping->get().quiet_seconds.value(), 1.0);
    EXPECT_DOUBLE_EQ(damping->get().reference_rms.value(), 0.1);
}

TEST(ModalDampingControlsTest, ANodeFieldIsAppliedEveryBlock)
{
    auto network = std::make_shared<ModalNetwork>(1, 440.0);
    auto damping = network->add<ModalDampingOperator>();
    damping->set({ .quiet_seconds = 1.0, .loud_seconds = constant_source(0.2), .reference_rms = 0.05, .smoothing_seconds = 0.0 });

    network->excite_mode(0);
    render(*network, 256);
    render(*network, 256);
    ASSERT_EQ(network->get_mode_decay_time(0), 0.2);

    damping->set_mode(0, { .loud_seconds = 0.9 });
    render(*network, 256);

    EXPECT_DOUBLE_EQ(damping->get().loud_seconds.value(), 0.2);
    EXPECT_EQ(network->get_mode_decay_time(0), 0.2);
}

TEST(ModalDampingControlsTest, AFreshOperatorHoldsTheDefaults)
{
    auto network = std::make_shared<ModalNetwork>(3, 440.0);
    auto damping = network->add<ModalDampingOperator>();

    const auto controls = damping->get();

    EXPECT_DOUBLE_EQ(controls.quiet_seconds.value(), 1.0);
    EXPECT_DOUBLE_EQ(controls.loud_seconds.value(), 0.2);
    EXPECT_DOUBLE_EQ(controls.reference_rms.value(), 0.1);
    EXPECT_DOUBLE_EQ(controls.smoothing_seconds.value(), 0.05);
    EXPECT_EQ(damping->query_state("mode_count"), 3.0);
}

TEST(ModalDampingControlsTest, SetModeChangesOnlyTheFieldsGiven)
{
    auto network = std::make_shared<ModalNetwork>(2, 440.0);
    auto damping = network->add<ModalDampingOperator>();
    damping->set({ .quiet_seconds = 2.0, .smoothing_seconds = 0.0 });

    damping->set_mode(1, { .quiet_seconds = 5.0 });
    render(*network, 256);

    EXPECT_EQ(network->get_mode_decay_time(0), 2.0);
    EXPECT_EQ(network->get_mode_decay_time(1), 5.0);
}

TEST(ModalDampingControlsTest, SetModeAppliesAPendingSetFirstSoCallsTakeEffectInOrder)
{
    auto network = std::make_shared<ModalNetwork>(2, 440.0);
    auto damping = network->add<ModalDampingOperator>();

    damping->set({ .quiet_seconds = 9.0, .smoothing_seconds = 0.0 });
    damping->set_mode(1, { .quiet_seconds = 4.0 });
    render(*network, 256);

    EXPECT_EQ(network->get_mode_decay_time(0), 9.0);
    EXPECT_EQ(network->get_mode_decay_time(1), 4.0);
}

TEST(ModalDampingControlsTest, AModeIndexPastTheLastIsIgnored)
{
    auto network = std::make_shared<ModalNetwork>(2, 440.0);
    auto damping = network->add<ModalDampingOperator>();

    damping->set_mode(2, { .quiet_seconds = 7.0 });
    damping->set_mode(99, { .quiet_seconds = 7.0 });
    render(*network, 64);

    EXPECT_EQ(damping->query_state("mode_count"), 2.0);
    EXPECT_DOUBLE_EQ(damping->get().quiet_seconds.value(), 1.0);
}

TEST(ModalDampingControlsTest, SetModeRefusesAnInvalidValueAndKeepsTheRest)
{
    auto network = std::make_shared<ModalNetwork>(1, 440.0);
    auto damping = network->add<ModalDampingOperator>();
    damping->set({ .smoothing_seconds = 0.0 });

    damping->set_mode(0, { .quiet_seconds = 0.0, .loud_seconds = 0.4 });
    network->excite_mode(0);
    render(*network, 256);
    render(*network, 256);

    EXPECT_EQ(network->get_mode_decay_time(0), 0.4);
}

TEST(ModalDampingControlsTest, ASourceOnASingleModeIsIgnored)
{
    auto network = std::make_shared<ModalNetwork>(1, 440.0);
    auto damping = network->add<ModalDampingOperator>();
    damping->set({ .smoothing_seconds = 0.0 });

    damping->set_mode(0, { .quiet_seconds = constant_source(6.0) });
    render(*network, 64);

    EXPECT_EQ(network->get_mode_decay_time(0), 1.0);
}

namespace {

    struct PhysicsScalarCase {
        const char* name;
        Nodes::Network::Control& (*member)(Nodes::Network::PhysicsControls&);
        double requested;
        double expected;
    };

    const std::array<PhysicsScalarCase, 9> kPhysicsScalars { {
        { .name = "Drag", .member = [](Nodes::Network::PhysicsControls& c) -> Nodes::Network::Control& { return c.drag; }, .requested = 0.2, .expected = 0.2 },
        { .name = "DragClampedLow", .member = [](Nodes::Network::PhysicsControls& c) -> Nodes::Network::Control& { return c.drag; }, .requested = -1.0, .expected = 0.0 },
        { .name = "DragClampedHigh", .member = [](Nodes::Network::PhysicsControls& c) -> Nodes::Network::Control& { return c.drag; }, .requested = 4.0, .expected = 1.0 },
        { .name = "InteractionRadius", .member = [](Nodes::Network::PhysicsControls& c) -> Nodes::Network::Control& { return c.interaction_radius; }, .requested = 2.5, .expected = 2.5 },
        { .name = "SpringStiffness", .member = [](Nodes::Network::PhysicsControls& c) -> Nodes::Network::Control& { return c.spring_stiffness; }, .requested = 3.0, .expected = 3.0 },
        { .name = "RepulsionStrength", .member = [](Nodes::Network::PhysicsControls& c) -> Nodes::Network::Control& { return c.repulsion_strength; }, .requested = 0.7, .expected = 0.7 },
        { .name = "SpatialInteractionsOn", .member = [](Nodes::Network::PhysicsControls& c) -> Nodes::Network::Control& { return c.spatial_interactions; }, .requested = 1.0, .expected = 1.0 },
        { .name = "SpatialInteractionsOff", .member = [](Nodes::Network::PhysicsControls& c) -> Nodes::Network::Control& { return c.spatial_interactions; }, .requested = 0.0, .expected = 0.0 },
        { .name = "AttractionStrength", .member = [](Nodes::Network::PhysicsControls& c) -> Nodes::Network::Control& { return c.attraction_strength; }, .requested = 2.0, .expected = 2.0 },
    } };

    class PhysicsScalarControlTest : public ::testing::TestWithParam<PhysicsScalarCase> { };

}

TEST_P(PhysicsScalarControlTest, TheValueReachesTheOperatorAndReadsBack)
{
    PhysicsOperator op;
    op.initialize(particles(2));

    Nodes::Network::PhysicsControls incoming;
    GetParam().member(incoming) = Nodes::Network::Control(GetParam().requested);
    op.set(incoming);
    op.process(0.0F);

    auto controls = op.get();
    EXPECT_NEAR(GetParam().member(controls).value(), GetParam().expected, 1e-5);
}

INSTANTIATE_TEST_SUITE_P(
    EveryScalar,
    PhysicsScalarControlTest,
    ::testing::ValuesIn(kPhysicsScalars),
    [](const ::testing::TestParamInfo<PhysicsScalarCase>& info) { return std::string(info.param.name); });

TEST(PhysicsControlsAppliedTest, ATurbulenceControlReachesTheOperator)
{
    PhysicsOperator op;
    op.initialize(particles(2));

    op.set({ .turbulence = 0.4 });
    op.process(0.0F);

    EXPECT_NEAR(op.get().turbulence.value(), 0.4, 1e-5);
}

TEST(PhysicsControlsAppliedTest, APointSizeControlResizesEveryPoint)
{
    PhysicsOperator op;
    op.initialize(particles(3));

    op.set({ .point_size = 9.0 });
    op.process(0.0F);

    for (const auto& vertex : op.extract_vertices()) {
        EXPECT_FLOAT_EQ(vertex.size, 9.0F);
    }
}

TEST(PhysicsControlsAppliedTest, ANodeSourceStaysSourcedInGet)
{
    PhysicsOperator op;
    op.initialize(particles(1));

    op.set({ .drag = constant_source(0.3) });
    op.process(0.0F);

    EXPECT_TRUE(op.get().drag.sourced());
}

TEST(PhysicsControlsAppliedTest, SetBeforeAnyParticlesStillTakesEffect)
{
    PhysicsOperator op;

    op.set({ .drag = 0.4 });
    op.process(0.0F);
    op.initialize(particles(1));

    EXPECT_NEAR(op.get().drag.value(), 0.4, 1e-5);
}

TEST(NetworkAddTest, AddAppendsHostFreeOperatorsToAnyNetworkInOrder)
{
    auto net = std::make_shared<ParticleNetwork>(8);
    auto field = net->add<FieldOperator>(FieldMode::ACCUMULATE);
    auto physics = net->add<PhysicsOperator>();

    ASSERT_EQ(net->get_operator_chain()->size(), 2U);
    EXPECT_EQ(net->get_operator_chain()->get(0), field);
    EXPECT_EQ(net->get_operator_chain()->get(1), physics);
    physics->set({ .drag = 0.25 });
    EXPECT_EQ(net->get_operator_chain()->find<PhysicsOperator>(), physics);
}

TEST(PhysicsControlsAppliedTest, TheControlsWorkThroughAParticleNetworksOperator)
{
    ParticleNetwork net(8);
    net.process_batch(1);

    auto* physics = net.as_operator<PhysicsOperator>();
    ASSERT_NE(physics, nullptr);

    physics->set({ .drag = 0.2, .point_size = 7.0 });
    net.process_batch(1);

    EXPECT_NEAR(physics->get().drag.value(), 0.2, 1e-5);
    EXPECT_NEAR(physics->get().point_size.value(), 7.0, 1e-5);
}

TEST(PhysicsControlsAppliedTest, ANetworkStringMappingAndATypedControlCanCoexist)
{
    ParticleNetwork net(8);
    net.process_batch(1);
    auto* physics = net.as_operator<PhysicsOperator>();
    ASSERT_NE(physics, nullptr);

    net.map_parameter("drag", constant_source(0.5));
    physics->set({ .point_size = 6.0 });
    net.process_batch(1);

    EXPECT_NEAR(physics->get().drag.value(), 0.5, 1e-5);
    EXPECT_NEAR(physics->get().point_size.value(), 6.0, 1e-5);
}

TEST(PathControlsTest, MoreSamplesPerSegmentGiveMoreVertices)
{
    PathOperator few;
    PathOperator many;
    few.initialize(line_points(5));
    many.initialize(line_points(5));

    few.set({ .samples_per_segment = 4.0 });
    many.set({ .samples_per_segment = 16.0 });
    for (int block = 0; block < 2; ++block) {
        few.process(0.0F);
        many.process(0.0F);
    }

    EXPECT_GT(many.get_vertex_count(), few.get_vertex_count());
}

TEST(PathControlsTest, AMemberLeftOutLeavesTheThicknessAlone)
{
    PathOperator op;
    op.initialize(line_points(4));
    op.set_global_thickness(3.0F);

    op.set({ .tension = 0.1 });
    op.process(0.0F);

    EXPECT_FLOAT_EQ(op.get_path(0)->get_path_thickness(), 3.0F);
}

TEST(TopologyControlsTest, MoreNeighboursGiveMoreConnections)
{
    TopologyOperator one;
    TopologyOperator three;
    one.initialize(line_points(8));
    three.initialize(line_points(8));

    one.set({ .k_neighbors = 1.0 });
    three.set({ .k_neighbors = 3.0 });
    for (int block = 0; block < 2; ++block) {
        one.process(0.0F);
        three.process(0.0F);
    }

    EXPECT_GT(three.get_vertex_count(), one.get_vertex_count());
}

TEST(TopologyControlsTest, ANumberIsAppliedOnceAndTheNodeEveryBlock)
{
    TopologyOperator once;
    TopologyOperator every;
    once.initialize(line_points(8));
    every.initialize(line_points(8));

    once.set({ .k_neighbors = 3.0 });
    every.set({ .k_neighbors = constant_source(3.0) });
    once.process(0.0F);
    every.process(0.0F);
    once.set_parameter("k_neighbors", 1.0);
    every.set_parameter("k_neighbors", 1.0);
    once.process(0.0F);
    every.process(0.0F);

    EXPECT_LT(once.get_vertex_count(), every.get_vertex_count());
}

TEST_F(PhysicsControlsTest, ANetworkColorColorsEachParticle)
{
    op.initialize(particles(3));

    op.set({ .color = index_source(3) });
    op.process(0.0F);

    const auto v = op.extract_vertices();
    EXPECT_NEAR(v[0].color.r, 0.0F, k_eps);
    EXPECT_NEAR(v[0].color.b, 1.0F, k_eps);
    EXPECT_NEAR(v[1].color.r, 1.0F, k_eps);
    EXPECT_NEAR(v[1].color.b, 0.0F, k_eps);
    EXPECT_NEAR(v[2].color.r, 1.0F, k_eps);
    EXPECT_NEAR(v[0].color.g, 0.5F, k_eps);
}

TEST_F(PhysicsControlsTest, ANetworkSizeScalesAndClampsParticleSize)
{
    op.initialize(particles(7));

    op.set({ .size = index_source(7) });
    op.process(0.0F);

    const auto v = op.extract_vertices();
    EXPECT_FLOAT_EQ(v[0].size, 1.0F);
    EXPECT_FLOAT_EQ(v[2].size, 20.0F);
    EXPECT_FLOAT_EQ(v[6].size, 50.0F);
}

TEST_F(PhysicsControlsTest, ANumberColorColorsEveryParticleTheSame)
{
    op.initialize(particles(3));

    op.set({ .color = 1.0 });
    op.process(0.0F);

    for (const auto& vertex : op.extract_vertices()) {
        EXPECT_NEAR(vertex.color.r, 1.0F, k_eps);
        EXPECT_NEAR(vertex.color.b, 0.0F, k_eps);
    }
}

TEST_F(PhysicsControlsTest, AColorOrSizeSourceOfTheWrongSizeIsIgnored)
{
    op.initialize(particles(4));

    op.set({ .color = index_source(2), .size = index_source(2) });
    op.process(0.0F);

    const auto v = op.extract_vertices();
    EXPECT_NEAR(v[0].color.r, 1.0F, k_eps);
    EXPECT_NEAR(v[0].color.g, 1.0F, k_eps);
    EXPECT_NEAR(v[0].color.b, 1.0F, k_eps);
}

TEST_F(PhysicsControlsTest, TheStringColorAndTheControlColorAgree)
{
    PhysicsOperator by_string;
    by_string.initialize(particles(3));
    by_string.apply_one_to_one("color", index_source(3));

    op.initialize(particles(3));
    op.set({ .color = index_source(3) });
    op.process(0.0F);

    for (size_t i = 0; i < 3; ++i) {
        EXPECT_NEAR(by_string.extract_vertices()[i].color.r, op.extract_vertices()[i].color.r, k_eps);
        EXPECT_NEAR(by_string.extract_vertices()[i].color.b, op.extract_vertices()[i].color.b, k_eps);
    }
}

namespace {

    Acoustics::DuctSpec duct_spec() { return { .length = 0.6, .sample_rate = 48000.0 }; }

    Acoustics::DuctGrid duct_grid()
    {
        return Acoustics::make_duct_grid(duct_spec(), [](double) { return 0.0075; }).value();
    }

}

TEST(ReedControlsTest, NumbersReachTheReedWhenItProcesses)
{
    ReedOperator reed;

    reed.set({ .pressure = 900.0, .closing_pressure = 2500.0, .opening = 3.0e-4, .width = 0.01 });
    reed.process(1.0F);

    EXPECT_DOUBLE_EQ(reed.query_state("pressure").value(), 900.0);
    EXPECT_DOUBLE_EQ(reed.query_state("closing_pressure").value(), 2500.0);
    EXPECT_DOUBLE_EQ(reed.query_state("opening").value(), 3.0e-4);
    EXPECT_DOUBLE_EQ(reed.query_state("width").value(), 0.01);
}

TEST(ReedControlsTest, EveryBreathNameReachesTheReed)
{
    ReedOperator reed;

    reed.set({ .smoothing = 0.01, .resonance = 2500.0, .reed_damping = 8000.0, .noise = 0.02, .noise_cutoff = 3500.0, .reed_area = 1.46e-4, .lip = 0.5 });
    reed.process(1.0F);

    EXPECT_DOUBLE_EQ(reed.query_state("smoothing").value(), 0.01);
    EXPECT_DOUBLE_EQ(reed.query_state("resonance").value(), 2500.0);
    EXPECT_DOUBLE_EQ(reed.query_state("reed_damping").value(), 8000.0);
    EXPECT_DOUBLE_EQ(reed.query_state("noise").value(), 0.02);
    EXPECT_DOUBLE_EQ(reed.query_state("noise_cutoff").value(), 3500.0);
    EXPECT_DOUBLE_EQ(reed.query_state("reed_area").value(), 1.46e-4);
    EXPECT_DOUBLE_EQ(reed.query_state("lip").value(), 0.5);
}

TEST(ReedControlsTest, ValuesThatTheStringPathRefusesAreRefusedHere)
{
    ReedOperator reed(0.012, 4.0e-4, 3000.0);

    reed.set({ .closing_pressure = -5.0, .opening = std::numeric_limits<double>::quiet_NaN(), .width = 0.0 });
    reed.process(1.0F);

    EXPECT_DOUBLE_EQ(reed.query_state("closing_pressure").value(), 3000.0);
    EXPECT_DOUBLE_EQ(reed.query_state("opening").value(), 4.0e-4);
    EXPECT_DOUBLE_EQ(reed.query_state("width").value(), 0.012);
}

TEST(ReedControlsTest, TheLipIsClampedAndTheNonNegativeOnesFloorAtZero)
{
    ReedOperator reed;

    reed.set({ .smoothing = -1.0, .noise = -0.5, .lip = 3.0 });
    reed.process(1.0F);

    EXPECT_DOUBLE_EQ(reed.query_state("lip").value(), 1.0);
    EXPECT_DOUBLE_EQ(reed.query_state("noise").value(), 0.0);
    EXPECT_DOUBLE_EQ(reed.query_state("smoothing").value(), 0.0);
}

TEST(ReedControlsTest, ANumberIsAppliedOnceAndALaterEditStays)
{
    ReedOperator reed;

    reed.set({ .pressure = 900.0 });
    reed.process(1.0F);
    reed.set_parameter("pressure", 400.0);
    reed.process(1.0F);

    EXPECT_DOUBLE_EQ(reed.query_state("pressure").value(), 400.0);
}

TEST(ReedControlsTest, ANodeIsAppliedEveryBlock)
{
    ReedOperator reed;

    reed.set({ .pressure = constant_source(1500.0) });
    reed.process(1.0F);
    reed.set_parameter("pressure", 400.0);
    reed.process(1.0F);

    EXPECT_DOUBLE_EQ(reed.query_state("pressure").value(), 1500.0);
}

TEST(ReedControlsTest, GetReportsTheConstructorValues)
{
    const ReedOperator reed(0.008, 2.0e-4, 3000.0);

    const auto controls = reed.get();

    EXPECT_DOUBLE_EQ(controls.width.value(), 0.008);
    EXPECT_DOUBLE_EQ(controls.opening.value(), 2.0e-4);
    EXPECT_DOUBLE_EQ(controls.closing_pressure.value(), 3000.0);
    EXPECT_FALSE(controls.pressure.sourced());
}

TEST(ReedControlsTest, TheTypedAndTheStringReedGiveTheSameCoupling)
{
    ReedOperator by_string(0.008, 2.0e-4, 3000.0);
    ReedOperator by_control(0.008, 2.0e-4, 3000.0);
    by_string.set_parameter("pressure", 1500.0);
    by_string.set_parameter("resonance", 2200.0);
    by_string.set_parameter("reed_area", 1.46e-4);
    by_string.set_parameter("noise", 0.02);
    by_control.set({ .pressure = 1500.0, .resonance = 2200.0, .noise = 0.02, .reed_area = 1.46e-4 });

    const auto grid = duct_grid();
    by_string.prepare(grid, duct_spec(), DuctEnd::MOUTH);
    by_control.prepare(grid, duct_spec(), DuctEnd::MOUTH);

    for (int sample = 0; sample < 64; ++sample) {
        const auto expected = by_string.couple(0.0, 0.0);
        const auto actual = by_control.couple(0.0, 0.0);
        EXPECT_DOUBLE_EQ(actual.first, expected.first) << "sample " << sample;
        EXPECT_DOUBLE_EQ(actual.second, expected.second) << "sample " << sample;
    }
}

TEST(ReedControlsTest, ANumberSetFromAnotherThreadIsNeverLostWhileTheReedProcesses)
{
    ReedOperator reed;
    constexpr int last = 4000;
    std::atomic<bool> done { false };

    std::thread writer([&] {
        for (int i = 1; i <= last; ++i) {
            reed.set({ .pressure = static_cast<double>(i) });
        }
        done.store(true, std::memory_order_release);
    });

    double previous = 0.0;
    bool ordered = true;
    for (;;) {
        const bool finished = done.load(std::memory_order_acquire);
        reed.process(1.0F);
        const double seen = reed.query_state("pressure").value();
        ordered = ordered && seen >= previous;
        previous = seen;
        if (finished) {
            break;
        }
    }
    writer.join();

    EXPECT_TRUE(ordered);
    EXPECT_DOUBLE_EQ(previous, static_cast<double>(last));
}

TEST(JunctionControlsTest, NumbersReachTheJunctionWhenItProcesses)
{
    JunctionOperator junction(
        [](double d) -> Acoustics::FlowSample { return { 1.0e-6 * d, 1.0e-6 }; }, 1.0, 100.0);

    junction.set({ .driving_pressure = 500.0, .maximum_flow = 2.0 });
    junction.process(1.0F);

    EXPECT_DOUBLE_EQ(junction.query_state("driving_pressure").value(), 500.0);
    EXPECT_DOUBLE_EQ(junction.query_state("maximum_flow").value(), 2.0);
}

TEST(JunctionControlsTest, ANonPositiveMaximumFlowIsRefusedLikeTheString)
{
    JunctionOperator junction(
        [](double d) -> Acoustics::FlowSample { return { 1.0e-6 * d, 1.0e-6 }; }, 1.0, 100.0);

    junction.set({ .maximum_flow = -3.0 });
    junction.process(1.0F);

    EXPECT_DOUBLE_EQ(junction.query_state("maximum_flow").value(), 1.0);
}

TEST(JunctionControlsTest, ANodeDrivingPressureIsAppliedEveryBlock)
{
    JunctionOperator junction(
        [](double d) -> Acoustics::FlowSample { return { 1.0e-6 * d, 1.0e-6 }; }, 1.0, 100.0);

    junction.set({ .driving_pressure = constant_source(800.0) });
    junction.process(1.0F);
    junction.set_parameter("driving_pressure", 50.0);
    junction.process(1.0F);

    EXPECT_DOUBLE_EQ(junction.query_state("driving_pressure").value(), 800.0);
}

TEST(JunctionControlsTest, GetReportsTheConstructorValuesAndTheStringStillWorks)
{
    JunctionOperator junction(
        [](double d) -> Acoustics::FlowSample { return { 1.0e-6 * d, 1.0e-6 }; }, 1.5, 250.0);

    EXPECT_DOUBLE_EQ(junction.get().driving_pressure.value(), 250.0);
    EXPECT_DOUBLE_EQ(junction.get().maximum_flow.value(), 1.5);

    junction.set_parameter("driving_pressure", 10.0);
    EXPECT_DOUBLE_EQ(junction.query_state("driving_pressure").value(), 10.0);
}

TEST(JunctionControlsTest, TheTypedAndTheStringJunctionGiveTheSameCoupling)
{
    const auto law = [](double d) -> Acoustics::FlowSample { return { 2.0e-6 * d, 2.0e-6 }; };
    JunctionOperator by_string(law, 1.0);
    JunctionOperator by_control(law, 1.0);
    by_string.set_parameter("driving_pressure", 300.0);
    by_control.set({ .driving_pressure = 300.0 });

    const auto grid = duct_grid();
    by_string.prepare(grid, duct_spec(), DuctEnd::MOUTH);
    by_control.prepare(grid, duct_spec(), DuctEnd::MOUTH);
    by_control.process(1.0F);

    for (int sample = 0; sample < 32; ++sample) {
        const auto expected = by_string.couple(10.0, 1.0e-5);
        const auto actual = by_control.couple(10.0, 1.0e-5);
        EXPECT_DOUBLE_EQ(actual.first, expected.first) << "sample " << sample;
        EXPECT_DOUBLE_EQ(actual.second, expected.second) << "sample " << sample;
    }
}

namespace {

    struct WalkRig {
        RelationNetwork network;
        std::shared_ptr<WalkOperator> walk;

        WalkRig()
        {
            network.add_slots({ { .name = "a", .node = constant_source(0.0) },
                { .name = "b", .node = constant_source(0.0) },
                { .name = "c", .node = constant_source(0.0) } });
            walk = network.add<WalkOperator>(std::make_shared<Kinesis::Stochastic::Weights>(3, 3));
        }
    };

}

TEST(WalkControlsTest, ANumberCurrentJumpsTheWalkWhenItProcesses)
{
    WalkRig rig;

    rig.walk->set({ .current = 2.0 });
    rig.walk->process(1.0F);

    EXPECT_EQ(rig.walk->current(), 2U);
}

TEST(WalkControlsTest, AnIndexOutOfRangeLeavesTheWalkInPlace)
{
    WalkRig rig;
    rig.walk->process(1.0F);

    rig.walk->set({ .current = 9.0 });
    rig.walk->process(1.0F);

    EXPECT_EQ(rig.walk->current(), 0U);
}

TEST(WalkControlsTest, ANodeCurrentFollowsTheNode)
{
    WalkRig rig;

    rig.walk->set({ .current = constant_source(1.0) });
    rig.walk->process(1.0F);

    EXPECT_EQ(rig.walk->current(), 1U);
}

TEST(WalkControlsTest, GetReportsTheSlotTheWalkIsOn)
{
    WalkRig rig;
    rig.walk->process(1.0F);
    rig.walk->jump(2);

    EXPECT_DOUBLE_EQ(rig.walk->get().current.value(), 2.0);
}

TEST(WalkControlsTest, TheStringCurrentStillWorks)
{
    WalkRig rig;

    rig.walk->set_parameter("current", 1.0);

    EXPECT_EQ(rig.walk->current(), 1U);
}

TEST(ModalDampingControlsTest, AStringNameIsTheFieldName)
{
    auto network = std::make_shared<ModalNetwork>(2, 440.0);
    auto damping = network->add<ModalDampingOperator>();

    damping->set_parameter("loud_seconds", 0.3);
    render(*network, 64);

    EXPECT_DOUBLE_EQ(damping->get().loud_seconds.value(), 0.3);
    EXPECT_EQ(damping->query_state("loud_seconds"), 0.3);
}

TEST(ModalDampingControlsTest, AStringAndATypedSetMeetInTheSameControlSoTheLaterWins)
{
    auto network = std::make_shared<ModalNetwork>(1, 440.0);
    auto damping = network->add<ModalDampingOperator>();

    damping->set({ .loud_seconds = 0.3 });
    damping->set_parameter("loud_seconds", 0.6);
    render(*network, 64);
    EXPECT_DOUBLE_EQ(damping->get().loud_seconds.value(), 0.6);

    damping->set_parameter("loud_seconds", 0.1);
    damping->set({ .loud_seconds = 0.5 });
    render(*network, 64);
    EXPECT_DOUBLE_EQ(damping->get().loud_seconds.value(), 0.5);
}

TEST(ModalDampingControlsTest, AnUnknownStringNameIsIgnored)
{
    auto network = std::make_shared<ModalNetwork>(1, 440.0);
    auto damping = network->add<ModalDampingOperator>();

    damping->set_parameter("loud", 1.0);
    damping->set_parameter("quiet_decay", 5.0);
    render(*network, 64);

    EXPECT_DOUBLE_EQ(damping->get().quiet_seconds.value(), 1.0);
    EXPECT_DOUBLE_EQ(damping->get().loud_seconds.value(), 0.2);
    EXPECT_FALSE(damping->query_state("loud").has_value());
    EXPECT_FALSE(damping->query_state("quiet_decay").has_value());
}

TEST(ModalDampingControlsTest, AStringValueThatWouldBeInvalidIsRefused)
{
    auto network = std::make_shared<ModalNetwork>(1, 440.0);
    auto damping = network->add<ModalDampingOperator>();

    damping->set_parameter("reference_rms", 0.0);
    damping->set_parameter("quiet_seconds", -2.0);
    render(*network, 64);

    EXPECT_DOUBLE_EQ(damping->get().reference_rms.value(), 0.1);
    EXPECT_DOUBLE_EQ(damping->get().quiet_seconds.value(), 1.0);
}

TEST(ModalDampingControlsTest, QueryStateReportsWhatTheOperatorHoldsNotWhatIsPending)
{
    auto network = std::make_shared<ModalNetwork>(1, 440.0);
    auto damping = network->add<ModalDampingOperator>();

    damping->set({ .quiet_seconds = 2.0 });
    EXPECT_EQ(damping->query_state("quiet_seconds"), 1.0);

    render(*network, 64);
    EXPECT_EQ(damping->query_state("quiet_seconds"), 2.0);
    EXPECT_EQ(damping->query_state("mode_count"), 1.0);
}

TEST(ModalDampingControlsTest, ASetValueReadsBackThroughGet)
{
    auto network = std::make_shared<ModalNetwork>(1, 440.0);
    auto damping = network->add<ModalDampingOperator>();

    damping->set({ .smoothing_seconds = 0.4 });
    render(*network, 64);

    EXPECT_DOUBLE_EQ(damping->get().smoothing_seconds.value(), 0.4);
}

}
