#include "gtest/gtest.h"

#include "../kakshya/video_test_support.hpp"

#include "MayaFlux/Buffers/AudioBuffer.hpp"
#include "MayaFlux/Buffers/BufferManager.hpp"
#include "MayaFlux/Kakshya/Source/DynamicSoundStream.hpp"
#include "MayaFlux/Kakshya/Source/DynamicVideoStream.hpp"
#include "MayaFlux/Kriya/BufferOperation.hpp"

namespace MayaFlux::Test {
namespace {

    using Buffers::AudioBuffer;
    using Buffers::ProcessingToken;
    using Kakshya::DataVariant;
    using Kakshya::DynamicSoundStream;
    using Kakshya::DynamicVideoStream;
    using Kriya::BufferCapture;
    using Kriya::BufferOperation;
    using Kriya::CaptureBuilder;
    using OpType = BufferOperation::OpType;

    constexpr uint32_t k_rate = 48000;

    std::shared_ptr<AudioBuffer> make_source(uint32_t channel = 0)
    {
        return std::make_shared<AudioBuffer>(channel, Buffers::s_preferred_buffer_size);
    }

    void expect_operation(const BufferOperation& op, OpType type, ProcessingToken token, bool capture_phase, bool process_phase)
    {
        EXPECT_EQ(op.get_type(), type);
        EXPECT_EQ(op.get_token(), token);
        EXPECT_EQ(BufferOperation::is_capture_phase_operation(op), capture_phase);
        EXPECT_EQ(BufferOperation::is_process_phase_operation(op), process_phase);
    }

    DataVariant keep(DataVariant& data, uint32_t)
    {
        return data;
    }

    void ignore(DataVariant&, uint32_t) { }

    DataVariant first_source(std::vector<DataVariant>& sources, uint32_t)
    {
        return sources.front();
    }

    TEST(BufferCaptureConfigTest, AnEmptyCaptureIsTransientAutomaticAndOneCycle)
    {
        const BufferCapture capture;

        EXPECT_EQ(capture.get_mode(), BufferCapture::CaptureMode::TRANSIENT);
        EXPECT_EQ(capture.get_processing_control(), BufferCapture::ProcessingControl::AUTOMATIC);
        EXPECT_EQ(capture.get_cycle_count(), 1U);
        EXPECT_EQ(capture.get_circular_size(), 0U);
        EXPECT_EQ(capture.get_window_size(), 0U);
        EXPECT_FLOAT_EQ(capture.get_overlap_ratio(), 0.0F);
        EXPECT_TRUE(capture.get_tag().empty());
        EXPECT_EQ(capture.get_source_buffer(), nullptr);
    }

    TEST(BufferCaptureConfigTest, AnAudioBufferIsTheOnlySource)
    {
        const auto source = make_source();

        const BufferCapture capture(source);

        EXPECT_EQ(capture.get_audio_buffer(), source);
        EXPECT_EQ(capture.get_graphics_buffer(), nullptr);
        EXPECT_EQ(capture.get_source_buffer(), source);
    }

    TEST(BufferCaptureConfigTest, AnExplicitModeAndCycleCountAreKept)
    {
        const BufferCapture capture(make_source(), BufferCapture::CaptureMode::ACCUMULATE, 6);

        EXPECT_EQ(capture.get_mode(), BufferCapture::CaptureMode::ACCUMULATE);
        EXPECT_EQ(capture.get_cycle_count(), 6U);
    }

    TEST(BufferCaptureConfigTest, ForCyclesChoosesTheModeFromTheCount)
    {
        BufferCapture capture(make_source());

        capture.for_cycles(4);
        EXPECT_EQ(capture.get_mode(), BufferCapture::CaptureMode::ACCUMULATE);
        EXPECT_EQ(capture.get_cycle_count(), 4U);

        capture.for_cycles(1);
        EXPECT_EQ(capture.get_mode(), BufferCapture::CaptureMode::TRANSIENT);
        EXPECT_EQ(capture.get_cycle_count(), 1U);
    }

    TEST(BufferCaptureConfigTest, AConditionSelectsTriggeredCapture)
    {
        BufferCapture capture(make_source());

        capture.until_condition([] { return true; });

        EXPECT_EQ(capture.get_mode(), BufferCapture::CaptureMode::TRIGGERED);
    }

    TEST(BufferCaptureConfigTest, AWindowSelectsWindowedCaptureAndKeepsItsShape)
    {
        BufferCapture capture(make_source());

        capture.with_window(1024, 0.25F);

        EXPECT_EQ(capture.get_mode(), BufferCapture::CaptureMode::WINDOWED);
        EXPECT_EQ(capture.get_window_size(), 1024U);
        EXPECT_FLOAT_EQ(capture.get_overlap_ratio(), 0.25F);
    }

    TEST(BufferCaptureConfigTest, AWindowWithoutAnOverlapHasNone)
    {
        BufferCapture capture(make_source());

        capture.with_window(256);

        EXPECT_FLOAT_EQ(capture.get_overlap_ratio(), 0.0F);
    }

    TEST(BufferCaptureConfigTest, ACircularSizeSelectsCircularCapture)
    {
        BufferCapture capture(make_source());

        capture.as_circular(2048);

        EXPECT_EQ(capture.get_mode(), BufferCapture::CaptureMode::CIRCULAR);
        EXPECT_EQ(capture.get_circular_size(), 2048U);
    }

    TEST(BufferCaptureConfigTest, TheLastModeSettingWins)
    {
        BufferCapture capture(make_source());

        capture.for_cycles(4).as_circular(1024);
        EXPECT_EQ(capture.get_mode(), BufferCapture::CaptureMode::CIRCULAR);
        EXPECT_EQ(capture.get_cycle_count(), 4U);

        capture.for_cycles(2);
        EXPECT_EQ(capture.get_mode(), BufferCapture::CaptureMode::ACCUMULATE);
    }

    TEST(BufferCaptureConfigTest, ProcessingControlCanBeSetEitherWay)
    {
        BufferCapture capture(make_source());

        capture.with_processing_control(BufferCapture::ProcessingControl::ON_CAPTURE);
        EXPECT_EQ(capture.get_processing_control(), BufferCapture::ProcessingControl::ON_CAPTURE);

        capture.set_processing_control(BufferCapture::ProcessingControl::MANUAL);
        EXPECT_EQ(capture.get_processing_control(), BufferCapture::ProcessingControl::MANUAL);
    }

    TEST(BufferCaptureConfigTest, TheTagReadsBack)
    {
        BufferCapture capture(make_source());

        capture.with_tag("room").with_metadata("unit", "dB");

        EXPECT_EQ(capture.get_tag(), "room");
    }

    TEST(BufferCaptureConfigTest, EveryOptionReturnsTheSameCapture)
    {
        BufferCapture capture(make_source());

        EXPECT_EQ(&capture.for_cycles(2), &capture);
        EXPECT_EQ(&capture.until_condition([] { return false; }), &capture);
        EXPECT_EQ(&capture.with_window(64), &capture);
        EXPECT_EQ(&capture.as_circular(64), &capture);
        EXPECT_EQ(&capture.on_data_ready(ignore), &capture);
        EXPECT_EQ(&capture.on_cycle_complete([](uint32_t) { }), &capture);
        EXPECT_EQ(&capture.on_data_expired([](const DataVariant&, uint32_t) { }), &capture);
        EXPECT_EQ(&capture.with_tag("t"), &capture);
        EXPECT_EQ(&capture.with_metadata("k", "v"), &capture);
        EXPECT_EQ(&capture.with_processing_control(BufferCapture::ProcessingControl::MANUAL), &capture);
    }

    TEST(CaptureBuilderTest, BecomesACaptureOperationOnTheAudioToken)
    {
        CaptureBuilder builder(make_source());
        builder.for_cycles(3).with_tag("take");

        const BufferOperation op = builder;

        expect_operation(op, OpType::CAPTURE, ProcessingToken::AUDIO_BACKEND, true, false);
        EXPECT_EQ(op.get_tag(), "take");
        EXPECT_EQ(op.get_priority(), 128U);
    }

    TEST(CaptureBuilderTest, TheTagReadsBackBeforeConversion)
    {
        CaptureBuilder builder(make_source());

        EXPECT_TRUE(builder.get_tag().empty());
        builder.with_tag("x");

        EXPECT_EQ(builder.get_tag(), "x");
    }

    TEST(CaptureBuilderTest, EveryOptionReturnsTheSameBuilder)
    {
        CaptureBuilder builder(make_source());

        EXPECT_EQ(&builder.for_cycles(2), &builder);
        EXPECT_EQ(&builder.on_capture_processing(), &builder);
        EXPECT_EQ(&builder.manual_processing(), &builder);
        EXPECT_EQ(&builder.auto_processing(), &builder);
        EXPECT_EQ(&builder.until_condition([] { return false; }), &builder);
        EXPECT_EQ(&builder.as_circular(64), &builder);
        EXPECT_EQ(&builder.with_window(64, 0.5F), &builder);
        EXPECT_EQ(&builder.on_data_ready(ignore), &builder);
        EXPECT_EQ(&builder.on_cycle_complete([](uint32_t) { }), &builder);
        EXPECT_EQ(&builder.on_data_expired([](const DataVariant&, uint32_t) { }), &builder);
        EXPECT_EQ(&builder.with_tag("t"), &builder);
        EXPECT_EQ(&builder.with_metadata("k", "v"), &builder);
    }

    TEST(CaptureBuilderTest, ASourcelessBuilderCannotBecomeAnOperation)
    {
        CaptureBuilder builder(nullptr);

        EXPECT_THROW((void)static_cast<BufferOperation>(builder), std::invalid_argument);
    }

    TEST(CaptureBuilderTest, CaptureFromNullIsSourcelessToo)
    {
        auto builder = BufferOperation::capture_from(nullptr);

        EXPECT_THROW((void)static_cast<BufferOperation>(builder), std::invalid_argument);
    }

    TEST(BufferOperationFactoryTest, ACaptureIsACapturePhaseOperation)
    {
        const auto op = BufferOperation::capture(BufferCapture(make_source()));

        expect_operation(op, OpType::CAPTURE, ProcessingToken::AUDIO_BACKEND, true, false);
    }

    TEST(BufferOperationFactoryTest, CaptureFromBuildsTheSameKindOfOperation)
    {
        auto builder = BufferOperation::capture_from(make_source());

        const BufferOperation op = builder;

        expect_operation(op, OpType::CAPTURE, ProcessingToken::AUDIO_BACKEND, true, false);
    }

    TEST(BufferOperationFactoryTest, ATransformIsAProcessPhaseOperationOnTheAudioTokenByDefault)
    {
        expect_operation(BufferOperation::transform(keep), OpType::TRANSFORM, ProcessingToken::AUDIO_BACKEND, false, true);
    }

    TEST(BufferOperationFactoryTest, ATransformTakesAnotherToken)
    {
        const auto op = BufferOperation::transform(keep, ProcessingToken::GRAPHICS_BACKEND);

        expect_operation(op, OpType::TRANSFORM, ProcessingToken::GRAPHICS_BACKEND, false, true);
    }

    TEST(BufferOperationFactoryTest, RoutingToAnAudioBufferUsesTheAudioToken)
    {
        expect_operation(BufferOperation::route_to_buffer(make_source()), OpType::ROUTE, ProcessingToken::AUDIO_BACKEND, false, true);
    }

    TEST(BufferOperationFactoryTest, RoutingToASoundStreamUsesTheAudioToken)
    {
        const auto stream = std::make_shared<DynamicSoundStream>(k_rate, 2);

        expect_operation(BufferOperation::route_to_container(stream, 1), OpType::ROUTE, ProcessingToken::AUDIO_BACKEND, false, true);
    }

    TEST(BufferOperationFactoryTest, RoutingToAVideoStreamUsesTheGraphicsToken)
    {
        const auto stream = std::make_shared<DynamicVideoStream>(VideoSupport::rgba8_spec(0));

        const auto op = BufferOperation::route_to_container(stream);

        expect_operation(op, OpType::ROUTE, ProcessingToken::GRAPHICS_BACKEND, false, true);
        EXPECT_EQ(op.get_graphics_stream(), stream);
    }

    TEST(BufferOperationFactoryTest, OtherRoutesHaveNoGraphicsStream)
    {
        EXPECT_EQ(BufferOperation::route_to_buffer(make_source()).get_graphics_stream(), nullptr);
        EXPECT_EQ(BufferOperation::transform(keep).get_graphics_stream(), nullptr);
    }

    TEST(BufferOperationFactoryTest, LoadingFromASoundStreamIsAProcessPhaseOperation)
    {
        const auto stream = std::make_shared<DynamicSoundStream>(k_rate, 1);

        const auto op = BufferOperation::load_from_container(stream, make_source(), 4, 16);

        expect_operation(op, OpType::LOAD, ProcessingToken::AUDIO_BACKEND, false, true);
    }

    TEST(BufferOperationFactoryTest, AConditionBelongsToNeitherPhase)
    {
        const auto op = BufferOperation::when([](uint32_t cycle) { return cycle > 0; });

        expect_operation(op, OpType::CONDITION, ProcessingToken::AUDIO_BACKEND, false, false);
    }

    TEST(BufferOperationFactoryTest, ADispatchIsAProcessPhaseOperation)
    {
        expect_operation(BufferOperation::dispatch_to(ignore), OpType::DISPATCH, ProcessingToken::AUDIO_BACKEND, false, true);
    }

    TEST(BufferOperationFactoryTest, FusingBuffersOrStreamsIsAProcessPhaseOperation)
    {
        const auto fused = BufferOperation::fuse_data({ make_source(), make_source() }, first_source, make_source());
        const auto left = std::make_shared<DynamicSoundStream>(k_rate, 1);
        const auto right = std::make_shared<DynamicSoundStream>(k_rate, 1);
        const auto target = std::make_shared<DynamicSoundStream>(k_rate, 1);
        const auto streams = BufferOperation::fuse_containers({ left, right }, first_source, target);

        expect_operation(fused, OpType::FUSE, ProcessingToken::AUDIO_BACKEND, false, true);
        expect_operation(streams, OpType::FUSE, ProcessingToken::AUDIO_BACKEND, false, true);
    }

    TEST(BufferOperationFactoryTest, ModifyingIsAProcessPhaseOperationUntilItStreams)
    {
        auto op = BufferOperation::modify_buffer(make_source(), [](const std::shared_ptr<AudioBuffer>&) { });

        expect_operation(op, OpType::MODIFY, ProcessingToken::AUDIO_BACKEND, false, true);
        EXPECT_FALSE(op.is_streaming());

        op.as_streaming();

        expect_operation(op, OpType::MODIFY, ProcessingToken::AUDIO_BACKEND, true, false);
        EXPECT_TRUE(op.is_streaming());
    }

    TEST(BufferOperationModifierTest, DefaultsAreMidPriorityOnTheBuffersTokenWithNoTag)
    {
        const auto op = BufferOperation::transform(keep);

        EXPECT_EQ(op.get_priority(), 128U);
        EXPECT_EQ(op.get_token(), ProcessingToken::AUDIO_BACKEND);
        EXPECT_TRUE(op.get_tag().empty());
        EXPECT_FALSE(op.is_streaming());
    }

    TEST(BufferOperationModifierTest, PriorityTokenAndTagReadBack)
    {
        auto op = BufferOperation::transform(keep);

        op.with_priority(10).on_token(ProcessingToken::GRAPHICS_BACKEND).with_tag("grade");

        EXPECT_EQ(op.get_priority(), 10U);
        EXPECT_EQ(op.get_token(), ProcessingToken::GRAPHICS_BACKEND);
        EXPECT_EQ(op.get_tag(), "grade");
    }

    TEST(BufferOperationModifierTest, EveryModifierReturnsTheSameOperation)
    {
        auto op = BufferOperation::transform(keep);

        EXPECT_EQ(&op.with_priority(1), &op);
        EXPECT_EQ(&op.on_token(ProcessingToken::AUDIO_BACKEND), &op);
        EXPECT_EQ(&op.every_n_cycles(2), &op);
        EXPECT_EQ(&op.with_tag("t"), &op);
        EXPECT_EQ(&op.for_cycles(2), &op);
        EXPECT_EQ(&op.as_capture_phase(), &op);
        EXPECT_EQ(&op.as_process_phase(), &op);
        EXPECT_EQ(&op.as_streaming(), &op);
    }

    TEST(BufferOperationModifierTest, ForcingAPhaseOverridesTheTypesDefault)
    {
        auto transform = BufferOperation::transform(keep);
        transform.as_capture_phase();
        auto capture = BufferOperation::capture(BufferCapture(make_source()));
        capture.as_process_phase();

        EXPECT_TRUE(BufferOperation::is_capture_phase_operation(transform));
        EXPECT_FALSE(BufferOperation::is_process_phase_operation(transform));
        EXPECT_FALSE(BufferOperation::is_capture_phase_operation(capture));
        EXPECT_TRUE(BufferOperation::is_process_phase_operation(capture));
    }

    TEST(BufferOperationModifierTest, ACaptureTagFlowsIntoTheOperation)
    {
        BufferCapture capture(make_source());
        capture.with_tag("from-capture");

        const auto op = BufferOperation::capture(std::move(capture));

        EXPECT_EQ(op.get_tag(), "from-capture");
    }

    TEST(BufferOperationInputTest, CapturingAnInputChannelRegistersABufferOnThatChannel)
    {
        const auto manager = std::make_shared<Buffers::BufferManager>();

        const auto op = BufferOperation::capture_input(manager, 0);

        expect_operation(op, OpType::CAPTURE, ProcessingToken::AUDIO_BACKEND, true, false);
        const auto& buffers = manager->get_buffers(ProcessingToken::AUDIO_BACKEND, 0);
        EXPECT_TRUE(std::ranges::any_of(buffers, [](const auto& buffer) { return buffer->get_channel_id() == 0; }));
    }

    TEST(BufferOperationInputTest, TheBuilderFormRegistersABufferToo)
    {
        const auto manager = std::make_shared<Buffers::BufferManager>();

        auto builder = BufferOperation::capture_input_from(manager, 1);

        const BufferOperation op = builder;
        expect_operation(op, OpType::CAPTURE, ProcessingToken::AUDIO_BACKEND, true, false);
        const auto& buffers = manager->get_buffers(ProcessingToken::AUDIO_BACKEND, 1);
        EXPECT_TRUE(std::ranges::any_of(buffers, [](const auto& buffer) { return buffer->get_channel_id() == 1; }));
    }

    TEST(BufferOperationInputTest, EachCaptureGetsItsOwnBuffer)
    {
        const auto manager = std::make_shared<Buffers::BufferManager>();

        (void)BufferOperation::capture_input(manager, 0);
        (void)BufferOperation::capture_input(manager, 0);

        EXPECT_GE(manager->get_buffers(ProcessingToken::AUDIO_BACKEND, 0).size(), 2U);
    }

} // namespace
} // namespace MayaFlux::Test
