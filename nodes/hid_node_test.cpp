#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Nodes/Input/HIDNode.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Core::HIDReportLayout;
    using Core::InputType;
    using Core::InputValue;
    using Nodes::Input::HIDConfig;
    using Nodes::Input::HIDContext;
    using Nodes::Input::HIDControl;
    using Nodes::Input::HIDControlSpec;
    using Nodes::Input::HIDControlTuning;
    using Nodes::Input::HIDNode;
    using Nodes::Input::HIDRest;
    using Nodes::Input::HIDSnapshot;
    using Nodes::Input::HIDStickTuning;
    using Nodes::Input::InputTuning;
    using Nodes::Input::SmoothingMode;

    std::vector<uint8_t> pad_descriptor(bool swapped = false)
    {
        const uint8_t first = swapped ? 0x31 : 0x30;
        const uint8_t second = swapped ? 0x30 : 0x31;

        return {
            0x05, 0x01, 0x09, 0x05, 0xA1, 0x01,
            0x09, first, 0x09, second,
            0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x02, 0x81, 0x02,
            0x09, 0x39,
            0x15, 0x00, 0x25, 0x07, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42,
            0x75, 0x04, 0x95, 0x01, 0x81, 0x03,
            0x05, 0x09, 0x19, 0x01, 0x29, 0x08,
            0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
            0xC0
        };
    }

    std::shared_ptr<const HIDReportLayout> pad_layout(bool swapped = false)
    {
        return std::make_shared<const HIDReportLayout>(
            Core::parse_hid_descriptor(pad_descriptor(swapped)));
    }

    std::vector<uint8_t> pad_report(uint8_t x, uint8_t y, uint8_t hat = 0, uint8_t buttons = 0)
    {
        return { x, y, static_cast<uint8_t>(hat & 0x0F), buttons };
    }

    InputValue frame_of(const std::shared_ptr<const HIDReportLayout>& layout,
        std::vector<uint8_t> report, uint32_t device = 1)
    {
        std::vector<int32_t> raw;
        raw.reserve(layout->fields.size());
        for (const auto& field : layout->fields) {
            raw.push_back(Core::read_hid_field(report, field));
        }
        return InputValue::make_hid(layout, std::move(raw), std::move(report), device);
    }

    HIDConfig unsmoothed(HIDConfig config)
    {
        config.smoothing = SmoothingMode::NONE;
        return config;
    }

    constexpr HIDControl k_x = HIDControl::x();
    constexpr HIDControl k_y = HIDControl::y();
    constexpr HIDControl k_hat = HIDControl::hat();

    TEST(HIDNodeTest, DeviceNodeIsQuietUntilAReportArrives)
    {
        HIDNode node(HIDConfig::device());

        EXPECT_EQ(node.get_snapshot(), nullptr);
        EXPECT_TRUE(node.controls().empty());
        EXPECT_DOUBLE_EQ(node.value(k_x), 0.0);
        EXPECT_FALSE(node.hat(k_hat).has_value());
    }

    TEST(HIDNodeTest, AReportPublishesASnapshotAlignedToTheLayout)
    {
        const auto layout = pad_layout();
        HIDNode node(HIDConfig::device());

        node.process_input(frame_of(layout, pad_report(10, 20, 3, 0x01)));

        const auto snapshot = node.get_snapshot();
        ASSERT_NE(snapshot, nullptr);
        EXPECT_EQ(snapshot->layout, layout);
        EXPECT_EQ(snapshot->raw.size(), layout->fields.size());
        EXPECT_EQ(snapshot->values.size(), layout->fields.size());
        EXPECT_EQ(snapshot->report, pad_report(10, 20, 3, 0x01));
    }

    TEST(HIDNodeTest, ControlNodeOutputsItsControlAsAUnitValue)
    {
        const auto layout = pad_layout();
        HIDNode node(unsmoothed(HIDConfig::control(k_x)));

        node.process_input(frame_of(layout, pad_report(255, 0)));
        EXPECT_DOUBLE_EQ(node.get_target_value(), 1.0);

        node.process_input(frame_of(layout, pad_report(0, 0)));
        EXPECT_DOUBLE_EQ(node.get_target_value(), 0.0);
    }

    TEST(HIDNodeTest, ButtonControlDisablesSmoothingAndAxisKeepsIt)
    {
        HIDNode button(HIDConfig::control(HIDControl::button(1)));
        HIDNode axis(HIDConfig::control(k_x));

        EXPECT_EQ(button.get_config().smoothing, SmoothingMode::NONE);
        EXPECT_EQ(axis.get_config().smoothing, SmoothingMode::EXPONENTIAL);
    }

    TEST(HIDNodeTest, DefaultSpecMapsTheRangeToUnitWithoutADeadzone)
    {
        const auto layout = pad_layout();
        HIDNode node(HIDConfig::device());

        node.process_input(frame_of(layout, pad_report(51, 255)));

        EXPECT_NEAR(node.value(k_x), 0.2, 1e-9);
        EXPECT_DOUBLE_EQ(node.value(k_y), 1.0);
    }

    TEST(HIDNodeTest, CenteredSpecMapsTheRangeToPlusMinusOne)
    {
        const auto layout = pad_layout();
        HIDNode node(HIDConfig::device().with_spec(k_x, HIDControlSpec::centered()));

        node.process_input(frame_of(layout, pad_report(0, 0)));
        EXPECT_DOUBLE_EQ(node.value(k_x), -1.0);

        node.process_input(frame_of(layout, pad_report(255, 0)));
        EXPECT_DOUBLE_EQ(node.value(k_x), 1.0);
    }

    TEST(HIDNodeTest, DeadzoneZeroesTheNeighbourhoodOfRest)
    {
        const auto layout = pad_layout();
        HIDNode node(HIDConfig::device().with_spec(k_x, HIDControlSpec::centered(0.1)));

        node.process_input(frame_of(layout, pad_report(128, 0)));
        EXPECT_DOUBLE_EQ(node.value(k_x), 0.0);

        node.process_input(frame_of(layout, pad_report(255, 0)));
        EXPECT_DOUBLE_EQ(node.value(k_x), 1.0);
    }

    TEST(HIDNodeTest, InvertedSpecFlipsTheOutput)
    {
        const auto layout = pad_layout();
        HIDNode node(HIDConfig::device().with_spec(k_x, HIDControlSpec {}.inverted()));

        node.process_input(frame_of(layout, pad_report(255, 0)));
        EXPECT_DOUBLE_EQ(node.value(k_x), 0.0);

        node.process_input(frame_of(layout, pad_report(0, 0)));
        EXPECT_DOUBLE_EQ(node.value(k_x), 1.0);
    }

    TEST(HIDNodeTest, RangeOverrideReplacesTheLogicalRange)
    {
        const auto layout = pad_layout();
        HIDNode node(HIDConfig::device().with_spec(k_x, HIDControlSpec {}.with_range(0, 127)));

        node.process_input(frame_of(layout, pad_report(127, 0)));
        EXPECT_DOUBLE_EQ(node.value(k_x), 1.0);

        node.process_input(frame_of(layout, pad_report(255, 0)));
        EXPECT_DOUBLE_EQ(node.value(k_x), 1.0);
    }

    TEST(HIDNodeTest, StickDeadzoneIsRadialAndFullDeflectionPassesThrough)
    {
        const auto layout = pad_layout();
        HIDNode node(HIDConfig::device().with_stick(k_x, k_y, 0.5));

        node.process_input(frame_of(layout, pad_report(160, 128)));
        EXPECT_DOUBLE_EQ(node.value(k_x), 0.0);
        EXPECT_DOUBLE_EQ(node.value(k_y), 0.0);

        node.process_input(frame_of(layout, pad_report(255, 128)));
        EXPECT_NEAR(node.value(k_x), 1.0, 1e-3);
        EXPECT_NEAR(node.value(k_y), 0.0, 1e-2);
    }

    TEST(HIDNodeTest, HatReportsADirectionOrNothing)
    {
        const auto layout = pad_layout();
        HIDNode node(HIDConfig::device());

        node.process_input(frame_of(layout, pad_report(0, 0, 3)));
        const auto direction = node.hat(k_hat);
        ASSERT_TRUE(direction.has_value());
        EXPECT_EQ(*direction, 3);

        node.process_input(frame_of(layout, pad_report(0, 0, 8)));
        EXPECT_FALSE(node.hat(k_hat).has_value());
        EXPECT_DOUBLE_EQ(node.value(k_hat), 0.0);
    }

    TEST(HIDNodeTest, PressAndReleaseFireOnEdgesOnly)
    {
        const auto layout = pad_layout();
        HIDNode node(HIDConfig::device());
        int pressed = 0;
        int released = 0;

        node.on_press(HIDControl::button(1), [&](HIDContext&) { ++pressed; });
        node.on_release(HIDControl::button(1), [&](HIDContext&) { ++released; });

        node.process_input(frame_of(layout, pad_report(0, 0, 0, 0x00)));
        node.process_input(frame_of(layout, pad_report(0, 0, 0, 0x01)));
        node.process_input(frame_of(layout, pad_report(0, 0, 0, 0x01)));
        node.process_input(frame_of(layout, pad_report(0, 0, 0, 0x00)));

        EXPECT_EQ(pressed, 1);
        EXPECT_EQ(released, 1);
    }

    TEST(HIDNodeTest, TheFirstReportDoesNotFireEdges)
    {
        const auto layout = pad_layout();
        HIDNode node(HIDConfig::device());
        int pressed = 0;

        node.on_press(HIDControl::button(1), [&](HIDContext&) { ++pressed; });
        node.process_input(frame_of(layout, pad_report(0, 0, 0, 0x01)));

        EXPECT_EQ(pressed, 0);
    }

    TEST(HIDNodeTest, ChangeFiresOnlyWhenTheOutputMoves)
    {
        const auto layout = pad_layout();
        HIDNode node(HIDConfig::device());
        int changes = 0;

        node.on_change(k_x, [&](HIDContext&) { ++changes; });

        node.process_input(frame_of(layout, pad_report(0, 0)));
        node.process_input(frame_of(layout, pad_report(0, 255)));
        node.process_input(frame_of(layout, pad_report(255, 255)));

        EXPECT_EQ(changes, 1);
    }

    TEST(HIDNodeTest, FrameFiresOnEveryReport)
    {
        const auto layout = pad_layout();
        HIDNode node(HIDConfig::device());
        int frames = 0;

        node.on_frame([&](HIDContext&) { ++frames; });

        for (int i = 0; i < 3; ++i) {
            node.process_input(frame_of(layout, pad_report(0, 0)));
        }

        EXPECT_EQ(frames, 3);
    }

    TEST(HIDNodeTest, HatEventFiresWhenTheDirectionChanges)
    {
        const auto layout = pad_layout();
        HIDNode node(HIDConfig::device());
        int changes = 0;

        node.on_hat(k_hat, [&](HIDContext&) { ++changes; });

        node.process_input(frame_of(layout, pad_report(0, 0, 0)));
        node.process_input(frame_of(layout, pad_report(0, 0, 3)));
        node.process_input(frame_of(layout, pad_report(0, 0, 3)));
        node.process_input(frame_of(layout, pad_report(0, 0, 8)));

        EXPECT_EQ(changes, 2);
    }

    TEST(HIDNodeTest, StickEventFiresWhenEitherAxisMoves)
    {
        const auto layout = pad_layout();
        HIDNode node(HIDConfig::device().with_stick(k_x, k_y));
        int moves = 0;
        double last_x = 0.0;
        double last_y = 0.0;

        node.on_stick(k_x, [&](HIDContext& ctx) {
            ++moves;
            last_x = ctx.value;
            last_y = ctx.y;
        });

        node.process_input(frame_of(layout, pad_report(128, 128)));
        node.process_input(frame_of(layout, pad_report(128, 128)));
        node.process_input(frame_of(layout, pad_report(128, 255)));

        EXPECT_EQ(moves, 1);
        EXPECT_NEAR(last_y, 1.0, 1e-3);
        EXPECT_NEAR(last_x, 0.0, 1e-2);
    }

    TEST(HIDNodeTest, EventContextCarriesTheControlAndTheSnapshot)
    {
        const auto layout = pad_layout();
        HIDNode node(HIDConfig::device());
        HIDControl seen;
        double value = -1.0;
        std::shared_ptr<const HIDSnapshot> snapshot;

        node.on_press(HIDControl::button(2), [&](HIDContext& ctx) {
            seen = ctx.control;
            value = ctx.value;
            snapshot = ctx.snapshot;
        });

        node.process_input(frame_of(layout, pad_report(0, 0, 0, 0x00)));
        node.process_input(frame_of(layout, pad_report(0, 0, 0, 0x02)));

        EXPECT_EQ(seen, HIDControl::button(2));
        EXPECT_DOUBLE_EQ(value, 1.0);
        ASSERT_NE(snapshot, nullptr);
        EXPECT_EQ(snapshot->layout, layout);
    }

    TEST(HIDNodeTest, SignalIsCreatedOnceAndFedByTheDeviceNode)
    {
        const auto layout = pad_layout();
        HIDNode node(HIDConfig::device());

        const auto signal = node.signal(k_x);
        EXPECT_EQ(signal, node.signal(k_x));

        node.process_input(frame_of(layout, pad_report(255, 0)));

        EXPECT_DOUBLE_EQ(signal->get_target_value(), 1.0);
    }

    TEST(HIDNodeTest, SignalOfAButtonDisablesSmoothing)
    {
        HIDNode node(HIDConfig::device());

        EXPECT_EQ(node.signal(HIDControl::button(1))->get_config().smoothing, SmoothingMode::NONE);
        EXPECT_EQ(node.signal(k_x)->get_config().smoothing, SmoothingMode::EXPONENTIAL);
    }

    TEST(HIDNodeTest, SignalForAMissingControlStaysAtItsDefault)
    {
        const auto layout = pad_layout();
        HIDNode node(HIDConfig::device());

        const auto signal = node.signal(HIDControl::button(20));
        node.process_input(frame_of(layout, pad_report(255, 255, 0, 0xFF)));

        EXPECT_DOUBLE_EQ(signal->get_target_value(), 0.0);
        EXPECT_FALSE(signal->has_new_input());
    }

    TEST(HIDNodeTest, CustomDerivationReadsTheReportBytes)
    {
        const auto layout = pad_layout();
        HIDNode node(unsmoothed(HIDConfig::custom([](const HIDSnapshot& snapshot) {
            return snapshot.report.size() > 1 ? snapshot.report[1] / 255.0 : 0.0;
        })));

        node.process_input(frame_of(layout, pad_report(0, 51)));

        EXPECT_NEAR(node.get_target_value(), 0.2, 1e-9);
    }

    TEST(HIDNodeTest, CustomWorksOnADeviceWithoutADescriptor)
    {
        const auto empty = std::make_shared<const HIDReportLayout>();
        HIDNode node(unsmoothed(HIDConfig::custom([](const HIDSnapshot& snapshot) {
            return static_cast<double>(snapshot.report.at(0));
        })));

        node.process_input(frame_of(empty, { 10, 20 }));

        EXPECT_DOUBLE_EQ(node.get_target_value(), 10.0);
        ASSERT_NE(node.get_snapshot(), nullptr);
        EXPECT_TRUE(node.get_snapshot()->raw.empty());
    }

    TEST(HIDNodeTest, DerivedControlCanReadAnEarlierOne)
    {
        const auto layout = pad_layout();
        HIDNode node(HIDConfig::device()
                         .derive(1, [](const HIDSnapshot&) { return 2.0; })
                         .derive(2, [](const HIDSnapshot& snapshot) {
                             return snapshot.value(HIDControl::derived(1)).value_or(0.0) * 3.0;
                         }));

        node.process_input(frame_of(layout, pad_report(0, 0)));

        EXPECT_DOUBLE_EQ(node.value(HIDControl::derived(1)), 2.0);
        EXPECT_DOUBLE_EQ(node.value(HIDControl::derived(2)), 6.0);
    }

    TEST(HIDNodeTest, DerivedControlIsAddressableByEvents)
    {
        const auto layout = pad_layout();
        HIDNode node(HIDConfig::device().derive(5, [](const HIDSnapshot& snapshot) {
            return (snapshot.report.at(3) & 0x01) != 0 ? 1.0 : 0.0;
        }));
        int pressed = 0;

        node.on_press(HIDControl::derived(5), [&](HIDContext&) { ++pressed; });

        node.process_input(frame_of(layout, pad_report(0, 0, 0, 0x00)));
        node.process_input(frame_of(layout, pad_report(0, 0, 0, 0x01)));

        EXPECT_EQ(pressed, 1);
    }

    TEST(HIDNodeTest, WithAppendsAndTheOtherPrimaryWins)
    {
        const auto base = HIDConfig::device().with_spec(k_x, HIDControlSpec::centered());
        const auto profile = HIDConfig::control(k_y).with_stick(k_x, k_y, 0.1);

        const auto merged = base.with(profile);
        EXPECT_EQ(merged.specs.size(), 1U);
        EXPECT_EQ(merged.sticks.size(), 1U);
        ASSERT_TRUE(merged.primary.has_value());
        EXPECT_EQ(*merged.primary, k_y);

        const auto kept = profile.with(base);
        ASSERT_TRUE(kept.primary.has_value());
        EXPECT_EQ(*kept.primary, k_y);
    }

    TEST(HIDNodeTest, ControlsListsTheFieldsOfTheDevice)
    {
        const auto layout = pad_layout();
        HIDNode node(HIDConfig::device());

        node.process_input(frame_of(layout, pad_report(0, 0)));

        const auto controls = node.controls();
        ASSERT_EQ(controls.size(), 11U);
        EXPECT_EQ(controls.front().control, k_x);
        EXPECT_EQ(controls.front().logical_max, 255);
        EXPECT_FALSE(controls.front().relative);
    }

    TEST(HIDNodeTest, NonHidInputLeavesTheNodeUntouched)
    {
        HIDNode node(HIDConfig::device());

        node.process_input(InputValue::make_scalar(0.7, 1, InputType::HID));

        EXPECT_EQ(node.get_snapshot(), nullptr);
    }

    TEST(HIDNodeTest, ANewLayoutReResolvesControls)
    {
        HIDNode node(HIDConfig::device());
        const auto report = pad_report(10, 200);

        node.process_input(frame_of(pad_layout(false), report));
        EXPECT_NEAR(node.value(k_x), 10.0 / 255.0, 1e-9);
        EXPECT_NEAR(node.value(k_y), 200.0 / 255.0, 1e-9);

        node.process_input(frame_of(pad_layout(true), report));
        EXPECT_NEAR(node.value(k_x), 200.0 / 255.0, 1e-9);
        EXPECT_NEAR(node.value(k_y), 10.0 / 255.0, 1e-9);
    }

    TEST(HIDNodeTest, MissingControlsAreNotFound)
    {
        const auto layout = pad_layout();
        HIDNode node(HIDConfig::device());

        node.process_input(frame_of(layout, pad_report(255, 255, 3, 0xFF)));

        EXPECT_DOUBLE_EQ(node.value(HIDControl::button(20)), 0.0);
        EXPECT_FALSE(node.hat(HIDControl::button(20)).has_value());
        EXPECT_FALSE(node.get_snapshot()->find(HIDControl::derived(9)).has_value());
    }

    TEST(HIDNodeTest, ControlKeyRoundTrips)
    {
        const HIDControl control { 0x09, 7, 2 };

        EXPECT_EQ(HIDControl::from_key(control.key()), control);
        EXPECT_NE(HIDControl::button(1).key(), HIDControl::derived(1).key());
    }

    TEST(HIDNodeTuningTest, AControlWithNoTuningIsConvertedByItsSpec)
    {
        const auto layout = pad_layout();
        HIDNode node(HIDConfig::device().with_spec(k_x, HIDControlSpec {}.with_deadzone(0.2)));

        node.process_input(frame_of(layout, pad_report(153, 0)));

        EXPECT_NEAR(node.value(k_x), 0.5, 1e-9);
        EXPECT_FALSE(node.get_tuning(k_x).deadzone.has_value());
    }

    TEST(HIDNodeTuningTest, ATunedInvertAppliesAndKeepsTheSpecsDeadzone)
    {
        const auto layout = pad_layout();
        HIDNode node(HIDConfig::device().with_spec(k_x, HIDControlSpec {}.with_deadzone(0.2)));
        HIDControlTuning tuning;
        tuning.invert = ControlValue(true);

        node.set_tuning(k_x, tuning);
        node.process_input(frame_of(layout, pad_report(153, 0)));

        EXPECT_NEAR(node.value(k_x), 0.25, 1e-9);
    }

    TEST(HIDNodeTuningTest, ATunedDeadzoneReplacesTheSpecsDeadzone)
    {
        const auto layout = pad_layout();
        HIDNode node(HIDConfig::device().with_spec(k_x, HIDControlSpec {}.with_deadzone(0.2)));
        HIDControlTuning tuning;
        tuning.deadzone = ControlValue(0.5);

        node.set_tuning(k_x, tuning);
        node.process_input(frame_of(layout, pad_report(204, 0)));

        EXPECT_NEAR(node.value(k_x), 0.6, 1e-9);
    }

    TEST(HIDNodeTuningTest, ATunedRestMakesAControlCentered)
    {
        const auto layout = pad_layout();
        HIDNode node(HIDConfig::device());

        node.process_input(frame_of(layout, pad_report(0, 0)));
        EXPECT_NEAR(node.value(k_x), 0.0, 1e-9);

        HIDControlTuning tuning;
        tuning.rest = ControlValue(HIDRest::CENTER);
        node.set_tuning(k_x, tuning);
        node.process_input(frame_of(layout, pad_report(0, 0)));

        EXPECT_NEAR(node.value(k_x), -1.0, 1e-9);
    }

    TEST(HIDNodeTuningTest, ATunedRangeNeedsBothEnds)
    {
        const auto layout = pad_layout();
        HIDNode node(HIDConfig::device());
        HIDControlTuning low;
        low.range_min = ControlValue(0.0);

        node.set_tuning(k_x, low);
        node.process_input(frame_of(layout, pad_report(50, 0)));
        EXPECT_NEAR(node.value(k_x), 50.0 / 255.0, 1e-9);

        HIDControlTuning high;
        high.range_max = ControlValue(100.0);
        node.set_tuning(k_x, high);
        node.process_input(frame_of(layout, pad_report(50, 0)));

        EXPECT_NEAR(node.value(k_x), 0.5, 1e-9);
    }

    TEST(HIDNodeTuningTest, ATuningLeavesOtherControlsAlone)
    {
        const auto layout = pad_layout();
        HIDNode node(HIDConfig::device());
        HIDControlTuning tuning;
        tuning.invert = ControlValue(true);

        node.set_tuning(k_x, tuning);
        node.process_input(frame_of(layout, pad_report(51, 51)));

        EXPECT_NEAR(node.value(k_x), 0.8, 1e-9);
        EXPECT_NEAR(node.value(k_y), 0.2, 1e-9);
    }

    TEST(HIDNodeTuningTest, ATuningMergesWithWhatWasTunedBefore)
    {
        HIDNode node(HIDConfig::device());
        HIDControlTuning first;
        first.deadzone = ControlValue(0.3);
        HIDControlTuning second;
        second.invert = ControlValue(true);

        node.set_tuning(k_x, first);
        node.set_tuning(k_x, second);

        const auto tuning = node.get_tuning(k_x);
        ASSERT_TRUE(tuning.deadzone.has_value());
        EXPECT_DOUBLE_EQ(tuning.deadzone.value(), 0.3);
        EXPECT_TRUE(tuning.invert.value<bool>());
        EXPECT_FALSE(tuning.rest.has_value());
        EXPECT_FALSE(node.get_tuning(k_y).invert.has_value());
    }

    TEST(HIDNodeTuningTest, ATuningForAControlTheDeviceLacksIsKeptAndIgnored)
    {
        const auto layout = pad_layout();
        HIDNode node(HIDConfig::device());
        HIDControlTuning tuning;
        tuning.invert = ControlValue(true);

        node.set_tuning(HIDControl::button(20), tuning);
        node.process_input(frame_of(layout, pad_report(51, 0)));

        EXPECT_NEAR(node.value(k_x), 0.2, 1e-9);
        EXPECT_TRUE(node.get_tuning(HIDControl::button(20)).invert.has_value());
    }

    TEST(HIDNodeTuningTest, AStickDeadzoneCanBeTuned)
    {
        const auto layout = pad_layout();
        HIDNode node(HIDConfig::device().with_stick(k_x, k_y, 0.0));

        node.process_input(frame_of(layout, pad_report(255, 255)));
        EXPECT_GT(node.value(k_x), 0.5);

        HIDStickTuning tuning;
        tuning.deadzone = ControlValue(1.0);
        node.set_stick_tuning(k_x, tuning);
        node.process_input(frame_of(layout, pad_report(255, 255)));

        EXPECT_DOUBLE_EQ(node.value(k_x), 0.0);
        EXPECT_DOUBLE_EQ(node.value(k_y), 0.0);
        EXPECT_TRUE(node.get_stick_tuning(k_x).deadzone.has_value());
        EXPECT_FALSE(node.get_stick_tuning(k_y).deadzone.has_value());
    }

    TEST(HIDNodeTuningTest, ASignalNodeTakesItsOwnSmoothingTuning)
    {
        const auto layout = pad_layout();
        HIDNode node(HIDConfig::device());
        const auto signal = node.signal(k_x);
        InputTuning tuning;
        tuning.smoothing = ControlValue(SmoothingMode::NONE);

        signal->set_tuning(tuning);
        node.process_input(frame_of(layout, pad_report(255, 0)));

        EXPECT_DOUBLE_EQ(signal->get_current_value(), 1.0);
    }

} // namespace
} // namespace MayaFlux::Test
