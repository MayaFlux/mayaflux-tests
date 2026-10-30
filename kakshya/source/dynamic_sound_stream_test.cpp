#include "gtest/gtest.h"

#include "MayaFlux/Kakshya/NDData/DataAccess.hpp"
#include "MayaFlux/Kakshya/Source/DynamicSoundStream.hpp"

namespace MayaFlux::Test {
namespace {

    using Kakshya::DynamicSoundStream;
    using Kakshya::MemoryLayout;
    using Kakshya::OrganizationStrategy;
    using Kakshya::ProcessingState;
    using Kakshya::Region;
    using Kakshya::RegionGroup;

    constexpr uint32_t k_rate = 44100;

    double sample(uint64_t frame, uint32_t channel, double bias = 0.0)
    {
        return bias + 1.0 + 10.0 * static_cast<double>(frame) + static_cast<double>(channel);
    }

    std::vector<double> expected_frame(uint32_t channels, uint64_t frame, double bias = 0.0)
    {
        std::vector<double> values(channels);
        for (uint32_t c = 0; c < channels; ++c) {
            values[c] = sample(frame, c, bias);
        }
        return values;
    }

    std::shared_ptr<DynamicSoundStream> make_stream(OrganizationStrategy organization, uint32_t channels, uint32_t rate = k_rate)
    {
        auto stream = std::make_shared<DynamicSoundStream>(rate, channels);
        stream->get_structure().organization = organization;
        return stream;
    }

    uint64_t write_ramp(DynamicSoundStream& stream, uint64_t frames, uint64_t start = 0, double bias = 0.0)
    {
        const uint32_t channels = stream.get_num_channels();

        if (stream.get_structure().organization == OrganizationStrategy::INTERLEAVED) {
            std::vector<double> flat(frames * channels);
            for (uint64_t f = 0; f < frames; ++f) {
                for (uint32_t c = 0; c < channels; ++c) {
                    flat[f * channels + c] = sample(start + f, c, bias);
                }
            }
            return stream.write_frames(std::vector<std::span<const double>> { std::span<const double>(flat) }, start);
        }

        std::vector<std::vector<double>> columns(channels, std::vector<double>(frames));
        for (uint64_t f = 0; f < frames; ++f) {
            for (uint32_t c = 0; c < channels; ++c) {
                columns[c][f] = sample(start + f, c, bias);
            }
        }
        std::vector<std::span<const double>> spans(columns.begin(), columns.end());
        return stream.write_frames(std::move(spans), start);
    }

    std::vector<double> frame_at(const DynamicSoundStream& stream, uint64_t frame)
    {
        const auto values = stream.get_frame(frame).as<double>();
        return { values.begin(), values.end() };
    }

    std::vector<double> flat_ramp(uint32_t channels, uint64_t first, uint64_t frames, double bias = 0.0)
    {
        std::vector<double> flat;
        for (uint64_t f = first; f < first + frames; ++f) {
            const auto values = expected_frame(channels, f, bias);
            flat.insert(flat.end(), values.begin(), values.end());
        }
        return flat;
    }

    std::vector<double> channel_zero(const DynamicSoundStream& stream, uint64_t first, uint64_t frames)
    {
        std::vector<double> values;
        for (uint64_t f = first; f < first + frames; ++f) {
            values.push_back(frame_at(stream, f)[0]);
        }
        return values;
    }

    struct StreamShape {
        OrganizationStrategy organization;
        uint32_t channels;
    };

    std::string shape_name(const ::testing::TestParamInfo<StreamShape>& info)
    {
        return std::string(info.param.organization == OrganizationStrategy::INTERLEAVED ? "Interleaved" : "Planar")
            + std::to_string(info.param.channels) + "ch";
    }

    class DynamicSoundStreamLayoutTest : public ::testing::TestWithParam<StreamShape> {
    protected:
        void SetUp() override
        {
            stream = make_stream(GetParam().organization, GetParam().channels);
        }

        [[nodiscard]] uint32_t channels() const { return GetParam().channels; }
        [[nodiscard]] bool interleaved() const { return GetParam().organization == OrganizationStrategy::INTERLEAVED; }

        std::shared_ptr<DynamicSoundStream> stream;
    };

    INSTANTIATE_TEST_SUITE_P(Shapes, DynamicSoundStreamLayoutTest,
        ::testing::Values(
            StreamShape { OrganizationStrategy::INTERLEAVED, 1 },
            StreamShape { OrganizationStrategy::INTERLEAVED, 2 },
            StreamShape { OrganizationStrategy::INTERLEAVED, 3 },
            StreamShape { OrganizationStrategy::PLANAR, 1 },
            StreamShape { OrganizationStrategy::PLANAR, 2 },
            StreamShape { OrganizationStrategy::PLANAR, 3 }),
        shape_name);

    TEST_P(DynamicSoundStreamLayoutTest, ConstructionReportsItsParametersAndStartsEmpty)
    {
        EXPECT_EQ(stream->get_sample_rate(), k_rate);
        EXPECT_EQ(stream->get_num_channels(), channels());
        EXPECT_TRUE(stream->get_auto_resize());
        EXPECT_FALSE(stream->is_circular());
        EXPECT_FALSE(stream->is_looping());
        EXPECT_EQ(stream->get_circular_capacity(), 0U);
        EXPECT_EQ(stream->get_num_frames(), 0U);
        EXPECT_EQ(stream->get_total_elements(), 0U);
        EXPECT_FALSE(stream->has_data());
        EXPECT_EQ(stream->get_processing_state(), ProcessingState::IDLE);
    }

    TEST_P(DynamicSoundStreamLayoutTest, WritingReturnsTheFramesAndSizesAnEmptyStreamExactly)
    {
        EXPECT_EQ(write_ramp(*stream, 4), 4U);

        EXPECT_EQ(stream->get_num_frames(), 4U);
        EXPECT_EQ(stream->get_total_elements(), 4U * channels());
        EXPECT_TRUE(stream->has_data());
    }

    TEST_P(DynamicSoundStreamLayoutTest, WrittenFramesReadBackPerChannel)
    {
        write_ramp(*stream, 4);

        for (uint64_t f = 0; f < 4; ++f) {
            EXPECT_EQ(frame_at(*stream, f), expected_frame(channels(), f)) << f;
        }
    }

    TEST_P(DynamicSoundStreamLayoutTest, FramesPastTheEndHaveNoSamples)
    {
        write_ramp(*stream, 4);

        EXPECT_TRUE(stream->get_frame(4).empty());
        EXPECT_TRUE(stream->get_frame(1000).empty());
    }

    TEST_P(DynamicSoundStreamLayoutTest, ALaterBlockLandsAfterTheEarlierOne)
    {
        write_ramp(*stream, 4);

        EXPECT_EQ(write_ramp(*stream, 3, 4), 3U);

        EXPECT_GE(stream->get_num_frames(), 7U);
        for (uint64_t f = 0; f < 7; ++f) {
            EXPECT_EQ(frame_at(*stream, f), expected_frame(channels(), f)) << f;
        }
    }

    TEST_P(DynamicSoundStreamLayoutTest, AnOverlappingBlockReplacesOnlyTheOverlap)
    {
        write_ramp(*stream, 4);

        EXPECT_EQ(write_ramp(*stream, 2, 3, 1000.0), 2U);

        EXPECT_GE(stream->get_num_frames(), 5U);
        for (uint64_t f = 0; f < 3; ++f) {
            EXPECT_EQ(frame_at(*stream, f), expected_frame(channels(), f)) << f;
        }
        EXPECT_EQ(frame_at(*stream, 3), expected_frame(channels(), 3, 1000.0));
        EXPECT_EQ(frame_at(*stream, 4), expected_frame(channels(), 4, 1000.0));
    }

    TEST_P(DynamicSoundStreamLayoutTest, WritingFarPastTheEndGrowsTheStreamAndLeavesSilenceBetween)
    {
        write_ramp(*stream, 2);

        EXPECT_EQ(write_ramp(*stream, 2, 1000), 2U);

        EXPECT_GE(stream->get_num_frames(), 1002U);
        EXPECT_EQ(frame_at(*stream, 1), expected_frame(channels(), 1));
        EXPECT_EQ(frame_at(*stream, 500), std::vector<double>(channels(), 0.0));
        EXPECT_EQ(frame_at(*stream, 1001), expected_frame(channels(), 1001));
    }

    TEST_P(DynamicSoundStreamLayoutTest, EmptyWritesWriteNothing)
    {
        const std::vector<double> nothing;

        EXPECT_EQ(stream->write_frames(std::vector<std::span<const double>> {}, 0), 0U);
        EXPECT_EQ(stream->write_frames(std::vector<std::span<const double>> { std::span<const double>(nothing) }, 0), 0U);
        EXPECT_EQ(stream->write_frames(std::span<const double>(nothing), 0, 0), 0U);
        EXPECT_FALSE(stream->has_data());
    }

    TEST_P(DynamicSoundStreamLayoutTest, WithoutAutoResizeAndWithoutCapacityNothingIsWritten)
    {
        stream->set_auto_resize(false);

        EXPECT_EQ(write_ramp(*stream, 4), 0U);

        EXPECT_FALSE(stream->has_data());
        EXPECT_EQ(stream->get_num_frames(), 0U);
    }

    TEST_P(DynamicSoundStreamLayoutTest, WithoutAutoResizeAWriteIsClippedToTheCapacity)
    {
        stream->set_auto_resize(false);
        stream->ensure_capacity(2);

        EXPECT_EQ(write_ramp(*stream, 4), 2U);

        EXPECT_EQ(stream->get_num_frames(), 2U);
        EXPECT_EQ(frame_at(*stream, 0), expected_frame(channels(), 0));
        EXPECT_EQ(frame_at(*stream, 1), expected_frame(channels(), 1));
    }

    TEST_P(DynamicSoundStreamLayoutTest, WithoutAutoResizeAWriteBeyondTheCapacityIsRefused)
    {
        stream->set_auto_resize(false);
        stream->ensure_capacity(2);

        EXPECT_EQ(write_ramp(*stream, 2, 5), 0U);
    }

    TEST_P(DynamicSoundStreamLayoutTest, AutoResizeCanBeToggled)
    {
        stream->set_auto_resize(false);
        EXPECT_FALSE(stream->get_auto_resize());

        stream->set_auto_resize(true);
        EXPECT_TRUE(stream->get_auto_resize());
    }

    TEST_P(DynamicSoundStreamLayoutTest, EnsureCapacityAllocatesSilentFrames)
    {
        stream->ensure_capacity(100);

        EXPECT_EQ(stream->get_num_frames(), 100U);
        EXPECT_EQ(stream->get_total_elements(), 100U * channels());
        EXPECT_EQ(frame_at(*stream, 0), std::vector<double>(channels(), 0.0));
        EXPECT_EQ(frame_at(*stream, 99), std::vector<double>(channels(), 0.0));
    }

    TEST_P(DynamicSoundStreamLayoutTest, EnsureCapacityNeverShrinksOrDisturbsData)
    {
        write_ramp(*stream, 8);

        stream->ensure_capacity(2);
        stream->ensure_capacity(0);

        EXPECT_EQ(stream->get_num_frames(), 8U);
        EXPECT_EQ(frame_at(*stream, 7), expected_frame(channels(), 7));
    }

    TEST_P(DynamicSoundStreamLayoutTest, GrowingKeepsEarlierFrames)
    {
        write_ramp(*stream, 4);

        stream->ensure_capacity(64);

        EXPECT_GE(stream->get_num_frames(), 64U);
        for (uint64_t f = 0; f < 4; ++f) {
            EXPECT_EQ(frame_at(*stream, f), expected_frame(channels(), f)) << f;
        }
    }

    TEST_P(DynamicSoundStreamLayoutTest, ASingleChannelWriteTouchesOnlyThatChannel)
    {
        const uint32_t target = channels() - 1;
        const std::vector<double> block { 1.5, 2.5, 3.5 };

        EXPECT_EQ(stream->write_frames(std::span<const double>(block), 0, target), 3U);

        for (uint64_t f = 0; f < 3; ++f) {
            const auto frame = frame_at(*stream, f);
            ASSERT_EQ(frame.size(), channels());
            EXPECT_DOUBLE_EQ(frame[target], block[f]) << f;
            for (uint32_t c = 0; c < target; ++c) {
                EXPECT_DOUBLE_EQ(frame[c], 0.0) << f << "," << c;
            }
        }
    }

    TEST_P(DynamicSoundStreamLayoutTest, ASingleChannelWriteHonoursItsStartFrame)
    {
        const std::vector<double> block { 7.0, 8.0 };

        EXPECT_EQ(stream->write_frames(std::span<const double>(block), 3, 0), 2U);

        EXPECT_GE(stream->get_num_frames(), 5U);
        EXPECT_DOUBLE_EQ(frame_at(*stream, 2)[0], 0.0);
        EXPECT_DOUBLE_EQ(frame_at(*stream, 3)[0], 7.0);
        EXPECT_DOUBLE_EQ(frame_at(*stream, 4)[0], 8.0);
    }

    TEST_P(DynamicSoundStreamLayoutTest, ASingleChannelWriteToAMissingChannelIsRefused)
    {
        const std::vector<double> block { 1.0, 2.0 };

        EXPECT_EQ(stream->write_frames(std::span<const double>(block), 0, channels()), 0U);
        EXPECT_FALSE(stream->has_data());
    }

    TEST_P(DynamicSoundStreamLayoutTest, SequentialReadsReturnInterleavedSamplesAndAdvance)
    {
        write_ramp(*stream, 4);
        stream->set_read_position(std::vector<uint64_t>(channels(), 0));
        std::vector<double> out(2 * channels());

        const uint64_t read = stream->read_sequential(out, out.size());

        EXPECT_EQ(read, 2U * channels());
        EXPECT_EQ(out, flat_ramp(channels(), 0, 2));
        EXPECT_EQ(stream->get_read_position(), std::vector<uint64_t>(channels(), 2));
    }

    TEST_P(DynamicSoundStreamLayoutTest, SequentialReadsContinueWhereThePreviousOneStopped)
    {
        write_ramp(*stream, 4);
        stream->set_read_position(std::vector<uint64_t>(channels(), 0));
        std::vector<double> first(2 * channels());
        std::vector<double> second(2 * channels());

        stream->read_sequential(first, first.size());
        const uint64_t read = stream->read_sequential(second, second.size());

        EXPECT_EQ(read, 2U * channels());
        EXPECT_EQ(second, flat_ramp(channels(), 2, 2));
        EXPECT_EQ(stream->get_read_position(), std::vector<uint64_t>(channels(), 4));
        EXPECT_TRUE(stream->is_at_end());
    }

    TEST_P(DynamicSoundStreamLayoutTest, ReadingPastTheEndGivesNothing)
    {
        write_ramp(*stream, 2);
        stream->set_read_position(std::vector<uint64_t>(channels(), 0));
        std::vector<double> out(2 * channels());
        stream->read_sequential(out, out.size());

        std::vector<double> more(2 * channels(), -1.0);
        EXPECT_EQ(stream->read_sequential(more, more.size()), 0U);
        EXPECT_EQ(more, std::vector<double>(more.size(), 0.0));
    }

    TEST_P(DynamicSoundStreamLayoutTest, ReadingAnEmptyStreamGivesNothing)
    {
        std::vector<double> out(4, -1.0);

        EXPECT_EQ(stream->read_sequential(out, 4), 0U);
    }

    TEST_P(DynamicSoundStreamLayoutTest, ReadFramesIsSequentialReading)
    {
        write_ramp(*stream, 2);
        stream->set_read_position(std::vector<uint64_t>(channels(), 0));
        std::vector<double> out(channels());

        EXPECT_EQ(stream->read_frames(out, channels()), channels());

        EXPECT_EQ(out, expected_frame(channels(), 0));
    }

    TEST_P(DynamicSoundStreamLayoutTest, PeekingDoesNotAdvance)
    {
        write_ramp(*stream, 4);
        stream->set_read_position(std::vector<uint64_t>(channels(), 0));
        std::vector<double> a(2 * channels());
        std::vector<double> b(2 * channels());

        stream->peek_sequential(a, a.size());
        stream->peek_sequential(b, b.size());

        EXPECT_EQ(a, b);
        EXPECT_EQ(a, flat_ramp(channels(), 0, 2));
        EXPECT_EQ(stream->get_read_position(), std::vector<uint64_t>(channels(), 0));
    }

    TEST_P(DynamicSoundStreamLayoutTest, PeekingSkipsWholeFramesForAnOffset)
    {
        write_ramp(*stream, 4);
        stream->set_read_position(std::vector<uint64_t>(channels(), 0));
        std::vector<double> out(channels());

        stream->peek_sequential(out, channels(), 2);

        EXPECT_EQ(out, expected_frame(channels(), 2));
    }

    TEST_P(DynamicSoundStreamLayoutTest, ReadPositionsAreSetAndAdvancedPerChannel)
    {
        write_ramp(*stream, 8);
        std::vector<uint64_t> start(channels());
        std::iota(start.begin(), start.end(), uint64_t { 1 });

        stream->set_read_position(start);
        EXPECT_EQ(stream->get_read_position(), start);

        stream->advance_read_position(std::vector<uint64_t>(channels(), 2));
        std::vector<uint64_t> expected = start;
        for (auto& position : expected) {
            position += 2;
        }
        EXPECT_EQ(stream->get_read_position(), expected);
    }

    TEST_P(DynamicSoundStreamLayoutTest, ResettingTheReadPositionRewinds)
    {
        write_ramp(*stream, 8);
        stream->set_read_position(std::vector<uint64_t>(channels(), 5));

        stream->reset_read_position();

        EXPECT_EQ(stream->get_read_position(), std::vector<uint64_t>(channels(), 0));
    }

    TEST_P(DynamicSoundStreamLayoutTest, RemainingFramesCountDownToTheEnd)
    {
        write_ramp(*stream, 8);
        stream->set_read_position(std::vector<uint64_t>(channels(), 3));

        EXPECT_EQ(stream->get_remaining_frames(), std::vector<uint64_t>(channels(), 5));
    }

    TEST_P(DynamicSoundStreamLayoutTest, GatherCopiesChosenFramesAndZeroesMissingOnes)
    {
        write_ramp(*stream, 4);
        const std::vector<uint64_t> frames { 2, 0, 99 };
        std::vector<double> out(frames.size() * channels(), -1.0);

        stream->gather_frames(frames, out);

        std::vector<double> expected = expected_frame(channels(), 2);
        const auto zeroth = expected_frame(channels(), 0);
        expected.insert(expected.end(), zeroth.begin(), zeroth.end());
        expected.insert(expected.end(), channels(), 0.0);
        EXPECT_EQ(out, expected);
    }

    TEST_P(DynamicSoundStreamLayoutTest, GatherAllowsRepeatsAndAnyOrder)
    {
        write_ramp(*stream, 4);
        const std::vector<uint64_t> frames { 3, 3, 1 };
        std::vector<double> out(frames.size() * channels());

        stream->gather_frames(frames, out);

        std::vector<double> expected;
        for (const uint64_t f : frames) {
            const auto values = expected_frame(channels(), f);
            expected.insert(expected.end(), values.begin(), values.end());
        }
        EXPECT_EQ(out, expected);
    }

    TEST_P(DynamicSoundStreamLayoutTest, RegionReadsReturnTheSelectedFrames)
    {
        write_ramp(*stream, 4);
        const Region region(std::vector<uint64_t> { 1, 0 }, std::vector<uint64_t> { 2, channels() - 1 });

        const auto data = stream->get_region_data(region);

        if (interleaved()) {
            ASSERT_EQ(data.size(), 1U);
            EXPECT_EQ(std::get<std::vector<double>>(data[0]), flat_ramp(channels(), 1, 2));
        } else {
            ASSERT_EQ(data.size(), channels());
            for (uint32_t c = 0; c < channels(); ++c) {
                EXPECT_EQ(std::get<std::vector<double>>(data[c]), (std::vector<double> { sample(1, c), sample(2, c) })) << c;
            }
        }
    }

    TEST_P(DynamicSoundStreamLayoutTest, ChannelAccessorsExposeTheStoredVariants)
    {
        write_ramp(*stream, 4);

        const auto all = stream->all_channel_data();

        const size_t stored = interleaved() ? 1U : channels();
        ASSERT_EQ(all.size(), stored);
        for (const auto& access : all) {
            EXPECT_EQ(access.element_count(), interleaved() ? 4U * channels() : 4U);
        }
        EXPECT_NO_THROW((void)stream->channel_data(stored - 1));
        EXPECT_THROW((void)stream->channel_data(stored), std::out_of_range);
    }

    TEST_P(DynamicSoundStreamLayoutTest, ChannelSpansExistOnlyForPlanarStorage)
    {
        write_ramp(*stream, 4);
        const uint32_t c = channels() - 1;

        const auto span = stream->get_channel_frames(c, 1, 2);

        if (interleaved()) {
            EXPECT_TRUE(span.empty());
        } else {
            EXPECT_EQ(std::vector<double>(span.begin(), span.end()), (std::vector<double> { sample(1, c), sample(2, c) }));
            EXPECT_EQ(stream->get_channel_frames(c, 3, 10).size(), 1U);
            EXPECT_TRUE(stream->get_channel_frames(c, 4, 1).empty());
        }
        EXPECT_TRUE(stream->get_channel_frames(channels(), 0, 1).empty());
    }

    TEST_P(DynamicSoundStreamLayoutTest, CopyingAChannelWorksInBothLayoutsAndZeroPadsPastTheEnd)
    {
        write_ramp(*stream, 4);
        const uint32_t c = channels() - 1;
        std::vector<double> out(4, -1.0);

        stream->get_channel_frames(out, c, 2);

        EXPECT_EQ(out, (std::vector<double> { sample(2, c), sample(3, c), 0.0, 0.0 }));
    }

    TEST_P(DynamicSoundStreamLayoutTest, CopyingFromAMissingChannelLeavesTheOutputAlone)
    {
        write_ramp(*stream, 4);
        std::vector<double> out(2, -1.0);

        stream->get_channel_frames(out, channels(), 0);

        EXPECT_EQ(out, (std::vector<double> { -1.0, -1.0 }));
    }

    TEST_P(DynamicSoundStreamLayoutTest, ClearEmptiesTheStreamAndRewindsReading)
    {
        write_ramp(*stream, 4);
        stream->set_read_position(std::vector<uint64_t>(channels(), 3));

        stream->clear();

        EXPECT_FALSE(stream->has_data());
        EXPECT_EQ(stream->get_num_frames(), 0U);
        EXPECT_EQ(stream->get_total_elements(), 0U);
        EXPECT_EQ(stream->get_read_position(), std::vector<uint64_t>(channels(), 0));
        EXPECT_EQ(stream->get_processing_state(), ProcessingState::IDLE);
    }

    TEST_P(DynamicSoundStreamLayoutTest, WritingMakesTheStreamReady)
    {
        EXPECT_FALSE(stream->is_ready());

        write_ramp(*stream, 4);

        EXPECT_EQ(stream->get_processing_state(), ProcessingState::READY);
        EXPECT_TRUE(stream->is_ready());
        EXPECT_TRUE(stream->is_ready_for_processing());
    }

    TEST_P(DynamicSoundStreamLayoutTest, StateChangesReachTheCallbackUntilItIsRemoved)
    {
        std::vector<ProcessingState> seen;
        stream->register_state_change_callback(
            [&seen](const std::shared_ptr<Kakshya::SignalSourceContainer>&, ProcessingState state) { seen.push_back(state); });

        write_ramp(*stream, 4);
        stream->update_processing_state(ProcessingState::PROCESSED);
        stream->unregister_state_change_callback();
        stream->update_processing_state(ProcessingState::IDLE);

        EXPECT_EQ(seen, (std::vector<ProcessingState> { ProcessingState::READY, ProcessingState::PROCESSED }));
    }

    TEST_P(DynamicSoundStreamLayoutTest, MarkingReadyNeedsDataAndUnmarkingGoesIdle)
    {
        stream->mark_ready_for_processing(true);
        EXPECT_EQ(stream->get_processing_state(), ProcessingState::IDLE);

        write_ramp(*stream, 4);
        stream->update_processing_state(ProcessingState::PROCESSED);
        stream->mark_ready_for_processing(true);
        EXPECT_EQ(stream->get_processing_state(), ProcessingState::READY);

        stream->mark_ready_for_processing(false);
        EXPECT_EQ(stream->get_processing_state(), ProcessingState::IDLE);
    }

    TEST_P(DynamicSoundStreamLayoutTest, TimeAndPositionConvertAtTheSampleRate)
    {
        EXPECT_EQ(stream->time_to_position(0.5), k_rate / 2U);
        EXPECT_DOUBLE_EQ(stream->position_to_time(k_rate), 1.0);
        EXPECT_EQ(stream->get_temporal_rate(), k_rate);
        EXPECT_EQ(stream->time_to_position(stream->position_to_time(k_rate / 2U)), k_rate / 2U);
    }

    TEST_P(DynamicSoundStreamLayoutTest, CoordinatesMapToLinearIndicesFrameMajor)
    {
        if (channels() == 1) {
            GTEST_SKIP() << "mono streams have no channel coordinate";
        }
        write_ramp(*stream, 4);
        const uint64_t c = channels() - 1;

        const uint64_t linear = stream->coordinates_to_linear_index({ 2, c });

        EXPECT_EQ(linear, 2U * channels() + c);
        EXPECT_EQ(stream->linear_index_to_coordinates(linear), (std::vector<uint64_t> { 2, c }));
    }

    TEST_P(DynamicSoundStreamLayoutTest, RingEnablingFixesTheCapacityAndLoops)
    {
        stream->enable_circular_buffer(4);

        EXPECT_TRUE(stream->is_circular());
        EXPECT_TRUE(stream->is_looping());
        EXPECT_EQ(stream->get_circular_capacity(), 4U);
        EXPECT_EQ(stream->get_num_frames(), 4U);
        const Region loop = stream->get_loop_region();
        EXPECT_EQ(loop.start_coordinates, (std::vector<uint64_t> { 0, 0 }));
        EXPECT_EQ(loop.end_coordinates, (std::vector<uint64_t> { 3, channels() - 1 }));
    }

    TEST_P(DynamicSoundStreamLayoutTest, RingDisablingKeepsTheData)
    {
        stream->enable_circular_buffer(4);
        write_ramp(*stream, 4);

        stream->disable_circular_buffer();

        EXPECT_FALSE(stream->is_circular());
        EXPECT_FALSE(stream->is_looping());
        EXPECT_EQ(stream->get_circular_capacity(), 0U);
        EXPECT_EQ(frame_at(*stream, 3), expected_frame(channels(), 3));
    }

    TEST_P(DynamicSoundStreamLayoutTest, ARingWriteWithinCapacityIsLinear)
    {
        stream->enable_circular_buffer(4);

        EXPECT_EQ(write_ramp(*stream, 4), 4U);

        for (uint64_t f = 0; f < 4; ++f) {
            EXPECT_EQ(frame_at(*stream, f), expected_frame(channels(), f)) << f;
        }
    }

    TEST_P(DynamicSoundStreamLayoutTest, ARingWriteBeyondCapacityWrapsAndOverwritesTheOldest)
    {
        stream->enable_circular_buffer(4);

        EXPECT_EQ(write_ramp(*stream, 6), 6U);

        EXPECT_EQ(stream->get_num_frames(), 4U);
        EXPECT_EQ(frame_at(*stream, 0), expected_frame(channels(), 4));
        EXPECT_EQ(frame_at(*stream, 1), expected_frame(channels(), 5));
        EXPECT_EQ(frame_at(*stream, 2), expected_frame(channels(), 2));
        EXPECT_EQ(frame_at(*stream, 3), expected_frame(channels(), 3));
    }

    TEST_P(DynamicSoundStreamLayoutTest, ARingLoopsWhenReadPastItsEnd)
    {
        stream->enable_circular_buffer(4);
        write_ramp(*stream, 4);
        stream->set_read_position(std::vector<uint64_t>(channels(), 0));
        std::vector<double> out(6 * channels());

        stream->peek_sequential(out, out.size());

        std::vector<double> expected;
        for (uint64_t f = 0; f < 6; ++f) {
            const auto values = expected_frame(channels(), f % 4);
            expected.insert(expected.end(), values.begin(), values.end());
        }
        EXPECT_EQ(out, expected);
    }

    TEST_P(DynamicSoundStreamLayoutTest, ARingNeverReachesItsEnd)
    {
        stream->enable_circular_buffer(4);
        write_ramp(*stream, 4);
        stream->set_read_position(std::vector<uint64_t>(channels(), 0));
        std::vector<double> out(3 * channels());

        std::vector<std::vector<double>> reads;
        for (int i = 0; i < 3; ++i) {
            stream->read_sequential(out, out.size());
            reads.push_back(out);
            EXPECT_FALSE(stream->is_at_end());
        }

        EXPECT_EQ(reads[0], flat_ramp(channels(), 0, 3));
        std::vector<double> second = expected_frame(channels(), 3);
        for (const uint64_t f : { 0U, 1U }) {
            const auto values = expected_frame(channels(), f);
            second.insert(second.end(), values.begin(), values.end());
        }
        EXPECT_EQ(reads[1], second);
        EXPECT_EQ(std::vector<double>(reads[2].begin(), reads[2].begin() + channels()), expected_frame(channels(), 2));
    }

    TEST_P(DynamicSoundStreamLayoutTest, ARingReportsUnboundedRemainingFrames)
    {
        stream->enable_circular_buffer(4);

        EXPECT_EQ(stream->get_remaining_frames(), std::vector<uint64_t>(channels(), std::numeric_limits<uint64_t>::max()));
    }

    TEST_P(DynamicSoundStreamLayoutTest, AppendWritesAtTheHeadAndAdvancesIt)
    {
        const std::vector<double> first { 1, 2, 3 };
        const std::vector<double> second { 4, 5 };

        EXPECT_EQ(stream->append_frames(std::span<const double>(first), 0), 3U);
        EXPECT_EQ(stream->get_write_head(0), 3U);
        EXPECT_EQ(stream->append_frames(std::span<const double>(second), 0), 2U);

        EXPECT_EQ(stream->get_write_head(0), 5U);
        EXPECT_EQ(channel_zero(*stream, 0, 5), (std::vector<double> { 1, 2, 3, 4, 5 }));
    }

    TEST_P(DynamicSoundStreamLayoutTest, ChannelsKeepIndependentHeads)
    {
        const std::vector<double> block { 1, 2, 3 };

        stream->append_frames(std::span<const double>(block), 0);

        EXPECT_EQ(stream->get_write_head(0), 3U);
        for (uint32_t c = 1; c < channels(); ++c) {
            EXPECT_EQ(stream->get_write_head(c), 0U) << c;
        }
    }

    TEST_P(DynamicSoundStreamLayoutTest, AppendIgnoresEmptyBlocksAndMissingChannels)
    {
        const std::vector<double> block { 1, 2 };

        EXPECT_EQ(stream->append_frames(std::span<const double>(), 0), 0U);
        EXPECT_EQ(stream->append_frames(std::span<const double>(block), channels()), 0U);
        EXPECT_EQ(stream->get_write_head(channels()), 0U);
        EXPECT_FALSE(stream->has_data());
    }

    TEST_P(DynamicSoundStreamLayoutTest, WritingFramesDoesNotMoveTheAppendHead)
    {
        write_ramp(*stream, 4);

        EXPECT_EQ(stream->get_write_head(0), 0U);
    }

    TEST_P(DynamicSoundStreamLayoutTest, AppendingToARingWrapsTheHeadAndOverwritesTheOldest)
    {
        stream->enable_circular_buffer(4);
        const std::vector<double> first { 1, 2, 3 };
        const std::vector<double> second { 4, 5, 6 };

        stream->append_frames(std::span<const double>(first), 0);
        EXPECT_EQ(stream->append_frames(std::span<const double>(second), 0), 3U);

        EXPECT_EQ(stream->get_write_head(0), 2U);
        EXPECT_EQ(channel_zero(*stream, 0, 4), (std::vector<double> { 5, 6, 3, 4 }));
    }

    TEST_P(DynamicSoundStreamLayoutTest, AppendingMoreThanTheRingHoldsKeepsTheNewestFrames)
    {
        stream->enable_circular_buffer(4);
        const std::vector<double> block { 1, 2, 3, 4, 5, 6 };

        EXPECT_EQ(stream->append_frames(std::span<const double>(block), 0), 4U);

        EXPECT_EQ(stream->get_write_head(0), 2U);
        EXPECT_EQ(channel_zero(*stream, 0, 4), (std::vector<double> { 5, 6, 3, 4 }));
    }

    TEST_P(DynamicSoundStreamLayoutTest, EnablingARingFoldsExistingHeadsIntoIt)
    {
        const std::vector<double> block { 1, 2, 3, 4, 5, 6 };
        stream->append_frames(std::span<const double>(block), 0);
        ASSERT_EQ(stream->get_write_head(0), 6U);

        stream->enable_circular_buffer(4);

        EXPECT_EQ(stream->get_write_head(0), 2U);
    }

    class DynamicSoundStreamSnapshotTest : public DynamicSoundStreamLayoutTest {
    protected:
        void append_block(std::vector<double> values)
        {
            for (uint32_t c = 0; c < channels(); ++c) {
                std::vector<double> shifted = values;
                for (double& v : shifted) {
                    v += 100.0 * c;
                }
                stream->append_frames(std::span<const double>(shifted), c);
            }
        }

        [[nodiscard]] std::vector<double> snapshot_channel(const std::shared_ptr<DynamicSoundStream>& copy, uint32_t channel) const
        {
            std::vector<double> values;
            for (uint64_t f = 0; f < copy->get_num_frames(); ++f) {
                values.push_back(frame_at(*copy, f)[channel]);
            }
            return values;
        }
    };

    INSTANTIATE_TEST_SUITE_P(Shapes, DynamicSoundStreamSnapshotTest,
        ::testing::Values(
            StreamShape { OrganizationStrategy::INTERLEAVED, 1 },
            StreamShape { OrganizationStrategy::INTERLEAVED, 2 },
            StreamShape { OrganizationStrategy::PLANAR, 1 },
            StreamShape { OrganizationStrategy::PLANAR, 2 }),
        shape_name);

    TEST_P(DynamicSoundStreamSnapshotTest, ALinearSnapshotKeepsTheMostRecentFramesOldestFirst)
    {
        append_block({ 1, 2, 3, 4, 5 });

        const auto copy = stream->snapshot(3);

        ASSERT_NE(copy, nullptr);
        EXPECT_EQ(copy->get_num_frames(), 3U);
        EXPECT_EQ(copy->get_num_channels(), channels());
        EXPECT_EQ(copy->get_sample_rate(), k_rate);
        for (uint32_t c = 0; c < channels(); ++c) {
            EXPECT_EQ(snapshot_channel(copy, c), (std::vector<double> { 3.0 + 100 * c, 4.0 + 100 * c, 5.0 + 100 * c })) << c;
        }
    }

    TEST_P(DynamicSoundStreamSnapshotTest, ASnapshotIsCappedAtWhatWasWritten)
    {
        append_block({ 1, 2, 3 });

        const auto copy = stream->snapshot(100);

        ASSERT_NE(copy, nullptr);
        EXPECT_EQ(copy->get_num_frames(), 3U);
    }

    TEST_P(DynamicSoundStreamSnapshotTest, NothingToSnapshotGivesNull)
    {
        EXPECT_EQ(stream->snapshot(4), nullptr);

        append_block({ 1, 2 });
        EXPECT_EQ(stream->snapshot(0), nullptr);
    }

    TEST_P(DynamicSoundStreamSnapshotTest, ASnapshotIsIndependentOfTheSource)
    {
        append_block({ 1, 2, 3 });
        const auto copy = stream->snapshot(3);

        append_block({ 9, 9, 9 });

        EXPECT_EQ(snapshot_channel(copy, 0), (std::vector<double> { 1, 2, 3 }));
    }

    TEST_P(DynamicSoundStreamSnapshotTest, ARingSnapshotUnrollsTheWrap)
    {
        stream->enable_circular_buffer(4);
        append_block({ 1, 2, 3 });
        append_block({ 4, 5, 6 });

        const auto all = stream->snapshot(4);
        const auto newest = stream->snapshot(2);

        ASSERT_NE(all, nullptr);
        ASSERT_NE(newest, nullptr);
        EXPECT_EQ(snapshot_channel(all, 0), (std::vector<double> { 3, 4, 5, 6 }));
        EXPECT_EQ(snapshot_channel(newest, 0), (std::vector<double> { 5, 6 }));
    }

    TEST_P(DynamicSoundStreamSnapshotTest, APartlyFilledRingPadsTheFrontWithSilence)
    {
        stream->enable_circular_buffer(4);
        append_block({ 1, 2 });

        const auto copy = stream->snapshot(4);

        ASSERT_NE(copy, nullptr);
        EXPECT_EQ(snapshot_channel(copy, 0), (std::vector<double> { 0, 0, 1, 2 }));
    }

    TEST(DynamicSoundStreamTest, DefaultsToTwoChannelsAtTheGivenRate)
    {
        const auto stream = std::make_shared<DynamicSoundStream>(44100);

        EXPECT_EQ(stream->get_sample_rate(), 44100U);
        EXPECT_EQ(stream->get_num_channels(), 2U);
        EXPECT_TRUE(stream->get_auto_resize());
        EXPECT_FALSE(stream->is_circular());
    }

    TEST(DynamicSoundStreamTest, NewStreamsAreOrganisedPlanar)
    {
        EXPECT_EQ(DynamicSoundStream(48000, 2).get_structure().organization, OrganizationStrategy::PLANAR);
    }

    TEST(DynamicSoundStreamTest, InterleavedStreamsStoreSamplesFrameMajor)
    {
        const auto stream = make_stream(OrganizationStrategy::INTERLEAVED, 2);
        const std::vector<double> samples { 1.0, 2.0, 3.0, 4.0 };

        stream->write_frames(std::vector<std::span<const double>> { std::span<const double>(samples) }, 0);
        std::vector<double> out(4);
        stream->set_read_position({ 0, 0 });
        stream->peek_sequential(out, 4);

        EXPECT_EQ(out, samples);
        EXPECT_EQ(stream->get_frame(0).as<double>().size(), 2U);
        EXPECT_EQ(frame_at(*stream, 0), (std::vector<double> { 1.0, 2.0 }));
        EXPECT_EQ(frame_at(*stream, 1), (std::vector<double> { 3.0, 4.0 }));
    }

    TEST(DynamicSoundStreamTest, AnIncompleteTrailingFrameOfInterleavedSamplesIsDropped)
    {
        const auto stream = make_stream(OrganizationStrategy::INTERLEAVED, 2);
        const std::vector<double> samples { 0.1, 0.2, 0.3 };

        EXPECT_EQ(stream->write_frames(std::vector<std::span<const double>> { std::span<const double>(samples) }, 0), 1U);

        EXPECT_EQ(frame_at(*stream, 0), (std::vector<double> { 0.1, 0.2 }));
    }

    TEST(DynamicSoundStreamTest, PlanarWritesTakeOneSpanPerChannel)
    {
        const auto stream = make_stream(OrganizationStrategy::PLANAR, 2);
        const std::vector<double> left { 0.1, 0.3, 0.5, 0.7 };
        const std::vector<double> right { 0.2, 0.4, 0.6, 0.8 };

        EXPECT_EQ(stream->write_frames(std::vector<std::span<const double>> { std::span<const double>(left), std::span<const double>(right) }, 0), 4U);

        EXPECT_EQ(frame_at(*stream, 0), (std::vector<double> { 0.1, 0.2 }));
        EXPECT_EQ(frame_at(*stream, 1), (std::vector<double> { 0.3, 0.4 }));
        EXPECT_EQ(frame_at(*stream, 3), (std::vector<double> { 0.7, 0.8 }));
    }

    TEST(DynamicSoundStreamTest, PlanarWritesNeedEveryChannel)
    {
        const auto stream = make_stream(OrganizationStrategy::PLANAR, 2);
        const std::vector<double> only { 0.1, 0.2 };

        EXPECT_EQ(stream->write_frames(std::vector<std::span<const double>> { std::span<const double>(only) }, 0), 0U);
        EXPECT_FALSE(stream->has_data());
    }

    TEST(DynamicSoundStreamTest, PlanarWritesNeedMatchingChannelLengths)
    {
        const auto stream = make_stream(OrganizationStrategy::PLANAR, 2);
        const std::vector<double> left { 0.1, 0.2, 0.3 };
        const std::vector<double> right { 0.1, 0.2 };

        EXPECT_EQ(stream->write_frames(std::vector<std::span<const double>> { std::span<const double>(left), std::span<const double>(right) }, 0), 0U);
        EXPECT_FALSE(stream->has_data());
    }

    TEST(DynamicSoundStreamTest, ZeroCapacityRingsAndCapacitiesDoNotThrow)
    {
        const auto stream = make_stream(OrganizationStrategy::INTERLEAVED, 2);

        EXPECT_NO_THROW(stream->ensure_capacity(0));
        EXPECT_NO_THROW(stream->enable_circular_buffer(0));
    }

    TEST(DynamicSoundStreamTest, DynamicSlotsAreUniqueAndReusedAfterRelease)
    {
        const auto stream = make_stream(OrganizationStrategy::PLANAR, 2);

        const uint32_t a = stream->allocate_dynamic_slot();
        const uint32_t b = stream->allocate_dynamic_slot();
        stream->release_dynamic_slot(a);
        const uint32_t c = stream->allocate_dynamic_slot();

        EXPECT_NE(a, b);
        EXPECT_EQ(c, a);
    }

    TEST(DynamicSoundStreamTest, DynamicSlotsStartEmptyAndHoldIndependentData)
    {
        const auto stream = make_stream(OrganizationStrategy::PLANAR, 2);
        const uint32_t a = stream->allocate_dynamic_slot();
        const uint32_t b = stream->allocate_dynamic_slot();
        EXPECT_TRUE(stream->get_dynamic_data(a).empty());

        stream->get_dynamic_data(a).push_back(std::vector<double> { 1.0 });

        EXPECT_EQ(stream->get_dynamic_data(a).size(), 1U);
        EXPECT_TRUE(stream->get_dynamic_data(b).empty());
    }

    TEST(DynamicSoundStreamTest, ReleasingASlotClearsItsData)
    {
        const auto stream = make_stream(OrganizationStrategy::PLANAR, 2);
        const uint32_t a = stream->allocate_dynamic_slot();
        stream->get_dynamic_data(a).push_back(std::vector<double> { 1.0 });

        stream->release_dynamic_slot(a);

        EXPECT_TRUE(stream->get_dynamic_data(a).empty());
        EXPECT_EQ(stream->allocate_dynamic_slot(), a);
        EXPECT_TRUE(stream->get_dynamic_data(a).empty());
    }

    TEST(DynamicSoundStreamTest, UnknownDynamicSlotsAreRefused)
    {
        const auto stream = make_stream(OrganizationStrategy::PLANAR, 2);

        EXPECT_THROW((void)stream->get_dynamic_data(0), std::out_of_range);
        EXPECT_NO_THROW(stream->release_dynamic_slot(7));
    }

    TEST(DynamicSoundStreamTest, ReadersAreTrackedPerDimension)
    {
        const auto stream = make_stream(OrganizationStrategy::PLANAR, 2);
        EXPECT_FALSE(stream->has_active_readers());

        const uint32_t first = stream->register_dimension_reader(0);
        const uint32_t second = stream->register_dimension_reader(0);

        EXPECT_TRUE(stream->has_active_readers());
        EXPECT_NE(first, second);

        stream->unregister_dimension_reader(0);
        EXPECT_TRUE(stream->has_active_readers());
        stream->unregister_dimension_reader(0);
        EXPECT_FALSE(stream->has_active_readers());
    }

    TEST(DynamicSoundStreamTest, ADimensionIsConsumedOnceEveryReaderHasMarkedIt)
    {
        const auto stream = make_stream(OrganizationStrategy::PLANAR, 2);
        const uint32_t first = stream->register_dimension_reader(0);
        const uint32_t second = stream->register_dimension_reader(0);

        EXPECT_FALSE(stream->all_dimensions_consumed());
        stream->mark_dimension_consumed(0, first);
        EXPECT_FALSE(stream->all_dimensions_consumed());
        stream->mark_dimension_consumed(0, second);
        EXPECT_TRUE(stream->all_dimensions_consumed());
    }

    TEST(DynamicSoundStreamTest, ConsumptionCanBeClearedForTheNextCycle)
    {
        const auto stream = make_stream(OrganizationStrategy::PLANAR, 2);
        const uint32_t reader = stream->register_dimension_reader(1);
        stream->mark_dimension_consumed(1, reader);
        ASSERT_TRUE(stream->all_dimensions_consumed());

        stream->clear_all_consumption();

        EXPECT_FALSE(stream->all_dimensions_consumed());
    }

    TEST(DynamicSoundStreamTest, MarkingForAnUnknownReaderChangesNothing)
    {
        const auto stream = make_stream(OrganizationStrategy::PLANAR, 2);
        stream->register_dimension_reader(0);

        stream->mark_dimension_consumed(0, 99);

        EXPECT_FALSE(stream->all_dimensions_consumed());
    }

    TEST(DynamicSoundStreamTest, WithNoReadersEveryDimensionCountsAsConsumed)
    {
        EXPECT_TRUE(make_stream(OrganizationStrategy::PLANAR, 2)->all_dimensions_consumed());
    }

    TEST(DynamicSoundStreamTest, ProcessingTokensAreHeldByOneChannelAtATime)
    {
        const auto stream = make_stream(OrganizationStrategy::PLANAR, 2);

        EXPECT_TRUE(stream->try_acquire_processing_token(0));
        EXPECT_FALSE(stream->try_acquire_processing_token(1));
        EXPECT_TRUE(stream->has_processing_token(0));
        EXPECT_FALSE(stream->has_processing_token(1));

        stream->reset_processing_token();

        EXPECT_FALSE(stream->has_processing_token(0));
        EXPECT_TRUE(stream->try_acquire_processing_token(1));
    }

    TEST(DynamicSoundStreamTest, RegionGroupsAreStoredByNameAndRemovable)
    {
        const auto stream = make_stream(OrganizationStrategy::PLANAR, 2);
        RegionGroup group("hits");
        group.add_region(Region(std::vector<uint64_t> { 0, 0 }, std::vector<uint64_t> { 3, 1 }));

        stream->add_region_group(group);

        EXPECT_EQ(stream->get_region_group("hits").regions.size(), 1U);
        EXPECT_EQ(stream->get_all_region_groups().size(), 1U);

        stream->remove_region_group("hits");

        EXPECT_TRUE(stream->get_all_region_groups().empty());
        EXPECT_TRUE(stream->get_region_group("hits").regions.empty());
    }

    TEST(DynamicSoundStreamTest, LoopRegionsAreStoredAndLoopingToggles)
    {
        const auto stream = make_stream(OrganizationStrategy::INTERLEAVED, 2);
        write_ramp(*stream, 8);
        const Region region(std::vector<uint64_t> { 2, 0 }, std::vector<uint64_t> { 5, 1 });

        stream->set_loop_region(region);
        stream->set_looping(true);

        EXPECT_TRUE(stream->is_looping());
        EXPECT_EQ(stream->get_loop_region().start_coordinates, region.start_coordinates);
        EXPECT_EQ(stream->get_loop_region().end_coordinates, region.end_coordinates);

        stream->set_looping(false);
        EXPECT_FALSE(stream->is_looping());
    }

    TEST(DynamicSoundStreamTest, ALoopRegionPullsAnOutsideReadPositionBackInside)
    {
        const auto stream = make_stream(OrganizationStrategy::INTERLEAVED, 2);
        write_ramp(*stream, 8);
        stream->set_looping(true);
        stream->set_read_position({ 0, 0 });

        stream->set_loop_region(Region(std::vector<uint64_t> { 2, 0 }, std::vector<uint64_t> { 5, 1 }));

        EXPECT_EQ(stream->get_read_position(), (std::vector<uint64_t> { 2, 0 }));
    }

    TEST(DynamicSoundStreamTest, MonoStreamsAreOneDimensional)
    {
        const auto stream = make_stream(OrganizationStrategy::PLANAR, 1);
        write_ramp(*stream, 4);

        EXPECT_EQ(stream->get_dimensions().size(), 1U);
        EXPECT_EQ(stream->get_structure().modality, Kakshya::DataModality::AUDIO_1D);
    }

    TEST(DynamicSoundStreamTest, MultichannelStreamsAreFramesByChannels)
    {
        const auto stream = make_stream(OrganizationStrategy::PLANAR, 2);
        write_ramp(*stream, 4);

        const auto dims = stream->get_dimensions();

        ASSERT_EQ(dims.size(), 2U);
        EXPECT_EQ(dims[0].size, 4U);
        EXPECT_EQ(dims[1].size, 2U);
        EXPECT_EQ(stream->get_structure().modality, Kakshya::DataModality::AUDIO_MULTICHANNEL);
        EXPECT_EQ(stream->get_frame_size(), 2U);
        EXPECT_EQ(stream->get_memory_layout(), MemoryLayout::ROW_MAJOR);
    }

    TEST(DynamicSoundStreamTest, ElementValuesAreDoubles)
    {
        EXPECT_EQ(make_stream(OrganizationStrategy::PLANAR, 2)->value_element_type(), std::type_index(typeid(double)));
    }

} // namespace
} // namespace MayaFlux::Test
