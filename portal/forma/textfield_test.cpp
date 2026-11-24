#include "gtest/gtest.h"
#include "../../support/test_metadata.hpp"

#include "../../support/scripted_window.hpp"
#include "../../support/text_test_support.hpp"

#include "MayaFlux/Portal/Forma/Context.hpp"
#include "MayaFlux/Portal/Forma/Primitives/Entry.hpp"
#include "MayaFlux/Portal/Forma/Primitives/Scrollable.hpp"
#include "MayaFlux/Portal/Forma/Primitives/TextField.hpp"
#include "MayaFlux/Portal/Forma/Surface.hpp"
#include "MayaFlux/Vruta/EventManager.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::system_font>() }))

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

#ifdef MAYAFLUX_PLATFORM_MACOS
    constexpr IO::Modifiers k_shortcut = IO::Modifiers::Super;
#else
    constexpr IO::Modifiers k_shortcut = IO::Modifiers::Ctrl;
#endif

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

    std::shared_ptr<Vruta::EventManager> manager = std::make_shared<Vruta::EventManager>();
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

    void shift_tap(Keys key)
    {
        window->key_down(key, IO::Modifiers::Shift);
        window->key_up(key, IO::Modifiers::Shift);
    }

    void chord(Keys key, IO::Modifiers mods = k_shortcut)
    {
        window->key_down(key, mods);
        window->key_up(key, mods);
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

TEST_F(FormaTextFieldTest, EditingKeysStillActWhileAModifierIsHeld)
{
    auto field = focused_field("hello");

    window->key_down(Keys::Backspace, IO::Modifiers::Shift);
    window->key_up(Keys::Backspace, IO::Modifiers::Shift);

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

// =============================================================================
// Single line, commit, cancel and change
// =============================================================================

TEST_F(FormaTextFieldTest, EnterCommitsTheTextOfASingleLineFieldAndInsertsNothing)
{
    auto field = focused_field("abc");
    std::vector<std::string> committed;
    field.single_line().on_commit([&](const std::string& text) { committed.push_back(text); });

    tap(Keys::Enter);

    EXPECT_EQ(committed, std::vector<std::string> { "abc" });
    EXPECT_EQ(field.state->value.text, "abc");
}

TEST_F(FormaTextFieldTest, EnterInAMultiLineFieldStillInsertsALineBreakAndCommitsNothing)
{
    auto field = focused_field("ab");
    int commits = 0;
    field.on_commit([&](const std::string&) { ++commits; });

    tap(Keys::Enter);

    EXPECT_EQ(field.state->value.text, "ab\n");
    EXPECT_EQ(commits, 0);
}

TEST_F(FormaTextFieldTest, HoldingEnterInASingleLineFieldCommitsOnce)
{
    auto field = focused_field("abc");
    int commits = 0;
    field.single_line().on_commit([&](const std::string&) { ++commits; });

    window->key_down(Keys::Enter);
    window->key_repeat(Keys::Enter);
    window->key_repeat(Keys::Enter);
    window->key_up(Keys::Enter);

    EXPECT_EQ(commits, 1);
}

TEST_F(FormaTextFieldTest, EscapeCancelsInEitherMode)
{
    auto field = focused_field("abc");
    int cancels = 0;
    field.on_cancel([&] { ++cancels; });

    tap(Keys::Escape);
    field.single_line();
    tap(Keys::Escape);

    EXPECT_EQ(cancels, 2);
    EXPECT_EQ(field.state->value.text, "abc");
}

TEST_F(FormaTextFieldTest, ChangeReportsEveryEditWithTheStateAlreadyUpdated)
{
    auto field = focused_field("");
    std::vector<std::string> seen;
    std::vector<bool> state_matched;
    field.on_change([&](const std::string& text) {
        seen.push_back(text);
        state_matched.push_back(field.state->value.text == text);
    });

    window->type('x');
    window->type('y');
    tap(Keys::Backspace);

    EXPECT_EQ(seen, (std::vector<std::string> { "x", "xy", "x" }));
    EXPECT_EQ(state_matched, (std::vector<bool> { true, true, true }));
}

TEST_F(FormaTextFieldTest, ChangeIsNotReportedForMovesOrACommit)
{
    auto field = focused_field("abc");
    int changes = 0;
    field.single_line().on_change([&](const std::string&) { ++changes; });
    field.on_commit([](const std::string&) { });

    tap(Keys::Left);
    shift_tap(Keys::Home);
    tap(Keys::Enter);

    EXPECT_EQ(changes, 0);
}

TEST_F(FormaTextFieldTest, ChangeMayRecolourTheSharedParametersForTheSameDraw)
{
    auto field = focused_field("");
    field.on_change([&](const std::string&) { params->color = { 1.F, 0.F, 0.F, 1.F }; });

    window->type('x');

    EXPECT_EQ(params->color, glm::vec4(1.F, 0.F, 0.F, 1.F));
}

TEST_F(FormaTextFieldTest, TabDoesNothingInASingleLineField)
{
    auto field = focused_field("ab");
    field.single_line();

    tap(Keys::Tab);

    EXPECT_EQ(field.state->value.text, "ab");
}

TEST_F(FormaTextFieldTest, UpAndDownJumpToTheStartAndEndOfASingleLineField)
{
    auto field = focused_field("hello");
    field.single_line();

    tap(Keys::Up);
    EXPECT_EQ(field.state->value.cursor, 0U);

    tap(Keys::Down);
    EXPECT_EQ(field.state->value.cursor, 5U);

    shift_tap(Keys::Up);
    const auto range = Portal::Text::selection(field.state->value);
    ASSERT_TRUE(range.has_value());
    EXPECT_EQ(range->first, 0U);
    EXPECT_EQ(range->second, 5U);
}

TEST_F(FormaTextFieldTest, PasteDropsLineBreaksInASingleLineField)
{
    auto field = focused_field("");
    field.single_line();
    window->set_clipboard(std::string("a\r\nb"));

    chord(Keys::V);
    window->poll();

    EXPECT_EQ(field.state->value.text, "ab");
}

TEST_F(FormaTextFieldTest, BehaviourIsSharedByCopiesOfAField)
{
    auto field = focused_field("abc");
    Forma::TextField copy = field;
    int commits = 0;

    copy.single_line().on_commit([&](const std::string&) { ++commits; });
    tap(Keys::Enter);

    EXPECT_EQ(commits, 1);
}

// =============================================================================
// Home and End
// =============================================================================

TEST_F(FormaTextFieldTest, HomeAndEndMoveToTheStartAndEndOfTheCursorsLine)
{
    auto field = focused_field("ab\ncd");

    const auto put_cursor = [&](size_t index) {
        Portal::Text::EditableText edit = field.state->value;
        Portal::Text::move_to(edit, index, false);
        field.state->write(edit);
    };

    put_cursor(4);
    tap(Keys::Home);
    EXPECT_EQ(field.state->value.cursor, 3U);

    tap(Keys::End);
    EXPECT_EQ(field.state->value.cursor, 5U);

    put_cursor(2);
    tap(Keys::End);
    EXPECT_EQ(field.state->value.cursor, 2U);

    tap(Keys::Home);
    EXPECT_EQ(field.state->value.cursor, 0U);
}

TEST_F(FormaTextFieldTest, HomeAndEndWithShiftExtendTheSelection)
{
    auto field = focused_field("hello");

    shift_tap(Keys::Home);
    const auto range = Portal::Text::selection(field.state->value);

    ASSERT_TRUE(range.has_value());
    EXPECT_EQ(range->first, 0U);
    EXPECT_EQ(range->second, 5U);

    tap(Keys::End);
    EXPECT_FALSE(Portal::Text::selection(field.state->value).has_value());
    EXPECT_EQ(field.state->value.cursor, 5U);
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

TEST_F(FormaTextFieldTest, UpAndDownMoveThroughAnEmptyLine)
{
    params->render_bounds.y = atlas->line_height() * 4;
    auto field = make_field("a\n\nb");
    click({ k_field.min.x + 0.01F, k_field.max.y - k_field.height() * 2.5F / 4.F });
    ASSERT_EQ(field.state->value.cursor, 3U);

    tap(Keys::Up);
    EXPECT_EQ(field.state->value.cursor, 2U);
    tap(Keys::Up);
    EXPECT_EQ(field.state->value.cursor, 0U);

    tap(Keys::Down);
    EXPECT_EQ(field.state->value.cursor, 2U);
    tap(Keys::Down);
    EXPECT_EQ(field.state->value.cursor, 3U);
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

TEST_F(FormaTextFieldTest, ClickingAnEmptyLinePlacesTheCursorThere)
{
    params->render_bounds.y = atlas->line_height() * 4;
    auto field = make_field("a\n\nb");

    click({ k_field.min.x + 0.01F, k_field.max.y - k_field.height() * 1.5F / 4.F });
    ASSERT_EQ(field.state->value.cursor, 2U);

    window->type('x');
    EXPECT_EQ(field.state->value.text, "a\nx\nb");
}

// =============================================================================
// Selection
// =============================================================================

TEST_F(FormaTextFieldTest, ShiftArrowExtendsTheSelectionFromTheCaret)
{
    auto field = focused_field("hello");

    shift_tap(Keys::Left);
    shift_tap(Keys::Left);

    EXPECT_EQ(field.state->value.anchor, 5U);
    EXPECT_EQ(field.state->value.cursor, 3U);
    EXPECT_EQ(Portal::Text::selected_text(field.state->value), "lo");
    EXPECT_EQ(field.state->value.text, "hello");
}

TEST_F(FormaTextFieldTest, ShiftRightGrowsTheSelectionTowardTheEnd)
{
    auto field = focused_field("hello");
    click(k_far_left_top);

    shift_tap(Keys::Right);
    shift_tap(Keys::Right);

    EXPECT_EQ(Portal::Text::selected_text(field.state->value), "he");
}

TEST_F(FormaTextFieldTest, AnArrowWithoutShiftCollapsesTheSelection)
{
    auto field = focused_field("hello");
    shift_tap(Keys::Left);
    shift_tap(Keys::Left);

    tap(Keys::Left);

    EXPECT_EQ(field.state->value.cursor, 3U);
    EXPECT_FALSE(Portal::Text::selection(field.state->value).has_value());
}

TEST_F(FormaTextFieldTest, ShiftClickExtendsTheSelectionToTheClickedPosition)
{
    auto field = focused_field("hello world");
    ASSERT_EQ(field.state->value.cursor, 11U);

    window->move_to_ndc(k_far_left_top);
    window->press(MouseButtons::Left, IO::Modifiers::Shift);
    window->release(MouseButtons::Left, IO::Modifiers::Shift);

    EXPECT_EQ(field.state->value.anchor, 11U);
    EXPECT_EQ(field.state->value.cursor, 0U);
    EXPECT_EQ(Portal::Text::selected_text(field.state->value), "hello world");
}

TEST_F(FormaTextFieldTest, DraggingWithTheLeftButtonSelectsFromThePressToThePointer)
{
    auto field = make_field("hello world");

    window->move_to_ndc(k_far_left_top);
    window->press(MouseButtons::Left);
    window->move_to_ndc(k_far_right_top);
    window->release(MouseButtons::Left);

    EXPECT_EQ(field.state->value.anchor, 0U);
    EXPECT_EQ(field.state->value.cursor, 11U);
    EXPECT_EQ(Portal::Text::selected_text(field.state->value), "hello world");
}

TEST_F(FormaTextFieldTest, DraggingBackTowardThePressShrinksTheSelection)
{
    auto field = make_field("hello world");

    window->move_to_ndc(k_far_left_top);
    window->press(MouseButtons::Left);
    window->move_to_ndc(k_far_right_top);
    window->move_to_ndc(k_far_left_top);
    window->release(MouseButtons::Left);

    EXPECT_FALSE(Portal::Text::selection(field.state->value).has_value());
}

TEST_F(FormaTextFieldTest, ANewPressAfterADragDropsTheSelection)
{
    auto field = make_field("hello world");
    window->move_to_ndc(k_far_left_top);
    window->press(MouseButtons::Left);
    window->move_to_ndc(k_far_right_top);
    window->release(MouseButtons::Left);
    ASSERT_TRUE(Portal::Text::selection(field.state->value).has_value());

    click(k_far_left_top);

    EXPECT_FALSE(Portal::Text::selection(field.state->value).has_value());
}

TEST_F(FormaTextFieldTest, APlainClickDropsTheSelection)
{
    auto field = focused_field("hello");
    shift_tap(Keys::Left);
    ASSERT_TRUE(Portal::Text::selection(field.state->value).has_value());

    click(k_far_left_top);

    EXPECT_FALSE(Portal::Text::selection(field.state->value).has_value());
    EXPECT_EQ(field.state->value.cursor, 0U);
}

TEST_F(FormaTextFieldTest, TypingReplacesTheSelection)
{
    auto field = focused_field("hello");
    shift_tap(Keys::Left);
    shift_tap(Keys::Left);

    window->type('p');

    EXPECT_EQ(field.state->value.text, "help");
    EXPECT_EQ(field.state->value.cursor, 4U);
    EXPECT_FALSE(Portal::Text::selection(field.state->value).has_value());
}

TEST_F(FormaTextFieldTest, BackspaceAndDeleteRemoveTheSelectionAndNothingElse)
{
    auto field = focused_field("hello");
    shift_tap(Keys::Left);
    shift_tap(Keys::Left);
    tap(Keys::Backspace);
    EXPECT_EQ(field.state->value.text, "hel");

    shift_tap(Keys::Left);
    tap(Keys::Delete);
    EXPECT_EQ(field.state->value.text, "he");
}

TEST_F(FormaTextFieldTest, EnterAndTabReplaceTheSelection)
{
    auto field = focused_field("hello");
    shift_tap(Keys::Left);
    shift_tap(Keys::Left);
    tap(Keys::Enter);
    EXPECT_EQ(field.state->value.text, "hel\n");

    shift_tap(Keys::Left);
    tap(Keys::Tab);
    EXPECT_EQ(field.state->value.text, "hel\t");
}

TEST_F(FormaTextFieldTest, ShiftUpAndDownExtendAcrossLines)
{
    params->render_bounds.y = atlas->line_height() * 3;
    auto field = focused_field("ab\ncd");
    while (field.state->value.cursor < 5U)
        tap(Keys::Right);
    ASSERT_EQ(field.state->value.cursor, 5U);

    shift_tap(Keys::Up);

    EXPECT_EQ(field.state->value.anchor, 5U);
    EXPECT_LT(field.state->value.cursor, 3U);
    EXPECT_TRUE(Portal::Text::selection(field.state->value).has_value());

    shift_tap(Keys::Down);
    EXPECT_GE(field.state->value.cursor, 3U);
    EXPECT_EQ(field.state->value.anchor, 5U);
}

TEST_F(FormaTextFieldTest, SelectAllSelectsTheWholeTextWithTheCursorAtTheEnd)
{
    auto field = focused_field("hello");
    click(k_far_left_top);

    chord(Keys::A);

    EXPECT_EQ(field.state->value.anchor, 0U);
    EXPECT_EQ(field.state->value.cursor, 5U);
    EXPECT_EQ(Portal::Text::selected_text(field.state->value), "hello");
}

// =============================================================================
// Clipboard
// =============================================================================

TEST_F(FormaTextFieldTest, CopyPutsTheSelectionOnTheClipboardAndLeavesTheText)
{
    auto field = focused_field("hello world");
    shift_tap(Keys::Left);
    shift_tap(Keys::Left);
    shift_tap(Keys::Left);

    chord(Keys::C);

    ASSERT_TRUE(window->clipboard().has_value());
    EXPECT_EQ(*window->clipboard(), "rld");
    EXPECT_EQ(field.state->value.text, "hello world");
    EXPECT_EQ(Portal::Text::selected_text(field.state->value), "rld");
}

TEST_F(FormaTextFieldTest, CopyWithNothingSelectedLeavesTheClipboardAlone)
{
    auto field = focused_field("hello");
    window->set_clipboard("keep");

    chord(Keys::C);

    EXPECT_EQ(window->clipboard(), "keep");
}

TEST_F(FormaTextFieldTest, CutMovesTheSelectionToTheClipboard)
{
    auto field = focused_field("hello world");
    shift_tap(Keys::Left);
    shift_tap(Keys::Left);
    shift_tap(Keys::Left);

    chord(Keys::X);

    EXPECT_EQ(window->clipboard(), "rld");
    EXPECT_EQ(field.state->value.text, "hello wo");
    EXPECT_EQ(field.state->value.cursor, 8U);
    EXPECT_FALSE(Portal::Text::selection(field.state->value).has_value());
}

TEST_F(FormaTextFieldTest, CutWithNothingSelectedChangesNothing)
{
    auto field = focused_field("hello");
    window->set_clipboard("keep");

    chord(Keys::X);

    EXPECT_EQ(window->clipboard(), "keep");
    EXPECT_EQ(field.state->value.text, "hello");
}

TEST_F(FormaTextFieldTest, PasteInsertsTheClipboardAtTheCursorOnceThePollDeliversIt)
{
    auto field = focused_field("hello");
    window->set_clipboard(" there");

    chord(Keys::V);
    EXPECT_EQ(field.state->value.text, "hello");

    window->poll();
    EXPECT_EQ(field.state->value.text, "hello there");
    EXPECT_EQ(field.state->value.cursor, 11U);
}

TEST_F(FormaTextFieldTest, PasteReplacesTheSelection)
{
    auto field = focused_field("hello world");
    shift_tap(Keys::Left);
    shift_tap(Keys::Left);
    shift_tap(Keys::Left);
    shift_tap(Keys::Left);
    shift_tap(Keys::Left);
    window->set_clipboard("moon");

    chord(Keys::V);
    window->poll();

    EXPECT_EQ(field.state->value.text, "hello moon");
    EXPECT_FALSE(Portal::Text::selection(field.state->value).has_value());
}

TEST_F(FormaTextFieldTest, PasteNormalisesLineEndingsAndDropsControlBytes)
{
    auto field = focused_field("");
    window->set_clipboard(std::string("a\r\nb\x01\tc"));

    chord(Keys::V);
    window->poll();

    EXPECT_EQ(field.state->value.text, "a\nb\tc");
}

TEST_F(FormaTextFieldTest, PasteWithAnEmptyClipboardChangesNothing)
{
    auto field = focused_field("hello");
    shift_tap(Keys::Left);

    chord(Keys::V);
    window->poll();

    EXPECT_EQ(field.state->value.text, "hello");
    EXPECT_TRUE(Portal::Text::selection(field.state->value).has_value());
}

TEST_F(FormaTextFieldTest, ACopyThenAPasteRoundTripsThroughTheClipboard)
{
    auto field = focused_field("abc");
    chord(Keys::A);
    chord(Keys::C);
    tap(Keys::Right);

    chord(Keys::V);
    window->poll();

    EXPECT_EQ(field.state->value.text, "abcabc");
}

TEST_F(FormaTextFieldTest, ShortcutsNeedExactlyTheShortcutModifier)
{
    auto field = focused_field("hello");
    window->set_clipboard("X");

    tap(Keys::V);
    chord(Keys::V, k_shortcut | IO::Modifiers::Shift);
    chord(Keys::V, IO::Modifiers::Alt);
    window->poll();

    EXPECT_EQ(field.state->value.text, "hello");
}

TEST_F(FormaTextFieldTest, ShortcutsDoNothingWithoutFocus)
{
    auto field = make_field("hello");
    window->set_clipboard("X");

    chord(Keys::V);
    window->poll();

    EXPECT_EQ(field.state->value.text, "hello");
}

TEST_F(FormaTextFieldTest, ShortcutsInsertNoTextOfTheirOwn)
{
    auto field = focused_field("hello");
    chord(Keys::A);
    window->type(0x01);

    EXPECT_EQ(field.state->value.text, "hello");
    EXPECT_TRUE(Portal::Text::selection(field.state->value).has_value());
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
