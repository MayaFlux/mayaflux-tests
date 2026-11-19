#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Core/Input/HIDDescriptor.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Core::HIDInputField;
    using Core::HIDReportLayout;
    using Core::hid_input_report_size;
    using Core::parse_hid_descriptor;
    using Core::read_hid_field;

    constexpr uint16_t k_page_generic = 0x01;
    constexpr uint16_t k_page_button = 0x09;

    std::vector<uint8_t> pad_descriptor(bool report_ids = false)
    {
        std::vector<uint8_t> descriptor { 0x05, 0x01, 0x09, 0x05, 0xA1, 0x01 };
        if (report_ids) {
            descriptor.insert(descriptor.end(), { 0x85, 0x01 });
        }
        descriptor.insert(descriptor.end(), {
                                                0x09, 0x30, 0x09, 0x31,
                                                0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x02, 0x81, 0x02,
                                                0x09, 0x39,
                                                0x15, 0x00, 0x25, 0x07, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42,
                                                0x75, 0x04, 0x95, 0x01, 0x81, 0x03,
                                                0x05, 0x09, 0x19, 0x01, 0x29, 0x08,
                                                0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
                                                0xC0 });
        return descriptor;
    }

    const HIDInputField* find_field(const HIDReportLayout& layout, uint16_t page, uint16_t usage)
    {
        for (const auto& field : layout.fields) {
            if (field.usage_page == page && field.usage == usage) {
                return &field;
            }
        }
        return nullptr;
    }

    TEST(HIDDescriptorTest, PadDescriptorYieldsOneFieldPerUsage)
    {
        const auto layout = parse_hid_descriptor(pad_descriptor());

        EXPECT_EQ(layout.fields.size(), 11U);
        EXPECT_FALSE(layout.uses_report_ids);
    }

    TEST(HIDDescriptorTest, FieldsSitAtCumulativeBitOffsets)
    {
        const auto layout = parse_hid_descriptor(pad_descriptor());

        const auto* x = find_field(layout, k_page_generic, 0x30);
        const auto* y = find_field(layout, k_page_generic, 0x31);
        const auto* hat = find_field(layout, k_page_generic, 0x39);
        const auto* first = find_field(layout, k_page_button, 1);
        const auto* last = find_field(layout, k_page_button, 8);

        ASSERT_NE(x, nullptr);
        ASSERT_NE(y, nullptr);
        ASSERT_NE(hat, nullptr);
        ASSERT_NE(first, nullptr);
        ASSERT_NE(last, nullptr);

        EXPECT_EQ(x->bit_offset, 0U);
        EXPECT_EQ(x->bit_size, 8U);
        EXPECT_EQ(y->bit_offset, 8U);
        EXPECT_EQ(hat->bit_offset, 16U);
        EXPECT_EQ(hat->bit_size, 4U);
        EXPECT_EQ(first->bit_offset, 24U);
        EXPECT_EQ(first->bit_size, 1U);
        EXPECT_EQ(last->bit_offset, 31U);
    }

    TEST(HIDDescriptorTest, LogicalRangesFollowTheDescriptor)
    {
        const auto layout = parse_hid_descriptor(pad_descriptor());

        const auto* x = find_field(layout, k_page_generic, 0x30);
        const auto* hat = find_field(layout, k_page_generic, 0x39);
        const auto* button = find_field(layout, k_page_button, 3);

        ASSERT_NE(x, nullptr);
        ASSERT_NE(hat, nullptr);
        ASSERT_NE(button, nullptr);

        EXPECT_EQ(x->logical_min, 0);
        EXPECT_EQ(x->logical_max, 255);
        EXPECT_EQ(hat->logical_max, 7);
        EXPECT_EQ(button->logical_max, 1);
    }

    TEST(HIDDescriptorTest, AbsoluteVariableFieldsAreNotFlagged)
    {
        const auto layout = parse_hid_descriptor(pad_descriptor());

        for (const auto& field : layout.fields) {
            EXPECT_FALSE(field.is_array);
            EXPECT_FALSE(field.is_relative);
        }
    }

    TEST(HIDDescriptorTest, RelativeFieldsAreFlagged)
    {
        const std::vector<uint8_t> mouse {
            0x05, 0x01, 0x09, 0x02, 0xA1, 0x01,
            0x09, 0x30, 0x09, 0x31,
            0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06,
            0xC0
        };

        const auto layout = parse_hid_descriptor(mouse);

        ASSERT_EQ(layout.fields.size(), 2U);
        for (const auto& field : layout.fields) {
            EXPECT_TRUE(field.is_relative);
            EXPECT_FALSE(field.is_array);
            EXPECT_EQ(field.logical_min, -127);
        }
    }

    TEST(HIDDescriptorTest, ArrayFieldsAreFlagged)
    {
        const std::vector<uint8_t> keyboard {
            0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,
            0x05, 0x07, 0x19, 0x00, 0x29, 0x65,
            0x15, 0x00, 0x25, 0x65, 0x75, 0x08, 0x95, 0x06, 0x81, 0x00,
            0xC0
        };

        const auto layout = parse_hid_descriptor(keyboard);

        ASSERT_FALSE(layout.fields.empty());
        for (const auto& field : layout.fields) {
            EXPECT_TRUE(field.is_array);
        }
    }

    TEST(HIDDescriptorTest, ReportSizeCoversTheFurthestField)
    {
        const auto layout = parse_hid_descriptor(pad_descriptor());

        EXPECT_EQ(hid_input_report_size(layout), 4U);
    }

    TEST(HIDDescriptorTest, ReportSizeCountsTheReportIdByte)
    {
        const auto layout = parse_hid_descriptor(pad_descriptor(true));

        EXPECT_TRUE(layout.uses_report_ids);
        EXPECT_EQ(hid_input_report_size(layout), 5U);
    }

    TEST(HIDDescriptorTest, ReportSizeOfAnEmptyLayoutIsZero)
    {
        EXPECT_EQ(hid_input_report_size(HIDReportLayout {}), 0U);
    }

    TEST(HIDDescriptorTest, ReadFieldExtractsBitsAtTheFieldOffset)
    {
        const auto layout = parse_hid_descriptor(pad_descriptor());
        const std::vector<uint8_t> report { 0x80, 0x10, 0x25, 0xA5 };

        const auto read = [&](uint16_t page, uint16_t usage) {
            const auto* field = find_field(layout, page, usage);
            return field ? read_hid_field(report, *field) : -1;
        };

        EXPECT_EQ(read(k_page_generic, 0x30), 128);
        EXPECT_EQ(read(k_page_generic, 0x31), 16);
        EXPECT_EQ(read(k_page_generic, 0x39), 5);
        EXPECT_EQ(read(k_page_button, 1), 1);
        EXPECT_EQ(read(k_page_button, 2), 0);
        EXPECT_EQ(read(k_page_button, 3), 1);
    }

    TEST(HIDDescriptorTest, ReadFieldPastTheEndOfAReportReadsZero)
    {
        const auto layout = parse_hid_descriptor(pad_descriptor());
        const std::vector<uint8_t> report { 0x80 };

        const auto* y = find_field(layout, k_page_generic, 0x31);
        ASSERT_NE(y, nullptr);

        EXPECT_EQ(read_hid_field(report, *y), 0);
    }

} // namespace
} // namespace MayaFlux::Test
