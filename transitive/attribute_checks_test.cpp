#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Transitive/Reflect/AttributeChecks.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

struct AttributeCheckElement {
    std::string name;
    uint64_t location {};
    std::vector<std::pair<std::string, std::string>> attributes;
};

}

namespace MayaFlux::Reflect {

template <>
struct RecordAccess<Test::AttributeCheckElement> {
    using Source = Test::AttributeCheckElement;

    static std::string_view name(const Source& source) { return source.name; }
    static uint64_t location(const Source& source) { return source.location; }

    static std::optional<std::string_view> attribute(const Source& source, std::string_view name)
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
            if (!visit(std::string_view(key), std::string_view(value)))
                return false;
        }
        return true;
    }

    template <typename Visitor>
    static bool for_each_child(const Source&, Visitor&&)
    {
        return true;
    }
};

}

namespace MayaFlux::Test {
namespace {

    using Attributes = std::vector<std::pair<std::string, std::string>>;
    using Index = Reflect::AttributeIndex<AttributeCheckElement>;

    AttributeCheckElement make_element(std::string name, uint64_t location, Attributes attributes = {})
    {
        return AttributeCheckElement {
            .name = std::move(name),
            .location = location,
            .attributes = std::move(attributes),
        };
    }

    TEST(AttributeChecksTest, ALocatedMessageNamesTheRow)
    {
        EXPECT_EQ(Reflect::located_message(7, "broken"), "row 7: broken");
    }

    TEST(AttributeChecksTest, ClaimingRecordsTheValueAndWhereItIs)
    {
        Index ids("id");
        std::string error;

        ASSERT_TRUE(ids.claim(make_element("item", 3, { { "id", "a" } }), error)) << error;
        ASSERT_TRUE(ids.claim(make_element("item", 9, { { "id", "b" } }), error)) << error;

        EXPECT_TRUE(ids.contains("a"));
        EXPECT_TRUE(ids.contains("b"));
        EXPECT_FALSE(ids.contains("c"));
        EXPECT_EQ(ids.location_of("a"), std::optional<uint64_t>(3));
        EXPECT_EQ(ids.location_of("b"), std::optional<uint64_t>(9));
        EXPECT_FALSE(ids.location_of("c"));
        EXPECT_EQ(ids.attribute(), "id");
    }

    TEST(AttributeChecksTest, ASecondClaimOfAValueIsRefusedWithTheRowOfTheFirst)
    {
        Index ids("id");
        std::string error;
        ASSERT_TRUE(ids.claim(make_element("item", 3, { { "id", "a" } }), error)) << error;

        EXPECT_FALSE(ids.claim(make_element("other", 8, { { "id", "a" } }), error));

        EXPECT_EQ(error, "row 8: id a is already used at row 3");
        EXPECT_EQ(ids.location_of("a"), std::optional<uint64_t>(3));
    }

    TEST(AttributeChecksTest, AMissingOrEmptyValueIsRefusedByName)
    {
        Index ids("id");
        std::string error;

        EXPECT_FALSE(ids.claim(make_element("item", 4), error));
        EXPECT_EQ(error, "row 4: <item> needs a non-empty id");

        EXPECT_FALSE(ids.claim(make_element("item", 5, { { "id", "" } }), error));
        EXPECT_EQ(error, "row 5: <item> needs a non-empty id");
    }

    TEST(AttributeChecksTest, TheIndexedAttributeIsWhateverItIsGiven)
    {
        Index names("name");
        std::string error;

        ASSERT_TRUE(names.claim(make_element("item", 1, { { "name", "x" }, { "id", "same" } }), error)) << error;
        ASSERT_TRUE(names.claim(make_element("item", 2, { { "name", "y" }, { "id", "same" } }), error)) << error;

        EXPECT_FALSE(names.claim(make_element("item", 3, { { "name", "x" } }), error));
        EXPECT_NE(error.find("name x is already used"), std::string::npos) << error;
    }

    TEST(AttributeChecksTest, AReferenceToAKnownValueReturnsIt)
    {
        Index templates("id");
        std::string error;
        ASSERT_TRUE(templates.claim(make_element("template", 1, { { "id", "t" } }), error)) << error;

        const auto value = Reflect::check_reference(make_element("instance", 6, { { "template", "t" } }),
            "template", [&](std::string_view known) { return templates.contains(known); }, error);

        ASSERT_TRUE(value) << error;
        EXPECT_EQ(*value, "t");
    }

    TEST(AttributeChecksTest, AReferenceThatIsMissingOrUnknownIsRefusedAtItsRow)
    {
        const auto knows_nothing = [](std::string_view) { return false; };
        std::string error;

        EXPECT_FALSE(Reflect::check_reference(make_element("instance", 6), "template", knows_nothing, error));
        EXPECT_EQ(error, "row 6: <instance> needs a template attribute");

        EXPECT_FALSE(Reflect::check_reference(
            make_element("instance", 7, { { "template", "ghost" } }), "template", knows_nothing, error));
        EXPECT_EQ(error, "row 7: <instance> refers to unknown template ghost");
    }

    TEST(AttributeChecksTest, AnyCallableCanSayWhatExists)
    {
        const std::set<std::string, std::less<>> known { "a", "b" };
        std::string error;

        EXPECT_TRUE(Reflect::check_reference(make_element("link", 1, { { "to", "a" } }), "to",
            [&](std::string_view value) { return known.contains(value); }, error));
        EXPECT_FALSE(Reflect::check_reference(make_element("link", 2, { { "to", "z" } }), "to",
            [&](std::string_view value) { return known.contains(value); }, error));
        EXPECT_NE(error.find("unknown to z"), std::string::npos) << error;
    }

}
}
