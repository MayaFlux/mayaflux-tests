#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "../support/frame_scheduler.hpp"

#include "MayaFlux/Buffers/BufferManager.hpp"
#include "MayaFlux/Buffers/Network/MeshNetworkBuffer.hpp"
#include "MayaFlux/Kriya/Course.hpp"
#include "MayaFlux/Kakshya/Semantic/MeshAssemblyContainer.hpp"
#include "MayaFlux/Nexus/Principals/Presence.hpp"
#include "MayaFlux/Nexus/State/Orchestrator.hpp"
#include "MayaFlux/Vruta/EventManager.hpp"
#include "MayaFlux/Vruta/Scheduler.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Kakshya::MeshAssemblyContainer;
    using Nexus::Agent;
    using Nexus::Emitter;
    using Nexus::Fabric;
    using Nexus::Locus;
    using Nexus::Sensor;
    using Nexus::StateOrchestrator;

    /** Row 0 a joint at x = 10, then a triangle, a light and a camera under it, one unit apart in y. */
    struct OrchestratedRoom {
        std::shared_ptr<MeshAssemblyContainer> assembly = std::make_shared<MeshAssemblyContainer>();

        OrchestratedRoom()
        {
            const auto shape = Kakshya::MeshData::from_positions(
                std::vector<glm::vec3> { { 0.0F, 0.0F, 0.0F }, { 1.0F, 0.0F, 0.0F }, { 0.0F, 1.0F, 0.0F } },
                std::vector<uint32_t> { 0, 1, 2 })
                                   .value();

            const auto tri = assembly->add_source({ .geometry = shape, .name = "tri" });
            const auto lamp = assembly->add_source(
                { .geometry = Kakshya::MeshAssemblyLight { .color = glm::vec3(0.25F, 0.5F, 0.75F) }, .name = "lamp" });
            const auto lens = assembly->add_source(
                { .geometry = Kakshya::MeshAssemblyLens { .near = 0.5, .far = 50.0 }, .name = "lens" });

            const auto root = assembly->add_placement(
                                          { .name = "room", .local = glm::translate(glm::mat4(1.0F), glm::vec3(10.0F, 0.0F, 0.0F)) })
                                  .value();
            const auto under = [&](const char* name, size_t source, float height) {
                std::ignore = assembly->add_placement({ .name = name,
                    .parent = root,
                    .source = source,
                    .local = glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, height, 0.0F)) });
            };
            under("tri", tri, 1.0F);
            under("lamp", lamp, 2.0F);
            under("eye", lens, 3.0F);
        }
    };

    TEST(NexusStateOrchestratorTest, ARowBecomesWhatItsKindSuggestsAndAJointNothing)
    {
        const OrchestratedRoom room;

        const auto [realization, rows] = StateOrchestrator { nullptr }.orchestrate(room.assembly);

        ASSERT_EQ(rows.size(), 3U);
        EXPECT_EQ(rows[0].first, 1U);
        EXPECT_EQ(rows[1].first, 2U);
        EXPECT_EQ(rows[2].first, 3U);
        EXPECT_TRUE(std::holds_alternative<std::shared_ptr<Agent>>(rows[0].second));
        EXPECT_TRUE(std::holds_alternative<std::shared_ptr<Emitter>>(rows[1].second));
        ASSERT_TRUE(std::holds_alternative<std::shared_ptr<Agent>>(rows[2].second));
        EXPECT_NE(std::dynamic_pointer_cast<Locus>(std::get<std::shared_ptr<Agent>>(rows[2].second)), nullptr);
    }

    TEST(NexusStateOrchestratorTest, PrincipalsStandWhereTheirRowsAreDrawnOrRest)
    {
        const OrchestratedRoom room;

        const auto [realization, rows] = StateOrchestrator { nullptr }.orchestrate(room.assembly);

        ASSERT_EQ(rows.size(), 3U);
        const auto agent = std::get<std::shared_ptr<Agent>>(rows[0].second);
        const auto lamp = std::get<std::shared_ptr<Emitter>>(rows[1].second);
        ASSERT_TRUE(agent->position().has_value());
        EXPECT_NEAR(agent->position()->x, 10.0F + 1.0F / 3.0F, 1e-4F);
        EXPECT_NEAR(agent->position()->y, 1.0F + 1.0F / 3.0F, 1e-4F);
        EXPECT_EQ(lamp->position(), glm::vec3(10.0F, 2.0F, 0.0F));
        EXPECT_EQ(lamp->color(), glm::vec3(0.25F, 0.5F, 0.75F));
        ASSERT_TRUE(lamp->orientation().has_value());
        EXPECT_NEAR(std::abs(glm::dot(*lamp->orientation(), glm::quat(1.0F, 0.0F, 0.0F, 0.0F))), 1.0F, 1e-5F);
    }

    TEST(NexusStateOrchestratorTest, ALocusTakesTheRowsViewpointAndLens)
    {
        const OrchestratedRoom room;

        const auto [realization, rows] = StateOrchestrator { nullptr }.orchestrate(room.assembly);

        ASSERT_EQ(rows.size(), 3U);
        const auto locus = std::dynamic_pointer_cast<Locus>(std::get<std::shared_ptr<Agent>>(rows[2].second));
        ASSERT_NE(locus, nullptr);
        ASSERT_TRUE(locus->position().has_value());
        EXPECT_NEAR(locus->position()->x, 10.0F, 1e-4F);
        EXPECT_NEAR(locus->position()->y, 3.0F, 1e-4F);

        const auto view = locus->view()->read();
        EXPECT_NEAR(view.first[3].x, 10.0F, 1e-4F);
        EXPECT_NEAR(view.second.near, 0.5, 1e-6);
        EXPECT_NEAR(view.second.far, 50.0, 1e-4);
    }

    TEST(NexusStateOrchestratorTest, ARuleDecidesEveryRowAndANullPointerMeansNothing)
    {
        const OrchestratedRoom room;
        StateOrchestrator orchestrator { nullptr };
        orchestrator.set_row_rule(
            [](const std::shared_ptr<const MeshAssemblyContainer>&, size_t row) -> Fabric::Member {
                if (row != 0) {
                    return {};
                }
                return std::make_shared<Sensor>(4.0F);
            });

        const auto [realization, rows] = orchestrator.orchestrate(room.assembly);

        ASSERT_EQ(rows.size(), 1U);
        const auto sensor = std::get<std::shared_ptr<Sensor>>(rows[0].second);
        EXPECT_EQ(sensor->position(), glm::vec3(10.0F, 0.0F, 0.0F));
        EXPECT_FLOAT_EQ(sensor->query_radius(), 4.0F);
    }

    TEST(NexusStateOrchestratorTest, WhatARuleAlreadySetIsKept)
    {
        const OrchestratedRoom room;
        StateOrchestrator orchestrator { nullptr };
        orchestrator.set_row_rule(
            [](const std::shared_ptr<const MeshAssemblyContainer>& assembly, size_t row) -> Fabric::Member {
                if (row != 2) {
                    return StateOrchestrator::default_row(assembly, row);
                }
                auto emitter = std::make_shared<Emitter>();
                emitter->set({ .position = glm::vec3(-1.0F) });
                return emitter;
            });

        const auto [realization, rows] = orchestrator.orchestrate(room.assembly);

        ASSERT_EQ(rows.size(), 3U);
        const auto lamp = std::get<std::shared_ptr<Emitter>>(rows[1].second);
        EXPECT_EQ(lamp->position(), glm::vec3(-1.0F));
        EXPECT_TRUE(lamp->orientation().has_value());
    }

    TEST(NexusStateOrchestratorTest, NothingComesOfNoAssemblyOrAnUnreadableFile)
    {
        StateOrchestrator orchestrator { nullptr };

        const auto [none, no_rows] = orchestrator.orchestrate(std::shared_ptr<const MeshAssemblyContainer> {});
        EXPECT_EQ(none, nullptr);
        EXPECT_TRUE(no_rows.empty());

        const auto [assembly, realization, rows] = orchestrator.orchestrate(std::string("no_such_model.glb"));
        EXPECT_EQ(assembly, nullptr);
        EXPECT_EQ(realization, nullptr);
        EXPECT_TRUE(rows.empty());

        const auto [archive, no_parts, no_archive_rows] = orchestrator.orchestrate(std::string("no_such_scene.abc"));
        EXPECT_EQ(archive, nullptr);
        EXPECT_EQ(no_parts, nullptr);
        EXPECT_TRUE(no_archive_rows.empty());
    }

    TEST(NexusStateOrchestratorTest, EachPieceWorksOnAnyRow)
    {
        const OrchestratedRoom room;
        StateOrchestrator orchestrator { nullptr };

        const auto locus = orchestrator.locus_from(room.assembly, 0);
        const auto emitter = orchestrator.emitter_from(room.assembly, 1);

        ASSERT_NE(locus, nullptr);
        ASSERT_NE(emitter, nullptr);
        ASSERT_TRUE(locus->position().has_value());
        EXPECT_NEAR(locus->position()->x, 10.0F, 1e-4F);
        EXPECT_EQ(emitter->position(), glm::vec3(10.0F, 1.0F, 0.0F));
        EXPECT_FALSE(emitter->color().has_value());
        EXPECT_EQ(emitter->id(), 0U);
        EXPECT_EQ(orchestrator.locus_from(room.assembly, 99), nullptr);
        EXPECT_EQ(orchestrator.emitter_from(room.assembly, 99), nullptr);
        EXPECT_EQ(orchestrator.agent_from(nullptr), nullptr);
    }

    TEST(NexusStateOrchestratorTest, ABufferSlotBecomesTheChildThatIsAskedFor)
    {
        const OrchestratedRoom room;
        StateOrchestrator orchestrator { nullptr };
        const auto [realization, rows] = orchestrator.orchestrate(room.assembly);
        const auto [buffer, slot] = StateOrchestrator::buffer_of(realization, 1);
        ASSERT_NE(buffer, nullptr);
        ASSERT_TRUE(slot.has_value());

        const auto presence = orchestrator.agent_from<Nexus::Presence>(
            buffer, slot, 2.0F,
            Agent::PerceptionFn {}, Agent::InfluenceFn {}, Nexus::Presence::RadiateFn {});

        ASSERT_NE(presence, nullptr);
        ASSERT_TRUE(presence->position().has_value());
        EXPECT_NEAR(presence->position()->y, 1.0F + 1.0F / 3.0F, 1e-4F);
        EXPECT_FLOAT_EQ(presence->query_radius(), 2.0F);
        EXPECT_EQ(StateOrchestrator::buffer_of(realization, 0).first, nullptr);
    }

    TEST(NexusStateOrchestratorTest, RowsAreAskedForByTheTypeOfTheirPrincipal)
    {
        const OrchestratedRoom room;
        const auto [realization, rows] = StateOrchestrator { nullptr }.orchestrate(room.assembly);

        const auto agents = StateOrchestrator::rows_as<Agent>(rows);
        const auto loci = StateOrchestrator::rows_as<Locus>(rows);
        const auto emitters = StateOrchestrator::rows_as<Emitter>(rows);

        ASSERT_EQ(agents.size(), 2U);
        EXPECT_EQ(agents[0].first, 1U);
        EXPECT_EQ(agents[1].first, 3U);
        ASSERT_EQ(loci.size(), 1U);
        EXPECT_EQ(loci[0].first, 3U);
        ASSERT_EQ(emitters.size(), 1U);
        EXPECT_EQ(emitters[0].first, 2U);
        EXPECT_TRUE(StateOrchestrator::rows_as<Sensor>(rows).empty());
        EXPECT_TRUE(StateOrchestrator::rows_as<Nexus::Presence>(rows).empty());
    }

    TEST(NexusStateOrchestratorTest, NothingMovesWithoutARealizationASchedulerOrAMotion)
    {
        const OrchestratedRoom room;
        StateOrchestrator unscheduled { nullptr };
        const StateOrchestrator scheduled { std::make_shared<Vruta::TaskScheduler>(48000) };
        const auto [realization, rows] = unscheduled.orchestrate(room.assembly);
        ASSERT_NE(realization, nullptr);

        const auto [no_scheduler, no_meshes, no_points] = unscheduled.motion_from(realization);
        const auto [no_motion, still_meshes, still_points] = scheduled.motion_from(realization);
        const auto [no_realization, none, nothing] = scheduled.motion_from(nullptr);

        EXPECT_EQ(no_scheduler, nullptr);
        EXPECT_EQ(no_motion, nullptr);
        EXPECT_TRUE(still_meshes.empty());
        EXPECT_TRUE(still_points.empty());
        EXPECT_EQ(no_realization, nullptr);
    }

    TEST(NexusStateOrchestratorTest, WithATapestryPrincipalsAreWiredAndFollowTheirBuffer)
    {
        const OrchestratedRoom room;
        const auto tapestry = std::make_shared<Nexus::Tapestry>(
            std::make_shared<Vruta::TaskScheduler>(48000),
            std::make_shared<Vruta::EventManager>(),
            std::make_shared<Buffers::BufferManager>());
        int wired {};
        StateOrchestrator orchestrator { nullptr, tapestry };
        orchestrator.set_wiring_rule([&wired](Nexus::Wiring&) { ++wired; });

        const auto [realization, rows] = orchestrator.orchestrate(room.assembly);

        ASSERT_EQ(rows.size(), 3U);
        ASSERT_NE(orchestrator.fabric(), nullptr);
        EXPECT_EQ(orchestrator.fabric()->all_ids().size(), 3U);
        EXPECT_EQ(wired, 3);

        const auto agent = std::get<std::shared_ptr<Agent>>(rows[0].second);
        agent->set({ .position = glm::vec3(-5.0F) });
        orchestrator.fabric()->fire(agent->id());

        EXPECT_NEAR(agent->position()->y, 1.0F + 1.0F / 3.0F, 1e-4F);
    }

    TEST(NexusStateOrchestratorTest, PiecesAreMadeFromPlainValuesToo)
    {
        StateOrchestrator orchestrator { nullptr };
        const glm::mat4 placed = glm::translate(glm::mat4(1.0F), glm::vec3(1.0F, 2.0F, 3.0F));

        const auto lamp = orchestrator.emitter_from(Kakshya::MeshAssemblyLight { .color = glm::vec3(0.5F) }, placed);
        const auto eye = orchestrator.locus_from(Kinesis::Viewpoint { placed, Kinesis::Frustum {} });

        ASSERT_NE(lamp, nullptr);
        ASSERT_NE(eye, nullptr);
        EXPECT_EQ(lamp->position(), glm::vec3(1.0F, 2.0F, 3.0F));
        EXPECT_EQ(lamp->color(), glm::vec3(0.5F));
        ASSERT_TRUE(eye->position().has_value());
        EXPECT_NEAR(eye->position()->x, 1.0F, 1e-4F);
        EXPECT_NEAR(eye->position()->z, 3.0F, 1e-4F);
    }

    TEST(NexusStateOrchestratorTest, NothingIsAnsweredForNothing)
    {
        EXPECT_FALSE(std::visit([](const auto& ptr) { return ptr != nullptr; },
            StateOrchestrator::default_row(nullptr, 0)));
        EXPECT_EQ(StateOrchestrator::buffer_of(nullptr, 0).first, nullptr);

        const auto source = StateOrchestrator::placement_from(nullptr);
        EXPECT_FALSE(source().position.has_value());
    }

    TEST(NexusStateOrchestratorTest, APlacementSourceStopsAnsweringWhenItsBufferIsGone)
    {
        const OrchestratedRoom room;
        StateOrchestrator orchestrator { nullptr };
        Nexus::Wiring::PlacementFn source;
        {
            const auto [realization, rows] = orchestrator.orchestrate(room.assembly);
            const auto [buffer, slot] = StateOrchestrator::buffer_of(realization, 1);
            source = StateOrchestrator::placement_from(buffer, slot);
            ASSERT_TRUE(source().position.has_value());
        }

        EXPECT_FALSE(source().position.has_value());
    }

    TEST(NexusStateOrchestratorTest, EveryPieceIsWiredIntoOneFabricOfTheTapestry)
    {
        const OrchestratedRoom room;
        const auto tapestry = std::make_shared<Nexus::Tapestry>(
            std::make_shared<Vruta::TaskScheduler>(48000),
            std::make_shared<Vruta::EventManager>(),
            std::make_shared<Buffers::BufferManager>());
        StateOrchestrator orchestrator { nullptr, tapestry };
        EXPECT_EQ(orchestrator.fabric(), nullptr);

        const auto locus = orchestrator.locus_from(room.assembly, 0);
        const auto lamp = orchestrator.emitter_from(room.assembly, 2);

        ASSERT_NE(orchestrator.fabric(), nullptr);
        EXPECT_EQ(tapestry->all_fabrics().size(), 1U);
        EXPECT_EQ(orchestrator.fabric()->all_ids(), (std::vector<uint32_t> { locus->id(), lamp->id() }));
    }

    TEST(NexusStateOrchestratorTest, APrincipalTheRuleAlreadyWiredIsLeftAsItIs)
    {
        const OrchestratedRoom room;
        const auto tapestry = std::make_shared<Nexus::Tapestry>(
            std::make_shared<Vruta::TaskScheduler>(48000),
            std::make_shared<Vruta::EventManager>(),
            std::make_shared<Buffers::BufferManager>());
        StateOrchestrator orchestrator { nullptr, tapestry };
        orchestrator.set_row_rule(
            [&orchestrator](const std::shared_ptr<const MeshAssemblyContainer>& assembly, size_t row) -> Fabric::Member {
                if (row != 2) {
                    return {};
                }
                return orchestrator.emitter_from(assembly, row);
            });

        const auto [realization, rows] = orchestrator.orchestrate(room.assembly);

        ASSERT_EQ(rows.size(), 1U);
        EXPECT_EQ(orchestrator.fabric()->all_ids().size(), 1U);
    }

    TEST(NexusStateOrchestratorTest, AMotionOfTheAssemblyStartsAsItsOwnReflow)
    {
        const OrchestratedRoom room;
        const auto times = Kinesis::TimeSampling::acyclic({ 0.0, 1.0 }).value();
        const auto rise = Kinesis::TimedValues<glm::vec3>::create(
            { .times = times, .values = { glm::vec3(0.0F, 1.0F, 0.0F), glm::vec3(0.0F, 5.0F, 0.0F) } });
        ASSERT_TRUE(room.assembly->add_motion({ .name = "rise", .channels = { { .row = 1, .translation = rise } } }).has_value());

        FrameScheduler frames;
        StateOrchestrator orchestrator { frames.scheduler };
        const auto [realization, rows] = orchestrator.orchestrate(room.assembly);

        const auto [placing, deforming, streaming] = orchestrator.motion_from(realization, 0, Kriya::CourseEdge::Once, true);

        ASSERT_NE(placing, nullptr);
        ASSERT_EQ(placing->courses().size(), 1U);
        EXPECT_TRUE(placing->courses().front()->paused());
        EXPECT_TRUE(deforming.empty());
        EXPECT_TRUE(streaming.empty());
    }

}
}
