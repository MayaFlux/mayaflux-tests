#include "gtest/gtest.h"

#include "MayaFlux/Buffers/AudioBuffer.hpp"
#include "MayaFlux/Buffers/BufferProcessingChain.hpp"
#include "MayaFlux/Buffers/BufferSpec.hpp"
#include "MayaFlux/Buffers/Container/SoundFileBridge.hpp"
#include "MayaFlux/Buffers/Container/SoundStreamWriter.hpp"
#include "MayaFlux/Kakshya/Processors/ContiguousAccessProcessor.hpp"
#include "MayaFlux/Kakshya/Source/DynamicSoundStream.hpp"
#include "MayaFlux/Kakshya/Source/SoundFileContainer.hpp"

namespace MayaFlux::Test {
namespace {

    using Buffers::AudioBuffer;
    using Buffers::ProcessingToken;
    using Buffers::SoundFileBridge;
    using Buffers::SoundStreamReader;
    using Buffers::SoundStreamWriter;
    using Kakshya::ContiguousAccessProcessor;
    using Kakshya::make_sound_file_container;
    using Kakshya::OrganizationStrategy;
    using Kakshya::SoundFileContainer;

    constexpr uint64_t k_blocks = 4;

    double sample(uint64_t frame, uint32_t channel)
    {
        return 1.0 + 10.0 * static_cast<double>(frame) + static_cast<double>(channel);
    }

    std::vector<double> channel_block(uint32_t channel, uint64_t first, uint64_t count, double gain = 1.0)
    {
        std::vector<double> values;
        values.reserve(count);
        for (uint64_t f = first; f < first + count; ++f) {
            values.push_back(sample(f, channel) * gain);
        }
        return values;
    }

    class ScaleProcessor : public Buffers::BufferProcessor {
    public:
        explicit ScaleProcessor(double gain)
            : m_gain(gain)
        {
        }

        void processing_function(const std::shared_ptr<Buffers::Buffer>& buffer) override
        {
            for (double& value : std::dynamic_pointer_cast<AudioBuffer>(buffer)->get_data()) {
                value *= m_gain;
            }
        }

    private:
        double m_gain;
    };

    class ProbeProcessor : public Buffers::BufferProcessor {
    public:
        explicit ProbeProcessor(std::shared_ptr<SoundStreamWriter> writer)
            : m_writer(std::move(writer))
        {
        }

        void processing_function(const std::shared_ptr<Buffers::Buffer>& buffer) override
        {
            const auto& data = std::dynamic_pointer_cast<AudioBuffer>(buffer)->get_data();
            first_samples.push_back(data.empty() ? 0.0 : data.front());
            written_before.push_back(m_writer->get_write_position());
        }

        std::vector<double> first_samples;
        std::vector<uint64_t> written_before;

    private:
        std::shared_ptr<SoundStreamWriter> m_writer;
    };

    struct StreamShape {
        OrganizationStrategy organization;
        uint32_t channels;
    };

    class SoundFileBridgeTest : public ::testing::TestWithParam<StreamShape> {
    protected:
        void SetUp() override
        {
            block = Buffers::s_preferred_buffer_size;
            rate = static_cast<uint32_t>(Buffers::s_registered_sample_rate);

            std::vector<std::vector<double>> data;
            for (uint32_t c = 0; c < channels(); ++c) {
                data.push_back(channel_block(c, 0, k_blocks * block));
            }
            file = make_sound_file_container(std::move(data), channels(), rate, organization());

            const auto processor = std::dynamic_pointer_cast<ContiguousAccessProcessor>(file->get_default_processor());
            processor->set_output_size({ block, channels() });
        }

        [[nodiscard]] OrganizationStrategy organization() const { return GetParam().organization; }
        [[nodiscard]] uint32_t channels() const { return GetParam().channels; }

        [[nodiscard]] std::shared_ptr<SoundFileBridge> make_bridge(uint32_t channel_id, uint32_t source_channel = 0) const
        {
            return std::make_shared<SoundFileBridge>(channel_id, file, source_channel);
        }

        [[nodiscard]] std::shared_ptr<SoundFileBridge> make_ready_bridge(uint32_t channel_id, uint32_t source_channel = 0) const
        {
            auto bridge = make_bridge(channel_id, source_channel);
            bridge->setup_processors(ProcessingToken::AUDIO_BACKEND);
            return bridge;
        }

        static void cycle(const std::shared_ptr<SoundFileBridge>& bridge)
        {
            bridge->process_default();
            bridge->get_processing_chain()->process_complete(bridge);
        }

        static std::vector<double> captured(const std::shared_ptr<SoundFileBridge>& bridge, uint32_t channel, uint64_t first, uint64_t count)
        {
            const auto span = bridge->get_capture_stream()->get_channel_frames(channel, first, count);
            return { span.begin(), span.end() };
        }

        uint64_t block {};
        uint32_t rate {};
        std::shared_ptr<SoundFileContainer> file;
    };

    INSTANTIATE_TEST_SUITE_P(Shapes, SoundFileBridgeTest,
        ::testing::Values(
            StreamShape { OrganizationStrategy::INTERLEAVED, 1 },
            StreamShape { OrganizationStrategy::INTERLEAVED, 2 },
            StreamShape { OrganizationStrategy::INTERLEAVED, 3 },
            StreamShape { OrganizationStrategy::PLANAR, 1 },
            StreamShape { OrganizationStrategy::PLANAR, 2 },
            StreamShape { OrganizationStrategy::PLANAR, 3 }),
        [](const ::testing::TestParamInfo<StreamShape>& info) {
            return std::string(info.param.organization == OrganizationStrategy::INTERLEAVED ? "Interleaved" : "Planar")
                + std::to_string(info.param.channels) + "ch";
        });

    TEST(SoundFileBridgeConstructionTest, ANullFileIsRefused)
    {
        EXPECT_THROW(SoundFileBridge(0, nullptr), std::invalid_argument);
    }

    TEST_P(SoundFileBridgeTest, ConstructionKeepsTheFileAndBothChannels)
    {
        const uint32_t source = channels() - 1;

        const auto bridge = make_bridge(0, source);

        EXPECT_EQ(bridge->get_container(), file);
        EXPECT_EQ(bridge->get_source_channel(), source);
        EXPECT_EQ(bridge->get_channel_id(), 0U);
        EXPECT_EQ(bridge->get_capture_stream(), nullptr);
        EXPECT_EQ(bridge->get_stream_writer(), nullptr);
        EXPECT_FALSE(file->has_active_readers());
    }

    TEST_P(SoundFileBridgeTest, SettingUpBuildsAnEmptyCaptureStreamForTheFilesChannels)
    {
        const auto bridge = make_ready_bridge(0);

        const auto capture = bridge->get_capture_stream();
        ASSERT_NE(capture, nullptr);
        EXPECT_EQ(capture->get_num_channels(), channels());
        EXPECT_EQ(capture->get_sample_rate(), rate);
        EXPECT_FALSE(capture->has_data());
    }

    TEST_P(SoundFileBridgeTest, SettingUpInstallsAReaderForTheFileAndAWriterForTheCapture)
    {
        const uint32_t source = channels() - 1;
        const auto bridge = make_ready_bridge(0, source);

        const auto reader = std::dynamic_pointer_cast<SoundStreamReader>(bridge->get_default_processor());
        ASSERT_NE(reader, nullptr);
        EXPECT_EQ(reader->get_container(), file);
        EXPECT_EQ(reader->get_source_channel(), source);
        EXPECT_TRUE(bridge->needs_default_processing());
        EXPECT_TRUE(file->has_active_readers());

        const auto writer = bridge->get_stream_writer();
        ASSERT_NE(writer, nullptr);
        EXPECT_EQ(writer->get_container(), bridge->get_capture_stream());
        EXPECT_EQ(bridge->get_processing_chain()->get_postprocessor(bridge), writer);
    }

    TEST_P(SoundFileBridgeTest, TheFirstCycleFillsTheBufferAndCapturesTheFirstBlock)
    {
        const auto bridge = make_ready_bridge(0);

        cycle(bridge);

        EXPECT_EQ(bridge->get_data(), channel_block(0, 0, block));
        EXPECT_EQ(captured(bridge, 0, 0, block), channel_block(0, 0, block));
        EXPECT_EQ(bridge->get_stream_writer()->get_write_position(), block);
    }

    TEST_P(SoundFileBridgeTest, SuccessiveCyclesAccumulateTheFileInOrder)
    {
        const auto bridge = make_ready_bridge(0);

        cycle(bridge);
        cycle(bridge);
        cycle(bridge);

        EXPECT_EQ(captured(bridge, 0, 0, 3 * block), channel_block(0, 0, 3 * block));
        EXPECT_EQ(bridge->get_stream_writer()->get_write_position(), 3U * block);
    }

    TEST_P(SoundFileBridgeTest, TheSourceChannelPicksWhichFileChannelIsRead)
    {
        const uint32_t source = channels() - 1;
        const auto bridge = make_ready_bridge(0, source);

        cycle(bridge);

        EXPECT_EQ(bridge->get_data(), channel_block(source, 0, block));
        EXPECT_EQ(captured(bridge, 0, 0, block), channel_block(source, 0, block));
    }

    TEST_P(SoundFileBridgeTest, TheBufferChannelPicksWhichCaptureChannelIsWritten)
    {
        if (channels() == 1) {
            GTEST_SKIP() << "a mono capture stream has only channel 0";
        }
        const uint32_t target = channels() - 1;
        const auto bridge = make_ready_bridge(target, 0);

        cycle(bridge);

        EXPECT_EQ(captured(bridge, target, 0, block), channel_block(0, 0, block));
        EXPECT_EQ(captured(bridge, 0, 0, block), std::vector<double>(block, 0.0));
    }

    TEST_P(SoundFileBridgeTest, ABufferChannelBeyondTheCaptureStreamIsReadButNotCaptured)
    {
        const auto bridge = make_ready_bridge(channels(), 0);

        cycle(bridge);

        EXPECT_EQ(bridge->get_data(), channel_block(0, 0, block));
        EXPECT_FALSE(bridge->get_capture_stream()->has_data());
        EXPECT_EQ(bridge->get_stream_writer()->get_write_position(), 0U);
    }

    TEST_P(SoundFileBridgeTest, AProcessorInTheChainShapesWhatIsCaptured)
    {
        const auto bridge = make_ready_bridge(0);
        ASSERT_TRUE(bridge->get_processing_chain()->add_processor(std::make_shared<ScaleProcessor>(0.5), bridge));

        cycle(bridge);
        cycle(bridge);

        EXPECT_EQ(captured(bridge, 0, 0, 2 * block), channel_block(0, 0, 2 * block, 0.5));
    }

    TEST_P(SoundFileBridgeTest, AProcessorSeesTheCurrentBlockBeforeTheWriterCapturesIt)
    {
        const auto bridge = make_ready_bridge(0);
        const auto probe = std::make_shared<ProbeProcessor>(bridge->get_stream_writer());
        ASSERT_TRUE(bridge->get_processing_chain()->add_processor(probe, bridge));

        cycle(bridge);
        cycle(bridge);
        cycle(bridge);

        EXPECT_EQ(probe->first_samples, (std::vector<double> { sample(0, 0), sample(block, 0), sample(2 * block, 0) }));
        EXPECT_EQ(probe->written_before, (std::vector<uint64_t> { 0, block, 2 * block }));
    }

    TEST_P(SoundFileBridgeTest, EachCycleLeavesItsBlockReadableAtOnce)
    {
        const auto bridge = make_ready_bridge(0);

        for (uint64_t k = 0; k < 3; ++k) {
            cycle(bridge);

            EXPECT_EQ(bridge->get_stream_writer()->get_write_position(), (k + 1) * block) << k;
            EXPECT_EQ(captured(bridge, 0, k * block, block), channel_block(0, k * block, block)) << k;
        }
    }

    TEST_P(SoundFileBridgeTest, TheCaptureClockIsTheBufferClock)
    {
        const auto bridge = make_ready_bridge(0);

        cycle(bridge);
        cycle(bridge);
        cycle(bridge);

        const auto capture = bridge->get_capture_stream();
        const double seconds = capture->position_to_time(bridge->get_stream_writer()->get_write_position());
        EXPECT_NEAR(seconds, 3.0 * static_cast<double>(block) / static_cast<double>(rate), 1e-9);
    }

    TEST_P(SoundFileBridgeTest, AProcessorAddedBetweenCyclesActsFromTheNextCycle)
    {
        const auto bridge = make_ready_bridge(0);

        cycle(bridge);
        ASSERT_TRUE(bridge->get_processing_chain()->add_processor(std::make_shared<ScaleProcessor>(0.5), bridge));
        cycle(bridge);

        EXPECT_EQ(captured(bridge, 0, 0, block), channel_block(0, 0, block));
        EXPECT_EQ(captured(bridge, 0, block, block), channel_block(0, block, block, 0.5));
    }

    TEST_P(SoundFileBridgeTest, AProcessorRemovedBetweenCyclesStopsAtOnce)
    {
        const auto bridge = make_ready_bridge(0);
        const auto scale = std::make_shared<ScaleProcessor>(0.5);
        ASSERT_TRUE(bridge->get_processing_chain()->add_processor(scale, bridge));

        cycle(bridge);
        bridge->get_processing_chain()->remove_processor(scale, bridge);
        cycle(bridge);

        EXPECT_EQ(captured(bridge, 0, 0, block), channel_block(0, 0, block, 0.5));
        EXPECT_EQ(captured(bridge, 0, block, block), channel_block(0, block, block));
    }

    TEST_P(SoundFileBridgeTest, PullingTheDefaultProcessorReadsOneBlockPerCallAndLeavesTheCaptureAlone)
    {
        const auto bridge = make_ready_bridge(0);

        bridge->process_default();
        EXPECT_EQ(bridge->get_data(), channel_block(0, 0, block));
        bridge->process_default();
        EXPECT_EQ(bridge->get_data(), channel_block(0, block, block));

        EXPECT_FALSE(bridge->get_capture_stream()->has_data());
        EXPECT_EQ(bridge->get_stream_writer()->get_write_position(), 0U);
    }

    TEST_P(SoundFileBridgeTest, TheChainAloneNeverReadsTheFile)
    {
        const auto bridge = make_ready_bridge(0);

        bridge->get_processing_chain()->process_complete(bridge);
        bridge->get_processing_chain()->process_complete(bridge);

        EXPECT_EQ(file->get_read_position(), std::vector<uint64_t>(channels(), 0));
        EXPECT_EQ(captured(bridge, 0, 0, 2 * block), std::vector<double>(2 * block, 0.0));
    }

    TEST_P(SoundFileBridgeTest, AWholeFileIsCapturedOnceAndTheBufferThenLeaves)
    {
        const auto bridge = make_ready_bridge(0);

        for (uint32_t i = 0; i < 2 * k_blocks && !bridge->needs_removal(); ++i) {
            cycle(bridge);
        }

        EXPECT_TRUE(bridge->needs_removal());
        EXPECT_EQ(bridge->get_stream_writer()->get_write_position(), k_blocks * block);
        for (uint64_t b = 0; b < k_blocks; ++b) {
            const auto first = b * block;
            const auto got = captured(bridge, 0, first, block);
            EXPECT_EQ(got.empty() ? 0.0 : got.front(), sample(first, 0)) << "block " << b << " starts wrong";
            EXPECT_EQ(got, channel_block(0, first, block)) << "block " << b;
        }
    }

} // namespace
} // namespace MayaFlux::Test
