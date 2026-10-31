#include "gtest/gtest.h"

#include "MayaFlux/Buffers/Container/SoundContainerBuffer.hpp"
#include "MayaFlux/Kakshya/Processors/ContiguousAccessProcessor.hpp"
#include "MayaFlux/Kakshya/Source/DynamicSoundStream.hpp"

namespace MayaFlux::Test {
namespace {

    using Buffers::SoundContainerBuffer;
    using Buffers::SoundStreamReader;
    using Kakshya::ContiguousAccessProcessor;
    using Kakshya::DynamicSoundStream;
    using Kakshya::OrganizationStrategy;
    using Kakshya::ProcessingState;

    const std::vector<double> k_left { 0.1, 0.3, 0.5, 0.7 };
    const std::vector<double> k_right { 0.2, 0.4, 0.6, 0.8 };

    std::shared_ptr<DynamicSoundStream> make_source(OrganizationStrategy organization)
    {
        auto stream = std::make_shared<DynamicSoundStream>(48000, 2);
        stream->get_structure().organization = organization;

        if (organization == OrganizationStrategy::INTERLEAVED) {
            std::vector<double> samples;
            for (size_t i = 0; i < k_left.size(); ++i) {
                samples.push_back(k_left[i]);
                samples.push_back(k_right[i]);
            }
            stream->write_frames(std::vector<std::span<const double>> { std::span<const double>(samples) }, 0);
        } else {
            stream->write_frames(std::vector<std::span<const double>> { std::span<const double>(k_left), std::span<const double>(k_right) }, 0);
        }

        auto processor = std::make_shared<ContiguousAccessProcessor>();
        processor->set_auto_advance(false);
        stream->set_default_processor(processor);
        stream->mark_ready_for_processing(true);
        stream->set_read_position({ 0, 0 });
        processor->set_output_size({ 4, 2 });
        return stream;
    }

    class SoundStreamReaderTest : public ::testing::TestWithParam<OrganizationStrategy> {
    protected:
        void SetUp() override
        {
            stream = make_source(GetParam());
            buffer = make_buffer(4, 0);
        }

        void TearDown() override
        {
            stream->unregister_state_change_callback();
        }

        [[nodiscard]] std::shared_ptr<SoundContainerBuffer> make_buffer(uint32_t samples, uint32_t source_channel) const
        {
            auto sized = std::make_shared<SoundContainerBuffer>(0, samples, stream, source_channel);
            sized->resize(samples);
            return sized;
        }

        std::shared_ptr<SoundStreamReader> reader_for(uint32_t channel, bool auto_advance = false)
        {
            auto reader = std::make_shared<SoundStreamReader>(stream);
            reader->set_source_channel(channel);
            reader->set_auto_advance(auto_advance);
            return reader;
        }

        std::shared_ptr<DynamicSoundStream> stream;
        std::shared_ptr<SoundContainerBuffer> buffer;
    };

    INSTANTIATE_TEST_SUITE_P(Organizations, SoundStreamReaderTest,
        ::testing::Values(OrganizationStrategy::INTERLEAVED, OrganizationStrategy::PLANAR),
        [](const ::testing::TestParamInfo<OrganizationStrategy>& info) {
            return std::string(info.param == OrganizationStrategy::INTERLEAVED ? "Interleaved" : "Planar");
        });

    TEST_P(SoundStreamReaderTest, DefaultsToAutoAdvanceAndFlagUpdates)
    {
        const SoundStreamReader reader(stream);

        EXPECT_TRUE(reader.get_auto_advance());
        EXPECT_TRUE(reader.get_update_flags());
        EXPECT_EQ(reader.get_source_channel(), 0U);
        EXPECT_EQ(reader.get_container(), stream);
    }

    TEST_P(SoundStreamReaderTest, FillsTheBufferWithItsSourceChannel)
    {
        auto reader = reader_for(0);
        reader->on_attach(buffer);

        reader->processing_function(buffer);

        EXPECT_EQ(buffer->get_data(), k_left);
    }

    TEST_P(SoundStreamReaderTest, FillsTheBufferWithTheOtherChannel)
    {
        auto reader = reader_for(1);
        reader->on_attach(buffer);

        reader->processing_function(buffer);

        EXPECT_EQ(buffer->get_data(), k_right);
    }

    TEST_P(SoundStreamReaderTest, RepeatedProcessingWithoutAdvancingIsStable)
    {
        auto reader = reader_for(0);
        reader->on_attach(buffer);

        reader->processing_function(buffer);
        const auto first = buffer->get_data();
        reader->processing_function(buffer);

        EXPECT_EQ(first, k_left);
        EXPECT_EQ(buffer->get_data(), k_left);
        EXPECT_EQ(stream->get_read_position(), (std::vector<uint64_t> { 0, 0 }));
    }

    TEST_P(SoundStreamReaderTest, AutoAdvanceMovesOnlyTheSourceChannelsPosition)
    {
        auto reader = reader_for(0, true);
        reader->on_attach(buffer);

        reader->processing_function(buffer);

        EXPECT_EQ(stream->get_read_position(), (std::vector<uint64_t> { 4, 0 }));
    }

    TEST_P(SoundStreamReaderTest, ReadingToTheEndMarksTheBufferForRemoval)
    {
        auto reader = reader_for(0, true);
        reader->on_attach(buffer);
        reader->processing_function(buffer);
        ASSERT_FALSE(buffer->needs_removal());

        reader->processing_function(buffer);

        EXPECT_TRUE(buffer->needs_removal());
    }

    TEST_P(SoundStreamReaderTest, AContainerAlreadyAtItsEndMarksTheBufferWithoutReading)
    {
        auto reader = reader_for(0);
        reader->on_attach(buffer);
        stream->set_read_position({ 4, 4 });

        reader->processing_function(buffer);

        EXPECT_TRUE(buffer->needs_removal());
        EXPECT_EQ(buffer->get_data(), std::vector<double>(4, 0.0));
    }

    TEST_P(SoundStreamReaderTest, ARemovalRequestFromTheContainerReachesTheBuffer)
    {
        auto reader = reader_for(0);
        reader->on_attach(buffer);
        stream->update_processing_state(ProcessingState::NEEDS_REMOVAL);

        reader->processing_function(buffer);

        EXPECT_TRUE(buffer->needs_removal());
    }

    TEST_P(SoundStreamReaderTest, ARemovalRequestIsIgnoredWhenFlagUpdatesAreOff)
    {
        auto reader = reader_for(0);
        reader->set_update_flags(false);
        reader->on_attach(buffer);
        stream->update_processing_state(ProcessingState::NEEDS_REMOVAL);

        reader->processing_function(buffer);

        EXPECT_FALSE(buffer->needs_removal());
    }

    TEST_P(SoundStreamReaderTest, FillingTheBufferFlagsItForProcessingUnlessFlagUpdatesAreOff)
    {
        auto flagging = reader_for(0);
        flagging->on_attach(buffer);
        buffer->mark_for_processing(false);
        flagging->processing_function(buffer);
        EXPECT_TRUE(buffer->has_data_for_cycle());

        const auto quiet_buffer = make_buffer(4, 0);
        auto quiet = reader_for(0);
        quiet->set_update_flags(false);
        quiet->on_attach(quiet_buffer);
        quiet_buffer->mark_for_processing(false);
        quiet->processing_function(quiet_buffer);

        EXPECT_FALSE(quiet_buffer->has_data_for_cycle());
        EXPECT_EQ(quiet_buffer->get_data(), k_left);
    }

    TEST_P(SoundStreamReaderTest, ABufferLongerThanTheDataIsZeroPadded)
    {
        const auto longer = make_buffer(10, 0);
        auto reader = reader_for(0);
        reader->on_attach(longer);

        reader->processing_function(longer);

        std::vector<double> expected = k_left;
        expected.resize(10, 0.0);
        EXPECT_EQ(longer->get_data(), expected);
    }

    TEST_P(SoundStreamReaderTest, AttachingRegistersAReaderAndDetachingRemovesIt)
    {
        auto reader = reader_for(0);
        ASSERT_FALSE(stream->has_active_readers());

        reader->on_attach(buffer);
        EXPECT_TRUE(stream->has_active_readers());

        reader->on_detach(buffer);
        EXPECT_FALSE(stream->has_active_readers());
    }

    TEST_P(SoundStreamReaderTest, AnUnreadyContainerRefusesAttachment)
    {
        const auto empty = std::make_shared<DynamicSoundStream>(48000, 2);
        const auto empty_buffer = std::make_shared<SoundContainerBuffer>(0, 4, empty, 0);
        SoundStreamReader reader(empty);

        EXPECT_THROW(reader.on_attach(empty_buffer), std::runtime_error);
    }

    TEST_P(SoundStreamReaderTest, AFailedAttachmentLeavesNoReaderRegistered)
    {
        const auto empty = std::make_shared<DynamicSoundStream>(48000, 2);
        const auto empty_buffer = std::make_shared<SoundContainerBuffer>(0, 4, empty, 0);
        SoundStreamReader reader(empty);

        EXPECT_THROW(reader.on_attach(empty_buffer), std::runtime_error);

        EXPECT_FALSE(empty->has_active_readers());
    }

    TEST_P(SoundStreamReaderTest, TwoReadersMustBothConsumeBeforeTheContainerRecycles)
    {
        auto first = reader_for(0);
        auto second = reader_for(0);
        first->on_attach(buffer);
        second->on_attach(buffer);

        first->processing_function(buffer);
        EXPECT_EQ(stream->get_processing_state(), ProcessingState::PROCESSED);
        EXPECT_FALSE(stream->all_dimensions_consumed());

        second->processing_function(buffer);
        EXPECT_EQ(stream->get_processing_state(), ProcessingState::READY);
    }

    TEST_P(SoundStreamReaderTest, ChannelsBeyondTheContainerAreRefused)
    {
        auto reader = std::make_shared<SoundStreamReader>(stream);

        EXPECT_THROW(reader->set_source_channel(2), std::out_of_range);
        EXPECT_NO_THROW(reader->set_source_channel(1));
        EXPECT_EQ(reader->get_source_channel(), 1U);
    }

    TEST_P(SoundStreamReaderTest, ChangingTheContainerChangesTheChannelLimit)
    {
        auto reader = std::make_shared<SoundStreamReader>(stream);
        const auto mono = std::make_shared<DynamicSoundStream>(48000, 1);

        reader->set_container(mono);

        EXPECT_EQ(reader->get_container(), mono);
        EXPECT_THROW(reader->set_source_channel(1), std::out_of_range);
    }

    TEST_P(SoundStreamReaderTest, WithoutAContainerOrBufferNothingHappens)
    {
        SoundStreamReader detached(nullptr);
        auto reader = reader_for(0);

        EXPECT_NO_THROW(detached.processing_function(buffer));
        EXPECT_NO_THROW(detached.on_attach(buffer));
        EXPECT_NO_THROW(reader->processing_function(nullptr));
        EXPECT_NO_THROW(reader->on_attach(nullptr));
    }

    TEST_P(SoundStreamReaderTest, ABufferBacksOntoItsContainerAndChannel)
    {
        const auto second = std::make_shared<SoundContainerBuffer>(3, 4, stream, 1);

        EXPECT_EQ(second->get_container(), stream);
        EXPECT_EQ(second->get_source_channel(), 1U);
        EXPECT_EQ(second->get_channel_id(), 3U);
        EXPECT_FALSE(second->is_zero_copy());
    }

    TEST_P(SoundStreamReaderTest, ABufferNeedsAContainer)
    {
        EXPECT_THROW(SoundContainerBuffer(0, 4, nullptr, 0), std::invalid_argument);
    }

    TEST_P(SoundStreamReaderTest, InitializingInstallsAReaderForTheSourceChannel)
    {
        const auto second = make_buffer(4, 1);

        second->initialize();

        const auto reader = std::dynamic_pointer_cast<SoundStreamReader>(second->get_default_processor());
        ASSERT_NE(reader, nullptr);
        EXPECT_EQ(reader->get_source_channel(), 1U);
        EXPECT_EQ(reader->get_container(), stream);
        EXPECT_TRUE(stream->has_active_readers());
    }

    TEST_P(SoundStreamReaderTest, TheInstalledReaderFillsTheBufferAndAdvances)
    {
        const auto second = make_buffer(4, 1);
        second->initialize();
        const auto reader = std::dynamic_pointer_cast<SoundStreamReader>(second->get_default_processor());
        ASSERT_NE(reader, nullptr);

        reader->processing_function(second);

        EXPECT_EQ(second->get_data(), k_right);
        EXPECT_EQ(stream->get_read_position(), (std::vector<uint64_t> { 0, 4 }));
    }

    TEST_P(SoundStreamReaderTest, InitializingTwiceKeepsTheSameReader)
    {
        const auto second = make_buffer(4, 0);
        second->initialize();
        const auto first_reader = second->get_default_processor();

        second->initialize();

        EXPECT_EQ(second->get_default_processor(), first_reader);
    }

    TEST_P(SoundStreamReaderTest, SettingTheContainerRepointsTheBufferAndItsReader)
    {
        const auto other = make_source(GetParam());
        const auto second = make_buffer(4, 0);
        second->initialize();

        second->set_container(other);

        EXPECT_EQ(second->get_container(), other);
        const auto reader = std::dynamic_pointer_cast<SoundStreamReader>(second->get_default_processor());
        ASSERT_NE(reader, nullptr);
        EXPECT_EQ(reader->get_container(), other);
        other->unregister_state_change_callback();
    }

    TEST(SoundStreamReaderAdvancingProcessorTest, EveryBlockIsDeliveredBeforeTheBufferLeaves)
    {
        const auto stream = std::make_shared<DynamicSoundStream>(48000, 2);
        const std::vector<double> left { 1, 2, 3, 4, 5, 6, 7, 8 };
        const std::vector<double> right { 11, 12, 13, 14, 15, 16, 17, 18 };
        stream->write_frames(std::vector<std::span<const double>> { std::span<const double>(left), std::span<const double>(right) }, 0);
        const auto processor = std::make_shared<ContiguousAccessProcessor>();
        stream->set_default_processor(processor);
        stream->mark_ready_for_processing(true);
        processor->set_output_size({ 4, 2 });
        const auto buffer = std::make_shared<SoundContainerBuffer>(0, 4, stream, 0);
        buffer->resize(4);
        buffer->initialize();
        const auto reader = std::dynamic_pointer_cast<SoundStreamReader>(buffer->get_default_processor());
        ASSERT_NE(reader, nullptr);

        reader->processing_function(buffer);
        EXPECT_EQ(buffer->get_data(), (std::vector<double> { 1, 2, 3, 4 }));
        EXPECT_EQ(stream->get_read_position(), (std::vector<uint64_t> { 4, 4 }));
        EXPECT_FALSE(buffer->needs_removal());

        reader->processing_function(buffer);
        EXPECT_EQ(buffer->get_data(), (std::vector<double> { 5, 6, 7, 8 }));
        EXPECT_EQ(stream->get_read_position(), (std::vector<uint64_t> { 8, 8 }));
        EXPECT_FALSE(buffer->needs_removal());

        reader->processing_function(buffer);
        EXPECT_TRUE(buffer->needs_removal());
        EXPECT_EQ(buffer->get_data(), (std::vector<double> { 5, 6, 7, 8 }));
        stream->unregister_state_change_callback();
    }

} // namespace
} // namespace MayaFlux::Test
