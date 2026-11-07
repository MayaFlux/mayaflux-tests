#include "gtest/gtest.h"

#include <functional>
#include <vector>

#include "MayaFlux/Kinesis/Vision/VisionOp.hpp"

namespace MayaFlux::Test {

using Kinesis::Vision::BoundingBox;
using Kinesis::Vision::CannyParams;
using Kinesis::Vision::ChannelMask;
using Kinesis::Vision::ConfineParams;
using Kinesis::Vision::ConnectedComponentsParams;
using Kinesis::Vision::ExtractPeaksParams;
using Kinesis::Vision::FilterSeparableParams;
using Kinesis::Vision::FindContoursParams;
using Kinesis::Vision::GaussianBlurParams;
using Kinesis::Vision::HarrisParams;
using Kinesis::Vision::MorphParams;
using Kinesis::Vision::NormalizeRangeParams;
using Kinesis::Vision::OpticalFlowDenseParams;
using Kinesis::Vision::OtsuParams;
using Kinesis::Vision::ThresholdAdaptiveParams;
using Kinesis::Vision::ThresholdParams;
using Kinesis::Vision::TrackKeypointsParams;
using Kinesis::Vision::VisionBackend;
using Kinesis::Vision::VisionOp;
using Kinesis::Vision::VisionParams;
using Kinesis::Vision::VisionSequence;
using Kinesis::Vision::VisionStep;

namespace {

    /**
     * Build a hash-mutation check: each mutation changes exactly the field it
     * names, and the step must hash differently afterwards. A field that is not
     * hashed leaves the hash unchanged and fails here.
     */
    template <typename P>
    void expect_every_field_hashed(VisionOp op, const P& base, const std::vector<std::function<void(P&)>>& mutations)
    {
        const size_t base_hash = Kinesis::Vision::hash_vision_step(op, VisionParams { base });
        for (size_t i = 0; i < mutations.size(); ++i) {
            P changed = base;
            mutations[i](changed);
            EXPECT_NE(Kinesis::Vision::hash_vision_step(op, VisionParams { changed }), base_hash)
                << "mutation " << i << " of " << Reflect::enum_to_string(op) << " is not hashed";
        }
    }

}

// ---------------------------------------------------------------------------
// Builder: each method pushes its op, in order, with its parameters.
// ---------------------------------------------------------------------------

TEST(VisionOpBuilderTest, EveryBuilderMethodPushesItsOpInOrder)
{
    const auto sequence = VisionSequence::Builder {}
                              .rgba_to_gray()
                              .rgba_to_hsv()
                              .gray_to_rgba()
                              .downsample_2x()
                              .threshold({ .value = 0.5F })
                              .threshold_adaptive({ .block_size = 9, .offset = 0.1F })
                              .threshold_otsu()
                              .normalize()
                              .normalize_range({ .lo = 0.0F, .hi = 2.0F })
                              .gaussian_blur({ .sigma = 1.5F })
                              .filter_separable({ .kernel_x = { 1.0F }, .kernel_y = { 1.0F } })
                              .sobel()
                              .scharr()
                              .canny({})
                              .erode({ .radius = 1 })
                              .dilate({ .radius = 1 })
                              .open({ .radius = 1 })
                              .close({ .radius = 1 })
                              .morph_gradient({ .radius = 1 })
                              .harris_response()
                              .extract_peaks({ .threshold = 0.1F, .nms_radius = 2 })
                              .connected_components()
                              .track_keypoints()
                              .find_contours()
                              .snapshot()
                              .optical_flow_dense()
                              .confine({ .bounds = { .x = 0.0F, .y = 0.0F, .w = 1.0F, .h = 1.0F } })
                              .build();

    const std::vector<VisionOp> expected {
        VisionOp::RgbaToGray, VisionOp::RgbaToHsv, VisionOp::GrayToRgba, VisionOp::Downsample2x,
        VisionOp::Threshold, VisionOp::ThresholdAdaptive, VisionOp::ThresholdOtsu, VisionOp::NormalizeInplace,
        VisionOp::NormalizeRange, VisionOp::GaussianBlur, VisionOp::FilterSeparable, VisionOp::Sobel,
        VisionOp::Scharr, VisionOp::Canny, VisionOp::Erode, VisionOp::Dilate,
        VisionOp::Open, VisionOp::Close, VisionOp::MorphGradient, VisionOp::HarrisResponse,
        VisionOp::ExtractPeaks, VisionOp::ConnectedComponents, VisionOp::TrackKeypoints, VisionOp::FindContours,
        VisionOp::Snapshot, VisionOp::OpticalFlowDense, VisionOp::Confine,
    };

    ASSERT_EQ(sequence.steps.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(sequence.steps[i].op, expected[i]) << "step " << i;
    }
}

TEST(VisionOpBuilderTest, ParametersAreStoredWithTheirStep)
{
    const auto sequence = VisionSequence::Builder {}
                              .threshold({ .value = 0.25F, .channels = ChannelMask::Ch1 })
                              .canny({ .sigma = 2.0F, .low_threshold = 0.2F, .high_threshold = 0.6F })
                              .extract_peaks({ .threshold = 0.3F, .nms_radius = 5, .export_keypoints = true })
                              .build();
    ASSERT_EQ(sequence.steps.size(), 3U);

    const auto& threshold = std::get<ThresholdParams>(sequence.steps[0].params);
    EXPECT_EQ(threshold.value, 0.25F);
    EXPECT_EQ(threshold.channels, ChannelMask::Ch1);

    const auto& canny = std::get<CannyParams>(sequence.steps[1].params);
    EXPECT_EQ(canny.sigma, 2.0F);
    EXPECT_EQ(canny.low_threshold, 0.2F);
    EXPECT_EQ(canny.high_threshold, 0.6F);

    const auto& peaks = std::get<ExtractPeaksParams>(sequence.steps[2].params);
    EXPECT_EQ(peaks.threshold, 0.3F);
    EXPECT_EQ(peaks.nms_radius, 5U);
    EXPECT_TRUE(peaks.export_keypoints);
    EXPECT_FALSE(peaks.region.has_value());
}

TEST(VisionOpBuilderTest, ParameterlessMethodsStoreMonostate)
{
    const auto sequence = VisionSequence::Builder {}.rgba_to_gray().sobel().snapshot().build();
    ASSERT_EQ(sequence.steps.size(), 3U);
    for (const auto& step : sequence.steps) {
        EXPECT_TRUE(std::holds_alternative<std::monostate>(step.params)) << static_cast<int>(step.op);
    }
}

// ---------------------------------------------------------------------------
// Parameter defaults: the values a caller gets when omitting arguments.
// ---------------------------------------------------------------------------

TEST(VisionOpDefaultsTest, ThresholdChannelBandsSpanTheFullRange)
{
    const ThresholdParams p { .value = 0.5F };
    EXPECT_EQ(p.channels, ChannelMask::NONE);
    for (const auto& band : p.bands) {
        EXPECT_EQ(band.lo, 0.0F);
        EXPECT_EQ(band.hi, 1.0F);
    }
}

TEST(VisionOpDefaultsTest, CannyDefaults)
{
    const CannyParams p {};
    EXPECT_EQ(p.sigma, 1.4F);
    EXPECT_EQ(p.low_threshold, 0.1F);
    EXPECT_EQ(p.high_threshold, 0.3F);
}

TEST(VisionOpDefaultsTest, HarrisDefaultsAndNoRegion)
{
    const HarrisParams p {};
    EXPECT_EQ(p.k, 0.04F);
    EXPECT_EQ(p.sigma, 1.0F);
    EXPECT_FALSE(p.region.has_value());
}

TEST(VisionOpDefaultsTest, TrackKeypointsDefaults)
{
    const TrackKeypointsParams p {};
    EXPECT_EQ(p.window_radius, 7U);
    EXPECT_EQ(p.max_iterations, 20U);
    EXPECT_EQ(p.levels, 4U);
    EXPECT_EQ(p.max_points, 512U);
    EXPECT_EQ(p.min_distance, 8.0F);
    EXPECT_EQ(p.forward_backward_threshold, 0.0F);
    EXPECT_FALSE(p.export_tracks);
    EXPECT_TRUE(p.host_tracks);
}

TEST(VisionOpDefaultsTest, OpticalFlowDenseDefaults)
{
    const OpticalFlowDenseParams p {};
    EXPECT_EQ(p.window_radius, 5U);
    EXPECT_EQ(p.iterations, 3U);
    EXPECT_EQ(p.levels, 4U);
    EXPECT_EQ(p.eigen_threshold, 1e-3F);
    EXPECT_EQ(p.max_step, 4.0F);
    EXPECT_FALSE(p.visualize);
    EXPECT_EQ(p.visual_range, 2.0F);
    EXPECT_EQ(p.visual_min_motion, 0.0F);
}

TEST(VisionOpDefaultsTest, ContourAndComponentDefaultsExportNothing)
{
    const ConnectedComponentsParams cc {};
    EXPECT_FALSE(cc.export_labels);
    EXPECT_FALSE(cc.with_colors);
    EXPECT_FALSE(cc.export_boxes);
    EXPECT_FALSE(cc.export_label_buffer);

    const FindContoursParams fc {};
    EXPECT_EQ(fc.min_area, 0.0F);
    EXPECT_EQ(fc.max_contours, 0U);
    EXPECT_EQ(fc.max_points_per_contour, 0U);
    EXPECT_FALSE(fc.as_image);
    EXPECT_FALSE(fc.export_contours_buffer);
}

TEST(VisionOpDefaultsTest, ConfineKeepsTheCropSize)
{
    const ConfineParams p {};
    EXPECT_FALSE(p.resize_to_source);
}

TEST(VisionOpDefaultsTest, BuilderDefaultArgumentsStoreTheDefaults)
{
    const auto sequence = VisionSequence::Builder {}.track_keypoints().build();
    ASSERT_EQ(sequence.steps.size(), 1U);
    const auto& p = std::get<TrackKeypointsParams>(sequence.steps[0].params);
    EXPECT_EQ(p.window_radius, TrackKeypointsParams {}.window_radius);
    EXPECT_EQ(p.max_points, TrackKeypointsParams {}.max_points);
}

// ---------------------------------------------------------------------------
// Sequence: backend selection and deferral.
// ---------------------------------------------------------------------------

TEST(VisionOpSequenceTest, DirectlyConstructedSequenceDefaultsToGpu)
{
    const VisionSequence sequence {};
    EXPECT_EQ(sequence.backend, VisionBackend::GPU);
    EXPECT_TRUE(sequence.steps.empty());
}

TEST(VisionOpSequenceTest, BuildDefaultsToGpuAndCanSelectCpu)
{
    EXPECT_EQ(VisionSequence::Builder {}.rgba_to_gray().build().backend, VisionBackend::GPU);
    EXPECT_EQ(VisionSequence::Builder {}.rgba_to_gray().build(VisionBackend::CPU).backend, VisionBackend::CPU);
}

TEST(VisionOpSequenceTest, DeferMarksOnlyTheMostRecentStep)
{
    const auto sequence = VisionSequence::Builder {}
                              .rgba_to_gray()
                              .threshold({ .value = 0.5F })
                              .defer()
                              .sobel()
                              .build();
    ASSERT_EQ(sequence.steps.size(), 3U);
    EXPECT_FALSE(sequence.steps[0].deferred);
    EXPECT_TRUE(sequence.steps[1].deferred);
    EXPECT_FALSE(sequence.steps[2].deferred);
}

TEST(VisionOpSequenceTest, DeferOnAnEmptyBuilderIsANoOp)
{
    const auto sequence = VisionSequence::Builder {}.defer().build();
    EXPECT_TRUE(sequence.steps.empty());
}

TEST(VisionOpSequenceTest, NewStepsAreNotDeferred)
{
    const VisionStep step { .op = VisionOp::Sobel };
    EXPECT_FALSE(step.deferred);
    EXPECT_TRUE(std::holds_alternative<std::monostate>(step.params));
}

// ---------------------------------------------------------------------------
// Predicates over a whole sequence.
// ---------------------------------------------------------------------------

TEST(VisionOpPredicateTest, TracksKeypointsIsDetectedAnywhereInTheSequence)
{
    const auto with = VisionSequence::Builder {}.rgba_to_gray().sobel().track_keypoints().build();
    const auto without = VisionSequence::Builder {}.rgba_to_gray().sobel().build();
    EXPECT_TRUE(Kinesis::Vision::tracks_keypoints(with));
    EXPECT_FALSE(Kinesis::Vision::tracks_keypoints(without));
    EXPECT_FALSE(Kinesis::Vision::tracks_keypoints(VisionSequence {}));
}

TEST(VisionOpPredicateTest, TrackFollowsPeaksOnlyWhenAdjacent)
{
    const auto adjacent = VisionSequence::Builder {}
                              .extract_peaks({ .threshold = 0.1F, .nms_radius = 2 })
                              .track_keypoints()
                              .build();
    const auto gapped = VisionSequence::Builder {}
                            .extract_peaks({ .threshold = 0.1F, .nms_radius = 2 })
                            .sobel()
                            .track_keypoints()
                            .build();
    const auto reversed = VisionSequence::Builder {}
                              .track_keypoints()
                              .extract_peaks({ .threshold = 0.1F, .nms_radius = 2 })
                              .build();
    EXPECT_TRUE(Kinesis::Vision::track_follows_peaks(adjacent));
    EXPECT_FALSE(Kinesis::Vision::track_follows_peaks(gapped));
    EXPECT_FALSE(Kinesis::Vision::track_follows_peaks(reversed));
    EXPECT_FALSE(Kinesis::Vision::track_follows_peaks(VisionSequence {}));
}

// ---------------------------------------------------------------------------
// Step hash: equal steps hash equal, and every field that changes the output
// changes the hash. Steps that hash equal are treated as interchangeable.
// ---------------------------------------------------------------------------

TEST(VisionOpHashTest, EqualStepsHashEqual)
{
    const ThresholdParams a { .value = 0.5F };
    const ThresholdParams b { .value = 0.5F };
    EXPECT_EQ(Kinesis::Vision::hash_vision_step(VisionOp::Threshold, VisionParams { a }),
        Kinesis::Vision::hash_vision_step(VisionOp::Threshold, VisionParams { b }));
}

TEST(VisionOpHashTest, TheOpIsPartOfTheHash)
{
    EXPECT_NE(Kinesis::Vision::hash_vision_step(VisionOp::Sobel, std::monostate {}),
        Kinesis::Vision::hash_vision_step(VisionOp::Scharr, std::monostate {}));
}

TEST(VisionOpHashTest, ThresholdFieldsAreHashed)
{
    expect_every_field_hashed<ThresholdParams>(VisionOp::Threshold, { .value = 0.5F },
        { [](auto& p) { p.value += 0.25F; },
            [](auto& p) { p.channels = ChannelMask::Ch0; },
            [](auto& p) { p.bands[0].lo += 0.1F; },
            [](auto& p) { p.bands[1].hi -= 0.1F; },
            [](auto& p) { p.bands[2].hi -= 0.1F; } });
}

TEST(VisionOpHashTest, ThresholdAdaptiveFieldsAreHashed)
{
    expect_every_field_hashed<ThresholdAdaptiveParams>(VisionOp::ThresholdAdaptive, { .block_size = 9, .offset = 0.1F },
        { [](auto& p) { p.block_size += 2; },
            [](auto& p) { p.offset += 0.1F; },
            [](auto& p) { p.channels = ChannelMask::Ch2; } });
}

TEST(VisionOpHashTest, OtsuChannelsAreHashed)
{
    expect_every_field_hashed<OtsuParams>(VisionOp::ThresholdOtsu, {},
        { [](auto& p) { p.channels = ChannelMask::Ch1; } });
}

TEST(VisionOpHashTest, NormalizeRangeFieldsAreHashed)
{
    expect_every_field_hashed<NormalizeRangeParams>(VisionOp::NormalizeRange, { .lo = 0.0F, .hi = 1.0F },
        { [](auto& p) { p.lo += 0.1F; },
            [](auto& p) { p.hi += 0.1F; } });
}

TEST(VisionOpHashTest, GaussianBlurSigmaIsHashed)
{
    expect_every_field_hashed<GaussianBlurParams>(VisionOp::GaussianBlur, { .sigma = 1.5F },
        { [](auto& p) { p.sigma += 0.5F; } });
}

TEST(VisionOpHashTest, CannyFieldsAreHashed)
{
    expect_every_field_hashed<CannyParams>(VisionOp::Canny, {},
        { [](auto& p) { p.sigma += 0.5F; },
            [](auto& p) { p.low_threshold += 0.05F; },
            [](auto& p) { p.high_threshold += 0.05F; } });
}

TEST(VisionOpHashTest, MorphRadiusIsHashed)
{
    expect_every_field_hashed<MorphParams>(VisionOp::Erode, { .radius = 1 },
        { [](auto& p) { p.radius += 1; } });
}

TEST(VisionOpHashTest, HarrisFieldsAndRegionAreHashed)
{
    expect_every_field_hashed<HarrisParams>(VisionOp::HarrisResponse, {},
        { [](auto& p) { p.k += 0.01F; },
            [](auto& p) { p.sigma += 0.5F; },
            [](auto& p) { p.region = BoundingBox { .x = 0.1F, .y = 0.1F, .w = 0.5F, .h = 0.5F }; } });

    HarrisParams with_region {};
    with_region.region = BoundingBox { .x = 0.1F, .y = 0.1F, .w = 0.5F, .h = 0.5F };
    expect_every_field_hashed<HarrisParams>(VisionOp::HarrisResponse, with_region,
        { [](auto& p) { p.region->x += 0.1F; },
            [](auto& p) { p.region->y += 0.1F; },
            [](auto& p) { p.region->w += 0.1F; },
            [](auto& p) { p.region->h += 0.1F; },
            [](auto& p) { p.region.reset(); } });
}

TEST(VisionOpHashTest, ExtractPeaksFieldsAndRegionAreHashed)
{
    expect_every_field_hashed<ExtractPeaksParams>(VisionOp::ExtractPeaks, { .threshold = 0.1F, .nms_radius = 2 },
        { [](auto& p) { p.threshold += 0.05F; },
            [](auto& p) { p.nms_radius += 1; },
            [](auto& p) { p.export_keypoints = true; },
            [](auto& p) { p.region = BoundingBox { .x = 0.0F, .y = 0.0F, .w = 0.5F, .h = 0.5F }; } });
}

TEST(VisionOpHashTest, TrackKeypointsFieldsAreHashed)
{
    expect_every_field_hashed<TrackKeypointsParams>(VisionOp::TrackKeypoints, {},
        { [](auto& p) { p.window_radius += 1; },
            [](auto& p) { p.max_iterations += 1; },
            [](auto& p) { p.eigen_threshold *= 2.0F; },
            [](auto& p) { p.error_threshold += 0.1F; },
            [](auto& p) { p.levels += 1; },
            [](auto& p) { p.max_points += 1; },
            [](auto& p) { p.min_distance += 1.0F; },
            [](auto& p) { p.forward_backward_threshold += 0.1F; },
            [](auto& p) { p.export_tracks = true; },
            [](auto& p) { p.host_tracks = false; } });
}

TEST(VisionOpHashTest, ConnectedComponentsFieldsAreHashed)
{
    expect_every_field_hashed<ConnectedComponentsParams>(VisionOp::ConnectedComponents, {},
        { [](auto& p) { p.export_labels = true; },
            [](auto& p) { p.with_colors = true; },
            [](auto& p) { p.export_boxes = true; },
            [](auto& p) { p.export_label_buffer = true; } });
}

TEST(VisionOpHashTest, FindContoursFieldsAreHashed)
{
    expect_every_field_hashed<FindContoursParams>(VisionOp::FindContours, {},
        { [](auto& p) { p.min_area += 1.0F; },
            [](auto& p) { p.max_contours += 1; },
            [](auto& p) { p.max_points_per_contour += 1; },
            [](auto& p) { p.as_image = true; },
            [](auto& p) { p.export_contours_buffer = true; } });
}

TEST(VisionOpHashTest, OpticalFlowDenseFieldsAreHashed)
{
    expect_every_field_hashed<OpticalFlowDenseParams>(VisionOp::OpticalFlowDense, {},
        { [](auto& p) { p.window_radius += 1; },
            [](auto& p) { p.iterations += 1; },
            [](auto& p) { p.levels += 1; },
            [](auto& p) { p.eigen_threshold *= 2.0F; },
            [](auto& p) { p.max_step += 1.0F; },
            [](auto& p) { p.visualize = true; },
            [](auto& p) { p.visual_range += 1.0F; },
            [](auto& p) { p.visual_min_motion += 0.1F; } });
}

TEST(VisionOpHashTest, ConfineBoundsAndResizeAreHashed)
{
    expect_every_field_hashed<ConfineParams>(VisionOp::Confine,
        { .bounds = { .x = 0.0F, .y = 0.0F, .w = 1.0F, .h = 1.0F } },
        { [](auto& p) { p.bounds.x += 0.1F; },
            [](auto& p) { p.bounds.y += 0.1F; },
            [](auto& p) { p.bounds.w -= 0.1F; },
            [](auto& p) { p.bounds.h -= 0.1F; },
            [](auto& p) { p.resize_to_source = true; } });
}

TEST(VisionOpHashTest, SeparableKernelsHashByTheirSplit)
{
    // Same concatenated values, different split between kernel_x and kernel_y.
    // A correct hash distinguishes these two steps.
    const auto split_after_first = Kinesis::Vision::hash_vision_step(VisionOp::FilterSeparable,
        VisionParams { FilterSeparableParams { .kernel_x = { 1.0F }, .kernel_y = { 2.0F } } });
    const auto split_after_second = Kinesis::Vision::hash_vision_step(VisionOp::FilterSeparable,
        VisionParams { FilterSeparableParams { .kernel_x = { 1.0F, 2.0F }, .kernel_y = {} } });
    EXPECT_NE(split_after_first, split_after_second);
}

} // namespace MayaFlux::Test
