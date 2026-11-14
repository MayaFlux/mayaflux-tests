#include "gtest/gtest.h"

#include "MayaFlux/Transitive/Reflect/RecordBinding.hpp"

namespace MayaFlux::Test {

enum class TextBindingMode : uint8_t { Alpha,
    Beta };

struct TextBindingSample {
    float x {};
    std::string tag { "keep" };
    bool flag {};
    std::optional<int32_t> count;
    TextBindingMode mode { TextBindingMode::Alpha };
    std::vector<std::string> unsupported;
};

struct RecordBindingItem {
    float x {};
    float y {};
    std::string tag;
};

struct RecordBindingList {
    std::string title;
    std::vector<RecordBindingItem> items;
};

struct RecordBindingElement {
    std::string name;
    uint64_t location {};
    std::vector<std::pair<std::string, std::string>> attributes;
    std::vector<RecordBindingElement> children;
};

}

namespace MayaFlux::Reflect::Generated {

template <>
struct Description<::MayaFlux::Test::TextBindingSample> {
    static constexpr auto members()
    {
        return std::tuple {
            Member<&::MayaFlux::Test::TextBindingSample::x> { "x" },
            Member<&::MayaFlux::Test::TextBindingSample::tag> { "tag" },
            Member<&::MayaFlux::Test::TextBindingSample::flag> { "flag" },
            Member<&::MayaFlux::Test::TextBindingSample::count> { "count" },
            Member<&::MayaFlux::Test::TextBindingSample::mode> { "mode" },
            Member<&::MayaFlux::Test::TextBindingSample::unsupported> { "unsupported" },
        };
    }
};

template <>
struct Description<::MayaFlux::Test::RecordBindingItem> {
    static constexpr auto members()
    {
        return std::tuple {
            Member<&::MayaFlux::Test::RecordBindingItem::x> { "x" },
            Member<&::MayaFlux::Test::RecordBindingItem::y> { "y" },
            Member<&::MayaFlux::Test::RecordBindingItem::tag> { "tag" },
        };
    }
};

template <>
struct Description<::MayaFlux::Test::RecordBindingList> {
    static constexpr auto members()
    {
        return std::tuple {
            Member<&::MayaFlux::Test::RecordBindingList::title> { "title" },
            Member<&::MayaFlux::Test::RecordBindingList::items> { "items" },
        };
    }
};

}

namespace MayaFlux::Reflect {

template <>
struct RecordAccess<Test::RecordBindingElement> {
    using Source = Test::RecordBindingElement;

    static std::string_view name(const Source& source) { return source.name; }
    static uint64_t location(const Source& source) { return source.location; }

    static std::optional<std::string_view> attribute(const Source& source,
        std::string_view name)
    {
        for (const auto& [key, value] : source.attributes) {
            if (key == name)
                return std::string_view(value);
        }
        return std::nullopt;
    }

    template <typename Visitor>
    static bool for_each_attribute(const Source& source, Visitor&& visit)
    {
        for (const auto& [key, value] : source.attributes) {
            if (!visit(key, value))
                return false;
        }
        return true;
    }

    template <typename Visitor>
    static bool for_each_child(const Source& source, Visitor&& visit)
    {
        for (const Source& child : source.children) {
            if (!visit(child))
                return false;
        }
        return true;
    }
};

}

namespace MayaFlux::Test {
namespace {

    using Element = RecordBindingElement;
    using Attributes = std::vector<std::pair<std::string, std::string>>;

    Element make_element(std::string name, uint64_t location, Attributes attributes = {},
        std::vector<Element> children = {})
    {
        return Element {
            .name = std::move(name),
            .location = location,
            .attributes = std::move(attributes),
            .children = std::move(children),
        };
    }

    Reflect::RecordBinding<RecordBindingList, Element> list_binding()
    {
        Reflect::RecordBinding<RecordBindingList, Element> binding;
        binding.repeated<&RecordBindingList::items>("item", "defaults", "x", 1);
        return binding;
    }

    TEST(TextCodecTest, NumbersParseFiniteTokensSeparatedByWhitespace)
    {
        const auto values = Reflect::parse_numbers<float>(" 1  2.5\t-3\n");
        ASSERT_TRUE(values);
        EXPECT_EQ(*values, (std::vector<float> { 1.F, 2.5F, -3.F }));

        const auto signed_value = Reflect::parse_numbers<int32_t>("+4");
        ASSERT_TRUE(signed_value);
        EXPECT_EQ(*signed_value, (std::vector<int32_t> { 4 }));

        const auto none = Reflect::parse_numbers<float>("   ");
        ASSERT_TRUE(none);
        EXPECT_TRUE(none->empty());
    }

    TEST(TextCodecTest, NumbersRejectMalformedAndNonFiniteTokens)
    {
        for (const char* source : { "1 x", "1,2", "inf", "nan", "++1", "+-1", "+", "1e999" })
            EXPECT_FALSE(Reflect::parse_numbers<float>(source)) << source;
        EXPECT_FALSE(Reflect::parse_numbers<int32_t>("1.5"));
        EXPECT_FALSE(Reflect::parse_numbers<uint32_t>("-1"));
    }

    TEST(TextCodecTest, TupleDistinguishesInvalidTextFromWrongCount)
    {
        Reflect::TextError error = Reflect::TextError::Unsupported;
        EXPECT_FALSE((Reflect::parse_tuple<float, 3>("1 2", error)));
        EXPECT_EQ(error, Reflect::TextError::Cardinality);
        EXPECT_FALSE((Reflect::parse_tuple<float, 3>("1 2 x", error)));
        EXPECT_EQ(error, Reflect::TextError::Invalid);

        const auto tuple = Reflect::parse_tuple<float, 2>("4 5", error);
        ASSERT_TRUE(tuple);
        EXPECT_EQ((*tuple)[1], 5.F);
    }

    TEST(TextCodecTest, BoolAcceptsOnlyTrueFalseOneAndZero)
    {
        Reflect::TextError error = Reflect::TextError::Unsupported;
        bool value = false;
        EXPECT_TRUE(Reflect::TextCodec<bool>::decode("true", value, error));
        EXPECT_TRUE(value);
        EXPECT_TRUE(Reflect::TextCodec<bool>::decode("0", value, error));
        EXPECT_FALSE(value);
        EXPECT_TRUE(Reflect::TextCodec<bool>::decode("1", value, error));
        EXPECT_TRUE(value);
        EXPECT_FALSE(Reflect::TextCodec<bool>::decode("yes", value, error));
        EXPECT_EQ(error, Reflect::TextError::Invalid);
        EXPECT_TRUE(value);
    }

    TEST(TextCodecTest, EnumsDecodeExactNamesOnly)
    {
        Reflect::TextError error = Reflect::TextError::Invalid;
        TextBindingMode mode = TextBindingMode::Alpha;
        EXPECT_TRUE(Reflect::TextCodec<TextBindingMode>::decode("Beta", mode, error));
        EXPECT_EQ(mode, TextBindingMode::Beta);
        EXPECT_FALSE(Reflect::TextCodec<TextBindingMode>::decode("beta", mode, error));
        EXPECT_EQ(error, Reflect::TextError::Unsupported);
        EXPECT_FALSE(Reflect::TextCodec<TextBindingMode>::decode("Gamma", mode, error));
        EXPECT_EQ(mode, TextBindingMode::Beta);
    }

    TEST(TextCodecTest, OptionalKeepsItsValueWhenDecodingFails)
    {
        Reflect::TextError error = Reflect::TextError::Unsupported;
        std::optional<float> value = 2.F;
        EXPECT_FALSE(Reflect::TextCodec<std::optional<float>>::decode("x", value, error));
        EXPECT_EQ(value, std::optional<float> { 2.F });
        EXPECT_TRUE(Reflect::TextCodec<std::optional<float>>::decode("5", value, error));
        EXPECT_EQ(value, std::optional<float> { 5.F });
        EXPECT_EQ(Reflect::TextCodec<std::optional<float>>::extent, 1U);

        std::string text;
        EXPECT_TRUE(Reflect::TextCodec<std::string>::decode(" as is ", text, error));
        EXPECT_EQ(text, " as is ");
    }

    TEST(TextCodecTest, ScalarSequencesTakeEveryToken)
    {
        static_assert(Reflect::TextCodec<std::vector<float>>::sequence);
        Reflect::TextError error = Reflect::TextError::Unsupported;
        std::vector<float> values { 9.F };
        EXPECT_TRUE(Reflect::TextCodec<std::vector<float>>::decode("1 2.5 -3", values, error));
        EXPECT_EQ(values, (std::vector<float> { 1.F, 2.5F, -3.F }));

        EXPECT_TRUE(Reflect::TextCodec<std::vector<float>>::decode("", values, error));
        EXPECT_TRUE(values.empty());

        values = { 7.F };
        EXPECT_FALSE(Reflect::TextCodec<std::vector<float>>::decode("1 x", values, error));
        EXPECT_EQ(error, Reflect::TextError::Invalid);
        EXPECT_EQ(values, (std::vector<float> { 7.F }));
    }

    TEST(TextCodecTest, IntegerSequencesRejectFractionsAndAnOptionalSequenceDecodes)
    {
        Reflect::TextError error = Reflect::TextError::Unsupported;
        std::vector<uint32_t> counts;
        EXPECT_TRUE(Reflect::TextCodec<std::vector<uint32_t>>::decode("3 5 8", counts, error));
        EXPECT_EQ(counts, (std::vector<uint32_t> { 3, 5, 8 }));
        EXPECT_FALSE(Reflect::TextCodec<std::vector<uint32_t>>::decode("3 5.5", counts, error));

        std::optional<std::vector<float>> maybe;
        EXPECT_TRUE(Reflect::TextCodec<std::optional<std::vector<float>>>::decode("1 2", maybe, error));
        ASSERT_TRUE(maybe);
        EXPECT_EQ(maybe->size(), 2U);
    }

    TEST(TextBindingTest, DecodeFieldNamesTheOwnerAndTheFailure)
    {
        std::string error;
        float number {};
        EXPECT_FALSE(Reflect::decode_text_field("x", "1 2", number, "owner", error));
        EXPECT_EQ(error, "<owner> needs 1 numbers in x");
        EXPECT_FALSE(Reflect::decode_text_field("x", "abc", number, "owner", error));
        EXPECT_EQ(error, "<owner> has an invalid x attribute");

        TextBindingMode mode {};
        EXPECT_FALSE(Reflect::decode_text_field("mode", "Gamma", mode, "owner", error));
        EXPECT_EQ(error, "<owner> has an unsupported mode Gamma");

        std::vector<std::string> unsupported;
        EXPECT_FALSE(Reflect::decode_text_field("w", "1", unsupported, "owner", error));
        EXPECT_EQ(error, "<owner> has an unsupported attribute w");

        EXPECT_TRUE(Reflect::decode_text_field("x", "7", number, "owner", error));
        EXPECT_EQ(number, 7.F);
    }

    TEST(TextBindingTest, OverlayChangesOnlyTheNamesSupplied)
    {
        TextBindingSample sample;
        std::string error;
        const bool applied = Reflect::overlay_text_fields(sample,
            [](const auto& visit) {
                return visit("x", "3") && visit("flag", "true") && visit("count", "9")
                    && visit("mode", "Beta");
            },
            "owner", error);
        ASSERT_TRUE(applied) << error;
        EXPECT_EQ(sample.x, 3.F);
        EXPECT_TRUE(sample.flag);
        EXPECT_EQ(sample.count, std::optional<int32_t> { 9 });
        EXPECT_EQ(sample.mode, TextBindingMode::Beta);
        EXPECT_EQ(sample.tag, "keep");
    }

    TEST(TextBindingTest, OverlayRejectsUnknownNamesAndBadValues)
    {
        TextBindingSample sample;
        std::string error;
        EXPECT_FALSE(Reflect::overlay_text_fields(sample,
            [](const auto& visit) { return visit("missing", "1"); }, "owner", error));
        EXPECT_EQ(error, "<owner> has an unsupported attribute missing");

        EXPECT_FALSE(Reflect::overlay_text_fields(sample,
            [](const auto& visit) { return visit("x", "abc"); }, "owner", error));
        EXPECT_EQ(error, "<owner> has an invalid x attribute");
    }

    TEST(RecordBindingTest, AttributesOverlayDescribedMembers)
    {
        Reflect::RecordBinding<RecordBindingList, Element> binding;
        binding.required("title");
        std::string error;
        uint64_t location = 99;
        const auto list = binding.read(make_element("list", 4, { { "title", "Notes" } }), error, &location);
        ASSERT_TRUE(list) << error;
        EXPECT_EQ(list->title, "Notes");
        EXPECT_TRUE(list->items.empty());
        EXPECT_EQ(location, 4U);
    }

    TEST(RecordBindingTest, MissingRequiredAttributeNamesTheElement)
    {
        Reflect::RecordBinding<RecordBindingList, Element> binding;
        binding.required("title");
        std::string error;
        uint64_t location = 0;
        EXPECT_FALSE(binding.read(make_element("list", 6), error, &location));
        EXPECT_EQ(error, "<list> needs a title attribute");
        EXPECT_EQ(location, 6U);

        EXPECT_FALSE(binding.read(make_element("list", 6, { { "id", "main" } }), error));
        EXPECT_EQ(error, "<main> needs a title attribute");
    }

    TEST(RecordBindingTest, UnknownAttributesAreRejectedByDefault)
    {
        Reflect::RecordBinding<RecordBindingList, Element> binding;
        std::string error;
        EXPECT_FALSE(binding.read(
            make_element("list", 0, { { "title", "t" }, { "id", "a1" } }), error));
        EXPECT_NE(error.find("unsupported attribute id"), std::string::npos);
    }

    TEST(RecordBindingTest, IgnoredAttributesAreSkippedButDescribedMembersStayStrict)
    {
        Reflect::RecordBinding<RecordBindingItem, Element> binding;
        binding.ignore({ "id", "form" });
        std::string error;

        const auto item = binding.read(
            make_element("item", 0, { { "id", "a" }, { "form", "f" }, { "x", "2" } }), error);
        ASSERT_TRUE(item) << error;
        EXPECT_EQ(item->x, 2.F);

        EXPECT_FALSE(binding.read(
            make_element("item", 0, { { "id", "a" }, { "x", "abc" } }), error));
        EXPECT_NE(error.find("invalid x attribute"), std::string::npos);

        EXPECT_FALSE(binding.read(
            make_element("item", 0, { { "id", "a" }, { "other", "1" } }), error));
        EXPECT_NE(error.find("unsupported attribute other"), std::string::npos);
    }

    TEST(RecordBindingTest, AnIgnoredPrefixSkipsEveryAttributeBeginningWithIt)
    {
        Reflect::RecordBinding<RecordBindingItem, Element> binding;
        binding.ignore_prefix("mf:");
        std::string error;

        const auto item = binding.read(
            make_element("item", 0, { { "mf:hidden", "true" }, { "mf:anything", "else" }, { "x", "2" } }), error);
        ASSERT_TRUE(item) << error;
        EXPECT_EQ(item->x, 2.F);

        EXPECT_FALSE(binding.read(make_element("item", 0, { { "other", "1" } }), error));
        EXPECT_NE(error.find("unsupported attribute other"), std::string::npos);

        EXPECT_FALSE(binding.read(make_element("item", 0, { { "mfx", "1" } }), error));
        EXPECT_NE(error.find("unsupported attribute mfx"), std::string::npos);
    }

    TEST(RecordBindingTest, NamesAndPrefixesCombineAndDescribedMembersStayStrict)
    {
        Reflect::RecordBinding<RecordBindingItem, Element> binding;
        binding.ignore({ "id" }).ignore_prefix("mf:");
        std::string error;

        const auto item = binding.read(
            make_element("item", 0, { { "id", "a" }, { "mf:front", "true" }, { "y", "3" } }), error);
        ASSERT_TRUE(item) << error;
        EXPECT_EQ(item->y, 3.F);

        EXPECT_FALSE(binding.read(
            make_element("item", 0, { { "mf:front", "true" }, { "x", "abc" } }), error));
        EXPECT_NE(error.find("invalid x attribute"), std::string::npos);
    }

    TEST(RecordBindingTest, AnIgnoredChildElementIsLeftToAnotherReaderAndOthersAreRejected)
    {
        Reflect::RecordBinding<RecordBindingItem, Element> binding;
        binding.ignore_child("on");
        std::string error;

        const auto item = binding.read(
            make_element("item", 0, { { "x", "2" } }, { make_element("on", 1), make_element("on", 2) }), error);
        ASSERT_TRUE(item) << error;
        EXPECT_EQ(item->x, 2.F);

        EXPECT_FALSE(binding.read(make_element("item", 0, {}, { make_element("legend", 3) }), error));
        EXPECT_NE(error.find("<legend> is not allowed in <item>"), std::string::npos);
    }

    TEST(RecordBindingTest, RepeatedChildrenInheritSharedDefaults)
    {
        const auto binding = list_binding();
        std::string error;
        const auto list = binding.read(
            make_element("list", 0, { { "title", "t" } },
                { make_element("defaults", 1, { { "tag", "base" }, { "y", "9" } }),
                    make_element("item", 2, { { "x", "1" } }),
                    make_element("item", 3, { { "x", "2" }, { "tag", "own" } }) }),
            error);
        ASSERT_TRUE(list) << error;
        ASSERT_EQ(list->items.size(), 2U);
        EXPECT_EQ(list->items[0].x, 1.F);
        EXPECT_EQ(list->items[0].y, 9.F);
        EXPECT_EQ(list->items[0].tag, "base");
        EXPECT_EQ(list->items[1].x, 2.F);
        EXPECT_EQ(list->items[1].y, 9.F);
        EXPECT_EQ(list->items[1].tag, "own");
    }

    TEST(RecordBindingTest, DefaultsMayNotCarryTheRequiredItemAttribute)
    {
        const auto binding = list_binding();
        std::string error;
        uint64_t location = 0;
        EXPECT_FALSE(binding.read(
            make_element("list", 0, { { "title", "t" } },
                { make_element("defaults", 5, { { "x", "1" } }) }),
            error, &location));
        EXPECT_EQ(error, "<defaults> has an unsupported attribute x");
        EXPECT_EQ(location, 5U);
    }

    TEST(RecordBindingTest, DefaultsMustAppearOnceBeforeTheItems)
    {
        const auto binding = list_binding();
        std::string error;
        EXPECT_FALSE(binding.read(
            make_element("list", 0, { { "title", "t" } },
                { make_element("item", 1, { { "x", "1" } }),
                    make_element("defaults", 2, { { "tag", "late" } }) }),
            error));
        EXPECT_NE(error.find("must appear once before"), std::string::npos);

        EXPECT_FALSE(binding.read(
            make_element("list", 0, { { "title", "t" } },
                { make_element("defaults", 1), make_element("defaults", 2),
                    make_element("item", 3, { { "x", "1" } }) }),
            error));
        EXPECT_NE(error.find("must appear once before"), std::string::npos);
    }

    TEST(RecordBindingTest, MinimumItemCountIsEnforcedAtTheParent)
    {
        const auto binding = list_binding();
        std::string error;
        uint64_t location = 0;
        EXPECT_FALSE(binding.read(
            make_element("list", 8, { { "title", "t" } }), error, &location));
        EXPECT_EQ(error, "<list> needs at least 1 item children");
        EXPECT_EQ(location, 8U);
    }

    TEST(RecordBindingTest, ItemsNeedTheirRequiredAttributeAndMayNotHaveChildren)
    {
        const auto binding = list_binding();
        std::string error;
        uint64_t location = 0;
        EXPECT_FALSE(binding.read(
            make_element("list", 0, { { "title", "t" } },
                { make_element("item", 4, { { "tag", "no x" } }) }),
            error, &location));
        EXPECT_EQ(error, "<item> needs a x attribute");
        EXPECT_EQ(location, 4U);

        EXPECT_FALSE(binding.read(
            make_element("list", 0, { { "title", "t" } },
                { make_element("item", 4, { { "x", "1" } }, { make_element("inner", 5) }) }),
            error));
        EXPECT_EQ(error, "<item> may not have children");
    }

    TEST(RecordBindingTest, UnknownChildrenAreRejectedAtTheirOwnLocation)
    {
        const auto binding = list_binding();
        std::string error;
        uint64_t location = 0;
        EXPECT_FALSE(binding.read(
            make_element("list", 0, { { "title", "t" } },
                { make_element("item", 1, { { "x", "1" } }), make_element("other", 7) }),
            error, &location));
        EXPECT_EQ(error, "<other> is not allowed in <list>");
        EXPECT_EQ(location, 7U);
    }

}
}
