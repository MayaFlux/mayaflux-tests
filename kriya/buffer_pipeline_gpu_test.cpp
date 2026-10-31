#include "gtest/gtest.h"


#include "../kakshya/video_test_support.hpp"
#include "../support/vulkan_test_support.hpp"

#include "MayaFlux/Buffers/AudioBuffer.hpp"
#include "MayaFlux/Buffers/BufferManager.hpp"
#include "MayaFlux/Buffers/BufferProcessingChain.hpp"
#include "MayaFlux/Buffers/Staging/StagingUtils.hpp"
#include "MayaFlux/Buffers/VKBuffer.hpp"
#include "MayaFlux/Kakshya/Source/DynamicVideoStream.hpp"
#include "MayaFlux/Kriya/BufferPipeline.hpp"
#include "MayaFlux/Registry/BackendRegistry.hpp"
#include "MayaFlux/Registry/Service/BufferService.hpp"

namespace MayaFlux::Test {
namespace {

    using Buffers::ProcessingToken;
    using Buffers::VKBuffer;
    using Kakshya::DataVariant;
    using Kriya::BufferOperation;
    using Kriya::BufferPipeline;

    constexpr uint32_t k_rate = 48000;

    std::vector<uint8_t> pattern(size_t bytes, uint8_t seed)
    {
        std::vector<uint8_t> values(bytes);
        for (size_t i = 0; i < bytes; ++i) {
            values[i] = static_cast<uint8_t>(seed + i * 7U);
        }
        return values;
    }

    class BufferPipelineGpuTest : public VulkanDeviceTest {
    protected:
        void SetUp() override
        {
            VulkanDeviceTest::SetUp();
            if (IsSkipped()) {
                return;
            }
            block = Buffers::s_preferred_buffer_size;
            scheduler = std::make_shared<Vruta::TaskScheduler>(k_rate);
            manager = std::make_shared<Buffers::BufferManager>();
        }

        void TearDown() override
        {
            for (const auto& pipeline : pipelines) {
                pipeline->end();
            }
            auto* service = Registry::BackendRegistry::instance().get_service<Registry::Service::BufferService>();
            for (const auto& buffer : buffers) {
                if (service && buffer && buffer->is_initialized()) {
                    service->destroy_buffer(buffer);
                }
            }
            buffers.clear();
            VulkanDeviceTest::TearDown();
        }

        std::shared_ptr<VKBuffer> make_buffer(size_t bytes)
        {
            auto buffer = manager->create_graphics_buffer<VKBuffer>(
                ProcessingToken::GRAPHICS_BACKEND, bytes, VKBuffer::Usage::HOST_STORAGE, Kakshya::DataModality::UNKNOWN);
            if (!buffer->is_initialized()) {
                auto* service = Registry::BackendRegistry::instance().get_service<Registry::Service::BufferService>();
                service->initialize_buffer(buffer);
            }
            buffer->get_processing_chain()->set_preferred_token(ProcessingToken::GRAPHICS_BACKEND);
            buffers.push_back(buffer);
            return buffer;
        }

        static std::vector<uint8_t> bytes_of(const std::shared_ptr<VKBuffer>& buffer)
        {
            std::vector<uint8_t> out(buffer->get_size_bytes());
            std::memcpy(out.data(), buffer->get_buffer_resources().mapped_ptr, out.size());
            return out;
        }

        std::shared_ptr<BufferPipeline> make_pipeline()
        {
            auto pipeline = BufferPipeline::create(*scheduler, manager);
            pipelines.push_back(pipeline);
            return pipeline;
        }

        void tick()
        {
            scheduler->process_buffer_cycle_tasks();
            scheduler->process_token(Vruta::ProcessingToken::SAMPLE_ACCURATE, block);
            manager->process_token(ProcessingToken::GRAPHICS_BACKEND, 1);
        }

        void run(const std::shared_ptr<BufferPipeline>& pipeline, uint32_t limit = 200)
        {
            for (uint32_t used = 0; pipeline->is_running() && used < limit; ++used) {
                tick();
            }
        }

        uint64_t block {};
        std::shared_ptr<Vruta::TaskScheduler> scheduler;
        std::shared_ptr<Buffers::BufferManager> manager;
        std::vector<std::shared_ptr<VKBuffer>> buffers;
        std::vector<std::shared_ptr<BufferPipeline>> pipelines;
    };

    TEST_F(BufferPipelineGpuTest, CapturingARawBufferDeliversItsBytesOnceTheReadResolves)
    {
        const auto source = make_buffer(256);
        const auto bytes = pattern(256, 3);
        Buffers::upload_to_gpu(bytes.data(), bytes.size(), source);
        const auto pipeline = make_pipeline();
        std::vector<std::vector<uint8_t>> seen;

        *pipeline
            >> BufferOperation::capture_from(source)
            >> BufferOperation::dispatch_to([&](DataVariant& data, uint32_t) {
                   if (const auto* values = std::get_if<std::vector<uint8_t>>(&data)) {
                       seen.push_back(*values);
                   }
               });
        pipeline->execute_for_cycles(12);
        run(pipeline);

        ASSERT_FALSE(seen.empty());
        EXPECT_EQ(seen.front(), bytes);
    }

    TEST_F(BufferPipelineGpuTest, ACaptureIsSkippedUntilTheFirstReadResolves)
    {
        const auto source = make_buffer(256);
        const auto bytes = pattern(256, 11);
        Buffers::upload_to_gpu(bytes.data(), bytes.size(), source);
        const auto pipeline = make_pipeline();
        std::vector<uint32_t> dispatched;

        *pipeline
            >> BufferOperation::capture_from(source)
            >> BufferOperation::dispatch_to([&](DataVariant&, uint32_t cycle) { dispatched.push_back(cycle); });
        pipeline->execute_for_cycles(12);
        tick();

        EXPECT_TRUE(dispatched.empty());
        run(pipeline);
        EXPECT_FALSE(dispatched.empty());
    }

    TEST_F(BufferPipelineGpuTest, RoutingAudioIntoARawBufferWritesItsBytesWhenTheManagerProcesses)
    {
        const auto source = std::make_shared<Buffers::AudioBuffer>(0, static_cast<uint32_t>(block));
        std::vector<double> samples(block);
        for (size_t i = 0; i < samples.size(); ++i) {
            samples[i] = 1.0 + static_cast<double>(i);
        }
        source->get_data() = samples;
        const auto target = make_buffer(block * sizeof(double));
        const auto pipeline = make_pipeline();

        *pipeline
            >> BufferOperation::capture_from(source)
            >> BufferOperation::route_to_buffer(target);
        pipeline->execute_for_cycles(1);
        run(pipeline);
        tick();
        tick();

        std::vector<uint8_t> expected(block * sizeof(double));
        std::memcpy(expected.data(), samples.data(), expected.size());
        EXPECT_EQ(bytes_of(target), expected);
    }

    TEST_F(BufferPipelineGpuTest, LoadingVideoFramesWritesThemIntoARawBuffer)
    {
        const auto video = std::make_shared<Kakshya::DynamicVideoStream>(VideoSupport::rgba8_spec(4));
        (void)VideoSupport::append_ramp(*video, 2);
        const auto expected = VideoSupport::frames_bytes(0, 2);
        const auto target = make_buffer(expected.size());
        const auto pipeline = make_pipeline();

        *pipeline >> BufferOperation::load_from_container(video, target, 0, 2);
        pipeline->execute_once();
        run(pipeline);
        tick();
        tick();

        EXPECT_EQ(bytes_of(target), expected);
    }

    TEST_F(BufferPipelineGpuTest, FusingRawBuffersWritesTheFusedBytesIntoTheTarget)
    {
        const auto left = make_buffer(128);
        const auto right = make_buffer(128);
        const auto target = make_buffer(128);
        const auto a = pattern(128, 1);
        const auto b = pattern(128, 5);
        Buffers::upload_to_gpu(a.data(), a.size(), left);
        Buffers::upload_to_gpu(b.data(), b.size(), right);
        const auto pipeline = make_pipeline();
        int fused = 0;
        pipeline->process_timing(Vruta::DelayContext::BUFFER_BASED);

        *pipeline
            >> BufferOperation::fuse_data(
                { left, right },
                [&](std::vector<DataVariant>& inputs, uint32_t) {
                    ++fused;
                    auto sum = std::get<std::vector<uint8_t>>(inputs[0]);
                    const auto& other = std::get<std::vector<uint8_t>>(inputs[1]);
                    for (size_t i = 0; i < sum.size(); ++i) {
                        sum[i] = static_cast<uint8_t>(sum[i] + other[i]);
                    }
                    return DataVariant(std::move(sum));
                },
                target);
        pipeline->execute_for_cycles(12);
        run(pipeline);
        tick();
        tick();

        std::vector<uint8_t> expected(128);
        for (size_t i = 0; i < expected.size(); ++i) {
            expected[i] = static_cast<uint8_t>(a[i] + b[i]);
        }
        EXPECT_GE(fused, 1);
        EXPECT_EQ(bytes_of(target), expected);
    }

    TEST_F(BufferPipelineGpuTest, ACapturedRawBufferCanBeRoutedIntoAVideoStreamAsFrames)
    {
        const auto frame = VideoSupport::frame_bytes(3);
        const auto source = make_buffer(frame.size());
        Buffers::upload_to_gpu(frame.data(), frame.size(), source);
        const auto video = std::make_shared<Kakshya::DynamicVideoStream>(VideoSupport::rgba8_spec(0));
        const auto pipeline = make_pipeline();

        *pipeline
            >> BufferOperation::capture_from(source)
            >> BufferOperation::route_to_container(video);
        pipeline->execute_for_cycles(8);
        run(pipeline);

        ASSERT_GE(video->get_write_head(), 1U);
        for (uint64_t f = 0; f < video->get_write_head(); ++f) {
            EXPECT_EQ(VideoSupport::pixels_of(*video, f), frame) << f;
        }
    }

    TEST_F(BufferPipelineGpuTest, AStreamingModifierActsOnARawBufferWhileRunningAndLetsGoAfterwards)
    {
        const auto target = make_buffer(64);
        const auto pipeline = make_pipeline();
        int modified = 0;
        auto modify = BufferOperation::modify_buffer(target, [&modified](const std::shared_ptr<VKBuffer>&) { ++modified; });
        modify.as_streaming();

        *pipeline >> std::move(modify);
        pipeline->execute_for_cycles(4);
        run(pipeline);
        tick();
        const int settled = modified;
        tick();
        tick();

        EXPECT_GE(settled, 1);
        EXPECT_EQ(modified, settled);
    }

} // namespace
} // namespace MayaFlux::Test
