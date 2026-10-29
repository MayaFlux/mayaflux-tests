#include "gtest/gtest.h"

#include "MayaFlux/Nexus/Fabric.hpp"
#include "MayaFlux/Vruta/EventManager.hpp"
#include "MayaFlux/Vruta/Scheduler.hpp"

namespace MayaFlux::Test {
namespace {

    using Kinesis::AABB3D;
    using Nexus::Agent;
    using Nexus::CrossingContext;
    using Nexus::Emitter;
    using Nexus::Expanse;
    using Nexus::Fabric;
    using Nexus::InfluenceContext;
    using Nexus::PerceptionContext;
    using Nexus::Sensor;

    class NexusFabricTest : public ::testing::Test {
    protected:
        std::shared_ptr<Emitter> add_emitter(
            const std::optional<glm::vec3>& at = std::nullopt,
            Emitter::InfluenceFn fn = {})
        {
            auto emitter = std::make_shared<Emitter>(std::move(fn));
            if (at) {
                emitter->set_position(*at);
            }
            fabric.wire(emitter).finalise();
            return emitter;
        }

        std::shared_ptr<Sensor> add_sensor(
            const std::optional<glm::vec3>& at = std::nullopt,
            float radius = 1.0F,
            Sensor::PerceptionFn fn = {})
        {
            auto sensor = std::make_shared<Sensor>(radius, std::move(fn));
            if (at) {
                sensor->set_position(*at);
            }
            fabric.wire(sensor).finalise();
            return sensor;
        }

        std::shared_ptr<Agent> add_agent(
            const std::optional<glm::vec3>& at = std::nullopt,
            float radius = 1.0F,
            Agent::PerceptionFn perception = {},
            Agent::InfluenceFn influence = {})
        {
            auto agent = std::make_shared<Agent>(radius, std::move(perception), std::move(influence));
            if (at) {
                agent->set_position(*at);
            }
            fabric.wire(agent).finalise();
            return agent;
        }

        std::shared_ptr<Emitter> add_recording_emitter(std::vector<uint32_t>& order)
        {
            auto slot = std::make_shared<uint32_t>();
            auto emitter = std::make_shared<Emitter>([&order, slot](const InfluenceContext&) { order.push_back(*slot); });
            fabric.wire(emitter).finalise();
            *slot = emitter->id();
            return emitter;
        }

        Vruta::TaskScheduler scheduler { 48000 };
        Vruta::EventManager events;
        Fabric fabric { scheduler, events, 1.0F };
    };

    TEST_F(NexusFabricTest, IdsStartAtOneAndAreSharedAcrossKinds)
    {
        const auto emitter = add_emitter();
        const auto sensor = add_sensor();
        const auto agent = add_agent();

        EXPECT_EQ(emitter->id(), 1U);
        EXPECT_EQ(sensor->id(), 2U);
        EXPECT_EQ(agent->id(), 3U);
        EXPECT_EQ(fabric.all_ids().size(), 3U);
    }

    TEST_F(NexusFabricTest, NameAndIdAreSettable)
    {
        EXPECT_TRUE(fabric.name().empty());
        EXPECT_EQ(fabric.id(), 0U);

        fabric.set_name("stage");
        fabric.set_id(12);

        EXPECT_EQ(fabric.name(), "stage");
        EXPECT_EQ(fabric.id(), 12U);
    }

    TEST_F(NexusFabricTest, KindReportsWhatWasWired)
    {
        const auto emitter = add_emitter();
        const auto sensor = add_sensor();
        const auto agent = add_agent();

        EXPECT_EQ(fabric.kind(emitter->id()), Fabric::Kind::Emitter);
        EXPECT_EQ(fabric.kind(sensor->id()), Fabric::Kind::Sensor);
        EXPECT_EQ(fabric.kind(agent->id()), Fabric::Kind::Agent);
    }

    TEST_F(NexusFabricTest, KindOfAnUnknownIdThrows)
    {
        EXPECT_THROW((void)fabric.kind(42), std::out_of_range);
    }

    TEST_F(NexusFabricTest, GettersReturnTheWiredObjectOrNull)
    {
        const auto emitter = add_emitter();
        const auto sensor = add_sensor();
        const auto agent = add_agent();

        EXPECT_EQ(fabric.get_emitter(emitter->id()), emitter);
        EXPECT_EQ(fabric.get_sensor(sensor->id()), sensor);
        EXPECT_EQ(fabric.get_agent(agent->id()), agent);

        EXPECT_EQ(fabric.get_sensor(emitter->id()), nullptr);
        EXPECT_EQ(fabric.get_agent(sensor->id()), nullptr);
        EXPECT_EQ(fabric.get_emitter(agent->id()), nullptr);
        EXPECT_EQ(fabric.get_emitter(99), nullptr);
    }

    TEST_F(NexusFabricTest, AWiredButUnfinalisedEntityDoesNotFireOnCommit)
    {
        int fired {};
        auto emitter = std::make_shared<Emitter>([&fired](const InfluenceContext&) { ++fired; });
        [[maybe_unused]] auto pending = fabric.wire(emitter);

        fabric.commit();

        EXPECT_EQ(fired, 0);
        EXPECT_EQ(fabric.wiring_for(emitter->id()), nullptr);
        EXPECT_EQ(fabric.get_emitter(emitter->id()), emitter);
    }

    TEST_F(NexusFabricTest, FireRunsAnUnfinalisedEntityOnDemand)
    {
        int fired {};
        auto emitter = std::make_shared<Emitter>([&fired](const InfluenceContext&) { ++fired; });
        [[maybe_unused]] auto pending = fabric.wire(emitter);

        fabric.fire(emitter->id());

        EXPECT_EQ(fired, 1);
    }

    TEST_F(NexusFabricTest, FinaliseWithNoSchedulingMakesTheEntityFireOnEveryCommit)
    {
        int fired {};
        const auto emitter = add_emitter(std::nullopt, [&fired](const InfluenceContext&) { ++fired; });

        fabric.commit();
        fabric.commit();
        fabric.commit();

        EXPECT_EQ(fired, 3);
        EXPECT_NE(fabric.wiring_for(emitter->id()), nullptr);
    }

    TEST_F(NexusFabricTest, FireRunsOnlyTheNamedEntity)
    {
        int first {};
        int second {};
        const auto a = add_emitter(std::nullopt, [&first](const InfluenceContext&) { ++first; });
        add_emitter(std::nullopt, [&second](const InfluenceContext&) { ++second; });

        fabric.fire(a->id());

        EXPECT_EQ(first, 1);
        EXPECT_EQ(second, 0);
    }

    TEST_F(NexusFabricTest, FireOfAnUnknownIdIsHarmless)
    {
        EXPECT_NO_THROW(fabric.fire(77));
    }

    TEST_F(NexusFabricTest, CommitFiresInWiringOrder)
    {
        std::vector<uint32_t> order;
        std::vector<uint32_t> wired;
        wired.reserve(8);
        for (int i = 0; i < 8; ++i) {
            wired.push_back(add_recording_emitter(order)->id());
        }

        fabric.commit();

        EXPECT_EQ(order, wired);
    }

    TEST_F(NexusFabricTest, CommitOrderIsRepeatableAcrossCommits)
    {
        std::vector<uint32_t> order;
        for (int i = 0; i < 8; ++i) {
            add_recording_emitter(order);
        }

        fabric.commit();
        const std::vector<uint32_t> first = order;
        order.clear();
        fabric.commit();

        EXPECT_EQ(order, first);
    }

    TEST_F(NexusFabricTest, AllIdsFollowInsertionOrder)
    {
        std::vector<uint32_t> wired;
        wired.push_back(add_emitter()->id());
        wired.push_back(add_sensor()->id());
        wired.push_back(add_agent()->id());
        wired.push_back(add_emitter()->id());
        wired.push_back(add_sensor()->id());
        wired.push_back(add_agent()->id());

        EXPECT_EQ(fabric.all_ids(), wired);
    }

    TEST_F(NexusFabricTest, RemoveDropsTheEntity)
    {
        const auto keep = add_emitter();
        const auto drop = add_emitter();

        fabric.remove(drop->id());

        EXPECT_EQ(fabric.all_ids(), (std::vector<uint32_t> { keep->id() }));
        EXPECT_EQ(fabric.get_emitter(drop->id()), nullptr);
        EXPECT_EQ(fabric.wiring_for(drop->id()), nullptr);
    }

    TEST_F(NexusFabricTest, RemovedEntitiesStopFiring)
    {
        int fired {};
        const auto emitter = add_emitter(std::nullopt, [&fired](const InfluenceContext&) { ++fired; });

        fabric.commit();
        fabric.remove(emitter->id());
        fabric.commit();

        EXPECT_EQ(fired, 1);
    }

    TEST_F(NexusFabricTest, RemovingAnUnknownIdIsHarmless)
    {
        add_emitter();
        EXPECT_NO_THROW(fabric.remove(99));
        EXPECT_EQ(fabric.all_ids().size(), 1U);
    }

    TEST_F(NexusFabricTest, IdsAreNotReusedAfterRemoval)
    {
        const auto first = add_emitter();
        const uint32_t removed = first->id();
        fabric.remove(removed);

        const auto second = add_emitter();

        EXPECT_NE(second->id(), removed);
    }

    TEST_F(NexusFabricTest, QueriesReadThePublishedSnapshot)
    {
        add_emitter(glm::vec3(0.0F));

        EXPECT_TRUE(fabric.within_radius(glm::vec3(0.0F), 1.0F).empty());

        fabric.commit();

        EXPECT_EQ(fabric.within_radius(glm::vec3(0.0F), 1.0F).size(), 1U);
    }

    TEST_F(NexusFabricTest, WithinRadiusReportsSquaredDistances)
    {
        add_emitter(glm::vec3(3.0F, 0.0F, 0.0F));
        add_emitter(glm::vec3(20.0F, 0.0F, 0.0F));
        fabric.commit();

        const auto hits = fabric.within_radius(glm::vec3(0.0F), 5.0F);

        ASSERT_EQ(hits.size(), 1U);
        EXPECT_FLOAT_EQ(hits[0].distance_sq, 9.0F);
    }

    TEST_F(NexusFabricTest, KNearestIsSortedByAscendingDistanceAndLimited)
    {
        add_emitter(glm::vec3(3.0F, 0.0F, 0.0F));
        add_emitter(glm::vec3(1.0F, 0.0F, 0.0F));
        add_emitter(glm::vec3(2.0F, 0.0F, 0.0F));
        add_emitter(glm::vec3(9.0F, 0.0F, 0.0F));
        fabric.commit();

        const auto hits = fabric.k_nearest(glm::vec3(0.0F), 3);

        ASSERT_EQ(hits.size(), 3U);
        EXPECT_FLOAT_EQ(hits[0].distance_sq, 1.0F);
        EXPECT_FLOAT_EQ(hits[1].distance_sq, 4.0F);
        EXPECT_FLOAT_EQ(hits[2].distance_sq, 9.0F);
    }

    TEST_F(NexusFabricTest, EntitiesWithoutAPositionAreNotIndexed)
    {
        add_emitter();
        add_sensor();
        add_agent();
        fabric.commit();

        EXPECT_TRUE(fabric.within_radius(glm::vec3(0.0F), 10.0F).empty());
    }

    TEST_F(NexusFabricTest, APositionGivenAfterWiringIsIndexedOnTheNextCommit)
    {
        const auto emitter = add_emitter();
        fabric.commit();
        ASSERT_TRUE(fabric.within_radius(glm::vec3(0.0F), 10.0F).empty());

        emitter->set_position(glm::vec3(1.0F, 0.0F, 0.0F));
        fabric.commit();

        EXPECT_EQ(fabric.within_radius(glm::vec3(0.0F), 5.0F).size(), 1U);
    }

    TEST_F(NexusFabricTest, AMovedEntityIsReindexedOnTheNextCommit)
    {
        const auto emitter = add_emitter(glm::vec3(0.0F));
        fabric.commit();

        emitter->set_position(glm::vec3(50.0F, 0.0F, 0.0F));
        fabric.commit();

        EXPECT_TRUE(fabric.within_radius(glm::vec3(0.0F), 5.0F).empty());
        EXPECT_EQ(fabric.within_radius(glm::vec3(50.0F, 0.0F, 0.0F), 5.0F).size(), 1U);
    }

    TEST_F(NexusFabricTest, AClearedPositionLeavesSpatialQueries)
    {
        const auto emitter = add_emitter(glm::vec3(0.0F));
        fabric.commit();
        ASSERT_EQ(fabric.within_radius(glm::vec3(0.0F), 1.0F).size(), 1U);

        emitter->clear_position();
        fabric.commit();

        EXPECT_TRUE(fabric.within_radius(glm::vec3(0.0F), 1.0F).empty());
    }

    TEST_F(NexusFabricTest, ARemovedEntityLeavesSpatialQueriesOnTheNextCommit)
    {
        const auto emitter = add_emitter(glm::vec3(0.0F));
        fabric.commit();
        ASSERT_EQ(fabric.within_radius(glm::vec3(0.0F), 1.0F).size(), 1U);

        fabric.remove(emitter->id());
        fabric.commit();

        EXPECT_TRUE(fabric.within_radius(glm::vec3(0.0F), 1.0F).empty());
    }

    TEST_F(NexusFabricTest, QueryIdsAreEntityIdsEvenWhenAnUnpositionedEntityWasWiredFirst)
    {
        add_sensor();
        const auto positioned = add_emitter(glm::vec3(0.0F));
        fabric.commit();

        const auto hits = fabric.within_radius(glm::vec3(0.0F), 0.5F);

        ASSERT_EQ(hits.size(), 1U);
        EXPECT_EQ(hits[0].id, positioned->id());
    }

    TEST_F(NexusFabricTest, EmitterContextCarriesItsPositionAndParameters)
    {
        InfluenceContext seen;
        const auto emitter = add_emitter(glm::vec3(1.0F, 2.0F, 3.0F),
            [&seen](const InfluenceContext& ctx) { seen = ctx; });
        emitter->set_intensity(0.5F);
        emitter->set_radius(3.0F);
        emitter->set_color(glm::vec3(0.1F, 0.2F, 0.3F));
        emitter->set_size(7.0F);

        fabric.commit();

        EXPECT_EQ(seen.position, glm::vec3(1.0F, 2.0F, 3.0F));
        EXPECT_FLOAT_EQ(seen.intensity, 0.5F);
        EXPECT_FLOAT_EQ(seen.radius, 3.0F);
        ASSERT_TRUE(seen.color.has_value());
        EXPECT_EQ(*seen.color, glm::vec3(0.1F, 0.2F, 0.3F));
        ASSERT_TRUE(seen.size.has_value());
        EXPECT_FLOAT_EQ(*seen.size, 7.0F);
        EXPECT_FALSE(seen.cursor_pos.has_value());
    }

    TEST_F(NexusFabricTest, EmitterWithoutAPositionStillFiresAtTheOrigin)
    {
        InfluenceContext seen;
        seen.position = glm::vec3(9.0F);
        add_emitter(std::nullopt, [&seen](const InfluenceContext& ctx) { seen = ctx; });

        fabric.commit();

        EXPECT_EQ(seen.position, glm::vec3(0.0F));
    }

    TEST_F(NexusFabricTest, SensorPerceivesOnlyWhatLiesWithinItsRadius)
    {
        std::vector<float> distances;
        add_sensor(glm::vec3(0.0F), 2.0F, [&distances](const PerceptionContext& ctx) {
            distances.clear();
            for (const auto& hit : ctx.spatial_results) {
                distances.push_back(hit.distance_sq);
            }
        });
        add_emitter(glm::vec3(1.0F, 0.0F, 0.0F));
        add_emitter(glm::vec3(5.0F, 0.0F, 0.0F));

        fabric.commit();

        EXPECT_EQ(std::ranges::count_if(distances, [](float d) { return d > 0.9F && d < 1.1F; }), 1);
        EXPECT_EQ(std::ranges::count_if(distances, [](float d) { return d > 4.0F; }), 0);
    }

    TEST_F(NexusFabricTest, SensorPerceptionIdsAreEntityIds)
    {
        std::set<uint32_t> ids;
        add_sensor();
        const auto sensor = add_sensor(glm::vec3(0.0F), 2.0F, [&ids](const PerceptionContext& ctx) {
            ids.clear();
            for (const auto& hit : ctx.spatial_results) {
                ids.insert(hit.id);
            }
        });
        const auto near_emitter = add_emitter(glm::vec3(1.0F, 0.0F, 0.0F));

        fabric.commit();

        EXPECT_TRUE(ids.contains(near_emitter->id()));
        EXPECT_TRUE(ids.contains(sensor->id()));
    }

    TEST_F(NexusFabricTest, SensorContextCarriesPositionAndQueryRadius)
    {
        PerceptionContext seen;
        add_sensor(glm::vec3(1.0F, 2.0F, 3.0F), 4.0F, [&seen](const PerceptionContext& ctx) { seen = ctx; });

        fabric.commit();

        EXPECT_EQ(seen.position, glm::vec3(1.0F, 2.0F, 3.0F));
        EXPECT_FLOAT_EQ(seen.radius, 4.0F);
    }

    TEST_F(NexusFabricTest, SensorWithoutAPositionPerceivesAnEmptyNeighbourhood)
    {
        size_t count { 99 };
        float radius {};
        add_sensor(std::nullopt, 6.0F, [&count, &radius](const PerceptionContext& ctx) {
            count = ctx.spatial_results.size();
            radius = ctx.radius;
        });
        add_emitter(glm::vec3(0.0F));

        fabric.commit();

        EXPECT_EQ(count, 0U);
        EXPECT_FLOAT_EQ(radius, 6.0F);
    }

    TEST_F(NexusFabricTest, AgentPerceivesBeforeItInfluences)
    {
        std::vector<std::string> order;
        add_agent(
            glm::vec3(0.0F), 1.0F,
            [&order](const PerceptionContext&) { order.emplace_back("perception"); },
            [&order](const InfluenceContext&) { order.emplace_back("influence"); });

        fabric.commit();

        EXPECT_EQ(order, (std::vector<std::string> { "perception", "influence" }));
    }

    TEST_F(NexusFabricTest, AgentContextsCarryPositionOrientationAndParameters)
    {
        PerceptionContext perceived;
        InfluenceContext influenced;
        const auto agent = add_agent(
            glm::vec3(1.0F, 2.0F, 3.0F), 4.0F,
            [&perceived](const PerceptionContext& ctx) { perceived = ctx; },
            [&influenced](const InfluenceContext& ctx) { influenced = ctx; });
        agent->set_orientation(glm::angleAxis(glm::radians(90.0F), glm::vec3(0.0F, 1.0F, 0.0F)));
        agent->set_intensity(0.25F);

        fabric.commit();

        EXPECT_EQ(perceived.position, glm::vec3(1.0F, 2.0F, 3.0F));
        EXPECT_FLOAT_EQ(perceived.radius, 4.0F);
        EXPECT_TRUE(perceived.orientation.has_value());
        EXPECT_EQ(influenced.position, glm::vec3(1.0F, 2.0F, 3.0F));
        EXPECT_TRUE(influenced.orientation.has_value());
        EXPECT_FLOAT_EQ(influenced.intensity, 0.25F);
    }

    TEST_F(NexusFabricTest, AgentWithoutAPositionStillPerceivesAndInfluences)
    {
        size_t neighbours { 99 };
        int influenced {};
        add_agent(
            std::nullopt, 3.0F,
            [&neighbours](const PerceptionContext& ctx) { neighbours = ctx.spatial_results.size(); },
            [&influenced](const InfluenceContext&) { ++influenced; });
        add_emitter(glm::vec3(0.0F));

        fabric.commit();

        EXPECT_EQ(neighbours, 0U);
        EXPECT_EQ(influenced, 1);
    }

    class NexusFabricExpanseTest : public NexusFabricTest {
    protected:
        std::shared_ptr<Expanse> add_box(const AABB3D& bounds)
        {
            auto expanse = std::make_shared<Expanse>(
                bounds,
                [this](uint32_t id) { entered.push_back(id); },
                [this](uint32_t id) { exited.push_back(id); });
            fabric.add_expanse(expanse);
            return expanse;
        }

        std::vector<uint32_t> entered;
        std::vector<uint32_t> exited;
    };

    TEST_F(NexusFabricExpanseTest, AddReturnsTheExpanseIdAndRegistersIt)
    {
        const auto expanse = std::make_shared<Expanse>(AABB3D::from_center(glm::vec3(0.0F), glm::vec3(1.0F)));

        const uint32_t id = fabric.add_expanse(expanse);

        EXPECT_EQ(id, expanse->id());
        EXPECT_EQ(fabric.get_expanse(id), expanse);
        EXPECT_EQ(fabric.all_expanse_ids(), (std::vector<uint32_t> { id }));
        EXPECT_EQ(fabric.get_expanse(id + 1000), nullptr);
    }

    TEST_F(NexusFabricExpanseTest, AddingTheSameIdTwiceKeepsTheFirst)
    {
        const auto expanse = std::make_shared<Expanse>(AABB3D::from_center(glm::vec3(0.0F), glm::vec3(1.0F)));

        fabric.add_expanse(expanse);
        fabric.add_expanse(expanse);

        EXPECT_EQ(fabric.all_expanse_ids().size(), 1U);
    }

    TEST_F(NexusFabricExpanseTest, AllExpanseIdsFollowInsertionOrder)
    {
        std::vector<uint32_t> added;
        added.reserve(6);
        for (int i = 0; i < 6; ++i) {
            added.push_back(fabric.add_expanse(std::make_shared<Expanse>(Expanse::ContainsFn {})));
        }

        EXPECT_EQ(fabric.all_expanse_ids(), added);
    }

    TEST_F(NexusFabricExpanseTest, CommitDrivesEntryAndExitWithEntityIds)
    {
        add_box(AABB3D::from_center(glm::vec3(0.0F), glm::vec3(1.0F)));
        const auto emitter = add_emitter(glm::vec3(5.0F, 0.0F, 0.0F));

        fabric.commit();
        EXPECT_TRUE(entered.empty());

        emitter->set_position(glm::vec3(0.0F));
        fabric.commit();
        fabric.commit();
        EXPECT_EQ(entered, (std::vector<uint32_t> { emitter->id() }));

        emitter->set_position(glm::vec3(5.0F, 0.0F, 0.0F));
        fabric.commit();
        EXPECT_EQ(exited, (std::vector<uint32_t> { emitter->id() }));
    }

    TEST_F(NexusFabricExpanseTest, CrossingIdsAreEntityIdsEvenWhenAnUnpositionedEntityWasWiredFirst)
    {
        add_box(AABB3D::from_center(glm::vec3(0.0F), glm::vec3(1.0F)));
        add_sensor();
        const auto positioned = add_emitter(glm::vec3(0.0F));

        fabric.commit();

        EXPECT_EQ(entered, (std::vector<uint32_t> { positioned->id() }));
    }

    TEST_F(NexusFabricExpanseTest, ExpansesAreEvaluatedBeforeEntitiesFire)
    {
        std::vector<std::string> order;
        auto expanse = std::make_shared<Expanse>(
            AABB3D::from_center(glm::vec3(0.0F), glm::vec3(1.0F)),
            [&order](uint32_t) { order.emplace_back("enter"); });
        fabric.add_expanse(expanse);
        add_emitter(glm::vec3(0.0F), [&order](const InfluenceContext&) { order.emplace_back("fire"); });

        fabric.commit();

        EXPECT_EQ(order, (std::vector<std::string> { "enter", "fire" }));
    }

    TEST_F(NexusFabricExpanseTest, ARemovedEntityExitsOnTheNextCommit)
    {
        add_box(AABB3D::from_center(glm::vec3(0.0F), glm::vec3(1.0F)));
        const auto emitter = add_emitter(glm::vec3(0.0F));
        fabric.commit();
        ASSERT_EQ(entered.size(), 1U);

        fabric.remove(emitter->id());
        fabric.commit();

        EXPECT_EQ(exited, (std::vector<uint32_t> { emitter->id() }));
    }

    TEST_F(NexusFabricExpanseTest, RemoveExpanseFiresExitsForEveryoneInside)
    {
        const auto expanse = add_box(AABB3D::from_center(glm::vec3(0.0F), glm::vec3(1.0F)));
        const auto emitter = add_emitter(glm::vec3(0.0F));
        fabric.commit();

        fabric.remove_expanse(expanse->id());

        EXPECT_EQ(exited, (std::vector<uint32_t> { emitter->id() }));
        EXPECT_EQ(fabric.get_expanse(expanse->id()), nullptr);
        EXPECT_TRUE(fabric.all_expanse_ids().empty());
    }

    TEST_F(NexusFabricExpanseTest, RemovingAnUnknownExpanseIsHarmless)
    {
        EXPECT_NO_THROW(fabric.remove_expanse(12345));
    }

    TEST_F(NexusFabricExpanseTest, ARemovedExpanseStopsBeingEvaluated)
    {
        const auto expanse = add_box(AABB3D::from_center(glm::vec3(0.0F), glm::vec3(1.0F)));
        const auto emitter = add_emitter(glm::vec3(5.0F, 0.0F, 0.0F));
        fabric.remove_expanse(expanse->id());

        emitter->set_position(glm::vec3(0.0F));
        fabric.commit();

        EXPECT_TRUE(entered.empty());
    }

    TEST_F(NexusFabricExpanseTest, DestroyingTheFabricFiresExitsForEveryoneInside)
    {
        const auto expanse = std::make_shared<Expanse>(
            AABB3D::from_center(glm::vec3(0.0F), glm::vec3(1.0F)),
            [this](uint32_t id) { entered.push_back(id); },
            [this](uint32_t id) { exited.push_back(id); });
        uint32_t entity {};
        {
            Fabric local(scheduler, events, 1.0F);
            local.add_expanse(expanse);
            auto emitter = std::make_shared<Emitter>();
            emitter->set_position(glm::vec3(0.0F));
            local.wire(emitter).finalise();
            entity = emitter->id();
            local.commit();
            ASSERT_EQ(entered.size(), 1U);
        }

        EXPECT_EQ(exited, (std::vector<uint32_t> { entity }));
        EXPECT_EQ(expanse->occupants(0), nullptr);
    }

    TEST_F(NexusFabricExpanseTest, TypedCrossingsCarryTheFabricId)
    {
        fabric.set_id(9);
        const auto expanse = add_box(AABB3D::from_center(glm::vec3(0.0F), glm::vec3(1.0F)));
        std::vector<CrossingContext> seen;
        expanse->add_entry<CrossingContext>([](const CrossingContext& c) { return c; })
            ->add_target([&seen](const CrossingContext& c) { seen.push_back(c); });
        add_emitter(glm::vec3(0.0F));

        fabric.commit();

        ASSERT_EQ(seen.size(), 1U);
        EXPECT_EQ(seen[0].fabric_id, 9U);
    }

    TEST_F(NexusFabricExpanseTest, OneExpanseOnTwoFabricsKeepsSeparateOccupants)
    {
        Fabric other(scheduler, events, 1.0F);
        fabric.set_id(1);
        other.set_id(2);
        const auto expanse = add_box(AABB3D::from_center(glm::vec3(0.0F), glm::vec3(1.0F)));
        other.add_expanse(expanse);

        add_emitter(glm::vec3(0.0F));
        auto outside = std::make_shared<Emitter>();
        outside->set_position(glm::vec3(9.0F, 0.0F, 0.0F));
        other.wire(outside).finalise();

        fabric.commit();
        other.commit();

        EXPECT_NE(expanse->occupants(1), nullptr);
        EXPECT_EQ(expanse->occupants(2), nullptr);
        EXPECT_EQ(expanse->occupied_fabrics(), (std::vector<uint32_t> { 1 }));
    }

}
}
