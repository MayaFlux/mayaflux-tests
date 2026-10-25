#include "gtest/gtest.h"

#include "MayaFlux/Nodes/Generators/Impulse.hpp"
#include "MayaFlux/Nodes/NodeGraphManager.hpp"
#include "MayaFlux/Nodes/Network/Operators/FieldOperator.hpp"
#include "MayaFlux/Nodes/Network/Operators/PathOperator.hpp"
#include "MayaFlux/Nodes/Network/Operators/PhysicsOperator.hpp"
#include "MayaFlux/Nodes/Network/Operators/TopologyOperator.hpp"
#include "MayaFlux/Nodes/Network/ParticleNetwork.hpp"
#include "MayaFlux/Nodes/Network/PointCloudNetwork.hpp"

namespace MayaFlux::Test {

namespace {

    using Nodes::PointVertex;
    using Nodes::Generator::Impulse;
    using Nodes::Network::FieldOperator;
    using Nodes::Network::MappingMode;
    using Nodes::Network::NodeNetwork;
    using Nodes::Network::ParticleNetwork;
    using Nodes::Network::PathOperator;
    using Nodes::Network::PhysicsOperator;
    using Nodes::Network::PointCloudNetwork;
    using Nodes::Network::Topology;
    using Nodes::Network::TopologyOperator;
    using BoundsMode = PhysicsOperator::BoundsMode;

    constexpr float k_eps = 1e-4F;

    std::vector<PointVertex> particles(uint32_t count, float spacing = 1.0F, float start = 0.0F)
    {
        std::vector<PointVertex> out;
        out.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            PointVertex v;
            v.position = { start + spacing * static_cast<float>(i), 0.0F, 0.0F };
            out.push_back(v);
        }
        return out;
    }

    std::vector<PointVertex> at(std::initializer_list<glm::vec3> positions)
    {
        std::vector<PointVertex> out;
        for (const auto& p : positions) {
            PointVertex v;
            v.position = p;
            out.push_back(v);
        }
        return out;
    }

    bool same_positions(const std::vector<PointVertex>& a, const std::vector<PointVertex>& b)
    {
        if (a.size() != b.size()) {
            return false;
        }
        for (size_t i = 0; i < a.size(); ++i) {
            if (a[i].position != b[i].position) {
                return false;
            }
        }
        return true;
    }

    ::testing::AssertionResult near_vec(const glm::vec3& a, const glm::vec3& b)
    {
        if (std::abs(a.x - b.x) < k_eps && std::abs(a.y - b.y) < k_eps && std::abs(a.z - b.z) < k_eps) {
            return ::testing::AssertionSuccess();
        }
        return ::testing::AssertionFailure() << "(" << a.x << ", " << a.y << ", " << a.z << ") != ("
                                             << b.x << ", " << b.y << ", " << b.z << ")";
    }

    class PhysicsOperatorTest : public ::testing::Test {
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

    TEST_F(PhysicsOperatorTest, InitializeKeepsTheVerticesInOrderAndStartsAtRest)
    {
        const auto input = particles(5, 2.0F);

        op.initialize(input);

        EXPECT_TRUE(same_positions(op.extract_vertices(), input));
        for (size_t i = 0; i < 5; ++i) {
            EXPECT_TRUE(near_vec(velocity(i), glm::vec3(0.0F))) << "particle " << i;
        }
    }

    TEST_F(PhysicsOperatorTest, EmptyInitializeKeepsWhatIsThere)
    {
        op.initialize(particles(4));

        op.initialize({});

        EXPECT_EQ(op.get_point_count(), 4U);
    }

    TEST_F(PhysicsOperatorTest, ReinitializeDiscardsMotion)
    {
        op.initialize(particles(2));
        op.apply_global_impulse({ 1.0F, 0.0F, 0.0F });
        op.process(0.1F);

        op.initialize(particles(3));

        EXPECT_EQ(op.get_point_count(), 3U);
        for (size_t i = 0; i < 3; ++i) {
            EXPECT_TRUE(near_vec(velocity(i), glm::vec3(0.0F))) << "particle " << i;
        }
    }

    TEST_F(PhysicsOperatorTest, CollectionsKeepGlobalOrderAndCount)
    {
        op.initialize_collections({ particles(2), particles(3, 1.0F, 10.0F) });

        EXPECT_EQ(op.get_point_count(), 5U);
        EXPECT_EQ(op.query_state("collection_count"), 2.0);
        EXPECT_EQ(op.query_state("point_count"), 5.0);
        EXPECT_NEAR(position(2).x, 10.0F, k_eps);
        EXPECT_NEAR(position(4).x, 12.0F, k_eps);
    }

    TEST_F(PhysicsOperatorTest, EmptyCollectionIsIgnored)
    {
        op.initialize(particles(2));

        op.add_collection({});

        EXPECT_EQ(op.query_state("collection_count"), 1.0);
    }

    TEST_F(PhysicsOperatorTest, GravityIntegratesWithSemiImplicitEuler)
    {
        op.set_gravity({ 0.0F, -10.0F, 0.0F });
        op.initialize(particles(1));

        op.process(0.1F);
        EXPECT_TRUE(near_vec(velocity(0), { 0.0F, -1.0F, 0.0F }));
        EXPECT_TRUE(near_vec(position(0), { 0.0F, -0.1F, 0.0F }));

        op.process(0.1F);
        EXPECT_TRUE(near_vec(velocity(0), { 0.0F, -2.0F, 0.0F }));
        EXPECT_TRUE(near_vec(position(0), { 0.0F, -0.3F, 0.0F }));
    }

    TEST_F(PhysicsOperatorTest, DragScalesVelocityEachStep)
    {
        op.set_drag(0.5F);
        op.initialize(particles(1));
        op.apply_impulse(0, { 1.0F, 0.0F, 0.0F });

        op.process(0.1F);

        EXPECT_TRUE(near_vec(velocity(0), { 0.5F, 0.0F, 0.0F }));
        EXPECT_NEAR(position(0).x, 0.05F, k_eps);
    }

    TEST_F(PhysicsOperatorTest, ForcedInternalDtOverridesTheCallersDt)
    {
        op.set_gravity({ 0.0F, -10.0F, 0.0F });
        op.set_force_internal_dt(true);
        op.set_internal_dt(0.5F);
        op.initialize(particles(1));

        op.process(0.1F);

        EXPECT_TRUE(near_vec(velocity(0), { 0.0F, -5.0F, 0.0F }));
        EXPECT_TRUE(op.uses_force_internal_dt());
    }

    TEST_F(PhysicsOperatorTest, ProcessWithoutParticlesIsSafe)
    {
        EXPECT_NO_THROW(op.process(0.1F));
        EXPECT_EQ(op.get_point_count(), 0U);
    }

    TEST_F(PhysicsOperatorTest, GravityDoesNotDependOnMass)
    {
        op.set_gravity({ 0.0F, -10.0F, 0.0F });
        op.add_collection(particles(1), 1.0F);
        op.add_collection(particles(1), 4.0F);

        op.process(0.1F);

        EXPECT_TRUE(near_vec(velocity(0), velocity(1)));
    }

    TEST_F(PhysicsOperatorTest, ForceFieldAccelerationFallsWithMass)
    {
        op.add_collection(particles(1), 1.0F);
        op.add_collection(particles(1), 2.0F);
        op.add_force_field(Kinesis::VectorField { .fn = [](const glm::vec3&) { return glm::vec3(0.0F, 2.0F, 0.0F); } });

        op.process(0.1F);

        EXPECT_TRUE(near_vec(velocity(0), { 0.0F, 0.2F, 0.0F }));
        EXPECT_TRUE(near_vec(velocity(1), { 0.0F, 0.1F, 0.0F }));
        EXPECT_EQ(op.force_field_count(), 1U);
    }

    TEST_F(PhysicsOperatorTest, ForceFieldsSumAndClear)
    {
        op.initialize(particles(1));
        op.add_force_field(Kinesis::VectorField { .fn = [](const glm::vec3&) { return glm::vec3(1.0F, 0.0F, 0.0F); } });
        op.add_force_field(Kinesis::VectorField { .fn = [](const glm::vec3&) { return glm::vec3(0.0F, 1.0F, 0.0F); } });
        op.process(0.1F);
        EXPECT_TRUE(near_vec(velocity(0), { 0.1F, 0.1F, 0.0F }));

        op.clear_force_fields();
        EXPECT_EQ(op.force_field_count(), 0U);
        op.process(0.1F);
        EXPECT_TRUE(near_vec(velocity(0), { 0.1F, 0.1F, 0.0F }));
    }

    TEST_F(PhysicsOperatorTest, ForceFieldIsEvaluatedAtTheParticlePosition)
    {
        op.initialize(at({ { 1.0F, 0.0F, 0.0F }, { 3.0F, 0.0F, 0.0F } }));
        op.add_force_field(Kinesis::VectorField { .fn = [](const glm::vec3& p) { return glm::vec3(0.0F, p.x, 0.0F); } });

        op.process(0.1F);

        EXPECT_NEAR(velocity(0).y, 0.1F, k_eps);
        EXPECT_NEAR(velocity(1).y, 0.3F, k_eps);
    }

    TEST_F(PhysicsOperatorTest, AttractionPullsTowardThePointAndStopsWhenCleared)
    {
        op.initialize(particles(1, 1.0F, 5.0F));
        op.set_attraction_point({ 0.0F, 0.0F, 0.0F });
        EXPECT_TRUE(op.has_attraction_point());

        op.process(0.1F);
        const auto pulled = velocity(0);
        EXPECT_LT(pulled.x, 0.0F);

        op.clear_attraction_point();
        EXPECT_FALSE(op.has_attraction_point());
        op.process(0.1F);
        EXPECT_TRUE(near_vec(velocity(0), pulled));
    }

    TEST_F(PhysicsOperatorTest, TurbulenceAddsMotionOnlyWhenEnabled)
    {
        op.initialize(particles(4));
        op.process(0.1F);
        EXPECT_NEAR(glm::length(velocity(0)), 0.0F, k_eps);

        op.set_turbulence_strength(5.0F);
        op.process(0.1F);

        float total = 0.0F;
        for (size_t i = 0; i < 4; ++i) {
            total += glm::length(velocity(i));
        }
        EXPECT_GT(total, 0.0F);
    }

    TEST_F(PhysicsOperatorTest, ImpulseDividesByMass)
    {
        op.add_collection(particles(1), 1.0F);
        op.add_collection(particles(1), 2.0F);

        op.apply_global_impulse({ 1.0F, 0.0F, 0.0F });

        EXPECT_TRUE(near_vec(velocity(0), { 1.0F, 0.0F, 0.0F }));
        EXPECT_TRUE(near_vec(velocity(1), { 0.5F, 0.0F, 0.0F }));
    }

    TEST_F(PhysicsOperatorTest, SingleImpulseUsesTheGlobalIndex)
    {
        op.initialize_collections({ particles(2), particles(3) });

        op.apply_impulse(3, { 1.0F, 0.0F, 0.0F });
        op.apply_impulse(99, { 1.0F, 0.0F, 0.0F });

        for (size_t i = 0; i < 5; ++i) {
            EXPECT_NEAR(velocity(i).x, i == 3 ? 1.0F : 0.0F, k_eps) << "particle " << i;
        }
        EXPECT_EQ(op.get_particle_velocity(3), 1.0);
        EXPECT_EQ(op.get_particle_velocity(0), 0.0);
        EXPECT_FALSE(op.get_particle_velocity(5).has_value());
    }

    TEST_F(PhysicsOperatorTest, AverageVelocityIsTheMagnitudeOfTheMeanVelocity)
    {
        op.initialize(particles(2));
        op.apply_impulse(0, { 1.0F, 0.0F, 0.0F });
        op.apply_impulse(1, { -1.0F, 0.0F, 0.0F });
        EXPECT_NEAR(*op.query_state("avg_velocity"), 0.0, k_eps);

        op.apply_impulse(1, { 2.0F, 0.0F, 0.0F });
        EXPECT_NEAR(*op.query_state("avg_velocity"), 1.0, k_eps);
        EXPECT_FALSE(op.query_state("no_such_state").has_value());
    }

    struct BoundsCase {
        const char* name;
        BoundsMode mode;
        float start_y;
        float push;
        float expected_y;
        float expected_vy;
    };

    class PhysicsOperatorBoundsTest : public ::testing::TestWithParam<BoundsCase> { };

    TEST_P(PhysicsOperatorBoundsTest, BehavesAtTheBox)
    {
        PhysicsOperator op;
        op.set_gravity(glm::vec3(0.0F));
        op.set_drag(0.0F);
        op.set_bounds({ -1.0F, -1.0F, -1.0F }, { 1.0F, 1.0F, 1.0F });
        op.set_bounds_mode(GetParam().mode);
        op.initialize(at({ { 0.5F, GetParam().start_y, 0.0F } }));
        op.apply_impulse(0, { 0.0F, GetParam().push, 0.0F });

        op.process(0.1F);

        const auto p = op.extract_vertices()[0].position;
        const auto v = op.extract_vertex_velocities()[0];
        EXPECT_NEAR(p.y, GetParam().expected_y, k_eps);
        EXPECT_NEAR(v.y, GetParam().expected_vy, k_eps);
        EXPECT_NEAR(p.x, 0.5F, k_eps);
    }

    INSTANTIATE_TEST_SUITE_P(
        PhysicsOperators,
        PhysicsOperatorBoundsTest,
        ::testing::Values(
            BoundsCase { "NoneAbove", BoundsMode::NONE, 0.9F, 100.0F, 10.9F, 100.0F },
            BoundsCase { "BounceAbove", BoundsMode::BOUNCE, 0.9F, 100.0F, 1.0F, -80.0F },
            BoundsCase { "WrapAbove", BoundsMode::WRAP, 0.9F, 100.0F, -1.0F, 100.0F },
            BoundsCase { "ClampAbove", BoundsMode::CLAMP, 0.9F, 100.0F, 1.0F, 0.0F },
            BoundsCase { "NoneBelow", BoundsMode::NONE, -0.9F, -100.0F, -10.9F, -100.0F },
            BoundsCase { "BounceBelow", BoundsMode::BOUNCE, -0.9F, -100.0F, -1.0F, 80.0F },
            BoundsCase { "WrapBelow", BoundsMode::WRAP, -0.9F, -100.0F, 1.0F, -100.0F },
            BoundsCase { "ClampBelow", BoundsMode::CLAMP, -0.9F, -100.0F, -1.0F, 0.0F }),
        [](const ::testing::TestParamInfo<BoundsCase>& info) { return std::string(info.param.name); });

    TEST(PhysicsOperatorDefaultsTest, DefaultsBounceInsideATenUnitBox)
    {
        PhysicsOperator op;

        EXPECT_EQ(op.get_bounds_mode(), BoundsMode::BOUNCE);
        EXPECT_EQ(op.get_gravity(), glm::vec3(0.0F, -9.81F, 0.0F));
        EXPECT_FLOAT_EQ(op.get_drag(), 0.01F);
        EXPECT_FALSE(op.spatial_interactions_enabled());
        EXPECT_FALSE(op.has_attraction_point());
        EXPECT_TRUE(op.bonds_enabled());
    }

    TEST_F(PhysicsOperatorTest, ParametersAreCaseInsensitiveAndReachTheState)
    {
        op.set_parameter("gravity_x", 1.0);
        op.set_parameter("GRAVITY_Y", 2.0);
        op.set_parameter("Gravity_Z", 3.0);
        op.set_parameter("interaction_radius", 4.0);
        op.set_parameter("repulsion_strength", 5.0);
        op.set_parameter("spatial_interactions", 1.0);

        EXPECT_EQ(op.get_gravity(), glm::vec3(1.0F, 2.0F, 3.0F));
        EXPECT_FLOAT_EQ(op.get_interaction_radius(), 4.0F);
        EXPECT_FLOAT_EQ(op.get_repulsion_strength(), 5.0F);
        EXPECT_TRUE(op.spatial_interactions_enabled());

        op.set_parameter("spatial_interactions", 0.0);
        EXPECT_FALSE(op.spatial_interactions_enabled());
    }

    TEST_F(PhysicsOperatorTest, DragParameterIsClampedToUnitRange)
    {
        op.set_parameter("drag", 5.0);
        EXPECT_FLOAT_EQ(op.get_drag(), 1.0F);

        op.set_parameter("drag", -3.0);
        EXPECT_FLOAT_EQ(op.get_drag(), 0.0F);

        op.set_parameter("drag", 0.25);
        EXPECT_FLOAT_EQ(op.get_drag(), 0.25F);
    }

    TEST_F(PhysicsOperatorTest, PointSizeParameterResizesExistingParticles)
    {
        op.initialize(particles(3));

        op.set_parameter("point_size", 9.0);

        for (const auto& v : op.extract_vertices()) {
            EXPECT_FLOAT_EQ(v.size, 9.0F);
        }
    }

    TEST_F(PhysicsOperatorTest, UnknownParameterChangesNothing)
    {
        op.set_parameter("no_such_parameter", 3.0);

        EXPECT_EQ(op.get_gravity(), glm::vec3(0.0F));
        EXPECT_FLOAT_EQ(op.get_drag(), 0.0F);
    }

    TEST_F(PhysicsOperatorTest, SpatialInteractionIsOffByDefault)
    {
        op.initialize(at({ { 0.0F, 0.0F, 0.0F }, { 0.8F, 0.0F, 0.0F } }));

        op.process(0.1F);

        EXPECT_TRUE(near_vec(velocity(0), glm::vec3(0.0F)));
        EXPECT_TRUE(near_vec(velocity(1), glm::vec3(0.0F)));
    }

    TEST_F(PhysicsOperatorTest, SpatialInteractionAttractsAMidRangePairEqualAndOpposite)
    {
        op.enable_spatial_interactions(true);
        op.initialize(at({ { 0.0F, 0.0F, 0.0F }, { 0.8F, 0.0F, 0.0F } }));

        op.process(0.1F);

        EXPECT_GT(velocity(0).x, 0.0F);
        EXPECT_LT(velocity(1).x, 0.0F);
        EXPECT_NEAR(velocity(0).x, -velocity(1).x, k_eps);
    }

    TEST_F(PhysicsOperatorTest, SpatialInteractionRepelsAVeryClosePair)
    {
        op.enable_spatial_interactions(true);
        op.initialize(at({ { 0.0F, 0.0F, 0.0F }, { 0.2F, 0.0F, 0.0F } }));

        op.process(0.1F);

        EXPECT_LT(velocity(0).x, 0.0F);
        EXPECT_GT(velocity(1).x, 0.0F);
    }

    TEST_F(PhysicsOperatorTest, SpatialInteractionIgnoresAPairBeyondTheRadius)
    {
        op.enable_spatial_interactions(true);
        op.set_interaction_radius(1.0F);
        op.initialize(at({ { 0.0F, 0.0F, 0.0F }, { 2.0F, 0.0F, 0.0F } }));

        op.process(0.1F);

        EXPECT_TRUE(near_vec(velocity(0), glm::vec3(0.0F)));
    }

    TEST(PhysicsOperatorPairTest, ACrossCollectionPairFeelsTheSameForceAsASameCollectionPair)
    {
        PhysicsOperator same;
        PhysicsOperator cross;
        for (auto* o : { &same, &cross }) {
            o->set_gravity(glm::vec3(0.0F));
            o->set_drag(0.0F);
            o->set_bounds_mode(BoundsMode::NONE);
            o->enable_spatial_interactions(true);
        }
        same.initialize(at({ { 0.0F, 0.0F, 0.0F }, { 0.8F, 0.0F, 0.0F } }));
        cross.initialize_collections({ at({ { 0.0F, 0.0F, 0.0F } }), at({ { 0.8F, 0.0F, 0.0F } }) });

        same.process(0.1F);
        cross.process(0.1F);

        EXPECT_NEAR(cross.extract_vertex_velocities()[0].x, same.extract_vertex_velocities()[0].x, k_eps);
        EXPECT_NEAR(cross.extract_vertex_velocities()[1].x, same.extract_vertex_velocities()[1].x, k_eps);
    }

    TEST_F(PhysicsOperatorTest, ClaimsAdoptBondsAndMoveMassToTheRoot)
    {
        op.initialize(particles(4));

        const std::vector<uint32_t> claimed { 0, 0, 2, 2 };
        op.sync_bonds_from_claims(claimed);

        EXPECT_EQ(op.bond_count(), 2U);
        EXPECT_FLOAT_EQ(op.get_accreted_mass(0), 2.0F);
        EXPECT_FLOAT_EQ(op.get_accreted_mass(1), 0.0F);
        EXPECT_FLOAT_EQ(op.get_accreted_mass(2), 2.0F);
        EXPECT_FLOAT_EQ(op.get_accreted_mass(3), 0.0F);
        EXPECT_FLOAT_EQ(op.get_total_mass(), 4.0F);
    }

    TEST_F(PhysicsOperatorTest, SyncingTheSameClaimsAgainChangesNothing)
    {
        op.initialize(particles(4));
        const std::vector<uint32_t> claimed { 0, 0, 2, 2 };
        op.sync_bonds_from_claims(claimed);

        op.sync_bonds_from_claims(claimed);

        EXPECT_FLOAT_EQ(op.get_accreted_mass(0), 2.0F);
        EXPECT_FLOAT_EQ(op.get_total_mass(), 4.0F);
    }

    TEST_F(PhysicsOperatorTest, AccretedMassDefaultsToOneBeforeAnyClaim)
    {
        op.initialize(particles(3));

        EXPECT_FLOAT_EQ(op.get_accreted_mass(1), 1.0F);
        EXPECT_FLOAT_EQ(op.get_accreted_mass(99), 1.0F);
        EXPECT_FLOAT_EQ(op.get_total_mass(), 3.0F);
        EXPECT_EQ(op.bond_count(), 0U);
    }

    TEST_F(PhysicsOperatorTest, DisablingBondsClearsThemAndIgnoresClaims)
    {
        op.initialize(particles(4));
        op.sync_bonds_from_claims(std::vector<uint32_t> { 0, 0, 2, 2 });
        ASSERT_EQ(op.bond_count(), 2U);

        op.enable_bonds(false);
        EXPECT_EQ(op.bond_count(), 0U);
        EXPECT_FALSE(op.bonds_enabled());

        op.sync_bonds_from_claims(std::vector<uint32_t> { 0, 0, 2, 2 });
        EXPECT_EQ(op.bond_count(), 0U);
    }

    TEST_F(PhysicsOperatorTest, ClearBondsRemovesBondsOnly)
    {
        op.initialize(particles(4));
        op.sync_bonds_from_claims(std::vector<uint32_t> { 0, 0, 2, 2 });

        op.clear_bonds();

        EXPECT_EQ(op.bond_count(), 0U);
        EXPECT_FLOAT_EQ(op.get_accreted_mass(0), 2.0F);
    }

    TEST_F(PhysicsOperatorTest, BondedSatellitePullsTowardItsRoot)
    {
        op.initialize(at({ { 0.0F, 0.0F, 0.0F }, { 3.0F, 0.0F, 0.0F } }));
        op.sync_bonds_from_claims(std::vector<uint32_t> { 0, 0 });

        op.process(0.1F);

        EXPECT_LT(velocity(1).x, 0.0F);
        EXPECT_NEAR(velocity(0).x, 0.0F, k_eps);
    }

    TEST_F(PhysicsOperatorTest, BondRestLengthGrowsWithTheRootsAccretedMass)
    {
        std::vector<PointVertex> pts = at({ { 0.0F, 0.0F, 0.0F }, { 2.0F, 0.0F, 0.0F }, { 4.0F, 0.0F, 0.0F } });
        for (float x = 100.0F; pts.size() < 8; x += 100.0F) {
            PointVertex v;
            v.position = { x, 0.0F, 0.0F };
            pts.push_back(v);
        }
        op.initialize(pts);
        op.set_bond_rest_length(1.0F);
        op.sync_bonds_from_claims(std::vector<uint32_t> { 0, 0, 0, 0, 0, 0, 0, 0 });

        op.process(0.1F);

        EXPECT_NEAR(velocity(1).x, 0.0F, 1e-3F);
        EXPECT_LT(velocity(2).x, 0.0F);
    }

    TEST_F(PhysicsOperatorTest, ReinitializeStartsWithoutBonds)
    {
        op.initialize(particles(4));
        op.sync_bonds_from_claims(std::vector<uint32_t> { 0, 0, 2, 2 });
        ASSERT_EQ(op.bond_count(), 2U);

        op.initialize(particles(4));

        EXPECT_EQ(op.bond_count(), 0U);
        EXPECT_FLOAT_EQ(op.get_total_mass(), 4.0F);
    }

    TEST_F(PhysicsOperatorTest, SeedsFromAnUpstreamOperatorWhenEmpty)
    {
        FieldOperator upstream;
        upstream.initialize(particles(5, 2.0F));

        op.seed_from_upstream(&upstream);

        EXPECT_TRUE(same_positions(op.extract_vertices(), particles(5, 2.0F)));
    }

    TEST_F(PhysicsOperatorTest, SeedingIgnoresPopulatedOperatorsNullAndEmptyUpstreams)
    {
        FieldOperator upstream;
        upstream.initialize(particles(5));
        FieldOperator empty;
        op.initialize(particles(2));

        op.seed_from_upstream(&upstream);
        EXPECT_EQ(op.get_point_count(), 2U);

        PhysicsOperator fresh;
        fresh.seed_from_upstream(nullptr);
        fresh.seed_from_upstream(&empty);
        EXPECT_EQ(fresh.get_point_count(), 0U);
    }

    TEST_F(PhysicsOperatorTest, VertexAttributesReportMassAndAccretedMass)
    {
        op.add_collection(particles(2), 3.0F);

        const auto attributes = op.extract_vertex_attributes();

        ASSERT_EQ(attributes.size(), 2U);
        EXPECT_EQ(attributes[0].first, "mass");
        EXPECT_EQ(attributes[1].first, "accreted_mass");
    }

    TEST_F(PhysicsOperatorTest, ReportsItsTypeNames)
    {
        EXPECT_EQ(op.get_type_name(), "Physics");
        EXPECT_STREQ(op.get_vertex_type_name(), "PointVertex");
    }

    std::shared_ptr<PointCloudNetwork> index_source(size_t count)
    {
        auto source = std::make_shared<PointCloudNetwork>(count);
        source->initialize();
        return source;
    }

    TEST(PhysicsOperatorMappingTest, MassMappingSetsPerParticleMassWithAFloor)
    {
        PhysicsOperator op;
        op.set_gravity(glm::vec3(0.0F));
        op.initialize(particles(4));

        op.apply_one_to_one("mass", index_source(4));
        op.apply_global_impulse({ 1.0F, 0.0F, 0.0F });

        const auto v = op.extract_vertex_velocities();
        EXPECT_NEAR(v[0].x, 10.0F, k_eps);
        EXPECT_NEAR(v[1].x, 1.0F, k_eps);
        EXPECT_NEAR(v[2].x, 0.5F, k_eps);
        EXPECT_NEAR(v[3].x, 1.0F / 3.0F, k_eps);
    }

    TEST(PhysicsOperatorMappingTest, ColorMappingColorsEachParticle)
    {
        PhysicsOperator op;
        op.initialize(particles(3));

        op.apply_one_to_one("color", index_source(3));

        const auto v = op.extract_vertices();
        EXPECT_TRUE(near_vec(v[0].color, { 0.0F, 0.5F, 1.0F }));
        EXPECT_TRUE(near_vec(v[1].color, { 1.0F, 0.5F, 0.0F }));
        EXPECT_TRUE(near_vec(v[2].color, { 1.0F, 0.5F, 0.0F }));
    }

    TEST(PhysicsOperatorMappingTest, SizeMappingScalesAndClampsParticleSize)
    {
        PhysicsOperator op;
        op.initialize(particles(7));

        op.apply_one_to_one("size", index_source(7));

        const auto v = op.extract_vertices();
        EXPECT_FLOAT_EQ(v[0].size, 1.0F);
        EXPECT_FLOAT_EQ(v[2].size, 20.0F);
        EXPECT_FLOAT_EQ(v[6].size, 50.0F);
    }

    TEST(PhysicsOperatorMappingTest, SourceOfTheWrongSizeIsIgnored)
    {
        PhysicsOperator op;
        op.set_gravity(glm::vec3(0.0F));
        op.initialize(particles(4));

        op.apply_one_to_one("color", index_source(2));
        op.apply_one_to_one("mass", index_source(2));
        op.apply_global_impulse({ 1.0F, 0.0F, 0.0F });

        EXPECT_TRUE(near_vec(op.extract_vertices()[0].color, glm::vec4(glm::vec3(1.0F), 1.F)));
        EXPECT_NEAR(op.extract_vertex_velocities()[0].x, 1.0F, k_eps);
    }

    TEST(PhysicsOperatorMappingTest, ForceMappingAcceleratesEachParticleByItsSourceValue)
    {
        PhysicsOperator op;
        op.set_gravity(glm::vec3(0.0F));
        op.set_drag(0.0F);
        op.set_bounds_mode(BoundsMode::NONE);
        op.initialize(particles(3));

        op.apply_one_to_one("force_x", index_source(3));
        op.process(0.1F);

        const auto v = op.extract_vertex_velocities();
        EXPECT_NEAR(v[0].x, 0.0F, k_eps);
        EXPECT_NEAR(v[1].x, 0.1F, k_eps);
        EXPECT_NEAR(v[2].x, 0.2F, k_eps);
    }

    ParticleNetwork make_swarm(size_t count = 8)
    {
        return ParticleNetwork(count, glm::vec3(-100.0F), glm::vec3(100.0F));
    }

    std::vector<PointVertex> points_of(ParticleNetwork& net)
    {
        if (auto* physics = net.as_operator<PhysicsOperator>()) {
            return physics->extract_vertices();
        }
        if (auto* field = net.as_operator<FieldOperator>()) {
            return field->extract_point_vertices();
        }
        return {};
    }

    TEST(ParticleNetworkTest, HasNoOperatorUntilInitialized)
    {
        ParticleNetwork net(8);

        EXPECT_FALSE(net.has_operator());
        EXPECT_EQ(net.get_node_count(), 8U);
    }

    TEST(ParticleNetworkTest, InitializeBuildsAPhysicsOperatorWithTheRequestedParticles)
    {
        ParticleNetwork net(8);

        net.initialize();

        ASSERT_TRUE(net.has_operator());
        EXPECT_EQ(net.get_operator()->get_type_name(), "Physics");
        EXPECT_EQ(net.get_node_count(), 8U);
        EXPECT_EQ(net.get_point_count(), 8U);
    }

    TEST(ParticleNetworkTest, InitializeTwiceKeepsTheSameOperator)
    {
        ParticleNetwork net(8);
        net.initialize();
        const auto* op = net.get_operator();

        net.initialize();

        EXPECT_EQ(net.get_operator(), op);
    }

    TEST(ParticleNetworkTest, ProcessBatchInitializesLazily)
    {
        ParticleNetwork net(8);

        net.process_batch(1);

        EXPECT_TRUE(net.has_operator());
    }

    TEST(ParticleNetworkTest, DisabledDefaultOperatorLeavesTheNetworkEmpty)
    {
        ParticleNetwork net(8);
        net.enable_default_operator(false);

        net.initialize();

        EXPECT_FALSE(net.default_operator_enabled());
        EXPECT_FALSE(net.has_operator());
        EXPECT_EQ(net.get_node_count(), 8U);
        EXPECT_FALSE(net.get_node_output(0).has_value());
    }

    TEST(ParticleNetworkDeathTest, ProcessBatchWithoutAnOperatorDoesNotCrash)
    {
        const auto previous = GTEST_FLAG_GET(death_test_style);
        GTEST_FLAG_SET(death_test_style, "threadsafe");

        EXPECT_EXIT(
            {
                ParticleNetwork net(4);
                net.enable_default_operator(false);
                net.initialize();
                net.process_batch(1);
                std::_Exit(0);
            },
            ::testing::ExitedWithCode(0), "");

        GTEST_FLAG_SET(death_test_style, previous);
    }

    TEST(ParticleNetworkTest, GeneratedParticlesStartInsideTheBounds)
    {
        const glm::vec3 lo(-2.0F, 0.0F, 1.0F);
        const glm::vec3 hi(2.0F, 3.0F, 4.0F);
        ParticleNetwork net(64, lo, hi);
        net.initialize();

        for (const auto& v : points_of(net)) {
            EXPECT_GE(v.position.x, lo.x);
            EXPECT_LE(v.position.x, hi.x);
            EXPECT_GE(v.position.y, lo.y);
            EXPECT_LE(v.position.y, hi.y);
            EXPECT_GE(v.position.z, lo.z);
            EXPECT_LE(v.position.z, hi.z);
        }
    }

    TEST(ParticleNetworkTest, ProcessBatchAppliesGravityEveryFrameWithTheTimestep)
    {
        auto net = make_swarm();
        net.initialize();
        auto before = points_of(net);

        net.process_batch(1);
        const auto after = points_of(net);

        const float dt = net.get_timestep();
        const float expected = -9.81F * dt * dt * (1.0F - 0.01F);
        EXPECT_NEAR(after[0].position.y - before[0].position.y, expected, 1e-4F);
    }

    TEST(ParticleNetworkTest, ABatchOfTwoFramesMatchesTwoBatchesOfOne)
    {
        auto batch = make_swarm();
        auto single = make_swarm();
        batch.initialize();
        single.initialize();
        const auto start_batch = points_of(batch)[0].position.y;
        const auto start_single = points_of(single)[0].position.y;

        batch.process_batch(2);
        single.process_batch(1);
        single.process_batch(1);

        EXPECT_NEAR(points_of(batch)[0].position.y - start_batch,
            points_of(single)[0].position.y - start_single, 1e-4F);
    }

    TEST(ParticleNetworkTest, TimestepScalesTheStep)
    {
        auto slow = make_swarm();
        auto fast = make_swarm();
        slow.set_timestep(0.016F);
        fast.set_timestep(0.032F);
        slow.initialize();
        fast.initialize();
        const auto slow_start = points_of(slow)[0].position.y;
        const auto fast_start = points_of(fast)[0].position.y;

        slow.process_batch(1);
        fast.process_batch(1);

        const float slow_drop = slow_start - points_of(slow)[0].position.y;
        const float fast_drop = fast_start - points_of(fast)[0].position.y;
        EXPECT_NEAR(fast_drop / slow_drop, 4.0F, 0.05F);
    }

    TEST(ParticleNetworkTest, DefaultTimestepIsTheFramePeriod)
    {
        ParticleNetwork net(4);
        EXPECT_NEAR(net.get_timestep(), 1.0F / 60.0F, 1e-6F);

        net.set_frame_rate(120);
        EXPECT_NEAR(net.get_timestep(), 1.0F / 120.0F, 1e-6F);
    }

    TEST(ParticleNetworkTest, ATimestepOverridesTheFramePeriodUntilCleared)
    {
        ParticleNetwork net(4);

        net.set_timestep(0.25F);
        net.set_frame_rate(120);
        EXPECT_FLOAT_EQ(net.get_timestep(), 0.25F);

        net.clear_timestep();
        EXPECT_NEAR(net.get_timestep(), 1.0F / 120.0F, 1e-6F);
    }

    TEST(ParticleNetworkTest, FrameRateScalesTheStep)
    {
        auto slow = make_swarm();
        auto fast = make_swarm();
        slow.set_frame_rate(60);
        fast.set_frame_rate(30);
        slow.initialize();
        fast.initialize();
        const auto slow_start = points_of(slow)[0].position.y;
        const auto fast_start = points_of(fast)[0].position.y;

        slow.process_batch(1);
        fast.process_batch(1);

        const float slow_drop = slow_start - points_of(slow)[0].position.y;
        const float fast_drop = fast_start - points_of(fast)[0].position.y;
        EXPECT_NEAR(fast_drop / slow_drop, 4.0F, 0.05F);
    }

    TEST(ParticleNetworkTest, ARegisteredNetworkTakesTheManagersFrameRate)
    {
        Nodes::NodeGraphManager manager(48000, 512, 144);
        auto net = std::make_shared<ParticleNetwork>(4);

        manager.register_network_global(net);

        EXPECT_EQ(net->get_frame_rate(), 144U);
        EXPECT_NEAR(net->get_timestep(), 1.0F / 144.0F, 1e-6F);
    }

    TEST(ParticleNetworkTest, DisabledNetworkDoesNotMove)
    {
        auto net = make_swarm();
        net.initialize();
        net.set_enabled(false);
        const auto before = points_of(net);

        net.process_batch(3);

        EXPECT_TRUE(same_positions(points_of(net), before));
    }

    TEST(ParticleNetworkTest, NodeOutputIsTheParticleSpeed)
    {
        auto net = make_swarm();
        net.initialize();
        EXPECT_EQ(net.get_node_output(0), 0.0);

        net.process_batch(2);

        EXPECT_GT(*net.get_node_output(0), 0.0);
        EXPECT_FALSE(net.get_node_output(99).has_value());
    }

    TEST(ParticleNetworkTest, ResetRegeneratesAtRestWithTheSameCount)
    {
        auto net = make_swarm();
        net.initialize();
        net.process_batch(3);
        ASSERT_GT(*net.as_operator<PhysicsOperator>()->query_state("avg_velocity"), 0.0);

        net.reset();

        EXPECT_EQ(net.get_node_count(), 8U);
        EXPECT_NEAR(*net.as_operator<PhysicsOperator>()->query_state("avg_velocity"), 0.0, k_eps);
    }

    TEST(ParticleNetworkTest, ResetClearsBondsFromAnEarlierClaim)
    {
        auto net = make_swarm(4);
        net.initialize();
        auto* physics = net.as_operator<PhysicsOperator>();
        physics->sync_bonds_from_claims(std::vector<uint32_t> { 0, 0, 2, 2 });
        ASSERT_EQ(physics->bond_count(), 2U);

        net.reset();

        EXPECT_EQ(net.as_operator<PhysicsOperator>()->bond_count(), 0U);
    }

    TEST(ParticleNetworkTest, ReinitializeChangesCountAndBoundsAndKeepsTheOperator)
    {
        auto net = make_swarm();
        net.initialize();
        const auto* op = net.get_operator();

        net.reinitialize(20, glm::vec3(-5.0F), glm::vec3(5.0F), Kinesis::SpatialDistribution::RANDOM_VOLUME);

        EXPECT_EQ(net.get_operator(), op);
        EXPECT_EQ(net.get_node_count(), 20U);
        EXPECT_EQ(net.get_bounds().max, glm::vec3(5.0F));
    }

    TEST(ParticleNetworkTest, ResetAppliesBoundsChangedWithSetBounds)
    {
        ParticleNetwork net(1, glm::vec3(-100.0F), glm::vec3(100.0F));
        net.initialize();
        net.set_bounds(glm::vec3(-1.0F), glm::vec3(1.0F));
        net.reset();
        auto* physics = net.as_operator<PhysicsOperator>();
        physics->set_gravity(glm::vec3(0.0F));
        physics->set_drag(0.0F);
        physics->apply_impulse(0, { 0.0F, 1000.0F, 0.0F });

        net.process_batch(1);

        EXPECT_LE(physics->extract_vertices()[0].position.y, 1.0F + k_eps);
    }

    class ParticleOperatorSwapTest : public ::testing::TestWithParam<std::pair<bool, bool>> { };

    TEST_P(ParticleOperatorSwapTest, SwapCarriesTheParticlePositions)
    {
        auto net = make_swarm(12);
        net.initialize();
        if (GetParam().first) {
            net.create_operator<FieldOperator>();
        }
        const auto before = points_of(net);
        ASSERT_EQ(before.size(), 12U);

        if (GetParam().second) {
            net.create_operator<FieldOperator>();
        } else {
            net.create_operator<PhysicsOperator>();
        }

        EXPECT_EQ(net.get_operator()->get_type_name(), GetParam().second ? "Field" : "Physics");
        EXPECT_TRUE(same_positions(points_of(net), before));
        EXPECT_EQ(net.get_node_count(), 12U);
    }

    INSTANTIATE_TEST_SUITE_P(
        ParticleNetworks,
        ParticleOperatorSwapTest,
        ::testing::Values(
            std::make_pair(false, true),
            std::make_pair(true, false),
            std::make_pair(false, false),
            std::make_pair(true, true)),
        [](const ::testing::TestParamInfo<std::pair<bool, bool>>& info) {
            const std::string from = info.param.first ? "Field" : "Physics";
            const std::string to = info.param.second ? "Field" : "Physics";
            return from + "To" + to;
        });

    TEST(ParticleNetworkSwapTest, UnsupportedOperatorsAreRejected)
    {
        auto net = make_swarm();
        net.initialize();

        net.set_operator(nullptr);
        net.set_operator(std::make_unique<TopologyOperator>());
        net.set_operator(std::make_unique<PathOperator>());

        EXPECT_EQ(net.get_operator()->get_type_name(), "Physics");
        EXPECT_EQ(net.get_node_count(), 8U);
    }

    TEST(ParticleNetworkSwapTest, ANewPhysicsOperatorTakesTheNetworkBounds)
    {
        ParticleNetwork net(1, glm::vec3(-1.0F), glm::vec3(1.0F));
        net.initialize();
        auto* fresh = net.create_operator<PhysicsOperator>();
        fresh->set_gravity(glm::vec3(0.0F));
        fresh->set_drag(0.0F);
        fresh->apply_impulse(0, { 0.0F, 1000.0F, 0.0F });

        net.process_batch(1);

        EXPECT_LE(fresh->extract_vertices()[0].position.y, 1.0F + k_eps);
    }

    class ParticleTopologyTest : public ::testing::TestWithParam<std::pair<Topology, bool>> { };

    TEST_P(ParticleTopologyTest, SetTopologyTogglesSpatialInteractions)
    {
        auto net = make_swarm();
        net.initialize();

        net.set_topology(GetParam().first);

        EXPECT_EQ(net.as_operator<PhysicsOperator>()->spatial_interactions_enabled(), GetParam().second);
    }

    TEST_P(ParticleTopologyTest, ANewPhysicsOperatorFollowsTheSameRule)
    {
        auto net = make_swarm();
        net.initialize();
        net.set_topology(GetParam().first);

        auto* fresh = net.create_operator<PhysicsOperator>();

        EXPECT_EQ(fresh->spatial_interactions_enabled(), GetParam().second);
    }

    INSTANTIATE_TEST_SUITE_P(
        ParticleNetworks,
        ParticleTopologyTest,
        ::testing::Values(
            std::make_pair(Topology::INDEPENDENT, false),
            std::make_pair(Topology::SPATIAL, true),
            std::make_pair(Topology::GRID_2D, true),
            std::make_pair(Topology::GRID_3D, true)),
        [](const ::testing::TestParamInfo<std::pair<Topology, bool>>& info) {
            switch (info.param.first) {
            case Topology::SPATIAL:
                return std::string("Spatial");
            case Topology::GRID_2D:
                return std::string("Grid2D");
            case Topology::GRID_3D:
                return std::string("Grid3D");
            default:
                return std::string("Independent");
            }
        });

    TEST(ParticleNetworkMetadataTest, DescribesThePhysicsOperator)
    {
        ParticleNetwork net(8);
        net.initialize();

        const auto meta = net.get_metadata();

        EXPECT_EQ(meta.at("point_count"), "8");
        EXPECT_EQ(meta.at("operator"), "Physics");
        EXPECT_EQ(meta.at("gravity"), "(0.00, -9.81, 0.00)");
        EXPECT_EQ(meta.at("bounds_min"), "(-10.00, -10.00, -10.00)");
        EXPECT_EQ(meta.at("bounds_max"), "(10.00, 10.00, 10.00)");
        EXPECT_TRUE(meta.contains("drag"));
        EXPECT_TRUE(meta.contains("avg_velocity"));
        EXPECT_TRUE(meta.contains("timestep"));
    }

    TEST(ParticleNetworkMetadataTest, OmitsPhysicsFieldsForAFieldOperator)
    {
        ParticleNetwork net(8);
        net.initialize();
        net.create_operator<FieldOperator>();

        const auto meta = net.get_metadata();

        EXPECT_EQ(meta.at("operator"), "Field");
        EXPECT_FALSE(meta.contains("gravity"));
    }

    TEST(ParticleNetworkMappingTest, BroadcastSourceDrivesAPhysicsParameter)
    {
        auto net = make_swarm();
        net.initialize();
        auto source = std::make_shared<Impulse>(0.5);
        source->set_amplitude(3.0);
        source->process_sample(0.0);

        net.map_parameter("gravity_y", source, MappingMode::BROADCAST);
        net.process_batch(1);

        EXPECT_FLOAT_EQ(net.as_operator<PhysicsOperator>()->get_gravity().y, 3.0F);
    }

    TEST(ParticleNetworkMappingTest, UnmappingStopsTheSourceDrivingTheParameter)
    {
        auto net = make_swarm();
        net.initialize();
        auto source = std::make_shared<Impulse>(0.5);
        source->set_amplitude(3.0);
        source->process_sample(0.0);
        net.map_parameter("gravity_y", source, MappingMode::BROADCAST);
        net.process_batch(1);

        net.unmap_parameter("gravity_y");
        net.as_operator<PhysicsOperator>()->set_gravity({ 0.0F, -1.0F, 0.0F });
        net.process_batch(1);

        EXPECT_FLOAT_EQ(net.as_operator<PhysicsOperator>()->get_gravity().y, -1.0F);
    }

    TEST(ParticleNetworkMappingTest, OneToOneMassMappingReachesEveryParticle)
    {
        auto net = make_swarm(4);
        net.initialize();
        auto* physics = net.as_operator<PhysicsOperator>();
        physics->set_gravity(glm::vec3(0.0F));

        static_cast<NodeNetwork&>(net).map_parameter("mass", index_source(4));
        net.process_batch(1);
        physics->apply_global_impulse({ 1.0F, 0.0F, 0.0F });

        const auto v = physics->extract_vertex_velocities();
        EXPECT_NEAR(v[0].x, 10.0F, 1e-3F);
        EXPECT_NEAR(v[2].x, 0.5F, 1e-3F);
    }

    TEST(ParticleNetworkMappingTest, OneToOneColorMappingShowsInTheParticleVertices)
    {
        auto net = make_swarm(4);
        net.initialize();

        static_cast<NodeNetwork&>(net).map_parameter("color", index_source(4));
        net.process_batch(1);

        const auto points = points_of(net);
        EXPECT_TRUE(near_vec(points[0].color, { 0.0F, 0.5F, 1.0F }));
        EXPECT_TRUE(near_vec(points[3].color, { 1.0F, 0.5F, 0.0F }));
    }

    TEST(ParticleNetworkMappingTest, OneToOneForceMappingAcceleratesTheParticlesThroughTheNetwork)
    {
        auto net = make_swarm(3);
        net.initialize();
        auto* physics = net.as_operator<PhysicsOperator>();
        physics->set_gravity(glm::vec3(0.0F));
        physics->set_drag(0.0F);

        static_cast<NodeNetwork&>(net).map_parameter("force_x", index_source(3));
        net.process_batch(1);

        const auto v = physics->extract_vertex_velocities();
        EXPECT_NEAR(v[0].x, 0.0F, k_eps);
        EXPECT_GT(v[2].x, v[1].x);
        EXPECT_GT(v[1].x, 0.0F);
    }

} // namespace

} // namespace MayaFlux::Test
