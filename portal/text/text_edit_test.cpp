#include "gtest/gtest.h"
#include "../../support/test_metadata.hpp"

#include <string_view>

#include "MayaFlux/Portal/Text/TextEdit.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Portal::Text::EditableText;

    TEST(TextEditInsertTest, CodepointIsInsertedAtTheCursorAndAdvancesItByEncodedLength)
    {
        EditableText s { "ac", 1 };
        Portal::Text::insert_codepoint(s, 'b');
        EXPECT_EQ(s.text, "abc");
        EXPECT_EQ(s.cursor, 2U);

        Portal::Text::insert_codepoint(s, 0x20AC);
        EXPECT_EQ(s.text, "ab\xE2\x82\xAC" "c");
        EXPECT_EQ(s.cursor, 5U);
    }

    TEST(TextEditInsertTest, ControlCodepointsAreIgnored)
    {
        EditableText s { "ab", 1 };
        Portal::Text::insert_codepoint(s, '\n');
        Portal::Text::insert_codepoint(s, 0x7F);
        EXPECT_EQ(s.text, "ab");
        EXPECT_EQ(s.cursor, 1U);
    }

    TEST(TextEditInsertTest, InvalidCodepointInsertsNothing)
    {
        EditableText s { "ab", 1 };
        Portal::Text::insert_codepoint(s, 0x110000);
        EXPECT_EQ(s.text, "ab");
        EXPECT_EQ(s.cursor, 1U);
    }

    TEST(TextEditInsertTest, LiteralBypassesTheControlFilter)
    {
        EditableText s { "ab", 1 };
        Portal::Text::insert_literal(s, '\n');
        EXPECT_EQ(s.text, "a\nb");
        EXPECT_EQ(s.cursor, 2U);
    }

    struct EditCase {
        std::string_view name;
        std::string_view text;
        size_t cursor;
        int n;
        std::string_view expected_text;
        size_t expected_cursor;
    };

    const auto edit_case_name = [](const ::testing::TestParamInfo<EditCase>& info) {
        return std::string(info.param.name);
    };

    class TextEditMoveTest : public ::testing::TestWithParam<EditCase> { };

    TEST_P(TextEditMoveTest, MovesByWholeCodepointsAndClampsAtBothEnds)
    {
        const auto& c = GetParam();
        EditableText s { std::string(c.text), c.cursor };
        Portal::Text::move(s, c.n);
        EXPECT_EQ(s.text, c.expected_text);
        EXPECT_EQ(s.cursor, c.expected_cursor);
    }

    INSTANTIATE_TEST_SUITE_P(
        Moves, TextEditMoveTest,
        ::testing::Values(
            EditCase { "ForwardOne", "abc", 0, 1, "abc", 1 },
            EditCase { "ForwardPastMultibyte", "a\xC3\xA9" "b", 1, 1, "a\xC3\xA9" "b", 3 },
            EditCase { "BackPastMultibyte", "a\xC3\xA9" "b", 3, -1, "a\xC3\xA9" "b", 1 },
            EditCase { "ForwardManyClampsAtEnd", "abc", 1, 50, "abc", 3 },
            EditCase { "BackManyClampsAtStart", "abc", 2, -50, "abc", 0 },
            EditCase { "ZeroIsANoOp", "abc", 1, 0, "abc", 1 },
            EditCase { "EmptyText", "", 0, 3, "", 0 },
            EditCase { "ForwardOverFourByte", "\xF0\x9F\x98\x80" "x", 0, 1, "\xF0\x9F\x98\x80" "x", 4 }),
        edit_case_name);

    class TextEditEraseTest : public ::testing::TestWithParam<EditCase> { };

    TEST_P(TextEditEraseTest, ErasesWholeCodepointsAroundTheCursor)
    {
        const auto& c = GetParam();
        EditableText s { std::string(c.text), c.cursor };
        Portal::Text::erase(s, c.n);
        EXPECT_EQ(s.text, c.expected_text);
        EXPECT_EQ(s.cursor, c.expected_cursor);
    }

    INSTANTIATE_TEST_SUITE_P(
        Erasures, TextEditEraseTest,
        ::testing::Values(
            EditCase { "ForwardOne", "abc", 1, 1, "ac", 1 },
            EditCase { "ForwardMany", "abcd", 1, 2, "ad", 1 },
            EditCase { "ForwardMultibyte", "a\xC3\xA9" "b", 1, 1, "ab", 1 },
            EditCase { "ForwardPastEndClamps", "abc", 1, 50, "a", 1 },
            EditCase { "ForwardAtEndIsANoOp", "abc", 3, 1, "abc", 3 },
            EditCase { "BackwardOne", "abc", 2, -1, "ac", 1 },
            EditCase { "BackwardMany", "abcd", 3, -2, "ad", 1 },
            EditCase { "BackwardMultibyte", "a\xC3\xA9" "b", 3, -1, "ab", 1 },
            EditCase { "BackwardPastStartClamps", "abc", 2, -50, "c", 0 },
            EditCase { "BackwardAtStartIsANoOp", "abc", 0, -1, "abc", 0 },
            EditCase { "ZeroIsANoOp", "abc", 1, 0, "abc", 1 },
            EditCase { "BackwardFourByte", "x\xF0\x9F\x98\x80", 5, -1, "x", 1 }),
        edit_case_name);

    TEST(TextEditSelectionTest, NothingIsSelectedByDefault)
    {
        EditableText s { "abc", 1 };
        EXPECT_FALSE(Portal::Text::selection(s).has_value());
        EXPECT_TRUE(Portal::Text::selected_text(s).empty());
    }

    TEST(TextEditSelectionTest, AnAnchorEqualToTheCursorSelectsNothing)
    {
        EditableText s { "abc", 1 };
        s.anchor = 1;
        EXPECT_FALSE(Portal::Text::selection(s).has_value());
    }

    TEST(TextEditSelectionTest, TheRangeIsOrderedWhicheverEndIsTheCursor)
    {
        EditableText forward { "abcd", 3 };
        forward.anchor = 1;
        const auto a = Portal::Text::selection(forward);
        ASSERT_TRUE(a.has_value());
        EXPECT_EQ(a->first, 1U);
        EXPECT_EQ(a->second, 3U);

        EditableText backward { "abcd", 1 };
        backward.anchor = 3;
        const auto b = Portal::Text::selection(backward);
        ASSERT_TRUE(b.has_value());
        EXPECT_EQ(b->first, 1U);
        EXPECT_EQ(b->second, 3U);
        EXPECT_EQ(Portal::Text::selected_text(backward), "bc");
    }

    TEST(TextEditSelectionTest, OffsetsPastTheEndAreClamped)
    {
        EditableText s { "ab", 2 };
        s.anchor = 40;
        EXPECT_FALSE(Portal::Text::selection(s).has_value());
    }

    TEST(TextEditSelectionTest, SelectAllCoversTheTextWithTheCursorAtTheEnd)
    {
        EditableText s { "a\xC3\xA9" "b", 1 };
        Portal::Text::select_all(s);
        EXPECT_EQ(s.cursor, 4U);
        EXPECT_EQ(Portal::Text::selected_text(s), s.text);
    }

    TEST(TextEditSelectionTest, SelectAllOnEmptyTextSelectsNothing)
    {
        EditableText s;
        Portal::Text::select_all(s);
        EXPECT_FALSE(Portal::Text::selection(s).has_value());
    }

    TEST(TextEditSelectionTest, ExtendGrowsFromWhereTheCursorWasByWholeCodepoints)
    {
        EditableText s { "a\xC3\xA9" "b", 1 };
        Portal::Text::extend(s, 1);
        EXPECT_EQ(s.cursor, 3U);
        EXPECT_EQ(Portal::Text::selected_text(s), "\xC3\xA9");
        Portal::Text::extend(s, 1);
        EXPECT_EQ(Portal::Text::selected_text(s), "\xC3\xA9" "b");
    }

    TEST(TextEditSelectionTest, ExtendBackOverTheAnchorFlipsTheSelection)
    {
        EditableText s { "abcd", 2 };
        Portal::Text::extend(s, 1);
        Portal::Text::extend(s, -2);
        EXPECT_EQ(s.cursor, 1U);
        EXPECT_EQ(Portal::Text::selected_text(s), "b");
    }

    TEST(TextEditSelectionTest, ExtendingBackToTheAnchorDropsTheSelection)
    {
        EditableText s { "abcd", 2 };
        Portal::Text::extend(s, 1);
        Portal::Text::extend(s, -1);
        EXPECT_FALSE(Portal::Text::selection(s).has_value());
        EXPECT_FALSE(s.anchor.has_value());
    }

    TEST(TextEditSelectionTest, ExtendClampsAtTheEnds)
    {
        EditableText s { "abc", 1 };
        Portal::Text::extend(s, 50);
        EXPECT_EQ(Portal::Text::selected_text(s), "bc");
        Portal::Text::extend(s, -50);
        EXPECT_EQ(Portal::Text::selected_text(s), "a");
    }

    TEST(TextEditSelectionTest, PlainMoveCollapsesToTheNearEdgeInTheDirectionOfTheMove)
    {
        EditableText left { "abcd", 3 };
        left.anchor = 1;
        Portal::Text::move(left, -1);
        EXPECT_EQ(left.cursor, 1U);
        EXPECT_FALSE(left.anchor.has_value());

        EditableText right { "abcd", 1 };
        right.anchor = 3;
        Portal::Text::move(right, 1);
        EXPECT_EQ(right.cursor, 3U);
        EXPECT_FALSE(right.anchor.has_value());
    }

    TEST(TextEditSelectionTest, MoveToWithoutExtendDropsTheSelection)
    {
        EditableText s { "abcd", 1 };
        s.anchor = 3;
        Portal::Text::move_to(s, 2);
        EXPECT_EQ(s.cursor, 2U);
        EXPECT_FALSE(s.anchor.has_value());
    }

    TEST(TextEditSelectionTest, MoveToWithExtendKeepsTheExistingAnchor)
    {
        EditableText s { "abcd", 1 };
        Portal::Text::move_to(s, 2, true);
        Portal::Text::move_to(s, 4, true);
        EXPECT_EQ(s.anchor, 1U);
        EXPECT_EQ(Portal::Text::selected_text(s), "bcd");
    }

    TEST(TextEditSelectionTest, MoveToClampsToTheTextLength)
    {
        EditableText s { "ab", 0 };
        Portal::Text::move_to(s, 99);
        EXPECT_EQ(s.cursor, 2U);
    }

    TEST(TextEditSelectionTest, EraseSelectionRemovesItAndPutsTheCursorAtItsStart)
    {
        EditableText s { "abcd", 3 };
        s.anchor = 1;
        EXPECT_TRUE(Portal::Text::erase_selection(s));
        EXPECT_EQ(s.text, "ad");
        EXPECT_EQ(s.cursor, 1U);
        EXPECT_FALSE(s.anchor.has_value());
    }

    TEST(TextEditSelectionTest, EraseSelectionWithNothingSelectedChangesNothing)
    {
        EditableText s { "abc", 1 };
        EXPECT_FALSE(Portal::Text::erase_selection(s));
        EXPECT_EQ(s.text, "abc");
    }

    TEST(TextEditSelectionTest, EraseRemovesOnlyTheSelectionWhateverTheCount)
    {
        EditableText back { "abcd", 3 };
        back.anchor = 1;
        Portal::Text::erase(back, -1);
        EXPECT_EQ(back.text, "ad");
        EXPECT_EQ(back.cursor, 1U);

        EditableText forward { "abcd", 1 };
        forward.anchor = 3;
        Portal::Text::erase(forward, 5);
        EXPECT_EQ(forward.text, "ad");
        EXPECT_EQ(forward.cursor, 1U);
    }

    TEST(TextEditSelectionTest, TypingReplacesTheSelection)
    {
        EditableText s { "abcd", 3 };
        s.anchor = 1;
        Portal::Text::insert_codepoint(s, 'x');
        EXPECT_EQ(s.text, "axd");
        EXPECT_EQ(s.cursor, 2U);
        EXPECT_FALSE(s.anchor.has_value());
    }

    TEST(TextEditSelectionTest, AnIgnoredControlCodepointKeepsTheSelection)
    {
        EditableText s { "abcd", 3 };
        s.anchor = 1;
        Portal::Text::insert_codepoint(s, 0x16);
        EXPECT_EQ(s.text, "abcd");
        EXPECT_EQ(Portal::Text::selected_text(s), "bc");
    }

    TEST(TextEditSelectionTest, LiteralReplacesTheSelection)
    {
        EditableText s { "abcd", 3 };
        s.anchor = 1;
        Portal::Text::insert_literal(s, '\n');
        EXPECT_EQ(s.text, "a\nd");
        EXPECT_EQ(s.cursor, 2U);
    }

    TEST(TextEditInsertTextTest, TextGoesInAtTheCursorAndAdvancesItByItsLength)
    {
        EditableText s { "ad", 1 };
        Portal::Text::insert_text(s, "bc");
        EXPECT_EQ(s.text, "abcd");
        EXPECT_EQ(s.cursor, 3U);
    }

    TEST(TextEditInsertTextTest, TextReplacesTheSelection)
    {
        EditableText s { "abcd", 3 };
        s.anchor = 1;
        Portal::Text::insert_text(s, "XY");
        EXPECT_EQ(s.text, "aXYd");
        EXPECT_EQ(s.cursor, 3U);
    }

    TEST(TextEditInsertTextTest, CrlfAndLoneCrBecomeLf)
    {
        EditableText s;
        Portal::Text::insert_text(s, "a\r\nb\rc\n");
        EXPECT_EQ(s.text, "a\nb\nc\n");
        EXPECT_EQ(s.cursor, 6U);
    }

    TEST(TextEditInsertTextTest, TabsStayAndOtherControlBytesAreDropped)
    {
        EditableText s;
        Portal::Text::insert_text(s, std::string("a\tb\x01\x07\x7F" "c\0d", 9));
        EXPECT_EQ(s.text, "a\tbcd");
    }

    TEST(TextEditInsertTextTest, MultibyteSequencesPassThroughWhole)
    {
        EditableText s;
        Portal::Text::insert_text(s, "\xE2\x82\xAC\xF0\x9F\x98\x80");
        EXPECT_EQ(s.text, "\xE2\x82\xAC\xF0\x9F\x98\x80");
        EXPECT_EQ(s.cursor, 7U);
    }

    TEST(TextEditInsertTextTest, TextThatFiltersToNothingKeepsTheSelection)
    {
        EditableText s { "abcd", 3 };
        s.anchor = 1;
        Portal::Text::insert_text(s, "\x01\x02");
        EXPECT_EQ(s.text, "abcd");
        EXPECT_EQ(Portal::Text::selected_text(s), "bc");
    }

    TEST(TextEditInsertTextTest, EmptyTextChangesNothing)
    {
        EditableText s { "ab", 1 };
        Portal::Text::insert_text(s, "");
        EXPECT_EQ(s.text, "ab");
        EXPECT_EQ(s.cursor, 1U);
    }

} // namespace
} // namespace MayaFlux::Test
