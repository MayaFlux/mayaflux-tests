#include "gtest/gtest.h"

#include "MayaFlux/Kakshya/NDData/CompositeAccess.hpp"
#include "MayaFlux/Kakshya/NDData/CompositeInsertion.hpp"

namespace MayaFlux::Test {
namespace {

    using Kakshya::CompositeArray;
    using Kakshya::CompositeInsertion;
    using Kakshya::CompositeLayout;
    using Kakshya::DataDimension;
    using Kakshya::DataModality;
    using Kakshya::DataVariant;
    using Kakshya::Region;

    CompositeLayout people_layout()
    {
        CompositeLayout layout;
        layout.add_field<std::string>("name");
        layout.add_field<uint32_t>("rank");
        layout.add_field<double>("score");
        return layout;
    }

    CompositeArray make_people()
    {
        CompositeArray people(people_layout());
        CompositeInsertion insert(people);

        const size_t ada = insert.append();
        insert.set_text(ada, "name", "Ada");
        insert.set(ada, "rank", uint32_t { 1 });
        insert.set(ada, "score", 0.95);

        const size_t lin = insert.append();
        insert.set_text(lin, "name", "Lin");
        insert.set(lin, "rank", uint32_t { 2 });
        insert.set(lin, "score", 0.81);

        const size_t jo = insert.append();
        insert.set_text(jo, "name", "Jo");
        insert.set(jo, "rank", uint32_t { 3 });

        const size_t nia = insert.append();
        insert.set_text(nia, "name", "Nia");
        insert.set(nia, "rank", uint32_t { 4 });
        insert.set(nia, "score", 0.74);

        return people;
    }

    CompositeLayout dynamic_layout()
    {
        CompositeLayout layout;
        layout.add_dynamic_field("any");
        layout.add_field<std::vector<uint8_t>>("bytes");
        layout.add_field<std::string>("label");
        return layout;
    }

    struct RawSlice {
        uint64_t offset;
        uint64_t length;
    };

    // ---------------------------------------------------------------------
    // CompositeLayout
    // ---------------------------------------------------------------------

    TEST(CompositeLayoutTest, DeclaredFieldsKeepTheirOrderAndNames)
    {
        const CompositeLayout layout = people_layout();

        ASSERT_EQ(layout.fields().size(), 3U);
        EXPECT_EQ(layout.fields()[0].name, "name");
        EXPECT_EQ(layout.fields()[1].name, "rank");
        EXPECT_EQ(layout.fields()[2].name, "score");
        EXPECT_EQ(layout.fields()[0].type, std::type_index(typeid(std::string)));
        EXPECT_EQ(layout.fields()[1].type, std::type_index(typeid(uint32_t)));
        EXPECT_EQ(layout.fields()[2].type, std::type_index(typeid(double)));
    }

    TEST(CompositeLayoutTest, FieldSizesFollowTheirKind)
    {
        CompositeLayout layout;
        layout.add_field<uint8_t>("byte");
        layout.add_field<float>("single");
        layout.add_field<double>("double");
        layout.add_field<std::string>("text");
        layout.add_field<std::vector<uint8_t>>("blob");
        layout.add_dynamic_field("dynamic");

        EXPECT_EQ(layout.fields()[0].size_bytes, 1U);
        EXPECT_EQ(layout.fields()[1].size_bytes, 4U);
        EXPECT_EQ(layout.fields()[2].size_bytes, 8U);
        EXPECT_EQ(layout.fields()[3].size_bytes, 16U);
        EXPECT_EQ(layout.fields()[4].size_bytes, 16U);
        EXPECT_EQ(layout.fields()[5].size_bytes, 24U);
    }

    TEST(CompositeLayoutTest, EmptyAndDuplicateNamesAreRefused)
    {
        CompositeLayout layout;

        EXPECT_TRUE(layout.add_field<double>("score"));
        EXPECT_FALSE(layout.add_field<double>(""));
        EXPECT_FALSE(layout.add_field<uint32_t>("score"));
        EXPECT_FALSE(layout.add_dynamic_field("score"));
        EXPECT_FALSE(layout.add_dynamic_field(""));
        EXPECT_EQ(layout.fields().size(), 1U);
    }

    TEST(CompositeLayoutTest, FindFieldReturnsTheDeclarationIndex)
    {
        const CompositeLayout layout = people_layout();

        EXPECT_EQ(layout.find_field("name"), 0U);
        EXPECT_EQ(layout.find_field("score"), 2U);
        EXPECT_FALSE(layout.find_field("missing").has_value());
        EXPECT_FALSE(layout.find_field("").has_value());
    }

    TEST(CompositeLayoutTest, OffsetsAndStrideAreAssignedWhenAnArrayIsBuilt)
    {
        const CompositeArray people(people_layout());
        const CompositeLayout& layout = people.layout();

        EXPECT_EQ(layout.presence_bytes(), 1U);
        EXPECT_EQ(layout.fields()[0].offset_bytes, 1U);
        EXPECT_EQ(layout.fields()[1].offset_bytes, 17U);
        EXPECT_EQ(layout.fields()[2].offset_bytes, 21U);
        EXPECT_EQ(layout.stride_bytes(), 29U);
    }

    TEST(CompositeLayoutTest, DeclaringAloneDoesNotAssignOffsets)
    {
        const CompositeLayout layout = people_layout();

        EXPECT_EQ(layout.stride_bytes(), 0U);
        EXPECT_EQ(layout.presence_bytes(), 0U);
    }

    TEST(CompositeLayoutTest, PresenceBitmapGrowsAByteEveryEightFields)
    {
        for (const size_t count : { size_t { 1 }, size_t { 8 }, size_t { 9 }, size_t { 16 }, size_t { 17 } }) {
            CompositeLayout layout;
            for (size_t i = 0; i < count; ++i) {
                layout.add_field<uint8_t>("f" + std::to_string(i));
            }

            const CompositeArray array(std::move(layout));

            EXPECT_EQ(array.layout().presence_bytes(), (count + 7) / 8) << count;
            EXPECT_EQ(array.layout().stride_bytes(), (count + 7) / 8 + count) << count;
            EXPECT_EQ(array.layout().fields().front().offset_bytes, array.layout().presence_bytes()) << count;
        }
    }

    TEST(CompositeLayoutTest, ArrayNeedsAtLeastOneField)
    {
        EXPECT_THROW(CompositeArray(CompositeLayout {}), std::invalid_argument);
    }

    // ---------------------------------------------------------------------
    // CompositeArray and Composite
    // ---------------------------------------------------------------------

    TEST(CompositeArrayTest, StartsEmpty)
    {
        const CompositeArray array(people_layout());

        EXPECT_TRUE(array.empty());
        EXPECT_EQ(array.size(), 0U);
        EXPECT_FALSE(array.at(0).has_value());
    }

    TEST(CompositeArrayTest, AppendCountsUpAndZeroFillsWithNothingPresent)
    {
        CompositeArray array(people_layout());

        EXPECT_EQ(array.append(), 0U);
        EXPECT_EQ(array.append(), 1U);

        EXPECT_EQ(array.size(), 2U);
        EXPECT_FALSE(array.empty());
        const auto& bytes = std::get<std::vector<uint8_t>>(array.element_data());
        EXPECT_EQ(bytes.size(), 2 * array.layout().stride_bytes());
        EXPECT_TRUE(std::ranges::all_of(bytes, [](uint8_t b) { return b == 0; }));
        for (const auto& field : { "name", "rank", "score" }) {
            EXPECT_FALSE(array.at(0)->has(field)) << field;
        }
    }

    TEST(CompositeArrayTest, AtIsBoundsChecked)
    {
        const CompositeArray people = make_people();

        EXPECT_TRUE(people.at(3).has_value());
        EXPECT_FALSE(people.at(4).has_value());
        EXPECT_FALSE(people.at(std::numeric_limits<size_t>::max()).has_value());
    }

    TEST(CompositeArrayTest, ValuesReadBackThroughTheirExactTypes)
    {
        const CompositeArray people = make_people();

        const auto ada = people.at(0);

        ASSERT_TRUE(ada.has_value());
        EXPECT_EQ(ada->text("name"), "Ada");
        EXPECT_EQ(ada->get<uint32_t>("rank"), 1U);
        EXPECT_DOUBLE_EQ(*ada->get<double>("score"), 0.95);
        EXPECT_EQ(&ada->layout(), &people.layout());
    }

    TEST(CompositeArrayTest, UnalignedFieldsReadBackIntact)
    {
        const CompositeArray people = make_people();

        EXPECT_NE(people.layout().fields()[2].offset_bytes % alignof(double), 0U);
        EXPECT_DOUBLE_EQ(*people.at(3)->get<double>("score"), 0.74);
    }

    TEST(CompositeArrayTest, AbsentFieldsAreNotValues)
    {
        const CompositeArray people = make_people();
        const auto jo = people.at(2);

        ASSERT_TRUE(jo.has_value());
        EXPECT_TRUE(jo->has("name"));
        EXPECT_FALSE(jo->has("score"));
        EXPECT_FALSE(jo->get<double>("score").has_value());
        EXPECT_TRUE(jo->has("rank"));
    }

    TEST(CompositeArrayTest, UnknownFieldsAreAbsent)
    {
        const CompositeArray people = make_people();
        const auto ada = people.at(0);

        EXPECT_FALSE(ada->has("missing"));
        EXPECT_FALSE(ada->get<double>("missing").has_value());
        EXPECT_FALSE(ada->text("missing").has_value());
        EXPECT_FALSE(ada->blob("missing").has_value());
        EXPECT_FALSE(ada->value("missing").has_value());
    }

    TEST(CompositeArrayTest, ReadsRequireTheExactDeclaredType)
    {
        const CompositeArray people = make_people();
        const auto ada = people.at(0);

        EXPECT_FALSE(ada->get<float>("score").has_value());
        EXPECT_FALSE(ada->get<uint64_t>("rank").has_value());
        EXPECT_FALSE(ada->get<double>("rank").has_value());
        EXPECT_FALSE(ada->text("rank").has_value());
        EXPECT_FALSE(ada->get<double>("name").has_value());
        EXPECT_FALSE(ada->blob("name").has_value());
    }

    TEST(CompositeArrayTest, WritesRequireTheExactDeclaredType)
    {
        CompositeArray array(people_layout());
        const size_t index = array.append();

        EXPECT_FALSE(array.set(index, "score", 0.5F));
        EXPECT_FALSE(array.set(index, "rank", 3));
        EXPECT_FALSE(array.set(index, "name", 1.0));
        EXPECT_FALSE(array.set(index, "missing", 1.0));
        EXPECT_FALSE(array.set(index + 1, "score", 1.0));
        EXPECT_FALSE(array.at(index)->has("score"));
        EXPECT_FALSE(array.at(index)->has("rank"));
    }

    TEST(CompositeArrayTest, SettingAFieldMarksItPresentWithoutTouchingOthers)
    {
        CompositeArray array(people_layout());
        const size_t index = array.append();

        EXPECT_TRUE(array.set(index, "rank", uint32_t { 7 }));

        const auto item = array.at(index);
        EXPECT_TRUE(item->has("rank"));
        EXPECT_FALSE(item->has("name"));
        EXPECT_FALSE(item->has("score"));
    }

    TEST(CompositeArrayTest, OverwritingReplacesTheValue)
    {
        CompositeArray array(people_layout());
        const size_t index = array.append();
        array.set(index, "score", 1.0);

        array.set(index, "score", 2.0);

        EXPECT_DOUBLE_EQ(*array.at(index)->get<double>("score"), 2.0);
    }

    TEST(CompositeArrayTest, ElementsDoNotDisturbEachOther)
    {
        CompositeArray array(people_layout());
        const size_t first = array.append();
        const size_t second = array.append();

        array.set(first, "rank", uint32_t { 111 });
        array.set(second, "rank", uint32_t { 222 });

        EXPECT_EQ(array.at(first)->get<uint32_t>("rank"), 111U);
        EXPECT_EQ(array.at(second)->get<uint32_t>("rank"), 222U);
    }

    TEST(CompositeArrayTest, ClearMarksAbsentButKeepsTheStoredBytes)
    {
        CompositeArray array(people_layout());
        const size_t index = array.append();
        array.set(index, "score", 0.5);
        const auto before = std::get<std::vector<uint8_t>>(array.element_data());

        EXPECT_TRUE(array.clear(index, "score"));

        EXPECT_FALSE(array.at(index)->has("score"));
        EXPECT_FALSE(array.at(index)->get<double>("score").has_value());
        const auto after = std::get<std::vector<uint8_t>>(array.element_data());
        const size_t offset = index * array.layout().stride_bytes() + array.layout().fields()[2].offset_bytes;
        EXPECT_TRUE(std::equal(before.begin() + static_cast<std::ptrdiff_t>(offset),
            before.begin() + static_cast<std::ptrdiff_t>(offset + sizeof(double)),
            after.begin() + static_cast<std::ptrdiff_t>(offset)));
    }

    TEST(CompositeArrayTest, ClearThenSetMakesTheFieldPresentAgain)
    {
        CompositeArray array(people_layout());
        const size_t index = array.append();
        array.set(index, "score", 0.5);
        array.clear(index, "score");

        array.set(index, "score", 0.75);

        EXPECT_DOUBLE_EQ(*array.at(index)->get<double>("score"), 0.75);
    }

    TEST(CompositeArrayTest, ClearRefusesBadIndicesAndNames)
    {
        CompositeArray array(people_layout());
        const size_t index = array.append();

        EXPECT_FALSE(array.clear(index + 1, "score"));
        EXPECT_FALSE(array.clear(index, "missing"));
    }

    TEST(CompositeArrayTest, EveryFieldOfAWideLayoutHasItsOwnPresenceBit)
    {
        CompositeLayout layout;
        for (int i = 0; i < 12; ++i) {
            layout.add_field<uint8_t>("f" + std::to_string(i));
        }
        CompositeArray array(std::move(layout));
        const size_t index = array.append();
        array.append();

        for (const int field : { 0, 7, 8, 11 }) {
            array.set(index, "f" + std::to_string(field), static_cast<uint8_t>(field + 1));
        }

        const auto item = array.at(index);
        for (int i = 0; i < 12; ++i) {
            const bool expected = i == 0 || i == 7 || i == 8 || i == 11;
            EXPECT_EQ(item->has("f" + std::to_string(i)), expected) << i;
            EXPECT_FALSE(array.at(1)->has("f" + std::to_string(i))) << i;
        }
        EXPECT_EQ(item->get<uint8_t>("f8"), 9U);
        EXPECT_EQ(item->get<uint8_t>("f11"), 12U);
    }

    TEST(CompositeArrayTest, PlainIntegerFieldsAreExactTypes)
    {
        CompositeLayout layout;
        layout.add_field<int64_t>("big");
        layout.add_field<float>("single");
        CompositeArray array(std::move(layout));
        const size_t index = array.append();

        EXPECT_TRUE(array.set(index, "big", int64_t { -5 }));
        EXPECT_TRUE(array.set(index, "single", 1.5F));

        EXPECT_EQ(array.at(index)->get<int64_t>("big"), -5);
        EXPECT_FLOAT_EQ(*array.at(index)->get<float>("single"), 1.5F);
        EXPECT_FALSE(array.at(index)->get<double>("big").has_value());
    }

    TEST(CompositeArrayTest, ViewsObserveLaterWrites)
    {
        CompositeArray array(people_layout());
        const size_t index = array.append();
        const auto item = array.at(index);
        ASSERT_FALSE(item->has("rank"));

        array.set(index, "rank", uint32_t { 9 });

        EXPECT_EQ(item->get<uint32_t>("rank"), 9U);
    }

    TEST(CompositeTextTest, TextIsStoredInTheSharedTextStore)
    {
        const CompositeArray people = make_people();

        const auto& text = std::get<std::vector<uint8_t>>(people.text_data());

        EXPECT_EQ(std::string(text.begin(), text.end()), "AdaLinJoNia");
    }

    TEST(CompositeTextTest, EmptyTextIsAPresentValue)
    {
        CompositeArray array(people_layout());
        const size_t index = array.append();

        EXPECT_TRUE(array.set_text(index, "name", ""));

        EXPECT_TRUE(array.at(index)->has("name"));
        ASSERT_TRUE(array.at(index)->text("name").has_value());
        EXPECT_TRUE(array.at(index)->text("name")->empty());
    }

    TEST(CompositeTextTest, RewritingAppendsAndLeavesTheOldBytesBehind)
    {
        CompositeArray array(people_layout());
        const size_t index = array.append();
        array.set_text(index, "name", "Ada");

        array.set_text(index, "name", "Grace");

        EXPECT_EQ(array.at(index)->text("name"), "Grace");
        EXPECT_EQ(std::get<std::vector<uint8_t>>(array.text_data()).size(), 8U);
    }

    TEST(CompositeTextTest, TextBytesAreNotInterpreted)
    {
        CompositeArray array(people_layout());
        const size_t index = array.append();
        const std::string value("a\0b\xC3\xA9", 5);

        array.set_text(index, "name", value);

        EXPECT_EQ(array.at(index)->text("name"), std::string_view(value));
    }

    TEST(CompositeTextTest, OnlyTextFieldsTakeText)
    {
        CompositeArray array(people_layout());
        const size_t index = array.append();

        EXPECT_FALSE(array.set_text(index, "rank", "x"));
        EXPECT_FALSE(array.set_text(index, "missing", "x"));
        EXPECT_FALSE(array.set_text(index + 1, "name", "x"));
        EXPECT_TRUE(std::get<std::vector<uint8_t>>(array.text_data()).empty());
    }

    TEST(CompositeTextTest, ClearedTextIsAbsent)
    {
        CompositeArray array(people_layout());
        const size_t index = array.append();
        array.set_text(index, "name", "Ada");

        array.clear(index, "name");

        EXPECT_FALSE(array.at(index)->text("name").has_value());
    }

    TEST(CompositeBlobTest, BytesRoundTripThroughTheBlobStore)
    {
        CompositeArray array(dynamic_layout());
        const size_t index = array.append();
        const std::vector<uint8_t> payload { 0, 1, 2, 255 };

        EXPECT_TRUE(array.set_blob(index, "bytes", payload));

        const auto blob = array.at(index)->blob("bytes");
        ASSERT_TRUE(blob.has_value());
        EXPECT_TRUE(std::ranges::equal(*blob, payload));
        EXPECT_EQ(std::get<std::vector<uint8_t>>(array.blob_data()), payload);
    }

    TEST(CompositeBlobTest, EmptyBlobIsAPresentValue)
    {
        CompositeArray array(dynamic_layout());
        const size_t index = array.append();

        EXPECT_TRUE(array.set_blob(index, "bytes", std::span<const uint8_t> {}));

        ASSERT_TRUE(array.at(index)->blob("bytes").has_value());
        EXPECT_TRUE(array.at(index)->blob("bytes")->empty());
    }

    TEST(CompositeBlobTest, BlobsAndTextUseSeparateFields)
    {
        CompositeArray array(dynamic_layout());
        const size_t index = array.append();
        const std::vector<uint8_t> payload { 1, 2 };
        array.set_blob(index, "bytes", payload);
        array.set_text(index, "label", "x");

        EXPECT_FALSE(array.set_blob(index, "label", payload));
        EXPECT_FALSE(array.set_text(index, "bytes", "y"));
        EXPECT_FALSE(array.at(index)->text("bytes").has_value());
        EXPECT_FALSE(array.at(index)->blob("label").has_value());
    }

    TEST(CompositeValueTest, DynamicFieldsHoldTheKindWrittenToThem)
    {
        CompositeArray array(dynamic_layout());
        const std::vector<uint8_t> payload { 9, 8, 7 };
        const size_t null_row = array.append();
        const size_t int_row = array.append();
        const size_t real_row = array.append();
        const size_t text_row = array.append();
        const size_t blob_row = array.append();

        EXPECT_TRUE(array.set_null(null_row, "any"));
        EXPECT_TRUE(array.set(int_row, "any", int64_t { -42 }));
        EXPECT_TRUE(array.set(real_row, "any", 2.5));
        EXPECT_TRUE(array.set_text(text_row, "any", "hello"));
        EXPECT_TRUE(array.set_blob(blob_row, "any", payload));

        EXPECT_TRUE(std::holds_alternative<std::monostate>(*array.at(null_row)->value("any")));
        EXPECT_EQ(std::get<int64_t>(*array.at(int_row)->value("any")), -42);
        EXPECT_DOUBLE_EQ(std::get<double>(*array.at(real_row)->value("any")), 2.5);
        EXPECT_EQ(std::get<std::string_view>(*array.at(text_row)->value("any")), "hello");
        EXPECT_TRUE(std::ranges::equal(std::get<std::span<const uint8_t>>(*array.at(blob_row)->value("any")), payload));
    }

    TEST(CompositeValueTest, NullIsPresentButAnAbsentFieldIsNot)
    {
        CompositeArray array(dynamic_layout());
        const size_t index = array.append();

        EXPECT_FALSE(array.at(index)->has("any"));
        EXPECT_FALSE(array.at(index)->value("any").has_value());

        array.set_null(index, "any");

        EXPECT_TRUE(array.at(index)->has("any"));
        EXPECT_TRUE(array.at(index)->value("any").has_value());
    }

    TEST(CompositeValueTest, DynamicNumbersReadThroughTheirOwnKindOnly)
    {
        CompositeArray array(dynamic_layout());
        const size_t integer = array.append();
        const size_t real = array.append();
        array.set(integer, "any", int64_t { 7 });
        array.set(real, "any", 0.5);

        EXPECT_EQ(array.at(integer)->get<int64_t>("any"), 7);
        EXPECT_FALSE(array.at(integer)->get<double>("any").has_value());
        EXPECT_DOUBLE_EQ(*array.at(real)->get<double>("any"), 0.5);
        EXPECT_FALSE(array.at(real)->get<int64_t>("any").has_value());
        EXPECT_FALSE(array.at(real)->get<float>("any").has_value());
    }

    TEST(CompositeValueTest, DynamicFieldsRefuseOtherNumericWrites)
    {
        CompositeArray array(dynamic_layout());
        const size_t index = array.append();

        EXPECT_FALSE(array.set(index, "any", 1.5F));
        EXPECT_FALSE(array.set(index, "any", 3));
        EXPECT_FALSE(array.set(index, "any", uint32_t { 3 }));
        EXPECT_FALSE(array.at(index)->has("any"));
    }

    TEST(CompositeValueTest, TextAndBlobAccessorsFollowTheStoredKind)
    {
        CompositeArray array(dynamic_layout());
        const size_t text_row = array.append();
        const size_t int_row = array.append();
        array.set_text(text_row, "any", "hi");
        array.set(int_row, "any", int64_t { 1 });

        EXPECT_EQ(array.at(text_row)->text("any"), "hi");
        EXPECT_FALSE(array.at(text_row)->blob("any").has_value());
        EXPECT_FALSE(array.at(int_row)->text("any").has_value());
        EXPECT_FALSE(array.at(int_row)->blob("any").has_value());
    }

    TEST(CompositeValueTest, ADynamicFieldCanChangeKind)
    {
        CompositeArray array(dynamic_layout());
        const size_t index = array.append();
        array.set(index, "any", int64_t { 1 });

        array.set_text(index, "any", "now text");

        EXPECT_EQ(array.at(index)->text("any"), "now text");
        EXPECT_FALSE(array.at(index)->get<int64_t>("any").has_value());
    }

    TEST(CompositeValueTest, FixedFieldsHaveNoDynamicValue)
    {
        CompositeArray array(dynamic_layout());
        const size_t index = array.append();
        array.set_text(index, "label", "x");

        EXPECT_FALSE(array.at(index)->value("label").has_value());
        EXPECT_FALSE(array.set_null(index, "label"));
    }

    // ---------------------------------------------------------------------
    // CompositeAccess
    // ---------------------------------------------------------------------

    TEST(CompositeAccessTest, ArrayAccessViewsItsStoresWithoutCopying)
    {
        const CompositeArray people = make_people();

        const auto access = people.access();

        ASSERT_TRUE(access.has_value());
        EXPECT_EQ(access->size(), 4U);
        EXPECT_EQ(&access->layout(), &people.layout());
        EXPECT_EQ(&access->element_data(), &people.element_data());
        EXPECT_EQ(&access->text_data(), &people.text_data());
        EXPECT_EQ(&access->blob_data(), &people.blob_data());
        EXPECT_EQ(access->element_bytes().data(), std::get<std::vector<uint8_t>>(people.element_data()).data());
        EXPECT_EQ(access->element_bytes().size(), 4 * people.layout().stride_bytes());
    }

    TEST(CompositeAccessTest, AtIsBoundsCheckedAndReadsLikeTheArray)
    {
        const CompositeArray people = make_people();
        const auto access = people.access();

        EXPECT_EQ(access->at(1)->text("name"), "Lin");
        EXPECT_EQ(access->at(3)->get<uint32_t>("rank"), 4U);
        EXPECT_FALSE(access->at(4).has_value());
    }

    TEST(CompositeAccessTest, ByteDimensionsAreElementsByStride)
    {
        const CompositeArray people = make_people();
        const auto access = people.access();

        const auto dims = access->byte_dimensions();

        ASSERT_EQ(dims.size(), 2U);
        EXPECT_EQ(dims[0].name, "elements");
        EXPECT_EQ(dims[0].size, 4U);
        EXPECT_EQ(dims[0].stride, people.layout().stride_bytes());
        EXPECT_EQ(dims[0].role, DataDimension::Role::CUSTOM);
        EXPECT_EQ(dims[1].name, "bytes");
        EXPECT_EQ(dims[1].size, people.layout().stride_bytes());
        EXPECT_EQ(dims[1].stride, 1U);
        EXPECT_EQ(dims[1].role, DataDimension::Role::CUSTOM);
    }

    TEST(CompositeAccessTest, EmptyArraysAreAccessible)
    {
        const CompositeArray array(people_layout());

        const auto access = array.access();

        ASSERT_TRUE(access.has_value());
        EXPECT_EQ(access->size(), 0U);
        EXPECT_TRUE(access->element_bytes().empty());
    }

    TEST(AsCompositeAccessTest, ExternalStoresAreValidatedAgainstTheLayout)
    {
        const CompositeArray people = make_people();

        const auto access = Kakshya::as_composite_access(
            people.element_data(), people.text_data(), people.blob_data(), people.layout());

        ASSERT_TRUE(access.has_value());
        EXPECT_EQ(access->size(), 4U);
    }

    TEST(AsCompositeAccessTest, EveryStoreMustHoldBytes)
    {
        const CompositeArray people = make_people();
        const DataVariant floats = std::vector<float>(4, 0.0F);

        EXPECT_FALSE(Kakshya::as_composite_access(floats, people.text_data(), people.blob_data(), people.layout()));
        EXPECT_FALSE(Kakshya::as_composite_access(people.element_data(), floats, people.blob_data(), people.layout()));
        EXPECT_FALSE(Kakshya::as_composite_access(people.element_data(), people.text_data(), floats, people.layout()));
    }

    TEST(AsCompositeAccessTest, ElementBytesMustBeWholeElements)
    {
        const CompositeArray people = make_people();
        const DataVariant partial = std::vector<uint8_t>(people.layout().stride_bytes() + 1, 0);

        EXPECT_FALSE(Kakshya::as_composite_access(partial, people.text_data(), people.blob_data(), people.layout()));
    }

    TEST(AsCompositeAccessTest, ALayoutWithoutAStrideIsRefused)
    {
        const CompositeArray people = make_people();
        const CompositeLayout unfinished = people_layout();

        EXPECT_FALSE(Kakshya::as_composite_access(people.element_data(), people.text_data(), people.blob_data(), unfinished));
    }

    TEST(AsCompositeAccessTest, TextBoundsAreCheckedWhenAFieldIsRead)
    {
        const CompositeArray owner(people_layout());
        const CompositeLayout& layout = owner.layout();
        const size_t name_offset = layout.fields()[0].offset_bytes;
        const auto build_rows = [&](RawSlice slice) {
            std::vector<uint8_t> rows(layout.stride_bytes(), 0);
            rows[0] = 1;
            std::memcpy(rows.data() + name_offset, &slice, sizeof(slice));
            return DataVariant { std::move(rows) };
        };
        const DataVariant text = std::vector<uint8_t> { 'a', 'b', 'c' };
        const DataVariant blob = std::vector<uint8_t> {};

        const DataVariant inside = build_rows({ 1, 2 });
        const DataVariant past_end = build_rows({ 2, 5 });
        const DataVariant offset_past_end = build_rows({ 9, 0 });
        const DataVariant overflowing = build_rows({ 1, std::numeric_limits<uint64_t>::max() });

        EXPECT_EQ(Kakshya::as_composite_access(inside, text, blob, layout)->at(0)->text("name"), "bc");
        EXPECT_FALSE(Kakshya::as_composite_access(past_end, text, blob, layout)->at(0)->text("name").has_value());
        EXPECT_FALSE(Kakshya::as_composite_access(offset_past_end, text, blob, layout)->at(0)->text("name").has_value());
        EXPECT_FALSE(Kakshya::as_composite_access(overflowing, text, blob, layout)->at(0)->text("name").has_value());
    }

    TEST(AsCompositeAccessTest, BlobBoundsAreCheckedWhenAFieldIsRead)
    {
        const CompositeArray owner(dynamic_layout());
        const CompositeLayout& layout = owner.layout();
        const size_t blob_offset = layout.fields()[1].offset_bytes;
        std::vector<uint8_t> rows(layout.stride_bytes(), 0);
        rows[0] = 0b010;
        const RawSlice slice { 2, 4 };
        std::memcpy(rows.data() + blob_offset, &slice, sizeof(slice));
        const DataVariant elements = std::move(rows);
        const DataVariant text = std::vector<uint8_t> {};
        const DataVariant blob = std::vector<uint8_t> { 1, 2, 3 };

        const auto access = Kakshya::as_composite_access(elements, text, blob, layout);

        ASSERT_TRUE(access.has_value());
        EXPECT_FALSE(access->at(0)->blob("bytes").has_value());
    }

    // ---------------------------------------------------------------------
    // CompositeSlice
    // ---------------------------------------------------------------------

    TEST(CompositeSliceTest, SelectsConsecutiveElementsWithRelativeIndices)
    {
        const CompositeArray people = make_people();

        const auto slice = people.slice(1, 2);

        ASSERT_TRUE(slice.has_value());
        EXPECT_EQ(slice->size(), 2U);
        EXPECT_EQ(slice->start(), 1U);
        EXPECT_EQ(slice->at(0)->text("name"), "Lin");
        EXPECT_EQ(slice->at(1)->text("name"), "Jo");
        EXPECT_FALSE(slice->at(2).has_value());
        EXPECT_EQ(&slice->layout(), &people.layout());
    }

    TEST(CompositeSliceTest, ElementBytesAreTheSelectedRangeOfThePackedStore)
    {
        const CompositeArray people = make_people();
        const size_t stride = people.layout().stride_bytes();

        const auto slice = people.slice(1, 2);

        EXPECT_EQ(slice->element_bytes().size(), 2 * stride);
        EXPECT_EQ(slice->element_bytes().data(), std::get<std::vector<uint8_t>>(people.element_data()).data() + stride);
    }

    TEST(CompositeSliceTest, SlicesShareTheOriginalTextAndBlobStores)
    {
        const CompositeArray people = make_people();

        const auto slice = people.slice(2, 1);

        EXPECT_EQ(&slice->text_data(), &people.text_data());
        EXPECT_EQ(&slice->blob_data(), &people.blob_data());
    }

    TEST(CompositeSliceTest, BoundsAreInclusiveOfTheEndAndEmptySlicesAreAllowed)
    {
        const CompositeArray people = make_people();

        EXPECT_TRUE(people.slice(0, 4).has_value());
        EXPECT_TRUE(people.slice(4, 0).has_value());
        EXPECT_TRUE(people.slice(2, 0).has_value());
        EXPECT_EQ(people.slice(4, 0)->size(), 0U);
        EXPECT_TRUE(people.slice(4, 0)->element_bytes().empty());
    }

    TEST(CompositeSliceTest, RangesPastTheEndAreRefused)
    {
        const CompositeArray people = make_people();

        EXPECT_FALSE(people.slice(0, 5).has_value());
        EXPECT_FALSE(people.slice(3, 2).has_value());
        EXPECT_FALSE(people.slice(5, 0).has_value());
        EXPECT_FALSE(people.slice(1, std::numeric_limits<size_t>::max()).has_value());
    }

    TEST(CompositeSliceTest, OffsetSlicesHaveNoRegion)
    {
        const CompositeArray people = make_people();

        EXPECT_EQ(people.slice(0, 2)->region(), nullptr);
    }

    TEST(CompositeSliceTest, RegionBoundsAreInclusive)
    {
        const CompositeArray people = make_people();
        const Region region(std::vector<uint64_t> { 1 }, std::vector<uint64_t> { 2 });

        const auto slice = people.slice(region);

        ASSERT_TRUE(slice.has_value());
        EXPECT_EQ(slice->start(), 1U);
        EXPECT_EQ(slice->size(), 2U);
        EXPECT_EQ(slice->at(0)->text("name"), "Lin");
        EXPECT_EQ(slice->at(1)->text("name"), "Jo");
    }

    TEST(CompositeSliceTest, ASinglePointRegionSelectsOneElement)
    {
        const CompositeArray people = make_people();
        const Region region(std::vector<uint64_t> { 3 });

        const auto slice = people.slice(region);

        ASSERT_TRUE(slice.has_value());
        EXPECT_EQ(slice->size(), 1U);
        EXPECT_EQ(slice->at(0)->text("name"), "Nia");
    }

    TEST(CompositeSliceTest, RegionSlicesRetainTheRegionAndItsAttributes)
    {
        const CompositeArray people = make_people();
        Region region(std::vector<uint64_t> { 0 }, std::vector<uint64_t> { 1 });
        region.set_attribute("label", std::string("top"));

        const auto slice = people.slice(region);

        ASSERT_TRUE(slice.has_value());
        ASSERT_NE(slice->region(), nullptr);
        EXPECT_EQ(slice->region()->start_coordinates, region.start_coordinates);
        EXPECT_EQ(slice->region()->end_coordinates, region.end_coordinates);
        EXPECT_EQ(std::any_cast<std::string>(slice->region()->attributes.at("label")), "top");
    }

    TEST(CompositeSliceTest, InvalidRegionsAreRefused)
    {
        const CompositeArray people = make_people();

        EXPECT_FALSE(people.slice(Region(std::vector<uint64_t> { 2 }, std::vector<uint64_t> { 1 })).has_value());
        EXPECT_FALSE(people.slice(Region(std::vector<uint64_t> { 0 }, std::vector<uint64_t> { 4 })).has_value());
        EXPECT_FALSE(people.slice(Region(std::vector<uint64_t> { 4 })).has_value());
        EXPECT_FALSE(people.slice(Region(std::vector<uint64_t> { 0, 0 }, std::vector<uint64_t> { 1, 1 })).has_value());
    }

    TEST(CompositeSliceTest, RegionsNeverSelectFromAnEmptyArray)
    {
        const CompositeArray array(people_layout());

        EXPECT_FALSE(array.slice(Region(std::vector<uint64_t> { 0 })).has_value());
    }

    TEST(CompositeSliceTest, AccessSlicingMatchesArraySlicing)
    {
        const CompositeArray people = make_people();
        const auto access = people.access();

        const auto from_access = access->slice(1, 3);
        const auto from_array = people.slice(1, 3);

        ASSERT_TRUE(from_access.has_value());
        EXPECT_EQ(from_access->size(), from_array->size());
        EXPECT_EQ(from_access->element_bytes().data(), from_array->element_bytes().data());
        EXPECT_FALSE(access->slice(2, 3).has_value());
    }

    // ---------------------------------------------------------------------
    // Projection
    // ---------------------------------------------------------------------

    TEST(CompositeProjectionTest, ProjectsAFieldAsValuesAndPresence)
    {
        const CompositeArray people = make_people();

        const auto projection = people.to_nddata<double>("score");

        ASSERT_TRUE(projection.has_value());
        const auto& values = std::get<std::vector<double>>(projection->values);
        const auto& presence = std::get<std::vector<uint8_t>>(projection->presence);
        ASSERT_EQ(values.size(), 4U);
        ASSERT_EQ(presence.size(), 4U);
        EXPECT_EQ(presence, (std::vector<uint8_t> { 1, 1, 0, 1 }));
        EXPECT_DOUBLE_EQ(values[0], 0.95);
        EXPECT_DOUBLE_EQ(values[1], 0.81);
        EXPECT_DOUBLE_EQ(values[2], 0.0);
        EXPECT_DOUBLE_EQ(values[3], 0.74);
    }

    TEST(CompositeProjectionTest, DescribesTheFieldAsOneCustomAxis)
    {
        const CompositeArray people = make_people();

        const auto projection = people.to_nddata<uint32_t>("rank");

        ASSERT_TRUE(projection.has_value());
        EXPECT_EQ(projection->modality, DataModality::TENSOR_ND);
        ASSERT_EQ(projection->dimensions.size(), 1U);
        EXPECT_EQ(projection->dimensions[0].name, "rank");
        EXPECT_EQ(projection->dimensions[0].size, 4U);
        EXPECT_EQ(projection->dimensions[0].stride, 1U);
        EXPECT_EQ(projection->dimensions[0].role, DataDimension::Role::CUSTOM);
        EXPECT_EQ(std::get<std::vector<uint32_t>>(projection->values), (std::vector<uint32_t> { 1, 2, 3, 4 }));
    }

    TEST(CompositeProjectionTest, ClearedValuesAreZeroFilledAndMarkedAbsent)
    {
        CompositeArray people = make_people();
        people.clear(0, "score");

        const auto projection = people.to_nddata<double>("score");

        EXPECT_DOUBLE_EQ(std::get<std::vector<double>>(projection->values)[0], 0.0);
        EXPECT_EQ(std::get<std::vector<uint8_t>>(projection->presence)[0], 0U);
    }

    TEST(CompositeProjectionTest, TheFieldTypeMustMatchExactly)
    {
        const CompositeArray people = make_people();

        EXPECT_FALSE(people.to_nddata<float>("score").has_value());
        EXPECT_FALSE(people.to_nddata<double>("rank").has_value());
        EXPECT_FALSE(people.to_nddata<double>("name").has_value());
        EXPECT_FALSE(people.to_nddata<double>("missing").has_value());
    }

    TEST(CompositeProjectionTest, SlicesProjectOnlyTheirElements)
    {
        const CompositeArray people = make_people();

        const auto projection = people.slice(1, 2)->to_nddata<double>("score");

        ASSERT_TRUE(projection.has_value());
        EXPECT_EQ(projection->dimensions[0].size, 2U);
        EXPECT_DOUBLE_EQ(std::get<std::vector<double>>(projection->values)[0], 0.81);
        EXPECT_EQ(std::get<std::vector<uint8_t>>(projection->presence), (std::vector<uint8_t> { 1, 0 }));
    }

    TEST(CompositeProjectionTest, EmptyArraysProjectEmptySequences)
    {
        const CompositeArray array(people_layout());

        const auto projection = array.to_nddata<double>("score");

        ASSERT_TRUE(projection.has_value());
        EXPECT_TRUE(std::get<std::vector<double>>(projection->values).empty());
        EXPECT_EQ(projection->dimensions[0].size, 0U);
    }

    TEST(CompositeProjectionTest, ProjectionIsAnIndependentCopy)
    {
        CompositeArray people = make_people();
        const auto projection = people.to_nddata<double>("score");

        people.set(0, "score", 9.0);

        EXPECT_DOUBLE_EQ(std::get<std::vector<double>>(projection->values)[0], 0.95);
    }

    TEST(CompositeProjectionTest, ProjectionsOfEverySupportedStorageTypeWork)
    {
        CompositeLayout layout;
        layout.add_field<uint8_t>("u8");
        layout.add_field<uint16_t>("u16");
        layout.add_field<float>("f32");
        CompositeArray array(std::move(layout));
        const size_t index = array.append();
        array.set(index, "u8", uint8_t { 200 });
        array.set(index, "u16", uint16_t { 60000 });
        array.set(index, "f32", 0.25F);

        EXPECT_EQ(std::get<std::vector<uint8_t>>(array.to_nddata<uint8_t>("u8")->values).front(), 200U);
        EXPECT_EQ(std::get<std::vector<uint16_t>>(array.to_nddata<uint16_t>("u16")->values).front(), 60000U);
        EXPECT_FLOAT_EQ(std::get<std::vector<float>>(array.to_nddata<float>("f32")->values).front(), 0.25F);
    }

    // ---------------------------------------------------------------------
    // CompositeInsertion
    // ---------------------------------------------------------------------

    TEST(CompositeInsertionTest, ForwardsWritesToTheBorrowedArray)
    {
        CompositeArray array(people_layout());
        CompositeInsertion insert(array);

        const size_t index = insert.append();

        EXPECT_EQ(index, 0U);
        EXPECT_TRUE(insert.set_text(index, "name", "Ada"));
        EXPECT_TRUE(insert.set(index, "rank", uint32_t { 5 }));
        EXPECT_TRUE(insert.set(index, "score", 0.5));
        EXPECT_EQ(array.at(index)->text("name"), "Ada");
        EXPECT_EQ(array.at(index)->get<uint32_t>("rank"), 5U);
        EXPECT_DOUBLE_EQ(*array.at(index)->get<double>("score"), 0.5);
    }

    TEST(CompositeInsertionTest, ForwardsRefusalsFromTheArray)
    {
        CompositeArray array(people_layout());
        CompositeInsertion insert(array);
        const size_t index = insert.append();

        EXPECT_FALSE(insert.set(index, "score", 1.0F));
        EXPECT_FALSE(insert.set(index + 1, "score", 1.0));
        EXPECT_FALSE(insert.set_text(index, "rank", "x"));
        EXPECT_FALSE(insert.set_null(index, "name"));
        EXPECT_FALSE(insert.clear(index, "missing"));
    }

    TEST(CompositeInsertionTest, ForwardsDynamicBlobAndClearWrites)
    {
        CompositeArray array(dynamic_layout());
        CompositeInsertion insert(array);
        const size_t index = insert.append();
        const std::vector<uint8_t> payload { 4, 5 };

        EXPECT_TRUE(insert.set_null(index, "any"));
        EXPECT_TRUE(insert.set_blob(index, "bytes", payload));
        EXPECT_TRUE(insert.clear(index, "any"));

        EXPECT_FALSE(array.at(index)->has("any"));
        EXPECT_TRUE(std::ranges::equal(*array.at(index)->blob("bytes"), payload));
    }

    TEST(CompositeInsertionTest, MultipleAdaptersWriteToTheSameArray)
    {
        CompositeArray array(people_layout());
        CompositeInsertion first(array);
        CompositeInsertion second(array);

        const size_t a = first.append();
        const size_t b = second.append();

        EXPECT_EQ(a, 0U);
        EXPECT_EQ(b, 1U);
        EXPECT_EQ(array.size(), 2U);
    }

    TEST(CompositeInsertionCopyTest, CopiesAllPresentFieldsIntoAnotherArray)
    {
        const CompositeArray source = make_people();
        CompositeArray destination(people_layout());
        CompositeInsertion insert(destination);

        const auto index = insert.append(*source.at(0));

        ASSERT_TRUE(index.has_value());
        EXPECT_EQ(*index, 0U);
        const auto copy = destination.at(*index);
        EXPECT_EQ(copy->text("name"), "Ada");
        EXPECT_EQ(copy->get<uint32_t>("rank"), 1U);
        EXPECT_DOUBLE_EQ(*copy->get<double>("score"), 0.95);
    }

    TEST(CompositeInsertionCopyTest, AbsentFieldsStayAbsent)
    {
        const CompositeArray source = make_people();
        CompositeArray destination(people_layout());

        const auto index = CompositeInsertion(destination).append(*source.at(2));

        ASSERT_TRUE(index.has_value());
        EXPECT_TRUE(destination.at(*index)->has("name"));
        EXPECT_FALSE(destination.at(*index)->has("score"));
    }

    TEST(CompositeInsertionCopyTest, TextIsRebasedIntoTheDestinationStore)
    {
        const CompositeArray source = make_people();
        CompositeArray destination(people_layout());
        CompositeInsertion insert(destination);
        const size_t existing = insert.append();
        insert.set_text(existing, "name", "Already here");

        const auto index = insert.append(*source.at(1));

        ASSERT_TRUE(index.has_value());
        EXPECT_EQ(destination.at(existing)->text("name"), "Already here");
        EXPECT_EQ(destination.at(*index)->text("name"), "Lin");
        const auto& text = std::get<std::vector<uint8_t>>(destination.text_data());
        EXPECT_EQ(std::string(text.begin(), text.end()), "Already hereLin");
        EXPECT_EQ(std::get<std::vector<uint8_t>>(source.text_data()).size(), 11U);
    }

    TEST(CompositeInsertionCopyTest, BlobsAndDynamicValuesAreRebased)
    {
        CompositeArray source(dynamic_layout());
        const std::vector<uint8_t> payload { 1, 2, 3 };
        const size_t text_row = source.append();
        const size_t blob_row = source.append();
        const size_t int_row = source.append();
        const size_t null_row = source.append();
        source.set_text(text_row, "any", "dyn");
        source.set_text(text_row, "label", "lab");
        source.set_blob(blob_row, "any", payload);
        source.set_blob(blob_row, "bytes", payload);
        source.set(int_row, "any", int64_t { 77 });
        source.set_null(null_row, "any");
        CompositeArray destination(dynamic_layout());
        CompositeInsertion insert(destination);
        insert.set_text(insert.append(), "label", "padding");
        const std::vector<uint8_t> blob_padding { 9, 9 };
        insert.set_blob(0, "bytes", blob_padding);

        const auto copied_text = insert.append(*source.at(text_row));
        const auto copied_blob = insert.append(*source.at(blob_row));
        const auto copied_int = insert.append(*source.at(int_row));
        const auto copied_null = insert.append(*source.at(null_row));

        ASSERT_TRUE(copied_text && copied_blob && copied_int && copied_null);
        EXPECT_EQ(destination.at(*copied_text)->text("any"), "dyn");
        EXPECT_EQ(destination.at(*copied_text)->text("label"), "lab");
        EXPECT_TRUE(std::ranges::equal(*destination.at(*copied_blob)->blob("any"), payload));
        EXPECT_TRUE(std::ranges::equal(*destination.at(*copied_blob)->blob("bytes"), payload));
        EXPECT_EQ(destination.at(*copied_int)->get<int64_t>("any"), 77);
        EXPECT_TRUE(std::holds_alternative<std::monostate>(*destination.at(*copied_null)->value("any")));
    }

    TEST(CompositeInsertionCopyTest, CopyIsIndependentOfTheSource)
    {
        CompositeArray source = make_people();
        CompositeArray destination(people_layout());
        const auto index = CompositeInsertion(destination).append(*source.at(0));

        source.set(0, "score", 0.0);
        source.set_text(0, "name", "Changed");

        EXPECT_DOUBLE_EQ(*destination.at(*index)->get<double>("score"), 0.95);
        EXPECT_EQ(destination.at(*index)->text("name"), "Ada");
    }

    TEST(CompositeInsertionCopyTest, AnArrayCanCopyItsOwnElements)
    {
        CompositeArray people = make_people();
        CompositeInsertion insert(people);

        const auto index = insert.append(*people.at(1));

        ASSERT_TRUE(index.has_value());
        EXPECT_EQ(*index, 4U);
        EXPECT_EQ(people.size(), 5U);
        EXPECT_EQ(people.at(*index)->text("name"), "Lin");
        EXPECT_EQ(people.at(*index)->get<uint32_t>("rank"), 2U);
        EXPECT_EQ(people.at(1)->text("name"), "Lin");
        EXPECT_EQ(people.at(0)->text("name"), "Ada");
    }

    TEST(CompositeInsertionCopyTest, DifferentLayoutsAreRefused)
    {
        const CompositeArray source = make_people();

        CompositeLayout fewer;
        fewer.add_field<std::string>("name");
        fewer.add_field<uint32_t>("rank");
        CompositeArray fewer_fields(std::move(fewer));

        CompositeLayout renamed;
        renamed.add_field<std::string>("name");
        renamed.add_field<uint32_t>("rank");
        renamed.add_field<double>("points");
        CompositeArray other_name(std::move(renamed));

        CompositeLayout retyped;
        retyped.add_field<std::string>("name");
        retyped.add_field<uint32_t>("rank");
        retyped.add_field<float>("score");
        CompositeArray other_type(std::move(retyped));

        EXPECT_FALSE(CompositeInsertion(fewer_fields).append(*source.at(0)).has_value());
        EXPECT_FALSE(CompositeInsertion(other_name).append(*source.at(0)).has_value());
        EXPECT_FALSE(CompositeInsertion(other_type).append(*source.at(0)).has_value());
        EXPECT_TRUE(fewer_fields.empty());
        EXPECT_TRUE(other_name.empty());
        EXPECT_TRUE(other_type.empty());
    }

    TEST(CompositeInsertionCopyTest, FieldOrderIsPartOfTheLayout)
    {
        const CompositeArray source = make_people();
        CompositeLayout reordered;
        reordered.add_field<uint32_t>("rank");
        reordered.add_field<std::string>("name");
        reordered.add_field<double>("score");
        CompositeArray destination(std::move(reordered));

        EXPECT_FALSE(CompositeInsertion(destination).append(*source.at(0)).has_value());
    }

    TEST(CompositeInsertionCopyTest, SourceTextWithBadBoundsIsRefusedAndNothingIsWritten)
    {
        const CompositeArray owner(people_layout());
        const CompositeLayout& layout = owner.layout();
        std::vector<uint8_t> rows(layout.stride_bytes(), 0);
        rows[0] = 1;
        const RawSlice bad { 10, 5 };
        std::memcpy(rows.data() + layout.fields()[0].offset_bytes, &bad, sizeof(bad));
        const DataVariant elements = std::move(rows);
        const DataVariant text = std::vector<uint8_t> { 'a' };
        const DataVariant blob = std::vector<uint8_t> {};
        const auto access = Kakshya::as_composite_access(elements, text, blob, layout);
        ASSERT_TRUE(access.has_value());
        CompositeArray destination(people_layout());

        const auto index = CompositeInsertion(destination).append(*access->at(0));

        EXPECT_FALSE(index.has_value());
        EXPECT_TRUE(destination.empty());
        EXPECT_TRUE(std::get<std::vector<uint8_t>>(destination.text_data()).empty());
    }

    TEST(CompositeInsertionCopyTest, CopiesFromASliceAndAnAccess)
    {
        const CompositeArray source = make_people();
        CompositeArray destination(people_layout());
        CompositeInsertion insert(destination);

        const auto from_slice = insert.append(*source.slice(2, 2)->at(1));
        const auto from_access = insert.append(*source.access()->at(0));

        ASSERT_TRUE(from_slice && from_access);
        EXPECT_EQ(destination.at(*from_slice)->text("name"), "Nia");
        EXPECT_EQ(destination.at(*from_access)->text("name"), "Ada");
    }

} // namespace
} // namespace MayaFlux::Test
