#include "gtest/gtest.h"

#include "MayaFlux/Nexus/Principals/Agent.hpp"
#include "MayaFlux/Nexus/Principals/Emitter.hpp"
#include "MayaFlux/Nexus/Principals/Sensor.hpp"

namespace MayaFlux::Test {
namespace {

    using Nexus::Agent;
    using Nexus::Emitter;
    using Nexus::InfluenceContext;
    using Nexus::PerceptionContext;
    using Nexus::Sensor;

    TEST(NexusEmitterTest, StartsWithoutPositionIdOrAttachment)
    {
        const Emitter emitter;

        EXPECT_FALSE(emitter.position().has_value());
        EXPECT_EQ(emitter.id(), 0U);
        EXPECT_FALSE(emitter.attached());
        EXPECT_TRUE(emitter.fn_name().empty());
        EXPECT_FALSE(emitter.fn());
        EXPECT_TRUE(emitter.audio_sinks().empty());
        EXPECT_TRUE(emitter.render_sinks().empty());
    }

    TEST(NexusEmitterTest, PositionSetsAndClears)
    {
        Emitter emitter;

        emitter.set_position({ 1.0F, 2.0F, 3.0F });
        ASSERT_TRUE(emitter.position().has_value());
        EXPECT_EQ(*emitter.position(), glm::vec3(1.0F, 2.0F, 3.0F));

        emitter.clear_position();
        EXPECT_FALSE(emitter.position().has_value());
    }

    TEST(NexusEmitterTest, ParametersDefaultAndRoundTrip)
    {
        Emitter emitter;

        EXPECT_FLOAT_EQ(emitter.intensity(), 1.0F);
        EXPECT_FLOAT_EQ(emitter.radius(), 1.0F);
        EXPECT_FALSE(emitter.color().has_value());
        EXPECT_FALSE(emitter.size().has_value());

        emitter.set_intensity(0.5F);
        emitter.set_radius(3.0F);
        emitter.set_color({ 0.1F, 0.2F, 0.3F });
        emitter.set_size(9.0F);

        EXPECT_FLOAT_EQ(emitter.intensity(), 0.5F);
        EXPECT_FLOAT_EQ(emitter.radius(), 3.0F);
        ASSERT_TRUE(emitter.color().has_value());
        EXPECT_EQ(*emitter.color(), glm::vec3(0.1F, 0.2F, 0.3F));
        ASSERT_TRUE(emitter.size().has_value());
        EXPECT_FLOAT_EQ(*emitter.size(), 9.0F);

        emitter.clear_color();
        emitter.clear_size();
        EXPECT_FALSE(emitter.color().has_value());
        EXPECT_FALSE(emitter.size().has_value());
    }

    TEST(NexusEmitterTest, NamedConstructorKeepsNameAndFunction)
    {
        int calls {};
        Emitter emitter("pulse", [&calls](const InfluenceContext&) { ++calls; });

        EXPECT_EQ(emitter.fn_name(), "pulse");
        ASSERT_TRUE(emitter.fn());

        emitter.invoke({});
        EXPECT_EQ(calls, 1);

        emitter.set_fn_name("renamed");
        EXPECT_EQ(emitter.fn_name(), "renamed");
    }

    TEST(NexusEmitterTest, InvokePassesTheContextToTheFunction)
    {
        InfluenceContext seen;
        Emitter emitter([&seen](const InfluenceContext& ctx) { seen = ctx; });

        InfluenceContext ctx;
        ctx.position = { 4.0F, 5.0F, 6.0F };
        ctx.intensity = 0.75F;
        emitter.invoke(ctx);

        EXPECT_EQ(seen.position, ctx.position);
        EXPECT_FLOAT_EQ(seen.intensity, 0.75F);
    }

    TEST(NexusEmitterTest, InvokeWithNoFunctionOrInfluenceIsHarmless)
    {
        const Emitter emitter;
        EXPECT_NO_THROW(emitter.invoke({}));
    }

    TEST(NexusEmitterTest, TypedInfluenceRunsAfterTheFunction)
    {
        std::vector<std::string> order;
        Emitter emitter([&order](const InfluenceContext&) { order.emplace_back("fn"); });
        emitter.set_influence<int>(
            [&order](const InfluenceContext&) {
                order.emplace_back("producer");
                return 1;
            },
            [&order](const int&) { order.emplace_back("target"); });

        emitter.invoke({});

        EXPECT_EQ(order, (std::vector<std::string> { "fn", "producer", "target" }));
    }

    TEST(NexusEmitterTest, SettingAnInfluenceReplacesTheTarget)
    {
        int first {};
        int second {};
        Emitter emitter;
        emitter.set_influence<int>([](const InfluenceContext&) { return 1; }, [&first](const int&) { ++first; });
        emitter.set_influence<int>([](const InfluenceContext&) { return 1; }, [&second](const int&) { ++second; });

        emitter.invoke({});

        EXPECT_EQ(first, 0);
        EXPECT_EQ(second, 1);
    }

    TEST(NexusEmitterTest, ClearInfluenceStopsTheTarget)
    {
        int received {};
        Emitter emitter;
        emitter.set_influence<int>([](const InfluenceContext&) { return 1; }, [&received](const int&) { ++received; });

        emitter.invoke({});
        emitter.clear_influence();
        emitter.invoke({});

        EXPECT_EQ(received, 1);
    }

    TEST(NexusEmitterTest, AttachNullBufferLeavesItDetached)
    {
        Emitter emitter;
        emitter.attach(nullptr);
        EXPECT_FALSE(emitter.attached());
    }

    TEST(NexusEmitterTest, DetachWithoutAttachmentIsHarmless)
    {
        Emitter emitter;
        EXPECT_NO_THROW(emitter.detach());
        EXPECT_NO_THROW(emitter.recenter());
        EXPECT_FALSE(emitter.attached());
    }

    TEST(NexusSensorTest, StartsWithoutPositionAndKeepsTheRadius)
    {
        const Sensor sensor(2.5F);

        EXPECT_FALSE(sensor.position().has_value());
        EXPECT_EQ(sensor.id(), 0U);
        EXPECT_FLOAT_EQ(sensor.query_radius(), 2.5F);
        EXPECT_FALSE(sensor.fn());
    }

    TEST(NexusSensorTest, DefaultRadiusIsOne)
    {
        const Sensor sensor;
        EXPECT_FLOAT_EQ(sensor.query_radius(), 1.0F);
    }

    TEST(NexusSensorTest, PositionAndRadiusMutate)
    {
        Sensor sensor(1.0F);

        sensor.set_position({ 1.0F, 0.0F, -1.0F });
        sensor.set_query_radius(6.0F);

        ASSERT_TRUE(sensor.position().has_value());
        EXPECT_EQ(*sensor.position(), glm::vec3(1.0F, 0.0F, -1.0F));
        EXPECT_FLOAT_EQ(sensor.query_radius(), 6.0F);

        sensor.clear_position();
        EXPECT_FALSE(sensor.position().has_value());
    }

    TEST(NexusSensorTest, NamedConstructorKeepsNameAndFunction)
    {
        int calls {};
        Sensor sensor(1.0F, "watch", [&calls](const PerceptionContext&) { ++calls; });

        EXPECT_EQ(sensor.fn_name(), "watch");
        ASSERT_TRUE(sensor.fn());

        sensor.invoke({});
        EXPECT_EQ(calls, 1);

        sensor.set_fn_name("renamed");
        EXPECT_EQ(sensor.fn_name(), "renamed");
    }

    TEST(NexusSensorTest, InvokePassesTheContextToTheFunction)
    {
        size_t seen {};
        Sensor sensor(1.0F, [&seen](const PerceptionContext& ctx) { seen = ctx.spatial_results.size(); });

        const std::vector<Kinesis::QueryResult> neighbours {
            { .id = 7, .distance_sq = 0.25F },
            { .id = 9, .distance_sq = 0.5F },
        };
        PerceptionContext ctx;
        ctx.spatial_results = neighbours;
        sensor.invoke(ctx);

        EXPECT_EQ(seen, 2U);
    }

    TEST(NexusSensorTest, TypedPerceptionRunsBeforeTheFunction)
    {
        std::vector<std::string> order;
        Sensor sensor(1.0F, [&order](const PerceptionContext&) { order.emplace_back("fn"); });
        sensor.set_perception<int>(
            [&order](const PerceptionContext&) {
                order.emplace_back("source");
                return 1;
            },
            [&order](const int&) { order.emplace_back("target"); });

        sensor.invoke({});

        EXPECT_EQ(order, (std::vector<std::string> { "source", "target", "fn" }));
    }

    TEST(NexusSensorTest, SettingAPerceptionReplacesTheTarget)
    {
        int first {};
        int second {};
        Sensor sensor;
        sensor.set_perception<int>([](const PerceptionContext&) { return 1; }, [&first](const int&) { ++first; });
        sensor.set_perception<int>([](const PerceptionContext&) { return 1; }, [&second](const int&) { ++second; });

        sensor.invoke({});

        EXPECT_EQ(first, 0);
        EXPECT_EQ(second, 1);
    }

    TEST(NexusSensorTest, ClearPerceptionStopsTheTarget)
    {
        int received {};
        Sensor sensor;
        sensor.set_perception<int>([](const PerceptionContext&) { return 1; }, [&received](const int&) { ++received; });

        sensor.invoke({});
        sensor.clear_perception();
        sensor.invoke({});

        EXPECT_EQ(received, 1);
    }

    TEST(NexusAgentTest, StartsWithoutPositionOrientationOrAttachment)
    {
        const Agent agent(3.0F);

        EXPECT_FALSE(agent.position().has_value());
        EXPECT_FALSE(agent.orientation().has_value());
        EXPECT_EQ(agent.id(), 0U);
        EXPECT_FALSE(agent.attached());
        EXPECT_FLOAT_EQ(agent.query_radius(), 3.0F);
        EXPECT_FALSE(agent.perception_fn());
        EXPECT_FALSE(agent.influence_fn());
    }

    TEST(NexusAgentTest, PositionOrientationAndRadiusMutate)
    {
        Agent agent;

        agent.set_position({ 1.0F, 2.0F, 3.0F });
        const glm::quat q = glm::angleAxis(glm::radians(90.0F), glm::vec3(0.0F, 1.0F, 0.0F));
        agent.set_orientation(q);
        agent.set_query_radius(8.0F);

        ASSERT_TRUE(agent.position().has_value());
        EXPECT_EQ(*agent.position(), glm::vec3(1.0F, 2.0F, 3.0F));
        ASSERT_TRUE(agent.orientation().has_value());
        EXPECT_FLOAT_EQ(agent.orientation()->w, q.w);
        EXPECT_FLOAT_EQ(agent.query_radius(), 8.0F);

        agent.clear_position();
        agent.clear_orientation();
        EXPECT_FALSE(agent.position().has_value());
        EXPECT_FALSE(agent.orientation().has_value());
    }

    TEST(NexusAgentTest, ParametersDefaultAndRoundTrip)
    {
        Agent agent;

        EXPECT_FLOAT_EQ(agent.intensity(), 1.0F);
        EXPECT_FLOAT_EQ(agent.radius(), 1.0F);
        EXPECT_FALSE(agent.color().has_value());
        EXPECT_FALSE(agent.size().has_value());

        agent.set_intensity(0.5F);
        agent.set_radius(3.0F);
        agent.set_color({ 0.1F, 0.2F, 0.3F });
        agent.set_size(9.0F);

        EXPECT_FLOAT_EQ(agent.intensity(), 0.5F);
        EXPECT_FLOAT_EQ(agent.radius(), 3.0F);
        ASSERT_TRUE(agent.color().has_value());
        ASSERT_TRUE(agent.size().has_value());
        EXPECT_FLOAT_EQ(*agent.size(), 9.0F);

        agent.clear_color();
        agent.clear_size();
        EXPECT_FALSE(agent.color().has_value());
        EXPECT_FALSE(agent.size().has_value());
    }

    TEST(NexusAgentTest, NamedConstructorKeepsBothNames)
    {
        Agent agent(1.0F,
            "see", [](const PerceptionContext&) { },
            "act", [](const InfluenceContext&) { });

        EXPECT_EQ(agent.perception_fn_name(), "see");
        EXPECT_EQ(agent.influence_fn_name(), "act");
        EXPECT_TRUE(agent.perception_fn());
        EXPECT_TRUE(agent.influence_fn());

        agent.set_perception_fn_name("a");
        agent.set_influence_fn_name("b");
        EXPECT_EQ(agent.perception_fn_name(), "a");
        EXPECT_EQ(agent.influence_fn_name(), "b");
    }

    TEST(NexusAgentTest, PerceptionRunsTypedPerceptionsBeforeTheFunction)
    {
        std::vector<std::string> order;
        Agent agent(1.0F,
            [&order](const PerceptionContext&) { order.emplace_back("fn"); },
            [](const InfluenceContext&) { });
        agent.add_perception<int>([&order](const PerceptionContext&) {
                 order.emplace_back("source");
                 return 1;
             })
            ->add_target([&order](const int&) { order.emplace_back("target"); });

        agent.invoke_perception({});

        EXPECT_EQ(order, (std::vector<std::string> { "source", "target", "fn" }));
    }

    TEST(NexusAgentTest, InfluenceRunsTheFunctionBeforeTypedInfluences)
    {
        std::vector<std::string> order;
        Agent agent(1.0F,
            [](const PerceptionContext&) { },
            [&order](const InfluenceContext&) { order.emplace_back("fn"); });
        agent.add_influence<int>([&order](const InfluenceContext&) {
                 order.emplace_back("producer");
                 return 1;
             })
            ->add_target([&order](const int&) { order.emplace_back("target"); });

        agent.invoke_influence({});

        EXPECT_EQ(order, (std::vector<std::string> { "fn", "producer", "target" }));
    }

    TEST(NexusAgentTest, TypedInfluencesRunInTheOrderAdded)
    {
        std::vector<int> order;
        Agent agent;
        for (int i = 0; i < 3; ++i) {
            agent.add_influence<int>([i](const InfluenceContext&) { return i; })
                ->add_target([&order](const int& v) { order.push_back(v); });
        }

        agent.invoke_influence({});

        EXPECT_EQ(order, (std::vector<int> { 0, 1, 2 }));
    }

    TEST(NexusAgentTest, AnInfluenceKeepsEveryTargetAdded)
    {
        int first {};
        int second {};
        Agent agent;
        auto influence = agent.add_influence<int>([](const InfluenceContext&) { return 1; });
        influence->add_target([&first](const int&) { ++first; });
        influence->add_target([&second](const int&) { ++second; });

        agent.invoke_influence({});

        EXPECT_EQ(first, 1);
        EXPECT_EQ(second, 1);
    }

    TEST(NexusAgentTest, RemoveInfluenceStopsOnlyThatOne)
    {
        int kept {};
        int removed {};
        Agent agent;
        auto keep = agent.add_influence<int>([](const InfluenceContext&) { return 1; });
        keep->add_target([&kept](const int&) { ++kept; });
        auto drop = agent.add_influence<int>([](const InfluenceContext&) { return 1; });
        drop->add_target([&removed](const int&) { ++removed; });

        agent.remove_influence(drop);
        agent.invoke_influence({});

        EXPECT_EQ(kept, 1);
        EXPECT_EQ(removed, 0);
    }

    TEST(NexusAgentTest, ClearInfluencesStopsEverything)
    {
        int received {};
        Agent agent;
        agent.add_influence<int>([](const InfluenceContext&) { return 1; })
            ->add_target([&received](const int&) { ++received; });

        agent.clear_influences();
        agent.invoke_influence({});

        EXPECT_EQ(received, 0);
    }

    TEST(NexusAgentTest, RemovePerceptionStopsOnlyThatOne)
    {
        int kept {};
        int removed {};
        Agent agent;
        auto keep = agent.add_perception<int>([](const PerceptionContext&) { return 1; });
        keep->add_target([&kept](const int&) { ++kept; });
        auto drop = agent.add_perception<int>([](const PerceptionContext&) { return 1; });
        drop->add_target([&removed](const int&) { ++removed; });

        agent.remove_perception(drop);
        agent.invoke_perception({});

        EXPECT_EQ(kept, 1);
        EXPECT_EQ(removed, 0);
    }

    TEST(NexusAgentTest, ClearPerceptionsStopsEverything)
    {
        int received {};
        Agent agent;
        agent.add_perception<int>([](const PerceptionContext&) { return 1; })
            ->add_target([&received](const int&) { ++received; });

        agent.clear_perceptions();
        agent.invoke_perception({});

        EXPECT_EQ(received, 0);
    }

    TEST(NexusAgentTest, InvokeWithNothingSetIsHarmless)
    {
        Agent agent;
        EXPECT_NO_THROW(agent.invoke_perception({}));
        EXPECT_NO_THROW(agent.invoke_influence({}));
    }

    TEST(NexusAgentTest, AttachNullBufferLeavesItDetached)
    {
        Agent agent;
        agent.attach(nullptr);
        EXPECT_FALSE(agent.attached());
    }

    TEST(NexusAgentTest, DetachWithoutAttachmentIsHarmless)
    {
        Agent agent;
        EXPECT_NO_THROW(agent.detach());
        EXPECT_NO_THROW(agent.recenter());
        EXPECT_FALSE(agent.attached());
    }

}
}
