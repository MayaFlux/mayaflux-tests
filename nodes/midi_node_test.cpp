#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Nodes/Input/MIDINode.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Core::InputType;
    using Core::InputValue;
    using Nodes::Input::MIDIConfig;
    using Nodes::Input::MIDIContext;
    using Nodes::Input::MIDIControl;
    using Nodes::Input::MIDIKind;
    using Nodes::Input::MIDINode;
    using Nodes::Input::SmoothingMode;

    constexpr uint8_t k_note_on = 0x90;
    constexpr uint8_t k_note_off = 0x80;
    constexpr uint8_t k_cc = 0xB0;
    constexpr uint8_t k_pressure = 0xD0;
    constexpr uint8_t k_program = 0xC0;
    constexpr uint8_t k_bend = 0xE0;

    void feed(MIDINode& node, uint8_t type, uint8_t channel, uint8_t data1, uint8_t data2)
    {
        node.process_input(InputValue::make_midi(static_cast<uint8_t>(type | channel), data1, data2, 1));
    }

    MIDIConfig unsmoothed(MIDIConfig config)
    {
        config.smoothing = SmoothingMode::NONE;
        return config;
    }

    TEST(MIDIControlTest, ANoteControlMatchesOnlyItsNumberAndChannel)
    {
        const auto any_note = MIDIControl::note();
        const auto middle_c = MIDIControl::note(60, 2);

        const auto on = InputValue::make_midi(0x92, 60, 100, 1).as_midi();
        const auto other_note = InputValue::make_midi(0x92, 61, 100, 1).as_midi();
        const auto other_channel = InputValue::make_midi(0x93, 60, 100, 1).as_midi();
        const auto off = InputValue::make_midi(0x82, 60, 0, 1).as_midi();

        EXPECT_TRUE(any_note.matches(on));
        EXPECT_TRUE(any_note.matches(other_channel));
        EXPECT_TRUE(middle_c.matches(on));
        EXPECT_TRUE(middle_c.matches(off));
        EXPECT_FALSE(middle_c.matches(other_note));
        EXPECT_FALSE(middle_c.matches(other_channel));
    }

    TEST(MIDIControlTest, KindsDoNotMatchEachOther)
    {
        const auto cc = InputValue::make_midi(0xB0, 60, 100, 1).as_midi();

        EXPECT_FALSE(MIDIControl::note(60).matches(cc));
        EXPECT_TRUE(MIDIControl::cc(60).matches(cc));
        EXPECT_FALSE(MIDIControl::pitch_bend().matches(cc));
    }

    TEST(MIDIControlTest, UnnumberedKindsIgnoreTheDataByte)
    {
        const auto bend = InputValue::make_midi(0xE3, 12, 90, 1).as_midi();

        EXPECT_TRUE(MIDIControl::pitch_bend().matches(bend));
        EXPECT_TRUE(MIDIControl::pitch_bend(3).matches(bend));
        EXPECT_FALSE(MIDIControl::pitch_bend(4).matches(bend));
    }

    TEST(MIDIControlTest, SystemAndPolyPressureMessagesHaveNoKind)
    {
        EXPECT_FALSE(MIDIControl::kind_of(0xA0).has_value());
        EXPECT_FALSE(MIDIControl::kind_of(0xF8).has_value());
        EXPECT_EQ(MIDIControl::kind_of(0x95), MIDIKind::NOTE);
    }

    TEST(MIDINodeTest, AControlFilterGatesItsCallbacks)
    {
        MIDINode node(MIDIConfig::cc(1));
        int fired = 0;

        node.on_cc([&](uint8_t, uint8_t) { ++fired; });

        feed(node, k_cc, 0, 1, 64);
        feed(node, k_cc, 0, 2, 64);
        feed(node, k_cc, 0, 74, 64);

        EXPECT_EQ(fired, 1);
    }

    TEST(MIDINodeTest, ANoteFilterGatesItsCallbacksOnBothOnAndOff)
    {
        MIDINode node(MIDIConfig::note(60));
        std::vector<uint8_t> seen;

        node.on_note([&](uint8_t note, uint8_t, bool) { seen.push_back(note); });

        feed(node, k_note_on, 0, 60, 100);
        feed(node, k_note_on, 0, 61, 100);
        feed(node, k_note_off, 0, 60, 0);

        EXPECT_EQ(seen, (std::vector<uint8_t> { 60, 60 }));
    }

    TEST(MIDINodeTest, ANoteFilterTakesInNoOtherKind)
    {
        MIDINode node(unsmoothed(MIDIConfig::note(60)));

        feed(node, k_cc, 0, 60, 127);

        EXPECT_DOUBLE_EQ(node.get_target_value(), 0.0);
        EXPECT_DOUBLE_EQ(node.cc_value(0, 60), 0.0);
    }

    TEST(MIDINodeTest, AChannelFilterGatesCallbacksAndScalar)
    {
        MIDINode node(unsmoothed(MIDIConfig::note().on_channel(0)));
        int fired = 0;

        node.on_note([&](uint8_t, uint8_t, bool) { ++fired; });

        feed(node, k_note_on, 1, 60, 127);
        EXPECT_EQ(fired, 0);
        EXPECT_DOUBLE_EQ(node.get_target_value(), 0.0);

        feed(node, k_note_on, 0, 60, 127);
        EXPECT_EQ(fired, 1);
        EXPECT_DOUBLE_EQ(node.get_target_value(), 1.0);
    }

    TEST(MIDINodeTest, RejectedMessagesLeaveTheStateAlone)
    {
        MIDINode node(MIDIConfig::cc(1));

        feed(node, k_cc, 0, 2, 64);
        feed(node, k_note_on, 0, 60, 100);

        EXPECT_DOUBLE_EQ(node.cc_value(0, 2), 0.0);
        EXPECT_FALSE(node.note_held(0, 60));
    }

    TEST(MIDINodeTest, NoteOnlyFilterTreatsZeroVelocityAsOff)
    {
        MIDIConfig config = MIDIConfig::note();
        config.note_on();
        MIDINode node(config);
        int fired = 0;

        node.on_note([&](uint8_t, uint8_t, bool) { ++fired; });

        feed(node, k_note_on, 0, 60, 100);
        feed(node, k_note_on, 0, 60, 0);
        feed(node, k_note_off, 0, 60, 0);

        EXPECT_EQ(fired, 1);
    }

    TEST(MIDINodeTest, NoteOffOnlyFilterTakesZeroVelocityNoteOnAsOff)
    {
        MIDIConfig config = MIDIConfig::note();
        config.note_off();
        MIDINode node(config);
        int fired = 0;

        node.on_note([&](uint8_t, uint8_t, bool) { ++fired; });

        feed(node, k_note_on, 0, 60, 100);
        feed(node, k_note_on, 0, 60, 0);
        feed(node, k_note_off, 0, 60, 0);

        EXPECT_EQ(fired, 2);
    }

    TEST(MIDINodeTest, ConfiguredSmoothingReachesTheBaseNode)
    {
        MIDIConfig config;
        config.smoothing = SmoothingMode::LINEAR;
        config.smoothing_factor = 0.5;

        MIDINode node(config);

        EXPECT_EQ(node.get_config().smoothing, SmoothingMode::LINEAR);
        EXPECT_DOUBLE_EQ(node.get_config().smoothing_factor, 0.5);
    }

    TEST(MIDINodeTest, HeldNotesFollowNoteOnAndOffPerChannel)
    {
        MIDINode node(MIDIConfig::device());

        feed(node, k_note_on, 0, 64, 90);
        feed(node, k_note_on, 0, 60, 100);
        feed(node, k_note_on, 1, 62, 70);

        EXPECT_EQ(node.held_notes(0), (std::vector<uint8_t> { 60, 64 }));
        EXPECT_EQ(node.held_notes(1), (std::vector<uint8_t> { 62 }));
        EXPECT_EQ(node.velocity_of(0, 60), 100);
        EXPECT_TRUE(node.note_held(1, 62));
        EXPECT_FALSE(node.note_held(1, 60));

        feed(node, k_note_off, 0, 60, 0);
        feed(node, k_note_on, 0, 64, 0);

        EXPECT_TRUE(node.held_notes(0).empty());
        EXPECT_EQ(node.velocity_of(0, 60), 0);
    }

    TEST(MIDINodeTest, ControlChangesAreKeptPerChannelAndNormalised)
    {
        MIDINode node(MIDIConfig::device());

        feed(node, k_cc, 2, 7, 127);
        feed(node, k_cc, 2, 7, 0);
        feed(node, k_cc, 2, 7, 64);

        EXPECT_NEAR(node.cc_value(2, 7), 64.0 / 127.0, 1e-12);
        EXPECT_DOUBLE_EQ(node.cc_value(0, 7), 0.0);
        EXPECT_DOUBLE_EQ(node.cc_value(2, 8), 0.0);
    }

    TEST(MIDINodeTest, BendPressureAndProgramAreKeptPerChannel)
    {
        MIDINode node(MIDIConfig::device());

        feed(node, k_bend, 0, 0x00, 0x40);
        EXPECT_DOUBLE_EQ(node.bend_value(0), 0.0);

        feed(node, k_bend, 0, 0x00, 0x00);
        EXPECT_DOUBLE_EQ(node.bend_value(0), -1.0);

        feed(node, k_bend, 0, 0x7F, 0x7F);
        EXPECT_NEAR(node.bend_value(0), 8191.0 / 8192.0, 1e-12);
        EXPECT_DOUBLE_EQ(node.bend_value(1), 0.0);

        feed(node, k_pressure, 3, 127, 0);
        EXPECT_DOUBLE_EQ(node.pressure_value(3), 1.0);
        EXPECT_DOUBLE_EQ(node.pressure_value(0), 0.0);

        feed(node, k_program, 4, 5, 0);
        EXPECT_EQ(node.program_of(4), 5);
        EXPECT_EQ(node.program_of(0), 0);
    }

    TEST(MIDINodeTest, KeyedEventsMatchKindNumberAndChannel)
    {
        MIDINode node(MIDIConfig::device());
        int fired = 0;

        node.on(MIDIControl::cc(74, 1), [&](MIDIContext&) { ++fired; });

        feed(node, k_cc, 1, 74, 10);
        feed(node, k_cc, 0, 74, 10);
        feed(node, k_cc, 1, 75, 10);
        feed(node, k_note_on, 1, 74, 10);

        EXPECT_EQ(fired, 1);
    }

    TEST(MIDINodeTest, AnyNumberKeyedEventSeesEveryNoteOnBothChannels)
    {
        MIDINode node(MIDIConfig::device());
        int fired = 0;

        node.on(MIDIControl::note(), [&](MIDIContext&) { ++fired; });

        feed(node, k_note_on, 0, 60, 100);
        feed(node, k_note_on, 5, 72, 100);
        feed(node, k_note_off, 0, 60, 0);
        feed(node, k_cc, 0, 1, 1);

        EXPECT_EQ(fired, 3);
    }

    TEST(MIDINodeTest, EventContextDescribesTheMessage)
    {
        struct Seen {
            MIDIKind kind;
            uint8_t channel;
            uint8_t number;
            uint8_t data;
            int16_t bend;
            bool is_on;
        };

        MIDINode node(MIDIConfig::device());
        std::vector<Seen> seen;

        const auto record = [&](MIDIContext& ctx) {
            seen.push_back({ ctx.kind, ctx.channel, ctx.number, ctx.data, ctx.bend, ctx.is_on });
        };

        node.on(MIDIControl::note(), record);
        node.on(MIDIControl::cc(), record);
        node.on(MIDIControl::pitch_bend(), record);

        feed(node, k_note_on, 3, 60, 100);
        feed(node, k_note_off, 3, 60, 0);
        feed(node, k_cc, 2, 74, 33);
        feed(node, k_bend, 1, 0x00, 0x00);

        ASSERT_EQ(seen.size(), 4U);

        EXPECT_EQ(seen[0].kind, MIDIKind::NOTE);
        EXPECT_EQ(seen[0].channel, 3);
        EXPECT_EQ(seen[0].number, 60);
        EXPECT_EQ(seen[0].data, 100);
        EXPECT_TRUE(seen[0].is_on);

        EXPECT_FALSE(seen[1].is_on);
        EXPECT_EQ(seen[1].data, 0);

        EXPECT_EQ(seen[2].kind, MIDIKind::CC);
        EXPECT_EQ(seen[2].channel, 2);
        EXPECT_EQ(seen[2].number, 74);
        EXPECT_EQ(seen[2].data, 33);

        EXPECT_EQ(seen[3].kind, MIDIKind::PITCH_BEND);
        EXPECT_EQ(seen[3].bend, -8192);
    }

    TEST(MIDINodeTest, KeyedEventsRespectTheNodeFilters)
    {
        MIDINode node(MIDIConfig::device().on_channel(0));
        int fired = 0;

        node.on(MIDIControl::note(), [&](MIDIContext&) { ++fired; });

        feed(node, k_note_on, 1, 60, 100);
        feed(node, k_note_on, 0, 60, 100);

        EXPECT_EQ(fired, 1);
    }

    TEST(MIDINodeTest, SignalIsCreatedOnceAndFedOnlyByMatchingMessages)
    {
        MIDINode node(MIDIConfig::device());

        const auto signal = node.signal(MIDIControl::cc(1, 0));
        EXPECT_EQ(signal, node.signal(MIDIControl::cc(1, 0)));
        EXPECT_NE(signal, node.signal(MIDIControl::cc(1, 1)));

        feed(node, k_cc, 0, 1, 127);
        EXPECT_DOUBLE_EQ(signal->get_target_value(), 1.0);

        feed(node, k_cc, 0, 2, 0);
        feed(node, k_cc, 1, 1, 0);
        EXPECT_DOUBLE_EQ(signal->get_target_value(), 1.0);
    }

    TEST(MIDINodeTest, SignalOfPitchBendIsSigned)
    {
        MIDINode node(MIDIConfig::device());
        const auto signal = node.signal(MIDIControl::pitch_bend(0));

        feed(node, k_bend, 0, 0x00, 0x00);

        EXPECT_DOUBLE_EQ(signal->get_target_value(), -1.0);
    }

    TEST(MIDINodeTest, SignalsOfNotesAndProgramsDisableSmoothing)
    {
        MIDINode node(MIDIConfig::device());

        EXPECT_EQ(node.signal(MIDIControl::note(60))->get_config().smoothing, SmoothingMode::NONE);
        EXPECT_EQ(node.signal(MIDIControl::program())->get_config().smoothing, SmoothingMode::NONE);
        EXPECT_EQ(node.signal(MIDIControl::cc(1))->get_config().smoothing, SmoothingMode::EXPONENTIAL);
    }

    TEST(MIDINodeTest, SignalOfANoteReportsVelocityAndZeroOnRelease)
    {
        MIDINode node(MIDIConfig::device());
        const auto signal = node.signal(MIDIControl::note(60));

        feed(node, k_note_on, 0, 60, 127);
        EXPECT_DOUBLE_EQ(signal->get_target_value(), 1.0);

        feed(node, k_note_off, 0, 60, 0);
        EXPECT_DOUBLE_EQ(signal->get_target_value(), 0.0);
    }

    TEST(MIDINodeTest, NonMidiInputIsIgnored)
    {
        MIDINode node(MIDIConfig::device());
        int fired = 0;

        node.on_note([&](uint8_t, uint8_t, bool) { ++fired; });
        node.on(MIDIControl::note(), [&](MIDIContext&) { ++fired; });

        node.process_input(InputValue::make_scalar(0.7, 1, InputType::MIDI));

        EXPECT_EQ(fired, 0);
        EXPECT_TRUE(node.held_notes(0).empty());
    }

    TEST(MIDINodeTest, TheLegacyPitchBendCallbackStillReportsTheBend)
    {
        MIDINode node(MIDIConfig::device());
        int bend = 1;

        node.on_pitch_bend([&](int16_t value) { bend = value; });
        feed(node, k_bend, 0, 0x00, 0x40);

        EXPECT_EQ(bend, 0);
    }

} // namespace
} // namespace MayaFlux::Test
