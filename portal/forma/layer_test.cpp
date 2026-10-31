#include "gtest/gtest.h"

#include "MayaFlux/Portal/Forma/Layer.hpp"

namespace MayaFlux::Test {

namespace Forma = Portal::Forma;
using Forma::Element;
using Forma::Layer;
using Kinesis::AABB2D;

namespace {

    Element rect(float x0, float y0, float x1, float y1)
    {
        return Element {}.with_bounds(AABB2D { .min = { x0, y0 }, .max = { x1, y1 } });
    }

    Element whole()
    {
        return rect(-1.F, -1.F, 1.F, 1.F);
    }

    std::vector<uint32_t> order(const Layer& layer)
    {
        std::vector<uint32_t> ids;
        for (const auto& el : layer.elements())
            ids.push_back(el.id);
        return ids;
    }

    size_t position(const Layer& layer, uint32_t id)
    {
        const auto ids = order(layer);
        return static_cast<size_t>(std::ranges::find(ids, id) - ids.begin());
    }

    bool contains_id(const std::vector<uint32_t>& ids, uint32_t id)
    {
        return std::ranges::find(ids, id) != ids.end();
    }

    const glm::vec2 k_origin { 0.F, 0.F };

}

// =============================================================================
// Registration
// =============================================================================

TEST(FormaLayerRegistrationTest, AddAssignsDistinctNonZeroIdsInInsertionOrder)
{
    Layer layer;

    const uint32_t a = layer.add(whole());
    const uint32_t b = layer.add(whole());
    const uint32_t c = layer.add(whole());

    EXPECT_NE(a, 0U);
    EXPECT_LT(a, b);
    EXPECT_LT(b, c);
    EXPECT_EQ(order(layer), (std::vector<uint32_t> { a, b, c }));
    EXPECT_EQ(layer.size(), 3U);
    EXPECT_FALSE(layer.empty());
}

TEST(FormaLayerRegistrationTest, AddOverwritesAnIdTheCallerPutOnTheElement)
{
    Layer layer;
    Element el = whole();
    el.id = 9999;

    const uint32_t id = layer.add(std::move(el));

    EXPECT_NE(id, 9999U);
    EXPECT_NE(layer.get(id), nullptr);
    EXPECT_EQ(layer.get(9999), nullptr);
}

TEST(FormaLayerRegistrationTest, GetReturnsNullForUnknownAndZeroIds)
{
    Layer layer;
    const uint32_t a = layer.add(whole());

    EXPECT_EQ(layer.get(0), nullptr);
    EXPECT_EQ(layer.get(a + 100), nullptr);
    ASSERT_NE(layer.get(a), nullptr);
    EXPECT_EQ(layer.get(a)->id, a);
}

TEST(FormaLayerRegistrationTest, ConstAndMutableGetAgree)
{
    Layer layer;
    const uint32_t a = layer.add(whole());
    const Layer& view = layer;

    layer.get(a)->name = "renamed";

    EXPECT_EQ(view.get(a)->name, "renamed");
}

TEST(FormaLayerRegistrationTest, SlotChainAppliesRelationVisibilityAndInteractivity)
{
    Layer layer;
    const uint32_t parent = layer.add(whole());

    const uint32_t child = layer.add(whole())
                               .relate_to(parent)
                               .hidden()
                               .non_interactive()
                               .id();

    EXPECT_EQ(layer.related_ids(parent), std::vector<uint32_t> { child });
    EXPECT_FALSE(layer.get(child)->visible);
    EXPECT_FALSE(layer.get(child)->interactive);
}

TEST(FormaLayerRegistrationTest, SlotToFrontAndToBackReorder)
{
    Layer layer;
    const uint32_t a = layer.add(whole());
    const uint32_t b = layer.add(whole());

    const uint32_t sent_back = layer.add(whole()).to_back().id();
    EXPECT_EQ(order(layer).front(), sent_back);

    const uint32_t brought_forward = layer.add(whole()).to_back().to_front().id();
    EXPECT_EQ(order(layer).back(), brought_forward);
    EXPECT_EQ(layer.hit_test(k_origin), brought_forward);
    EXPECT_EQ(layer.size(), 4U);
    EXPECT_TRUE(contains_id(order(layer), a));
    EXPECT_TRUE(contains_id(order(layer), b));
}

TEST(FormaLayerRegistrationTest, ClearDropsElementsAndRelations)
{
    Layer layer;
    const uint32_t a = layer.add(whole());
    const uint32_t b = layer.add(whole());
    ASSERT_TRUE(layer.relate(a, b));

    layer.clear();

    EXPECT_TRUE(layer.empty());
    EXPECT_EQ(layer.get(a), nullptr);
    EXPECT_TRUE(layer.related_ids(a).empty());
    EXPECT_FALSE(layer.hit_test(k_origin).has_value());
}

TEST(FormaLayerRegistrationTest, ManyElementsRemainAddressableById)
{
    Layer layer;
    std::vector<uint32_t> ids;
    for (int i = 0; i < 1000; ++i)
        ids.push_back(layer.add(whole()));

    for (uint32_t id : ids) {
        ASSERT_NE(layer.get(id), nullptr);
        EXPECT_EQ(layer.get(id)->id, id);
    }
    for (size_t i = 0; i < ids.size(); i += 2)
        ASSERT_TRUE(layer.remove(ids[i]));
    EXPECT_EQ(layer.size(), 500U);
    for (size_t i = 0; i < ids.size(); ++i)
        EXPECT_EQ(layer.get(ids[i]) != nullptr, i % 2 == 1);
}

TEST(FormaLayerRegistrationTest, LookupFollowsReorderingRemovalAndLaterAdds)
{
    Layer layer;
    std::vector<uint32_t> ids;
    for (int i = 0; i < 6; ++i)
        ids.push_back(layer.add(whole()));

    ASSERT_TRUE(layer.bring_to_front(ids[1]));
    ASSERT_TRUE(layer.send_to_back(ids[4]));
    EXPECT_EQ(layer.elements().front().id, ids[4]);
    EXPECT_EQ(layer.elements().back().id, ids[1]);
    for (uint32_t id : ids) {
        ASSERT_NE(layer.get(id), nullptr);
        EXPECT_EQ(layer.get(id)->id, id);
    }

    ASSERT_TRUE(layer.remove(ids[2]));
    const uint32_t added = layer.add(whole());
    EXPECT_EQ(layer.get(ids[2]), nullptr);
    ASSERT_NE(layer.get(added), nullptr);
    EXPECT_EQ(layer.get(added)->id, added);
    EXPECT_EQ(layer.elements().back().id, added);
    for (uint32_t id : ids) {
        if (id != ids[2])
            EXPECT_EQ(layer.get(id)->id, id);
    }

    layer.clear();
    EXPECT_EQ(layer.get(added), nullptr);
    const uint32_t fresh = layer.add(whole());
    ASSERT_NE(layer.get(fresh), nullptr);
    EXPECT_EQ(layer.get(fresh)->id, fresh);
}

// =============================================================================
// Mutation
// =============================================================================

TEST(FormaLayerMutationTest, SettersReportUnknownIds)
{
    Layer layer;

    EXPECT_FALSE(layer.set_bounds(42, AABB2D {}));
    EXPECT_FALSE(layer.set_contains(42, [](glm::vec2) { return true; }));
    EXPECT_FALSE(layer.set_interactive(42, false));
    EXPECT_FALSE(layer.set_visible(42, false));
    EXPECT_FALSE(layer.bring_to_front(42));
    EXPECT_FALSE(layer.send_to_back(42));
    EXPECT_FALSE(layer.remove(42));
}

TEST(FormaLayerMutationTest, SetBoundsMovesTheHitRegion)
{
    Layer layer;
    const uint32_t a = layer.add(rect(-1.F, -1.F, -0.5F, -0.5F));
    ASSERT_FALSE(layer.hit_test(k_origin).has_value());

    ASSERT_TRUE(layer.set_bounds(a, AABB2D { .min = { -0.1F, -0.1F }, .max = { 0.1F, 0.1F } }));

    EXPECT_EQ(layer.hit_test(k_origin), a);
    EXPECT_FALSE(layer.hit_test({ -0.75F, -0.75F }).has_value());
}

TEST(FormaLayerMutationTest, SetContainsReplacesThePredicate)
{
    Layer layer;
    const uint32_t a = layer.add(whole().with_contains([](glm::vec2) { return false; }));
    ASSERT_FALSE(layer.hit_test(k_origin).has_value());

    ASSERT_TRUE(layer.set_contains(a, [](glm::vec2) { return true; }));

    EXPECT_EQ(layer.hit_test(k_origin), a);
}

TEST(FormaLayerMutationTest, SetInteractiveAndVisibleGateHitTesting)
{
    Layer layer;
    const uint32_t a = layer.add(whole());
    ASSERT_EQ(layer.hit_test(k_origin), a);

    ASSERT_TRUE(layer.set_interactive(a, false));
    EXPECT_FALSE(layer.hit_test(k_origin).has_value());
    ASSERT_TRUE(layer.set_interactive(a, true));
    EXPECT_EQ(layer.hit_test(k_origin), a);

    ASSERT_TRUE(layer.set_visible(a, false));
    EXPECT_FALSE(layer.hit_test(k_origin).has_value());
    ASSERT_TRUE(layer.set_visible(a, true));
    EXPECT_EQ(layer.hit_test(k_origin), a);
}

TEST(FormaLayerMutationTest, SetAllVisibleReachesEveryElement)
{
    Layer layer;
    const uint32_t a = layer.add(whole());
    const uint32_t b = layer.add(whole());
    const uint32_t c = layer.add(whole());

    layer.set_all_visible(false);
    for (uint32_t id : { a, b, c })
        EXPECT_FALSE(layer.get(id)->visible);
    EXPECT_FALSE(layer.hit_test(k_origin).has_value());

    layer.set_all_visible(true);
    for (uint32_t id : { a, b, c })
        EXPECT_TRUE(layer.get(id)->visible);
}

TEST(FormaLayerMutationTest, SetClipConfinesHitsToTheRegionInsideTheBounds)
{
    Layer layer;
    const uint32_t a = layer.add(whole());
    const AABB2D left_half { .min = { -1.F, -1.F }, .max = { 0.F, 1.F } };

    ASSERT_TRUE(layer.set_clip(a, left_half));

    EXPECT_EQ(layer.hit_test({ -0.5F, 0.F }), a);
    EXPECT_FALSE(layer.hit_test({ 0.5F, 0.F }).has_value());
}

TEST(FormaLayerMutationTest, TheClipIsIndependentOfBoundsAndPredicate)
{
    Layer layer;
    const uint32_t a = layer.add(whole());
    ASSERT_TRUE(layer.set_clip(a, AABB2D { .min = { -1.F, -1.F }, .max = { 0.F, 1.F } }));

    ASSERT_TRUE(layer.set_bounds(a, AABB2D { .min = { -1.F, -1.F }, .max = { 1.F, 1.F } }));
    ASSERT_TRUE(layer.set_contains(a, [](glm::vec2) { return true; }));

    EXPECT_FALSE(layer.hit_test({ 0.5F, 0.F }).has_value());
    EXPECT_EQ(layer.hit_test({ -0.5F, 0.F }), a);
}

TEST(FormaLayerMutationTest, ClearingTheClipRestoresHitTesting)
{
    Layer layer;
    const uint32_t a = layer.add(whole());
    ASSERT_TRUE(layer.set_clip(a, AABB2D { .min = { -1.F, -1.F }, .max = { 0.F, 1.F } }));
    ASSERT_FALSE(layer.hit_test({ 0.5F, 0.F }).has_value());

    ASSERT_TRUE(layer.set_clip(a, std::nullopt));

    EXPECT_EQ(layer.hit_test({ 0.5F, 0.F }), a);
}

TEST(FormaLayerMutationTest, SetClipReportsUnknownIds)
{
    Layer layer;

    EXPECT_FALSE(layer.set_clip(42, AABB2D {}));
}

TEST(FormaLayerMutationTest, AClippedElementDoesNotBlockTheOneBelow)
{
    Layer layer;
    const uint32_t below = layer.add(whole());
    const uint32_t above = layer.add(whole());
    ASSERT_TRUE(layer.set_clip(above, AABB2D { .min = { -1.F, -1.F }, .max = { 0.F, 1.F } }));

    EXPECT_EQ(layer.hit_test({ 0.5F, 0.F }), below);
    EXPECT_EQ(layer.hit_test({ -0.5F, 0.F }), above);
    EXPECT_EQ(layer.hit_test_all({ -0.5F, 0.F }), (std::vector<uint32_t> { above, below }));
}

// =============================================================================
// Hit testing
// =============================================================================

TEST(FormaLayerHitTest, BoundsOnlyElementHitsInsideIncludingEdges)
{
    Layer layer;
    const uint32_t a = layer.add(rect(-0.5F, -0.25F, 0.5F, 0.25F));

    EXPECT_EQ(layer.hit_test({ 0.F, 0.F }), a);
    EXPECT_EQ(layer.hit_test({ -0.5F, -0.25F }), a);
    EXPECT_EQ(layer.hit_test({ 0.5F, 0.25F }), a);
    EXPECT_FALSE(layer.hit_test({ 0.51F, 0.F }).has_value());
    EXPECT_FALSE(layer.hit_test({ 0.F, -0.26F }).has_value());
}

TEST(FormaLayerHitTest, ElementWithNeitherBoundsNorPredicateNeverHits)
{
    Layer layer;
    (void)layer.add(Element {});

    EXPECT_FALSE(layer.hit_test(k_origin).has_value());
    EXPECT_TRUE(layer.hit_test_all(k_origin).empty());
}

TEST(FormaLayerHitTest, PredicateAloneIsAuthoritativeWithoutBounds)
{
    Layer layer;
    const uint32_t a = layer.add(Element {}.with_circle({ 0.5F, 0.5F }, 0.2F));

    EXPECT_EQ(layer.hit_test({ 0.5F, 0.5F }), a);
    EXPECT_EQ(layer.hit_test({ 0.6F, 0.5F }), a);
    EXPECT_FALSE(layer.hit_test({ 0.8F, 0.5F }).has_value());
    EXPECT_FALSE(layer.hit_test({ -0.5F, -0.5F }).has_value());
}

TEST(FormaLayerHitTest, PredicateNarrowsTheBoundsHint)
{
    Layer layer;
    const uint32_t a = layer.add(
        rect(-0.5F, -0.5F, 0.5F, 0.5F).with_circle({ 0.F, 0.F }, 0.5F));

    EXPECT_EQ(layer.hit_test({ 0.F, 0.F }), a);
    EXPECT_FALSE(layer.hit_test({ 0.45F, 0.45F }).has_value());
}

TEST(FormaLayerHitTest, BoundsHintRejectsBeforeThePredicateRuns)
{
    Layer layer;
    int calls = 0;
    (void)layer.add(rect(-0.5F, -0.5F, 0.5F, 0.5F).with_contains([&calls](glm::vec2) {
        ++calls;
        return true;
    }));

    EXPECT_FALSE(layer.hit_test({ 0.9F, 0.9F }).has_value());
    EXPECT_EQ(calls, 0);
    EXPECT_TRUE(layer.hit_test({ 0.F, 0.F }).has_value());
    EXPECT_EQ(calls, 1);
}

TEST(FormaLayerHitTest, NonInteractiveAndHiddenElementsAreSkipped)
{
    Layer layer;
    const uint32_t below = layer.add(whole());
    const uint32_t ghost = layer.add(whole());
    const uint32_t inert = layer.add(whole());
    ASSERT_TRUE(layer.set_visible(ghost, false));
    ASSERT_TRUE(layer.set_interactive(inert, false));

    EXPECT_EQ(layer.hit_test(k_origin), below);
    EXPECT_EQ(layer.hit_test_all(k_origin), std::vector<uint32_t> { below });
}

TEST(FormaLayerHitTest, TopmostWinsAndHitTestAllListsFrontToBack)
{
    Layer layer;
    const uint32_t a = layer.add(whole());
    const uint32_t b = layer.add(whole());
    const uint32_t c = layer.add(whole());

    EXPECT_EQ(layer.hit_test(k_origin), c);
    EXPECT_EQ(layer.hit_test_all(k_origin), (std::vector<uint32_t> { c, b, a }));
}

TEST(FormaLayerHitTest, OverlapOnlyReportsTheElementsUnderThePoint)
{
    Layer layer;
    const uint32_t left = layer.add(rect(-1.F, -1.F, 0.2F, 1.F));
    const uint32_t right = layer.add(rect(-0.2F, -1.F, 1.F, 1.F));

    EXPECT_EQ(layer.hit_test_all({ 0.F, 0.F }), (std::vector<uint32_t> { right, left }));
    EXPECT_EQ(layer.hit_test_all({ -0.8F, 0.F }), std::vector<uint32_t> { left });
    EXPECT_EQ(layer.hit_test_all({ 0.8F, 0.F }), std::vector<uint32_t> { right });
}

TEST(FormaLayerHitTest, PixelOverloadsUseATopLeftOrigin)
{
    Layer layer;
    const uint32_t top_left = layer.add(rect(-1.F, 0.5F, -0.5F, 1.F));
    const uint32_t bottom_right = layer.add(rect(0.5F, -1.F, 1.F, -0.5F));
    const uint32_t centre = layer.add(rect(-0.1F, -0.1F, 0.1F, 0.1F));

    EXPECT_EQ(layer.hit_test(10.0, 10.0, 200, 100), top_left);
    EXPECT_EQ(layer.hit_test(190.0, 90.0, 200, 100), bottom_right);
    EXPECT_EQ(layer.hit_test(100.0, 50.0, 200, 100), centre);
    EXPECT_EQ(layer.hit_test_all(100.0, 50.0, 200, 100), std::vector<uint32_t> { centre });
    EXPECT_FALSE(layer.hit_test(190.0, 10.0, 200, 100).has_value());
}

// =============================================================================
// Z-order
// =============================================================================

TEST(FormaLayerOrderTest, BringToFrontMakesTheElementTopmost)
{
    Layer layer;
    const uint32_t a = layer.add(whole());
    const uint32_t b = layer.add(whole());
    const uint32_t c = layer.add(whole());

    ASSERT_TRUE(layer.bring_to_front(a));

    EXPECT_EQ(layer.hit_test(k_origin), a);
    EXPECT_EQ(order(layer), (std::vector<uint32_t> { b, c, a }));
}

TEST(FormaLayerOrderTest, SendToBackMakesTheElementBottommost)
{
    Layer layer;
    const uint32_t a = layer.add(whole());
    const uint32_t b = layer.add(whole());
    const uint32_t c = layer.add(whole());

    ASSERT_TRUE(layer.send_to_back(c));

    EXPECT_EQ(layer.hit_test(k_origin), b);
    EXPECT_EQ(order(layer), (std::vector<uint32_t> { c, a, b }));
}

TEST(FormaLayerOrderTest, BringToFrontOfTheTopmostIsStable)
{
    Layer layer;
    const uint32_t a = layer.add(whole());
    const uint32_t b = layer.add(whole());

    ASSERT_TRUE(layer.bring_to_front(b));

    EXPECT_EQ(order(layer), (std::vector<uint32_t> { a, b }));
}

TEST(FormaLayerOrderTest, BringToFrontLiftsTheWholeRelatedGroupAboveOthers)
{
    Layer layer;
    const uint32_t group = layer.add(whole());
    const uint32_t member = layer.add(whole());
    const uint32_t other_1 = layer.add(whole());
    const uint32_t other_2 = layer.add(whole());
    ASSERT_TRUE(layer.relate(group, member));

    ASSERT_TRUE(layer.bring_to_front(group));

    const size_t floor = std::min(position(layer, group), position(layer, member));
    EXPECT_GT(floor, position(layer, other_1));
    EXPECT_GT(floor, position(layer, other_2));
    EXPECT_EQ(position(layer, group), layer.size() - 1);
}

TEST(FormaLayerOrderTest, SendToBackDropsTheWholeRelatedGroupBelowOthers)
{
    Layer layer;
    const uint32_t other_1 = layer.add(whole());
    const uint32_t group = layer.add(whole());
    const uint32_t member = layer.add(whole());
    const uint32_t other_2 = layer.add(whole());
    ASSERT_TRUE(layer.relate(group, member));

    ASSERT_TRUE(layer.send_to_back(group));

    const size_t ceiling = std::max(position(layer, group), position(layer, member));
    EXPECT_LT(ceiling, position(layer, other_1));
    EXPECT_LT(ceiling, position(layer, other_2));
    EXPECT_EQ(position(layer, group), 0U);
}

TEST(FormaLayerOrderTest, BringToFrontReachesTransitiveRelations)
{
    Layer layer;
    const uint32_t root = layer.add(whole());
    const uint32_t child = layer.add(whole());
    const uint32_t grandchild = layer.add(whole());
    const uint32_t other = layer.add(whole());
    ASSERT_TRUE(layer.relate(root, child));
    ASSERT_TRUE(layer.relate(child, grandchild));

    ASSERT_TRUE(layer.bring_to_front(root));

    EXPECT_GT(position(layer, grandchild), position(layer, other));
    EXPECT_GT(position(layer, child), position(layer, other));
    EXPECT_EQ(position(layer, root), layer.size() - 1);
}

TEST(FormaLayerOrderTest, SendToBackReachesTransitiveRelations)
{
    Layer layer;
    const uint32_t other = layer.add(whole());
    const uint32_t root = layer.add(whole());
    const uint32_t child = layer.add(whole());
    const uint32_t grandchild = layer.add(whole());
    ASSERT_TRUE(layer.relate(root, child));
    ASSERT_TRUE(layer.relate(child, grandchild));

    ASSERT_TRUE(layer.send_to_back(root));

    EXPECT_LT(position(layer, grandchild), position(layer, other));
    EXPECT_LT(position(layer, child), position(layer, other));
    EXPECT_EQ(position(layer, root), 0U);
}

// =============================================================================
// Relations
// =============================================================================

TEST(FormaLayerRelationTest, RelateRejectsSelfAndUnknownIds)
{
    Layer layer;
    const uint32_t a = layer.add(whole());
    const uint32_t b = layer.add(whole());

    EXPECT_FALSE(layer.relate(a, a));
    EXPECT_FALSE(layer.relate(a, 999));
    EXPECT_FALSE(layer.relate(999, a));
    EXPECT_TRUE(layer.related_ids(a).empty());
    EXPECT_TRUE(layer.relate(a, b));
}

TEST(FormaLayerRelationTest, RelatingTwiceKeepsASingleEdge)
{
    Layer layer;
    const uint32_t a = layer.add(whole());
    const uint32_t b = layer.add(whole());

    EXPECT_TRUE(layer.relate(a, b));
    EXPECT_TRUE(layer.relate(a, b));

    EXPECT_EQ(layer.related_ids(a), std::vector<uint32_t> { b });
}

TEST(FormaLayerRelationTest, RelateAdoptsTheVisibilityOfThePrimaryBothWays)
{
    Layer layer;
    const uint32_t primary = layer.add(whole());
    const uint32_t was_hidden = layer.add(whole());
    ASSERT_TRUE(layer.set_visible(was_hidden, false));

    ASSERT_TRUE(layer.relate(primary, was_hidden));

    EXPECT_TRUE(layer.get(was_hidden)->visible);
}

TEST(FormaLayerRelationTest, RelationsAreDirectional)
{
    Layer layer;
    const uint32_t parent = layer.add(whole());
    const uint32_t child = layer.add(whole());
    ASSERT_TRUE(layer.relate(parent, child));

    EXPECT_EQ(layer.related_ids(parent), std::vector<uint32_t> { child });
    EXPECT_TRUE(layer.related_ids(child).empty());
}

TEST(FormaLayerRelationTest, ElementsMayBeRelatedBothWaysToFormOneGroup)
{
    Layer layer;
    const uint32_t a = layer.add(whole());
    const uint32_t b = layer.add(whole());
    const uint32_t c = layer.add(whole());

    EXPECT_TRUE(layer.relate(a, b));
    EXPECT_TRUE(layer.relate(b, a));
    EXPECT_TRUE(layer.relate(b, c));
    EXPECT_TRUE(layer.relate(c, a));

    for (uint32_t id : { a, b, c }) {
        const auto ids = layer.closure(id);
        EXPECT_EQ(ids.size(), 3U) << id;
        EXPECT_EQ(ids.front(), id);
    }
}

TEST(FormaLayerRelationTest, EveryCascadeTerminatesOnARelationCycle)
{
    Layer layer;
    const uint32_t a = layer.add(whole());
    const uint32_t b = layer.add(whole());
    const uint32_t other = layer.add(whole());
    ASSERT_TRUE(layer.relate(a, b));
    ASSERT_TRUE(layer.relate(b, a));

    EXPECT_TRUE(layer.set_visible(a, false));
    EXPECT_FALSE(layer.get(b)->visible);
    EXPECT_TRUE(layer.get(other)->visible);
    EXPECT_TRUE(layer.set_visible(b, true));
    EXPECT_TRUE(layer.get(a)->visible);

    EXPECT_TRUE(layer.send_to_back(a));
    EXPECT_LT(position(layer, a), position(layer, other));
    EXPECT_LT(position(layer, b), position(layer, other));

    EXPECT_TRUE(layer.bring_to_front(b));
    EXPECT_GT(position(layer, a), position(layer, other));
    EXPECT_EQ(position(layer, b), layer.size() - 1);

    EXPECT_TRUE(layer.remove(a));
    EXPECT_EQ(order(layer), std::vector<uint32_t> { other });
}

TEST(FormaLayerRelationTest, AMutuallyRelatedPairVanishesTogetherFromEitherSide)
{
    Layer layer;
    const uint32_t field = layer.add(whole());
    const uint32_t viewport = layer.add(whole());
    ASSERT_TRUE(layer.relate(viewport, field));
    ASSERT_TRUE(layer.relate(field, viewport));

    ASSERT_TRUE(layer.remove(field));

    EXPECT_TRUE(layer.empty());
}

TEST(FormaLayerRelationTest, DiamondsAreAllowed)
{
    Layer layer;
    const uint32_t top = layer.add(whole());
    const uint32_t left = layer.add(whole());
    const uint32_t right = layer.add(whole());
    const uint32_t bottom = layer.add(whole());
    ASSERT_TRUE(layer.relate(top, left));
    ASSERT_TRUE(layer.relate(top, right));
    ASSERT_TRUE(layer.relate(left, bottom));

    EXPECT_TRUE(layer.relate(right, bottom));
    EXPECT_EQ(layer.closure(top).size(), 4U);
}

TEST(FormaLayerRelationTest, UnrelateRemovesOnlyThatEdge)
{
    Layer layer;
    const uint32_t a = layer.add(whole());
    const uint32_t b = layer.add(whole());
    const uint32_t c = layer.add(whole());
    ASSERT_TRUE(layer.relate(a, b));
    ASSERT_TRUE(layer.relate(a, c));

    EXPECT_TRUE(layer.unrelate(a, b));

    EXPECT_EQ(layer.related_ids(a), std::vector<uint32_t> { c });
    EXPECT_FALSE(layer.unrelate(a, b));
    EXPECT_FALSE(layer.unrelate(b, a));
    EXPECT_NE(layer.get(b), nullptr);
    EXPECT_TRUE(layer.unrelate(a, c));
    EXPECT_TRUE(layer.related_ids(a).empty());
}

TEST(FormaLayerRelationTest, RelatedIdsOfAnUnrelatedOrUnknownIdIsEmpty)
{
    Layer layer;
    const uint32_t a = layer.add(whole());

    EXPECT_TRUE(layer.related_ids(a).empty());
    EXPECT_TRUE(layer.related_ids(999).empty());
}

TEST(FormaLayerRelationTest, ClosureStartsAtTheRootAndListsEachIdOnce)
{
    Layer layer;
    const uint32_t root = layer.add(whole());
    const uint32_t child = layer.add(whole());
    const uint32_t grandchild = layer.add(whole());
    const uint32_t bystander = layer.add(whole());
    ASSERT_TRUE(layer.relate(root, child));
    ASSERT_TRUE(layer.relate(child, grandchild));
    ASSERT_TRUE(layer.relate(root, grandchild));

    const auto ids = layer.closure(root);

    ASSERT_EQ(ids.size(), 3U);
    EXPECT_EQ(ids.front(), root);
    EXPECT_TRUE(contains_id(ids, child));
    EXPECT_TRUE(contains_id(ids, grandchild));
    EXPECT_FALSE(contains_id(ids, bystander));
    EXPECT_EQ(layer.closure(bystander), std::vector<uint32_t> { bystander });
}

// =============================================================================
// Visibility cascade
// =============================================================================

TEST(FormaLayerVisibilityTest, SetVisibleCascadesThroughTransitiveRelations)
{
    Layer layer;
    const uint32_t root = layer.add(whole());
    const uint32_t child = layer.add(whole());
    const uint32_t grandchild = layer.add(whole());
    const uint32_t bystander = layer.add(whole());
    ASSERT_TRUE(layer.relate(root, child));
    ASSERT_TRUE(layer.relate(child, grandchild));

    ASSERT_TRUE(layer.set_visible(root, false));

    EXPECT_FALSE(layer.get(root)->visible);
    EXPECT_FALSE(layer.get(child)->visible);
    EXPECT_FALSE(layer.get(grandchild)->visible);
    EXPECT_TRUE(layer.get(bystander)->visible);
    EXPECT_EQ(layer.hit_test(k_origin), bystander);
}

TEST(FormaLayerVisibilityTest, HidingAChildLeavesItsParentVisible)
{
    Layer layer;
    const uint32_t parent = layer.add(whole());
    const uint32_t child = layer.add(whole());
    ASSERT_TRUE(layer.relate(parent, child));

    ASSERT_TRUE(layer.set_visible(child, false));

    EXPECT_TRUE(layer.get(parent)->visible);
    EXPECT_FALSE(layer.get(child)->visible);
}

TEST(FormaLayerVisibilityTest, RelatingAnElementToAHiddenPrimaryHidesIt)
{
    Layer layer;
    const uint32_t primary = layer.add(whole());
    const uint32_t body = layer.add(whole());
    ASSERT_TRUE(layer.set_visible(primary, false));
    ASSERT_TRUE(layer.get(body)->visible);

    ASSERT_TRUE(layer.relate(primary, body));

    EXPECT_FALSE(layer.get(body)->visible);
}

// =============================================================================
// Removal
// =============================================================================

TEST(FormaLayerRemovalTest, RemoveCascadesToEveryRelatedElement)
{
    Layer layer;
    const uint32_t root = layer.add(whole());
    const uint32_t child = layer.add(whole());
    const uint32_t grandchild = layer.add(whole());
    const uint32_t bystander = layer.add(whole());
    ASSERT_TRUE(layer.relate(root, child));
    ASSERT_TRUE(layer.relate(child, grandchild));

    ASSERT_TRUE(layer.remove(root));

    EXPECT_EQ(order(layer), std::vector<uint32_t> { bystander });
    EXPECT_TRUE(layer.related_ids(root).empty());
    EXPECT_TRUE(layer.related_ids(child).empty());
}

TEST(FormaLayerRemovalTest, RemovingAChildLeavesTheParentAndItsOtherChildren)
{
    Layer layer;
    const uint32_t parent = layer.add(whole());
    const uint32_t first = layer.add(whole());
    const uint32_t second = layer.add(whole());
    ASSERT_TRUE(layer.relate(parent, first));
    ASSERT_TRUE(layer.relate(parent, second));

    ASSERT_TRUE(layer.remove(first));

    EXPECT_NE(layer.get(parent), nullptr);
    EXPECT_EQ(layer.get(first), nullptr);
    EXPECT_EQ(layer.related_ids(parent), std::vector<uint32_t> { second });
}

TEST(FormaLayerRemovalTest, RemovingTheLastChildClearsTheParentRelationEntry)
{
    Layer layer;
    const uint32_t parent = layer.add(whole());
    const uint32_t child = layer.add(whole());
    ASSERT_TRUE(layer.relate(parent, child));

    ASSERT_TRUE(layer.remove(child));

    EXPECT_TRUE(layer.related_ids(parent).empty());
    EXPECT_EQ(layer.closure(parent), std::vector<uint32_t> { parent });
}

TEST(FormaLayerRemovalTest, RemoveTakesRemovedElementsOutOfHitTesting)
{
    Layer layer;
    const uint32_t below = layer.add(whole());
    const uint32_t above = layer.add(whole());
    ASSERT_EQ(layer.hit_test(k_origin), above);

    ASSERT_TRUE(layer.remove(above));

    EXPECT_EQ(layer.hit_test(k_origin), below);
    EXPECT_FALSE(layer.remove(above));
}

TEST(FormaLayerRemovalTest, IdsAreNotReusedAfterRemoval)
{
    Layer layer;
    const uint32_t a = layer.add(whole());
    ASSERT_TRUE(layer.remove(a));

    const uint32_t b = layer.add(whole());

    EXPECT_NE(a, b);
}

}
