#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Transitive/Protocol/Framing.hpp"

#include <random>

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    namespace P = MayaFlux::Transitive::Protocol;

    using Bytes = std::vector<uint8_t>;
    using Texts = std::vector<std::string>;

    struct Boom : std::exception { };

    Bytes bytes_of(std::string_view text)
    {
        return Bytes(text.begin(), text.end());
    }

    Bytes be32(uint32_t value)
    {
        return { static_cast<uint8_t>(value >> 24), static_cast<uint8_t>(value >> 16),
            static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value) };
    }

    Bytes join(std::initializer_list<Bytes> parts)
    {
        Bytes out;
        for (const auto& part : parts) {
            out.insert(out.end(), part.begin(), part.end());
        }
        return out;
    }

    Bytes framed(std::string_view payload)
    {
        return join({ be32(static_cast<uint32_t>(payload.size())), bytes_of(payload) });
    }

    class Collector {
    public:
        auto sink()
        {
            return [this](P::Message message) { m_messages.emplace_back(P::as_text(message)); };
        }

        [[nodiscard]] const Texts& messages() const { return m_messages; }

    private:
        Texts m_messages;
    };

    template <typename Framer>
    P::FrameReport feed(Framer& framer, Collector& out, std::string_view text)
    {
        return framer.push(bytes_of(text), out.sink());
    }

    template <typename Framer>
    void feed_each_byte(Framer& framer, Collector& out, const Bytes& data)
    {
        for (size_t index = 0; index < data.size(); ++index) {
            (void)framer.push(P::Message { data.data() + index, 1 }, out.sink());
        }
    }

    template <typename Framer>
    P::FrameReport feed_random(Framer& framer, Collector& out, const Bytes& data, std::mt19937_64& rng, size_t max_chunk)
    {
        P::FrameReport total;
        size_t offset = 0;
        while (offset < data.size()) {
            const size_t want = 1 + static_cast<size_t>(rng() % max_chunk);
            const size_t take = std::min(want, data.size() - offset);
            const auto report = framer.push(P::Message { data.data() + offset, take }, out.sink());
            total.messages += report.messages;
            total.discarded += report.discarded;
            if (report.fault != P::FrameFault::None) {
                total.fault = report.fault;
            }
            offset += take;
        }
        return total;
    }

    Bytes random_stream(std::mt19937_64& rng, size_t size, const Bytes& alphabet)
    {
        Bytes data(size);
        for (auto& byte : data) {
            byte = alphabet[static_cast<size_t>(rng() % alphabet.size())];
        }
        return data;
    }

    struct LineReference {
        Texts lines;
        size_t discarded {};
        size_t partial {};
    };

    LineReference reference_lines(const Bytes& data, size_t max_line)
    {
        LineReference reference;
        std::string current;
        for (const uint8_t byte : data) {
            if (byte != '\n') {
                current.push_back(static_cast<char>(byte));
                continue;
            }
            if (current.size() > max_line) {
                ++reference.discarded;
            } else {
                if (!current.empty() && current.back() == '\r') {
                    current.pop_back();
                }
                reference.lines.push_back(current);
            }
            current.clear();
        }
        if (current.size() > max_line) {
            ++reference.discarded;
        } else {
            reference.partial = current.size();
        }
        return reference;
    }

    bool inside(const uint8_t* pointer, const Bytes& buffer)
    {
        return pointer >= buffer.data() && pointer < buffer.data() + buffer.size();
    }

    // ---------------------------------------------------------------------
    // LineFramer
    // ---------------------------------------------------------------------

    TEST(LineFramerTest, DeliversEveryLineOfOnePush)
    {
        P::LineFramer framer;
        Collector got;

        const auto report = feed(framer, got, "a\nbb\nccc\n");

        EXPECT_EQ(got.messages(), (Texts { "a", "bb", "ccc" }));
        EXPECT_EQ(report.messages, 3U);
        EXPECT_EQ(report.discarded, 0U);
        EXPECT_TRUE(report.ok());
        EXPECT_EQ(framer.buffered(), 0U);
    }

    TEST(LineFramerTest, KeepsAPartialLineUntilItsTerminator)
    {
        P::LineFramer framer;
        Collector got;

        (void)feed(framer, got, "ab");
        EXPECT_TRUE(got.messages().empty());
        EXPECT_EQ(framer.buffered(), 2U);

        (void)feed(framer, got, "c\n");
        EXPECT_EQ(got.messages(), (Texts { "abc" }));
        EXPECT_EQ(framer.buffered(), 0U);
    }

    TEST(LineFramerTest, HoldsATrailingPartialAfterCompleteLines)
    {
        P::LineFramer framer;
        Collector got;

        (void)feed(framer, got, "a\nb");

        EXPECT_EQ(got.messages(), (Texts { "a" }));
        EXPECT_EQ(framer.buffered(), 1U);
    }

    TEST(LineFramerTest, StripsExactlyOneTrailingCarriageReturn)
    {
        P::LineFramer framer;
        Collector got;

        (void)feed(framer, got, "a\r\nb\r\r\nc\n");

        EXPECT_EQ(got.messages(), (Texts { "a", "b\r", "c" }));
    }

    TEST(LineFramerTest, ALoneCarriageReturnIsNotATerminator)
    {
        P::LineFramer framer;
        Collector got;

        (void)feed(framer, got, "a\rb\n");

        EXPECT_EQ(got.messages(), (Texts { "a\rb" }));
    }

    TEST(LineFramerTest, ACarriageReturnAndItsLineFeedMaySplitAcrossPushes)
    {
        P::LineFramer framer;
        Collector got;

        (void)feed(framer, got, "abc\r");
        EXPECT_TRUE(got.messages().empty());
        (void)feed(framer, got, "\n");

        EXPECT_EQ(got.messages(), (Texts { "abc" }));
    }

    TEST(LineFramerTest, EmptyLinesAreDeliveredAsEmptyMessages)
    {
        P::LineFramer framer;
        Collector got;

        (void)feed(framer, got, "\n\r\nx\n");

        EXPECT_EQ(got.messages(), (Texts { "", "", "x" }));
    }

    TEST(LineFramerTest, AnEmptyPushDoesNothing)
    {
        P::LineFramer framer;
        Collector got;
        (void)feed(framer, got, "ab");

        const auto report = framer.push(P::Message {}, got.sink());

        EXPECT_EQ(report.messages, 0U);
        EXPECT_EQ(report.discarded, 0U);
        EXPECT_TRUE(report.ok());
        EXPECT_EQ(framer.buffered(), 2U);
    }

    TEST(LineFramerTest, BinaryBytesPassThroughUntouched)
    {
        P::LineFramer framer;
        Collector got;
        const Bytes data { 0x00, 0xFF, 0x80, '\n', 0x01, 0xC3, 0xA9, '\n' };

        (void)framer.push(data, got.sink());

        ASSERT_EQ(got.messages().size(), 2U);
        EXPECT_EQ(got.messages()[0], std::string("\0\xFF\x80", 3));
        EXPECT_EQ(got.messages()[1], "\x01\xC3\xA9");
    }

    TEST(LineFramerTest, EveryByteOnItsOwnGivesTheSameMessages)
    {
        const Bytes data = bytes_of("alpha\r\nbeta\n\ngamma\nunterminated");
        P::LineFramer whole_framer;
        P::LineFramer byte_framer;
        Collector whole;
        Collector bytewise;

        (void)whole_framer.push(data, whole.sink());
        feed_each_byte(byte_framer, bytewise, data);

        EXPECT_EQ(bytewise.messages(), whole.messages());
        EXPECT_EQ(byte_framer.buffered(), whole_framer.buffered());
        EXPECT_EQ(byte_framer.buffered(), std::string_view("unterminated").size());
    }

    TEST(LineFramerTest, ALineInsideOnePushIsAViewIntoTheInput)
    {
        const Bytes input = bytes_of("hello\nworld\n");
        P::LineFramer framer;
        std::vector<const uint8_t*> seen;

        (void)framer.push(input, [&](P::Message message) { seen.push_back(message.data()); });

        ASSERT_EQ(seen.size(), 2U);
        EXPECT_EQ(seen[0], input.data());
        EXPECT_EQ(seen[1], input.data() + 6);
    }

    TEST(LineFramerTest, ALineSplitAcrossPushesIsNotAViewIntoTheLaterInput)
    {
        const Bytes first = bytes_of("hel");
        const Bytes second = bytes_of("lo\n");
        P::LineFramer framer;
        const uint8_t* seen = nullptr;
        std::string text;

        (void)framer.push(first, [](P::Message) { });
        (void)framer.push(second, [&](P::Message message) {
            seen = message.data();
            text = std::string(P::as_text(message));
        });

        EXPECT_EQ(text, "hello");
        EXPECT_FALSE(inside(seen, first));
        EXPECT_FALSE(inside(seen, second));
    }

    TEST(LineFramerTest, ALineLongerThanTheLimitIsDroppedAndTheNextOneIsDelivered)
    {
        P::LineFramer framer(8);
        Collector got;

        const auto report = feed(framer, got, "0123456789\nok\n");

        EXPECT_EQ(got.messages(), (Texts { "ok" }));
        EXPECT_EQ(report.messages, 1U);
        EXPECT_EQ(report.discarded, 1U);
        EXPECT_EQ(report.fault, P::FrameFault::Oversized);
        EXPECT_FALSE(report.ok());
    }

    TEST(LineFramerTest, TheLimitCountsRawBytesBeforeTheLineFeed)
    {
        P::LineFramer framer(8);
        Collector got;

        const auto at_limit = feed(framer, got, "12345678\n");
        const auto over_limit = feed(framer, got, "123456789\n");
        const auto crlf_at_limit = feed(framer, got, "1234567\r\n");
        const auto crlf_over_limit = feed(framer, got, "12345678\r\n");

        EXPECT_EQ(got.messages(), (Texts { "12345678", "1234567" }));
        EXPECT_EQ(at_limit.discarded, 0U);
        EXPECT_EQ(over_limit.discarded, 1U);
        EXPECT_EQ(crlf_at_limit.discarded, 0U);
        EXPECT_EQ(crlf_over_limit.discarded, 1U);
    }

    TEST(LineFramerTest, AnOversizeLineSplitAcrossPushesIsDroppedOnceAndResyncsAtItsTerminator)
    {
        P::LineFramer framer(8);
        Collector got;

        const auto first = feed(framer, got, "0123456");
        const auto second = feed(framer, got, "89ABC");
        const auto third = feed(framer, got, "DEF\nok\n");

        EXPECT_EQ(first.discarded, 0U);
        EXPECT_EQ(framer.buffered(), 0U);
        EXPECT_EQ(second.discarded, 1U);
        EXPECT_EQ(second.fault, P::FrameFault::Oversized);
        EXPECT_EQ(third.discarded, 0U);
        EXPECT_TRUE(third.ok());
        EXPECT_EQ(got.messages(), (Texts { "ok" }));
    }

    TEST(LineFramerTest, ALineFeedStartingTheNextPushEndsADiscard)
    {
        P::LineFramer framer(4);
        Collector got;

        (void)feed(framer, got, "0123456789");
        (void)feed(framer, got, "\nok\n");

        EXPECT_EQ(got.messages(), (Texts { "ok" }));
    }

    TEST(LineFramerTest, MemoryStaysBoundedWhileDiscardingAnEndlessLine)
    {
        P::LineFramer framer(1024);
        Collector got;
        const Bytes chunk(1024, 'x');
        size_t discarded = 0;

        for (int index = 0; index < 10 * 1024; ++index) {
            discarded += framer.push(chunk, got.sink()).discarded;
            ASSERT_LE(framer.buffered(), 1024U);
        }
        const auto end = feed(framer, got, "\nok\n");

        EXPECT_EQ(discarded, 1U);
        EXPECT_EQ(end.messages, 1U);
        EXPECT_EQ(got.messages(), (Texts { "ok" }));
    }

    TEST(LineFramerTest, ResetDropsAPartialLine)
    {
        P::LineFramer framer;
        Collector got;
        (void)feed(framer, got, "partial");

        framer.reset();
        (void)feed(framer, got, "x\n");

        EXPECT_EQ(framer.buffered(), 0U);
        EXPECT_EQ(got.messages(), (Texts { "x" }));
    }

    TEST(LineFramerTest, ResetEndsADiscardInProgress)
    {
        P::LineFramer framer(4);
        Collector got;
        (void)feed(framer, got, "0123456789");

        framer.reset();
        (void)feed(framer, got, "ok\n");

        EXPECT_EQ(got.messages(), (Texts { "ok" }));
    }

    TEST(LineFramerTest, ASinkThatThrowsOnASplitLineLeavesTheFramerUsable)
    {
        P::LineFramer framer;
        Collector got;
        (void)feed(framer, got, "ab");

        EXPECT_THROW((void)framer.push(bytes_of("c\nrest\n"), [](P::Message) { throw Boom {}; }), Boom);

        EXPECT_EQ(framer.buffered(), 0U);
        (void)feed(framer, got, "x\n");
        EXPECT_EQ(got.messages(), (Texts { "x" }));
    }

    TEST(LineFramerTest, ASinkThatThrowsOnAWholeLineLeavesTheFramerUsable)
    {
        P::LineFramer framer;
        Collector got;

        EXPECT_THROW((void)framer.push(bytes_of("a\nb\n"), [](P::Message) { throw Boom {}; }), Boom);

        EXPECT_EQ(framer.buffered(), 0U);
        (void)feed(framer, got, "c\n");
        EXPECT_EQ(got.messages(), (Texts { "c" }));
    }

    TEST(LineFramerTest, RandomChunkingMatchesAReferenceSplit)
    {
        const Bytes alphabet { 'a', 'b', ' ', '\r', '\n', '\n', 0x00, 0xFF };
        for (uint64_t seed = 1; seed <= 300; ++seed) {
            std::mt19937_64 rng(seed);
            const Bytes data = random_stream(rng, static_cast<size_t>(rng() % 400), alphabet);
            const auto reference = reference_lines(data, P::default_max_line);
            P::LineFramer framer;
            Collector got;

            const auto report = feed_random(framer, got, data, rng, 1 + static_cast<size_t>(rng() % 17));

            ASSERT_EQ(got.messages(), reference.lines) << "seed " << seed;
            ASSERT_EQ(framer.buffered(), reference.partial) << "seed " << seed;
            ASSERT_EQ(report.discarded, 0U) << "seed " << seed;
        }
    }

    TEST(LineFramerTest, RandomChunkingMatchesAReferenceSplitWithASmallLimit)
    {
        const Bytes alphabet { 'a', 'b', '\r', '\n', '\n' };
        constexpr size_t limit = 5;
        for (uint64_t seed = 1; seed <= 400; ++seed) {
            std::mt19937_64 rng(seed);
            const Bytes data = random_stream(rng, static_cast<size_t>(rng() % 300), alphabet);
            const auto reference = reference_lines(data, limit);
            P::LineFramer framer(limit);
            Collector got;

            const auto report = feed_random(framer, got, data, rng, 1 + static_cast<size_t>(rng() % 9));

            ASSERT_EQ(got.messages(), reference.lines) << "seed " << seed;
            ASSERT_EQ(report.discarded, reference.discarded) << "seed " << seed;
            ASSERT_EQ(framer.buffered(), reference.partial) << "seed " << seed;
        }
    }

    // ---------------------------------------------------------------------
    // LengthPrefixFramer
    // ---------------------------------------------------------------------

    TEST(LengthPrefixFramerTest, ReadsABigEndianHeader)
    {
        P::LengthPrefixFramer framer;
        Collector got;
        const Bytes data { 0, 0, 0, 3, 'a', 'b', 'c' };

        const auto report = framer.push(data, got.sink());

        EXPECT_EQ(got.messages(), (Texts { "abc" }));
        EXPECT_EQ(report.messages, 1U);
        EXPECT_TRUE(report.ok());
        EXPECT_EQ(framer.buffered(), 0U);
        EXPECT_FALSE(framer.broken());
    }

    TEST(LengthPrefixFramerTest, TheHeaderIsNotLittleEndian)
    {
        P::LengthPrefixFramer framer;
        Collector got;
        const std::string payload(258, 'p');

        const auto report = framer.push(framed(payload), got.sink());

        ASSERT_EQ(got.messages().size(), 1U);
        EXPECT_EQ(got.messages()[0], payload);
        EXPECT_TRUE(report.ok());
    }

    TEST(LengthPrefixFramerTest, DeliversSeveralFramesOfOnePush)
    {
        P::LengthPrefixFramer framer;
        Collector got;
        const Bytes data = join({ framed("one"), framed("two"), framed("three") });

        const auto report = framer.push(data, got.sink());

        EXPECT_EQ(got.messages(), (Texts { "one", "two", "three" }));
        EXPECT_EQ(report.messages, 3U);
    }

    TEST(LengthPrefixFramerTest, AHeaderSplitAtEveryPositionStillParses)
    {
        for (size_t split = 1; split < 4; ++split) {
            P::LengthPrefixFramer framer;
            Collector got;
            const Bytes data = framed("payload");

            (void)framer.push(P::Message { data.data(), split }, got.sink());
            EXPECT_EQ(framer.buffered(), split);
            EXPECT_TRUE(got.messages().empty());
            (void)framer.push(P::Message { data.data() + split, data.size() - split }, got.sink());

            EXPECT_EQ(got.messages(), (Texts { "payload" })) << "split " << split;
            EXPECT_EQ(framer.buffered(), 0U);
        }
    }

    TEST(LengthPrefixFramerTest, APayloadSplitAtEveryPositionStillParses)
    {
        const std::string payload = "0123456789";
        const Bytes data = framed(payload);
        for (size_t split = 1; split < data.size(); ++split) {
            P::LengthPrefixFramer framer;
            Collector got;

            (void)framer.push(P::Message { data.data(), split }, got.sink());
            (void)framer.push(P::Message { data.data() + split, data.size() - split }, got.sink());

            ASSERT_EQ(got.messages(), (Texts { payload })) << "split " << split;
        }
    }

    TEST(LengthPrefixFramerTest, EveryByteOnItsOwnGivesTheSameFrames)
    {
        const Bytes data = join({ framed("alpha"), framed(""), framed("beta"), framed(std::string(300, 'z')) });
        P::LengthPrefixFramer whole_framer;
        P::LengthPrefixFramer byte_framer;
        Collector whole;
        Collector bytewise;

        (void)whole_framer.push(data, whole.sink());
        feed_each_byte(byte_framer, bytewise, data);

        EXPECT_EQ(bytewise.messages(), whole.messages());
        EXPECT_EQ(whole.messages().size(), 4U);
    }

    TEST(LengthPrefixFramerTest, PartialHeaderBytesAndPayloadBytesAreCounted)
    {
        P::LengthPrefixFramer framer;
        Collector got;
        const Bytes header = be32(10);

        (void)framer.push(P::Message { header.data(), 2 }, got.sink());
        EXPECT_EQ(framer.buffered(), 2U);

        const Bytes rest { header[2], header[3], 'a', 'b', 'c', 'd' };
        (void)framer.push(rest, got.sink());

        EXPECT_EQ(framer.buffered(), 4U);
        EXPECT_TRUE(got.messages().empty());
    }

    TEST(LengthPrefixFramerTest, ATruncatedFrameNeverReachesTheSink)
    {
        P::LengthPrefixFramer framer;
        Collector got;

        (void)framer.push(join({ be32(10), bytes_of("abcd") }), got.sink());

        EXPECT_TRUE(got.messages().empty());
        EXPECT_FALSE(framer.broken());
    }

    TEST(LengthPrefixFramerTest, AZeroLengthFrameIsAnEmptyMessageByDefault)
    {
        P::LengthPrefixFramer framer;
        Collector got;

        const auto report = framer.push(join({ be32(0), framed("x") }), got.sink());

        EXPECT_EQ(got.messages(), (Texts { "", "x" }));
        EXPECT_EQ(report.messages, 2U);
        EXPECT_TRUE(report.ok());
    }

    TEST(LengthPrefixFramerTest, RejectingEmptyFramesMakesAZeroLengthTerminal)
    {
        P::LengthPrefixFramer framer(P::default_max_frame, true);
        Collector got;

        const auto report = framer.push(join({ framed("a"), be32(0), framed("later") }), got.sink());

        EXPECT_EQ(got.messages(), (Texts { "a" }));
        EXPECT_EQ(report.messages, 1U);
        EXPECT_EQ(report.fault, P::FrameFault::Invalid);
        EXPECT_TRUE(framer.broken());
        EXPECT_EQ(framer.buffered(), 0U);

        const auto again = framer.push(framed("ignored"), got.sink());
        EXPECT_EQ(again.messages, 0U);
        EXPECT_EQ(again.fault, P::FrameFault::Invalid);
        EXPECT_EQ(got.messages().size(), 1U);

        framer.reset();
        EXPECT_FALSE(framer.broken());
        (void)framer.push(framed("fresh"), got.sink());
        EXPECT_EQ(got.messages(), (Texts { "a", "fresh" }));
    }

    TEST(LengthPrefixFramerTest, ALengthAboveTheLimitIsTerminalUntilReset)
    {
        P::LengthPrefixFramer framer(64);
        Collector got;

        const auto report = framer.push(join({ be32(65), bytes_of("payload that must not be buffered") }), got.sink());

        EXPECT_EQ(report.fault, P::FrameFault::Oversized);
        EXPECT_TRUE(framer.broken());
        EXPECT_EQ(framer.buffered(), 0U);
        EXPECT_TRUE(got.messages().empty());

        const auto again = framer.push(framed("ok"), got.sink());
        EXPECT_EQ(again.fault, P::FrameFault::Oversized);
        EXPECT_TRUE(got.messages().empty());

        framer.reset();
        (void)framer.push(framed("ok"), got.sink());
        EXPECT_EQ(got.messages(), (Texts { "ok" }));
    }

    TEST(LengthPrefixFramerTest, ALengthExactlyAtTheLimitIsAccepted)
    {
        P::LengthPrefixFramer framer(4);
        Collector got;

        const auto at_limit = framer.push(framed("abcd"), got.sink());
        const auto over_limit = framer.push(framed("abcde"), got.sink());

        EXPECT_TRUE(at_limit.ok());
        EXPECT_EQ(got.messages(), (Texts { "abcd" }));
        EXPECT_EQ(over_limit.fault, P::FrameFault::Oversized);
    }

    TEST(LengthPrefixFramerTest, TheLargestDeclaredLengthIsRefusedWithoutBuffering)
    {
        P::LengthPrefixFramer framer;
        Collector got;
        const Bytes hostile = join({ Bytes { 0xFF, 0xFF, 0xFF, 0xFF }, Bytes(4096, 'x') });

        const auto report = framer.push(hostile, got.sink());

        EXPECT_EQ(report.fault, P::FrameFault::Oversized);
        EXPECT_EQ(framer.buffered(), 0U);
        EXPECT_TRUE(got.messages().empty());
    }

    TEST(LengthPrefixFramerTest, ALimitBeyondFourBytesIsClampedToTheFieldItself)
    {
        P::LengthPrefixFramer framer(std::numeric_limits<size_t>::max());
        Collector got;

        const auto report = framer.push(join({ Bytes { 0xFF, 0xFF, 0xFF, 0xFF }, bytes_of("abc") }), got.sink());

        EXPECT_TRUE(report.ok());
        EXPECT_FALSE(framer.broken());
        EXPECT_EQ(framer.buffered(), 3U);
        EXPECT_TRUE(got.messages().empty());
    }

    TEST(LengthPrefixFramerTest, FramesCompletedBeforeAFaultInTheSamePushAreDelivered)
    {
        P::LengthPrefixFramer framer(16);
        Collector got;
        const Bytes data = join({ framed("one"), framed("two"), be32(1000), bytes_of("tail") });

        const auto report = framer.push(data, got.sink());

        EXPECT_EQ(got.messages(), (Texts { "one", "two" }));
        EXPECT_EQ(report.messages, 2U);
        EXPECT_EQ(report.fault, P::FrameFault::Oversized);
    }

    TEST(LengthPrefixFramerTest, PayloadBytesAreOpaque)
    {
        P::LengthPrefixFramer framer;
        Collector got;
        const std::string payload("\0\n\r\xFF\x80 line\nfeed\n", 16);

        (void)framer.push(framed(payload), got.sink());

        ASSERT_EQ(got.messages().size(), 1U);
        EXPECT_EQ(got.messages()[0], payload);
    }

    TEST(LengthPrefixFramerTest, AFrameInsideOnePushIsAViewIntoTheInput)
    {
        const Bytes input = join({ framed("hello"), framed("world") });
        P::LengthPrefixFramer framer;
        std::vector<const uint8_t*> seen;

        (void)framer.push(input, [&](P::Message message) { seen.push_back(message.data()); });

        ASSERT_EQ(seen.size(), 2U);
        EXPECT_EQ(seen[0], input.data() + 4);
        EXPECT_EQ(seen[1], input.data() + 4 + 5 + 4);
    }

    TEST(LengthPrefixFramerTest, AFrameSplitAcrossPushesIsNotAViewIntoTheLaterInput)
    {
        const Bytes all = framed("hello");
        const Bytes first(all.begin(), all.begin() + 6);
        const Bytes second(all.begin() + 6, all.end());
        P::LengthPrefixFramer framer;
        const uint8_t* seen = nullptr;
        std::string text;

        (void)framer.push(first, [](P::Message) { });
        (void)framer.push(second, [&](P::Message message) {
            seen = message.data();
            text = std::string(P::as_text(message));
        });

        EXPECT_EQ(text, "hello");
        EXPECT_FALSE(inside(seen, first));
        EXPECT_FALSE(inside(seen, second));
    }

    TEST(LengthPrefixFramerTest, ASinkThatThrowsOnASplitFrameLeavesTheFramerUsable)
    {
        const Bytes all = framed("hello");
        P::LengthPrefixFramer framer;
        Collector got;
        (void)framer.push(P::Message { all.data(), 6 }, got.sink());

        EXPECT_THROW((void)framer.push(P::Message { all.data() + 6, all.size() - 6 }, [](P::Message) { throw Boom {}; }), Boom);

        EXPECT_EQ(framer.buffered(), 0U);
        (void)framer.push(framed("next"), got.sink());
        EXPECT_EQ(got.messages(), (Texts { "next" }));
    }

    TEST(LengthPrefixFramerTest, ASinkThatThrowsOnAWholeFrameLeavesTheFramerUsable)
    {
        P::LengthPrefixFramer framer;
        Collector got;

        EXPECT_THROW((void)framer.push(join({ framed("a"), framed("b") }), [](P::Message) { throw Boom {}; }), Boom);

        EXPECT_EQ(framer.buffered(), 0U);
        (void)framer.push(framed("c"), got.sink());
        EXPECT_EQ(got.messages(), (Texts { "c" }));
    }

    TEST(LengthPrefixFramerTest, AnEmptyPushDoesNothing)
    {
        P::LengthPrefixFramer framer;
        Collector got;

        const auto report = framer.push(P::Message {}, got.sink());

        EXPECT_EQ(report.messages, 0U);
        EXPECT_TRUE(report.ok());
    }

    TEST(LengthPrefixFramerTest, ResetDropsAPartialFrame)
    {
        P::LengthPrefixFramer framer;
        Collector got;
        (void)framer.push(join({ be32(10), bytes_of("abc") }), got.sink());

        framer.reset();
        (void)framer.push(framed("x"), got.sink());

        EXPECT_EQ(got.messages(), (Texts { "x" }));
    }

    TEST(LengthPrefixFramerTest, AFiveMegabytePayloadCrossesManyPushes)
    {
        constexpr size_t size = size_t { 5 } << 20;
        std::mt19937_64 rng(7);
        Bytes payload(size);
        for (auto& byte : payload) {
            byte = static_cast<uint8_t>(rng());
        }
        const Bytes data = join({ be32(static_cast<uint32_t>(size)), payload });
        P::LengthPrefixFramer framer(size_t { 8 } << 20);
        size_t delivered = 0;
        bool identical = false;

        for (size_t offset = 0; offset < data.size(); offset += 4096) {
            const size_t take = std::min<size_t>(4096, data.size() - offset);
            (void)framer.push(P::Message { data.data() + offset, take }, [&](P::Message message) {
                ++delivered;
                identical = std::ranges::equal(message, payload);
            });
        }

        EXPECT_EQ(delivered, 1U);
        EXPECT_TRUE(identical);
        EXPECT_EQ(framer.buffered(), 0U);
    }

    TEST(LengthPrefixFramerTest, RandomChunkingMatchesTheFramesThatWereSent)
    {
        for (uint64_t seed = 1; seed <= 300; ++seed) {
            std::mt19937_64 rng(seed);
            Texts expected;
            Bytes data;
            const size_t count = static_cast<size_t>(rng() % 12);
            for (size_t index = 0; index < count; ++index) {
                std::string payload(static_cast<size_t>(rng() % 200), '\0');
                for (auto& character : payload) {
                    character = static_cast<char>(rng());
                }
                const Bytes frame = framed(payload);
                data.insert(data.end(), frame.begin(), frame.end());
                expected.push_back(payload);
            }
            P::LengthPrefixFramer framer;
            Collector got;

            const auto report = feed_random(framer, got, data, rng, 1 + static_cast<size_t>(rng() % 23));

            ASSERT_EQ(got.messages(), expected) << "seed " << seed;
            ASSERT_TRUE(report.ok()) << "seed " << seed;
            ASSERT_EQ(framer.buffered(), 0U) << "seed " << seed;
        }
    }

    TEST(LengthPrefixFramerTest, RandomGarbageNeverCrashesAndStaysConsistent)
    {
        for (uint64_t seed = 1; seed <= 500; ++seed) {
            std::mt19937_64 rng(seed);
            Bytes data(static_cast<size_t>(rng() % 600));
            for (auto& byte : data) {
                byte = static_cast<uint8_t>(rng());
            }
            P::LengthPrefixFramer framer(256);
            Collector got;

            const auto report = feed_random(framer, got, data, rng, 1 + static_cast<size_t>(rng() % 31));

            for (const auto& message : got.messages()) {
                ASSERT_LE(message.size(), 256U) << "seed " << seed;
            }
            ASSERT_EQ(framer.broken(), report.fault != P::FrameFault::None) << "seed " << seed;
            ASSERT_LE(framer.buffered(), 256U + 4U) << "seed " << seed;
        }
    }

    // ---------------------------------------------------------------------
    // Encoders
    // ---------------------------------------------------------------------

    TEST(FramingEncodeTest, LengthPrefixedWritesABigEndianHeaderThenThePayload)
    {
        Bytes out;

        EXPECT_TRUE(P::encode_length_prefixed(bytes_of("abc"), out));

        EXPECT_EQ(out, (Bytes { 0, 0, 0, 3, 'a', 'b', 'c' }));
    }

    TEST(FramingEncodeTest, LengthPrefixedAppendsToWhatIsThere)
    {
        Bytes out = bytes_of("keep");

        EXPECT_TRUE(P::encode_length_prefixed(bytes_of("x"), out));

        EXPECT_EQ(out, (Bytes { 'k', 'e', 'e', 'p', 0, 0, 0, 1, 'x' }));
    }

    TEST(FramingEncodeTest, LengthPrefixedRefusesAnOversizePayloadAndLeavesOutAlone)
    {
        Bytes out = bytes_of("keep");

        EXPECT_FALSE(P::encode_length_prefixed(bytes_of("abcd"), out, 3));

        EXPECT_EQ(out, bytes_of("keep"));
        EXPECT_TRUE(P::encode_length_prefixed(bytes_of("abc"), out, 3));
    }

    TEST(FramingEncodeTest, LengthPrefixedAcceptsAnEmptyPayloadUnlessRejected)
    {
        Bytes out;

        EXPECT_TRUE(P::encode_length_prefixed(P::Message {}, out));
        EXPECT_EQ(out, be32(0));

        Bytes refused = bytes_of("keep");
        EXPECT_FALSE(P::encode_length_prefixed(P::Message {}, refused, P::default_max_frame, true));
        EXPECT_EQ(refused, bytes_of("keep"));
    }

    TEST(FramingEncodeTest, LengthPrefixedAndItsFramerAgreeOnEveryPayload)
    {
        for (uint64_t seed = 1; seed <= 300; ++seed) {
            std::mt19937_64 rng(seed);
            Texts expected;
            Bytes wire;
            const size_t count = static_cast<size_t>(rng() % 10);
            for (size_t index = 0; index < count; ++index) {
                Bytes payload(static_cast<size_t>(rng() % 100));
                for (auto& byte : payload) {
                    byte = static_cast<uint8_t>(rng());
                }
                ASSERT_TRUE(P::encode_length_prefixed(payload, wire)) << "seed " << seed;
                expected.emplace_back(P::as_text(payload));
            }
            P::LengthPrefixFramer framer;
            Collector got;

            (void)feed_random(framer, got, wire, rng, 1 + static_cast<size_t>(rng() % 11));

            ASSERT_EQ(got.messages(), expected) << "seed " << seed;
        }
    }

    TEST(FramingEncodeTest, LineAppendsThePayloadAndALineFeed)
    {
        Bytes out = bytes_of("keep");

        EXPECT_TRUE(P::encode_line(bytes_of("abc"), out));

        EXPECT_EQ(out, bytes_of("keepabc\n"));
    }

    TEST(FramingEncodeTest, LineAcceptsAnEmptyPayloadAndAnInteriorCarriageReturn)
    {
        Bytes out;

        EXPECT_TRUE(P::encode_line(P::Message {}, out));
        EXPECT_TRUE(P::encode_line(bytes_of("a\rb"), out));

        EXPECT_EQ(out, bytes_of("\na\rb\n"));
    }

    TEST(FramingEncodeTest, LineRefusesWhatItsFramerCouldNotReturnAndLeavesOutAlone)
    {
        Bytes out = bytes_of("keep");

        EXPECT_FALSE(P::encode_line(bytes_of("two\nlines"), out));
        EXPECT_FALSE(P::encode_line(bytes_of("ends in cr\r"), out));
        EXPECT_FALSE(P::encode_line(bytes_of("\n"), out));

        EXPECT_EQ(out, bytes_of("keep"));
    }

    TEST(FramingEncodeTest, LineAndItsFramerAgreeOnEveryAcceptedPayload)
    {
        const Bytes alphabet { 'a', 'b', ' ', '\r', '\n', 0x00, 0xFF };
        for (uint64_t seed = 1; seed <= 400; ++seed) {
            std::mt19937_64 rng(seed);
            Texts expected;
            Bytes wire;
            const size_t count = static_cast<size_t>(rng() % 10);
            for (size_t index = 0; index < count; ++index) {
                const Bytes payload = random_stream(rng, static_cast<size_t>(rng() % 40), alphabet);
                if (P::encode_line(payload, wire)) {
                    expected.emplace_back(P::as_text(payload));
                }
            }
            P::LineFramer framer;
            Collector got;

            (void)feed_random(framer, got, wire, rng, 1 + static_cast<size_t>(rng() % 9));

            ASSERT_EQ(got.messages(), expected) << "seed " << seed;
            ASSERT_EQ(framer.buffered(), 0U) << "seed " << seed;
        }
    }


    std::string text_of(const Bytes& data)
    {
        return std::string(data.begin(), data.end());
    }

    Bytes encoded_cobs(const Bytes& payload)
    {
        Bytes out;
        P::encode_cobs(payload, out);
        return out;
    }

    Bytes encoded_slip(const Bytes& payload)
    {
        Bytes out;
        P::encode_slip(payload, out);
        return out;
    }

    Bytes random_payload(std::mt19937_64& rng, size_t max_size)
    {
        Bytes payload(static_cast<size_t>(rng() % (max_size + 1)));
        for (auto& byte : payload) {
            byte = static_cast<uint8_t>(rng() % 4 == 0 ? 0 : rng() % 256);
        }
        return payload;
    }

    // ------------------------------------------------------------------
    // DelimiterFramer
    // ------------------------------------------------------------------

    TEST(DelimiterFramerTest, SingleByteTerminatorDelivers)
    {
        P::DelimiterFramer framer(">");
        Collector got;

        const auto report = feed(framer, got, "OK>41 00>");

        EXPECT_EQ(report.messages, 2U);
        EXPECT_TRUE(report.ok());
        EXPECT_EQ(got.messages(), (Texts { "OK", "41 00" }));
    }

    TEST(DelimiterFramerTest, MessageAndTerminatorMaySplitAcrossPushes)
    {
        P::DelimiterFramer framer("\r\n");
        Collector got;

        (void)feed(framer, got, "ab");
        (void)feed(framer, got, "c\r");
        EXPECT_TRUE(got.messages().empty());
        (void)feed(framer, got, "\nde\r\n");

        EXPECT_EQ(got.messages(), (Texts { "abc", "de" }));
        EXPECT_EQ(framer.buffered(), 0U);
    }

    TEST(DelimiterFramerTest, EmptyMessagesAreDelivered)
    {
        P::DelimiterFramer framer("\r\n");
        Collector got;

        (void)feed(framer, got, "\r\n\r\nx\r\n");

        EXPECT_EQ(got.messages(), (Texts { "", "", "x" }));
    }

    TEST(DelimiterFramerTest, PartOfTheTerminatorInsideAMessageStaysInIt)
    {
        P::DelimiterFramer framer("\r\n");
        Collector got;

        (void)feed(framer, got, "a\rb\nc\r\n");

        EXPECT_EQ(got.messages(), (Texts { "a\rb\nc" }));
    }

    TEST(DelimiterFramerTest, OverlappingTerminatorEndsAtTheFirstMatch)
    {
        P::DelimiterFramer framer("aa");
        Collector got;

        (void)feed(framer, got, "xaaayaa");

        EXPECT_EQ(got.messages(), (Texts { "x", "ay" }));
    }

    TEST(DelimiterFramerTest, MessageOfExactlyTheLimitIsDelivered)
    {
        P::DelimiterFramer framer(">", 4);
        Collector got;

        const auto report = feed(framer, got, "abcd>");

        EXPECT_EQ(got.messages(), (Texts { "abcd" }));
        EXPECT_EQ(report.discarded, 0U);
    }

    TEST(DelimiterFramerTest, OversizedMessageIsDroppedAndTheNextOneDelivered)
    {
        P::DelimiterFramer framer(">", 4);
        Collector got;

        const auto report = feed(framer, got, "abcdefgh>ok>");

        EXPECT_EQ(got.messages(), (Texts { "ok" }));
        EXPECT_EQ(report.discarded, 1U);
        EXPECT_EQ(report.fault, P::FrameFault::Oversized);
    }

    TEST(DelimiterFramerTest, OversizedMessageWithAMultiByteTerminatorIsDropped)
    {
        P::DelimiterFramer framer("\r\n", 3);
        Collector got;

        const auto report = feed(framer, got, "abcdef\r\nxy\r\n");

        EXPECT_EQ(got.messages(), (Texts { "xy" }));
        EXPECT_EQ(report.discarded, 1U);
    }

    TEST(DelimiterFramerTest, TerminatorSplitAcrossPushesEndsADiscard)
    {
        P::DelimiterFramer framer("\r\n", 2);
        Collector got;

        (void)feed(framer, got, "abcd\r");
        (void)feed(framer, got, "\nok\r\n");

        EXPECT_EQ(got.messages(), (Texts { "ok" }));
    }

    TEST(DelimiterFramerTest, ResetDropsAPartialMessageAndADiscard)
    {
        P::DelimiterFramer framer(">", 2);
        Collector got;
        (void)feed(framer, got, "abcdef");

        framer.reset();
        (void)feed(framer, got, "x>");

        EXPECT_EQ(got.messages(), (Texts { "x" }));
        EXPECT_EQ(framer.buffered(), 0U);
    }

    TEST(DelimiterFramerTest, AnEmptyTerminatorIsTakenAsLf)
    {
        P::DelimiterFramer framer(std::string_view {});
        Collector got;

        (void)feed(framer, got, "a\nb\n");

        EXPECT_EQ(got.messages(), (Texts { "a", "b" }));
    }

    TEST(DelimiterFramerTest, ASinkThatThrowsLeavesTheFramerUsable)
    {
        P::DelimiterFramer framer(">");

        EXPECT_THROW((void)framer.push(bytes_of("a>b>"), [](P::Message) { throw Boom {}; }), Boom);

        Collector got;
        (void)feed(framer, got, "c>");
        EXPECT_EQ(got.messages(), (Texts { "c" }));
    }

    TEST(DelimiterFramerTest, AnyChunkingGivesTheSameMessages)
    {
        for (uint64_t seed = 1; seed <= 40; ++seed) {
            std::mt19937_64 rng(seed);
            Texts expected;
            Bytes wire;
            for (size_t i = 0; i < 30; ++i) {
                std::string message(static_cast<size_t>(rng() % 12), 'a');
                for (auto& c : message) {
                    c = static_cast<char>('a' + rng() % 3);
                }
                expected.push_back(message);
                const auto part = bytes_of(message + "\r\n");
                wire.insert(wire.end(), part.begin(), part.end());
            }
            P::DelimiterFramer framer("\r\n");
            Collector got;

            (void)feed_random(framer, got, wire, rng, 1 + static_cast<size_t>(rng() % 9));

            ASSERT_EQ(got.messages(), expected) << "seed " << seed;
            ASSERT_EQ(framer.buffered(), 0U) << "seed " << seed;
        }
    }

    TEST(DelimiterFramerTest, EncodeProducesWhatTheFramerReadsBack)
    {
        Bytes wire;
        ASSERT_TRUE(P::encode_delimited(bytes_of("PING"), bytes_of("\r\n"), wire));
        ASSERT_TRUE(P::encode_delimited(bytes_of(""), bytes_of("\r\n"), wire));
        EXPECT_EQ(wire, bytes_of("PING\r\n\r\n"));

        P::DelimiterFramer framer("\r\n");
        Collector got;
        (void)framer.push(wire, got.sink());
        EXPECT_EQ(got.messages(), (Texts { "PING", "" }));
    }

    TEST(DelimiterFramerTest, EncodeRefusesWhatTheFramerCouldNotReturnAndLeavesOutAlone)
    {
        Bytes wire = bytes_of("keep");

        EXPECT_FALSE(P::encode_delimited(bytes_of("a\r\nb"), bytes_of("\r\n"), wire));
        EXPECT_FALSE(P::encode_delimited(bytes_of("a"), bytes_of("aa"), wire));
        EXPECT_FALSE(P::encode_delimited(bytes_of("a"), Bytes {}, wire));
        EXPECT_EQ(wire, bytes_of("keep"));
    }

    // ------------------------------------------------------------------
    // CobsFramer
    // ------------------------------------------------------------------

    TEST(CobsFramerTest, EncodesTheReferenceVectors)
    {
        EXPECT_EQ(encoded_cobs({}), (Bytes { 0x01, 0x00 }));
        EXPECT_EQ(encoded_cobs({ 0x00 }), (Bytes { 0x01, 0x01, 0x00 }));
        EXPECT_EQ(encoded_cobs({ 0x00, 0x00 }), (Bytes { 0x01, 0x01, 0x01, 0x00 }));
        EXPECT_EQ(encoded_cobs({ 0x11, 0x22, 0x00, 0x33 }), (Bytes { 0x03, 0x11, 0x22, 0x02, 0x33, 0x00 }));
        EXPECT_EQ(encoded_cobs({ 0x11, 0x22, 0x33, 0x44 }), (Bytes { 0x05, 0x11, 0x22, 0x33, 0x44, 0x00 }));
        EXPECT_EQ(encoded_cobs({ 0x11, 0x00, 0x00, 0x00 }), (Bytes { 0x02, 0x11, 0x01, 0x01, 0x01, 0x00 }));
    }

    TEST(CobsFramerTest, EncodesRunsOfTwoHundredFiftyFourAndMore)
    {
        Bytes run254;
        for (uint8_t value = 1; value <= 254; ++value)
            run254.push_back(value);
        Bytes expected254 { 0xFF };
        expected254.insert(expected254.end(), run254.begin(), run254.end());
        expected254.push_back(0x00);
        EXPECT_EQ(encoded_cobs(run254), expected254);

        Bytes run255 = run254;
        run255.push_back(0xFF);
        Bytes expected255 { 0xFF };
        expected255.insert(expected255.end(), run254.begin(), run254.end());
        expected255.push_back(0x02);
        expected255.push_back(0xFF);
        expected255.push_back(0x00);
        EXPECT_EQ(encoded_cobs(run255), expected255);
    }

    TEST(CobsFramerTest, ReadsBackWhatItEncodesFromTheReferenceVectors)
    {
        const std::vector<Bytes> payloads {
            {}, { 0x00 }, { 0x00, 0x00 }, { 0x11, 0x22, 0x00, 0x33 }, { 0x11, 0x00, 0x00, 0x00 }
        };
        P::CobsFramer framer;
        Collector got;
        Texts expected;
        for (const auto& payload : payloads) {
            expected.push_back(text_of(payload));
            const auto wire = encoded_cobs(payload);
            (void)framer.push(wire, got.sink());
        }

        EXPECT_EQ(got.messages(), expected);
    }

    TEST(CobsFramerTest, BytesMayArriveOneAtATime)
    {
        const Bytes wire = join({ encoded_cobs({ 0x11, 0x00, 0x22 }), encoded_cobs({ 0x33 }) });
        P::CobsFramer framer;
        Collector got;

        feed_each_byte(framer, got, wire);

        EXPECT_EQ(got.messages(), (Texts { text_of({ 0x11, 0x00, 0x22 }), text_of({ 0x33 }) }));
        EXPECT_EQ(framer.buffered(), 0U);
    }

    TEST(CobsFramerTest, FramesThatAreOnlyADelimiterAreSkipped)
    {
        P::CobsFramer framer;
        Collector got;
        const Bytes wire = join({ Bytes { 0x00, 0x00 }, encoded_cobs({ 0x41 }), Bytes { 0x00 } });

        const auto report = framer.push(wire, got.sink());

        EXPECT_EQ(got.messages(), (Texts { "A" }));
        EXPECT_TRUE(report.ok());
        EXPECT_EQ(report.discarded, 0U);
    }

    TEST(CobsFramerTest, AFrameThatDoesNotDecodeIsDroppedAndTheNextOneDelivered)
    {
        P::CobsFramer framer;
        Collector got;
        const Bytes wire = join({ Bytes { 0x05, 0x11, 0x00 }, encoded_cobs({ 0x42 }) });

        const auto report = framer.push(wire, got.sink());

        EXPECT_EQ(got.messages(), (Texts { "B" }));
        EXPECT_EQ(report.discarded, 1U);
        EXPECT_EQ(report.fault, P::FrameFault::Invalid);
    }

    TEST(CobsFramerTest, AFrameLongerThanTheLimitIsDroppedAndTheNextOneDelivered)
    {
        P::CobsFramer framer(4);
        Collector got;
        const Bytes wire = join({ Bytes { 0x07, 1, 2, 3, 4, 5, 6, 0x00 }, encoded_cobs({ 0x68 }) });

        const auto report = framer.push(wire, got.sink());

        EXPECT_EQ(got.messages(), (Texts { "h" }));
        EXPECT_EQ(report.discarded, 1U);
        EXPECT_EQ(report.fault, P::FrameFault::Oversized);
    }

    TEST(CobsFramerTest, ResetDropsAPartialFrame)
    {
        P::CobsFramer framer;
        Collector got;
        (void)framer.push(Bytes { 0x05, 0x11 }, got.sink());
        ASSERT_EQ(framer.buffered(), 2U);

        framer.reset();
        (void)framer.push(encoded_cobs({ 0x43 }), got.sink());

        EXPECT_EQ(got.messages(), (Texts { "C" }));
    }

    TEST(CobsFramerTest, AnyPayloadSurvivesAnyChunking)
    {
        for (uint64_t seed = 1; seed <= 40; ++seed) {
            std::mt19937_64 rng(seed);
            Texts expected;
            Bytes wire;
            for (size_t i = 0; i < 20; ++i) {
                const Bytes payload = random_payload(rng, 600);
                if (payload.empty() && rng() % 2 == 0) {
                    continue;
                }
                expected.push_back(text_of(payload));
                const auto encoded = encoded_cobs(payload);
                wire.insert(wire.end(), encoded.begin(), encoded.end());
            }
            P::CobsFramer framer;
            Collector got;

            const auto report = feed_random(framer, got, wire, rng, 1 + static_cast<size_t>(rng() % 17));

            ASSERT_EQ(got.messages(), expected) << "seed " << seed;
            ASSERT_TRUE(report.ok()) << "seed " << seed;
            ASSERT_EQ(framer.buffered(), 0U) << "seed " << seed;
        }
    }

    // ------------------------------------------------------------------
    // SlipFramer
    // ------------------------------------------------------------------

    TEST(SlipFramerTest, EncodesEscapesAndTheEnd)
    {
        EXPECT_EQ(encoded_slip({}), (Bytes { 0xC0 }));
        EXPECT_EQ(encoded_slip({ 'a', 'b' }), (Bytes { 'a', 'b', 0xC0 }));
        EXPECT_EQ(encoded_slip({ 0xC0 }), (Bytes { 0xDB, 0xDC, 0xC0 }));
        EXPECT_EQ(encoded_slip({ 0xDB }), (Bytes { 0xDB, 0xDD, 0xC0 }));
        EXPECT_EQ(encoded_slip({ 0x01, 0xC0, 0xDB, 0x02 }), (Bytes { 0x01, 0xDB, 0xDC, 0xDB, 0xDD, 0x02, 0xC0 }));
    }

    TEST(SlipFramerTest, ReadsBackWhatItEncodes)
    {
        const std::vector<Bytes> payloads { { 'a' }, { 0xC0 }, { 0xDB, 0xC0, 0xDB }, { 0x00, 0xFF } };
        P::SlipFramer framer;
        Collector got;
        Texts expected;
        for (const auto& payload : payloads) {
            expected.push_back(text_of(payload));
            (void)framer.push(encoded_slip(payload), got.sink());
        }

        EXPECT_EQ(got.messages(), expected);
    }

    TEST(SlipFramerTest, EmptyFramesAndALeadingEndAreSkipped)
    {
        P::SlipFramer framer;
        Collector got;

        const auto report = framer.push(Bytes { 0xC0, 0xC0, 'a', 0xC0, 0xC0 }, got.sink());

        EXPECT_EQ(got.messages(), (Texts { "a" }));
        EXPECT_EQ(report.messages, 1U);
        EXPECT_TRUE(report.ok());
    }

    TEST(SlipFramerTest, AnEscapeMaySplitAcrossPushes)
    {
        P::SlipFramer framer;
        Collector got;

        (void)framer.push(Bytes { 'a', 0xDB }, got.sink());
        (void)framer.push(Bytes { 0xDC, 'b', 0xC0 }, got.sink());

        EXPECT_EQ(got.messages(), (Texts { text_of({ 'a', 0xC0, 'b' }) }));
    }

    TEST(SlipFramerTest, AnEscapeFollowedByAnythingElseDropsTheFrame)
    {
        P::SlipFramer framer;
        Collector got;

        const auto report = framer.push(Bytes { 'a', 0xDB, 0x01, 'b', 0xC0, 'c', 0xC0 }, got.sink());

        EXPECT_EQ(got.messages(), (Texts { "c" }));
        EXPECT_EQ(report.discarded, 1U);
        EXPECT_EQ(report.fault, P::FrameFault::Invalid);
    }

    TEST(SlipFramerTest, AnEscapeFollowedByTheEndDropsTheFrame)
    {
        P::SlipFramer framer;
        Collector got;

        const auto report = framer.push(Bytes { 'a', 0xDB, 0xC0, 'x', 0xC0 }, got.sink());

        EXPECT_EQ(got.messages(), (Texts { "x" }));
        EXPECT_EQ(report.discarded, 1U);
        EXPECT_EQ(report.fault, P::FrameFault::Invalid);
    }

    TEST(SlipFramerTest, ADecodedMessageLongerThanTheLimitIsDropped)
    {
        P::SlipFramer framer(3);
        Collector got;

        const auto report = framer.push(Bytes { 'a', 'b', 'c', 'd', 0xC0, 'e', 0xC0 }, got.sink());

        EXPECT_EQ(got.messages(), (Texts { "e" }));
        EXPECT_EQ(report.discarded, 1U);
        EXPECT_EQ(report.fault, P::FrameFault::Oversized);
    }

    TEST(SlipFramerTest, TheLimitCountsDecodedBytes)
    {
        P::SlipFramer framer(3);
        Collector got;

        const auto report = framer.push(Bytes { 0xDB, 0xDC, 0xDB, 0xDD, 'x', 0xC0 }, got.sink());

        EXPECT_EQ(got.messages(), (Texts { text_of({ 0xC0, 0xDB, 'x' }) }));
        EXPECT_TRUE(report.ok());
    }

    TEST(SlipFramerTest, ResetDropsAPartialFrameAndAPendingEscape)
    {
        P::SlipFramer framer;
        Collector got;
        (void)framer.push(Bytes { 'a', 0xDB }, got.sink());

        framer.reset();
        (void)framer.push(Bytes { 'b', 0xC0 }, got.sink());

        EXPECT_EQ(got.messages(), (Texts { "b" }));
        EXPECT_EQ(framer.buffered(), 0U);
    }

    TEST(SlipFramerTest, AnyPayloadSurvivesAnyChunking)
    {
        for (uint64_t seed = 1; seed <= 40; ++seed) {
            std::mt19937_64 rng(seed);
            Texts expected;
            Bytes wire;
            for (size_t i = 0; i < 20; ++i) {
                Bytes payload = random_payload(rng, 300);
                for (auto& byte : payload) {
                    if (rng() % 7 == 0) {
                        byte = rng() % 2 == 0 ? P::SlipFramer::end : P::SlipFramer::escape;
                    }
                }
                if (payload.empty()) {
                    continue;
                }
                expected.push_back(text_of(payload));
                const auto encoded = encoded_slip(payload);
                wire.insert(wire.end(), encoded.begin(), encoded.end());
            }
            P::SlipFramer framer;
            Collector got;

            const auto report = feed_random(framer, got, wire, rng, 1 + static_cast<size_t>(rng() % 17));

            ASSERT_EQ(got.messages(), expected) << "seed " << seed;
            ASSERT_TRUE(report.ok()) << "seed " << seed;
            ASSERT_EQ(framer.buffered(), 0U) << "seed " << seed;
        }
    }

    // ------------------------------------------------------------------
    // IdleGapFramer
    // ------------------------------------------------------------------

    TEST(IdleGapFramerTest, ReportsItsGap)
    {
        P::IdleGapFramer framer(std::chrono::microseconds(3500));

        EXPECT_EQ(framer.gap(), std::chrono::microseconds(3500));
    }

    TEST(IdleGapFramerTest, PushDeliversNothingAndFlushDeliversEverythingGathered)
    {
        P::IdleGapFramer framer(std::chrono::milliseconds(2));
        Collector got;

        const auto first = feed(framer, got, "ab");
        const auto second = feed(framer, got, "cd");

        EXPECT_EQ(first.messages, 0U);
        EXPECT_EQ(second.messages, 0U);
        EXPECT_TRUE(got.messages().empty());
        EXPECT_EQ(framer.buffered(), 4U);

        const auto flushed = framer.flush(got.sink());

        EXPECT_EQ(flushed.messages, 1U);
        EXPECT_EQ(got.messages(), (Texts { "abcd" }));
        EXPECT_EQ(framer.buffered(), 0U);
    }

    TEST(IdleGapFramerTest, FlushWithNothingGatheredDeliversNothing)
    {
        P::IdleGapFramer framer(std::chrono::milliseconds(2));
        Collector got;

        const auto report = framer.flush(got.sink());

        EXPECT_EQ(report.messages, 0U);
        EXPECT_TRUE(got.messages().empty());
    }

    TEST(IdleGapFramerTest, ASecondFlushDeliversNothingMore)
    {
        P::IdleGapFramer framer(std::chrono::milliseconds(2));
        Collector got;
        (void)feed(framer, got, "ab");

        (void)framer.flush(got.sink());
        (void)framer.flush(got.sink());

        EXPECT_EQ(got.messages(), (Texts { "ab" }));
    }

    TEST(IdleGapFramerTest, AMessageOfExactlyTheLimitIsDelivered)
    {
        P::IdleGapFramer framer(std::chrono::milliseconds(2), 4);
        Collector got;

        const auto pushed = feed(framer, got, "abcd");
        (void)framer.flush(got.sink());

        EXPECT_TRUE(pushed.ok());
        EXPECT_EQ(got.messages(), (Texts { "abcd" }));
    }

    TEST(IdleGapFramerTest, AnOversizedBurstIsDroppedUntilTheNextFlush)
    {
        P::IdleGapFramer framer(std::chrono::milliseconds(2), 4);
        Collector got;

        (void)feed(framer, got, "abc");
        const auto over = feed(framer, got, "def");
        const auto more = feed(framer, got, "gh");
        const auto flushed = framer.flush(got.sink());

        EXPECT_EQ(over.discarded, 1U);
        EXPECT_EQ(over.fault, P::FrameFault::Oversized);
        EXPECT_TRUE(more.ok());
        EXPECT_EQ(flushed.messages, 0U);
        EXPECT_TRUE(got.messages().empty());

        (void)feed(framer, got, "ok");
        (void)framer.flush(got.sink());
        EXPECT_EQ(got.messages(), (Texts { "ok" }));
    }

    TEST(IdleGapFramerTest, ResetDropsWhatWasGatheredAndADiscard)
    {
        P::IdleGapFramer framer(std::chrono::milliseconds(2), 2);
        Collector got;
        (void)feed(framer, got, "abc");

        framer.reset();
        (void)feed(framer, got, "x");
        (void)framer.flush(got.sink());

        EXPECT_EQ(got.messages(), (Texts { "x" }));
    }

    TEST(IdleGapFramerTest, ASinkThatThrowsOnFlushLeavesTheFramerUsable)
    {
        P::IdleGapFramer framer(std::chrono::milliseconds(2));
        Collector got;
        (void)feed(framer, got, "a");

        EXPECT_THROW((void)framer.flush([](P::Message) { throw Boom {}; }), Boom);
        EXPECT_EQ(framer.buffered(), 0U);

        (void)feed(framer, got, "b");
        (void)framer.flush(got.sink());
        EXPECT_EQ(got.messages(), (Texts { "b" }));
    }

}
}
