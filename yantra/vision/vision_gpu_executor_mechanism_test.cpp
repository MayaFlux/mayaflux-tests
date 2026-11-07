#include "gtest/gtest.h"

#include "../../support/vision_test_support.hpp"

namespace MayaFlux::Test::Vision {

using Kinesis::Vision::ComponentResult;
using Kinesis::Vision::TrackResult;
using Kinesis::Vision::VisionOp;
using Kinesis::Vision::VisionParams;
using Kinesis::Vision::VisionSequence;
using Kinesis::Vision::VisionResult;
using Yantra::VisionGpuContexts;
using Yantra::VisionGpuExecutor;

namespace {

constexpr uint32_t k_flow_size = 256;
constexpr float k_dx = 3.0F;
constexpr float k_dy = 2.0F;
constexpr float k_track_tolerance = 0.25F;

VisionSequence tracking_sequence(bool deferred)
{
    auto builder = VisionSequence::Builder {}
                       .rgba_to_gray()
                       .harris_response({ .sigma = 1.5F })
                       .extract_peaks({ .threshold = 0.05F, .nms_radius = 6 })
                       .track_keypoints({ .error_threshold = 0.08F })
                       .optical_flow_dense();
    if (deferred) {
        builder.defer();
    }
    return builder.build();
}

const std::vector<TrackResult>* tracks_of(const VisionResult& result)
{
    return std::get_if<std::vector<TrackResult>>(&result.structured);
}

VisionResult run_until_ready(VisionGpuExecutor& exec, const VisionSequence& sequence,
    const ImageHandle& frame, size_t& polls)
{
    auto result = exec.run(sequence, frame, k_flow_size, k_flow_size);
    while (!result.is_ready() && polls < 100000) {
        result = exec.run(sequence, frame, k_flow_size, k_flow_size);
        ++polls;
    }
    return result;
}

} // namespace

class VisionExecutorLifecycleTest : public VisionGpuFixture { };

TEST_F(VisionExecutorLifecycleTest, ResetWithNothingOutstandingIsSafe)
{
    VisionGpuExecutor exec;
    exec.reset();
    EXPECT_FALSE(exec.is_suspended());

    const auto frame = wave_frame(64, 0.0F, 0.0F);
    ASSERT_NE(frame, nullptr);
    const auto result = exec.run(VisionSequence::Builder {}.rgba_to_gray().build(), frame, 64, 64);
    EXPECT_TRUE(result.is_ready());
    EXPECT_NE(result.gray, nullptr);
    EXPECT_FALSE(exec.is_suspended());
}

TEST_F(VisionExecutorLifecycleTest, ResetDuringSuspensionDropsTheResumePoint)
{
    const auto frame0 = flow_frame(k_flow_size, 0.0F, 0.0F, false);
    const auto frame1 = flow_frame(k_flow_size, k_dx, k_dy, false);
    const auto frame2 = flow_frame(k_flow_size, 2.0F * k_dx, 2.0F * k_dy, false);
    ASSERT_NE(frame0, nullptr);
    ASSERT_NE(frame1, nullptr);
    ASSERT_NE(frame2, nullptr);

    const auto sequence = tracking_sequence(true);
    VisionGpuExecutor exec;
    const auto first = exec.run(sequence, frame0, k_flow_size, k_flow_size);
    ASSERT_TRUE(first.is_ready());

    const auto pending = exec.run(sequence, frame1, k_flow_size, k_flow_size);
    EXPECT_FALSE(pending.is_ready());
    EXPECT_TRUE(exec.is_suspended());

    exec.reset();
    EXPECT_FALSE(exec.is_suspended());

    const auto reseed = exec.run(sequence, frame2, k_flow_size, k_flow_size);
    ASSERT_NE(tracks_of(reseed), nullptr);
    EXPECT_TRUE(tracks_of(reseed)->empty());
    EXPECT_EQ(reseed.flow, nullptr);
}

TEST_F(VisionExecutorLifecycleTest, ResetClearsThePreviousFlowSoTheNextDenseRunHasNoFlow)
{
    const auto frame0 = flow_frame(k_flow_size, 0.0F, 0.0F, false);
    const auto frame1 = flow_frame(k_flow_size, k_dx, k_dy, false);
    ASSERT_NE(frame0, nullptr);
    ASSERT_NE(frame1, nullptr);

    const auto sequence = VisionSequence::Builder {}.rgba_to_gray().optical_flow_dense().build();
    VisionGpuExecutor exec;
    static_cast<void>(exec.run(sequence, frame0, k_flow_size, k_flow_size));
    const auto before = exec.run(sequence, frame1, k_flow_size, k_flow_size);
    ASSERT_NE(before.flow, nullptr);

    exec.reset();
    const auto after = exec.run(sequence, frame1, k_flow_size, k_flow_size);
    EXPECT_EQ(after.flow, nullptr);
}

class VisionExecutorSuspensionTest : public VisionGpuFixture { };

TEST_F(VisionExecutorSuspensionTest, DeferredRunPollsUntilTheWalkCompletes)
{
    const auto frame0 = flow_frame(k_flow_size, 0.0F, 0.0F, false);
    const auto frame1 = flow_frame(k_flow_size, k_dx, k_dy, false);
    ASSERT_NE(frame0, nullptr);
    ASSERT_NE(frame1, nullptr);

    const auto sequence = tracking_sequence(true);
    VisionGpuExecutor exec;
    static_cast<void>(exec.run(sequence, frame0, k_flow_size, k_flow_size));

    size_t polls = 0;
    const auto result = run_until_ready(exec, sequence, frame1, polls);
    ASSERT_TRUE(result.is_ready());
    EXPECT_FALSE(exec.is_suspended());
    ASSERT_NE(result.flow, nullptr);

    const auto* tracks = tracks_of(result);
    ASSERT_NE(tracks, nullptr);
    const auto stats = measure_tracks(*tracks, k_flow_size, k_dx, k_dy, k_track_tolerance);
    EXPECT_GE(stats.tracked, 8U);
    EXPECT_GE(static_cast<float>(stats.within), 0.9F * static_cast<float>(stats.tracked));
}

TEST_F(VisionExecutorSuspensionTest, PollingWhileSuspendedUsesTheFrameTheWalkStartedOn)
{
    const auto frame0 = flow_frame(k_flow_size, 0.0F, 0.0F, false);
    const auto frame1 = flow_frame(k_flow_size, k_dx, k_dy, false);
    const auto unrelated = flow_frame(k_flow_size, 9.0F, -7.0F, false);
    ASSERT_NE(frame0, nullptr);
    ASSERT_NE(frame1, nullptr);
    ASSERT_NE(unrelated, nullptr);

    const auto sequence = tracking_sequence(true);
    VisionGpuExecutor exec;
    static_cast<void>(exec.run(sequence, frame0, k_flow_size, k_flow_size));

    const auto started = exec.run(sequence, frame1, k_flow_size, k_flow_size);
    ASSERT_FALSE(started.is_ready());

    VisionResult polled = started;
    size_t polls = 0;
    while (!polled.is_ready() && polls < 100000) {
        polled = exec.run(sequence, unrelated, k_flow_size, k_flow_size);
        ++polls;
    }
    ASSERT_TRUE(polled.is_ready());

    const auto* tracks = tracks_of(polled);
    ASSERT_NE(tracks, nullptr);
    const auto stats = measure_tracks(*tracks, k_flow_size, k_dx, k_dy, k_track_tolerance);
    EXPECT_GE(stats.tracked, 8U);
    EXPECT_GE(static_cast<float>(stats.within), 0.9F * static_cast<float>(stats.tracked));
}

class VisionExecutorRecordingTest : public VisionGpuFixture { };

TEST_F(VisionExecutorRecordingTest, ConsecutiveRunsOnOneExecutorMatchFreshExecutors)
{
    const auto frame = wave_frame(64, 0.0F, 0.0F);
    const auto warm = wave_frame(64, 5.0F, -3.0F);
    ASSERT_NE(frame, nullptr);
    ASSERT_NE(warm, nullptr);

    const auto sequence = VisionSequence::Builder {}
                              .rgba_to_gray()
                              .gaussian_blur({ .sigma = 1.5F })
                              .sobel()
                              .threshold({ .value = 0.2F })
                              .build();

    VisionGpuExecutor shared;
    static_cast<void>(shared.run(VisionSequence::Builder {}.rgba_to_gray().sobel().build(), warm, 64, 64));
    const auto chained = shared.run(sequence, frame, 64, 64);

    VisionGpuExecutor fresh;
    const auto reference = fresh.run(sequence, frame, 64, 64);

    ASSERT_NE(chained.images.threshold, nullptr);
    ASSERT_NE(reference.images.threshold, nullptr);
    EXPECT_LE(max_abs_diff(read_normalised(chained.images.threshold), read_normalised(reference.images.threshold)),
        k_alias_tolerance);
}

class VisionExecutorRepeatTest : public VisionGpuFixture { };

TEST_F(VisionExecutorRepeatTest, RepeatedFrameHoldsTheLastDistinctFlow)
{
    const auto frame0 = flow_frame(k_flow_size, 0.0F, 0.0F, false);
    const auto frame1 = flow_frame(k_flow_size, k_dx, k_dy, false);
    ASSERT_NE(frame0, nullptr);
    ASSERT_NE(frame1, nullptr);

    const auto sequence = VisionSequence::Builder {}.rgba_to_gray().optical_flow_dense().build();
    VisionGpuExecutor exec;
    static_cast<void>(exec.run(sequence, frame0, k_flow_size, k_flow_size));
    const auto distinct = exec.run(sequence, frame1, k_flow_size, k_flow_size);
    ASSERT_NE(distinct.flow, nullptr);
    const auto distinct_pixels = flow_download(distinct.flow);

    const auto repeat = exec.run(sequence, frame1, k_flow_size, k_flow_size);
    ASSERT_NE(repeat.flow, nullptr);
    EXPECT_LE(max_abs_diff(flow_download(repeat.flow), distinct_pixels), k_alias_tolerance);
}

TEST_F(VisionExecutorRepeatTest, RepeatedFrameRepublishesTheLastDistinctTracks)
{
    const auto frame0 = flow_frame(k_flow_size, 0.0F, 0.0F, false);
    const auto frame1 = flow_frame(k_flow_size, k_dx, k_dy, false);
    ASSERT_NE(frame0, nullptr);
    ASSERT_NE(frame1, nullptr);

    const auto sequence = tracking_sequence(false);
    VisionGpuExecutor exec;
    static_cast<void>(exec.run(sequence, frame0, k_flow_size, k_flow_size));
    const auto distinct = exec.run(sequence, frame1, k_flow_size, k_flow_size);
    const auto* first = tracks_of(distinct);
    ASSERT_NE(first, nullptr);
    ASSERT_FALSE(first->empty());

    const auto repeat = exec.run(sequence, frame1, k_flow_size, k_flow_size);
    const auto* again = tracks_of(repeat);
    ASSERT_NE(again, nullptr);
    EXPECT_EQ(again->size(), first->size());
}

TEST_F(VisionExecutorRepeatTest, MotionEnergyIsReportedForMovingFramesAndZeroForFlatOnes)
{
    const auto sequence = VisionSequence::Builder {}.rgba_to_gray().optical_flow_dense().build();

    VisionGpuExecutor moving;
    float peak = 0.0F;
    for (uint32_t k = 0; k < 4; ++k) {
        const auto frame = flow_frame(k_flow_size, k_dx * static_cast<float>(k), k_dy * static_cast<float>(k), false);
        ASSERT_NE(frame, nullptr);
        peak = std::max(peak, moving.run(sequence, frame, k_flow_size, k_flow_size).motion_energy);
    }
    EXPECT_GT(peak, 1e-4F);

    VisionGpuExecutor flat;
    for (uint32_t k = 0; k < 3; ++k) {
        const auto frame = flow_frame(k_flow_size, 0.0F, 0.0F, true);
        ASSERT_NE(frame, nullptr);
        EXPECT_EQ(flat.run(sequence, frame, k_flow_size, k_flow_size).motion_energy, 0.0F);
    }
}

class VisionExecutorSelectLabelTest : public VisionGpuFixture { };

TEST_F(VisionExecutorSelectLabelTest, SelectLabelKeepsOnlyTheChosenComponentOpaque)
{
    constexpr uint32_t size = 16;
    std::vector<float> pixels(static_cast<size_t>(size) * size * 4, 0.0F);
    for (uint32_t y = 1; y <= 3; ++y) {
        for (uint32_t x = 1; x <= 3; ++x) {
            set_all_channels(pixels, size, x, y, 1.0F);
        }
    }
    for (uint32_t y = 8; y <= 10; ++y) {
        for (uint32_t x = 8; x <= 10; ++x) {
            set_all_channels(pixels, size, x, y, 1.0F);
        }
    }
    set_all_channels(pixels, size, 13, 13, 1.0F);
    const auto blobs = rgba_image(size, size, pixels);
    ASSERT_NE(blobs, nullptr);

    std::vector<float> white(static_cast<size_t>(size) * size * 4, 1.0F);
    const auto source = rgba_image(size, size, white);
    ASSERT_NE(source, nullptr);

    const auto sequence = VisionSequence::Builder {}
                              .connected_components({ .export_labels = true, .with_colors = true })
                              .build();
    VisionGpuContexts contexts;
    VisionGpuExecutor exec;
    const auto result = exec.run(contexts, sequence, blobs, size, size);
    ASSERT_NE(std::get_if<ComponentResult>(&result.structured), nullptr);
    ASSERT_EQ(result.component_boxes.size(), 3U);

    const auto label_at = [&](uint32_t x, uint32_t y) -> uint32_t {
        for (const auto& box : result.component_boxes) {
            const auto b = pixel_box(box, size, size);
            if (x >= b[0] && x <= b[2] && y >= b[1] && y <= b[3]) {
                return box.label_id;
            }
        }
        return 0;
    };
    const uint32_t label = label_at(2, 2);
    ASSERT_NE(label, 0U);
    ASSERT_NE(label_at(9, 9), label);

    const auto selected = exec.select_label(contexts, source, label, size, size);
    ASSERT_NE(selected, nullptr);
    contexts.recording->submit();
    contexts.recording->wait();

    const auto out = read_floats(selected);
    const auto alpha_at = [&](uint32_t x, uint32_t y) {
        return out[(static_cast<size_t>(y) * size + x) * 4 + 3];
    };
    EXPECT_GT(alpha_at(2, 2), 0.5F);
    EXPECT_LT(alpha_at(9, 9), 0.5F);
    EXPECT_LT(alpha_at(13, 13), 0.5F);
    EXPECT_LT(alpha_at(0, 0), 0.5F);
}

class VisionExecutorConfigGpuTest : public VisionGpuFixture { };

TEST_F(VisionExecutorConfigGpuTest, ImplementedOpsReturnACompiledShaderPath)
{
    const VisionParams params {};
    for (const auto op : { VisionOp::Sobel, VisionOp::GaussianBlur, VisionOp::HarrisResponse,
             VisionOp::ConnectedComponents, VisionOp::OpticalFlowDense }) {
        const auto config = VisionGpuExecutor::config(op, params);
        const bool compiled = config.shader_id != Portal::Graphics::INVALID_SHADER || !config.shader_path.empty();
        EXPECT_TRUE(compiled) << static_cast<int>(op);
    }
}

TEST(VisionExecutorConfigTest, OpsWithoutAGpuImplementationReturnAnInvalidShader)
{
    const auto config = VisionGpuExecutor::config(VisionOp::Snapshot, VisionParams {});
    EXPECT_EQ(config.shader_id, Portal::Graphics::INVALID_SHADER);
    EXPECT_TRUE(config.shader_path.empty());
}

TEST(VisionExecutorConfigTest, ConfigIsStatelessAndRepeatable)
{
    const VisionParams params {};
    const auto first = VisionGpuExecutor::config(VisionOp::Sobel, params);
    const auto second = VisionGpuExecutor::config(VisionOp::Sobel, params);
    EXPECT_EQ(first.shader_path, second.shader_path);
    EXPECT_EQ(first.push_constant_size, second.push_constant_size);
}

} // namespace MayaFlux::Test::Vision
