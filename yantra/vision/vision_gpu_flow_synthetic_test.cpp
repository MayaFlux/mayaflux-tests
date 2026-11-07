#include "gtest/gtest.h"

#include "../../support/vision_test_support.hpp"

namespace MayaFlux::Test::Vision {

using Kinesis::Vision::BoundingBox;
using Kinesis::Vision::TrackResult;
using Kinesis::Vision::VisionResult;
using Kinesis::Vision::VisionSequence;
using Yantra::VisionGpuExecutor;

namespace {

const std::vector<TrackResult>* tracks_of(const VisionResult& result)
{
    return std::get_if<std::vector<TrackResult>>(&result.structured);
}

VisionResult run_to_ready(VisionGpuExecutor& exec, const VisionSequence& sequence,
    const ImageHandle& frame, uint32_t size, size_t& polls)
{
    auto result = exec.run(sequence, frame, size, size);
    while (!result.is_ready() && polls < 100000) {
        result = exec.run(sequence, frame, size, size);
        ++polls;
    }
    return result;
}

bool dense_pass(const DenseStats& dense)
{
    return dense.interior > 0
        && static_cast<float>(dense.confident) >= 0.2F * static_cast<float>(dense.interior)
        && static_cast<float>(dense.within) >= 0.9F * static_cast<float>(dense.confident);
}

bool sparse_pass(const TrackStats& sparse)
{
    return sparse.tracked >= 8
        && static_cast<float>(sparse.within) >= 0.9F * static_cast<float>(sparse.tracked);
}

} // namespace

struct SparseFlowCase {
    const char* name;
    uint32_t size;
    float dx;
    float dy;
    uint32_t levels;
    float tolerance;
    bool blank;
    bool deferred;
    float forward_backward_threshold;
};

class VisionGpuSparseFlowTest : public VisionGpuFixture, public ::testing::WithParamInterface<SparseFlowCase> { };

TEST_P(VisionGpuSparseFlowTest, TracksRecoverTheKnownShiftOnTheSecondFrame)
{
    const auto c = GetParam();

    auto builder = VisionSequence::Builder {}
                       .rgba_to_gray()
                       .harris_response({ .sigma = 1.5F })
                       .extract_peaks({ .threshold = 0.05F, .nms_radius = 6 })
                       .track_keypoints({ .error_threshold = 0.08F, .levels = c.levels, .forward_backward_threshold = c.forward_backward_threshold });
    if (c.deferred) {
        builder.defer();
    }
    const auto sequence = builder.build();

    const auto frame0 = flow_frame(c.size, 0.0F, 0.0F, false);
    const auto frame1 = flow_frame(c.size, c.dx, c.dy, c.blank);
    ASSERT_NE(frame0, nullptr);
    ASSERT_NE(frame1, nullptr);

    VisionGpuExecutor exec;
    size_t polls = 0;
    const auto first = run_to_ready(exec, sequence, frame0, c.size, polls);
    ASSERT_NE(tracks_of(first), nullptr);
    EXPECT_TRUE(tracks_of(first)->empty());

    const auto second = run_to_ready(exec, sequence, frame1, c.size, polls);
    ASSERT_TRUE(second.is_ready());
    const auto* tracks = tracks_of(second);
    ASSERT_NE(tracks, nullptr);

    const auto stats = measure_tracks(*tracks, c.size, c.dx, c.dy, c.tolerance);
    if (c.blank) {
        EXPECT_EQ(stats.tracked_any, 0U);
    } else {
        EXPECT_TRUE(sparse_pass(stats))
            << "tracked=" << stats.tracked << " within=" << stats.within << " mean_err=" << stats.mean_error;
    }
}

INSTANTIATE_TEST_SUITE_P(
    SparseFlowCases,
    VisionGpuSparseFlowTest,
    ::testing::Values(
        SparseFlowCase { "zero_shift", 256, 0.0F, 0.0F, 4, 0.1F, false, false, 0.0F },
        SparseFlowCase { "integer_shift", 256, 3.0F, 2.0F, 4, 0.25F, false, false, 0.0F },
        SparseFlowCase { "subpixel_shift", 256, 1.37F, -0.62F, 4, 0.25F, false, false, 0.0F },
        SparseFlowCase { "large_shift_four_levels", 256, 14.0F, -9.0F, 4, 0.5F, false, false, 0.0F },
        SparseFlowCase { "textureless_next_frame", 256, 0.0F, 0.0F, 4, 0.5F, true, false, 0.0F },
        SparseFlowCase { "deferred_large_shift", 256, 14.0F, -9.0F, 4, 0.5F, false, true, 0.0F },
        SparseFlowCase { "forward_backward_integer_shift", 256, 3.0F, 2.0F, 4, 0.25F, false, false, 0.5F },
        SparseFlowCase { "forward_backward_large_shift", 256, 14.0F, -9.0F, 4, 0.5F, false, false, 0.5F },
        SparseFlowCase { "forward_backward_deferred_shift", 256, 3.0F, 2.0F, 4, 0.25F, false, true, 0.5F }),
    [](const ::testing::TestParamInfo<SparseFlowCase>& info) { return std::string(info.param.name); });

struct PersistenceCase {
    const char* name;
    uint32_t size;
    float dx;
    float dy;
    uint32_t frames;
    float tolerance;
    float forward_backward_threshold;
    uint32_t cap;
    uint32_t min_points;
    bool host_tracks;
};

class VisionGpuPersistenceTest : public VisionGpuFixture, public ::testing::WithParamInterface<PersistenceCase> { };

TEST_P(VisionGpuPersistenceTest, TracksPersistAcrossFramesWithUniqueIdsAndTheCap)
{
    const auto c = GetParam();
    const auto sequence = VisionSequence::Builder {}
                              .rgba_to_gray()
                              .harris_response()
                              .extract_peaks({ .threshold = 0.05F, .nms_radius = 6 })
                              .track_keypoints({ .error_threshold = 0.08F, .max_points = c.cap, .forward_backward_threshold = c.forward_backward_threshold, .export_tracks = !c.host_tracks, .host_tracks = c.host_tracks })
                              .build();

    VisionGpuExecutor exec;
    std::vector<TrackResult> last;
    bool structured_empty = true;
    for (uint32_t k = 0; k < c.frames; ++k) {
        const auto frame = flow_frame(c.size, c.dx * static_cast<float>(k), c.dy * static_cast<float>(k), false);
        ASSERT_NE(frame, nullptr);
        const auto result = exec.run(sequence, frame, c.size, c.size);
        const auto* tracks = tracks_of(result);
        if (c.host_tracks) {
            if (tracks) {
                last = *tracks;
            }
        } else {
            if (tracks && !tracks->empty()) {
                structured_empty = false;
            }
            last = VisionGpuExecutor::read_exported_tracks(result);
        }
    }

    const auto stats = measure_tracks(last, c.size, c.dx, c.dy, c.tolerance);
    size_t old_tracks = 0;
    std::vector<uint32_t> ids;
    ids.reserve(last.size());
    for (const auto& t : last) {
        ids.push_back(t.id);
        if (t.tracked && t.age >= c.frames / 2) {
            ++old_tracks;
        }
    }
    std::ranges::sort(ids);
    const bool unique_ids = std::ranges::adjacent_find(ids) == ids.end();

    EXPECT_TRUE(sparse_pass(stats));
    EXPECT_GE(static_cast<float>(old_tracks), 0.5F * static_cast<float>(stats.tracked));
    EXPECT_TRUE(unique_ids);
    EXPECT_LE(last.size(), c.cap);
    EXPECT_GE(last.size(), c.min_points);
    EXPECT_TRUE(structured_empty);
}

INSTANTIATE_TEST_SUITE_P(
    PersistenceCases,
    VisionGpuPersistenceTest,
    ::testing::Values(
        PersistenceCase { "shift_two_one_per_frame", 256, 2.0F, 1.0F, 8, 0.25F, 0.0F, 256, 0, true },
        PersistenceCase { "forward_backward", 256, 2.0F, 1.0F, 8, 0.25F, 0.5F, 256, 0, true },
        PersistenceCase { "above_one_workgroup", 512, 2.0F, 1.0F, 8, 0.25F, 0.0F, 1024, 257, true },
        PersistenceCase { "host_tracks_off_export_only", 256, 2.0F, 1.0F, 8, 0.25F, 0.0F, 256, 0, false }),
    [](const ::testing::TestParamInfo<PersistenceCase>& info) { return std::string(info.param.name); });

class VisionGpuFlowSequenceTest : public VisionGpuFixture { };

TEST_F(VisionGpuFlowSequenceTest, SparseAndDenseTrackTheSameShiftFromOneChainedSequence)
{
    constexpr uint32_t size = 256;
    constexpr float dx = 3.0F;
    constexpr float dy = 2.0F;
    constexpr float tolerance = 0.25F;

    const auto sequence = VisionSequence::Builder {}
                              .rgba_to_gray()
                              .harris_response({ .sigma = 1.5F })
                              .extract_peaks({ .threshold = 0.05F, .nms_radius = 6 })
                              .track_keypoints({ .error_threshold = 0.08F })
                              .optical_flow_dense()
                              .build();
    const auto frame0 = flow_frame(size, 0.0F, 0.0F, false);
    const auto frame1 = flow_frame(size, dx, dy, false);
    const auto frame2 = flow_frame(size, 2.0F * dx, 2.0F * dy, false);
    ASSERT_NE(frame0, nullptr);
    ASSERT_NE(frame1, nullptr);
    ASSERT_NE(frame2, nullptr);

    VisionGpuExecutor exec;
    const auto first = exec.run(sequence, frame0, size, size);
    ASSERT_NE(tracks_of(first), nullptr);
    EXPECT_TRUE(tracks_of(first)->empty());
    EXPECT_EQ(first.flow, nullptr);

    const auto check = [&](const VisionResult& result) {
        const auto* tracks = tracks_of(result);
        ASSERT_NE(tracks, nullptr);
        ASSERT_NE(result.flow, nullptr);
        const auto sparse = measure_tracks(*tracks, size, dx, dy, tolerance);
        const auto dense = measure_dense(flow_download(result.flow), size, dx, dy, tolerance, flow_dense_margin(5, 4));
        EXPECT_TRUE(sparse_pass(sparse));
        EXPECT_TRUE(dense_pass(dense));
        EXPECT_GE(static_cast<float>(dense.within), 0.9F * static_cast<float>(dense.confident));
    };

    const auto second = exec.run(sequence, frame1, size, size);
    check(second);
    const auto third = exec.run(sequence, frame2, size, size);
    check(third);
    ASSERT_NE(second.flow, nullptr);
    ASSERT_NE(third.flow, nullptr);
    EXPECT_NE(second.flow, third.flow);
}

TEST_F(VisionGpuFlowSequenceTest, ChangingFrameSizeReseedsBothSolversAndRestoresCleanly)
{
    constexpr float dx = 3.0F;
    constexpr float dy = 2.0F;
    constexpr float tolerance = 0.25F;

    const auto sequence = VisionSequence::Builder {}
                              .rgba_to_gray()
                              .harris_response({ .sigma = 1.5F })
                              .extract_peaks({ .threshold = 0.05F, .nms_radius = 6 })
                              .track_keypoints({ .error_threshold = 0.08F })
                              .optical_flow_dense()
                              .build();

    VisionGpuExecutor exec;
    const auto check_size = [&](uint32_t size) {
        const auto frame0 = flow_frame(size, 0.0F, 0.0F, false);
        const auto frame1 = flow_frame(size, dx, dy, false);
        ASSERT_NE(frame0, nullptr);
        ASSERT_NE(frame1, nullptr);

        const auto first = exec.run(sequence, frame0, size, size);
        ASSERT_NE(tracks_of(first), nullptr);
        EXPECT_TRUE(tracks_of(first)->empty());
        EXPECT_EQ(first.flow, nullptr);

        const auto second = exec.run(sequence, frame1, size, size);
        ASSERT_NE(tracks_of(second), nullptr);
        ASSERT_NE(second.flow, nullptr);
        EXPECT_EQ(second.flow->get_width(), size);
        EXPECT_EQ(second.flow->get_height(), size);

        const auto sparse = measure_tracks(*tracks_of(second), size, dx, dy, tolerance);
        const auto dense = measure_dense(flow_download(second.flow), size, dx, dy, tolerance, flow_dense_margin(5, 4));
        EXPECT_TRUE(sparse_pass(sparse));
        EXPECT_TRUE(dense_pass(dense));
        EXPECT_GE(static_cast<float>(dense.within), 0.9F * static_cast<float>(dense.confident));
    };

    check_size(256);
    check_size(320);
    check_size(256);
}

TEST_F(VisionGpuFlowSequenceTest, ResetWhileSuspendedThenAFreshPairRecoversTheShift)
{
    constexpr uint32_t size = 256;
    constexpr float dx = 3.0F;
    constexpr float dy = 2.0F;
    constexpr float tolerance = 0.25F;

    const auto sequence = VisionSequence::Builder {}
                              .rgba_to_gray()
                              .harris_response({ .sigma = 1.5F })
                              .extract_peaks({ .threshold = 0.05F, .nms_radius = 6 })
                              .track_keypoints({ .error_threshold = 0.08F })
                              .optical_flow_dense()
                              .defer()
                              .build();
    const auto frame0 = flow_frame(size, 0.0F, 0.0F, false);
    const auto frame1 = flow_frame(size, dx, dy, false);
    const auto frame2 = flow_frame(size, 2.0F * dx, 2.0F * dy, false);
    const auto frame3 = flow_frame(size, 3.0F * dx, 3.0F * dy, false);
    ASSERT_NE(frame0, nullptr);
    ASSERT_NE(frame1, nullptr);
    ASSERT_NE(frame2, nullptr);
    ASSERT_NE(frame3, nullptr);

    VisionGpuExecutor exec;
    const auto first = exec.run(sequence, frame0, size, size);
    ASSERT_NE(tracks_of(first), nullptr);
    EXPECT_TRUE(tracks_of(first)->empty());
    EXPECT_EQ(first.flow, nullptr);

    const auto pending = exec.run(sequence, frame1, size, size);
    EXPECT_FALSE(pending.is_ready());

    exec.reset();
    const auto reseed = exec.run(sequence, frame2, size, size);
    ASSERT_NE(tracks_of(reseed), nullptr);
    EXPECT_TRUE(tracks_of(reseed)->empty());
    EXPECT_EQ(reseed.flow, nullptr);

    size_t polls = 0;
    const auto recovered = run_to_ready(exec, sequence, frame3, size, polls);
    ASSERT_TRUE(recovered.is_ready());
    ASSERT_NE(tracks_of(recovered), nullptr);
    ASSERT_NE(recovered.flow, nullptr);

    const auto sparse = measure_tracks(*tracks_of(recovered), size, dx, dy, tolerance);
    const auto dense = measure_dense(flow_download(recovered.flow), size, dx, dy, tolerance, flow_dense_margin(5, 4));
    EXPECT_TRUE(sparse_pass(sparse));
    EXPECT_TRUE(dense_pass(dense));
    EXPECT_GE(static_cast<float>(dense.within), 0.9F * static_cast<float>(dense.confident));
}

struct DegenerateCase {
    const char* name;
    uint32_t size;
    uint32_t radius;
    uint32_t iterations;
    uint32_t levels;
};

class VisionGpuDegenerateFlowTest : public VisionGpuFixture, public ::testing::WithParamInterface<DegenerateCase> { };

TEST_P(VisionGpuDegenerateFlowTest, ClampedParametersAndSmallFramesStillProduceAFiniteField)
{
    const auto c = GetParam();
    const auto sequence = VisionSequence::Builder {}
                              .rgba_to_gray()
                              .optical_flow_dense({ .window_radius = c.radius, .iterations = c.iterations, .levels = c.levels })
                              .build();
    const auto frame0 = flow_frame(c.size, 0.0F, 0.0F, false);
    const auto frame1 = flow_frame(c.size, 1.0F, 0.0F, false);
    ASSERT_NE(frame0, nullptr);
    ASSERT_NE(frame1, nullptr);

    VisionGpuExecutor exec;
    const auto first = exec.run(sequence, frame0, c.size, c.size);
    EXPECT_EQ(first.flow, nullptr);

    const auto second = exec.run(sequence, frame1, c.size, c.size);
    ASSERT_NE(second.flow, nullptr);
    EXPECT_EQ(second.flow->get_width(), c.size);
    EXPECT_EQ(second.flow->get_height(), c.size);
    const auto pixels = flow_download(second.flow);
    EXPECT_TRUE(std::ranges::all_of(pixels, [](float v) { return std::isfinite(v); }));
}

INSTANTIATE_TEST_SUITE_P(
    DegenerateCases,
    VisionGpuDegenerateFlowTest,
    ::testing::Values(
        DegenerateCase { "zero_parameters", 64, 0, 0, 0 },
        DegenerateCase { "oversized_parameters", 64, 99, 0, 99 },
        DegenerateCase { "small_image", 24, 5, 3, 4 }),
    [](const ::testing::TestParamInfo<DegenerateCase>& info) { return std::string(info.param.name); });

class VisionGpuShearFlowTest : public VisionGpuFixture { };

TEST_F(VisionGpuShearFlowTest, SpatiallyVaryingHorizontalMotionIsRecoveredInBothOutputs)
{
    constexpr uint32_t size = 256;
    constexpr float shear = 0.02F;
    constexpr float tolerance = 0.5F;
    const float center = 0.5F * static_cast<float>(size - 1U);

    const auto sequence = VisionSequence::Builder {}
                              .rgba_to_gray()
                              .harris_response({ .sigma = 1.5F })
                              .extract_peaks({ .threshold = 0.05F, .nms_radius = 6 })
                              .track_keypoints({ .error_threshold = 0.08F })
                              .optical_flow_dense()
                              .build();
    const auto frame0 = flow_frame(size, 0.0F, 0.0F, false);
    const auto frame1 = flow_warp_frame(size, 0.0F, 0.0F, shear, false);
    ASSERT_NE(frame0, nullptr);
    ASSERT_NE(frame1, nullptr);

    VisionGpuExecutor exec;
    const auto first = exec.run(sequence, frame0, size, size);
    ASSERT_NE(tracks_of(first), nullptr);
    EXPECT_TRUE(tracks_of(first)->empty());
    EXPECT_EQ(first.flow, nullptr);

    const auto second = exec.run(sequence, frame1, size, size);
    ASSERT_NE(tracks_of(second), nullptr);
    ASSERT_NE(second.flow, nullptr);

    size_t sparse_tracked = 0;
    size_t sparse_within = 0;
    for (const auto& track : *tracks_of(second)) {
        const glm::vec2 source = track.previous * static_cast<float>(size);
        const glm::vec2 expected(shear * (source.y - center), 0.0F);
        const glm::vec2 target = source + expected;
        if (!track.tracked || source.x < 8.0F || source.y < 8.0F
            || target.x < 8.0F || target.y < 8.0F
            || source.x >= size - 8.0F || source.y >= size - 8.0F
            || target.x >= size - 8.0F || target.y >= size - 8.0F) {
            continue;
        }
        ++sparse_tracked;
        const glm::vec2 actual = (track.position - track.previous) * static_cast<float>(size);
        if (glm::length(actual - expected) <= tolerance) {
            ++sparse_within;
        }
    }
    EXPECT_GE(sparse_tracked, 8U);
    EXPECT_GE(static_cast<float>(sparse_within), 0.9F * static_cast<float>(sparse_tracked));

    const auto pixels = flow_download(second.flow);
    const float margin = flow_dense_margin(5, 4);
    size_t interior = 0;
    size_t confident = 0;
    size_t dense_within = 0;
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            const float fx = static_cast<float>(x);
            const float fy = static_cast<float>(y);
            const float expected_x = shear * (fy - center);
            if (fx < margin || fy < margin || fx + expected_x < margin
                || fx >= size - margin || fy >= size - margin || fx + expected_x >= size - margin) {
                continue;
            }
            ++interior;
            const size_t i = (static_cast<size_t>(y) * size + x) * 4;
            if (pixels[i + 2] < 1e-3F) {
                continue;
            }
            ++confident;
            if (glm::length(glm::vec2(pixels[i], pixels[i + 1]) - glm::vec2(expected_x, 0.0F)) <= tolerance) {
                ++dense_within;
            }
        }
    }
    EXPECT_GT(interior, 0U);
    EXPECT_GE(static_cast<float>(confident), 0.2F * static_cast<float>(interior));
    EXPECT_GE(static_cast<float>(dense_within), 0.9F * static_cast<float>(confident));
}

class VisionGpuOcclusionFlowTest : public VisionGpuFixture, public ::testing::WithParamInterface<float> { };

TEST_P(VisionGpuOcclusionFlowTest, SparseRejectsTheOccluderAndDenseRecoversTheExterior)
{
    constexpr uint32_t size = 256;
    constexpr float dx = 3.0F;
    constexpr float dy = 2.0F;
    constexpr float tolerance = 0.5F;

    const auto sequence = VisionSequence::Builder {}
                              .rgba_to_gray()
                              .harris_response({ .sigma = 1.5F })
                              .extract_peaks({ .threshold = 0.05F, .nms_radius = 6 })
                              .track_keypoints({ .error_threshold = 0.08F, .forward_backward_threshold = GetParam() })
                              .optical_flow_dense()
                              .build();
    const auto frame0 = flow_frame(size, 0.0F, 0.0F, false);
    const auto frame1 = flow_warp_frame(size, dx, dy, 0.0F, true);
    ASSERT_NE(frame0, nullptr);
    ASSERT_NE(frame1, nullptr);

    VisionGpuExecutor exec;
    const auto first = exec.run(sequence, frame0, size, size);
    ASSERT_NE(tracks_of(first), nullptr);
    EXPECT_TRUE(tracks_of(first)->empty());
    EXPECT_EQ(first.flow, nullptr);

    const auto second = exec.run(sequence, frame1, size, size);
    ASSERT_NE(tracks_of(second), nullptr);
    ASSERT_NE(second.flow, nullptr);

    size_t occluded = 0;
    size_t rejected = 0;
    size_t exterior_tracked = 0;
    size_t exterior_within = 0;
    for (const auto& track : *tracks_of(second)) {
        const glm::vec2 source = track.previous * static_cast<float>(size);
        const glm::vec2 target = source + glm::vec2(dx, dy);
        if (source.x < 8.0F || source.y < 8.0F
            || target.x < 8.0F || target.y < 8.0F
            || source.x >= size - 8.0F || source.y >= size - 8.0F
            || target.x >= size - 8.0F || target.y >= size - 8.0F) {
            continue;
        }
        if (target.x >= 88.0F && target.x < 168.0F && target.y >= 88.0F && target.y < 168.0F) {
            ++occluded;
            if (!track.tracked) {
                ++rejected;
            }
        } else if (target.x < 64.0F || target.x >= 192.0F || target.y < 64.0F || target.y >= 192.0F) {
            if (!track.tracked) {
                continue;
            }
            ++exterior_tracked;
            const glm::vec2 actual = (track.position - track.previous) * static_cast<float>(size);
            if (glm::length(actual - glm::vec2(dx, dy)) <= tolerance) {
                ++exterior_within;
            }
        }
    }
    EXPECT_GE(occluded, 4U);
    EXPECT_GE(static_cast<float>(rejected), 0.5F * static_cast<float>(occluded));
    EXPECT_GE(exterior_tracked, 8U);
    EXPECT_GE(static_cast<float>(exterior_within), 0.9F * static_cast<float>(exterior_tracked));

    const auto pixels = flow_download(second.flow);
    const float margin = flow_dense_margin(5, 4);
    size_t exterior_interior = 0;
    size_t dense_confident = 0;
    size_t dense_within = 0;
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            const float fx = static_cast<float>(x);
            const float fy = static_cast<float>(y);
            if (fx < margin || fy < margin || fx + dx >= size - margin || fy + dy >= size - margin
                || (fx + dx >= 64.0F && fx + dx < 192.0F && fy + dy >= 64.0F && fy + dy < 192.0F)) {
                continue;
            }
            ++exterior_interior;
            const size_t i = (static_cast<size_t>(y) * size + x) * 4;
            if (pixels[i + 2] < 1e-3F) {
                continue;
            }
            ++dense_confident;
            if (glm::length(glm::vec2(pixels[i], pixels[i + 1]) - glm::vec2(dx, dy)) <= tolerance) {
                ++dense_within;
            }
        }
    }
    EXPECT_GT(exterior_interior, 0U);
    EXPECT_GE(static_cast<float>(dense_confident), 0.2F * static_cast<float>(exterior_interior));
    EXPECT_GE(static_cast<float>(dense_within), 0.9F * static_cast<float>(dense_confident));
}

INSTANTIATE_TEST_SUITE_P(
    ForwardBackwardThresholds,
    VisionGpuOcclusionFlowTest,
    ::testing::Values(0.0F, 0.5F),
    [](const ::testing::TestParamInfo<float>& info) { return info.param == 0.0F ? std::string("default") : std::string("fb_half"); });

struct DenseFlowCase {
    const char* name;
    uint32_t size;
    float dx;
    float dy;
    uint32_t levels;
    float tolerance;
    bool flat;
};

class VisionGpuDenseFlowTest : public VisionGpuFixture, public ::testing::WithParamInterface<DenseFlowCase> { };

TEST_P(VisionGpuDenseFlowTest, DenseFieldRecoversTheShiftOnTheSecondFrame)
{
    const auto c = GetParam();
    const auto sequence = VisionSequence::Builder {}
                              .rgba_to_gray()
                              .optical_flow_dense({ .levels = c.levels })
                              .build();
    const auto frame0 = flow_frame(c.size, 0.0F, 0.0F, c.flat);
    const auto frame1 = flow_frame(c.size, c.dx, c.dy, c.flat);
    ASSERT_NE(frame0, nullptr);
    ASSERT_NE(frame1, nullptr);

    VisionGpuExecutor exec;
    const auto first = exec.run(sequence, frame0, c.size, c.size);
    EXPECT_EQ(first.flow, nullptr);

    const auto second = exec.run(sequence, frame1, c.size, c.size);
    ASSERT_NE(second.flow, nullptr);
    const auto stats = measure_dense(flow_download(second.flow), c.size, c.dx, c.dy, c.tolerance, flow_dense_margin(5, c.levels));
    if (c.flat) {
        EXPECT_EQ(stats.confident, 0U);
        EXPECT_LT(stats.max_flow, 1e-3F);
    } else {
        EXPECT_TRUE(dense_pass(stats))
            << "interior=" << stats.interior << " confident=" << stats.confident << " within=" << stats.within;
    }
}

INSTANTIATE_TEST_SUITE_P(
    DenseFlowCases,
    VisionGpuDenseFlowTest,
    ::testing::Values(
        DenseFlowCase { "zero_shift", 256, 0.0F, 0.0F, 4, 0.1F, false },
        DenseFlowCase { "integer_shift", 256, 3.0F, 2.0F, 4, 0.25F, false },
        DenseFlowCase { "subpixel_shift", 256, 1.37F, -0.62F, 4, 0.25F, false },
        DenseFlowCase { "large_shift_four_levels", 256, 14.0F, -9.0F, 4, 0.5F, false },
        DenseFlowCase { "featureless_frames", 256, 0.0F, 0.0F, 4, 0.5F, true }),
    [](const ::testing::TestParamInfo<DenseFlowCase>& info) { return std::string(info.param.name); });

class VisionGpuDenseFlowParityTest : public VisionGpuFixture { };

TEST_F(VisionGpuDenseFlowParityTest, ResultImageAlternatesBetweenTwoBuffers)
{
    constexpr uint32_t size = 256;
    const auto sequence = VisionSequence::Builder {}.rgba_to_gray().optical_flow_dense().build();
    VisionGpuExecutor exec;
    std::vector<ImageHandle> flows;
    for (uint32_t k = 0; k < 4; ++k) {
        const auto frame = flow_frame(size, 2.0F * static_cast<float>(k), static_cast<float>(k), false);
        ASSERT_NE(frame, nullptr);
        flows.push_back(exec.run(sequence, frame, size, size).flow);
    }
    EXPECT_EQ(flows[0], nullptr);
    ASSERT_NE(flows[1], nullptr);
    ASSERT_NE(flows[2], nullptr);
    ASSERT_NE(flows[3], nullptr);
    EXPECT_NE(flows[1], flows[2]);
    EXPECT_EQ(flows[1], flows[3]);
}

class VisionGpuFlowHsvGateTest : public VisionGpuFixture { };

TEST_F(VisionGpuFlowHsvGateTest, HsvInputProducesTheSameMeanFlowAsGrayInTheRegion)
{
    constexpr uint32_t size = 256;
    constexpr float dx = 2.0F;
    constexpr float dy = 1.0F;
    const BoundingBox region { .x = 0.1F, .y = 0.1F, .w = 0.8F, .h = 0.8F };

    const auto frame0 = wave_frame(size, 0.0F, 0.0F);
    const auto frame1 = wave_frame(size, dx, dy);
    ASSERT_NE(frame0, nullptr);
    ASSERT_NE(frame1, nullptr);

    const auto gray_sequence = VisionSequence::Builder {}.rgba_to_gray().optical_flow_dense().build();
    const auto hsv_sequence = VisionSequence::Builder {}.rgba_to_hsv().optical_flow_dense().build();

    const auto mean_flow = [&](const VisionSequence& sequence) -> std::optional<glm::vec2> {
        VisionGpuExecutor executor;
        static_cast<void>(executor.run(sequence, frame0, size, size));
        const auto result = executor.run(sequence, frame1, size, size);
        if (!result.flow) {
            return std::nullopt;
        }
        const auto pixels = flow_download(result.flow);
        const auto x0 = static_cast<uint32_t>(region.x * static_cast<float>(size));
        const auto y0 = static_cast<uint32_t>(region.y * static_cast<float>(size));
        const auto x1 = static_cast<uint32_t>((region.x + region.w) * static_cast<float>(size));
        const auto y1 = static_cast<uint32_t>((region.y + region.h) * static_cast<float>(size));
        glm::vec2 sum { 0.0F, 0.0F };
        size_t count = 0;
        for (uint32_t y = y0; y < y1; ++y) {
            for (uint32_t x = x0; x < x1; ++x) {
                const size_t i = (static_cast<size_t>(y) * size + x) * 4;
                sum += glm::vec2(pixels[i], pixels[i + 1]);
                ++count;
            }
        }
        return sum / static_cast<float>(count);
    };

    const auto gray = mean_flow(gray_sequence);
    const auto hsv = mean_flow(hsv_sequence);
    ASSERT_TRUE(gray.has_value());
    ASSERT_TRUE(hsv.has_value());

    const float gray_mag = glm::length(*gray);
    const float hsv_mag = glm::length(*hsv);
    EXPECT_GT(gray_mag, 1e-4F);
    EXPECT_GE(hsv_mag, 0.5F * gray_mag);
    EXPECT_LE(glm::length(*gray - *hsv), 0.1F * gray_mag);
}

} // namespace MayaFlux::Test::Vision
