#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include <random>

#include "../support/serial_test_support.hpp"
#include "MayaFlux/Core/Input/SerialDevice.hpp"
#include "MayaFlux/Transitive/IO/JSONSerializer.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    namespace P = MayaFlux::Transitive::Protocol;
    using namespace std::chrono_literals;
    using Core::EndpointState;
    using Core::InputType;
    using Core::InputValue;
    using Core::SerialDevice;
    using Core::SerialFraming;
    using Core::SerialParse;
    using Core::SerialReply;
    using Core::SerialSpec;

    using Bytes = std::vector<uint8_t>;
    using Numbers = std::vector<double>;

    Bytes bytes_of(std::string_view text)
    {
        return Bytes(text.begin(), text.end());
    }

    struct Rig {
        explicit Rig(SerialSpec spec = SerialSpec::lines(), uint32_t id = 7)
        {
            auto fake = std::make_unique<FakeSerialLink>();
            link = fake.get();
            device = std::make_unique<SerialDevice>(id, std::move(spec), std::move(fake));
            device->set_value_callback([this](const InputValue& value) { values.push_back(value); });
            device->set_state_callback([this](EndpointState previous, EndpointState current) {
                states.emplace_back(previous, current);
            });
            EXPECT_TRUE(device->open("/dev/ttyTEST0"));
        }

        [[nodiscard]] std::vector<Numbers> vectors() const
        {
            std::vector<Numbers> out;
            for (const auto& value : values) {
                out.push_back(value.as_vector());
            }
            return out;
        }

        [[nodiscard]] std::vector<std::string> texts() const
        {
            std::vector<std::string> out;
            for (const auto& value : values) {
                const auto& bytes = value.as_bytes();
                out.emplace_back(bytes.begin(), bytes.end());
            }
            return out;
        }

        FakeSerialLink* link { nullptr };
        std::unique_ptr<SerialDevice> device;
        std::vector<InputValue> values;
        std::vector<std::pair<EndpointState, EndpointState>> states;
    };

    // ------------------------------------------------------------------
    // SerialSpec
    // ------------------------------------------------------------------

    TEST(SerialSpecTest, DefaultsAreLinesOfNumbers)
    {
        const SerialSpec spec;

        EXPECT_EQ(spec.framing, SerialFraming::LINES);
        EXPECT_EQ(spec.parse, SerialParse::NUMBERS);
        EXPECT_EQ(spec.message_limit, 4096U);
        EXPECT_EQ(spec.line.baud_rate, 115200U);
        EXPECT_EQ(spec.delimiter(), (Bytes { '\n' }));
    }

    TEST(SerialSpecTest, SettersReturnChangedCopiesAndLeaveTheOriginal)
    {
        const SerialSpec base = SerialSpec::lines();

        const SerialSpec changed = base.baud(9600)
                                       .data_bits(7)
                                       .parity(Core::SerialParity::EVEN)
                                       .stop_bits(Core::SerialStopBits::TWO)
                                       .flow_control(Core::SerialFlowControl::HARDWARE)
                                       .dtr_on_open(false)
                                       .rts_on_open(false)
                                       .settle_ms(1500)
                                       .max_message(64)
                                       .bytes();

        EXPECT_EQ(base.line.baud_rate, 115200U);
        EXPECT_EQ(base.line.settle_ms, 0U);
        EXPECT_EQ(base.parse, SerialParse::NUMBERS);
        EXPECT_EQ(changed.line.baud_rate, 9600U);
        EXPECT_EQ(changed.line.data_bits, 7);
        EXPECT_EQ(changed.line.parity, Core::SerialParity::EVEN);
        EXPECT_EQ(changed.line.stop_bits, Core::SerialStopBits::TWO);
        EXPECT_EQ(changed.line.flow_control, Core::SerialFlowControl::HARDWARE);
        EXPECT_FALSE(changed.line.dtr_on_open);
        EXPECT_FALSE(changed.line.rts_on_open);
        EXPECT_EQ(changed.line.settle_ms, 1500U);
        EXPECT_EQ(changed.message_limit, 64U);
        EXPECT_EQ(changed.parse, SerialParse::BYTES);
    }

    TEST(SerialSpecTest, FactoriesChooseFramingAndParsing)
    {
        EXPECT_EQ(SerialSpec::cobs().framing, SerialFraming::COBS);
        EXPECT_EQ(SerialSpec::cobs().parse, SerialParse::BYTES);
        EXPECT_EQ(SerialSpec::slip().framing, SerialFraming::SLIP);
        EXPECT_EQ(SerialSpec::raw().framing, SerialFraming::RAW);
        EXPECT_EQ(SerialSpec::raw().parse, SerialParse::BYTES);

        const auto gap = SerialSpec::idle_gap(5ms);
        EXPECT_EQ(gap.framing, SerialFraming::IDLE_GAP);
        EXPECT_EQ(gap.gap, 5ms);

        const auto midi = SerialSpec::midi();
        EXPECT_EQ(midi.framing, SerialFraming::RAW);
        EXPECT_EQ(midi.parse, SerialParse::MIDI);
        EXPECT_EQ(midi.line.baud_rate, 31250U);

        EXPECT_EQ(SerialSpec::delimited(">").delimiter(), (Bytes { '>' }));
        EXPECT_EQ(SerialSpec::delimited("\r\n").delimiter(), (Bytes { '\r', '\n' }));
        EXPECT_EQ(SerialSpec::delimited("").delimiter(), (Bytes { '\n' }));
    }

    TEST(SerialSpecTest, ParseWithHoldsTheParser)
    {
        const auto spec = SerialSpec::lines().parse_with(
            [](P::Message) -> std::optional<InputValue> { return std::nullopt; });

        EXPECT_EQ(spec.parse, SerialParse::CUSTOM);
        EXPECT_TRUE(static_cast<bool>(spec.parser));
    }

    TEST(SerialSpecTest, FromTakesTheLineAndTheProtocolOfAConfiguredPort)
    {
        Core::SerialLineSettings line;
        line.baud_rate = 250000;
        line.settle_ms = 1500;
        Core::SerialProtocolSettings protocol;
        protocol.framing = SerialFraming::DELIMITED;
        protocol.terminator = "\r\n";
        protocol.gap_ms = 9;
        protocol.message_limit = 77;
        protocol.parse = SerialParse::BYTES;

        const auto spec = SerialSpec::from(line, protocol);

        EXPECT_EQ(spec.line.baud_rate, 250000U);
        EXPECT_EQ(spec.line.settle_ms, 1500U);
        EXPECT_EQ(spec.framing, SerialFraming::DELIMITED);
        EXPECT_EQ(spec.delimiter(), (Bytes { '\r', '\n' }));
        EXPECT_EQ(spec.gap, 9ms);
        EXPECT_EQ(spec.message_limit, 77U);
        EXPECT_EQ(spec.parse, SerialParse::BYTES);
        EXPECT_FALSE(static_cast<bool>(spec.parser));
    }

    TEST(SerialSpecTest, DefaultProtocolSettingsAreTheDefaultSpec)
    {
        const auto spec = SerialSpec::from({}, {});

        EXPECT_EQ(spec.framing, SerialFraming::LINES);
        EXPECT_EQ(spec.parse, SerialParse::NUMBERS);
        EXPECT_EQ(spec.message_limit, 4096U);
        EXPECT_EQ(spec.gap, 2ms);
        EXPECT_EQ(spec.delimiter(), (Bytes { '\n' }));
    }

    TEST(SerialSpecTest, APortConfigSurvivesAJsonRoundTrip)
    {
        Core::SerialPortConfig port;
        port.port_name = "/dev/ttyACM0";
        port.line.baud_rate = 31250;
        port.line.data_bits = 7;
        port.line.parity = Core::SerialParity::ODD;
        port.line.stop_bits = Core::SerialStopBits::TWO;
        port.line.flow_control = Core::SerialFlowControl::HARDWARE;
        port.line.dtr_on_open = false;
        port.line.settle_ms = 1500;
        port.protocol.framing = SerialFraming::DELIMITED;
        port.protocol.terminator = "\r\n";
        port.protocol.gap_ms = 7;
        port.protocol.message_limit = 99;
        port.protocol.parse = SerialParse::BYTES;

        IO::JSONSerializer json;
        const auto text = json.encode(port);
        const auto back = json.decode<Core::SerialPortConfig>(text);

        ASSERT_TRUE(back.has_value());
        EXPECT_EQ(back->port_name, "/dev/ttyACM0");
        EXPECT_EQ(back->line.baud_rate, 31250U);
        EXPECT_EQ(back->line.data_bits, 7);
        EXPECT_EQ(back->line.parity, Core::SerialParity::ODD);
        EXPECT_EQ(back->line.stop_bits, Core::SerialStopBits::TWO);
        EXPECT_EQ(back->line.flow_control, Core::SerialFlowControl::HARDWARE);
        EXPECT_FALSE(back->line.dtr_on_open);
        EXPECT_EQ(back->line.settle_ms, 1500U);
        EXPECT_EQ(back->protocol.framing, SerialFraming::DELIMITED);
        EXPECT_EQ(back->protocol.terminator, "\r\n");
        EXPECT_EQ(back->protocol.gap_ms, 7U);
        EXPECT_EQ(back->protocol.message_limit, 99U);
        EXPECT_EQ(back->protocol.parse, SerialParse::BYTES);
    }

    // ------------------------------------------------------------------
    // Opening
    // ------------------------------------------------------------------

    TEST(SerialDeviceTest, OpenGivesTheLinkThePathAndTheSpecsLine)
    {
        Rig rig(SerialSpec::lines().baud(9600).settle_ms(50).dtr_on_open(false));

        ASSERT_EQ(rig.link->opened.size(), 1U);
        EXPECT_EQ(rig.link->opened[0].first, "/dev/ttyTEST0");
        EXPECT_EQ(rig.link->opened[0].second.baud_rate, 9600U);
        EXPECT_EQ(rig.link->opened[0].second.settle_ms, 50U);
        EXPECT_FALSE(rig.link->opened[0].second.dtr_on_open);
        EXPECT_EQ(rig.device->state(), EndpointState::OPEN);
    }

    TEST(SerialDeviceTest, ALinkThatCannotOpenFailsTheOpen)
    {
        auto fake = std::make_unique<FakeSerialLink>();
        fake->open_result = false;
        SerialDevice device(1, SerialSpec::lines(), std::move(fake));

        EXPECT_FALSE(device.open("/dev/none"));
    }

    // ------------------------------------------------------------------
    // Lines of numbers
    // ------------------------------------------------------------------

    TEST(SerialDeviceTest, ALineOfNumbersBecomesAVectorStampedWithTheDevice)
    {
        Rig rig;

        rig.link->feed("1.5, 2 3\n");

        ASSERT_EQ(rig.values.size(), 1U);
        EXPECT_EQ(rig.values[0].type, InputValue::Type::VECTOR);
        EXPECT_EQ(rig.values[0].as_vector(), (Numbers { 1.5, 2.0, 3.0 }));
        EXPECT_EQ(rig.values[0].device_id, 7U);
        EXPECT_EQ(rig.values[0].source_type, InputType::SERIAL);
    }

    TEST(SerialDeviceTest, ALineMaySplitAcrossChunks)
    {
        Rig rig;

        rig.link->feed("1,2");
        rig.link->feed(",3\n4");
        EXPECT_EQ(rig.values.size(), 1U);
        rig.link->feed("\n");

        EXPECT_EQ(rig.vectors(), (std::vector<Numbers> { { 1, 2, 3 }, { 4 } }));
    }

    TEST(SerialDeviceTest, CrLfAndSemicolonsAreSeparators)
    {
        Rig rig;

        rig.link->feed("1;2\r\n+5\r\n");

        EXPECT_EQ(rig.vectors(), (std::vector<Numbers> { { 1, 2 }, { 5 } }));
    }

    TEST(SerialDeviceTest, LinesThatAreNotAllNumbersAreCountedAndNotDelivered)
    {
        Rig rig;

        rig.link->feed("hello\n\n1 x\n5\n");

        EXPECT_EQ(rig.vectors(), (std::vector<Numbers> { { 5 } }));
        const auto stats = rig.device->stats();
        EXPECT_EQ(stats.frames, 4U);
        EXPECT_EQ(stats.unparsed, 3U);
        EXPECT_EQ(stats.dropped, 0U);
    }

    TEST(SerialDeviceTest, NonFiniteOutOfRangeAndMalformedNumbersAreRefused)
    {
        Rig rig;

        rig.link->feed("nan\ninf\n-inf\n1e999\n0x10\n+-5\n1..2\n--3\n1e\n");
        rig.link->feed("2\n");

        EXPECT_EQ(rig.vectors(), (std::vector<Numbers> { { 2 } }));
        EXPECT_EQ(rig.device->stats().unparsed, 9U);
    }

    TEST(SerialDeviceTest, AnOverlongLineIsDroppedAndTheStreamCarriesOn)
    {
        Rig rig(SerialSpec::lines().max_message(8));

        rig.link->feed("123456789012\n1 2\n");

        EXPECT_EQ(rig.vectors(), (std::vector<Numbers> { { 1, 2 } }));
        EXPECT_EQ(rig.device->stats().dropped, 1U);
    }

    TEST(SerialDeviceTest, NoiseInLineModeNeverDeliversANonFiniteValueAndTheStreamRecovers)
    {
        for (uint64_t seed = 1; seed <= 25; ++seed) {
            std::mt19937_64 rng(seed);
            Rig rig(SerialSpec::lines().max_message(64));
            Bytes noise(4096);
            for (auto& byte : noise) {
                byte = static_cast<uint8_t>(rng() % 256);
            }

            rig.link->feed(noise);
            rig.link->feed("\n9\n");

            for (const auto& value : rig.values) {
                for (const double number : value.as_vector()) {
                    ASSERT_TRUE(std::isfinite(number)) << "seed " << seed;
                }
            }
            ASSERT_FALSE(rig.values.empty()) << "seed " << seed;
            EXPECT_EQ(rig.values.back().as_vector(), (Numbers { 9 })) << "seed " << seed;
            const auto stats = rig.device->stats();
            EXPECT_EQ(stats.frames, rig.values.size() + stats.unparsed) << "seed " << seed;
        }
    }

    TEST(SerialDeviceTest, AnyChunkingOfTextGivesTheSameValues)
    {
        for (uint64_t seed = 1; seed <= 40; ++seed) {
            std::mt19937_64 rng(seed);
            std::string text;
            for (size_t i = 0; i < 60; ++i) {
                switch (rng() % 5) {
                case 0:
                    text += "junk\n";
                    break;
                case 1:
                    text += "\r\n";
                    break;
                default:
                    text += std::to_string(rng() % 1000) + "," + std::to_string(rng() % 50) + (rng() % 2 ? "\n" : "\r\n");
                    break;
                }
            }

            Rig whole;
            whole.link->feed(text);

            Rig pieces;
            size_t offset = 0;
            while (offset < text.size()) {
                const size_t take = std::min<size_t>(1 + rng() % 11, text.size() - offset);
                pieces.link->feed(std::string_view(text).substr(offset, take));
                offset += take;
            }

            ASSERT_EQ(pieces.vectors(), whole.vectors()) << "seed " << seed;
            ASSERT_EQ(pieces.device->stats().frames, whole.device->stats().frames) << "seed " << seed;
            ASSERT_EQ(pieces.device->stats().unparsed, whole.device->stats().unparsed) << "seed " << seed;
        }
    }

    // ------------------------------------------------------------------
    // Other framings and parsers
    // ------------------------------------------------------------------

    TEST(SerialDeviceTest, DelimitedMessagesBecomeBytes)
    {
        Rig rig(SerialSpec::delimited(">").bytes());

        rig.link->feed("OK>41 00>");

        EXPECT_EQ(rig.texts(), (std::vector<std::string> { "OK", "41 00" }));
        EXPECT_EQ(rig.values[0].type, InputValue::Type::BYTES);
    }

    TEST(SerialDeviceTest, CobsFramesCarryZerosAndACorruptFrameIsDropped)
    {
        Rig rig(SerialSpec::cobs());
        Bytes wire;
        P::encode_cobs(Bytes { 1, 0, 2 }, wire);
        const Bytes corrupt { 0x05, 0x11, 0x00 };
        wire.insert(wire.end(), corrupt.begin(), corrupt.end());
        P::encode_cobs(Bytes { 0, 0 }, wire);

        rig.link->feed(wire);

        ASSERT_EQ(rig.values.size(), 2U);
        EXPECT_EQ(rig.values[0].as_bytes(), (Bytes { 1, 0, 2 }));
        EXPECT_EQ(rig.values[1].as_bytes(), (Bytes { 0, 0 }));
        EXPECT_EQ(rig.device->stats().dropped, 1U);
    }

    TEST(SerialDeviceTest, SlipFramesCarryTheReservedBytes)
    {
        Rig rig(SerialSpec::slip());
        Bytes wire;
        P::encode_slip(Bytes { 0xC0, 1, 0xDB }, wire);
        P::encode_slip(Bytes { 2 }, wire);

        rig.link->feed(wire);

        ASSERT_EQ(rig.values.size(), 2U);
        EXPECT_EQ(rig.values[0].as_bytes(), (Bytes { 0xC0, 1, 0xDB }));
        EXPECT_EQ(rig.values[1].as_bytes(), (Bytes { 2 }));
    }

    TEST(SerialDeviceTest, RawDeliversEachChunkAsOneMessage)
    {
        Rig rig(SerialSpec::raw());

        rig.link->feed("ab");
        rig.link->feed(Bytes { 0, 1, 2 });

        ASSERT_EQ(rig.values.size(), 2U);
        EXPECT_EQ(rig.values[0].as_bytes(), bytes_of("ab"));
        EXPECT_EQ(rig.values[1].as_bytes(), (Bytes { 0, 1, 2 }));
    }

    TEST(SerialDeviceTest, IdleGapJoinsABurstAndTheTimerEndsIt)
    {
        Rig rig(SerialSpec::idle_gap(4ms));

        rig.link->feed("ab");
        ASSERT_EQ(rig.link->timers.size(), 1U);
        EXPECT_EQ(rig.link->timers.begin()->second.delay, 4ms);
        const auto first = rig.link->timers.begin()->first;

        rig.link->feed("cd");
        EXPECT_EQ(rig.link->cancelled, (std::vector<uint64_t> { first }));
        ASSERT_EQ(rig.link->timers.size(), 1U);
        EXPECT_TRUE(rig.values.empty());

        ASSERT_TRUE(rig.link->fire(rig.link->timers.begin()->first));

        EXPECT_EQ(rig.texts(), (std::vector<std::string> { "abcd" }));
    }

    TEST(SerialDeviceTest, ABurstCutOffByTheLinkLeavingOpenIsDropped)
    {
        Rig rig(SerialSpec::idle_gap(4ms));
        rig.link->feed("ab");
        ASSERT_EQ(rig.link->timers.size(), 1U);

        rig.link->transition(EndpointState::OPEN, EndpointState::RECONNECTING);
        EXPECT_TRUE(rig.link->timers.empty());
        rig.link->transition(EndpointState::RECONNECTING, EndpointState::OPEN);
        rig.link->feed("z");
        ASSERT_TRUE(rig.link->fire(rig.link->timers.begin()->first));

        EXPECT_EQ(rig.texts(), (std::vector<std::string> { "z" }));
    }

    TEST(SerialDeviceTest, MidiNotesUseRunningStatus)
    {
        Rig rig(SerialSpec::midi());

        rig.link->feed(Bytes { 0x90, 60, 100, 61, 0, 0x80, 60, 64 });

        ASSERT_EQ(rig.values.size(), 3U);
        for (const auto& value : rig.values) {
            EXPECT_EQ(value.type, InputValue::Type::MIDI);
            EXPECT_EQ(value.device_id, 7U);
            EXPECT_EQ(value.source_type, InputType::SERIAL);
        }
        EXPECT_EQ(rig.values[0].as_midi().status, 0x90);
        EXPECT_EQ(rig.values[0].as_midi().data1, 60);
        EXPECT_EQ(rig.values[0].as_midi().data2, 100);
        EXPECT_EQ(rig.values[1].as_midi().status, 0x90);
        EXPECT_EQ(rig.values[1].as_midi().data1, 61);
        EXPECT_EQ(rig.values[1].as_midi().data2, 0);
        EXPECT_EQ(rig.values[2].as_midi().status, 0x80);
    }

    TEST(SerialDeviceTest, MidiProgramChangeAndChannelPressureTakeOneDataByte)
    {
        Rig rig(SerialSpec::midi());

        rig.link->feed(Bytes { 0xC2, 5, 0xD1, 40 });

        ASSERT_EQ(rig.values.size(), 2U);
        EXPECT_EQ(rig.values[0].as_midi().status, 0xC2);
        EXPECT_EQ(rig.values[0].as_midi().data1, 5);
        EXPECT_EQ(rig.values[0].as_midi().data2, 0);
        EXPECT_EQ(rig.values[1].as_midi().status, 0xD1);
        EXPECT_EQ(rig.values[1].as_midi().data1, 40);
    }

    TEST(SerialDeviceTest, MidiRealtimeBytesInterleaveWithoutBreakingAMessage)
    {
        Rig rig(SerialSpec::midi());

        rig.link->feed(Bytes { 0x90, 60, 0xF8, 100 });

        ASSERT_EQ(rig.values.size(), 2U);
        EXPECT_EQ(rig.values[0].as_midi().status, 0xF8);
        EXPECT_EQ(rig.values[1].as_midi().status, 0x90);
        EXPECT_EQ(rig.values[1].as_midi().data1, 60);
        EXPECT_EQ(rig.values[1].as_midi().data2, 100);
    }

    TEST(SerialDeviceTest, MidiSysexAndStrayDataAreIgnored)
    {
        Rig rig(SerialSpec::midi());

        rig.link->feed(Bytes { 0x12, 0x34, 0xF0, 1, 2, 3, 0xF7, 0x80, 60, 0 });

        ASSERT_EQ(rig.values.size(), 1U);
        EXPECT_EQ(rig.values[0].as_midi().status, 0x80);
        EXPECT_EQ(rig.values[0].as_midi().data1, 60);
    }

    TEST(SerialDeviceTest, MidiSystemCommonMessagesAreSkippedWhileRealtimeStillPassesThrough)
    {
        Rig rig(SerialSpec::midi());

        rig.link->feed(Bytes { 0xF1, 0x20, 0xF2, 1, 2, 0xF3, 5, 0xF6, 0xF8, 0x33, 0x80, 60, 0 });

        ASSERT_EQ(rig.values.size(), 2U);
        EXPECT_EQ(rig.values[0].as_midi().status, 0xF8);
        EXPECT_EQ(rig.values[1].as_midi().status, 0x80);
        EXPECT_EQ(rig.values[1].as_midi().data1, 60);
    }

    TEST(SerialDeviceTest, MidiMessageMaySplitAcrossChunksAndANewStatusInterruptsOne)
    {
        Rig rig(SerialSpec::midi());

        rig.link->feed(Bytes { 0x90 });
        rig.link->feed(Bytes { 60 });
        EXPECT_TRUE(rig.values.empty());
        rig.link->feed(Bytes { 100 });
        ASSERT_EQ(rig.values.size(), 1U);

        rig.link->feed(Bytes { 0x90, 60, 0x80, 61, 0 });

        ASSERT_EQ(rig.values.size(), 2U);
        EXPECT_EQ(rig.values[1].as_midi().status, 0x80);
        EXPECT_EQ(rig.values[1].as_midi().data1, 61);
    }

    TEST(SerialDeviceTest, MidiOnRandomBytesOnlyEverYieldsWellFormedMessages)
    {
        for (uint64_t seed = 1; seed <= 25; ++seed) {
            std::mt19937_64 rng(seed);
            Rig rig(SerialSpec::midi());
            Bytes noise(8192);
            for (auto& byte : noise) {
                byte = static_cast<uint8_t>(rng() % 256);
            }
            size_t offset = 0;
            while (offset < noise.size()) {
                const size_t take = std::min<size_t>(1 + rng() % 13, noise.size() - offset);
                rig.link->feed(Bytes(noise.begin() + static_cast<std::ptrdiff_t>(offset),
                    noise.begin() + static_cast<std::ptrdiff_t>(offset + take)));
                offset += take;
            }

            for (const auto& value : rig.values) {
                ASSERT_EQ(value.type, InputValue::Type::MIDI) << "seed " << seed;
                ASSERT_GE(value.as_midi().status, 0x80) << "seed " << seed;
                ASSERT_LT(value.as_midi().data1, 0x80) << "seed " << seed;
                ASSERT_LT(value.as_midi().data2, 0x80) << "seed " << seed;
            }
        }
    }

    TEST(SerialDeviceTest, ACustomParserSeesMessagesAndTheDeviceStampsTheResult)
    {
        const auto spec = SerialSpec::lines().parse_with([](P::Message message) -> std::optional<InputValue> {
            if (P::as_text(message) == "skip") {
                return std::nullopt;
            }
            return InputValue::make_scalar(static_cast<double>(message.size()), 99, InputType::MIDI);
        });
        Rig rig(spec);

        rig.link->feed("abc\nskip\nwxyz\n");

        ASSERT_EQ(rig.values.size(), 2U);
        EXPECT_EQ(rig.values[0].as_scalar(), 3.0);
        EXPECT_EQ(rig.values[1].as_scalar(), 4.0);
        EXPECT_EQ(rig.values[0].device_id, 7U);
        EXPECT_EQ(rig.values[0].source_type, InputType::SERIAL);
        EXPECT_EQ(rig.device->stats().unparsed, 1U);
    }

    TEST(SerialDeviceTest, ACustomSpecWithoutAParserDeliversNothing)
    {
        SerialSpec spec = SerialSpec::lines();
        spec.parse = SerialParse::CUSTOM;
        Rig rig(spec);

        rig.link->feed("1\n2\n");

        EXPECT_TRUE(rig.values.empty());
        EXPECT_EQ(rig.device->stats().unparsed, 2U);
    }

    struct Readings {
        std::vector<InputValue> values;

        auto callback()
        {
            return [this](const InputValue& value) { values.push_back(value); };
        }

        [[nodiscard]] std::vector<std::string> texts() const
        {
            std::vector<std::string> out;
            for (const auto& value : values) {
                const auto& bytes = value.as_bytes();
                out.emplace_back(bytes.begin(), bytes.end());
            }
            return out;
        }
    };

    SerialSpec as_bytes(SerialSpec spec)
    {
        spec.parse = SerialParse::BYTES;
        return spec;
    }

    TEST(SerialDeviceTest, AReaderSeesTheSameBytesWithItsOwnFramingAndIdWhileTheDeviceReadsOnAsBefore)
    {
        Rig rig;
        Readings raw;
        const auto token = rig.device->add_reader(as_bytes(SerialSpec::raw()), 99, raw.callback());

        rig.link->feed("1 2\n3");
        rig.link->feed("\n");

        EXPECT_NE(token, 0U);
        EXPECT_EQ(raw.texts(), (std::vector<std::string> { "1 2\n3", "\n" }));
        EXPECT_EQ(raw.values[0].device_id, 99U);
        EXPECT_EQ(rig.vectors(), (std::vector<Numbers> { { 1, 2 }, { 3 } }));
        EXPECT_EQ(rig.device->stats().frames, 2U);
    }

    TEST(SerialDeviceTest, ReadersCutTheSameBytesIndependently)
    {
        Rig rig;
        Readings semicolons;
        Readings tabs;
        rig.device->add_reader(as_bytes(SerialSpec::delimited(";")), 11, semicolons.callback());
        rig.device->add_reader(as_bytes(SerialSpec::delimited("\t")), 12, tabs.callback());

        rig.link->feed("a;b\tc;d\t");

        EXPECT_EQ(semicolons.texts(), (std::vector<std::string> { "a", "b\tc" }));
        EXPECT_EQ(tabs.texts(), (std::vector<std::string> { "a;b", "c;d" }));
        EXPECT_TRUE(rig.values.empty());
    }

    TEST(SerialDeviceTest, ARemovedReaderStopsReceivingAndTheOthersDoNot)
    {
        Rig rig;
        Readings first;
        Readings second;
        const auto gone = rig.device->add_reader(as_bytes(SerialSpec::raw()), 11, first.callback());
        rig.device->add_reader(as_bytes(SerialSpec::raw()), 12, second.callback());

        rig.link->feed("x");
        rig.device->remove_reader(gone);
        rig.device->remove_reader(gone);
        rig.link->feed("y");

        EXPECT_EQ(first.texts(), (std::vector<std::string> { "x" }));
        EXPECT_EQ(second.texts(), (std::vector<std::string> { "x", "y" }));
    }

    TEST(SerialDeviceTest, ALinkStateChangeDropsThePartialMessageOfEveryReader)
    {
        Rig rig;
        Readings readings;
        rig.device->add_reader(as_bytes(SerialSpec::delimited(";")), 11, readings.callback());

        rig.link->feed("ab");
        rig.link->transition(EndpointState::OPEN, EndpointState::RECONNECTING);
        rig.link->transition(EndpointState::RECONNECTING, EndpointState::OPEN);
        rig.link->feed("c;");

        EXPECT_EQ(readings.texts(), (std::vector<std::string> { "c" }));
    }

    TEST(SerialDeviceTest, AnIdleGapReaderFlushesOnItsOwnTimer)
    {
        Rig rig;
        Readings readings;
        auto spec = as_bytes(SerialSpec::idle_gap(5ms));
        rig.device->add_reader(spec, 11, readings.callback());

        rig.link->feed("abc");
        ASSERT_FALSE(rig.link->timers.empty());
        rig.link->fire(rig.link->timers.begin()->first);

        EXPECT_EQ(readings.texts(), (std::vector<std::string> { "abc" }));
    }

    TEST(SerialDeviceTest, WithoutAValueCallbackFramesAreStillCounted)
    {
        auto fake = std::make_unique<FakeSerialLink>();
        auto* link = fake.get();
        SerialDevice device(3, SerialSpec::lines(), std::move(fake));
        ASSERT_TRUE(device.open("/dev/x"));

        link->feed("1\n2\n");

        EXPECT_EQ(device.stats().frames, 2U);
    }

    // ------------------------------------------------------------------
    // State
    // ------------------------------------------------------------------

    TEST(SerialDeviceTest, ALinkThatDropsAndReturnsNeverJoinsTheOldPartialLineToTheNewOne)
    {
        Rig rig;
        rig.link->feed("1 2");

        rig.link->transition(EndpointState::OPEN, EndpointState::RECONNECTING);
        rig.link->transition(EndpointState::RECONNECTING, EndpointState::OPEN);
        rig.link->feed("3\n");

        EXPECT_EQ(rig.vectors(), (std::vector<Numbers> { { 3 } }));
        ASSERT_EQ(rig.states.size(), 2U);
        EXPECT_EQ(rig.states[0], std::make_pair(EndpointState::OPEN, EndpointState::RECONNECTING));
        EXPECT_EQ(rig.states[1], std::make_pair(EndpointState::RECONNECTING, EndpointState::OPEN));
    }

    TEST(SerialDeviceTest, MidiStatusDoesNotSurviveALinkDrop)
    {
        Rig rig(SerialSpec::midi());
        rig.link->feed(Bytes { 0x90, 60, 100 });

        rig.link->transition(EndpointState::OPEN, EndpointState::RECONNECTING);
        rig.link->transition(EndpointState::RECONNECTING, EndpointState::OPEN);
        rig.link->feed(Bytes { 61, 0 });

        EXPECT_EQ(rig.values.size(), 1U);
    }

    // ------------------------------------------------------------------
    // Sending
    // ------------------------------------------------------------------

    TEST(SerialDeviceTest, SendWritesBytesAsTheyAre)
    {
        Rig rig;

        EXPECT_TRUE(rig.device->send(bytes_of("G0 X1")));

        ASSERT_EQ(rig.link->writes.size(), 1U);
        EXPECT_EQ(rig.link->writes[0], bytes_of("G0 X1"));
    }

    TEST(SerialDeviceTest, SendFramedUsesTheFramingTheSpecReads)
    {
        {
            Rig rig(SerialSpec::lines());
            EXPECT_TRUE(rig.device->send_framed(bytes_of("hi")));
            EXPECT_EQ(rig.link->writes.back(), bytes_of("hi\n"));
            EXPECT_FALSE(rig.device->send_framed(bytes_of("a\nb")));
            EXPECT_EQ(rig.link->writes.size(), 1U);
        }
        {
            Rig rig(SerialSpec::delimited("\r\n"));
            EXPECT_TRUE(rig.device->send_framed(bytes_of("AT")));
            EXPECT_EQ(rig.link->writes.back(), bytes_of("AT\r\n"));
            EXPECT_FALSE(rig.device->send_framed(bytes_of("A\r\nT")));
        }
        {
            Rig rig(SerialSpec::cobs());
            Bytes expected;
            P::encode_cobs(Bytes { 1, 0, 2 }, expected);
            EXPECT_TRUE(rig.device->send_framed(Bytes { 1, 0, 2 }));
            EXPECT_EQ(rig.link->writes.back(), expected);
        }
        {
            Rig rig(SerialSpec::slip());
            Bytes expected;
            P::encode_slip(Bytes { 0xC0 }, expected);
            EXPECT_TRUE(rig.device->send_framed(Bytes { 0xC0 }));
            EXPECT_EQ(rig.link->writes.back(), expected);
        }
        {
            Rig rig(SerialSpec::raw());
            EXPECT_TRUE(rig.device->send_framed(bytes_of("x")));
            EXPECT_EQ(rig.link->writes.back(), bytes_of("x"));
        }
    }

    TEST(SerialDeviceTest, ARefusedWriteIsReported)
    {
        Rig rig;
        rig.link->write_result = false;

        EXPECT_FALSE(rig.device->send(bytes_of("x")));
        EXPECT_FALSE(rig.device->send_framed(bytes_of("x")));
    }

    TEST(SerialDeviceTest, LineControlBacklogAndBreakPassThrough)
    {
        Rig rig;
        rig.link->backlog_value = 123;

        EXPECT_TRUE(rig.device->set_control_lines(false, std::nullopt));
        EXPECT_TRUE(rig.device->send_break());

        EXPECT_EQ(rig.device->write_backlog(), 123U);
        ASSERT_EQ(rig.link->control_calls.size(), 1U);
        EXPECT_EQ(rig.link->control_calls[0].dtr, std::optional<bool> { false });
        EXPECT_FALSE(rig.link->control_calls[0].rts.has_value());
        EXPECT_EQ(rig.link->breaks, 1);
    }

    // ------------------------------------------------------------------
    // Re-entrancy
    // ------------------------------------------------------------------

    TEST(SerialDeviceTest, AValueCallbackMayCloseTheDeviceFromInsideItself)
    {
        Rig rig;
        rig.device->set_value_callback([&](const InputValue& value) {
            rig.values.push_back(value);
            rig.device->close();
        });

        rig.link->feed("1\n2\n3\n");

        EXPECT_GE(rig.values.size(), 1U);
        EXPECT_GE(rig.link->closes, 1);
    }

    TEST(SerialDeviceTest, AValueCallbackMayAnswerWithASend)
    {
        Rig rig;
        rig.device->set_value_callback([&](const InputValue&) {
            EXPECT_TRUE(rig.device->send(bytes_of("ack\n")));
        });

        rig.link->feed("1\n2\n");

        EXPECT_EQ(rig.link->writes.size(), 2U);
    }

    TEST(SerialDeviceTest, StatsAlwaysBalanceOverRandomTraffic)
    {
        for (uint64_t seed = 1; seed <= 30; ++seed) {
            std::mt19937_64 rng(seed);
            Rig rig(SerialSpec::lines().max_message(32));
            for (size_t i = 0; i < 200; ++i) {
                switch (rng() % 4) {
                case 0:
                    rig.link->feed(std::to_string(rng() % 100) + "\n");
                    break;
                case 1:
                    rig.link->feed(std::string(rng() % 60, 'x') + "\n");
                    break;
                case 2: {
                    Bytes noise(rng() % 40);
                    for (auto& byte : noise)
                        byte = static_cast<uint8_t>(rng() % 256);
                    rig.link->feed(noise);
                    break;
                }
                default:
                    rig.link->transition(EndpointState::OPEN, EndpointState::RECONNECTING);
                    rig.link->transition(EndpointState::RECONNECTING, EndpointState::OPEN);
                    break;
                }
            }
            const auto stats = rig.device->stats();
            ASSERT_EQ(stats.frames, rig.values.size() + stats.unparsed) << "seed " << seed;
        }
    }

    // ------------------------------------------------------------------
    // Requests
    // ------------------------------------------------------------------

    struct Replies {
        std::vector<std::pair<SerialReply, std::string>> seen;

        auto callback()
        {
            return [this](SerialReply how, P::Message reply) {
                seen.emplace_back(how, std::string(P::as_text(reply)));
            };
        }
    };

    bool starts_with_ok(P::Message message)
    {
        return P::as_text(message).starts_with("ok");
    }

    TEST(SerialDeviceRequestTest, AReplyCompletesTheRequestAndDoesNotReachTheValues)
    {
        Rig rig;
        Replies replies;

        ASSERT_TRUE(rig.device->request(bytes_of("?\n"), starts_with_ok, 100ms, replies.callback()));
        EXPECT_EQ(rig.link->writes.back(), bytes_of("?\n"));
        ASSERT_EQ(rig.link->timers.size(), 1U);
        EXPECT_EQ(rig.link->timers.begin()->second.delay, 100ms);

        rig.link->feed("5 6\nok 1.2\n7\n");

        ASSERT_EQ(replies.seen.size(), 1U);
        EXPECT_EQ(replies.seen[0].first, SerialReply::REPLIED);
        EXPECT_EQ(replies.seen[0].second, "ok 1.2");
        EXPECT_EQ(rig.vectors(), (std::vector<Numbers> { { 5, 6 }, { 7 } }));
        EXPECT_TRUE(rig.link->timers.empty());
    }

    TEST(SerialDeviceRequestTest, ATimeoutEndsItOnceAndALaterReplyIsJustAFrame)
    {
        Rig rig;
        Replies replies;
        ASSERT_TRUE(rig.device->request(bytes_of("?\n"), starts_with_ok, 100ms, replies.callback()));

        ASSERT_TRUE(rig.link->fire(rig.link->timers.begin()->first));
        rig.link->feed("ok late\n");

        ASSERT_EQ(replies.seen.size(), 1U);
        EXPECT_EQ(replies.seen[0].first, SerialReply::TIMED_OUT);
        EXPECT_EQ(rig.device->stats().unparsed, 1U);
        EXPECT_TRUE(rig.device->request(bytes_of("?\n"), starts_with_ok, 100ms, replies.callback()));
    }

    TEST(SerialDeviceRequestTest, OnlyOneRequestIsPendingAtATime)
    {
        Rig rig;
        Replies replies;
        ASSERT_TRUE(rig.device->request(bytes_of("a\n"), starts_with_ok, 100ms, replies.callback()));

        EXPECT_FALSE(rig.device->request(bytes_of("b\n"), starts_with_ok, 100ms, replies.callback()));
        EXPECT_EQ(rig.link->writes.size(), 1U);

        rig.link->feed("ok\n");
        EXPECT_TRUE(rig.device->request(bytes_of("b\n"), starts_with_ok, 100ms, replies.callback()));
    }

    TEST(SerialDeviceRequestTest, ARequestNeedsAnOpenPortAndCallables)
    {
        Rig rig;
        Replies replies;
        rig.link->current = EndpointState::RECONNECTING;

        EXPECT_FALSE(rig.device->request(bytes_of("a\n"), starts_with_ok, 100ms, replies.callback()));

        rig.link->current = EndpointState::OPEN;
        EXPECT_FALSE(rig.device->request(bytes_of("a\n"), nullptr, 100ms, replies.callback()));
        EXPECT_FALSE(rig.device->request(bytes_of("a\n"), starts_with_ok, 100ms, nullptr));
        EXPECT_TRUE(rig.link->writes.empty());
    }

    TEST(SerialDeviceRequestTest, AFailedWriteLeavesNothingPending)
    {
        Rig rig;
        Replies replies;
        rig.link->write_result = false;

        EXPECT_FALSE(rig.device->request(bytes_of("a\n"), starts_with_ok, 100ms, replies.callback()));
        EXPECT_TRUE(rig.link->timers.empty());

        rig.link->write_result = true;
        EXPECT_TRUE(rig.device->request(bytes_of("a\n"), starts_with_ok, 100ms, replies.callback()));
        EXPECT_TRUE(replies.seen.empty());
    }

    TEST(SerialDeviceRequestTest, LosingThePortEndsTheRequestAndAStaleTimerIsHarmless)
    {
        Rig rig;
        Replies replies;
        ASSERT_TRUE(rig.device->request(bytes_of("a\n"), starts_with_ok, 100ms, replies.callback()));
        const auto stale = rig.link->timers.begin()->second.callback;

        rig.link->transition(EndpointState::OPEN, EndpointState::RECONNECTING);
        stale();
        rig.link->transition(EndpointState::RECONNECTING, EndpointState::OPEN);
        rig.link->feed("ok\n");

        ASSERT_EQ(replies.seen.size(), 1U);
        EXPECT_EQ(replies.seen[0].first, SerialReply::LINK_LOST);
        EXPECT_TRUE(rig.link->timers.empty());
    }

    TEST(SerialDeviceRequestTest, ClosingTheDeviceEndsTheRequest)
    {
        Rig rig;
        Replies replies;
        ASSERT_TRUE(rig.device->request(bytes_of("a\n"), starts_with_ok, 100ms, replies.callback()));

        rig.device->close();

        ASSERT_EQ(replies.seen.size(), 1U);
        EXPECT_EQ(replies.seen[0].first, SerialReply::LINK_LOST);
        EXPECT_EQ(rig.link->closes, 1);
    }

    TEST(SerialDeviceRequestTest, DestroyingTheDeviceDropsAPendingRequestWithoutCallingIt)
    {
        Replies replies;
        {
            Rig rig;
            ASSERT_TRUE(rig.device->request(bytes_of("a\n"), starts_with_ok, 100ms, replies.callback()));
        }

        EXPECT_TRUE(replies.seen.empty());
    }

    TEST(SerialDeviceRequestTest, AReplyThatBeatsTheTimerTokenLeavesNoStaleTimer)
    {
        Rig rig;
        Replies replies;
        rig.link->on_post_after = [&] { rig.link->feed("ok\n"); };

        const bool accepted = rig.device->request(bytes_of("?\n"), starts_with_ok, 100ms, replies.callback());
        rig.link->on_post_after = nullptr;

        EXPECT_TRUE(accepted);
        ASSERT_EQ(replies.seen.size(), 1U);
        EXPECT_EQ(replies.seen[0].first, SerialReply::REPLIED);
        EXPECT_TRUE(rig.link->timers.empty());
        EXPECT_EQ(rig.link->writes.back(), bytes_of("?\n"));
    }

    TEST(SerialDeviceRequestTest, ATimeoutThatBeatsAFailedSendStillCompletesTheRequestExactlyOnce)
    {
        Rig rig;
        Replies replies;
        rig.link->write_result = false;
        rig.link->on_write = [&] { rig.link->fire(rig.link->timers.begin()->first); };

        const bool accepted = rig.device->request(bytes_of("?\n"), starts_with_ok, 100ms, replies.callback());

        ASSERT_EQ(replies.seen.size(), 1U);
        EXPECT_EQ(replies.seen[0].first, SerialReply::TIMED_OUT);
        EXPECT_TRUE(accepted);
        EXPECT_TRUE(rig.link->timers.empty());
    }

    TEST(SerialDeviceRequestTest, ARequestThatIsRefusedNeverCallsBack)
    {
        Rig rig;
        Replies replies;

        rig.link->write_result = false;
        EXPECT_FALSE(rig.device->request(bytes_of("a\n"), starts_with_ok, 100ms, replies.callback()));
        rig.link->write_result = true;
        rig.link->current = EndpointState::RECONNECTING;
        EXPECT_FALSE(rig.device->request(bytes_of("a\n"), starts_with_ok, 100ms, replies.callback()));
        rig.link->current = EndpointState::OPEN;
        ASSERT_TRUE(rig.device->request(bytes_of("a\n"), starts_with_ok, 100ms, replies.callback()));
        EXPECT_FALSE(rig.device->request(bytes_of("b\n"), starts_with_ok, 100ms, replies.callback()));
        rig.link->feed("ok\n");

        ASSERT_EQ(replies.seen.size(), 1U);
        EXPECT_EQ(replies.seen[0].first, SerialReply::REPLIED);
    }

    TEST(SerialDeviceRequestTest, ARequestStillCompletesWhileAReaderSeesTheReplyToo)
    {
        Rig rig;
        Replies replies;
        Readings raw;
        rig.device->add_reader(as_bytes(SerialSpec::raw()), 99, raw.callback());
        ASSERT_TRUE(rig.device->request(bytes_of("?\n"), starts_with_ok, 100ms, replies.callback()));

        rig.link->feed("ok\n");

        ASSERT_EQ(replies.seen.size(), 1U);
        EXPECT_EQ(replies.seen[0].first, SerialReply::REPLIED);
        EXPECT_EQ(raw.texts(), (std::vector<std::string> { "ok\n" }));
        EXPECT_TRUE(rig.values.empty());
    }

    TEST(SerialDeviceRequestTest, AReplyAndAStaleTimeoutCompleteTheRequestOnce)
    {
        Rig rig;
        Replies replies;
        ASSERT_TRUE(rig.device->request(bytes_of("a\n"), starts_with_ok, 100ms, replies.callback()));
        const auto timeout = rig.link->timers.begin()->second.callback;

        rig.link->feed("ok\n");
        timeout();

        ASSERT_EQ(replies.seen.size(), 1U);
        EXPECT_EQ(replies.seen[0].first, SerialReply::REPLIED);
    }

    TEST(SerialDeviceRequestTest, ACompletionMayStartTheNextRequest)
    {
        Rig rig;
        int completed = 0;
        std::function<void(SerialReply, P::Message)> again;
        again = [&](SerialReply how, P::Message) {
            ++completed;
            if (how == SerialReply::REPLIED && completed < 3) {
                EXPECT_TRUE(rig.device->request(bytes_of("n\n"), starts_with_ok, 100ms, again));
            }
        };
        ASSERT_TRUE(rig.device->request(bytes_of("n\n"), starts_with_ok, 100ms, again));

        rig.link->feed("ok\nok\nok\n");

        EXPECT_EQ(completed, 3);
        EXPECT_EQ(rig.link->writes.size(), 3U);
    }

    TEST(SerialDeviceRequestTest, FramesThatDoNotMatchLeaveTheRequestWaiting)
    {
        Rig rig;
        Replies replies;
        ASSERT_TRUE(rig.device->request(bytes_of("a\n"), starts_with_ok, 100ms, replies.callback()));

        rig.link->feed("1\n2\nerr\n");

        EXPECT_TRUE(replies.seen.empty());
        EXPECT_EQ(rig.values.size(), 2U);
        rig.link->feed("ok\n");
        EXPECT_EQ(replies.seen.size(), 1U);
    }

} // namespace
} // namespace MayaFlux::Test
