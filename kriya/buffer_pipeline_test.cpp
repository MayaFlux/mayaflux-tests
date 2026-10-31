#include "gtest/gtest.h"

#include "../kakshya/video_test_support.hpp"

#include "MayaFlux/Buffers/AudioBuffer.hpp"
#include "MayaFlux/Buffers/BufferManager.hpp"
#include "MayaFlux/Buffers/BufferProcessor.hpp"
#include "MayaFlux/Kakshya/Source/DynamicSoundStream.hpp"
#include "MayaFlux/Kakshya/Source/DynamicVideoStream.hpp"
#include "MayaFlux/Kriya/Awaiters/DelayAwaiters.hpp"
#include "MayaFlux/Kriya/BufferPipeline.hpp"

namespace MayaFlux::Test {
namespace {

    using Buffers::AudioBuffer;
    using Buffers::ProcessingToken;
    using Kakshya::DataVariant;
    using Kakshya::DynamicSoundStream;
    using Kakshya::DynamicVideoStream;
    using Kriya::BufferOperation;
    using Kriya::BufferPipeline;
    using Kriya::ExecutionStrategy;

    constexpr uint32_t k_rate = 48000;

    double sample(uint64_t index)
    {
        return 1.0 + static_cast<double>(index);
    }

    std::vector<double> ramp_blocks(uint64_t first_block, uint64_t blocks, uint64_t block)
    {
        std::vector<double> values;
        values.reserve(blocks * block);
        for (uint64_t i = first_block * block; i < (first_block + blocks) * block; ++i) {
            values.push_back(sample(i));
        }
        return values;
    }

    std::vector<double> values_of(const DataVariant& data)
    {
        const auto* values = std::get_if<std::vector<double>>(&data);
        return values ? *values : std::vector<double> {};
    }

    size_t size_of(const DataVariant& data)
    {
        return std::visit([](const auto& values) { return values.size(); }, data);
    }

    Vruta::SoundRoutine count_sample_delays(int& hits, uint64_t samples)
    {
        for (int i = 0; i < 3; ++i) {
            ++hits;
            co_await Kriya::SampleDelay { samples };
        }
    }

    class FillProcessor : public Buffers::BufferProcessor {
    public:
        void processing_function(const std::shared_ptr<Buffers::Buffer>& buffer) override
        {
            auto& data = std::dynamic_pointer_cast<AudioBuffer>(buffer)->get_data();
            data = ramp_blocks(runs, 1, data.size());
            ++runs;
        }

        uint64_t runs {};
    };

    class BufferPipelineTest : public ::testing::Test {
    protected:
        void SetUp() override
        {
            block = Buffers::s_preferred_buffer_size;
            scheduler = std::make_shared<Vruta::TaskScheduler>(k_rate);
            manager = std::make_shared<Buffers::BufferManager>();
            source = make_buffer();
            set_source_block(0);
        }

        void TearDown() override
        {
            for (const auto& pipeline : pipelines) {
                pipeline->end();
            }
        }

        [[nodiscard]] std::shared_ptr<AudioBuffer> make_buffer() const
        {
            return std::make_shared<AudioBuffer>(0, static_cast<uint32_t>(block));
        }

        std::shared_ptr<BufferPipeline> make_pipeline()
        {
            auto pipeline = BufferPipeline::create(*scheduler, manager);
            pipelines.push_back(pipeline);
            return pipeline;
        }

        void set_source_block(uint64_t index)
        {
            source->get_data() = ramp_blocks(index, 1, block);
        }

        void tick()
        {
            ++ticks;
            scheduler->process_buffer_cycle_tasks();
            scheduler->process_token(Vruta::ProcessingToken::SAMPLE_ACCURATE, block);
        }

        void tick(uint32_t count)
        {
            for (uint32_t i = 0; i < count; ++i) {
                tick();
            }
        }

        uint32_t run(const std::shared_ptr<BufferPipeline>& pipeline, uint32_t limit = 600)
        {
            uint32_t used = 0;
            while (pipeline->is_running() && used < limit) {
                tick();
                ++used;
            }
            return used;
        }

        Kriya::OperationFunction advancing_source()
        {
            return [this](DataVariant&, uint32_t) { set_source_block(++next_block); };
        }

        uint64_t block {};
        uint64_t next_block {};
        uint32_t ticks {};
        std::shared_ptr<Vruta::TaskScheduler> scheduler;
        std::shared_ptr<Buffers::BufferManager> manager;
        std::shared_ptr<AudioBuffer> source;
        std::vector<std::shared_ptr<BufferPipeline>> pipelines;
    };

    TEST_F(BufferPipelineTest, ACaptureThenATransformRunOncePerCycle)
    {
        const auto pipeline = make_pipeline();
        std::vector<uint32_t> captured;
        std::vector<uint32_t> transformed;
        int completed = 0;
        pipeline->on_complete([&completed] { ++completed; });

        *pipeline
            >> BufferOperation::capture_from(source).for_cycles(1).on_data_ready([&](DataVariant&, uint32_t cycle) { captured.push_back(cycle); })
            >> BufferOperation::transform([&](DataVariant& data, uint32_t cycle) {
                   transformed.push_back(cycle);
                   return data;
               });
        pipeline->execute_for_cycles(3);
        run(pipeline);

        EXPECT_EQ(captured, (std::vector<uint32_t> { 0, 1, 2 }));
        EXPECT_EQ(transformed, (std::vector<uint32_t> { 0, 1, 2 }));
        EXPECT_EQ(completed, 1);
        EXPECT_FALSE(pipeline->is_running());
        EXPECT_EQ(pipeline->get_current_cycle(), 3U);
    }

    TEST_F(BufferPipelineTest, ATransformSeesTheBufferContents)
    {
        const auto pipeline = make_pipeline();
        std::vector<double> seen;

        *pipeline
            >> BufferOperation::capture_from(source)
            >> BufferOperation::transform([&](DataVariant& data, uint32_t) {
                   seen = values_of(data);
                   return data;
               });
        pipeline->execute_once();
        run(pipeline);

        EXPECT_EQ(seen, ramp_blocks(0, 1, block));
    }

    TEST_F(BufferPipelineTest, AccumulationJoinsTheCapturedBlocksInOrder)
    {
        const auto pipeline = make_pipeline();
        std::vector<std::vector<double>> seen;

        *pipeline
            >> BufferOperation::capture_from(source).for_cycles(4).on_data_ready(advancing_source())
            >> BufferOperation::transform([&](DataVariant& data, uint32_t) {
                   seen.push_back(values_of(data));
                   return data;
               });
        pipeline->execute_for_cycles(1);
        run(pipeline);

        ASSERT_EQ(seen.size(), 1U);
        EXPECT_EQ(seen[0], ramp_blocks(0, 4, block));
    }

    TEST_F(BufferPipelineTest, AccumulationStartsOverEveryCycle)
    {
        const auto pipeline = make_pipeline();
        std::vector<std::vector<double>> seen;

        *pipeline
            >> BufferOperation::capture_from(source).for_cycles(4).on_data_ready(advancing_source())
            >> BufferOperation::transform([&](DataVariant& data, uint32_t) {
                   seen.push_back(values_of(data));
                   return data;
               });
        pipeline->execute_for_cycles(2);
        run(pipeline);

        ASSERT_EQ(seen.size(), 2U);
        EXPECT_EQ(seen[0], ramp_blocks(0, 4, block));
        EXPECT_EQ(seen[1], ramp_blocks(4, 4, block));
    }

    TEST_F(BufferPipelineTest, ACircularCaptureKeepsTheNewestSamples)
    {
        const auto pipeline = make_pipeline();
        std::vector<double> seen;

        *pipeline
            >> BufferOperation::capture_from(source).for_cycles(5).as_circular(static_cast<uint32_t>(2 * block)).on_data_ready(advancing_source())
            >> BufferOperation::transform([&](DataVariant& data, uint32_t) {
                   seen = values_of(data);
                   return data;
               });
        pipeline->execute_for_cycles(1);
        run(pipeline);

        EXPECT_EQ(seen, ramp_blocks(3, 2, block));
    }

    TEST_F(BufferPipelineTest, AnOverlappingWindowSlidesByTheHop)
    {
        const auto pipeline = make_pipeline();
        std::vector<double> seen;

        *pipeline
            >> BufferOperation::capture_from(source).for_cycles(5).with_window(static_cast<uint32_t>(2 * block), 0.5F).on_data_ready(advancing_source())
            >> BufferOperation::transform([&](DataVariant& data, uint32_t) {
                   seen = values_of(data);
                   return data;
               });
        pipeline->execute_for_cycles(1);
        run(pipeline);

        EXPECT_EQ(seen, ramp_blocks(3, 2, block));
    }

    TEST_F(BufferPipelineTest, AWindowWithoutOverlapRestartsOnceFull)
    {
        const auto pipeline = make_pipeline();
        std::vector<double> seen;

        *pipeline
            >> BufferOperation::capture_from(source).for_cycles(5).with_window(static_cast<uint32_t>(2 * block), 0.0F).on_data_ready(advancing_source())
            >> BufferOperation::transform([&](DataVariant& data, uint32_t) {
                   seen = values_of(data);
                   return data;
               });
        pipeline->execute_for_cycles(1);
        run(pipeline);

        EXPECT_EQ(seen, ramp_blocks(4, 1, block));
    }

    TEST_F(BufferPipelineTest, ATriggeredCaptureKeepsDataOnlyOnceItsConditionHolds)
    {
        const auto pipeline = make_pipeline();
        std::vector<size_t> sizes;
        int asked = 0;

        *pipeline
            >> BufferOperation::capture_from(source).until_condition([&asked] { return ++asked >= 3; })
            >> BufferOperation::transform([&](DataVariant& data, uint32_t) {
                   sizes.push_back(size_of(data));
                   return data;
               });
        pipeline->execute_for_cycles(3);
        run(pipeline);

        EXPECT_EQ(sizes, (std::vector<size_t> { 0, 0, block }));
    }

    TEST_F(BufferPipelineTest, OnCaptureProcessingRunsTheDefaultProcessorBeforeEachRead)
    {
        const auto fill = std::make_shared<FillProcessor>();
        source->set_default_processor(fill);
        const auto pipeline = make_pipeline();
        std::vector<double> first_samples;

        *pipeline
            >> BufferOperation::capture_from(source).for_cycles(3).on_capture_processing().on_data_ready([&](DataVariant& data, uint32_t) {
                   first_samples.push_back(values_of(data).front());
               });
        pipeline->execute_for_cycles(1);
        run(pipeline);

        EXPECT_EQ(fill->runs, 3U);
        EXPECT_EQ(first_samples, (std::vector<double> { sample(0), sample(block), sample(2 * block) }));
    }

    TEST_F(BufferPipelineTest, AutomaticAndManualProcessingNeverRunTheDefaultProcessor)
    {
        const auto fill = std::make_shared<FillProcessor>();
        source->set_default_processor(fill);
        const auto automatic = make_pipeline();
        const auto manual = make_pipeline();

        *automatic >> BufferOperation::capture_from(source).for_cycles(2).auto_processing();
        *manual >> BufferOperation::capture_from(source).for_cycles(2).manual_processing();
        automatic->execute_for_cycles(1);
        manual->execute_for_cycles(1);
        run(automatic);
        run(manual);

        EXPECT_EQ(fill->runs, 0U);
    }

    TEST_F(BufferPipelineTest, ACapturedBlockIsRoutedStraightIntoASoundStream)
    {
        const auto stream = std::make_shared<DynamicSoundStream>(k_rate, 1);
        const auto pipeline = make_pipeline();

        *pipeline
            >> BufferOperation::capture_from(source).for_cycles(3).on_data_ready(advancing_source())
            >> BufferOperation::route_to_container(stream);
        pipeline->execute_for_cycles(1);
        run(pipeline);

        EXPECT_EQ(stream->get_write_head(0), 3 * block);
        const auto frames = stream->get_channel_frames(0, 0, 3 * block);
        EXPECT_EQ(std::vector<double>(frames.begin(), frames.end()), ramp_blocks(0, 3, block));
    }

    TEST_F(BufferPipelineTest, TheRouteChannelSelectsTheStreamChannel)
    {
        const auto stream = std::make_shared<DynamicSoundStream>(k_rate, 2);
        const auto pipeline = make_pipeline();

        *pipeline
            >> BufferOperation::capture_from(source)
            >> BufferOperation::route_to_container(stream, 1);
        pipeline->execute_for_cycles(1);
        run(pipeline);

        EXPECT_EQ(stream->get_write_head(0), 0U);
        EXPECT_EQ(stream->get_write_head(1), block);
        const auto frames = stream->get_channel_frames(1, 0, block);
        EXPECT_EQ(std::vector<double>(frames.begin(), frames.end()), ramp_blocks(0, 1, block));
    }

    TEST_F(BufferPipelineTest, WhatIsRoutedIsWhatTheTransformReturned)
    {
        const auto stream = std::make_shared<DynamicSoundStream>(k_rate, 1);
        const auto pipeline = make_pipeline();

        *pipeline
            >> BufferOperation::capture_from(source)
            >> BufferOperation::transform([](DataVariant& data, uint32_t) {
                   auto values = std::get<std::vector<double>>(data);
                   for (double& value : values) {
                       value *= 0.5;
                   }
                   return DataVariant(std::move(values));
               })
            >> BufferOperation::route_to_container(stream);
        pipeline->execute_for_cycles(1);
        run(pipeline);

        std::vector<double> expected = ramp_blocks(0, 1, block);
        for (double& value : expected) {
            value *= 0.5;
        }
        const auto frames = stream->get_channel_frames(0, 0, block);
        EXPECT_EQ(std::vector<double>(frames.begin(), frames.end()), expected);
    }

    TEST_F(BufferPipelineTest, ARoutedBufferTakesTheDataWhenTheManagerNextProcesses)
    {
        const auto target = make_buffer();
        manager->add_buffer(target, ProcessingToken::AUDIO_BACKEND, 0);
        const auto pipeline = make_pipeline();

        *pipeline
            >> BufferOperation::capture_from(source)
            >> BufferOperation::route_to_buffer(target);
        pipeline->execute_for_cycles(1);
        run(pipeline);

        EXPECT_EQ(target->get_data(), std::vector<double>(block, 0.0));
        manager->process_token(ProcessingToken::AUDIO_BACKEND, 1);
        EXPECT_EQ(target->get_data(), ramp_blocks(0, 1, block));
    }

    TEST_F(BufferPipelineTest, LoadingFillsTheBufferWithTheRequestedRangeEveryCycle)
    {
        const auto stream = std::make_shared<DynamicSoundStream>(k_rate, 1);
        const auto ramp = ramp_blocks(0, 4, block);
        stream->write_frames(std::vector<std::span<const double>> { std::span<const double>(ramp) }, 0);
        const auto target = make_buffer();
        const auto pipeline = make_pipeline();
        std::vector<std::vector<double>> after_cycle;
        pipeline->with_lifecycle({}, [&](uint32_t) { after_cycle.push_back(target->get_data()); });

        *pipeline >> BufferOperation::load_from_container(stream, target, 0, static_cast<uint32_t>(block));
        pipeline->execute_for_cycles(2);
        run(pipeline);

        ASSERT_EQ(after_cycle.size(), 2U);
        EXPECT_EQ(after_cycle[0], ramp_blocks(0, 1, block));
        EXPECT_EQ(after_cycle[1], ramp_blocks(0, 1, block));
    }

    TEST_F(BufferPipelineTest, LoadingWithoutALengthTakesTheRestOfTheStream)
    {
        const auto stream = std::make_shared<DynamicSoundStream>(k_rate, 1);
        const auto ramp = ramp_blocks(0, 4, block);
        stream->write_frames(std::vector<std::span<const double>> { std::span<const double>(ramp) }, 0);
        const auto whole = make_buffer();
        const auto tail = make_buffer();
        const auto pipeline = make_pipeline();

        *pipeline >> BufferOperation::load_from_container(stream, whole);
        *pipeline >> BufferOperation::load_from_container(stream, tail, 3 * block);
        pipeline->execute_once();
        run(pipeline);

        EXPECT_EQ(whole->get_data(), ramp);
        EXPECT_EQ(tail->get_data(), ramp_blocks(3, 1, block));
    }

    TEST_F(BufferPipelineTest, LoadingLeavesTheStreamsReadPositionAlone)
    {
        const auto stream = std::make_shared<DynamicSoundStream>(k_rate, 1);
        const auto ramp = ramp_blocks(0, 4, block);
        stream->write_frames(std::vector<std::span<const double>> { std::span<const double>(ramp) }, 0);
        stream->set_read_position({ 7 });
        const auto pipeline = make_pipeline();

        *pipeline >> BufferOperation::load_from_container(stream, make_buffer(), 0, static_cast<uint32_t>(block));
        pipeline->execute_for_cycles(2);
        run(pipeline);

        EXPECT_EQ(stream->get_read_position(), (std::vector<uint64_t> { 7 }));
    }

    TEST_F(BufferPipelineTest, LoadingStartsAtTheRequestedFrame)
    {
        const auto stream = std::make_shared<DynamicSoundStream>(k_rate, 1);
        const auto ramp = ramp_blocks(0, 4, block);
        stream->write_frames(std::vector<std::span<const double>> { std::span<const double>(ramp) }, 0);
        const auto target = make_buffer();
        const auto pipeline = make_pipeline();

        *pipeline >> BufferOperation::load_from_container(stream, target, 2 * block, static_cast<uint32_t>(block));
        pipeline->execute_once();
        run(pipeline);

        EXPECT_EQ(target->get_data(), ramp_blocks(2, 1, block));
    }

    TEST_F(BufferPipelineTest, FusingBuffersWritesTheFusedResultIntoTheTarget)
    {
        const auto low = make_buffer();
        const auto high = make_buffer();
        low->get_data().assign(block, 1.0);
        high->get_data().assign(block, 3.0);
        const auto target = make_buffer();
        const auto pipeline = make_pipeline();
        std::vector<uint32_t> cycles;

        *pipeline
            >> BufferOperation::fuse_data(
                { low, high },
                [&](std::vector<DataVariant>& inputs, uint32_t cycle) {
                    cycles.push_back(cycle);
                    auto fused = std::get<std::vector<double>>(inputs[0]);
                    const auto& other = std::get<std::vector<double>>(inputs[1]);
                    for (size_t i = 0; i < fused.size(); ++i) {
                        fused[i] = (fused[i] + other[i]) / 2.0;
                    }
                    return DataVariant(std::move(fused));
                },
                target);
        pipeline->execute_for_cycles(2);
        run(pipeline);

        EXPECT_EQ(cycles, (std::vector<uint32_t> { 0, 1 }));
        EXPECT_EQ(target->get_data(), std::vector<double>(block, 2.0));
    }

    TEST_F(BufferPipelineTest, FusingStreamsWritesTheFusedResultIntoTheTargetStream)
    {
        const auto left = std::make_shared<DynamicSoundStream>(k_rate, 1);
        const auto right = std::make_shared<DynamicSoundStream>(k_rate, 1);
        const auto target = std::make_shared<DynamicSoundStream>(k_rate, 1);
        const auto ramp = ramp_blocks(0, 1, block);
        const std::vector<double> constant(block, 10.0);
        left->write_frames(std::vector<std::span<const double>> { std::span<const double>(ramp) }, 0);
        right->write_frames(std::vector<std::span<const double>> { std::span<const double>(constant) }, 0);
        const auto pipeline = make_pipeline();

        *pipeline
            >> BufferOperation::fuse_containers(
                { left, right },
                [](std::vector<DataVariant>& inputs, uint32_t) {
                    auto fused = std::get<std::vector<double>>(inputs[0]);
                    const auto& other = std::get<std::vector<double>>(inputs[1]);
                    for (size_t i = 0; i < fused.size(); ++i) {
                        fused[i] += other[i];
                    }
                    return DataVariant(std::move(fused));
                },
                target);
        pipeline->execute_once();
        run(pipeline);

        std::vector<double> expected = ramp;
        for (double& value : expected) {
            value += 10.0;
        }
        EXPECT_EQ(target->get_write_head(0), block);
        const auto frames = target->get_channel_frames(0, 0, block);
        EXPECT_EQ(std::vector<double>(frames.begin(), frames.end()), expected);
    }

    TEST_F(BufferPipelineTest, AnyDataShapeCanBeRoutedIntoAVideoStream)
    {
        const auto video = std::make_shared<DynamicVideoStream>(VideoSupport::rgba8_spec(0));
        const auto pipeline = make_pipeline();

        *pipeline
            >> BufferOperation::transform([](DataVariant&, uint32_t) { return DataVariant(VideoSupport::frames_bytes(0, 2)); })
            >> BufferOperation::route_to_container(video);
        pipeline->execute_once();
        run(pipeline);

        EXPECT_EQ(video->get_write_head(), 2U);
        EXPECT_EQ(VideoSupport::pixels_of(*video, 0), VideoSupport::frame_bytes(0));
        EXPECT_EQ(VideoSupport::pixels_of(*video, 1), VideoSupport::frame_bytes(1));
    }

    TEST_F(BufferPipelineTest, ADispatchHandlerReceivesTheLatestDataAndTheCycle)
    {
        const auto pipeline = make_pipeline();
        std::vector<uint32_t> cycles;
        std::vector<double> first_samples;

        *pipeline
            >> BufferOperation::capture_from(source).on_data_ready(advancing_source())
            >> BufferOperation::dispatch_to([&](DataVariant& data, uint32_t cycle) {
                   cycles.push_back(cycle);
                   first_samples.push_back(values_of(data).front());
               });
        pipeline->execute_for_cycles(3);
        run(pipeline);

        EXPECT_EQ(cycles, (std::vector<uint32_t> { 0, 1, 2 }));
        EXPECT_EQ(first_samples, (std::vector<double> { sample(0), sample(block), sample(2 * block) }));
    }

    TEST_F(BufferPipelineTest, AnOperationEveryNCyclesRunsOnTheMultiples)
    {
        const auto pipeline = make_pipeline();
        std::vector<uint32_t> cycles;
        auto transform = BufferOperation::transform([&](DataVariant& data, uint32_t cycle) {
            cycles.push_back(cycle);
            return data;
        });
        transform.every_n_cycles(3);

        *pipeline >> BufferOperation::capture_from(source);
        *pipeline >> std::move(transform);
        pipeline->execute_for_cycles(9);
        run(pipeline);

        EXPECT_EQ(cycles, (std::vector<uint32_t> { 0, 3, 6 }));
    }

    TEST_F(BufferPipelineTest, AStreamingModifierActsOnTheBufferWhileRunningAndLetsGoAfterwards)
    {
        const auto target = make_buffer();
        target->get_data().assign(block, 1.0);
        manager->add_buffer(target, ProcessingToken::AUDIO_BACKEND, 0);
        const auto pipeline = make_pipeline();
        auto modify = BufferOperation::modify_buffer(target, [](const std::shared_ptr<AudioBuffer>& buffer) {
            for (double& value : buffer->get_data()) {
                value *= 0.5;
            }
        });
        modify.as_streaming();

        *pipeline >> std::move(modify);
        pipeline->execute_for_cycles(4);
        for (uint32_t i = 0; i < 100 && pipeline->is_running(); ++i) {
            tick();
            manager->process_token(ProcessingToken::AUDIO_BACKEND, 1);
        }

        const double settled = target->get_data().front();
        EXPECT_LT(settled, 1.0);
        EXPECT_GT(settled, 0.0);
        manager->process_token(ProcessingToken::AUDIO_BACKEND, 1);
        manager->process_token(ProcessingToken::AUDIO_BACKEND, 1);
        EXPECT_DOUBLE_EQ(target->get_data().front(), settled);
    }

    TEST_F(BufferPipelineTest, StreamingFlowsEachCapturedBlockThroughToTheNextOperation)
    {
        const auto pipeline = make_pipeline();
        std::vector<size_t> sizes;

        pipeline->with_strategy(ExecutionStrategy::STREAMING);
        *pipeline
            >> BufferOperation::capture_from(source).for_cycles(3).on_data_ready(advancing_source())
            >> BufferOperation::transform([&](DataVariant& data, uint32_t) {
                   sizes.push_back(size_of(data));
                   return data;
               });
        pipeline->execute_for_cycles(1);
        run(pipeline);

        EXPECT_EQ(sizes, (std::vector<size_t> { block, 2 * block, 3 * block }));
    }

    TEST_F(BufferPipelineTest, StreamingKeepsFlowingOverSeveralCycles)
    {
        const auto pipeline = make_pipeline();
        std::vector<uint32_t> transformed;

        pipeline->with_strategy(ExecutionStrategy::STREAMING);
        *pipeline
            >> BufferOperation::capture_from(source)
            >> BufferOperation::transform([&](DataVariant& data, uint32_t cycle) {
                   transformed.push_back(cycle);
                   return data;
               });
        pipeline->execute_for_cycles(3);
        run(pipeline);

        EXPECT_EQ(transformed, (std::vector<uint32_t> { 0, 1, 2 }));
    }

    TEST_F(BufferPipelineTest, UnimplementedStrategiesBehaveLikePhased)
    {
        for (const auto strategy : { ExecutionStrategy::PARALLEL, ExecutionStrategy::REACTIVE }) {
            SCOPED_TRACE(static_cast<int>(strategy));
            const auto pipeline = make_pipeline();
            std::vector<uint32_t> captured;
            pipeline->with_strategy(strategy);

            *pipeline >> BufferOperation::capture_from(source).on_data_ready([&](DataVariant&, uint32_t cycle) { captured.push_back(cycle); });
            pipeline->execute_for_cycles(3);
            run(pipeline);

            EXPECT_EQ(captured, (std::vector<uint32_t> { 0, 1, 2 }));
        }
    }

    TEST_F(BufferPipelineTest, ExecuteOnceRunsExactlyOneCycle)
    {
        const auto pipeline = make_pipeline();
        std::vector<uint32_t> captured;
        int completed = 0;
        pipeline->on_complete([&completed] { ++completed; });

        *pipeline >> BufferOperation::capture_from(source).on_data_ready([&](DataVariant&, uint32_t cycle) { captured.push_back(cycle); });
        pipeline->execute_once();
        run(pipeline);

        EXPECT_EQ(captured, (std::vector<uint32_t> { 0 }));
        EXPECT_EQ(completed, 1);
        EXPECT_FALSE(pipeline->is_running());
    }

    TEST_F(BufferPipelineTest, BufferRateExecutionRunsTheGivenNumberOfCycles)
    {
        const auto pipeline = make_pipeline();
        int captured = 0;

        *pipeline >> BufferOperation::capture_from(source).on_data_ready([&](DataVariant&, uint32_t) { ++captured; });
        pipeline->execute_buffer_rate(5);
        run(pipeline);

        EXPECT_EQ(captured, 5);
    }

    TEST_F(BufferPipelineTest, AContinuousPipelineKeepsRunningUntilItIsEnded)
    {
        const auto pipeline = make_pipeline();
        int captured = 0;

        *pipeline >> BufferOperation::capture_from(source).on_data_ready([&](DataVariant&, uint32_t) { ++captured; });
        pipeline->execute_continuous();
        tick(12);

        EXPECT_TRUE(pipeline->is_running());
        EXPECT_GE(captured, 10);

        pipeline->end();

        EXPECT_FALSE(pipeline->is_running());
    }

    TEST_F(BufferPipelineTest, EndingCancelsARunningPipelineAndReportsCompletionOnce)
    {
        const auto pipeline = make_pipeline();
        int completed = 0;
        pipeline->on_complete([&completed] { ++completed; });

        *pipeline >> BufferOperation::capture_from(source);
        pipeline->execute_continuous();
        tick(3);
        ASSERT_TRUE(pipeline->is_running());

        pipeline->end();
        pipeline->end();

        EXPECT_FALSE(pipeline->is_running());
        EXPECT_EQ(completed, 1);
    }

    TEST_F(BufferPipelineTest, AnEmptyPipelineFinishesAtOnce)
    {
        const auto pipeline = make_pipeline();

        pipeline->execute_for_cycles(3);
        tick();

        EXPECT_FALSE(pipeline->is_running());
    }

    TEST_F(BufferPipelineTest, ExecutingWithoutASchedulerIsRefused)
    {
        const auto pipeline = std::make_shared<BufferPipeline>();

        EXPECT_THROW(pipeline->execute_once(), std::runtime_error);
        EXPECT_THROW(pipeline->execute_for_cycles(2), std::runtime_error);
        EXPECT_THROW(pipeline->execute_buffer_rate(2), std::runtime_error);
        EXPECT_THROW(pipeline->execute_scheduled(2, 8), std::runtime_error);
    }

    TEST_F(BufferPipelineTest, EachCaptureIterationWaitsOneBufferCycle)
    {
        const auto pipeline = make_pipeline();
        std::vector<uint32_t> at_tick;

        *pipeline >> BufferOperation::capture_from(source).for_cycles(4).on_data_ready([&](DataVariant&, uint32_t) { at_tick.push_back(ticks); });
        pipeline->execute_for_cycles(1);
        run(pipeline);

        ASSERT_EQ(at_tick.size(), 4U);
        for (size_t i = 1; i < at_tick.size(); ++i) {
            EXPECT_EQ(at_tick[i] - at_tick[i - 1], 1U) << i;
        }
    }

    TEST_F(BufferPipelineTest, TheHarnessResumesASampleDelayOnceEnoughSamplesHavePassed)
    {
        int hits = 0;
        const auto routine = std::make_shared<Vruta::SoundRoutine>(count_sample_delays(hits, 4 * block));
        scheduler->add_task(routine);

        tick(40);

        EXPECT_EQ(hits, 3);
        EXPECT_FALSE(routine->is_active());
    }

    TEST_F(BufferPipelineTest, ScheduledExecutionSpacesOperationsBySamples)
    {
        const auto plain = make_pipeline();
        *plain
            >> BufferOperation::capture_from(source)
            >> BufferOperation::transform([](DataVariant& data, uint32_t) { return data; });
        plain->execute_for_cycles(3);
        const uint32_t plain_ticks = run(plain);

        const auto spaced = make_pipeline();
        std::vector<uint32_t> transformed_at;
        *spaced
            >> BufferOperation::capture_from(source)
            >> BufferOperation::transform([&](DataVariant& data, uint32_t) {
                   transformed_at.push_back(ticks);
                   return data;
               });
        spaced->execute_scheduled(3, 4 * block);
        const uint32_t spaced_ticks = run(spaced);

        EXPECT_EQ(transformed_at.size(), 3U);
        EXPECT_GE(spaced_ticks, plain_ticks + 3 * 2);
    }

    TEST_F(BufferPipelineTest, LifecycleCallbacksBracketEveryCycle)
    {
        const auto pipeline = make_pipeline();
        std::vector<std::string> events;
        pipeline->with_lifecycle(
            [&](uint32_t cycle) { events.push_back("start" + std::to_string(cycle)); },
            [&](uint32_t cycle) { events.push_back("end" + std::to_string(cycle)); });

        *pipeline >> BufferOperation::capture_from(source);
        pipeline->execute_for_cycles(3);
        run(pipeline);

        EXPECT_EQ(events, (std::vector<std::string> { "start0", "end0", "start1", "end1", "start2", "end2" }));
    }

    TEST_F(BufferPipelineTest, AnAsynchronousBranchRunsWhenItsConditionHolds)
    {
        const auto pipeline = make_pipeline();
        int main_captures = 0;
        int branch_runs = 0;

        *pipeline >> BufferOperation::capture_from(source).on_data_ready([&](DataVariant&, uint32_t) { ++main_captures; });
        pipeline->branch_if(
            [](uint32_t cycle) { return cycle % 3 == 0; },
            [&](BufferPipeline& branch) {
                branch >> BufferOperation::dispatch_to([&](DataVariant&, uint32_t) { ++branch_runs; });
            },
            false);
        pipeline->execute_for_cycles(9);
        run(pipeline);
        tick(6);

        EXPECT_EQ(main_captures, 9);
        EXPECT_EQ(branch_runs, 3);
    }

    TEST_F(BufferPipelineTest, ASynchronousBranchFinishesInsideTheCycleThatStartedIt)
    {
        const auto pipeline = make_pipeline();
        std::vector<std::string> events;
        pipeline->with_lifecycle(
            [&](uint32_t cycle) { events.push_back("start" + std::to_string(cycle)); },
            [&](uint32_t cycle) { events.push_back("end" + std::to_string(cycle)); });

        *pipeline >> BufferOperation::capture_from(source);
        pipeline->branch_if(
            [](uint32_t cycle) { return cycle == 1; },
            [&](BufferPipeline& branch) {
                branch >> BufferOperation::dispatch_to([&](DataVariant&, uint32_t) { events.emplace_back("branch"); });
            },
            true);
        pipeline->execute_for_cycles(3);
        run(pipeline);

        const auto start = std::ranges::find(events, "start1");
        const auto branch = std::ranges::find(events, "branch");
        const auto end = std::ranges::find(events, "end1");
        ASSERT_NE(branch, events.end());
        EXPECT_LT(start, branch);
        EXPECT_LT(branch, end);
    }

} // namespace
} // namespace MayaFlux::Test
