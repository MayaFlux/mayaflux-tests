#include "gtest/gtest.h"

#include "MayaFlux/Buffers/AudioBuffer.hpp"
#include "MayaFlux/Buffers/Container/SoundStreamWriter.hpp"
#include "MayaFlux/Kakshya/Source/DynamicSoundStream.hpp"

namespace MayaFlux::Test {
namespace {

    using Buffers::AudioBuffer;
    using Buffers::SoundStreamWriter;
    using Kakshya::DynamicSoundStream;

    constexpr uint32_t k_rate = 48000;

    std::shared_ptr<AudioBuffer> make_buffer(uint32_t channel, std::vector<double> samples)
    {
        auto buffer = std::make_shared<AudioBuffer>(channel, static_cast<uint32_t>(samples.size()));
        buffer->get_data() = std::move(samples);
        return buffer;
    }

    std::vector<double> channel_samples(const DynamicSoundStream& stream, uint32_t channel, uint64_t first, uint64_t count)
    {
        const auto span = stream.get_channel_frames(channel, first, count);
        return { span.begin(), span.end() };
    }

    class SoundStreamWriterTest : public ::testing::Test {
    protected:
        void SetUp() override
        {
            stream = std::make_shared<DynamicSoundStream>(k_rate, 2);
            writer = std::make_shared<SoundStreamWriter>(stream);
        }

        std::shared_ptr<DynamicSoundStream> stream;
        std::shared_ptr<SoundStreamWriter> writer;
    };

    TEST_F(SoundStreamWriterTest, KeepsItsContainerAndStartsAtTheGivenPosition)
    {
        const SoundStreamWriter shifted(stream, 10);

        EXPECT_EQ(writer->get_container(), stream);
        EXPECT_EQ(writer->get_write_position(), 0U);
        EXPECT_EQ(shifted.get_write_position(), 10U);
    }

    TEST_F(SoundStreamWriterTest, WritingPutsTheBufferIntoItsChannelAndAdvances)
    {
        writer->processing_function(make_buffer(0, { 0.1, 0.2, 0.3, 0.4 }));

        EXPECT_EQ(stream->get_num_frames(), 4U);
        EXPECT_EQ(channel_samples(*stream, 0, 0, 4), (std::vector<double> { 0.1, 0.2, 0.3, 0.4 }));
        EXPECT_EQ(channel_samples(*stream, 1, 0, 4), (std::vector<double> { 0, 0, 0, 0 }));
        EXPECT_EQ(writer->get_write_position(), 4U);
    }

    TEST_F(SoundStreamWriterTest, ConsecutiveBuffersAccumulate)
    {
        writer->processing_function(make_buffer(0, { 1, 2, 3, 4 }));
        writer->processing_function(make_buffer(0, { 5, 6, 7, 8 }));

        EXPECT_EQ(channel_samples(*stream, 0, 0, 8), (std::vector<double> { 1, 2, 3, 4, 5, 6, 7, 8 }));
        EXPECT_EQ(writer->get_write_position(), 8U);
    }

    TEST_F(SoundStreamWriterTest, TheBuffersChannelSelectsTheContainersChannel)
    {
        writer->processing_function(make_buffer(1, { 0.5, 0.6 }));

        EXPECT_EQ(channel_samples(*stream, 1, 0, 2), (std::vector<double> { 0.5, 0.6 }));
        EXPECT_EQ(channel_samples(*stream, 0, 0, 2), (std::vector<double> { 0, 0 }));
    }

    TEST_F(SoundStreamWriterTest, OneWriterPerChannelKeepsTheChannelsAligned)
    {
        SoundStreamWriter right(stream);

        writer->processing_function(make_buffer(0, { 1, 2, 3 }));
        right.processing_function(make_buffer(1, { 4, 5, 6 }));

        EXPECT_EQ(stream->get_num_frames(), 3U);
        EXPECT_EQ(stream->get_frame(0).as<double>().size(), 2U);
        EXPECT_EQ(channel_samples(*stream, 0, 0, 3), (std::vector<double> { 1, 2, 3 }));
        EXPECT_EQ(channel_samples(*stream, 1, 0, 3), (std::vector<double> { 4, 5, 6 }));
    }

    TEST_F(SoundStreamWriterTest, AChannelBeyondTheContainerIsSkipped)
    {
        writer->processing_function(make_buffer(2, { 1, 2, 3, 4 }));

        EXPECT_FALSE(stream->has_data());
        EXPECT_EQ(writer->get_write_position(), 0U);
    }

    TEST_F(SoundStreamWriterTest, NullAndEmptyBuffersAreIgnored)
    {
        const auto empty = make_buffer(0, {});

        EXPECT_NO_THROW(writer->processing_function(nullptr));
        EXPECT_NO_THROW(writer->processing_function(empty));

        EXPECT_FALSE(stream->has_data());
        EXPECT_EQ(writer->get_write_position(), 0U);
    }

    TEST_F(SoundStreamWriterTest, ABufferMarkedForRemovalIsNotWritten)
    {
        const auto leaving = make_buffer(0, { 1, 2, 3, 4 });
        leaving->mark_for_removal();

        writer->processing_function(leaving);

        EXPECT_FALSE(stream->has_data());
        EXPECT_EQ(writer->get_write_position(), 0U);
    }

    TEST_F(SoundStreamWriterTest, TheWritePositionCanBeMovedAndRewound)
    {
        writer->set_write_position(10);
        writer->processing_function(make_buffer(0, { 7, 8 }));

        EXPECT_GE(stream->get_num_frames(), 12U);
        EXPECT_EQ(channel_samples(*stream, 0, 9, 3), (std::vector<double> { 0, 7, 8 }));
        EXPECT_EQ(writer->get_write_position(), 12U);

        writer->reset_position();
        writer->processing_function(make_buffer(0, { 1, 2 }));

        EXPECT_EQ(channel_samples(*stream, 0, 0, 2), (std::vector<double> { 1, 2 }));
        EXPECT_EQ(writer->get_write_position(), 2U);
    }

    TEST_F(SoundStreamWriterTest, WritingBeforeTheEndOverwritesWhatWasThere)
    {
        writer->processing_function(make_buffer(0, { 1, 2, 3, 4 }));
        writer->set_write_position(1);

        writer->processing_function(make_buffer(0, { 9, 9 }));

        EXPECT_EQ(channel_samples(*stream, 0, 0, 4), (std::vector<double> { 1, 9, 9, 4 }));
    }

    TEST_F(SoundStreamWriterTest, ThePositionConvertsToAndFromTime)
    {
        writer->set_write_position_time(0.5);

        EXPECT_EQ(writer->get_write_position(), k_rate / 2U);
        EXPECT_DOUBLE_EQ(writer->get_write_position_time(), 0.5);
    }

    TEST_F(SoundStreamWriterTest, ARingWriterWrapsToTheStartAtCapacity)
    {
        stream->enable_circular_buffer(4);

        writer->processing_function(make_buffer(0, { 1, 2, 3, 4 }));
        EXPECT_EQ(writer->get_write_position(), 0U);
        writer->processing_function(make_buffer(0, { 5, 6, 7, 8 }));

        EXPECT_EQ(channel_samples(*stream, 0, 0, 4), (std::vector<double> { 5, 6, 7, 8 }));
    }

    TEST_F(SoundStreamWriterTest, ARingWriterContinuesWhereTheRingWrapped)
    {
        stream->enable_circular_buffer(6);

        writer->processing_function(make_buffer(0, { 1, 2, 3, 4 }));
        writer->processing_function(make_buffer(0, { 5, 6, 7, 8 }));

        EXPECT_EQ(writer->get_write_position(), 2U);
        EXPECT_EQ(channel_samples(*stream, 0, 0, 6), (std::vector<double> { 7, 8, 3, 4, 5, 6 }));
    }

    TEST(SoundStreamWriterWithoutContainerTest, DoesNothingAndReportsTimeZero)
    {
        SoundStreamWriter writer(nullptr);

        EXPECT_NO_THROW(writer.processing_function(make_buffer(0, { 1, 2 })));
        writer.set_write_position_time(1.0);

        EXPECT_EQ(writer.get_container(), nullptr);
        EXPECT_EQ(writer.get_write_position(), 0U);
        EXPECT_DOUBLE_EQ(writer.get_write_position_time(), 0.0);
    }

    TEST(SoundStreamWriterPipelineTest, CopiesASourceStreamIntoASinkThroughABuffer)
    {
        const auto source = std::make_shared<DynamicSoundStream>(k_rate, 2);
        const auto sink = std::make_shared<DynamicSoundStream>(k_rate, 2);
        const std::vector<double> left { 0.1, 0.3, 0.5, 0.7 };
        const std::vector<double> right { 0.2, 0.4, 0.6, 0.8 };
        source->write_frames(std::vector<std::span<const double>> { std::span<const double>(left), std::span<const double>(right) }, 0);
        SoundStreamWriter to_left(sink);
        SoundStreamWriter to_right(sink);

        to_left.processing_function(make_buffer(0, channel_samples(*source, 0, 0, 4)));
        to_right.processing_function(make_buffer(1, channel_samples(*source, 1, 0, 4)));

        EXPECT_EQ(channel_samples(*sink, 0, 0, 4), left);
        EXPECT_EQ(channel_samples(*sink, 1, 0, 4), right);
        EXPECT_EQ(sink->get_frame(2).as<double>().size(), 2U);
        EXPECT_DOUBLE_EQ(sink->get_frame(2).as<double>()[0], 0.5);
        EXPECT_DOUBLE_EQ(sink->get_frame(2).as<double>()[1], 0.6);
    }

} // namespace
} // namespace MayaFlux::Test
