#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Buffers/BufferManager.hpp"
#include "MayaFlux/Nexus/Fabric.hpp"
#include "MayaFlux/Nexus/Principals/Locus.hpp"
#include "MayaFlux/Nexus/Principals/Presence.hpp"
#include "MayaFlux/Transitive/Facet/ControlBind.hpp"
#include "MayaFlux/Vruta/EventManager.hpp"
#include "MayaFlux/Vruta/Scheduler.hpp"

#include <glm/gtc/quaternion.hpp>

#include "../transitive/control_store_fixtures.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Nexus::Agent;
    using Nexus::AgentControls;
    using Nexus::Emitter;
    using Nexus::EmitterControls;
    using Nexus::Fabric;
    using Nexus::Locus;
    using Nexus::LocusControls;
    using Nexus::Presence;
    using Nexus::PresenceControls;
    using Nexus::Quantity;
    using Nexus::Sensor;
    using Nexus::SensorControls;

    class NexusEntityControlsTest : public ::testing::Test {
    protected:
        Vruta::TaskScheduler scheduler { 48000 };
        Vruta::EventManager events;
        Buffers::BufferManager buffers;
        Fabric fabric { scheduler, events, buffers, 1.0F };
    };

    TEST(NexusEmitterControlsTest, ANewEmitterHoldsOnlyItsDefaults)
    {
        const Emitter emitter;

        EXPECT_FALSE(emitter.position().has_value());
        EXPECT_FALSE(emitter.color().has_value());
        EXPECT_FALSE(emitter.size().has_value());
        EXPECT_FLOAT_EQ(emitter.intensity(), 1.0F);
        EXPECT_FLOAT_EQ(emitter.radius(), 1.0F);
    }

    TEST(NexusEmitterControlsTest, SetAppliesOnlyTheMembersThatHoldAValue)
    {
        Emitter emitter;

        emitter.set(EmitterControls { .position = glm::vec3(1.0F, 2.0F, 3.0F), .intensity = 0.5 });

        EXPECT_EQ(emitter.position(), glm::vec3(1.0F, 2.0F, 3.0F));
        EXPECT_FLOAT_EQ(emitter.intensity(), 0.5F);
        EXPECT_FLOAT_EQ(emitter.radius(), 1.0F);
        EXPECT_FALSE(emitter.color().has_value());
    }

    TEST(NexusEmitterControlsTest, AnUnsetMemberIsTakenAway)
    {
        Emitter emitter;
        emitter.set(EmitterControls { .position = glm::vec3(1.0F), .color = glm::vec3(0.1F, 0.2F, 0.3F), .size = 4.0 });

        emitter.set(EmitterControls { .position = Quantity::unset(), .color = Quantity::unset() });

        EXPECT_FALSE(emitter.position().has_value());
        EXPECT_FALSE(emitter.color().has_value());
        ASSERT_TRUE(emitter.size().has_value());
        EXPECT_FLOAT_EQ(*emitter.size(), 4.0F);
    }

    TEST(NexusEmitterControlsTest, GetCopiesTheStateAsItIsNow)
    {
        Emitter emitter;
        emitter.set(EmitterControls { .radius = 2.5 });

        auto copy = emitter.get();
        copy.radius = 9.0;

        EXPECT_FLOAT_EQ(copy.radius.value<float>(), 9.0F);
        EXPECT_FLOAT_EQ(emitter.radius(), 2.5F);
        EXPECT_FLOAT_EQ(emitter.controls().radius.value<float>(), 2.5F);
    }

    TEST(NexusEmitterControlsTest, TheStoreNamesEveryControl)
    {
        const Emitter emitter;

        std::vector<std::string> names;
        emitter.store().for_each([&names](std::string_view name, const Quantity&) { names.emplace_back(name); });

        const std::vector<std::string> expected { "position", "intensity", "radius", "color", "size" };
        EXPECT_EQ(names, expected);
    }

    TEST(NexusEmitterControlsTest, AUsersOwnStructExtendsTheEmitter)
    {
        Emitter emitter;
        auto& bloom = emitter.extend<StoreBloomTuning>();

        emitter.set(StoreBloomTuning { .rate = ControlValue(0.4) });

        EXPECT_DOUBLE_EQ(bloom.rate.value(), 0.4);
        EXPECT_FALSE(bloom.angle.has_value());
        EXPECT_DOUBLE_EQ(emitter.get<StoreBloomTuning>().rate.value(), 0.4);
    }

    TEST(NexusEmitterControlsTest, AControlWriterDrivesAMember)
    {
        const auto emitter = std::make_shared<Emitter>();
        const auto write = control_writer<&EmitterControls::intensity, double>(emitter);

        write(0.25);

        EXPECT_FLOAT_EQ(emitter->intensity(), 0.25F);
        EXPECT_FALSE(emitter->position().has_value());
    }

    TEST(NexusSensorControlsTest, TheQueryRadiusIsAControl)
    {
        Sensor sensor(2.0F);
        EXPECT_FLOAT_EQ(sensor.query_radius(), 2.0F);

        sensor.set(SensorControls { .position = glm::vec3(1.0F), .query_radius = 5.0 });

        EXPECT_FLOAT_EQ(sensor.query_radius(), 5.0F);
        EXPECT_EQ(sensor.position(), glm::vec3(1.0F));
    }

    TEST(NexusAgentControlsTest, AnOrientationIsAQuaternionControl)
    {
        Agent agent(3.0F);
        EXPECT_FALSE(agent.orientation().has_value());
        EXPECT_FLOAT_EQ(agent.query_radius(), 3.0F);

        agent.set(AgentControls { .orientation = glm::quat(0.5F, 0.1F, 0.2F, 0.3F) });

        const auto orientation = agent.orientation();
        ASSERT_TRUE(orientation.has_value());
        EXPECT_FLOAT_EQ(orientation->w, 0.5F);
        EXPECT_FLOAT_EQ(orientation->x, 0.1F);
        EXPECT_FLOAT_EQ(orientation->z, 0.3F);

        agent.set(AgentControls { .orientation = Quantity::unset() });
        EXPECT_FALSE(agent.orientation().has_value());
    }

    TEST(NexusAgentControlsTest, APresenceAndALocusKeepTheAgentControlsAndAddTheirOwn)
    {
        Presence presence(4.0F, {}, {}, {});
        Locus locus(Kinesis::NavigationConfig {}, 2.0F, {}, {});

        presence.set(AgentControls { .intensity = 0.3 });
        locus.set(AgentControls { .radius = 7.0 });

        EXPECT_FLOAT_EQ(presence.intensity(), 0.3F);
        EXPECT_FLOAT_EQ(locus.radius(), 7.0F);
        EXPECT_FLOAT_EQ(presence.falloff_radius(), 4.0F);
        EXPECT_FLOAT_EQ(locus.aspect(), 1.0F);
        EXPECT_FALSE(locus.timestep().has_value());
    }

    TEST(NexusAgentControlsTest, APresenceFalloffRadiusFollowsTheQueryRadiusUntilSet)
    {
        Presence presence(4.0F, {}, {}, {});

        presence.set(PresenceControls { .falloff_radius = 1.5 });
        EXPECT_FLOAT_EQ(presence.falloff_radius(), 1.5F);

        presence.set(PresenceControls { .falloff_radius = Quantity::unset() });
        EXPECT_FLOAT_EQ(presence.falloff_radius(), 4.0F);
    }

    TEST(NexusAgentControlsTest, APresenceFalloffCurveIsAppliedAtTheNextPerception)
    {
        Presence presence(4.0F, {}, {}, {});
        EXPECT_EQ(presence.falloff_curve(), Presence::FalloffCurve::Linear);

        presence.set(PresenceControls { .falloff_curve = Presence::FalloffCurve::Cosine });
        EXPECT_EQ(presence.falloff_curve(), Presence::FalloffCurve::Linear);

        presence.invoke_perception(Nexus::PerceptionContext {});
        EXPECT_EQ(presence.falloff_curve(), Presence::FalloffCurve::Cosine);
    }

    TEST(NexusAgentControlsTest, ACustomFalloffEmptiesTheCurveControl)
    {
        Presence presence(4.0F, {}, {}, {});

        presence.set_falloff([](float) { return 0.5F; });

        EXPECT_FALSE(presence.falloff_curve().has_value());
    }

    TEST(NexusAgentControlsTest, ALocusTakesItsAspectAndFixedTimestepFromControls)
    {
        Locus locus(Kinesis::NavigationConfig {}, 2.0F, {}, {});

        locus.set(LocusControls { .aspect = 1.5, .timestep = 0.01 });

        EXPECT_FLOAT_EQ(locus.aspect(), 1.5F);
        ASSERT_TRUE(locus.timestep().has_value());
        EXPECT_FLOAT_EQ(*locus.timestep(), 0.01F);

        locus.set(LocusControls { .timestep = Quantity::unset() });
        EXPECT_FALSE(locus.timestep().has_value());
    }

    TEST_F(NexusEntityControlsTest, AFabricIndexesAnEntityOnceItsPositionIsSet)
    {
        const auto sensor = std::make_shared<Sensor>(10.0F);
        const auto emitter = std::make_shared<Emitter>();
        fabric.wire(sensor).finalise();
        fabric.wire(emitter).finalise();
        sensor->set(SensorControls { .position = glm::vec3(0.0F) });
        fabric.commit();
        EXPECT_TRUE(fabric.within_radius(glm::vec3(0.0F), 1.0F).size() == 1U);

        emitter->set(EmitterControls { .position = glm::vec3(0.5F, 0.0F, 0.0F) });
        fabric.commit();
        EXPECT_EQ(fabric.within_radius(glm::vec3(0.0F), 1.0F).size(), 2U);
    }

    TEST_F(NexusEntityControlsTest, AnUnsetPositionTakesAnEntityOutOfTheIndex)
    {
        const auto emitter = std::make_shared<Emitter>();
        emitter->set(EmitterControls { .position = glm::vec3(0.0F) });
        fabric.wire(emitter).finalise();
        fabric.commit();
        ASSERT_EQ(fabric.within_radius(glm::vec3(0.0F), 1.0F).size(), 1U);

        emitter->set(EmitterControls { .position = Quantity::unset() });
        fabric.commit();

        EXPECT_TRUE(fabric.within_radius(glm::vec3(0.0F), 1.0F).empty());
    }

    TEST_F(NexusEntityControlsTest, TheContextAnInfluenceReadsCarriesTheControls)
    {
        std::optional<Nexus::InfluenceContext> seen;
        const auto emitter = std::make_shared<Emitter>(
            [&seen](const Nexus::InfluenceContext& ctx) { seen = ctx; });
        emitter->set(EmitterControls { .position = glm::vec3(1.0F, 2.0F, 3.0F), .intensity = 0.6, .color = glm::vec3(0.5F) });
        fabric.wire(emitter).finalise();

        fabric.fire(emitter->id());

        ASSERT_TRUE(seen.has_value());
        EXPECT_EQ(seen->position, glm::vec3(1.0F, 2.0F, 3.0F));
        EXPECT_FLOAT_EQ(seen->intensity, 0.6F);
        ASSERT_TRUE(seen->color.has_value());
        EXPECT_EQ(*seen->color, glm::vec3(0.5F));
        EXPECT_FALSE(seen->size.has_value());
    }

}
}
