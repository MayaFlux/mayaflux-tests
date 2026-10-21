#include "gtest/gtest.h"

#include <string>
#include <string_view>

#include "MayaFlux/Portal/Text/TextEdit.hpp"

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

} // namespace
} // namespace MayaFlux::Test
