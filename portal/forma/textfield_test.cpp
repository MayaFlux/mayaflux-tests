#include "../../test_config.h"

#include "../../support/scripted_window.hpp"
#include "../../support/text_test_support.hpp"

#include "MayaFlux/Portal/Forma/Context.hpp"
#include "MayaFlux/Portal/Forma/Primitives/Entry.hpp"
#include "MayaFlux/Portal/Forma/Primitives/Scrollable.hpp"
#include "MayaFlux/Portal/Forma/Primitives/TextField.hpp"
#include "MayaFlux/Portal/Forma/Surface.hpp"
#include "MayaFlux/Vruta/EventManager.hpp"

namespace MayaFlux::Test {

namespace Forma = Portal::Forma;
using IO::Keys;
using IO::MouseButtons;
using Kinesis::AABB2D;
using Portal::Graphics::PrimitiveTopology;

namespace {

    constexpr uint32_t k_pixel_size = 24;
    const AABB2D k_field { .min = { -0.9F, 0.1F }, .max = { 0.9F, 0.9F } };
    const glm::vec2 k_centre { 0.F, 0.5F };
    const glm::vec2 k_far_left_top { -0.89F, 0.85F };
    const glm::vec2 k_far_right_top { 0.89F, 0.85F };

    std::shared_ptr<Buffers::FormaBuffer> make_buffer()
    {
        return std::make_shared<Buffers::FormaBuffer>(4096, PrimitiveTopology::TRIANGLE_LIST);
    }

}

class FormaTextFieldTest : public SystemFontTest {
protected:
    void SetUp() override
    {
        SystemFontTest::SetUp();
        if (HasFatalFailure())
            return;

        auto& foundry = Portal::Text::TypeFaceFoundry::instance();
        ASSERT_TRUE(foundry.set_default_font(font_path, k_pixel_size, 512));
        atlas = foundry.get_default_glyph_atlas();
        ASSERT_NE(atlas, nullptr);

        params = std::make_shared<Portal::Text::PressParams>();
        params->atlas = atlas;
        params->render_bounds = { 320, 96 };
    }

    Vruta::EventManager manager;
    std::shared_ptr<ScriptedWindow> window = std::make_shared<ScriptedWindow>(200U, 100U);
    std::shared_ptr<Forma::Layer> layer = std::make_shared<Forma::Layer>();
    std::shared_ptr<Forma::Context> ctx = std::make_shared<Forma::Context>(layer, window, manager, "field");
    Forma::Surface surface { Forma::SurfaceConfig { .window = window, .layer = layer, .ctx = ctx, .detach_on_close = false } };
    Portal::Text::GlyphAtlas* atlas { nullptr };
    std::shared_ptr<Portal::Text::PressParams> params;

    Forma::TextField make_field(std::string text)
    {
        Forma::TextField field;
        field.place(make_buffer(), surface, k_field, params, std::move(text));
        return field;
    }

    void click(glm::vec2 ndc)
    {
        window->move_to_ndc(ndc);
        window->press(MouseButtons::Left);
        window->release(MouseButtons::Left);
    }

    Forma::TextField focused_field(std::string text)
    {
        Forma::TextField field = make_field(std::move(text));
        click(k_far_right_top);
        return field;
    }

    void tap(Keys key)
    {
        window->key_down(key);
        window->key_up(key);
    }
};

// =============================================================================
// Placement
// =============================================================================

TEST_F(FormaTextFieldTest, PlaceStartsWithTheInitialTextAndTheCursorAtItsEnd)
{
    auto field = make_field("hello");

    ASSERT_NE(field.state, nullptr);
    EXPECT_EQ(field.state->value.text, "hello");
    EXPECT_EQ(field.state->value.cursor, 5U);
    EXPECT_NE(field.element_id, 0U);
    ASSERT_NE(layer->get(field.element_id), nullptr);
    EXPECT_EQ(layer->get(field.element_id)->bounds_hint->min, k_field.min);
    EXPECT_EQ(field.bounds().max, k_field.max);
}

TEST_F(FormaTextFieldTest, AnEmptyFieldStartsEmptyWithTheCursorAtZero)
{
    auto field = make_field("");

    EXPECT_TRUE(field.state->value.text.empty());
    EXPECT_EQ(field.state->value.cursor, 0U);
}

TEST_F(FormaTextFieldTest, NullParametersFallBackToDefaults)
{
    Forma::TextField field;

    field.place(make_buffer(), surface, k_field, nullptr, "x");

    EXPECT_NE(field.params, nullptr);
}

// =============================================================================
// Typing
// =============================================================================

TEST_F(FormaTextFieldTest, TypingNeedsFocus)
{
    auto field = make_field("hi");

    window->type('a');

    EXPECT_EQ(field.state->value.text, "hi");
}

TEST_F(FormaTextFieldTest, ClickingFocusesTheField)
{
    auto field = make_field("hi");

    click(k_centre);

    EXPECT_EQ(ctx->focused(), field.element_id);
}

TEST_F(FormaTextFieldTest, TypingInsertsAtTheCursorAndAdvancesIt)
{
    auto field = focused_field("hello");

    window->type('!');
    window->type('?');

    EXPECT_EQ(field.state->value.text, "hello!?");
    EXPECT_EQ(field.state->value.cursor, 7U);
}

TEST_F(FormaTextFieldTest, TypingMidTextInsertsBeforeTheCursor)
{
    auto field = focused_field("hello");
    tap(Keys::Left);
    tap(Keys::Left);

    window->type('X');

    EXPECT_EQ(field.state->value.text, "helXlo");
    EXPECT_EQ(field.state->value.cursor, 4U);
}

TEST_F(FormaTextFieldTest, MultibyteCodepointsAreInsertedWhole)
{
    auto field = focused_field("a");

    window->type(0x00E9);
    window->type(0x20AC);

    EXPECT_EQ(field.state->value.text, "a\xC3\xA9\xE2\x82\xAC");
    EXPECT_EQ(field.state->value.cursor, field.state->value.text.size());
}

TEST_F(FormaTextFieldTest, ControlCodepointsAreNotInserted)
{
    auto field = focused_field("ok");

    window->type(0x07);

    EXPECT_EQ(field.state->value.text, "ok");
}

// =============================================================================
// Editing keys
// =============================================================================

TEST_F(FormaTextFieldTest, BackspaceRemovesTheCodepointBeforeTheCursor)
{
    auto field = focused_field("hello");

    tap(Keys::Backspace);

    EXPECT_EQ(field.state->value.text, "hell");
    EXPECT_EQ(field.state->value.cursor, 4U);
}

TEST_F(FormaTextFieldTest, BackspaceRemovesAWholeMultibyteCodepoint)
{
    auto field = focused_field("a\xC3\xA9");

    tap(Keys::Backspace);

    EXPECT_EQ(field.state->value.text, "a");
    EXPECT_EQ(field.state->value.cursor, 1U);
}

TEST_F(FormaTextFieldTest, DeleteRemovesTheCodepointAfterTheCursor)
{
    auto field = focused_field("hello");
    tap(Keys::Left);

    tap(Keys::Delete);

    EXPECT_EQ(field.state->value.text, "hell");
    EXPECT_EQ(field.state->value.cursor, 4U);
}

TEST_F(FormaTextFieldTest, BackspaceAtTheStartAndDeleteAtTheEndChangeNothing)
{
    auto at_end = focused_field("ab");
    tap(Keys::Delete);
    EXPECT_EQ(at_end.state->value.text, "ab");

    tap(Keys::Left);
    tap(Keys::Left);
    tap(Keys::Backspace);
    EXPECT_EQ(at_end.state->value.text, "ab");
    EXPECT_EQ(at_end.state->value.cursor, 0U);
}

TEST_F(FormaTextFieldTest, TabAndEnterInsertLiteralCharacters)
{
    auto field = focused_field("");

    tap(Keys::Tab);
    tap(Keys::Enter);

    EXPECT_EQ(field.state->value.text, "\t\n");
    EXPECT_EQ(field.state->value.cursor, 2U);
}

TEST_F(FormaTextFieldTest, EachEditPublishesANewStateVersion)
{
    auto field = focused_field("ab");
    const auto before = field.state->version;

    window->type('c');
    const auto after_type = field.state->version;
    tap(Keys::Backspace);

    EXPECT_GT(after_type, before);
    EXPECT_GT(field.state->version, after_type);
}

// =============================================================================
// Cursor movement
// =============================================================================

TEST_F(FormaTextFieldTest, ArrowsMoveByCodepointBoundariesAndStopAtTheEnds)
{
    auto field = focused_field("a\xC3\xA9"
                               "b");
    ASSERT_EQ(field.state->value.cursor, 4U);

    tap(Keys::Right);
    EXPECT_EQ(field.state->value.cursor, 4U);

    tap(Keys::Left);
    EXPECT_EQ(field.state->value.cursor, 3U);
    tap(Keys::Left);
    EXPECT_EQ(field.state->value.cursor, 1U);
    tap(Keys::Left);
    EXPECT_EQ(field.state->value.cursor, 0U);
    tap(Keys::Left);
    EXPECT_EQ(field.state->value.cursor, 0U);

    tap(Keys::Right);
    EXPECT_EQ(field.state->value.cursor, 1U);
}

TEST_F(FormaTextFieldTest, ArrowsLeaveTheTextUntouched)
{
    auto field = focused_field("abc");

    tap(Keys::Left);
    tap(Keys::Right);
    tap(Keys::Up);
    tap(Keys::Down);

    EXPECT_EQ(field.state->value.text, "abc");
}

TEST_F(FormaTextFieldTest, UpAndDownMoveBetweenLines)
{
    auto field = make_field("ab\ncd");
    click(k_far_right_top);
    ASSERT_LE(field.state->value.cursor, 3U);

    tap(Keys::Down);
    EXPECT_GE(field.state->value.cursor, 3U);

    tap(Keys::Up);
    EXPECT_LE(field.state->value.cursor, 2U);
}

TEST_F(FormaTextFieldTest, UpOnTheFirstLineAndDownOnTheLastStayOnThatLine)
{
    auto field = focused_field("one line");

    tap(Keys::Up);
    EXPECT_LE(field.state->value.cursor, 8U);
    tap(Keys::Down);
    EXPECT_EQ(field.state->value.text, "one line");
}

// =============================================================================
// Click positioning
// =============================================================================

TEST_F(FormaTextFieldTest, ClickingTheLeftEdgePutsTheCursorAtTheStart)
{
    auto field = make_field("hello world");

    click(k_far_left_top);

    EXPECT_EQ(field.state->value.cursor, 0U);
}

TEST_F(FormaTextFieldTest, ClickingPastTheEndOfTheTextPutsTheCursorAtTheEnd)
{
    auto field = make_field("hello world");
    click(k_far_left_top);
    ASSERT_EQ(field.state->value.cursor, 0U);

    click(k_far_right_top);

    EXPECT_EQ(field.state->value.cursor, 11U);
}

TEST_F(FormaTextFieldTest, ClickingThenTypingEditsAtTheClickedPosition)
{
    auto field = make_field("hello");
    click(k_far_left_top);

    window->type('>');

    EXPECT_EQ(field.state->value.text, ">hello");
}

TEST_F(FormaTextFieldTest, RepositionUpdatesTheReportedBounds)
{
    auto field = make_field("x");
    const AABB2D moved { .min = { -0.5F, -0.5F }, .max = { 0.5F, 0.F } };

    field.reposition(moved);

    EXPECT_EQ(field.bounds().min, moved.min);
    EXPECT_EQ(field.bounds().max, moved.max);
}

// =============================================================================
// Inside a Scrollable
// =============================================================================

class FormaTextFieldScrollTest : public FormaTextFieldTest {
protected:
    Forma::Scrollable make_panel()
    {
        return Forma::make_scrollable(make_buffer(), surface, k_field);
    }
};

TEST_F(FormaTextFieldScrollTest, TheFieldIsTrackedAndClippedToTheViewport)
{
    auto panel = make_panel();
    Forma::TextField field;

    field.scrollable(make_buffer(), surface, panel, params, "scroll me", 3.F);

    ASSERT_NE(field.element_id, 0U);
    ASSERT_TRUE(layer->get(field.element_id)->clip.has_value());
    EXPECT_EQ(layer->get(field.element_id)->clip->min, k_field.min);
    EXPECT_EQ(layer->get(field.element_id)->clip->max, k_field.max);
    EXPECT_NEAR(panel.scroll->content_bounds.height(), 3.F * k_field.height(), 1e-5F);
}

TEST_F(FormaTextFieldScrollTest, TheTallFieldCannotBeHitOutsideTheViewport)
{
    auto panel = make_panel();
    Forma::TextField field;
    field.scrollable(make_buffer(), surface, panel, params, "scroll me", 3.F);

    EXPECT_EQ(layer->hit_test(k_centre), field.element_id);
    EXPECT_FALSE(layer->hit_test({ 0.F, -0.5F }).has_value());
}

TEST_F(FormaTextFieldScrollTest, WheelOverTheFieldScrollsThePanel)
{
    auto panel = make_panel();
    Forma::TextField field;
    field.scrollable(make_buffer(), surface, panel, params, "scroll me", 3.F);

    window->move_to_ndc(k_centre);
    window->scroll(0.0, 1.0);

    EXPECT_NEAR(panel.scroll->offset.y, 0.05F, 1e-6F);
}

TEST_F(FormaTextFieldScrollTest, FieldAndViewportAreOneGroupInBothDirections)
{
    auto panel = make_panel();
    Forma::TextField field;
    field.scrollable(make_buffer(), surface, panel, params, "scroll me", 3.F);

    const auto from_field = layer->related_ids(field.element_id);
    const auto from_viewport = layer->related_ids(panel.viewport_id);

    EXPECT_EQ(from_field, std::vector<uint32_t> { panel.viewport_id });
    EXPECT_EQ(from_viewport, std::vector<uint32_t> { field.element_id });
}

TEST_F(FormaTextFieldScrollTest, HidingOrRemovingTheFieldTakesThePanelWithIt)
{
    auto panel = make_panel();
    Forma::TextField field;
    field.scrollable(make_buffer(), surface, panel, params, "scroll me", 3.F);

    ASSERT_TRUE(layer->set_visible(field.element_id, false));
    EXPECT_FALSE(layer->get(panel.viewport_id)->visible);

    ASSERT_TRUE(layer->remove(field.element_id));
    EXPECT_TRUE(layer->empty());
}

TEST_F(FormaTextFieldScrollTest, TypingStillEditsTheFieldAfterScrolling)
{
    auto panel = make_panel();
    Forma::TextField field;
    field.scrollable(make_buffer(), surface, panel, params, "ab", 3.F);
    window->move_to_ndc(k_centre);
    window->scroll(0.0, 1.0);
    click(k_far_right_top);

    window->type('c');

    EXPECT_NE(field.state->value.text.find('c'), std::string::npos);
}

// =============================================================================
// row_pixel_dims
// =============================================================================

class FormaRowPixelDimsTest : public SystemFontTest {
protected:
    void SetUp() override
    {
        SystemFontTest::SetUp();
        if (HasFatalFailure())
            return;

        auto& foundry = Portal::Text::TypeFaceFoundry::instance();
        ASSERT_TRUE(foundry.set_default_font(font_path, k_pixel_size, 512));
    }

    std::shared_ptr<ScriptedWindow> window = std::make_shared<ScriptedWindow>(200U, 100U);
};

TEST_F(FormaRowPixelDimsTest, WidthAndHeightScaleWithTheWindowAndTheNdcExtent)
{
    const glm::uvec2 dims = Forma::row_pixel_dims(window, -1.F, 1.F, 0.5F);

    EXPECT_EQ(dims.x, 200U);
    EXPECT_EQ(dims.y, 25U);
}

TEST_F(FormaRowPixelDimsTest, TheHeightNeverDropsBelowTheGlyphPixelSize)
{
    const glm::uvec2 dims = Forma::row_pixel_dims(window, -1.F, 1.F, 0.02F);

    EXPECT_EQ(dims.y, k_pixel_size);
}

TEST_F(FormaRowPixelDimsTest, AZeroWidthRowStillHasOnePixel)
{
    const glm::uvec2 dims = Forma::row_pixel_dims(window, 0.3F, 0.3F, 0.5F);

    EXPECT_EQ(dims.x, 1U);
}

TEST_F(FormaRowPixelDimsTest, ResizingTheWindowScalesTheDimensions)
{
    window->resize(400, 200);

    const glm::uvec2 dims = Forma::row_pixel_dims(window, -1.F, 1.F, 0.5F);

    EXPECT_EQ(dims.x, 400U);
    EXPECT_EQ(dims.y, 50U);
}

}
