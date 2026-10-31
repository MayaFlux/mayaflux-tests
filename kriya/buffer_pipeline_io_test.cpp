#include "gtest/gtest.h"

#include "../support/scoped_working_dir.hpp"

#include "MayaFlux/Buffers/BufferManager.hpp"
#include "MayaFlux/Core/GlobalStreamInfo.hpp"
#include "MayaFlux/IO/Audio/SoundFileWriter.hpp"
#include "MayaFlux/IO/IOManager.hpp"
#include "MayaFlux/Kakshya/Source/DynamicSoundStream.hpp"
#include "MayaFlux/Kriya/BufferPipeline.hpp"

namespace MayaFlux::Test {
namespace {

    using Kakshya::DataVariant;
    using Kriya::BufferOperation;
    using Kriya::BufferPipeline;

    constexpr uint32_t k_channels = 2;
    constexpr uint64_t k_blocks = 4;
    constexpr double k_tolerance = 1e-6;

    double sample(uint64_t frame, uint32_t channel)
    {
        return static_cast<double>(frame + 1) * 1e-4 + static_cast<double>(channel) * 0.05;
    }

    std::vector<double> channel_blocks(uint32_t channel, uint64_t first_block, uint64_t blocks, uint64_t block)
    {
        std::vector<double> values;
        values.reserve(blocks * block);
        for (uint64_t f = first_block * block; f < (first_block + blocks) * block; ++f) {
            values.push_back(sample(f, channel));
        }
        return values;
    }

    std::vector<double> values_of(const DataVariant& data)
    {
        const auto* values = std::get_if<std::vector<double>>(&data);
        return values ? *values : std::vector<double> {};
    }

    void expect_near(const std::vector<double>& actual, const std::vector<double>& expected)
    {
        ASSERT_EQ(actual.size(), expected.size());
        for (size_t i = 0; i < actual.size(); ++i) {
            ASSERT_NEAR(actual[i], expected[i], k_tolerance) << i;
        }
    }

    class BufferPipelineFileTest : public ::testing::Test {
    protected:
        void SetUp() override
        {
            stream_info.sample_rate = 48000;
            stream_info.buffer_size = 512;
            block = Buffers::s_preferred_buffer_size;
            scheduler = std::make_shared<Vruta::TaskScheduler>(stream_info.sample_rate);
            manager = std::make_shared<Buffers::BufferManager>();
            io = std::make_shared<IO::IOManager>(stream_info, 60, manager, scheduler);
            path = write_tone();
        }

        void TearDown() override
        {
            for (const auto& pipeline : pipelines) {
                pipeline->end();
            }
            io->stop();
        }

        std::string write_tone()
        {
            const std::string file = std::filesystem::absolute("tone.wav").string();
            const uint64_t frames = k_blocks * block;
            std::vector<double> interleaved;
            interleaved.reserve(frames * k_channels);
            for (uint64_t f = 0; f < frames; ++f) {
                for (uint32_t c = 0; c < k_channels; ++c) {
                    interleaved.push_back(sample(f, c));
                }
            }

            IO::SoundFileWriter writer;
            EXPECT_TRUE(writer.open(file, k_channels, stream_info.sample_rate, AV_CODEC_ID_PCM_F32LE)) << writer.last_error();
            writer.write(std::span<const double>(interleaved), static_cast<uint32_t>(frames));
            EXPECT_TRUE(writer.close().get()) << writer.last_error();
            return file;
        }

        std::shared_ptr<BufferPipeline> make_pipeline()
        {
            auto pipeline = BufferPipeline::create(*scheduler, manager, io);
            pipelines.push_back(pipeline);
            return pipeline;
        }

        void tick()
        {
            scheduler->process_buffer_cycle_tasks();
            scheduler->process_token(Vruta::ProcessingToken::SAMPLE_ACCURATE, block);
        }

        void run(const std::shared_ptr<BufferPipeline>& pipeline, uint32_t limit = 600)
        {
            for (uint32_t used = 0; pipeline->is_running() && used < limit; ++used) {
                tick();
            }
        }

        ScopedWorkingDir directory;
        Core::GlobalStreamInfo stream_info;
        uint64_t block {};
        std::string path;
        std::shared_ptr<Vruta::TaskScheduler> scheduler;
        std::shared_ptr<Buffers::BufferManager> manager;
        std::shared_ptr<IO::IOManager> io;
        std::vector<std::shared_ptr<BufferPipeline>> pipelines;
    };

    TEST_F(BufferPipelineFileTest, AFileCaptureReadsTheNextBlockOfTheFileEachCycle)
    {
        const auto pipeline = make_pipeline();
        std::vector<std::vector<double>> blocks;

        *pipeline >> BufferOperation::capture_file_from(io, path).on_data_ready([&](DataVariant& data, uint32_t) {
            blocks.push_back(values_of(data));
        });
        pipeline->execute_for_cycles(3);
        run(pipeline);

        ASSERT_EQ(blocks.size(), 3U);
        for (uint64_t b = 0; b < 3; ++b) {
            expect_near(blocks[b], channel_blocks(0, b, 1, block));
        }
    }

    TEST_F(BufferPipelineFileTest, AccumulatingAFileCaptureJoinsTheBlocksInOrder)
    {
        const auto pipeline = make_pipeline();
        std::vector<double> seen;

        *pipeline
            >> BufferOperation::capture_file(io, path, 0, 3)
            >> BufferOperation::transform([&](DataVariant& data, uint32_t) {
                   seen = values_of(data);
                   return data;
               });
        pipeline->execute_for_cycles(1);
        run(pipeline);

        expect_near(seen, channel_blocks(0, 0, 3, block));
    }

    TEST_F(BufferPipelineFileTest, AFileCaptureWithoutACycleCountStillCapturesEveryCycle)
    {
        const auto pipeline = make_pipeline();
        std::vector<size_t> sizes;
        auto op = BufferOperation::capture_file(io, path);

        *pipeline >> std::move(op);
        *pipeline >> BufferOperation::dispatch_to([&](DataVariant& data, uint32_t) { sizes.push_back(values_of(data).size()); });
        pipeline->execute_for_cycles(3);
        run(pipeline);

        EXPECT_EQ(sizes, (std::vector<size_t> { block, block, block }));
    }

    TEST_F(BufferPipelineFileTest, TheWholeFileCanBeCapturedAndRoutedIntoAStream)
    {
        const auto stream = std::make_shared<Kakshya::DynamicSoundStream>(stream_info.sample_rate, 1);
        const auto pipeline = make_pipeline();

        *pipeline
            >> BufferOperation::capture_file_from(io, path).for_cycles(static_cast<uint32_t>(k_blocks - 1))
            >> BufferOperation::route_to_container(stream);
        pipeline->execute_for_cycles(1);
        run(pipeline);

        EXPECT_EQ(stream->get_write_head(0), (k_blocks - 1) * block);
        const auto frames = stream->get_channel_frames(0, 0, (k_blocks - 1) * block);
        expect_near(std::vector<double>(frames.begin(), frames.end()), channel_blocks(0, 0, k_blocks - 1, block));
    }

    TEST_F(BufferPipelineFileTest, FileToStreamDeliversTheFileIntoTheTargetStream)
    {
        const auto target = std::make_shared<Kakshya::DynamicSoundStream>(stream_info.sample_rate, 1);
        const auto pipeline = make_pipeline();

        *pipeline >> BufferOperation::file_to_stream(io, path, target, static_cast<uint32_t>(k_blocks));
        pipeline->execute_for_cycles(static_cast<uint64_t>(k_blocks));
        run(pipeline);

        EXPECT_EQ(target->get_write_head(0), k_blocks * block);
    }

    TEST_F(BufferPipelineFileTest, AMissingFileIsRefusedByEveryFileFactory)
    {
        const auto target = std::make_shared<Kakshya::DynamicSoundStream>(stream_info.sample_rate, 1);
        const std::string missing = std::filesystem::absolute("missing.wav").string();

        EXPECT_THROW((void)BufferOperation::capture_file(io, missing), std::runtime_error);
        EXPECT_THROW((void)BufferOperation::capture_file_from(io, missing), std::runtime_error);
        EXPECT_THROW((void)BufferOperation::file_to_stream(io, missing, target), std::runtime_error);
    }

} // namespace
} // namespace MayaFlux::Test
