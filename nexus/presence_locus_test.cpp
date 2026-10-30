#include "gtest/gtest.h"

#include "MayaFlux/Nexus/Principals/Locus.hpp"
#include "MayaFlux/Nexus/Principals/Presence.hpp"

namespace MayaFlux::Test {
namespace {

    using Kinesis::NavigationConfig;
    using Kinesis::QueryResult;
    using Nexus::InfluenceContext;
    using Nexus::Locus;
    using Nexus::PerceptionContext;
    using Nexus::Presence;

    class NexusPresenceTest : public ::testing::Test {
    protected:
        void SetUp() override
        {
            presence = std::make_unique<Presence>(
                4.0F,
                [](const PerceptionContext&) { },
                [](const InfluenceContext&) { },
                [this](uint32_t id, float weight) { radiated.emplace_back(id, weight); });
        }

        static std::vector<QueryResult> hits(std::initializer_list<std::pair<uint32_t, float>> pairs)
        {
            std::vector<QueryResult> results;
            for (const auto& [id, distance_sq] : pairs) {
                results.push_back({ .id = id, .distance_sq = distance_sq });
            }
            return results;
        }

        void perceive(std::initializer_list<std::pair<uint32_t, float>> pairs)
        {
            neighbours = hits(pairs);
            PerceptionContext ctx;
            ctx.spatial_results = neighbours;
            presence->invoke_perception(ctx);
        }

        [[nodiscard]] float weight_of(uint32_t id) const
        {
            for (const auto& [neighbour, weight] : presence->neighbors()) {
                if (neighbour == id) {
                    return weight;
                }
            }
            return -1.0F;
        }

        std::unique_ptr<Presence> presence;
        std::vector<QueryResult> neighbours;
        std::vector<std::pair<uint32_t, float>> radiated;
    };

    TEST_F(NexusPresenceTest, FalloffRadiusTracksTheQueryRadiusUntilSet)
    {
        EXPECT_FLOAT_EQ(presence->falloff_radius(), 4.0F);

        presence->set_query_radius(6.0F);
        EXPECT_FLOAT_EQ(presence->falloff_radius(), 6.0F);

        presence->set_falloff_radius(2.0F);
        EXPECT_FLOAT_EQ(presence->falloff_radius(), 2.0F);

        presence->set_query_radius(9.0F);
        EXPECT_FLOAT_EQ(presence->falloff_radius(), 2.0F);
    }

    TEST_F(NexusPresenceTest, LinearWeightFallsFromOneAtTheCentreToZeroAtTheRadius)
    {
        perceive({ { 1, 0.0F }, { 2, 4.0F }, { 3, 16.0F } });

        EXPECT_FLOAT_EQ(weight_of(1), 1.0F);
        EXPECT_FLOAT_EQ(weight_of(2), 0.5F);
        EXPECT_FLOAT_EQ(weight_of(3), 0.0F);
    }

    TEST_F(NexusPresenceTest, NeighboursBeyondTheFalloffRadiusAreDropped)
    {
        perceive({ { 1, 1.0F }, { 2, 16.01F }, { 3, 100.0F } });

        ASSERT_EQ(presence->neighbors().size(), 1U);
        EXPECT_EQ(presence->neighbors().front().first, 1U);
    }

    TEST_F(NexusPresenceTest, NarrowerFalloffRadiusDropsNeighboursTheQueryFound)
    {
        presence->set_falloff_radius(2.0F);

        perceive({ { 1, 1.0F }, { 2, 9.0F } });

        ASSERT_EQ(presence->neighbors().size(), 1U);
        EXPECT_EQ(presence->neighbors().front().first, 1U);
        EXPECT_FLOAT_EQ(weight_of(1), 0.5F);
    }

    TEST_F(NexusPresenceTest, NeighboursAreReplacedOnEveryPerception)
    {
        perceive({ { 1, 1.0F }, { 2, 1.0F } });
        ASSERT_EQ(presence->neighbors().size(), 2U);

        perceive({ { 3, 1.0F } });
        ASSERT_EQ(presence->neighbors().size(), 1U);
        EXPECT_EQ(presence->neighbors().front().first, 3U);

        perceive({});
        EXPECT_TRUE(presence->neighbors().empty());
    }

    TEST_F(NexusPresenceTest, NeighbourOrderFollowsTheSpatialResults)
    {
        perceive({ { 9, 1.0F }, { 4, 1.0F }, { 7, 1.0F } });

        ASSERT_EQ(presence->neighbors().size(), 3U);
        EXPECT_EQ(presence->neighbors()[0].first, 9U);
        EXPECT_EQ(presence->neighbors()[1].first, 4U);
        EXPECT_EQ(presence->neighbors()[2].first, 7U);
    }

    TEST_F(NexusPresenceTest, ZeroFalloffRadiusKeepsOnlyCoincidentNeighboursAtFullWeight)
    {
        presence->set_query_radius(0.0F);

        perceive({ { 1, 0.0F }, { 2, 0.25F } });

        ASSERT_EQ(presence->neighbors().size(), 1U);
        EXPECT_FLOAT_EQ(weight_of(1), 1.0F);
    }

    TEST_F(NexusPresenceTest, RadiationVisitsEveryCapturedNeighbourWithItsWeight)
    {
        perceive({ { 1, 0.0F }, { 2, 4.0F } });

        presence->invoke_influence({});

        ASSERT_EQ(radiated.size(), 2U);
        EXPECT_EQ(radiated[0].first, 1U);
        EXPECT_FLOAT_EQ(radiated[0].second, 1.0F);
        EXPECT_EQ(radiated[1].first, 2U);
        EXPECT_FLOAT_EQ(radiated[1].second, 0.5F);
    }

    TEST_F(NexusPresenceTest, RadiationBeforeAnyPerceptionVisitsNobody)
    {
        presence->invoke_influence({});
        EXPECT_TRUE(radiated.empty());
    }

    TEST_F(NexusPresenceTest, SetRadiateReplacesTheCallable)
    {
        int replaced {};
        presence->set_radiate([&replaced](uint32_t, float) { ++replaced; });
        perceive({ { 1, 1.0F } });

        presence->invoke_influence({});

        EXPECT_EQ(replaced, 1);
        EXPECT_TRUE(radiated.empty());
    }

    TEST_F(NexusPresenceTest, UserCallablesRunBeforeTheNeighbourhoodIsUsed)
    {
        std::vector<std::string> order;
        Presence ordered(
            4.0F,
            [&order](const PerceptionContext&) { order.emplace_back("perception"); },
            [&order](const InfluenceContext&) { order.emplace_back("influence"); },
            [&order](uint32_t, float) { order.emplace_back("radiate"); });

        const std::vector<QueryResult> results = hits({ { 1, 1.0F } });
        PerceptionContext ctx;
        ctx.spatial_results = results;
        ordered.invoke_perception(ctx);
        ordered.invoke_influence({});

        EXPECT_EQ(order, (std::vector<std::string> { "perception", "influence", "radiate" }));
    }

    TEST_F(NexusPresenceTest, NamedConstructorKeepsEveryName)
    {
        Presence named(
            1.0F,
            "see", [](const PerceptionContext&) { },
            "act", [](const InfluenceContext&) { },
            "spread", [](uint32_t, float) { });

        EXPECT_EQ(named.perception_fn_name(), "see");
        EXPECT_EQ(named.influence_fn_name(), "act");
        EXPECT_EQ(named.radiate_fn_name(), "spread");

        named.set_radiate_fn_name("other");
        EXPECT_EQ(named.radiate_fn_name(), "other");
    }

    TEST_F(NexusPresenceTest, DefaultCurveIsLinear)
    {
        ASSERT_TRUE(presence->falloff_curve().has_value());
        EXPECT_EQ(*presence->falloff_curve(), Presence::FalloffCurve::Linear);
    }

    struct CurveCase {
        Presence::FalloffCurve curve;
        float at_half;
        float at_edge;
    };

    class NexusPresenceCurveTest : public NexusPresenceTest,
                                   public ::testing::WithParamInterface<CurveCase> { };

    TEST_P(NexusPresenceCurveTest, NamedCurveWeighsCentreHalfAndEdge)
    {
        const CurveCase& c = GetParam();
        presence->set_falloff_curve(c.curve);

        perceive({ { 1, 0.0F }, { 2, 4.0F }, { 3, 16.0F } });

        ASSERT_TRUE(presence->falloff_curve().has_value());
        EXPECT_EQ(*presence->falloff_curve(), c.curve);
        EXPECT_FLOAT_EQ(weight_of(1), 1.0F);
        EXPECT_NEAR(weight_of(2), c.at_half, 1e-5F);
        EXPECT_NEAR(weight_of(3), c.at_edge, 1e-5F);
    }

    INSTANTIATE_TEST_SUITE_P(
        Curves,
        NexusPresenceCurveTest,
        ::testing::Values(
            CurveCase { Presence::FalloffCurve::Linear, 0.5F, 0.0F },
            CurveCase { Presence::FalloffCurve::Cosine, 0.5F, 0.0F },
            CurveCase { Presence::FalloffCurve::Exponential, 0.135335283F, 0.0183156389F },
            CurveCase { Presence::FalloffCurve::InverseSquare, 0.2F, 0.0588235294F }));

    TEST_F(NexusPresenceTest, CustomFalloffReceivesTheNormalisedDistance)
    {
        std::vector<float> seen;
        presence->set_falloff([&seen](float t) {
            seen.push_back(t);
            return 0.25F;
        });

        perceive({ { 1, 4.0F } });

        ASSERT_EQ(seen.size(), 1U);
        EXPECT_FLOAT_EQ(seen.front(), 0.5F);
        EXPECT_FLOAT_EQ(weight_of(1), 0.25F);
    }

    TEST_F(NexusPresenceTest, CustomFalloffClearsTheNamedCurve)
    {
        presence->set_falloff_curve(Presence::FalloffCurve::Cosine);
        presence->set_falloff([](float) { return 0.0F; });

        EXPECT_FALSE(presence->falloff_curve().has_value());
    }

    class NexusLocusTest : public ::testing::Test {
    protected:
        static NavigationConfig config()
        {
            NavigationConfig c;
            c.initial_eye = { 0.0F, 0.0F, 5.0F };
            c.initial_target = { 0.0F, 0.0F, 0.0F };
            c.move_speed = 100.0F;
            return c;
        }

        void age_the_clock()
        {
            locus.nav().last_tick = std::chrono::steady_clock::now() - std::chrono::milliseconds(100);
        }

        Locus locus { config(), 2.0F, [](const PerceptionContext&) { }, [](const InfluenceContext&) { } };
    };

    TEST_F(NexusLocusTest, PositionStartsAtTheEye)
    {
        ASSERT_TRUE(locus.position().has_value());
        EXPECT_EQ(*locus.position(), glm::vec3(0.0F, 0.0F, 5.0F));
        EXPECT_EQ(locus.nav().eye, glm::vec3(0.0F, 0.0F, 5.0F));
        EXPECT_FLOAT_EQ(locus.query_radius(), 2.0F);
    }

    TEST_F(NexusLocusTest, InitialYawAndPitchFaceTheTarget)
    {
        EXPECT_NEAR(std::abs(locus.nav().yaw), glm::pi<float>(), 1e-5F);
        EXPECT_NEAR(locus.nav().pitch, 0.0F, 1e-5F);
    }

    TEST_F(NexusLocusTest, CurrentViewIsUsableBeforeTheFirstPerception)
    {
        const auto& view = locus.current_view();
        EXPECT_NE(view.view, glm::mat4(0.0F));
        EXPECT_NE(view.projection, glm::mat4(0.0F));
    }

    TEST_F(NexusLocusTest, AspectDefaultsToOneAndMutates)
    {
        EXPECT_FLOAT_EQ(locus.aspect(), 1.0F);
        locus.set_aspect(16.0F / 9.0F);
        EXPECT_FLOAT_EQ(locus.aspect(), 16.0F / 9.0F);
    }

    TEST_F(NexusLocusTest, PerceptionWithoutHeldFlagsLeavesTheEyeWhereItIs)
    {
        age_the_clock();

        locus.invoke_perception({});

        EXPECT_EQ(locus.nav().eye, glm::vec3(0.0F, 0.0F, 5.0F));
        EXPECT_EQ(*locus.position(), glm::vec3(0.0F, 0.0F, 5.0F));
    }

    TEST_F(NexusLocusTest, HeldForwardMovesTheEyeTowardTheTargetAndPositionFollows)
    {
        locus.nav().forward_held = true;
        age_the_clock();

        locus.invoke_perception({});

        EXPECT_LT(locus.nav().eye.z, 5.0F);
        EXPECT_NEAR(locus.nav().eye.x, 0.0F, 1e-4F);
        EXPECT_EQ(*locus.position(), locus.nav().eye);
    }

    TEST_F(NexusLocusTest, NoTimestepIsSetByDefault)
    {
        EXPECT_FALSE(locus.timestep().has_value());
    }

    TEST_F(NexusLocusTest, AFixedTimestepMovesTheEyeByExactlyThatDistance)
    {
        locus.nav().forward_held = true;
        locus.set_timestep(0.01F);

        locus.invoke_perception({});

        EXPECT_NEAR(locus.nav().eye.z, 4.0F, 1e-4F);
        EXPECT_NEAR(locus.nav().eye.x, 0.0F, 1e-4F);
    }

    TEST_F(NexusLocusTest, AFixedTimestepIgnoresTheWallClock)
    {
        locus.nav().forward_held = true;
        locus.set_timestep(0.01F);
        age_the_clock();

        locus.invoke_perception({});

        EXPECT_NEAR(locus.nav().eye.z, 4.0F, 1e-4F);
    }

    TEST_F(NexusLocusTest, EachPerceptionAdvancesByTheTimestep)
    {
        locus.nav().forward_held = true;
        locus.set_timestep(0.01F);

        for (int i = 0; i < 5; ++i) {
            locus.invoke_perception({});
        }

        EXPECT_NEAR(locus.nav().eye.z, 0.0F, 1e-3F);
    }

    TEST_F(NexusLocusTest, TwoLociWithTheSameInputAndTimestepFollowTheSameEyePath)
    {
        Locus other { config(), 2.0F, [](const PerceptionContext&) { }, [](const InfluenceContext&) { } };
        for (auto* l : { &locus, &other }) {
            l->nav().forward_held = true;
            l->nav().right_held = true;
            l->set_timestep(1.0F / 60.0F);
        }
        other.nav().last_tick = std::chrono::steady_clock::now() - std::chrono::seconds(30);

        for (int i = 0; i < 10; ++i) {
            locus.invoke_perception({});
            other.invoke_perception({});
        }

        EXPECT_EQ(locus.nav().eye, other.nav().eye);
        EXPECT_NE(locus.nav().eye, glm::vec3(0.0F, 0.0F, 5.0F));
    }

    TEST_F(NexusLocusTest, AZeroTimestepLeavesTheEyeWhereItIs)
    {
        locus.nav().forward_held = true;
        locus.set_timestep(0.0F);
        age_the_clock();

        locus.invoke_perception({});

        EXPECT_EQ(locus.nav().eye, glm::vec3(0.0F, 0.0F, 5.0F));
    }

    TEST_F(NexusLocusTest, ClearingTheTimestepReturnsToWallTime)
    {
        locus.nav().forward_held = true;
        locus.set_timestep(0.01F);
        locus.clear_timestep();
        age_the_clock();

        locus.invoke_perception({});

        EXPECT_FALSE(locus.timestep().has_value());
        EXPECT_LT(locus.nav().eye.z, 0.0F);
    }

    TEST_F(NexusLocusTest, HeldBackMovesTheEyeAwayFromTheTarget)
    {
        locus.nav().back_held = true;
        age_the_clock();

        locus.invoke_perception({});

        EXPECT_GT(locus.nav().eye.z, 5.0F);
    }

    TEST_F(NexusLocusTest, PerceptionPublishesAnOrientationFromYawAndPitch)
    {
        ASSERT_FALSE(locus.orientation().has_value());

        locus.invoke_perception({});

        ASSERT_TRUE(locus.orientation().has_value());
        EXPECT_NEAR(glm::length(*locus.orientation()), 1.0F, 1e-5F);
    }

    TEST_F(NexusLocusTest, PerceptionRebuildsTheViewFromTheMovedEye)
    {
        const glm::mat4 before = locus.current_view().view;
        locus.nav().forward_held = true;
        age_the_clock();

        locus.invoke_perception({});

        EXPECT_NE(locus.current_view().view, before);
    }

    TEST_F(NexusLocusTest, UserPerceptionRunsBeforeTheEyeAdvances)
    {
        glm::vec3 seen {};
        Locus watcher(config(), 1.0F,
            [&seen, &watcher](const PerceptionContext&) { seen = watcher.nav().eye; },
            [](const InfluenceContext&) { });
        watcher.nav().forward_held = true;
        watcher.nav().last_tick = std::chrono::steady_clock::now() - std::chrono::milliseconds(100);

        watcher.invoke_perception({});

        EXPECT_EQ(seen, glm::vec3(0.0F, 0.0F, 5.0F));
        EXPECT_LT(watcher.nav().eye.z, 5.0F);
    }

    TEST_F(NexusLocusTest, NullViewTargetIsIgnored)
    {
        locus.add_view_target(nullptr);
        EXPECT_TRUE(locus.view_targets().empty());
    }

    TEST_F(NexusLocusTest, RemovingAViewTargetThatWasNeverAddedIsHarmless)
    {
        EXPECT_NO_THROW(locus.remove_view_target(nullptr));
        EXPECT_TRUE(locus.view_targets().empty());
    }

    TEST_F(NexusLocusTest, InfluenceRunsTheUserCallableWithNoViewTargets)
    {
        int calls {};
        Locus counted(config(), 1.0F,
            [](const PerceptionContext&) { },
            [&calls](const InfluenceContext&) { ++calls; });

        counted.invoke_influence({});

        EXPECT_EQ(calls, 1);
    }

    TEST_F(NexusLocusTest, NamedConstructorKeepsNamesAndSeedsThePosition)
    {
        Locus named(config(), 1.0F,
            "see", [](const PerceptionContext&) { },
            "act", [](const InfluenceContext&) { });

        EXPECT_EQ(named.perception_fn_name(), "see");
        EXPECT_EQ(named.influence_fn_name(), "act");
        ASSERT_TRUE(named.position().has_value());
        EXPECT_EQ(*named.position(), glm::vec3(0.0F, 0.0F, 5.0F));
    }

}
}
