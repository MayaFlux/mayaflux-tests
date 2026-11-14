#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "MayaFlux/Kinesis/Vision/VisionQuery.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

using Kinesis::Vision::BoundingBox;
using Kinesis::Vision::CannyParams;
using Kinesis::Vision::ChannelMask;
using Kinesis::Vision::ConnectedComponentsParams;
using Kinesis::Vision::DetectEdgesParams;
using Kinesis::Vision::DetectFeaturesParams;
using Kinesis::Vision::EstimateMotionParams;
using Kinesis::Vision::ExtractPeaksParams;
using Kinesis::Vision::FindContoursParams;
using Kinesis::Vision::FindElementsParams;
using Kinesis::Vision::HarrisParams;
using Kinesis::Vision::MeasureAppearanceParams;
using Kinesis::Vision::OpticalFlowDenseParams;
using Kinesis::Vision::SegmentationSpace;
using Kinesis::Vision::ThresholdAdaptiveParams;
using Kinesis::Vision::ThresholdParams;
using Kinesis::Vision::TrackKeypointsParams;
using Kinesis::Vision::TrackObjectsParams;
using Kinesis::Vision::VisionIntent;
using Kinesis::Vision::VisionOp;
using Kinesis::Vision::VisionQuery;
using Kinesis::Vision::VisionResult;
using Kinesis::Vision::VisionSequence;

namespace {

    std::vector<VisionOp> ops_of(const VisionSequence& sequence)
    {
        std::vector<VisionOp> ops;
        ops.reserve(sequence.steps.size());
        for (const auto& step : sequence.steps)
            ops.push_back(step.op);
        return ops;
    }

    /** Non-owning handle used only for pointer identity; never dereferenced. */
    std::shared_ptr<Core::VKImage> marker_image(uintptr_t tag)
    {
        return std::shared_ptr<Core::VKImage>(reinterpret_cast<Core::VKImage*>(tag), [](Core::VKImage*) { });
    }

    const BoundingBox k_region { .x = 0.1F, .y = 0.2F, .w = 0.5F, .h = 0.4F };

} // namespace

// ---------------------------------------------------------------------------
// Empty and ignored inputs
// ---------------------------------------------------------------------------

TEST(VisionQueryResolveTest, EmptyQueryResolvesToNothing)
{
    EXPECT_TRUE(Kinesis::Vision::resolve(VisionQuery {}).empty());
}

TEST(VisionQueryResolveTest, IntentWithoutItsParamsIsIgnored)
{
    VisionQuery query;
    query.intents = VisionIntent::TrackObjects | VisionIntent::FindElements
        | VisionIntent::DetectFeatures | VisionIntent::DetectEdges
        | VisionIntent::EstimateMotion | VisionIntent::MeasureAppearance;
    EXPECT_TRUE(Kinesis::Vision::resolve(query).empty());
}

TEST(VisionQueryResolveTest, ParamsWithoutTheirIntentAreIgnored)
{
    VisionQuery query;
    query.detect_edges = DetectEdgesParams {};
    query.measure_appearance = MeasureAppearanceParams {};
    EXPECT_TRUE(Kinesis::Vision::resolve(query).empty());
}

// Current behaviour: intents with no backing VisionOp contribute no steps.
// Pinned until the expected behaviour for these intents is confirmed.
TEST(VisionQueryResolveTest, UnbackedIntentsContributeNoSteps)
{
    VisionQuery plain;
    plain.intents = VisionIntent::Classify | VisionIntent::EstimateDepth
        | VisionIntent::DetectAnomaly | VisionIntent::Register;
    EXPECT_TRUE(Kinesis::Vision::resolve(plain).empty());

    VisionQuery mixed;
    mixed.intents = VisionIntent::DetectEdges | VisionIntent::Classify | VisionIntent::Register;
    mixed.detect_edges = DetectEdgesParams {};
    const auto resolved = Kinesis::Vision::resolve(mixed);
    ASSERT_EQ(resolved.size(), 1U);
    EXPECT_EQ(ops_of(resolved[0].sequence), (std::vector<VisionOp> { VisionOp::RgbaToGray, VisionOp::Canny }));
}

// ---------------------------------------------------------------------------
// Single-intent chains
// ---------------------------------------------------------------------------

TEST(VisionQueryResolveTest, DetectEdgesBuildsGrayThenCanny)
{
    VisionQuery query;
    query.intents = VisionIntent::DetectEdges;
    query.detect_edges = DetectEdgesParams { .canny = { .sigma = 2.0F, .low_threshold = 0.2F, .high_threshold = 0.6F } };

    const auto resolved = Kinesis::Vision::resolve(query);
    ASSERT_EQ(resolved.size(), 1U);
    EXPECT_EQ(ops_of(resolved[0].sequence), (std::vector<VisionOp> { VisionOp::RgbaToGray, VisionOp::Canny }));
    EXPECT_EQ(resolved[0].seed_field, nullptr);

    const auto& canny = std::get<CannyParams>(resolved[0].sequence.steps[1].params);
    EXPECT_EQ(canny.sigma, 2.0F);
    EXPECT_EQ(canny.low_threshold, 0.2F);
    EXPECT_EQ(canny.high_threshold, 0.6F);
}

TEST(VisionQueryResolveTest, MeasureAppearanceBuildsGrayThenSobel)
{
    VisionQuery query;
    query.intents = VisionIntent::MeasureAppearance;
    query.measure_appearance = MeasureAppearanceParams {};

    const auto resolved = Kinesis::Vision::resolve(query);
    ASSERT_EQ(resolved.size(), 1U);
    EXPECT_EQ(ops_of(resolved[0].sequence), (std::vector<VisionOp> { VisionOp::RgbaToGray, VisionOp::Sobel }));
}

TEST(VisionQueryResolveTest, EstimateMotionBuildsGrayThenDenseFlow)
{
    VisionQuery query;
    query.intents = VisionIntent::EstimateMotion;
    query.estimate_motion = EstimateMotionParams { .flow = { .window_radius = 9U } };

    const auto resolved = Kinesis::Vision::resolve(query);
    ASSERT_EQ(resolved.size(), 1U);
    EXPECT_EQ(ops_of(resolved[0].sequence), (std::vector<VisionOp> { VisionOp::RgbaToGray, VisionOp::OpticalFlowDense }));
    EXPECT_EQ(std::get<OpticalFlowDenseParams>(resolved[0].sequence.steps[1].params).window_radius, 9U);
}

TEST(VisionQueryResolveTest, DetectFeaturesPassesRegionToHarrisAndPeaks)
{
    VisionQuery query;
    query.intents = VisionIntent::DetectFeatures;
    query.detect_features = DetectFeaturesParams {
        .harris = { .k = 0.05F, .sigma = 1.2F },
        .peaks = { .threshold = 0.3F, .nms_radius = 4U },
        .region = k_region,
    };

    const auto resolved = Kinesis::Vision::resolve(query);
    ASSERT_EQ(resolved.size(), 1U);
    EXPECT_EQ(ops_of(resolved[0].sequence),
        (std::vector<VisionOp> { VisionOp::RgbaToGray, VisionOp::HarrisResponse, VisionOp::ExtractPeaks }));

    const auto& harris = std::get<HarrisParams>(resolved[0].sequence.steps[1].params);
    const auto& peaks = std::get<ExtractPeaksParams>(resolved[0].sequence.steps[2].params);
    ASSERT_TRUE(harris.region.has_value());
    ASSERT_TRUE(peaks.region.has_value());
    EXPECT_EQ(harris.region->x, k_region.x);
    EXPECT_EQ(peaks.region->h, k_region.h);
    EXPECT_EQ(harris.k, 0.05F);
    EXPECT_EQ(peaks.threshold, 0.3F);
}

TEST(VisionQueryResolveTest, TrackObjectsPassesRegionToHarrisPeaksAndTracker)
{
    VisionQuery query;
    query.intents = VisionIntent::TrackObjects;
    query.track_objects = TrackObjectsParams {
        .harris = {},
        .peaks = { .threshold = 0.1F, .nms_radius = 2U },
        .track = { .window_radius = 11U },
        .region = k_region,
    };

    const auto resolved = Kinesis::Vision::resolve(query);
    ASSERT_EQ(resolved.size(), 1U);
    EXPECT_EQ(ops_of(resolved[0].sequence),
        (std::vector<VisionOp> { VisionOp::RgbaToGray, VisionOp::HarrisResponse, VisionOp::ExtractPeaks, VisionOp::TrackKeypoints }));

    const auto& harris = std::get<HarrisParams>(resolved[0].sequence.steps[1].params);
    const auto& peaks = std::get<ExtractPeaksParams>(resolved[0].sequence.steps[2].params);
    const auto& track = std::get<TrackKeypointsParams>(resolved[0].sequence.steps[3].params);
    ASSERT_TRUE(harris.region.has_value());
    ASSERT_TRUE(peaks.region.has_value());
    EXPECT_EQ(harris.region->w, k_region.w);
    EXPECT_EQ(peaks.region->y, k_region.y);
    EXPECT_EQ(track.window_radius, 11U);
}

TEST(VisionQueryResolveTest, NoRegionLeavesHarrisAndPeaksUnrestricted)
{
    VisionQuery query;
    query.intents = VisionIntent::DetectFeatures;
    query.detect_features = DetectFeaturesParams { .peaks = { .threshold = 0.3F, .nms_radius = 4U } };

    const auto resolved = Kinesis::Vision::resolve(query);
    ASSERT_EQ(resolved.size(), 1U);
    EXPECT_FALSE(std::get<HarrisParams>(resolved[0].sequence.steps[1].params).region.has_value());
    EXPECT_FALSE(std::get<ExtractPeaksParams>(resolved[0].sequence.steps[2].params).region.has_value());
}

// ---------------------------------------------------------------------------
// FindElements: segmentation space, method, and forced export.
// ---------------------------------------------------------------------------

TEST(VisionQueryFindElementsTest, GraySpaceConvertsThenOtsuThenComponents)
{
    VisionQuery query;
    query.intents = VisionIntent::FindElements;
    query.find_elements = FindElementsParams {};

    const auto resolved = Kinesis::Vision::resolve(query);
    ASSERT_EQ(resolved.size(), 1U);
    EXPECT_EQ(ops_of(resolved[0].sequence),
        (std::vector<VisionOp> { VisionOp::RgbaToGray, VisionOp::ThresholdOtsu,
            VisionOp::ConnectedComponents, VisionOp::FindContours }));
}

TEST(VisionQueryFindElementsTest, HsvSpaceUsesHsvConversion)
{
    VisionQuery query;
    query.intents = VisionIntent::FindElements;
    query.find_elements = FindElementsParams { .segmentation = { .space = SegmentationSpace::Hsv } };

    const auto resolved = Kinesis::Vision::resolve(query);
    ASSERT_EQ(resolved.size(), 1U);
    EXPECT_EQ(resolved[0].sequence.steps.front().op, VisionOp::RgbaToHsv);
}

TEST(VisionQueryFindElementsTest, RgbaSpaceSkipsAnyConversion)
{
    VisionQuery query;
    query.intents = VisionIntent::FindElements;
    query.find_elements = FindElementsParams { .segmentation = { .space = SegmentationSpace::Rgba } };

    const auto resolved = Kinesis::Vision::resolve(query);
    ASSERT_EQ(resolved.size(), 1U);
    EXPECT_EQ(ops_of(resolved[0].sequence),
        (std::vector<VisionOp> { VisionOp::ThresholdOtsu, VisionOp::ConnectedComponents, VisionOp::FindContours }));
}

TEST(VisionQueryFindElementsTest, AdaptiveMethodMapsToThresholdAdaptive)
{
    VisionQuery query;
    query.intents = VisionIntent::FindElements;
    query.find_elements = FindElementsParams {
        .segmentation = { .method = ThresholdAdaptiveParams { .block_size = 15U, .offset = 0.05F } },
    };

    const auto resolved = Kinesis::Vision::resolve(query);
    ASSERT_EQ(resolved.size(), 1U);
    EXPECT_EQ(resolved[0].sequence.steps[1].op, VisionOp::ThresholdAdaptive);
    const auto& p = std::get<ThresholdAdaptiveParams>(resolved[0].sequence.steps[1].params);
    EXPECT_EQ(p.block_size, 15U);
    EXPECT_EQ(p.offset, 0.05F);
}

TEST(VisionQueryFindElementsTest, ThresholdMethodMapsToThreshold)
{
    VisionQuery query;
    query.intents = VisionIntent::FindElements;
    query.find_elements = FindElementsParams {
        .segmentation = { .method = ThresholdParams { .value = 0.4F, .channels = ChannelMask::Ch0 } },
    };

    const auto resolved = Kinesis::Vision::resolve(query);
    ASSERT_EQ(resolved.size(), 1U);
    EXPECT_EQ(resolved[0].sequence.steps[1].op, VisionOp::Threshold);
    const auto& p = std::get<ThresholdParams>(resolved[0].sequence.steps[1].params);
    EXPECT_EQ(p.value, 0.4F);
    EXPECT_EQ(p.channels, ChannelMask::Ch0);
}

TEST(VisionQueryFindElementsTest, ComponentExportBoxesIsForcedOn)
{
    VisionQuery query;
    query.intents = VisionIntent::FindElements;
    query.find_elements = FindElementsParams { .components = { .with_colors = true, .export_boxes = false } };

    const auto resolved = Kinesis::Vision::resolve(query);
    ASSERT_EQ(resolved.size(), 1U);
    const auto& cc = std::get<ConnectedComponentsParams>(resolved[0].sequence.steps[2].params);
    EXPECT_TRUE(cc.export_boxes);
    EXPECT_TRUE(cc.with_colors);
}

TEST(VisionQueryFindElementsTest, ContourParamsPassThrough)
{
    VisionQuery query;
    query.intents = VisionIntent::FindElements;
    query.find_elements = FindElementsParams { .contours = { .min_area = 3.0F, .max_contours = 5U } };

    const auto resolved = Kinesis::Vision::resolve(query);
    ASSERT_EQ(resolved.size(), 1U);
    const auto& fc = std::get<FindContoursParams>(resolved[0].sequence.steps[3].params);
    EXPECT_EQ(fc.min_area, 3.0F);
    EXPECT_EQ(fc.max_contours, 5U);
}

// ---------------------------------------------------------------------------
// Flow lane: TrackObjects and EstimateMotion share one sequence.
// ---------------------------------------------------------------------------

TEST(VisionQueryFlowLaneTest, TrackObjectsAndEstimateMotionMergeIntoOneSequence)
{
    VisionQuery query;
    query.intents = VisionIntent::TrackObjects | VisionIntent::EstimateMotion;
    query.track_objects = TrackObjectsParams {};
    query.estimate_motion = EstimateMotionParams {};

    const auto resolved = Kinesis::Vision::resolve(query);
    ASSERT_EQ(resolved.size(), 1U);
    EXPECT_EQ(ops_of(resolved[0].sequence),
        (std::vector<VisionOp> { VisionOp::RgbaToGray, VisionOp::HarrisResponse, VisionOp::ExtractPeaks,
            VisionOp::TrackKeypoints, VisionOp::OpticalFlowDense }));
    EXPECT_EQ(resolved[0].seed_field, nullptr);
}

// ---------------------------------------------------------------------------
// Ordering and shared prefixes.
// ---------------------------------------------------------------------------

TEST(VisionQuerySharingTest, UnsharedChainsKeepTheirFullStepsInInsertionOrder)
{
    VisionQuery query;
    query.intents = VisionIntent::FindElements | VisionIntent::MeasureAppearance | VisionIntent::DetectEdges;
    query.find_elements = FindElementsParams {};
    query.measure_appearance = MeasureAppearanceParams {};
    query.detect_edges = DetectEdgesParams {};

    const auto resolved = Kinesis::Vision::resolve(query);
    ASSERT_EQ(resolved.size(), 3U);
    EXPECT_EQ(ops_of(resolved[0].sequence),
        (std::vector<VisionOp> { VisionOp::RgbaToGray, VisionOp::ThresholdOtsu,
            VisionOp::ConnectedComponents, VisionOp::FindContours }));
    EXPECT_EQ(ops_of(resolved[1].sequence), (std::vector<VisionOp> { VisionOp::RgbaToGray, VisionOp::Sobel }));
    EXPECT_EQ(ops_of(resolved[2].sequence), (std::vector<VisionOp> { VisionOp::RgbaToGray, VisionOp::Canny }));
    for (const auto& r : resolved) {
        EXPECT_EQ(r.seed_field, nullptr);
    }
}

TEST(VisionQuerySharingTest, RefusedWhenTheReferenceOverwritesSharedImagesAfterThePrefix)
{
    // DetectFeatures is the reference here. Its Harris step overwrites shared
    // images after the common Gray prefix, so MeasureAppearance keeps its steps.
    VisionQuery query;
    query.intents = VisionIntent::DetectFeatures | VisionIntent::MeasureAppearance;
    query.detect_features = DetectFeaturesParams {};
    query.measure_appearance = MeasureAppearanceParams {};

    const auto resolved = Kinesis::Vision::resolve(query);
    ASSERT_EQ(resolved.size(), 2U);
    EXPECT_EQ(ops_of(resolved[1].sequence), (std::vector<VisionOp> { VisionOp::RgbaToGray, VisionOp::Sobel }));
    EXPECT_EQ(resolved[1].seed_field, nullptr);
}

TEST(VisionQuerySharingTest, SharedHarrisPrefixSeedsTheLaterChainFromTheFlowLane)
{
    VisionQuery query;
    query.intents = VisionIntent::TrackObjects | VisionIntent::DetectFeatures;
    query.track_objects = TrackObjectsParams {};
    query.detect_features = DetectFeaturesParams {};

    const auto resolved = Kinesis::Vision::resolve(query);
    ASSERT_EQ(resolved.size(), 2U);
    EXPECT_EQ(ops_of(resolved[0].sequence),
        (std::vector<VisionOp> { VisionOp::RgbaToGray, VisionOp::HarrisResponse, VisionOp::ExtractPeaks,
            VisionOp::TrackKeypoints }));

    // Gray and Harris are shared; only the peak extraction remains to run.
    EXPECT_EQ(resolved[1].seed_lane, 0U);
    EXPECT_EQ(ops_of(resolved[1].sequence), (std::vector<VisionOp> { VisionOp::ExtractPeaks }));
    ASSERT_NE(resolved[1].seed_field, nullptr);

    // The seed is the Harris response, not the gray frame.
    VisionResult result;
    result.gray = marker_image(0x10);
    result.images.harris_response = marker_image(0x20);
    EXPECT_EQ(resolved[1].seed_field(result), result.images.harris_response);
}

TEST(VisionQuerySharingTest, SeededChainsFollowUnseededChains)
{
    // FindElements is inserted before DetectFeatures, but its Otsu step breaks
    // sharing with the flow lane, so it stays unseeded and lands last.
    VisionQuery query;
    query.intents = VisionIntent::TrackObjects | VisionIntent::FindElements | VisionIntent::DetectFeatures;
    query.track_objects = TrackObjectsParams {};
    query.find_elements = FindElementsParams {};
    query.detect_features = DetectFeaturesParams {};

    const auto resolved = Kinesis::Vision::resolve(query);
    ASSERT_EQ(resolved.size(), 3U);
    EXPECT_EQ(resolved[0].seed_field, nullptr);
    ASSERT_NE(resolved[1].seed_field, nullptr);
    EXPECT_EQ(resolved[2].seed_field, nullptr);
    EXPECT_EQ(ops_of(resolved[2].sequence),
        (std::vector<VisionOp> { VisionOp::RgbaToGray, VisionOp::ThresholdOtsu,
            VisionOp::ConnectedComponents, VisionOp::FindContours }));
}

} // namespace MayaFlux::Test
