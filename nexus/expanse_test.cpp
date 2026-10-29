#include "gtest/gtest.h"

#include "MayaFlux/Nexus/Expanse.hpp"

namespace MayaFlux::Test {
namespace {

    using Kinesis::AABB3D;
    using Nexus::CrossingContext;
    using Nexus::Expanse;
    using Nexus::PerceptionContext;

    using Snapshot = std::vector<std::pair<uint32_t, glm::vec3>>;

    Snapshot snap(std::initializer_list<std::pair<uint32_t, glm::vec3>> entries)
    {
        return { entries.begin(), entries.end() };
    }

    AABB3D unit_box()
    {
        return AABB3D::from_center(glm::vec3(0.0F), glm::vec3(1.0F));
    }

    class GatedExpanse : public Expanse {
    public:
        GatedExpanse()
            : Expanse(Expanse::ContainsFn {})
        {
        }

        [[nodiscard]] bool contains(const glm::vec3&) const override { return m_open; }

        int begun {};

    protected:
        void begin_evaluate() override
        {
            ++begun;
            m_open = true;
        }

    private:
        bool m_open {};
    };

    class NexusExpanseTest : public ::testing::Test {
    protected:
        std::vector<uint32_t> entered;
        std::vector<uint32_t> exited;
        std::vector<std::string> log;

        std::shared_ptr<Expanse> box = std::make_shared<Expanse>(
            unit_box(),
            [this](uint32_t id) { entered.push_back(id); },
            [this](uint32_t id) { exited.push_back(id); });

        static Snapshot at(uint32_t id, const glm::vec3& p) { return { { id, p } }; }
    };

    TEST(NexusExpanseContainsTest, BoxOnlyIsInclusiveAtTheEdge)
    {
        const Expanse box(unit_box());

        EXPECT_TRUE(box.contains({ 0.0F, 0.0F, 0.0F }));
        EXPECT_TRUE(box.contains({ 1.0F, 1.0F, 1.0F }));
        EXPECT_FALSE(box.contains({ 1.01F, 0.0F, 0.0F }));
        EXPECT_FALSE(box.contains({ 0.0F, -1.01F, 0.0F }));
    }

    TEST(NexusExpanseContainsTest, PredicateOnly)
    {
        const Expanse half([](const glm::vec3& p) { return p.x > 0.0F; });

        EXPECT_TRUE(half.contains({ 5.0F, 0.0F, 0.0F }));
        EXPECT_FALSE(half.contains({ -5.0F, 0.0F, 0.0F }));
    }

    TEST(NexusExpanseContainsTest, BoxAndPredicateMustBothPass)
    {
        Expanse both([](const glm::vec3& p) { return p.x > 0.0F; });
        both.set_bounds(unit_box());

        EXPECT_TRUE(both.contains({ 0.5F, 0.0F, 0.0F }));
        EXPECT_FALSE(both.contains({ -0.5F, 0.0F, 0.0F }));
        EXPECT_FALSE(both.contains({ 5.0F, 0.0F, 0.0F }));
    }

    TEST(NexusExpanseContainsTest, NeitherBoxNorPredicateContainsNothing)
    {
        const Expanse empty { Expanse::ContainsFn {} };
        EXPECT_FALSE(empty.contains({ 0.0F, 0.0F, 0.0F }));
    }

    TEST(NexusExpanseContainsTest, SetBoundsMovesTheRegionAndClearBoundsRemovesIt)
    {
        Expanse region(unit_box());
        ASSERT_TRUE(region.bounds().has_value());

        region.set_bounds(unit_box().translated({ 10.0F, 0.0F, 0.0F }));
        EXPECT_FALSE(region.contains({ 0.0F, 0.0F, 0.0F }));
        EXPECT_TRUE(region.contains({ 10.0F, 0.0F, 0.0F }));

        region.clear_bounds();
        EXPECT_FALSE(region.bounds().has_value());
        EXPECT_FALSE(region.contains({ 10.0F, 0.0F, 0.0F }));
    }

    TEST(NexusExpanseIdentityTest, IdsAreNonZeroAndUnique)
    {
        const Expanse a(unit_box());
        const Expanse b(unit_box());
        const Expanse c { Expanse::ContainsFn {} };

        EXPECT_NE(a.id(), 0U);
        EXPECT_NE(a.id(), b.id());
        EXPECT_NE(b.id(), c.id());
        EXPECT_NE(a.id(), c.id());
    }

    TEST(NexusExpanseIdentityTest, NamedConstructorsKeepEveryName)
    {
        Expanse named("inside", "arrive", "depart",
            [](const glm::vec3&) { return true; }, [](uint32_t) { }, [](uint32_t) { });

        EXPECT_EQ(named.fn_name(), "inside");
        EXPECT_EQ(named.on_enter_fn_name(), "arrive");
        EXPECT_EQ(named.on_exit_fn_name(), "depart");

        named.set_fn_name("a");
        named.set_on_enter_fn_name("b");
        named.set_on_exit_fn_name("c");
        EXPECT_EQ(named.fn_name(), "a");
        EXPECT_EQ(named.on_enter_fn_name(), "b");
        EXPECT_EQ(named.on_exit_fn_name(), "c");

        const Expanse partly("only", [](const glm::vec3&) { return true; }, nullptr, nullptr);
        EXPECT_EQ(partly.fn_name(), "only");
        EXPECT_TRUE(partly.on_enter_fn_name().empty());
    }

    TEST_F(NexusExpanseTest, EntryFiresOnceWhileTheEntityStaysInside)
    {
        box->evaluate(0, at(1, { 0.0F, 0.0F, 0.0F }));
        box->evaluate(0, at(1, { 0.5F, 0.0F, 0.0F }));
        box->evaluate(0, at(1, { 0.0F, 0.5F, 0.0F }));

        EXPECT_EQ(entered, (std::vector<uint32_t> { 1 }));
        EXPECT_TRUE(exited.empty());
    }

    TEST_F(NexusExpanseTest, EntitiesOutsideNeverEnter)
    {
        box->evaluate(0, at(1, { 5.0F, 0.0F, 0.0F }));

        EXPECT_TRUE(entered.empty());
        EXPECT_TRUE(exited.empty());
        EXPECT_EQ(box->occupants(0), nullptr);
    }

    TEST_F(NexusExpanseTest, ExitFiresWhenTheEntityLeavesTheRegion)
    {
        box->evaluate(0, at(1, { 0.0F, 0.0F, 0.0F }));
        box->evaluate(0, at(1, { 5.0F, 0.0F, 0.0F }));
        box->evaluate(0, at(1, { 6.0F, 0.0F, 0.0F }));

        EXPECT_EQ(entered, (std::vector<uint32_t> { 1 }));
        EXPECT_EQ(exited, (std::vector<uint32_t> { 1 }));
    }

    TEST_F(NexusExpanseTest, ReEntryFiresEntryAgain)
    {
        box->evaluate(0, at(1, { 0.0F, 0.0F, 0.0F }));
        box->evaluate(0, at(1, { 5.0F, 0.0F, 0.0F }));
        box->evaluate(0, at(1, { 0.0F, 0.0F, 0.0F }));

        EXPECT_EQ(entered, (std::vector<uint32_t> { 1, 1 }));
    }

    TEST_F(NexusExpanseTest, ExitFiresWhenTheEntityVanishesFromTheSnapshot)
    {
        box->evaluate(0, at(1, { 0.0F, 0.0F, 0.0F }));
        box->evaluate(0, {});

        EXPECT_EQ(exited, (std::vector<uint32_t> { 1 }));
        EXPECT_EQ(box->occupants(0), nullptr);
    }

    TEST_F(NexusExpanseTest, OccupantsTrackWhoIsInsideAndWhichFabrics)
    {
        EXPECT_TRUE(box->occupied_fabrics().empty());

        box->evaluate(3, snap({ { 1, { 0.0F, 0.0F, 0.0F } }, { 2, { 0.5F, 0.0F, 0.0F } }, { 3, { 9.0F, 0.0F, 0.0F } } }));

        const auto* inside = box->occupants(3);
        ASSERT_NE(inside, nullptr);
        EXPECT_EQ(inside->size(), 2U);
        EXPECT_TRUE(inside->contains(1));
        EXPECT_TRUE(inside->contains(2));
        EXPECT_EQ(box->occupied_fabrics(), (std::vector<uint32_t> { 3 }));
        EXPECT_EQ(box->occupants(4), nullptr);
    }

    TEST_F(NexusExpanseTest, FabricsKeepIndependentOccupants)
    {
        box->evaluate(1, at(5, { 0.0F, 0.0F, 0.0F }));
        box->evaluate(2, at(5, { 9.0F, 0.0F, 0.0F }));

        EXPECT_NE(box->occupants(1), nullptr);
        EXPECT_EQ(box->occupants(2), nullptr);
        EXPECT_EQ(entered, (std::vector<uint32_t> { 5 }));
        EXPECT_TRUE(exited.empty());
    }

    TEST_F(NexusExpanseTest, TypedEntryReceivesFabricEntityAndPosition)
    {
        std::vector<CrossingContext> seen;
        box->add_entry<CrossingContext>([](const CrossingContext& c) { return c; })
            ->add_target([&seen](const CrossingContext& c) { seen.push_back(c); });

        box->evaluate(7, at(4, { 0.25F, 0.5F, 0.75F }));

        ASSERT_EQ(seen.size(), 1U);
        EXPECT_EQ(seen[0].fabric_id, 7U);
        EXPECT_EQ(seen[0].entity, 4U);
        ASSERT_TRUE(seen[0].position.has_value());
        EXPECT_EQ(*seen[0].position, glm::vec3(0.25F, 0.5F, 0.75F));
    }

    TEST_F(NexusExpanseTest, TypedExitCarriesTheNewPositionWhileTheEntityIsStillInTheSnapshot)
    {
        std::vector<CrossingContext> seen;
        box->add_exit<CrossingContext>([](const CrossingContext& c) { return c; })
            ->add_target([&seen](const CrossingContext& c) { seen.push_back(c); });

        box->evaluate(0, at(1, { 0.0F, 0.0F, 0.0F }));
        box->evaluate(0, at(1, { 5.0F, 0.0F, 0.0F }));

        ASSERT_EQ(seen.size(), 1U);
        ASSERT_TRUE(seen[0].position.has_value());
        EXPECT_EQ(*seen[0].position, glm::vec3(5.0F, 0.0F, 0.0F));
    }

    TEST_F(NexusExpanseTest, TypedExitHasNoPositionWhenTheEntityLeftTheSnapshot)
    {
        std::vector<CrossingContext> seen;
        box->add_exit<CrossingContext>([](const CrossingContext& c) { return c; })
            ->add_target([&seen](const CrossingContext& c) { seen.push_back(c); });

        box->evaluate(0, at(1, { 0.0F, 0.0F, 0.0F }));
        box->evaluate(0, {});

        ASSERT_EQ(seen.size(), 1U);
        EXPECT_FALSE(seen[0].position.has_value());
    }

    TEST_F(NexusExpanseTest, InsideActionsRunForEveryEntityOnEveryEvaluate)
    {
        int runs {};
        box->add_inside<int>([](const CrossingContext&) { return 1; })
            ->add_target([&runs](const int&) { ++runs; });

        box->evaluate(0, snap({ { 1, { 0.0F, 0.0F, 0.0F } }, { 2, { 0.5F, 0.0F, 0.0F } }, { 3, { 9.0F, 0.0F, 0.0F } } }));
        box->evaluate(0, snap({ { 1, { 0.0F, 0.0F, 0.0F } }, { 2, { 0.5F, 0.0F, 0.0F } } }));

        EXPECT_EQ(runs, 4);
    }

    TEST_F(NexusExpanseTest, PlainCallbacksRunBeforeTypedActions)
    {
        auto plain = std::make_shared<Expanse>(
            unit_box(),
            [this](uint32_t) { log.emplace_back("plain enter"); },
            [this](uint32_t) { log.emplace_back("plain exit"); });
        plain->add_entry<int>([](const CrossingContext&) { return 1; })
            ->add_target([this](const int&) { log.emplace_back("typed enter"); });
        plain->add_exit<int>([](const CrossingContext&) { return 1; })
            ->add_target([this](const int&) { log.emplace_back("typed exit"); });

        plain->evaluate(0, at(1, { 0.0F, 0.0F, 0.0F }));
        plain->evaluate(0, {});

        EXPECT_EQ(log, (std::vector<std::string> { "plain enter", "typed enter", "plain exit", "typed exit" }));
    }

    TEST_F(NexusExpanseTest, PerceptionRunsBeforeAnyCrossingAction)
    {
        box->add_perception<int>([this](const PerceptionContext&) {
                log.emplace_back("perception");
                return 1;
            })
            ->add_target([](const int&) { });
        box->add_entry<int>([](const CrossingContext&) { return 1; })
            ->add_target([this](const int&) { log.emplace_back("entry"); });

        box->evaluate(0, at(1, { 0.0F, 0.0F, 0.0F }));

        EXPECT_EQ(log, (std::vector<std::string> { "perception", "entry" }));
    }

    TEST_F(NexusExpanseTest, PerceptionSeesOccupantsWithSquaredDistanceFromTheCentre)
    {
        PerceptionContext seen;
        std::vector<Kinesis::QueryResult> occupants;
        box->add_perception<int>([&seen, &occupants](const PerceptionContext& ctx) {
                seen = ctx;
                occupants.assign(ctx.spatial_results.begin(), ctx.spatial_results.end());
                return 0;
            })
            ->add_target([](const int&) { });

        box->evaluate(0, snap({ { 1, { 0.5F, 0.0F, 0.0F } }, { 2, { 0.0F, 0.0F, 0.0F } }, { 3, { 9.0F, 0.0F, 0.0F } } }));

        EXPECT_EQ(seen.position, glm::vec3(0.0F));
        EXPECT_NEAR(seen.radius, std::sqrt(3.0F), 1e-5F);
        ASSERT_EQ(occupants.size(), 2U);
        for (const auto& hit : occupants) {
            EXPECT_NE(hit.id, 3U);
            EXPECT_FLOAT_EQ(hit.distance_sq, hit.id == 1U ? 0.25F : 0.0F);
        }
    }

    TEST_F(NexusExpanseTest, PerceptionRunsOncePerEvaluateEvenWhenNobodyIsInside)
    {
        int runs {};
        size_t occupant_count { 99 };
        box->add_perception<int>([&runs, &occupant_count](const PerceptionContext& ctx) {
                ++runs;
                occupant_count = ctx.spatial_results.size();
                return 0;
            })
            ->add_target([](const int&) { });

        box->evaluate(0, {});
        box->evaluate(0, at(1, { 9.0F, 0.0F, 0.0F }));

        EXPECT_EQ(runs, 2);
        EXPECT_EQ(occupant_count, 0U);
    }

    TEST_F(NexusExpanseTest, EvictFiresExitsForEveryoneInsideAndForgetsThem)
    {
        std::vector<CrossingContext> seen;
        box->add_exit<CrossingContext>([](const CrossingContext& c) { return c; })
            ->add_target([&seen](const CrossingContext& c) { seen.push_back(c); });
        box->evaluate(2, snap({ { 1, { 0.0F, 0.0F, 0.0F } }, { 2, { 0.5F, 0.0F, 0.0F } } }));

        box->evict(2);

        EXPECT_EQ(exited.size(), 2U);
        ASSERT_EQ(seen.size(), 2U);
        for (const auto& c : seen) {
            EXPECT_EQ(c.fabric_id, 2U);
            EXPECT_FALSE(c.position.has_value());
        }
        EXPECT_EQ(box->occupants(2), nullptr);
        EXPECT_TRUE(box->occupied_fabrics().empty());
    }

    TEST_F(NexusExpanseTest, EvictTwiceOrForAnUnknownFabricFiresNothingMore)
    {
        box->evaluate(2, at(1, { 0.0F, 0.0F, 0.0F }));

        box->evict(2);
        box->evict(2);
        box->evict(99);

        EXPECT_EQ(exited.size(), 1U);
    }

    TEST_F(NexusExpanseTest, EvictOnlyAffectsTheNamedFabric)
    {
        box->evaluate(1, at(1, { 0.0F, 0.0F, 0.0F }));
        box->evaluate(2, at(1, { 0.0F, 0.0F, 0.0F }));

        box->evict(1);

        EXPECT_EQ(box->occupants(1), nullptr);
        EXPECT_NE(box->occupants(2), nullptr);
    }

    TEST_F(NexusExpanseTest, AnEvictedEntityEntersAgainOnTheNextEvaluate)
    {
        box->evaluate(0, at(1, { 0.0F, 0.0F, 0.0F }));
        box->evict(0);
        box->evaluate(0, at(1, { 0.0F, 0.0F, 0.0F }));

        EXPECT_EQ(entered, (std::vector<uint32_t> { 1, 1 }));
    }

    TEST_F(NexusExpanseTest, RemoveCrossingStopsOnlyThatAction)
    {
        int kept {};
        int dropped {};
        box->add_entry<int>([](const CrossingContext&) { return 1; })
            ->add_target([&kept](const int&) { ++kept; });
        auto drop = box->add_entry<int>([](const CrossingContext&) { return 1; });
        drop->add_target([&dropped](const int&) { ++dropped; });

        box->remove_crossing(drop);
        box->evaluate(0, at(1, { 0.0F, 0.0F, 0.0F }));

        EXPECT_EQ(kept, 1);
        EXPECT_EQ(dropped, 0);
    }

    TEST_F(NexusExpanseTest, ClearCrossingsStopsEntryExitAndInsideActions)
    {
        int runs {};
        for (auto add : { 0, 1, 2 }) {
            auto action = add == 0   ? box->add_entry<int>([](const CrossingContext&) { return 1; })
                : add == 1           ? box->add_exit<int>([](const CrossingContext&) { return 1; })
                                     : box->add_inside<int>([](const CrossingContext&) { return 1; });
            action->add_target([&runs](const int&) { ++runs; });
        }

        box->clear_crossings();
        box->evaluate(0, at(1, { 0.0F, 0.0F, 0.0F }));
        box->evaluate(0, {});

        EXPECT_EQ(runs, 0);
    }

    TEST_F(NexusExpanseTest, RemoveAndClearPerceptionsStopThem)
    {
        int kept {};
        int dropped {};
        box->add_perception<int>([](const PerceptionContext&) { return 1; })
            ->add_target([&kept](const int&) { ++kept; });
        auto drop = box->add_perception<int>([](const PerceptionContext&) { return 1; });
        drop->add_target([&dropped](const int&) { ++dropped; });

        box->remove_perception(drop);
        box->evaluate(0, {});
        EXPECT_EQ(kept, 1);
        EXPECT_EQ(dropped, 0);

        box->clear_perceptions();
        box->evaluate(0, {});
        EXPECT_EQ(kept, 1);
    }

    TEST_F(NexusExpanseTest, AnExpanseWithNoActionsEvaluatesHarmlessly)
    {
        Expanse bare(unit_box());
        EXPECT_NO_THROW(bare.evaluate(0, at(1, { 0.0F, 0.0F, 0.0F })));
        EXPECT_NO_THROW(bare.evaluate(0, {}));
        EXPECT_NO_THROW(bare.evict(0));
    }

    TEST(NexusExpanseDerivedTest, BeginEvaluateRunsBeforeAnyPositionIsTested)
    {
        auto gated = std::make_shared<GatedExpanse>();
        std::vector<uint32_t> entered;
        gated->add_entry<int>([&entered](const CrossingContext& c) {
                entered.push_back(c.entity);
                return 0;
            })
            ->add_target([](const int&) { });

        gated->evaluate(0, snap({ { 1, { 0.0F, 0.0F, 0.0F } } }));
        gated->evaluate(0, snap({ { 1, { 0.0F, 0.0F, 0.0F } } }));

        EXPECT_EQ(gated->begun, 2);
        EXPECT_EQ(entered, (std::vector<uint32_t> { 1 }));
    }

}
}
