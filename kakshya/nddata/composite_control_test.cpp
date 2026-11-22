#include "gtest/gtest.h"
#include "../../support/test_metadata.hpp"

#include "MayaFlux/IO/Composite/DescribedRecords.hpp"
#include "MayaFlux/Kakshya/NDData/CompositeControl.hpp"
#include "MayaFlux/Kinesis/Spatial/Bounds.hpp"
#include "MayaFlux/Transitive/Facet/ControlValueStore.hpp"

#include <glm/gtc/quaternion.hpp>

#include "../../transitive/control_store_fixtures.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Kakshya::CompositeArray;
    using Kakshya::CompositeInsertion;
    using Nexus::Quantity;
    using QuantityStore = ControlValueStore<Quantity>;

    template <typename T>
    CompositeArray written_row(const T& value)
    {
        CompositeArray rows(Kakshya::described_composite_layout<T>());
        CompositeInsertion insertion(rows);
        std::string error;
        EXPECT_TRUE(Kakshya::write_described(insertion, insertion.append(), value, error)) << error;
        return rows;
    }

    template <typename T>
    bool overlaid(const CompositeArray& rows, T& target, std::string& error)
    {
        const auto row = rows.at(0);
        return row && Kakshya::overlay_described(*row, target, error);
    }

    TEST(CompositeControlTest, ASlotMemberIsOneDynamicField)
    {
        const CompositeArray rows(Kakshya::described_composite_layout<StoreBloomTuning>());
        const auto& fields = rows.layout().fields();

        ASSERT_EQ(fields.size(), 2U);
        EXPECT_EQ(fields[0].name, "rate");
        EXPECT_EQ(fields[0].type, std::type_index(typeid(Kakshya::detail::CompositeDynamic)));
        EXPECT_EQ(fields[1].name, "angle");
        EXPECT_EQ(fields[1].type, std::type_index(typeid(Kakshya::detail::CompositeDynamic)));
    }

    TEST(CompositeControlTest, ANumberIsARealAndAnEmptyMemberIsAbsent)
    {
        StoreBloomTuning source;
        source.rate = ControlValue(0.5);

        const CompositeArray rows = written_row(source);
        const auto row = rows.at(0);

        ASSERT_TRUE(row.has_value());
        EXPECT_TRUE(row->has("rate"));
        EXPECT_FALSE(row->has("angle"));
        const auto cell = row->value("rate");
        ASSERT_TRUE(cell.has_value());
        ASSERT_TRUE(std::holds_alternative<double>(*cell));
        EXPECT_DOUBLE_EQ(std::get<double>(*cell), 0.5);
    }

    TEST(CompositeControlTest, ANumberReadsBackAsAPendingValueAndAnAbsentMemberStaysEmpty)
    {
        StoreBloomTuning source;
        source.rate = ControlValue(0.5);
        const CompositeArray rows = written_row(source);

        StoreBloomTuning target;
        std::string error;
        ASSERT_TRUE(overlaid(rows, target, error)) << error;

        EXPECT_TRUE(target.rate.due());
        EXPECT_DOUBLE_EQ(target.rate.value(), 0.5);
        EXPECT_FALSE(target.angle.has_value());
    }

    TEST(CompositeControlTest, AVectorIsTextOfItsNumbers)
    {
        StoreBloomTuning source;
        source.angle = ControlValue(glm::vec3(1.0F, 2.5F, -3.0F));

        const CompositeArray rows = written_row(source);
        const auto cell = rows.at(0)->value("angle");

        ASSERT_TRUE(cell.has_value());
        ASSERT_TRUE(std::holds_alternative<std::string_view>(*cell));
        EXPECT_EQ(std::get<std::string_view>(*cell), "1 2.5 -3");
    }

    TEST(CompositeControlTest, EveryWidthRoundTrips)
    {
        StoreBloomTuning source;
        source.rate = ControlValue(glm::vec2(1.0F, 2.0F));
        source.angle = ControlValue(glm::vec4(1.0F, 2.0F, 3.0F, 4.0F));
        CompositeArray rows = written_row(source);
        StoreBloomTuning target;
        std::string error;
        ASSERT_TRUE(overlaid(rows, target, error)) << error;
        EXPECT_EQ(target.rate.width(), 2U);
        EXPECT_EQ(target.rate.value<glm::vec2>(), glm::vec2(1.0F, 2.0F));
        EXPECT_EQ(target.angle.width(), 4U);
        EXPECT_EQ(target.angle.value<glm::vec4>(), glm::vec4(1.0F, 2.0F, 3.0F, 4.0F));

        StoreBloomTuning boxed;
        boxed.rate = ControlValue(Kinesis::AABB3D { .min = { -1.0F, -2.0F, -3.0F }, .max = { 4.0F, 5.0F, 6.0F } });
        rows = written_row(boxed);
        StoreBloomTuning boxed_target;
        ASSERT_TRUE(overlaid(rows, boxed_target, error)) << error;
        EXPECT_EQ(boxed_target.rate.width(), 6U);
        EXPECT_EQ(boxed_target.rate.value<Kinesis::AABB3D>().min, glm::vec3(-1.0F, -2.0F, -3.0F));
        EXPECT_EQ(boxed_target.rate.value<Kinesis::AABB3D>().max, glm::vec3(4.0F, 5.0F, 6.0F));
    }

    TEST(CompositeControlTest, ABoolAndAnEnumTravelAsNumbers)
    {
        StoreBloomTuning source;
        source.rate = ControlValue(true);
        source.angle = ControlValue(2);
        const CompositeArray rows = written_row(source);

        StoreBloomTuning target;
        std::string error;
        ASSERT_TRUE(overlaid(rows, target, error)) << error;

        EXPECT_TRUE(target.rate.value<bool>());
        EXPECT_EQ(target.angle.value<int>(), 2);
    }

    TEST(CompositeControlTest, AQuaternionTravelsAsFourNumbers)
    {
        StoreQuantityTuning source;
        source.orientation = glm::quat(0.5F, 0.1F, 0.2F, 0.3F);
        const CompositeArray rows = written_row(source);

        StoreQuantityTuning target;
        std::string error;
        ASSERT_TRUE(overlaid(rows, target, error)) << error;

        EXPECT_EQ(target.orientation.width(), 4U);
        EXPECT_FLOAT_EQ(target.orientation.value<glm::quat>().w, 0.5F);
        EXPECT_FLOAT_EQ(target.orientation.value<glm::quat>().x, 0.1F);
        EXPECT_FLOAT_EQ(target.orientation.value<glm::quat>().y, 0.2F);
        EXPECT_FLOAT_EQ(target.orientation.value<glm::quat>().z, 0.3F);
    }

    TEST(CompositeControlTest, AnAbsentMemberLeavesWhatTheTargetHolds)
    {
        StoreBloomTuning source;
        source.angle = ControlValue(0.25);
        const CompositeArray rows = written_row(source);

        StoreBloomTuning target;
        target.rate = ControlValue(0.9);
        std::string error;
        ASSERT_TRUE(overlaid(rows, target, error)) << error;

        EXPECT_DOUBLE_EQ(target.rate.value(), 0.9);
        EXPECT_DOUBLE_EQ(target.angle.value(), 0.25);
    }

    TEST(CompositeControlTest, AnUnsetSlotIsAPresentNullAndEmptiesTheTarget)
    {
        StoreQuantityTuning source;
        source.position = Quantity::unset();
        const CompositeArray rows = written_row(source);
        const auto cell = rows.at(0)->value("position");

        ASSERT_TRUE(cell.has_value());
        EXPECT_TRUE(std::holds_alternative<std::monostate>(*cell));

        StoreQuantityTuning target;
        target.position = glm::vec3(1.0F, 2.0F, 3.0F);
        target.intensity = 0.5;
        std::string error;
        ASSERT_TRUE(overlaid(rows, target, error)) << error;

        EXPECT_FALSE(target.position.has_value());
        EXPECT_DOUBLE_EQ(target.intensity.value(), 0.5);
    }

    TEST(CompositeControlTest, ANullForASlotThatCannotBeUnsetLeavesTheMemberAlone)
    {
        CompositeArray rows(Kakshya::described_composite_layout<StoreBloomTuning>());
        CompositeInsertion insertion(rows);
        const size_t row = insertion.append();
        ASSERT_TRUE(insertion.set_null(row, "rate"));

        StoreBloomTuning target;
        target.rate = ControlValue(2.0);
        std::string error;
        ASSERT_TRUE(overlaid(rows, target, error)) << error;

        EXPECT_DOUBLE_EQ(target.rate.value(), 2.0);
    }

    TEST(CompositeControlTest, AnIntegerCellReadsAsANumber)
    {
        CompositeArray rows(Kakshya::described_composite_layout<StoreBloomTuning>());
        CompositeInsertion insertion(rows);
        const size_t row = insertion.append();
        ASSERT_TRUE(insertion.set(row, "rate", int64_t { 3 }));

        StoreBloomTuning target;
        std::string error;
        ASSERT_TRUE(overlaid(rows, target, error)) << error;

        EXPECT_DOUBLE_EQ(target.rate.value(), 3.0);
    }

    TEST(CompositeControlTest, TextThatIsNotNumbersIsRefusedWithAReason)
    {
        CompositeArray rows(Kakshya::described_composite_layout<StoreBloomTuning>());
        CompositeInsertion insertion(rows);
        const size_t row = insertion.append();
        ASSERT_TRUE(insertion.set_text(row, "angle", "not numbers"));

        StoreBloomTuning target;
        std::string error;

        EXPECT_FALSE(overlaid(rows, target, error));
        EXPECT_FALSE(error.empty());
        EXPECT_FALSE(target.angle.has_value());
    }

    TEST(CompositeControlTest, ABlobCellIsRefused)
    {
        CompositeArray rows(Kakshya::described_composite_layout<StoreBloomTuning>());
        CompositeInsertion insertion(rows);
        const size_t row = insertion.append();
        const std::array<uint8_t, 2> bytes { 1, 2 };
        ASSERT_TRUE(insertion.set_blob(row, "rate", bytes));

        StoreBloomTuning target;
        std::string error;

        EXPECT_FALSE(overlaid(rows, target, error));
        EXPECT_FALSE(error.empty());
    }

    TEST(CompositeControlTest, ATextSlotIsAStringFieldAndAnEmptyOneIsAbsent)
    {
        StoreMixedTuning source;
        source.gain = ControlValue(0.5);
        source.label.set(std::string("lead"));
        const CompositeArray rows = written_row(source);

        EXPECT_EQ(rows.layout().fields()[1].type, std::type_index(typeid(std::string)));
        const auto stored = rows.at(0)->text("label");
        ASSERT_TRUE(stored.has_value());
        EXPECT_EQ(*stored, "lead");

        StoreMixedTuning target;
        std::string error;
        ASSERT_TRUE(overlaid(rows, target, error)) << error;
        ASSERT_TRUE(target.label.has_value());
        EXPECT_EQ(*target.label.value(), "lead");
        EXPECT_TRUE(target.label.due());
        EXPECT_DOUBLE_EQ(target.gain.value(), 0.5);

        StoreMixedTuning gain_only;
        gain_only.gain = ControlValue(1.0);
        const CompositeArray gain_rows = written_row(gain_only);
        EXPECT_FALSE(gain_rows.at(0)->has("label"));
    }

    TEST(CompositeControlTest, ARowReadBackIsAPatchForAStore)
    {
        QuantityStore from;
        auto& held = from.declare<StoreQuantityTuning>();
        held.position = glm::vec3(1.0F, 2.0F, 3.0F);
        held.intensity = 0.75;
        const CompositeArray rows = written_row(from.snapshot<StoreQuantityTuning>());

        QuantityStore to;
        auto& live = to.declare<StoreQuantityTuning>();
        live.orientation = glm::quat(1.0F, 0.0F, 0.0F, 0.0F);
        StoreQuantityTuning decoded;
        std::string error;
        ASSERT_TRUE(overlaid(rows, decoded, error)) << error;
        to.merge(decoded);

        EXPECT_EQ(live.position.value<glm::vec3>(), glm::vec3(1.0F, 2.0F, 3.0F));
        EXPECT_DOUBLE_EQ(live.intensity.value(), 0.75);
        EXPECT_TRUE(live.orientation.has_value());
    }

    TEST(CompositeControlTest, SeveralEntitiesAreSeveralRowsWithDifferentPresentFields)
    {
        CompositeArray rows(Kakshya::described_composite_layout<StoreQuantityTuning>());
        CompositeInsertion insertion(rows);
        std::string error;

        StoreQuantityTuning first;
        first.position = glm::vec3(1.0F);
        StoreQuantityTuning second;
        second.intensity = 0.5;
        second.position = Quantity::unset();
        ASSERT_TRUE(Kakshya::write_described(insertion, insertion.append(), first, error)) << error;
        ASSERT_TRUE(Kakshya::write_described(insertion, insertion.append(), second, error)) << error;

        const auto records = IO::records_of<StoreQuantityTuning>(rows, error);

        ASSERT_TRUE(records.has_value()) << error;
        ASSERT_EQ(records->size(), 2U);
        EXPECT_EQ((*records)[0].position.value<glm::vec3>(), glm::vec3(1.0F));
        EXPECT_FALSE((*records)[0].intensity.has_value());
        EXPECT_FALSE((*records)[1].position.has_value());
        EXPECT_TRUE((*records)[1].position.clear_requested());
        EXPECT_DOUBLE_EQ((*records)[1].intensity.value(), 0.5);
    }

}
}
