#include "gtest/gtest.h"
#include "../../support/test_metadata.hpp"

#include "../../support/scripted_window.hpp"

#include "MayaFlux/Portal/Forma/Context.hpp"
#include "MayaFlux/Portal/Forma/Primitives/FormFactory.hpp"
#include "MayaFlux/Vruta/EventManager.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

namespace Forma = Portal::Forma;
using IO::Keys;
using IO::MouseButtons;

namespace {

    const glm::vec2 k_left { -0.5F, 0.F };
    const glm::vec2 k_right { 0.5F, 0.F };

    void expect_near(glm::vec2 a, glm::vec2 b)
    {
        EXPECT_NEAR(a.x, b.x, 1e-5F);
        EXPECT_NEAR(a.y, b.y, 1e-5F);
    }

}

class FormaContextTest : public ::testing::Test {
protected:
    std::shared_ptr<Vruta::EventManager> manager = std::make_shared<Vruta::EventManager>();
    std::shared_ptr<ScriptedWindow> window = std::make_shared<ScriptedWindow>(200U, 100U);
    std::shared_ptr<Forma::Layer> layer = std::make_shared<Forma::Layer>();
    std::unique_ptr<Forma::Context> ctx = std::make_unique<Forma::Context>(layer, window, manager, "ctx");

    uint32_t add(float x0, float y0, float x1, float y1)
    {
        return layer->add(Forma::Element {}.with_rect({ x0, y0 }, { x1, y1 }));
    }

    uint32_t left_half() { return add(-1.F, -1.F, -0.1F, 1.F); }
    uint32_t right_half() { return add(0.1F, -1.F, 1.F, 1.F); }
    uint32_t whole() { return add(-1.F, -1.F, 1.F, 1.F); }

    void focus(uint32_t id, glm::vec2 at)
    {
        window->move_to_ndc(at);
        window->press(MouseButtons::Left);
        ASSERT_EQ(ctx->focused(), id);
    }
};

// =============================================================================
// Registration and lifetime
// =============================================================================

TEST_F(FormaContextTest, RegistersOneNamedEventPerInputKind)
{
    for (const char* suffix : { "_move", "_press_left", "_release_left", "_press_right", "_release_right",
             "_press_middle", "_release_middle", "_scroll", "_text", "_resize", "_close" }) {
        EXPECT_NE(manager->get_event(std::string("ctx") + suffix), nullptr) << suffix;
    }
}

TEST_F(FormaContextTest, ANullEventManagerIsRefused)
{
    EXPECT_THROW(Forma::Context(layer, window, nullptr, "null_manager"), std::invalid_argument);
}

TEST_F(FormaContextTest, TheContextKeepsItsEventManagerAliveAfterEveryOtherOwnerLetsGo)
{
    auto owned = std::make_shared<Vruta::EventManager>();
    const std::weak_ptr<Vruta::EventManager> weak = owned;
    auto local = std::make_unique<Forma::Context>(layer, window, owned, "keepalive");
    owned.reset();

    EXPECT_FALSE(weak.expired());
    ASSERT_NE(weak.lock()->get_event("keepalive_press_left"), nullptr);

    local.reset();

    EXPECT_TRUE(weak.expired());
}

TEST_F(FormaContextTest, DestroyingTheContextAfterTheEngineDroppedItsEventManagerCancelsCleanly)
{
    auto owned = std::make_shared<Vruta::EventManager>();
    auto local = std::make_unique<Forma::Context>(layer, window, owned, "late");
    const std::weak_ptr<Vruta::EventManager> weak = owned;
    owned.reset();

    EXPECT_NO_THROW(local.reset());
    EXPECT_TRUE(weak.expired());
}

TEST_F(FormaContextTest, DestroyingTheContextCancelsEveryHandler)
{
    const uint32_t id = whole();
    int presses = 0;
    ctx->on_press(id, MouseButtons::Left, [&](uint32_t, glm::vec2) { ++presses; });
    ctx->on_press(id, Keys::A, [](uint32_t) { });

    ctx.reset();

    for (const char* suffix : { "_move", "_press_left", "_release_left", "_press_right", "_release_right",
             "_press_middle", "_release_middle", "_scroll", "_text", "_resize", "_close" }) {
        EXPECT_EQ(manager->get_event(std::string("ctx") + suffix), nullptr) << suffix;
    }
    EXPECT_EQ(manager->get_event("ctx_key_press_" + std::to_string(static_cast<int>(Keys::A))), nullptr);
    window->move_to_ndc(k_left);
    window->press(MouseButtons::Left);
    EXPECT_EQ(presses, 0);
}

TEST_F(FormaContextTest, KeyListenersRegisterOncePerKeyNotPerElement)
{
    const uint32_t a = left_half();
    const uint32_t b = right_half();
    const size_t before = manager->get_all_events().size();

    ctx->on_press(a, Keys::A, [](uint32_t) { });
    const size_t after_first = manager->get_all_events().size();
    ctx->on_press(b, Keys::A, [](uint32_t) { });

    EXPECT_EQ(after_first, before + 1);
    EXPECT_EQ(manager->get_all_events().size(), after_first);
}

// =============================================================================
// Pointer buttons
// =============================================================================

TEST_F(FormaContextTest, PressFiresTheElementCallbackWithThePressPosition)
{
    const uint32_t id = left_half();
    std::vector<glm::vec2> got;
    uint32_t got_id = 0;
    ctx->on_press(id, MouseButtons::Left, [&](uint32_t pid, glm::vec2 ndc) {
        got_id = pid;
        got.push_back(ndc);
    });

    window->move_to_ndc(k_left);
    window->press(MouseButtons::Left);

    ASSERT_EQ(got.size(), 1U);
    EXPECT_EQ(got_id, id);
    expect_near(got[0], k_left);
}

TEST_F(FormaContextTest, PressOutsideEveryElementFiresNothing)
{
    const uint32_t id = left_half();
    int presses = 0;
    ctx->on_press(id, MouseButtons::Left, [&](uint32_t, glm::vec2) { ++presses; });

    window->move_to_ndc(k_right);
    window->press(MouseButtons::Left);

    EXPECT_EQ(presses, 0);
}

TEST_F(FormaContextTest, PressReachesOnlyTheTopmostElement)
{
    const uint32_t below = whole();
    const uint32_t above = whole();
    int below_presses = 0;
    int above_presses = 0;
    ctx->on_press(below, MouseButtons::Left, [&](uint32_t, glm::vec2) { ++below_presses; });
    ctx->on_press(above, MouseButtons::Left, [&](uint32_t, glm::vec2) { ++above_presses; });

    window->move_to_ndc(k_left);
    window->press(MouseButtons::Left);

    EXPECT_EQ(above_presses, 1);
    EXPECT_EQ(below_presses, 0);
}

TEST_F(FormaContextTest, EachButtonHasItsOwnPressCallback)
{
    const uint32_t id = whole();
    int lefts = 0;
    int rights = 0;
    ctx->on_press(id, MouseButtons::Left, [&](uint32_t, glm::vec2) { ++lefts; });
    ctx->on_press(id, MouseButtons::Right, [&](uint32_t, glm::vec2) { ++rights; });

    window->move_to_ndc(k_left);
    window->press(MouseButtons::Right);
    window->press(MouseButtons::Right);
    window->press(MouseButtons::Left);

    EXPECT_EQ(lefts, 1);
    EXPECT_EQ(rights, 2);
}

TEST_F(FormaContextTest, MiddleButtonCallbacksFireLikeLeftAndRight)
{
    const uint32_t id = whole();
    int presses = 0;
    int releases = 0;
    ctx->on_press(id, MouseButtons::Middle, [&](uint32_t, glm::vec2) { ++presses; });
    ctx->on_release(id, MouseButtons::Middle, [&](uint32_t, glm::vec2) { ++releases; });

    window->move_to_ndc(k_left);
    window->press(MouseButtons::Middle);
    window->release(MouseButtons::Middle);

    EXPECT_EQ(presses, 1);
    EXPECT_EQ(releases, 1);
}

TEST_F(FormaContextTest, ReleaseFiresOnTheElementUnderTheCursorNotThePressedOne)
{
    const uint32_t pressed = left_half();
    const uint32_t under = right_half();
    int pressed_releases = 0;
    std::vector<glm::vec2> under_releases;
    ctx->on_release(pressed, MouseButtons::Left, [&](uint32_t, glm::vec2) { ++pressed_releases; });
    ctx->on_release(under, MouseButtons::Left, [&](uint32_t, glm::vec2 ndc) { under_releases.push_back(ndc); });

    window->move_to_ndc(k_left);
    window->press(MouseButtons::Left);
    window->move_to_ndc(k_right);
    window->release(MouseButtons::Left);

    EXPECT_EQ(pressed_releases, 0);
    ASSERT_EQ(under_releases.size(), 1U);
    expect_near(under_releases[0], k_right);
}

TEST_F(FormaContextTest, ReleaseOutsideEveryElementFiresNothing)
{
    const uint32_t id = left_half();
    int releases = 0;
    ctx->on_release(id, MouseButtons::Left, [&](uint32_t, glm::vec2) { ++releases; });

    window->move_to_ndc(k_right);
    window->release(MouseButtons::Left);

    EXPECT_EQ(releases, 0);
}

// =============================================================================
// Hover
// =============================================================================

TEST_F(FormaContextTest, FirstMoveOverAnElementEntersItAndReportsTheMove)
{
    const uint32_t id = left_half();
    int enters = 0;
    std::vector<glm::vec2> moves;
    ctx->on_enter(id, [&](uint32_t) { ++enters; });
    ctx->on_move(id, [&](uint32_t, glm::vec2 ndc) { moves.push_back(ndc); });

    window->move_to_ndc(k_left);

    EXPECT_EQ(enters, 1);
    ASSERT_EQ(moves.size(), 1U);
    expect_near(moves[0], k_left);
    EXPECT_EQ(ctx->hovered(), id);
}

TEST_F(FormaContextTest, MoveOverEmptySpaceLeavesNothingHovered)
{
    const uint32_t id = left_half();
    int enters = 0;
    ctx->on_enter(id, [&](uint32_t) { ++enters; });

    window->move_to_ndc(k_right);

    EXPECT_EQ(enters, 0);
    EXPECT_FALSE(ctx->hovered().has_value());
}

TEST_F(FormaContextTest, EnterAndLeaveFollowTheCursorAcrossElements)
{
    const uint32_t a = left_half();
    const uint32_t b = right_half();
    std::vector<std::string> log;
    ctx->on_enter(a, [&](uint32_t) { log.emplace_back("enter a"); });
    ctx->on_leave(a, [&](uint32_t) { log.emplace_back("leave a"); });
    ctx->on_enter(b, [&](uint32_t) { log.emplace_back("enter b"); });
    ctx->on_leave(b, [&](uint32_t) { log.emplace_back("leave b"); });

    window->move_to_ndc(k_left);
    window->move_to_ndc(k_right);
    window->move_to_ndc({ 0.F, 0.F });

    EXPECT_EQ(log, (std::vector<std::string> { "enter a", "leave a", "enter b", "leave b" }));
    EXPECT_FALSE(ctx->hovered().has_value());
}

TEST_F(FormaContextTest, EveryMotionEventReachesBothTheMoveAndTheDragListeners)
{
    const uint32_t id = whole();
    int moves = 0;
    int drags = 0;
    ctx->on_move(id, [&](uint32_t, glm::vec2) { ++moves; });
    ctx->on_drag(id, MouseButtons::Left, [&](uint32_t, glm::vec2) { ++drags; });
    window->press(MouseButtons::Left);

    for (int i = 0; i < 6; ++i)
        window->move_to_ndc({ 0.1F * static_cast<float>(i), 0.F });

    EXPECT_EQ(moves, 6);
    EXPECT_EQ(drags, 6);
}

// =============================================================================
// Drag
// =============================================================================

TEST_F(FormaContextTest, DragKeepsTrackingThePressedElementOutsideItsBounds)
{
    const uint32_t id = left_half();
    std::vector<glm::vec2> drags;
    ctx->on_drag(id, MouseButtons::Left, [&](uint32_t did, glm::vec2 ndc) {
        EXPECT_EQ(did, id);
        drags.push_back(ndc);
    });

    window->move_to_ndc(k_left);
    window->press(MouseButtons::Left);
    window->move_to_ndc(k_right);

    ASSERT_EQ(drags.size(), 1U);
    expect_near(drags[0], k_right);
}

TEST_F(FormaContextTest, MotionWithoutAHeldButtonIsNotADrag)
{
    const uint32_t id = whole();
    int drags = 0;
    ctx->on_drag(id, MouseButtons::Left, [&](uint32_t, glm::vec2) { ++drags; });

    window->move_to_ndc(k_left);
    window->move_to_ndc(k_right);

    EXPECT_EQ(drags, 0);
}

TEST_F(FormaContextTest, WireDragForwardsOnlyThePosition)
{
    const uint32_t id = left_half();
    std::vector<glm::vec2> seen;
    ctx->wire_drag(id, [&](glm::vec2 ndc) { seen.push_back(ndc); });

    window->move_to_ndc(k_left);
    window->press(MouseButtons::Left);
    window->move_to_ndc(k_right);

    ASSERT_EQ(seen.size(), 1U);
    expect_near(seen[0], k_right);
}

TEST_F(FormaContextTest, WireDragIgnoresAnEmptySink)
{
    const uint32_t id = whole();

    EXPECT_EQ(&ctx->wire_drag(id, {}), ctx.get());
}

TEST_F(FormaContextTest, CanvasDragWritesTheSampleUnderTheCursor)
{
    const uint32_t id = whole();
    auto state = std::make_shared<Forma::MappedState<std::vector<float>>>();
    state->value.assign(5, 0.F);
    const Kinesis::AABB2D bounds { .min = { -1.F, -1.F }, .max = { 1.F, 1.F } };
    Forma::Geometry::wire_canvas_drag(*ctx, id, state, bounds);

    window->move_to_ndc({ 0.F, 0.F });
    window->press(MouseButtons::Left);
    window->move_to_ndc({ 1.F, 1.F });

    EXPECT_EQ(state->version, 1U);
    EXPECT_FLOAT_EQ(state->value.back(), 1.F);
    EXPECT_FLOAT_EQ(state->value.front(), 0.F);
}

// =============================================================================
// Scroll
// =============================================================================

TEST_F(FormaContextTest, ScrollReachesTheHoveredElementWithPositionAndDeltas)
{
    const uint32_t id = left_half();
    uint32_t got_id = 0;
    glm::vec2 got_ndc {};
    double got_dx = 0.0;
    double got_dy = 0.0;
    ctx->on_scroll(id, [&](uint32_t sid, glm::vec2 ndc, double dx, double dy) {
        got_id = sid;
        got_ndc = ndc;
        got_dx = dx;
        got_dy = dy;
    });

    window->move_to_ndc(k_left);
    window->scroll(1.5, -2.0);

    EXPECT_EQ(got_id, id);
    expect_near(got_ndc, k_left);
    EXPECT_DOUBLE_EQ(got_dx, 1.5);
    EXPECT_DOUBLE_EQ(got_dy, -2.0);
}

TEST_F(FormaContextTest, ScrollWithNothingHoveredIsIgnored)
{
    const uint32_t id = left_half();
    int scrolls = 0;
    ctx->on_scroll(id, [&](uint32_t, glm::vec2, double, double) { ++scrolls; });

    window->move_to_ndc(k_right);
    window->scroll(0.0, 1.0);

    EXPECT_EQ(scrolls, 0);
}

// =============================================================================
// Focus
// =============================================================================

TEST_F(FormaContextTest, PressFocusesTheElementAndFiresFocusGainedOnce)
{
    const uint32_t id = left_half();
    int gained = 0;
    ctx->on_focus_gained(id, [&](uint32_t) { ++gained; });

    focus(id, k_left);
    window->press(MouseButtons::Left);

    EXPECT_EQ(gained, 1);
    EXPECT_EQ(ctx->focused(), id);
}

TEST_F(FormaContextTest, PressingAnotherElementFiresLostBeforeGained)
{
    const uint32_t a = left_half();
    const uint32_t b = right_half();
    std::vector<std::string> log;
    ctx->on_focus_gained(a, [&](uint32_t) { log.emplace_back("gained a"); });
    ctx->on_focus_lost(a, [&](uint32_t) { log.emplace_back("lost a"); });
    ctx->on_focus_gained(b, [&](uint32_t) { log.emplace_back("gained b"); });

    focus(a, k_left);
    window->move_to_ndc(k_right);
    window->press(MouseButtons::Left);

    EXPECT_EQ(log, (std::vector<std::string> { "gained a", "lost a", "gained b" }));
    EXPECT_EQ(ctx->focused(), b);
}

TEST_F(FormaContextTest, PressingEmptySpaceClearsFocus)
{
    const uint32_t id = left_half();
    int lost = 0;
    ctx->on_focus_lost(id, [&](uint32_t) { ++lost; });
    focus(id, k_left);

    window->move_to_ndc(k_right);
    window->press(MouseButtons::Left);

    EXPECT_EQ(lost, 1);
    EXPECT_FALSE(ctx->focused().has_value());
}

TEST_F(FormaContextTest, ClearFocusFiresLostOnceAndIsIdempotent)
{
    const uint32_t id = left_half();
    int lost = 0;
    ctx->on_focus_lost(id, [&](uint32_t) { ++lost; });
    focus(id, k_left);

    ctx->clear_focus();
    ctx->clear_focus();

    EXPECT_EQ(lost, 1);
    EXPECT_FALSE(ctx->focused().has_value());
}

// =============================================================================
// Keyboard and text
// =============================================================================

TEST_F(FormaContextTest, KeyPressReachesOnlyTheFocusedElement)
{
    const uint32_t a = left_half();
    const uint32_t b = right_half();
    int a_hits = 0;
    int b_hits = 0;
    ctx->on_press(a, Keys::A, [&](uint32_t) { ++a_hits; });
    ctx->on_press(b, Keys::A, [&](uint32_t) { ++b_hits; });

    window->key_down(Keys::A);
    EXPECT_EQ(a_hits + b_hits, 0);

    focus(a, k_left);
    window->key_down(Keys::A);

    EXPECT_EQ(a_hits, 1);
    EXPECT_EQ(b_hits, 0);
}

TEST_F(FormaContextTest, OnlyTheBoundKeyFires)
{
    const uint32_t id = whole();
    int hits = 0;
    ctx->on_press(id, Keys::A, [&](uint32_t) { ++hits; });
    focus(id, k_left);

    window->key_down(Keys::B);

    EXPECT_EQ(hits, 0);
}

TEST_F(FormaContextTest, KeyReleaseFiresOnTheFocusedElement)
{
    const uint32_t id = whole();
    int releases = 0;
    ctx->on_release(id, Keys::A, [&](uint32_t) { ++releases; });
    focus(id, k_left);

    window->key_down(Keys::A);
    window->key_up(Keys::A);

    EXPECT_EQ(releases, 1);
}

TEST_F(FormaContextTest, HeldFiresOnEachPressAndIgnoresRelease)
{
    const uint32_t id = whole();
    int held = 0;
    ctx->on_held(id, Keys::A, [&](uint32_t) { ++held; });
    focus(id, k_left);

    window->key_down(Keys::A);
    window->key_down(Keys::A);
    window->key_up(Keys::A);

    EXPECT_EQ(held, 2);
}

TEST_F(FormaContextTest, PressAndHoldFiresOncePerPhysicalPress)
{
    const uint32_t id = whole();
    int hits = 0;
    ctx->press_and_hold(id, Keys::A, [&](uint32_t) { ++hits; });
    focus(id, k_left);

    window->key_down(Keys::A);

    EXPECT_EQ(hits, 1);
}

TEST_F(FormaContextTest, APressAndAHeldHandlerOnTheSameKeyEachSeeThePress)
{
    const uint32_t id = whole();
    int pressed = 0;
    int held = 0;
    ctx->on_press(id, Keys::A, [&](uint32_t) { ++pressed; });
    ctx->on_held(id, Keys::A, [&](uint32_t) { ++held; });
    focus(id, k_left);

    window->key_down(Keys::A);

    EXPECT_EQ(pressed, 1);
    EXPECT_EQ(held, 1);
}

TEST_F(FormaContextTest, OfTwoOverlappingContextsOnlyTheLaterOneReceivesAPress)
{
    const uint32_t first_id = whole();
    int first_presses = 0;
    ctx->on_press(first_id, MouseButtons::Left, [&](uint32_t, glm::vec2) { ++first_presses; });

    auto second_layer = std::make_shared<Forma::Layer>();
    Forma::Context second(second_layer, window, manager, "ctx_second");
    const uint32_t second_id = second_layer->add(Forma::Element {}.with_rect({ -1.F, -1.F }, { 1.F, 1.F }));
    int second_presses = 0;
    second.on_press(second_id, MouseButtons::Left, [&](uint32_t, glm::vec2) { ++second_presses; });

    window->move_to_ndc(k_left);
    window->press(MouseButtons::Left);
    window->release(MouseButtons::Left);

    EXPECT_EQ(first_presses, 0);
    EXPECT_EQ(second_presses, 1);
}

TEST_F(FormaContextTest, InputPriorityOutranksTheOrderContextsWereMadeIn)
{
    const uint32_t first_id = whole();
    int first_presses = 0;
    ctx->on_press(first_id, MouseButtons::Left, [&](uint32_t, glm::vec2) { ++first_presses; });

    auto second_layer = std::make_shared<Forma::Layer>();
    Forma::Context second(second_layer, window, manager, "ctx_second");
    const uint32_t second_id = second_layer->add(Forma::Element {}.with_rect({ -1.F, -1.F }, { 1.F, 1.F }));
    int second_presses = 0;
    second.on_press(second_id, MouseButtons::Left, [&](uint32_t, glm::vec2) { ++second_presses; });
    ctx->set_input_priority(1);

    window->move_to_ndc(k_left);
    window->press(MouseButtons::Left);
    window->release(MouseButtons::Left);

    EXPECT_EQ(first_presses, 1);
    EXPECT_EQ(second_presses, 0);
}

TEST_F(FormaContextTest, ContextsThatDoNotOverlapEachKeepTheirOwnPartOfTheWindow)
{
    const uint32_t first_id = left_half();
    int first_presses = 0;
    ctx->on_press(first_id, MouseButtons::Left, [&](uint32_t, glm::vec2) { ++first_presses; });

    auto second_layer = std::make_shared<Forma::Layer>();
    Forma::Context second(second_layer, window, manager, "ctx_second");
    const uint32_t second_id = second_layer->add(Forma::Element {}.with_rect({ 0.1F, -1.F }, { 1.F, 1.F }));
    int second_presses = 0;
    second.on_press(second_id, MouseButtons::Left, [&](uint32_t, glm::vec2) { ++second_presses; });

    window->move_to_ndc(k_left);
    window->press(MouseButtons::Left);
    window->release(MouseButtons::Left);
    EXPECT_EQ(ctx->focused(), first_id);

    window->move_to_ndc(k_right);
    window->press(MouseButtons::Left);
    window->release(MouseButtons::Left);

    EXPECT_EQ(first_presses, 1);
    EXPECT_EQ(second_presses, 1);
    EXPECT_FALSE(ctx->focused().has_value());
    EXPECT_EQ(second.focused(), second_id);
}

TEST_F(FormaContextTest, ADragStaysWithItsContextAcrossAnotherThatOutranksIt)
{
    const uint32_t first_id = left_half();
    int drags = 0;
    ctx->on_drag(first_id, MouseButtons::Left, [&](uint32_t, glm::vec2) { ++drags; });

    auto second_layer = std::make_shared<Forma::Layer>();
    Forma::Context second(second_layer, window, manager, "ctx_second");
    const uint32_t second_id = second_layer->add(Forma::Element {}.with_rect({ 0.1F, -1.F }, { 1.F, 1.F }));
    int entered = 0;
    int stolen = 0;
    second.on_enter(second_id, [&](uint32_t) { ++entered; });
    second.on_drag(second_id, MouseButtons::Left, [&](uint32_t, glm::vec2) { ++stolen; });
    second.set_input_priority(5);

    window->move_to_ndc(k_left);
    window->press(MouseButtons::Left);
    window->move_to_ndc(k_right);

    EXPECT_EQ(drags, 1);
    EXPECT_EQ(stolen, 0);
    EXPECT_EQ(entered, 0);

    window->release(MouseButtons::Left);
    window->move_to_ndc(k_right);

    EXPECT_EQ(entered, 1);
}

TEST_F(FormaContextTest, ADestroyedContextGivesUpItsClaim)
{
    const uint32_t first_id = whole();
    int first_presses = 0;
    ctx->on_press(first_id, MouseButtons::Left, [&](uint32_t, glm::vec2) { ++first_presses; });

    {
        auto second_layer = std::make_shared<Forma::Layer>();
        Forma::Context second(second_layer, window, manager, "ctx_second");
        (void)second_layer->add(Forma::Element {}.with_rect({ -1.F, -1.F }, { 1.F, 1.F }));
        window->move_to_ndc(k_left);
        window->press(MouseButtons::Left);
    }

    window->release(MouseButtons::Left);
    window->press(MouseButtons::Left);

    EXPECT_EQ(first_presses, 1);
}

TEST_F(FormaContextTest, KeyStepMovesTheStateByDeltaAndClamps)
{
    const uint32_t id = whole();
    auto state = std::make_shared<Forma::MappedState<float>>();
    state->write(0.5F);
    ctx->key_step(id, state, Keys::A, Keys::B, 0.25F);
    focus(id, k_left);

    window->key_down(Keys::B);
    EXPECT_FLOAT_EQ(state->value, 0.75F);
    window->key_down(Keys::B);
    window->key_down(Keys::B);
    EXPECT_FLOAT_EQ(state->value, 1.F);
    window->key_down(Keys::A);
    EXPECT_FLOAT_EQ(state->value, 0.75F);
    EXPECT_EQ(state->version, 5U);
}

TEST_F(FormaContextTest, TextReachesTheFocusedElementOnly)
{
    const uint32_t id = left_half();
    std::vector<uint32_t> typed;
    ctx->on_text(id, [&](uint32_t, uint32_t cp) { typed.push_back(cp); });

    window->type(0x41);
    EXPECT_TRUE(typed.empty());

    focus(id, k_left);
    window->type(0x41);
    window->type(0x20AC);

    EXPECT_EQ(typed, (std::vector<uint32_t> { 0x41, 0x20AC }));
}

// =============================================================================
// Resize, unbind and close
// =============================================================================

TEST_F(FormaContextTest, ResizeReachesEveryBoundElementWithTheNewSize)
{
    const uint32_t a = left_half();
    const uint32_t b = right_half();
    std::vector<std::pair<uint32_t, uint32_t>> sizes_a;
    std::vector<std::pair<uint32_t, uint32_t>> sizes_b;
    ctx->on_resize(a, [&](uint32_t w, uint32_t h) { sizes_a.emplace_back(w, h); });
    ctx->on_resize(b, [&](uint32_t w, uint32_t h) { sizes_b.emplace_back(w, h); });

    window->resize(400, 300);

    EXPECT_EQ(sizes_a, (std::vector<std::pair<uint32_t, uint32_t>> { { 400, 300 } }));
    EXPECT_EQ(sizes_b, (std::vector<std::pair<uint32_t, uint32_t>> { { 400, 300 } }));
}

TEST_F(FormaContextTest, SyncSizeRunsTheResizeCallbacksOnlyWhenTheSizeChangedWithoutAnEvent)
{
    const uint32_t a = left_half();
    std::vector<std::pair<uint32_t, uint32_t>> sizes;
    ctx->on_resize(a, [&](uint32_t w, uint32_t h) { sizes.emplace_back(w, h); });

    ctx->sync_size();
    EXPECT_TRUE(sizes.empty());

    window->change_size_silently(400, 300);
    ctx->sync_size();
    ctx->sync_size();

    EXPECT_EQ(sizes, (std::vector<std::pair<uint32_t, uint32_t>> { { 400, 300 } }));
}

TEST_F(FormaContextTest, AnInputEventCatchesTheResizeCallbacksUpWithAMissedSize)
{
    const uint32_t a = left_half();
    std::vector<std::pair<uint32_t, uint32_t>> sizes;
    ctx->on_resize(a, [&](uint32_t w, uint32_t h) { sizes.emplace_back(w, h); });
    window->change_size_silently(400, 300);

    window->move_to_ndc({ -0.5F, 0.F });

    EXPECT_EQ(sizes, (std::vector<std::pair<uint32_t, uint32_t>> { { 400, 300 } }));
}

TEST_F(FormaContextTest, PixelRectsConvertToNdcFromTheLiveWindowSize)
{
    const auto full = ctx->to_ndc_rect(0.0, 0.0, 200.0, 100.0);
    expect_near(full.min, { -1.F, -1.F });
    expect_near(full.max, { 1.F, 1.F });

    const auto top_left_quarter = ctx->to_ndc_rect(0.0, 0.0, 100.0, 50.0);
    expect_near(top_left_quarter.min, { -1.F, 0.F });
    expect_near(top_left_quarter.max, { 0.F, 1.F });

    window->resize(400, 200);
    const auto after = ctx->to_ndc_rect(0.0, 0.0, 200.0, 100.0);
    expect_near(after.min, { -1.F, 0.F });
    expect_near(after.max, { 0.F, 1.F });
}

TEST_F(FormaContextTest, UnbindRemovesCallbacksAndReleasesFocusHoverAndDrag)
{
    const uint32_t id = left_half();
    int presses = 0;
    int lost = 0;
    int drags = 0;
    ctx->on_press(id, MouseButtons::Left, [&](uint32_t, glm::vec2) { ++presses; });
    ctx->on_focus_lost(id, [&](uint32_t) { ++lost; });
    ctx->on_drag(id, MouseButtons::Left, [&](uint32_t, glm::vec2) { ++drags; });
    focus(id, k_left);
    ASSERT_EQ(presses, 1);
    ASSERT_EQ(ctx->hovered(), id);

    ctx->unbind(id);

    EXPECT_EQ(lost, 1);
    EXPECT_FALSE(ctx->focused().has_value());
    EXPECT_FALSE(ctx->hovered().has_value());
    window->move_to_ndc(k_right);
    EXPECT_EQ(drags, 0);
    window->move_to_ndc(k_left);
    window->press(MouseButtons::Left);
    EXPECT_EQ(presses, 1);
}

TEST_F(FormaContextTest, CloseFiresCloseCallbacksAndFocusLostForTheFocusedElementOnly)
{
    const uint32_t focused = left_half();
    const uint32_t other = right_half();
    int closes = 0;
    int focused_lost = 0;
    int other_lost = 0;
    ctx->on_close(focused, [&] { ++closes; });
    ctx->on_close(other, [&] { ++closes; });
    ctx->on_focus_lost(focused, [&](uint32_t) { ++focused_lost; });
    ctx->on_focus_lost(other, [&](uint32_t) { ++other_lost; });
    focus(focused, k_left);

    window->close();

    EXPECT_EQ(closes, 2);
    EXPECT_EQ(focused_lost, 1);
    EXPECT_EQ(other_lost, 0);
    EXPECT_FALSE(ctx->focused().has_value());
}

TEST_F(FormaContextTest, CloseDetachesTheContextFromFurtherInput)
{
    const uint32_t id = whole();
    int presses = 0;
    ctx->on_press(id, MouseButtons::Left, [&](uint32_t, glm::vec2) { ++presses; });

    window->close();
    window->move_to_ndc(k_left);
    window->press(MouseButtons::Left);

    EXPECT_EQ(presses, 0);
    EXPECT_EQ(manager->get_event("ctx_close"), nullptr);
    EXPECT_EQ(manager->get_event("ctx_press_left"), nullptr);
}

// =============================================================================
// Drag per button
// =============================================================================

TEST_F(FormaContextTest, EachButtonDragsOnlyWhileThatButtonIsHeld)
{
    const uint32_t id = whole();
    int lefts = 0;
    int rights = 0;
    int middles = 0;
    ctx->on_drag(id, MouseButtons::Left, [&](uint32_t, glm::vec2) { ++lefts; });
    ctx->on_drag(id, MouseButtons::Right, [&](uint32_t, glm::vec2) { ++rights; });
    ctx->on_drag(id, MouseButtons::Middle, [&](uint32_t, glm::vec2) { ++middles; });
    window->move_to_ndc(k_left);

    window->press(MouseButtons::Right);
    window->move_to_ndc(k_right);
    EXPECT_EQ(lefts, 0);
    EXPECT_EQ(rights, 1);
    EXPECT_EQ(middles, 0);

    window->release(MouseButtons::Right);
    window->press(MouseButtons::Middle);
    window->move_to_ndc(k_left);
    EXPECT_EQ(lefts, 0);
    EXPECT_EQ(rights, 1);
    EXPECT_EQ(middles, 1);
}

TEST_F(FormaContextTest, ReleasingTheButtonEndsTheDrag)
{
    const uint32_t id = whole();
    int drags = 0;
    ctx->on_drag(id, MouseButtons::Left, [&](uint32_t, glm::vec2) { ++drags; });
    window->move_to_ndc(k_left);
    window->press(MouseButtons::Left);
    window->move_to_ndc(k_right);
    ASSERT_EQ(drags, 1);

    window->release(MouseButtons::Left);
    window->move_to_ndc(k_left);
    window->move_to_ndc(k_right);

    EXPECT_EQ(drags, 1);
}

TEST_F(FormaContextTest, HeldButtonsDragIndependentlyOnTheSameMotion)
{
    const uint32_t id = whole();
    int lefts = 0;
    int rights = 0;
    ctx->on_drag(id, MouseButtons::Left, [&](uint32_t, glm::vec2) { ++lefts; });
    ctx->on_drag(id, MouseButtons::Right, [&](uint32_t, glm::vec2) { ++rights; });
    window->move_to_ndc(k_left);
    window->press(MouseButtons::Left);
    window->press(MouseButtons::Right);

    window->move_to_ndc(k_right);

    EXPECT_EQ(lefts, 1);
    EXPECT_EQ(rights, 1);
}

TEST_F(FormaContextTest, HeldButtonWithoutADragCallbackDoesNothing)
{
    const uint32_t id = whole();
    int lefts = 0;
    ctx->on_drag(id, MouseButtons::Left, [&](uint32_t, glm::vec2) { ++lefts; });
    window->move_to_ndc(k_left);

    window->press(MouseButtons::Right);
    window->move_to_ndc(k_right);

    EXPECT_EQ(lefts, 0);
}

TEST_F(FormaContextTest, DragStartedOverEmptySpaceAdoptsTheFirstElementEntered)
{
    const uint32_t id = left_half();
    std::vector<glm::vec2> drags;
    ctx->on_drag(id, MouseButtons::Left, [&](uint32_t, glm::vec2 ndc) { drags.push_back(ndc); });
    window->move_to_ndc(k_right);
    window->press(MouseButtons::Left);

    window->move_to_ndc(k_left);
    window->move_to_ndc(k_right);

    ASSERT_EQ(drags.size(), 2U);
    expect_near(drags[0], k_left);
    expect_near(drags[1], k_right);
}

// =============================================================================
// Additive subscriptions
// =============================================================================

namespace {

    using Core::WindowEventType;

    Vruta::WindowEventFilter filter_for(WindowEventType type,
        std::optional<MouseButtons> button = std::nullopt, std::optional<Keys> key = std::nullopt)
    {
        Vruta::WindowEventFilter filter;
        filter.event_type = type;
        filter.button = button;
        filter.key_code = key;
        return filter;
    }

    Forma::Context::SubscriberFn count_into(int& counter)
    {
        return [&counter](uint32_t, const Core::WindowEvent&, glm::vec2) { ++counter; };
    }

    Forma::Context::SubscriberFn log_into(std::vector<std::string>& log, std::string entry)
    {
        return [&log, entry = std::move(entry)](uint32_t, const Core::WindowEvent&, glm::vec2) {
            log.push_back(entry);
        };
    }

}

TEST_F(FormaContextTest, ASubscriberRunsBesideTheElementsOwnPressCallback)
{
    const uint32_t id = left_half();
    std::vector<std::string> log;
    ctx->on_press(id, MouseButtons::Left, [&](uint32_t, glm::vec2) { log.emplace_back("own"); });
    ASSERT_TRUE(ctx->subscribe(id, filter_for(WindowEventType::MOUSE_BUTTON_PRESSED, MouseButtons::Left),
        log_into(log, "subscriber")));

    window->move_to_ndc(k_left);
    window->press(MouseButtons::Left);

    EXPECT_EQ(log, (std::vector<std::string> { "own", "subscriber" }));
}

TEST_F(FormaContextTest, SubscribersRunInSubscriptionOrderAndSurviveReplacingTheOwnCallback)
{
    const uint32_t id = left_half();
    std::vector<std::string> log;
    const auto press = filter_for(WindowEventType::MOUSE_BUTTON_PRESSED, MouseButtons::Left);
    ctx->on_press(id, MouseButtons::Left, [&](uint32_t, glm::vec2) { log.emplace_back("first own"); });
    ctx->subscribe(id, press, log_into(log, "a"));
    ctx->subscribe(id, press, log_into(log, "b"));
    ctx->on_press(id, MouseButtons::Left, [&](uint32_t, glm::vec2) { log.emplace_back("second own"); });

    window->move_to_ndc(k_left);
    window->press(MouseButtons::Left);

    EXPECT_EQ(log, (std::vector<std::string> { "second own", "a", "b" }));
}

TEST_F(FormaContextTest, ASubscriberReceivesTheWindowEventTheElementAndThePointer)
{
    const uint32_t id = left_half();
    uint32_t got_id = 0;
    std::optional<Core::WindowEvent> got;
    glm::vec2 got_ndc {};
    ctx->subscribe(id, filter_for(WindowEventType::MOUSE_BUTTON_PRESSED, MouseButtons::Left),
        [&](uint32_t sid, const Core::WindowEvent& event, glm::vec2 ndc) {
            got_id = sid;
            got = event;
            got_ndc = ndc;
        });

    window->move_to_ndc(k_left);
    window->press(MouseButtons::Left);

    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got_id, id);
    EXPECT_EQ(got->type, WindowEventType::MOUSE_BUTTON_PRESSED);
    const auto* data = std::get_if<Core::WindowEvent::MouseButtonData>(&got->data);
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(data->button, static_cast<int8_t>(MouseButtons::Left));
    expect_near(got_ndc, k_left);
}

TEST_F(FormaContextTest, AButtonInTheFilterSelectsThatButtonAndNoButtonSelectsAny)
{
    const uint32_t id = left_half();
    int rights = 0;
    int any = 0;
    ctx->subscribe(id, filter_for(WindowEventType::MOUSE_BUTTON_PRESSED, MouseButtons::Right), count_into(rights));
    ctx->subscribe(id, filter_for(WindowEventType::MOUSE_BUTTON_PRESSED), count_into(any));

    window->move_to_ndc(k_left);
    window->press(MouseButtons::Left);
    EXPECT_EQ(rights, 0);
    EXPECT_EQ(any, 1);

    window->press(MouseButtons::Right);
    EXPECT_EQ(rights, 1);
    EXPECT_EQ(any, 2);
}

TEST_F(FormaContextTest, UnbindRemovesTheOwnCallbackAndEverySubscriber)
{
    const uint32_t id = left_half();
    int calls = 0;
    ctx->on_press(id, MouseButtons::Left, [&](uint32_t, glm::vec2) { ++calls; });
    ctx->subscribe(id, filter_for(WindowEventType::MOUSE_BUTTON_PRESSED, MouseButtons::Left), count_into(calls));
    ctx->subscribe(id, filter_for(WindowEventType::MOUSE_ENTERED), count_into(calls));

    ctx->unbind(id);
    window->move_to_ndc(k_left);
    window->press(MouseButtons::Left);

    EXPECT_EQ(calls, 0);
}

TEST_F(FormaContextTest, ReleaseSubscribersRunBesideOnRelease)
{
    const uint32_t id = left_half();
    std::vector<std::string> log;
    ctx->on_release(id, MouseButtons::Left, [&](uint32_t, glm::vec2) { log.emplace_back("own"); });
    ctx->subscribe(id, filter_for(WindowEventType::MOUSE_BUTTON_RELEASED, MouseButtons::Left),
        log_into(log, "subscriber"));

    window->move_to_ndc(k_left);
    window->press(MouseButtons::Left);
    window->release(MouseButtons::Left);

    EXPECT_EQ(log, (std::vector<std::string> { "own", "subscriber" }));
}

TEST_F(FormaContextTest, MotionWithoutAButtonIsHoverAndMotionWithAButtonIsTheDrag)
{
    const uint32_t id = whole();
    int hover = 0;
    int drag = 0;
    ctx->subscribe(id, filter_for(WindowEventType::MOUSE_MOTION), count_into(hover));
    ctx->subscribe(id, filter_for(WindowEventType::MOUSE_MOTION, MouseButtons::Left), count_into(drag));

    window->move_to_ndc(k_left);
    EXPECT_EQ(hover, 1);
    EXPECT_EQ(drag, 0);

    window->press(MouseButtons::Left);
    window->move_to_ndc(k_right);
    EXPECT_EQ(hover, 2);
    EXPECT_EQ(drag, 1);
}

TEST_F(FormaContextTest, ADragSubscriberStartsCaptureOnItsOwnAndKeepsTrackingOutsideTheElement)
{
    const uint32_t id = left_half();
    std::vector<glm::vec2> drags;
    ctx->subscribe(id, filter_for(WindowEventType::MOUSE_MOTION, MouseButtons::Left),
        [&](uint32_t did, const Core::WindowEvent&, glm::vec2 ndc) {
            EXPECT_EQ(did, id);
            drags.push_back(ndc);
        });

    window->move_to_ndc(k_left);
    window->press(MouseButtons::Left);
    window->move_to_ndc(k_right);

    ASSERT_EQ(drags.size(), 1U);
    expect_near(drags[0], k_right);
}

TEST_F(FormaContextTest, ADragSubscriberRunsBesideOnDrag)
{
    const uint32_t id = left_half();
    std::vector<std::string> log;
    ctx->on_drag(id, MouseButtons::Left, [&](uint32_t, glm::vec2) { log.emplace_back("own"); });
    ctx->subscribe(id, filter_for(WindowEventType::MOUSE_MOTION, MouseButtons::Left), log_into(log, "subscriber"));

    window->move_to_ndc(k_left);
    window->press(MouseButtons::Left);
    window->move_to_ndc(k_right);

    EXPECT_EQ(log, (std::vector<std::string> { "own", "subscriber" }));
}

TEST_F(FormaContextTest, EnterMotionAndExitSubscribersRunBesideTheOwnCallbacks)
{
    const uint32_t id = left_half();
    std::vector<std::string> log;
    ctx->on_enter(id, [&](uint32_t) { log.emplace_back("own enter"); });
    ctx->subscribe(id, filter_for(WindowEventType::MOUSE_ENTERED), log_into(log, "sub enter"));
    ctx->on_move(id, [&](uint32_t, glm::vec2) { log.emplace_back("own move"); });
    ctx->subscribe(id, filter_for(WindowEventType::MOUSE_MOTION), log_into(log, "sub move"));
    ctx->on_leave(id, [&](uint32_t) { log.emplace_back("own leave"); });
    ctx->subscribe(id, filter_for(WindowEventType::MOUSE_EXITED), log_into(log, "sub leave"));

    window->move_to_ndc(k_left);
    window->move_to_ndc(k_right);

    EXPECT_EQ(log, (std::vector<std::string> { "own enter", "sub enter", "own move", "sub move",
                       "own leave", "sub leave" }));
}

TEST_F(FormaContextTest, ScrollSubscribersReceiveThePointerAndTheDelta)
{
    const uint32_t id = left_half();
    glm::vec2 got_ndc {};
    std::optional<Core::WindowEvent::ScrollData> got;
    int own = 0;
    ctx->on_scroll(id, [&](uint32_t, glm::vec2, double, double) { ++own; });
    ctx->subscribe(id, filter_for(WindowEventType::MOUSE_SCROLLED),
        [&](uint32_t, const Core::WindowEvent& event, glm::vec2 ndc) {
            got_ndc = ndc;
            if (const auto* data = std::get_if<Core::WindowEvent::ScrollData>(&event.data))
                got = *data;
        });

    window->move_to_ndc(k_left);
    window->scroll(1.5, -2.0);

    EXPECT_EQ(own, 1);
    expect_near(got_ndc, k_left);
    ASSERT_TRUE(got.has_value());
    EXPECT_DOUBLE_EQ(got->x_offset, 1.5);
    EXPECT_DOUBLE_EQ(got->y_offset, -2.0);
}

TEST_F(FormaContextTest, ASubscriberMayUnbindItsElementWhileTheEventIsBeingDelivered)
{
    const uint32_t id = left_half();
    std::vector<std::string> log;
    const auto press = filter_for(WindowEventType::MOUSE_BUTTON_PRESSED, MouseButtons::Left);
    ctx->subscribe(id, press, [&](uint32_t, const Core::WindowEvent&, glm::vec2) {
        log.emplace_back("first");
        ctx->unbind(id);
    });
    ctx->subscribe(id, press, log_into(log, "second"));

    window->move_to_ndc(k_left);
    window->press(MouseButtons::Left);
    window->press(MouseButtons::Left);

    EXPECT_EQ(log, (std::vector<std::string> { "first", "second" }));
}

TEST_F(FormaContextTest, KeySubscribersReceiveTheirKeyOnlyWhileTheElementHasFocus)
{
    const uint32_t id = left_half();
    int presses = 0;
    int releases = 0;
    int repeats = 0;
    int others = 0;
    ctx->subscribe(id, filter_for(WindowEventType::KEY_PRESSED, std::nullopt, Keys::A), count_into(presses));
    ctx->subscribe(id, filter_for(WindowEventType::KEY_RELEASED, std::nullopt, Keys::A), count_into(releases));
    ctx->subscribe(id, filter_for(WindowEventType::KEY_REPEAT, std::nullopt, Keys::A), count_into(repeats));
    ctx->subscribe(id, filter_for(WindowEventType::KEY_PRESSED, std::nullopt, Keys::B), count_into(others));

    window->key_down(Keys::A);
    EXPECT_EQ(presses, 0);

    focus(id, k_left);
    window->key_down(Keys::A);
    window->key_repeat(Keys::A);
    window->key_up(Keys::A);

    EXPECT_EQ(presses, 1);
    EXPECT_EQ(repeats, 1);
    EXPECT_EQ(releases, 1);
    EXPECT_EQ(others, 0);
}

TEST_F(FormaContextTest, AKeySubscriptionReceivesTheKeyInItsEvent)
{
    const uint32_t id = left_half();
    std::optional<Core::WindowEvent::KeyData> got;
    ctx->subscribe(id, filter_for(WindowEventType::KEY_PRESSED, std::nullopt, Keys::A),
        [&](uint32_t, const Core::WindowEvent& event, glm::vec2) {
            if (const auto* data = std::get_if<Core::WindowEvent::KeyData>(&event.data))
                got = *data;
        });

    focus(id, k_left);
    window->key_down(Keys::A);

    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->key, static_cast<int16_t>(Keys::A));
}

TEST_F(FormaContextTest, AKeySubscriptionReceivesTheModifiersHeldWithTheKey)
{
    const uint32_t id = left_half();
    std::vector<IO::Modifiers> seen;
    ctx->subscribe(id, filter_for(WindowEventType::KEY_PRESSED, std::nullopt, Keys::A),
        [&](uint32_t, const Core::WindowEvent& event, glm::vec2) {
            if (const auto* data = std::get_if<Core::WindowEvent::KeyData>(&event.data))
                seen.push_back(data->mods);
        });

    focus(id, k_left);
    window->key_down(Keys::A, IO::Modifiers::Ctrl | IO::Modifiers::Shift);

    EXPECT_EQ(seen, std::vector<IO::Modifiers> { IO::Modifiers::Ctrl | IO::Modifiers::Shift });
}

TEST_F(FormaContextTest, AKeySubscriptionWithModifiersReceivesOnlyThoseModifiers)
{
    const uint32_t id = left_half();
    int calls = 0;
    auto filter = filter_for(WindowEventType::KEY_PRESSED, std::nullopt, Keys::V);
    filter.modifiers = IO::Modifiers::Ctrl;
    ctx->subscribe(id, filter, count_into(calls));

    focus(id, k_left);
    window->key_down(Keys::V);
    window->key_down(Keys::V, IO::Modifiers::Shift);
    EXPECT_EQ(calls, 0);

    window->key_down(Keys::V, IO::Modifiers::Ctrl);
    EXPECT_EQ(calls, 1);
}

TEST_F(FormaContextTest, OnPressWithModifiersFiresOnlyForExactlyThoseHeld)
{
    const uint32_t id = left_half();
    int calls = 0;
    ctx->on_press(id, Keys::V | IO::Modifiers::Ctrl, [&](uint32_t) { ++calls; });

    focus(id, k_left);
    window->key_down(Keys::V);
    window->key_down(Keys::V, IO::Modifiers::Shift);
    window->key_down(Keys::V, IO::Modifiers::Ctrl | IO::Modifiers::Shift);
    EXPECT_EQ(calls, 0);

    window->key_down(Keys::V, IO::Modifiers::Ctrl);
    EXPECT_EQ(calls, 1);
}

TEST_F(FormaContextTest, ABareOnPressAndAChordOnPressAreSeparateSlots)
{
    const uint32_t id = left_half();
    int bare = 0;
    int chord = 0;
    ctx->on_press(id, Keys::V, [&](uint32_t) { ++bare; });
    ctx->on_press(id, Keys::V | IO::Modifiers::Ctrl, [&](uint32_t) { ++chord; });

    focus(id, k_left);
    window->key_down(Keys::V);
    EXPECT_EQ(bare, 1);
    EXPECT_EQ(chord, 0);

    window->key_down(Keys::V, IO::Modifiers::Ctrl);
    EXPECT_EQ(bare, 2);
    EXPECT_EQ(chord, 1);
}

TEST_F(FormaContextTest, ExplicitNoneOnPressIgnoresAKeyHeldWithModifiers)
{
    const uint32_t id = left_half();
    int calls = 0;
    ctx->on_press(id, Keys::A | IO::Modifiers::None, [&](uint32_t) { ++calls; });

    focus(id, k_left);
    window->key_down(Keys::A, IO::Modifiers::Ctrl);
    EXPECT_EQ(calls, 0);

    window->key_down(Keys::A);
    EXPECT_EQ(calls, 1);
}

TEST_F(FormaContextTest, PressAndHoldWithModifiersFiresOnPressAndRepeatOnly)
{
    const uint32_t id = left_half();
    int calls = 0;
    ctx->press_and_hold(id, Keys::Left | IO::Modifiers::Shift, [&](uint32_t) { ++calls; });

    focus(id, k_left);
    window->key_down(Keys::Left);
    EXPECT_EQ(calls, 0);

    window->key_down(Keys::Left, IO::Modifiers::Shift);
    window->key_repeat(Keys::Left, IO::Modifiers::Shift);
    window->key_up(Keys::Left, IO::Modifiers::Shift);
    EXPECT_EQ(calls, 2);
}

TEST_F(FormaContextTest, ChordListenersRegisterOncePerChordNotPerElement)
{
    const uint32_t a = left_half();
    const uint32_t b = right_half();
    const size_t before = manager->get_all_events().size();

    ctx->on_press(a, Keys::A | IO::Modifiers::Ctrl, [](uint32_t) { });
    const size_t after_first = manager->get_all_events().size();
    ctx->on_press(b, Keys::A | IO::Modifiers::Ctrl, [](uint32_t) { });
    const size_t after_second = manager->get_all_events().size();
    ctx->on_press(a, Keys::A, [](uint32_t) { });

    EXPECT_EQ(after_first, before + 1);
    EXPECT_EQ(after_second, after_first);
    EXPECT_EQ(manager->get_all_events().size(), after_first + 1);
}

TEST_F(FormaContextTest, ChordHandlersBelongToTheirOwnElement)
{
    const uint32_t a = left_half();
    const uint32_t b = right_half();
    int from_a = 0;
    int from_b = 0;
    ctx->on_press(a, Keys::A | IO::Modifiers::Ctrl, [&](uint32_t) { ++from_a; });
    ctx->on_press(b, Keys::A | IO::Modifiers::Ctrl, [&](uint32_t) { ++from_b; });

    focus(a, k_left);
    window->key_down(Keys::A, IO::Modifiers::Ctrl);
    EXPECT_EQ(from_a, 1);
    EXPECT_EQ(from_b, 0);

    focus(b, k_right);
    window->key_down(Keys::A, IO::Modifiers::Ctrl);
    EXPECT_EQ(from_a, 1);
    EXPECT_EQ(from_b, 1);
}

TEST_F(FormaContextTest, UnbindRemovesChordHandlers)
{
    const uint32_t id = left_half();
    int calls = 0;
    ctx->on_press(id, MouseButtons::Left, [](uint32_t, glm::vec2) { });
    ctx->on_press(id, Keys::A | IO::Modifiers::Ctrl, [&](uint32_t) { ++calls; });
    ctx->on_held(id, Keys::B | IO::Modifiers::Shift, [&](uint32_t) { ++calls; });

    focus(id, k_left);
    window->key_down(Keys::A, IO::Modifiers::Ctrl);
    ASSERT_EQ(calls, 1);

    ctx->unbind(id);
    ctx->on_press(id, MouseButtons::Left, [](uint32_t, glm::vec2) { });
    focus(id, k_left);
    window->key_down(Keys::A, IO::Modifiers::Ctrl);
    window->key_down(Keys::B, IO::Modifiers::Shift);

    EXPECT_EQ(calls, 1);
}

TEST_F(FormaContextTest, DestroyingTheContextCancelsChordHandlers)
{
    const uint32_t id = whole();
    ctx->on_press(id, Keys::A | IO::Modifiers::Ctrl, [](uint32_t) { });
    ctx->on_held(id, Keys::B | IO::Modifiers::Shift, [](uint32_t) { });

    ctx.reset();

    EXPECT_TRUE(manager->get_all_events().empty());
}

TEST_F(FormaContextTest, ATextSubscriberReceivesTheCodepointWhileFocused)
{
    const uint32_t id = left_half();
    std::vector<uint32_t> typed;
    ctx->subscribe(id, filter_for(WindowEventType::TEXT_INPUT),
        [&](uint32_t, const Core::WindowEvent& event, glm::vec2) {
            if (const auto* data = std::get_if<Core::WindowEvent::TextData>(&event.data))
                typed.push_back(data->codepoint);
        });

    window->type(0x41);
    focus(id, k_left);
    window->type(0x42);

    EXPECT_EQ(typed, (std::vector<uint32_t> { 0x42 }));
}

TEST_F(FormaContextTest, FocusSubscribersRunBesideTheOwnFocusCallbacks)
{
    const uint32_t a = left_half();
    const uint32_t b = right_half();
    std::vector<std::string> log;
    ctx->on_focus_gained(a, [&](uint32_t) { log.emplace_back("own gained"); });
    ctx->subscribe(a, filter_for(WindowEventType::WINDOW_FOCUS_GAINED), log_into(log, "sub gained"));
    ctx->on_focus_lost(a, [&](uint32_t) { log.emplace_back("own lost"); });
    ctx->subscribe(a, filter_for(WindowEventType::WINDOW_FOCUS_LOST), log_into(log, "sub lost"));

    focus(a, k_left);
    focus(b, k_right);

    EXPECT_EQ(log, (std::vector<std::string> { "own gained", "sub gained", "own lost", "sub lost" }));
}

TEST_F(FormaContextTest, AFilterWithNoEventTypeReceivesEveryEventItsOtherFieldsAllow)
{
    const uint32_t id = left_half();
    int calls = 0;
    ctx->subscribe(id, Vruta::WindowEventFilter {}, count_into(calls));

    window->move_to_ndc(k_left);

    EXPECT_GE(calls, 2);
}

TEST_F(FormaContextTest, ReceivesAcceptsKeyEventsWithOrWithoutAKeyAndRefusesEventsAnElementNeverGets)
{
    using Forma::Context;

    EXPECT_TRUE(Context::receives(filter_for(WindowEventType::MOUSE_BUTTON_PRESSED)));
    EXPECT_TRUE(Context::receives(filter_for(WindowEventType::KEY_PRESSED, std::nullopt, Keys::A)));
    EXPECT_TRUE(Context::receives(Vruta::WindowEventFilter {}));
    EXPECT_TRUE(Context::receives(filter_for(WindowEventType::KEY_PRESSED)));
    EXPECT_TRUE(Context::receives(filter_for(WindowEventType::KEY_RELEASED)));
    EXPECT_TRUE(Context::receives(filter_for(WindowEventType::KEY_REPEAT)));
    EXPECT_FALSE(Context::receives(filter_for(WindowEventType::WINDOW_RESIZED)));
    EXPECT_FALSE(Context::receives(filter_for(WindowEventType::CUSTOM)));
}

TEST_F(FormaContextTest, SubscribeReturnsFalseAndSubscribesNothingForARefusedFilter)
{
    const uint32_t id = left_half();
    int calls = 0;

    EXPECT_FALSE(ctx->subscribe(id, filter_for(WindowEventType::WINDOW_RESIZED), count_into(calls)));
    EXPECT_FALSE(ctx->subscribe(id, filter_for(WindowEventType::MOUSE_ENTERED), nullptr));

    window->move_to_ndc(k_left);
    EXPECT_EQ(calls, 0);
}

TEST_F(FormaContextTest, AKeySubscriptionNamingNoKeyReceivesEveryKeyOfTheFocusedElement)
{
    const uint32_t id = left_half();
    std::vector<int> keys;
    Vruta::WindowEventFilter filter;
    filter.event_type = WindowEventType::KEY_PRESSED;
    ASSERT_TRUE(ctx->subscribe(id, filter, [&](uint32_t, const Core::WindowEvent& event, glm::vec2) {
        keys.push_back(std::get<Core::WindowEvent::KeyData>(event.data).key);
    }));

    window->key_down(Keys::A);
    EXPECT_TRUE(keys.empty());

    focus(id, k_left);
    window->key_down(Keys::A);
    window->key_down(Keys::B);

    ASSERT_EQ(keys.size(), 2U);
    EXPECT_EQ(keys.front(), static_cast<int>(Keys::A));
    EXPECT_EQ(keys.back(), static_cast<int>(Keys::B));
}

TEST_F(FormaContextTest, ANamedAndAnUnnamedKeySubscriptionEachReceiveAKeyOnce)
{
    const uint32_t id = left_half();
    int named = 0;
    int unnamed = 0;
    ASSERT_TRUE(ctx->subscribe(id, filter_for(WindowEventType::KEY_PRESSED, std::nullopt, Keys::A), count_into(named)));
    ASSERT_TRUE(ctx->subscribe(id, filter_for(WindowEventType::KEY_PRESSED), count_into(unnamed)));
    focus(id, k_left);

    window->key_down(Keys::A);
    EXPECT_EQ(named, 1);
    EXPECT_EQ(unnamed, 1);

    window->key_down(Keys::B);
    EXPECT_EQ(named, 1);
    EXPECT_EQ(unnamed, 2);
}

TEST_F(FormaContextTest, DestroyingTheContextCancelsAnUnnamedKeySubscription)
{
    const uint32_t id = left_half();
    int calls = 0;
    ASSERT_TRUE(ctx->subscribe(id, filter_for(WindowEventType::KEY_PRESSED), count_into(calls)));
    const std::string name = "ctx_any_key_" + std::to_string(static_cast<int>(WindowEventType::KEY_PRESSED));
    ASSERT_NE(manager->get_event(name), nullptr);

    ctx.reset();

    EXPECT_EQ(manager->get_event(name), nullptr);
}

TEST_F(FormaContextTest, APointerSubscriptionCanRequireModifiers)
{
    const uint32_t id = left_half();
    int with_ctrl = 0;
    Vruta::WindowEventFilter filter;
    filter.event_type = WindowEventType::MOUSE_BUTTON_PRESSED;
    filter.modifiers = IO::Modifiers::Ctrl;
    ASSERT_TRUE(ctx->subscribe(id, filter, count_into(with_ctrl)));

    window->move_to_ndc(k_left);
    window->press(MouseButtons::Left);
    EXPECT_EQ(with_ctrl, 0);

    window->press(MouseButtons::Left, IO::Modifiers::Ctrl);
    EXPECT_EQ(with_ctrl, 1);
}

}
