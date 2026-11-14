#include "gtest/gtest.h"
#include "../../support/test_metadata.hpp"

#include "../../support/vision_test_support.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))

namespace MayaFlux::Test::Vision {

namespace KV = MayaFlux::Kinesis::Vision;
using Yantra::VisionAnalyzer;
using Yantra::VisionAnalyzerConfig;

namespace {

using Fixture = VisionGpuFixture;

KV::VisionQuery track_query()
{
    KV::VisionQuery query;
    query.intents = KV::VisionIntent::TrackObjects;
    query.track_objects = KV::TrackObjectsParams {
        .harris = { .sigma = 1.5F },
        .peaks = { .threshold = 0.05F, .nms_radius = 6 },
        .track = { .error_threshold = 0.2F },
    };
    return query;
}

bool element_pixel(uint32_t x, uint32_t y)
{
    const bool square = x >= 1 && x <= 3 && y >= 1 && y <= 3;
    const bool tall = x >= 8 && x <= 9 && y >= 1 && y <= 2;
    const bool bar = y == 3 && x >= 8 && x <= 10;
    const bool tip = x == 10 && y == 4;
    const bool small = x >= 13 && x <= 14 && y >= 13 && y <= 14;
    return square || tall || bar || tip || small;
}

ImageHandle element_frame()
{
    return pattern_frame(16, [](uint32_t x, uint32_t y) {
        return element_pixel(x, y) ? glm::vec4 { 1.0F } : glm::vec4 { 0.0F };
    });
}

ImageHandle step_frame(uint32_t size)
{
    return pattern_frame(size, [size](uint32_t x, uint32_t) {
        const float v = x < size / 2 ? 0.0F : 1.0F;
        return glm::vec4 { v, v, v, 1.0F };
    });
}

ImageHandle red_blue_frame()
{
    return pattern_frame(32, [](uint32_t x, uint32_t y) {
        if (x >= 4 && x < 12 && y >= 4 && y < 12)
            return glm::vec4 { 1.0F, 0.0F, 0.0F, 1.0F };
        if (x >= 20 && x < 28 && y >= 20 && y < 28)
            return glm::vec4 { 0.0F, 0.0F, 1.0F, 1.0F };
        return glm::vec4 { 0.0F, 0.0F, 0.0F, 1.0F };
    });
}

} // namespace

class VisionAnalyzerTest : public Fixture { };

TEST_F(VisionAnalyzerTest, TrackVelocityMatchesTheShiftOfTheFrame)
{
    constexpr uint32_t size = 256;
    constexpr float dx = 3.0F;
    constexpr float dy = -2.0F;
    constexpr float tolerance_px = 1.0F;

    const auto frame0 = wave_frame(size, 0.0F, 0.0F);
    const auto frame1 = wave_frame(size, dx, dy);
    ASSERT_TRUE(frame0 && frame1);

    VisionAnalyzer analyzer(analyzer_config(track_query()));
    const auto seed = analyzer.analyze_vision(as_container(frame0), 0);
    EXPECT_TRUE(!seed.track_objects || seed.track_objects->tracks.empty());

    const auto analysis = analyzer.analyze_vision(as_container(frame1), 0);
    ASSERT_TRUE(analysis.track_objects.has_value());

    const auto& to = *analysis.track_objects;
    const auto fsize = static_cast<float>(size);
    EXPECT_NEAR(to.velocity.x * fsize, dx, tolerance_px);
    EXPECT_NEAR(to.velocity.y * fsize, dy, tolerance_px);
    EXPECT_GE(to.bounds.w, 0.0F);
    EXPECT_GE(to.bounds.h, 0.0F);
    EXPECT_GE(to.tracks.size(), 4U);
}

TEST_F(VisionAnalyzerTest, TrackCountsAgeAndSpeedMatchTheHostReference)
{
    constexpr uint32_t size = 256;
    constexpr float dx = 3.0F;
    constexpr float dy = -2.0F;
    constexpr float tolerance = 0.05F;

    const auto frame0 = wave_frame(size, 0.0F, 0.0F);
    const auto frame1 = wave_frame(size, dx, dy);
    ASSERT_TRUE(frame0 && frame1);

    VisionAnalyzer analyzer(analyzer_config(track_query()));
    (void)analyzer.analyze_vision(as_container(frame0), 0);
    const auto analysis = analyzer.analyze_vision(as_container(frame1), 0);
    ASSERT_TRUE(analysis.track_objects.has_value());
    ASSERT_FALSE(analysis.track_objects->tracks.empty());

    const auto& to = *analysis.track_objects;
    const auto fsize = static_cast<float>(size);

    uint32_t expected_tracked = 0;
    uint32_t expected_lost = 0;
    float age_sum = 0.0F;
    uint32_t expected_max_age = 0;
    for (const auto& t : to.tracks) {
        if (t.tracked) {
            ++expected_tracked;
        } else {
            ++expected_lost;
        }
        age_sum += static_cast<float>(t.age);
        expected_max_age = std::max(expected_max_age, t.age);
    }
    const float expected_mean_age = age_sum / static_cast<float>(to.tracks.size());
    const float expected_speed_px = glm::length(glm::vec2 { to.velocity.x * fsize, to.velocity.y * fsize });

    EXPECT_EQ(to.tracked_count, expected_tracked);
    EXPECT_EQ(to.lost_count, expected_lost);
    EXPECT_EQ(to.tracked_count + to.lost_count, to.tracks.size());
    EXPECT_NEAR(to.mean_age, expected_mean_age, tolerance);
    EXPECT_EQ(to.max_age, expected_max_age);
    EXPECT_NEAR(to.mean_speed_px, expected_speed_px, tolerance);
}

TEST_F(VisionAnalyzerTest, FindElementsSortsContoursLargestFirstAndJoinsEachToABox)
{
    const auto image = element_frame();
    ASSERT_TRUE(image);

    KV::VisionQuery query;
    query.intents = KV::VisionIntent::FindElements;
    query.find_elements = KV::FindElementsParams {
        .contours = { .max_contours = 3 },
    };

    VisionAnalyzer analyzer(analyzer_config(query));
    const auto analysis = analyzer.analyze_vision(as_container(image), 0);
    ASSERT_TRUE(analysis.find_elements.has_value());

    const auto& contours = analysis.find_elements->contours;
    const auto& boxes = analysis.find_elements->boxes;
    EXPECT_EQ(contours.size(), 3U);

    for (size_t i = 1; i < contours.size(); ++i)
        EXPECT_LE(contours[i].area, contours[i - 1].area);

    EXPECT_FALSE(boxes.empty());
    for (const auto& c : contours) {
        ASSERT_GE(c.label_id, 1U);
        ASSERT_LE(c.label_id, boxes.size());
        EXPECT_EQ(boxes[c.label_id - 1].label_id, c.label_id);
    }
}

TEST_F(VisionAnalyzerTest, ShapeDescriptorsSeparateASquareFromAnElongatedBlob)
{
    constexpr uint32_t size = 64;
    const auto image = pattern_frame(size, [](uint32_t x, uint32_t y) {
        const bool square = x >= 5 && x < 15 && y >= 5 && y < 15;
        const bool elongated = x >= 20 && x < 50 && y >= 45 && y < 49;
        return (square || elongated) ? glm::vec4 { 1.0F } : glm::vec4 { 0.0F };
    });
    ASSERT_TRUE(image);

    KV::VisionQuery query;
    query.intents = KV::VisionIntent::FindElements;
    query.find_elements = KV::FindElementsParams {};

    VisionAnalyzer analyzer(analyzer_config(query));
    const auto analysis = analyzer.analyze_vision(as_container(image), 0);
    ASSERT_TRUE(analysis.find_elements.has_value());
    ASSERT_EQ(analysis.find_elements->boxes.size(), 2U);

    const auto& fe = *analysis.find_elements;
    ASSERT_EQ(fe.shapes.size(), 2U);
    ASSERT_EQ(fe.nearest_neighbor_distance_px.size(), 2U);

    const size_t square_idx = fe.shapes[0].aspect_ratio <= fe.shapes[1].aspect_ratio ? 0 : 1;
    const size_t elongated_idx = 1 - square_idx;

    EXPECT_NEAR(fe.shapes[square_idx].aspect_ratio, 1.0F, 0.3F);
    EXPECT_GT(fe.shapes[elongated_idx].aspect_ratio, 3.0F);
    EXPECT_GT(fe.shapes[square_idx].compactness, fe.shapes[elongated_idx].compactness);
    EXPECT_GT(fe.nearest_neighbor_distance_px[0], 0.0F);
    EXPECT_GT(fe.nearest_neighbor_distance_px[1], 0.0F);
}

TEST_F(VisionAnalyzerTest, TrackingAndMotionShareOneSequenceAndBothReport)
{
    constexpr uint32_t size = 256;
    constexpr float dx = 2.0F;
    constexpr float dy = 1.5F;

    const auto frame0 = wave_frame(size, 0.0F, 0.0F);
    const auto frame1 = wave_frame(size, dx, dy);
    ASSERT_TRUE(frame0 && frame1);

    KV::VisionQuery query = track_query();
    query.intents = KV::VisionIntent::TrackObjects | KV::VisionIntent::EstimateMotion;
    query.estimate_motion = KV::EstimateMotionParams {};

    VisionAnalyzer analyzer(analyzer_config(query));
    (void)analyzer.analyze_vision(as_container(frame0), 0);
    const auto analysis = analyzer.analyze_vision(as_container(frame1), 0);

    ASSERT_TRUE(analysis.track_objects.has_value());
    EXPECT_GE(analysis.track_objects->tracks.size(), 4U);

    const auto fsize = static_cast<float>(size);
    EXPECT_NEAR(analysis.track_objects->velocity.x * fsize, dx, 1.0F);
    EXPECT_NEAR(analysis.track_objects->velocity.y * fsize, dy, 1.0F);
    EXPECT_NE(analysis.estimate_motion, nullptr);
}

TEST_F(VisionAnalyzerTest, ContextFilterKeepsOnlyTracksInsideTheSubjectBounds)
{
    constexpr uint32_t size = 256;
    constexpr float dx = 2.0F;
    constexpr float dy = 1.0F;

    const auto frame0 = wave_frame(size, 0.0F, 0.0F);
    const auto frame1 = wave_frame(size, dx, dy);
    ASSERT_TRUE(frame0 && frame1);

    const auto query = track_query();

    VisionAnalyzer unfiltered(analyzer_config(query));
    (void)unfiltered.analyze_vision(as_container(frame0), 0);
    const auto all = unfiltered.analyze_vision(as_container(frame1), 0);
    ASSERT_TRUE(all.track_objects.has_value());
    ASSERT_FALSE(all.track_objects->tracks.empty());

    KV::VisionAnalysisContext context;
    context.track_objects = KV::TrackObjectsContext {
        .subject_bounds = KV::BoundingBox { .x = 0.0F, .y = 0.0F, .w = 0.5F, .h = 0.5F },
    };

    VisionAnalyzer filtered(analyzer_config(query));
    (void)filtered.analyze_vision(as_container(frame0), 0);
    edit_config<VisionAnalyzerConfig>(filtered, [&](auto& c) { c.context = context; });
    const auto some = filtered.analyze_vision(as_container(frame1), 0);
    ASSERT_TRUE(some.track_objects.has_value());

    const auto& bounds = *context.track_objects->subject_bounds;
    for (const auto& t : some.track_objects->tracks) {
        EXPECT_GE(t.position.x, bounds.x);
        EXPECT_LE(t.position.x, bounds.x + bounds.w);
        EXPECT_GE(t.position.y, bounds.y);
        EXPECT_LE(t.position.y, bounds.y + bounds.h);
    }
    EXPECT_LE(some.track_objects->tracks.size(), all.track_objects->tracks.size());
}

TEST_F(VisionAnalyzerTest, DetectFeaturesCentroidBoundsAndResponseMatchTheHostReduction)
{
    constexpr uint32_t size = 256;
    constexpr float tolerance = 0.01F;

    const auto frame = wave_frame(size, 0.0F, 0.0F);
    ASSERT_TRUE(frame);

    KV::VisionQuery query;
    query.intents = KV::VisionIntent::DetectFeatures;
    query.detect_features = KV::DetectFeaturesParams {
        .harris = { .sigma = 1.5F },
        .peaks = { .threshold = 0.05F, .nms_radius = 6 },
    };

    VisionAnalyzer analyzer(analyzer_config(query));
    const auto analysis = analyzer.analyze_vision(as_container(frame), 0);
    ASSERT_TRUE(analysis.detect_features.has_value());
    ASSERT_FALSE(analysis.detect_features->keypoints.empty());

    const auto& kp = analysis.detect_features->keypoints;
    glm::vec2 sum { 0.0F, 0.0F };
    glm::vec2 mn { 1.0F, 1.0F };
    glm::vec2 mx { 0.0F, 0.0F };
    float response_sum = 0.0F;
    for (const auto& k : kp) {
        sum += k.position;
        mn = glm::min(mn, k.position);
        mx = glm::max(mx, k.position);
        response_sum += k.response;
    }
    const glm::vec2 expected_centroid = sum / static_cast<float>(kp.size());
    const float expected_mean_response = response_sum / static_cast<float>(kp.size());

    const auto& df = *analysis.detect_features;
    EXPECT_NEAR(df.centroid.x, expected_centroid.x, tolerance);
    EXPECT_NEAR(df.centroid.y, expected_centroid.y, tolerance);
    EXPECT_NEAR(df.bounds.x, mn.x, tolerance);
    EXPECT_NEAR(df.bounds.y, mn.y, tolerance);
    EXPECT_NEAR(df.bounds.w, mx.x - mn.x, tolerance);
    EXPECT_NEAR(df.bounds.h, mx.y - mn.y, tolerance);
    EXPECT_NEAR(df.mean_response, expected_mean_response, tolerance);
}

TEST_F(VisionAnalyzerTest, AnalyzeOnAnImageFillsTheSameFieldsAsAContainer)
{
    constexpr uint32_t size = 256;

    const auto frame = wave_frame(size, 0.0F, 0.0F);
    ASSERT_TRUE(frame);

    KV::VisionQuery query;
    query.intents = KV::VisionIntent::DetectFeatures;
    query.detect_features = KV::DetectFeaturesParams {
        .harris = { .sigma = 1.5F },
        .peaks = { .threshold = 0.05F, .nms_radius = 6 },
    };

    VisionAnalyzer analyzer(analyzer_config(query));
    const auto analysis = analyzer.analyze_vision(frame);
    ASSERT_TRUE(analysis.detect_features.has_value());
    EXPECT_FALSE(analysis.detect_features->keypoints.empty());
}

TEST_F(VisionAnalyzerTest, MeasureAppearanceCountsEveryPixelAndReadsTheSeamGradient)
{
    constexpr uint32_t size = 64;
    const auto image = step_frame(size);
    ASSERT_TRUE(image);

    KV::VisionQuery query;
    query.intents = KV::VisionIntent::MeasureAppearance;
    query.measure_appearance = KV::MeasureAppearanceParams {};

    VisionAnalyzer analyzer(analyzer_config(query));
    const auto analysis = analyzer.analyze_vision(as_container(image), 0);
    ASSERT_TRUE(analysis.measure_appearance.has_value());

    const auto& appearance = *analysis.measure_appearance;
    uint64_t histogram_total = 0;
    for (const auto count : appearance.histogram)
        histogram_total += count;

    EXPECT_EQ(histogram_total, static_cast<uint64_t>(size) * size);
    EXPECT_NEAR(appearance.mean_brightness, 0.5F, 0.05F);
    EXPECT_GT(glm::length(appearance.gradient.max_val - appearance.gradient.min_val), 0.01F);
}

TEST_F(VisionAnalyzerTest, MotionActivityIsNearZeroForARepeatedFrameAndHigherForAShift)
{
    constexpr uint32_t size = 128;
    constexpr float dx = 6.0F;
    constexpr float dy = 4.0F;

    const auto frame0 = wave_frame(size, 0.0F, 0.0F);
    const auto frame1 = wave_frame(size, dx, dy);
    ASSERT_TRUE(frame0 && frame1);

    KV::VisionQuery query;
    query.intents = KV::VisionIntent::EstimateMotion;
    query.estimate_motion = KV::EstimateMotionParams {};

    VisionAnalyzer analyzer(analyzer_config(query));
    (void)analyzer.analyze_vision(as_container(frame0), 0);
    (void)analyzer.analyze_vision(as_container(frame0), 0);
    const auto duplicate = analyzer.analyze_vision(as_container(frame1), 0);
    const auto moved = analyzer.analyze_vision(as_container(frame1), 0);

    ASSERT_TRUE(duplicate.motion_activity.has_value());
    ASSERT_TRUE(moved.motion_activity.has_value());
    EXPECT_GT(*moved.motion_activity, *duplicate.motion_activity);
}

TEST_F(VisionAnalyzerTest, AppearanceAndEdgesAreReportedTogetherOnTheSameFrame)
{
    constexpr uint32_t size = 64;
    const auto image = step_frame(size);
    ASSERT_TRUE(image);

    KV::VisionQuery query;
    query.intents = KV::VisionIntent::DetectEdges | KV::VisionIntent::MeasureAppearance;
    query.detect_edges = KV::DetectEdgesParams { .canny = { .sigma = 1.4F, .low_threshold = 0.1F, .high_threshold = 0.3F } };
    query.measure_appearance = KV::MeasureAppearanceParams {};

    VisionAnalyzer analyzer(analyzer_config(query));
    const auto analysis = analyzer.analyze_vision(as_container(image), 0);
    ASSERT_TRUE(analysis.measure_appearance.has_value());
    ASSERT_NE(analysis.detect_edges, nullptr);

    uint64_t total = 0;
    for (const auto count : analysis.measure_appearance->histogram)
        total += count;
    EXPECT_EQ(total, static_cast<uint64_t>(size) * size);
    EXPECT_NEAR(analysis.measure_appearance->mean_brightness, 0.5F, 0.05F);

    const auto edge_pixels = read_normalised(analysis.detect_edges);
    float edge_peak = 0.0F;
    for (uint32_t x = 30; x <= 33; ++x)
        edge_peak = std::max(edge_peak, red_at(edge_pixels, x, 32, size));
    EXPECT_GT(edge_peak, 0.5F);
    EXPECT_LT(red_at(edge_pixels, 5, 32, size), 0.5F);
}

TEST_F(VisionAnalyzerTest, FindElementsAndTrackingInSeparateLanesBothComplete)
{
    const auto image = element_frame();
    ASSERT_TRUE(image);

    KV::VisionQuery query;
    query.intents = KV::VisionIntent::FindElements | KV::VisionIntent::TrackObjects;
    query.find_elements = KV::FindElementsParams {};
    query.track_objects = KV::TrackObjectsParams {};

    VisionAnalyzer analyzer(analyzer_config(query));
    const auto analysis = analyzer.analyze_vision(as_container(image), 0);
    ASSERT_TRUE(analysis.find_elements.has_value());
    EXPECT_EQ(analysis.find_elements->boxes.size(), 3U);
}

TEST_F(VisionAnalyzerTest, TrackingInsideARegionFollowsTheShiftAndStaysNearTheRegion)
{
    constexpr uint32_t size = 256;
    constexpr float dx = 3.0F;
    constexpr float dy = -2.0F;
    const KV::BoundingBox region { .x = 0.25F, .y = 0.25F, .w = 0.5F, .h = 0.5F };

    const auto frame0 = wave_frame(size, 0.0F, 0.0F);
    const auto frame1 = wave_frame(size, dx, dy);
    ASSERT_TRUE(frame0 && frame1);

    auto query = track_query();
    query.track_objects->region = region;

    VisionAnalyzer analyzer(analyzer_config(query));
    (void)analyzer.analyze_vision(as_container(frame0), 0);
    const auto analysis = analyzer.analyze_vision(as_container(frame1), 0);
    ASSERT_TRUE(analysis.track_objects.has_value());
    ASSERT_FALSE(analysis.track_objects->tracks.empty());

    const auto& to = *analysis.track_objects;
    const auto fsize = static_cast<float>(size);
    const float eps = 1.0F / fsize;
    const float moved_eps = (std::max(std::abs(dx), std::abs(dy)) + 1.0F) / fsize;
    const auto in_region = [&](const glm::vec2& p, float tolerance) {
        return p.x >= region.x - tolerance && p.x <= region.x + region.w + tolerance
            && p.y >= region.y - tolerance && p.y <= region.y + region.h + tolerance;
    };

    for (const auto& t : to.tracks) {
        EXPECT_TRUE(in_region(t.previous, eps));
        EXPECT_TRUE(in_region(t.position, moved_eps));
    }

    EXPECT_NEAR(to.velocity.x * fsize, dx, 1.0F);
    EXPECT_NEAR(to.velocity.y * fsize, dy, 1.0F);
    EXPECT_TRUE(in_region(to.centroid, moved_eps));
    EXPECT_TRUE(in_region({ to.bounds.x, to.bounds.y }, moved_eps));
    EXPECT_TRUE(in_region({ to.bounds.x + to.bounds.w, to.bounds.y + to.bounds.h }, moved_eps));
}

TEST_F(VisionAnalyzerTest, FeaturesInsideARegionStayInsideIt)
{
    constexpr uint32_t size = 256;
    const KV::BoundingBox region { .x = 0.5F, .y = 0.0F, .w = 0.5F, .h = 0.5F };

    const auto frame = wave_frame(size, 0.0F, 0.0F);
    ASSERT_TRUE(frame);

    KV::VisionQuery query;
    query.intents = KV::VisionIntent::DetectFeatures;
    query.detect_features = KV::DetectFeaturesParams {
        .harris = { .sigma = 1.5F },
        .peaks = { .threshold = 0.05F, .nms_radius = 6 },
        .region = region,
    };

    VisionAnalyzer analyzer(analyzer_config(query));
    const auto analysis = analyzer.analyze_vision(as_container(frame), 0);
    ASSERT_TRUE(analysis.detect_features.has_value());
    ASSERT_FALSE(analysis.detect_features->keypoints.empty());

    const auto& df = *analysis.detect_features;
    const float eps = 1.0F / static_cast<float>(size);
    const auto in_region = [&](const glm::vec2& p) {
        return p.x >= region.x - eps && p.x <= region.x + region.w + eps
            && p.y >= region.y - eps && p.y <= region.y + region.h + eps;
    };

    for (const auto& k : df.keypoints)
        EXPECT_TRUE(in_region(k.position));
    EXPECT_TRUE(in_region(df.centroid));
    EXPECT_TRUE(in_region({ df.bounds.x, df.bounds.y }));
    EXPECT_TRUE(in_region({ df.bounds.x + df.bounds.w, df.bounds.y + df.bounds.h }));
}

TEST_F(VisionAnalyzerTest, ChangingTheRegionKeepsTheTracksAlreadyFollowed)
{
    constexpr uint32_t size = 256;
    constexpr float dx = 3.0F;
    constexpr float dy = -2.0F;
    const KV::BoundingBox region_a { .x = 0.0F, .y = 0.0F, .w = 0.5F, .h = 0.5F };
    const KV::BoundingBox region_b { .x = 0.5F, .y = 0.5F, .w = 0.5F, .h = 0.5F };

    const auto frame0 = wave_frame(size, 0.0F, 0.0F);
    const auto frame1 = wave_frame(size, dx, dy);
    const auto frame2 = wave_frame(size, 2.0F * dx, 2.0F * dy);
    ASSERT_TRUE(frame0 && frame1 && frame2);

    const auto make_query = [](const KV::BoundingBox& region) {
        auto query = track_query();
        query.track_objects->region = region;
        return query;
    };

    VisionAnalyzer analyzer(analyzer_config(make_query(region_a)));
    (void)analyzer.analyze_vision(as_container(frame0), 0);
    (void)analyzer.analyze_vision(as_container(frame1), 0);

    edit_config<VisionAnalyzerConfig>(analyzer, [&](auto& c) {
        c.query = make_query(region_b);
        c.method = c.query.intents;
    });
    const auto after = analyzer.analyze_vision(as_container(frame2), 0);
    ASSERT_TRUE(after.track_objects.has_value());
    ASSERT_FALSE(after.track_objects->tracks.empty());

    const auto& to = *after.track_objects;
    const auto fsize = static_cast<float>(size);
    EXPECT_NEAR(to.velocity.x * fsize, dx, 1.0F);
    EXPECT_NEAR(to.velocity.y * fsize, dy, 1.0F);
}

TEST_F(VisionAnalyzerTest, SeededRegionFindsTheSameFeaturesAsStandalone)
{
    constexpr uint32_t size = 256;
    const KV::BoundingBox region { .x = 0.25F, .y = 0.25F, .w = 0.5F, .h = 0.5F };

    const auto frame = wave_frame(size, 0.0F, 0.0F);
    ASSERT_TRUE(frame);

    const auto harris = KV::HarrisParams { .sigma = 1.5F };
    const auto peaks = KV::ExtractPeaksParams { .threshold = 0.05F, .nms_radius = 6 };

    KV::VisionQuery alone;
    alone.intents = KV::VisionIntent::DetectFeatures;
    alone.detect_features = KV::DetectFeaturesParams { .harris = harris, .peaks = peaks, .region = region };
    VisionAnalyzer alone_analyzer(analyzer_config(alone));
    const auto standalone = alone_analyzer.analyze_vision(as_container(frame), 0);

    KV::VisionQuery both;
    both.intents = KV::VisionIntent::TrackObjects | KV::VisionIntent::DetectFeatures;
    both.track_objects = KV::TrackObjectsParams { .harris = harris, .peaks = peaks, .region = region };
    both.detect_features = KV::DetectFeaturesParams { .harris = harris, .peaks = peaks, .region = region };

    VisionAnalyzer both_analyzer(analyzer_config(both));
    const auto combined = both_analyzer.analyze_vision(as_container(frame), 0);

    ASSERT_TRUE(standalone.detect_features.has_value());
    ASSERT_TRUE(combined.detect_features.has_value());
    const auto a = standalone.detect_features->keypoints.size();
    const auto b = combined.detect_features->keypoints.size();
    EXPECT_GT(a, 0U);
    EXPECT_EQ(a, b);
}

TEST_F(VisionAnalyzerTest, RegionContextNarrowsTheTracksToTheSubject)
{
    constexpr uint32_t size = 256;
    constexpr float dx = 3.0F;
    constexpr float dy = -2.0F;
    const KV::BoundingBox region { .x = 0.25F, .y = 0.25F, .w = 0.5F, .h = 0.5F };
    const KV::BoundingBox subject { .x = 0.25F, .y = 0.25F, .w = 0.25F, .h = 0.25F };

    const auto frame0 = wave_frame(size, 0.0F, 0.0F);
    const auto frame1 = wave_frame(size, dx, dy);
    ASSERT_TRUE(frame0 && frame1);

    auto query = track_query();
    query.track_objects->region = region;

    VisionAnalyzer unfiltered(analyzer_config(query));
    (void)unfiltered.analyze_vision(as_container(frame0), 0);
    const auto all = unfiltered.analyze_vision(as_container(frame1), 0);

    KV::VisionAnalysisContext context;
    context.track_objects = KV::TrackObjectsContext { .subject_bounds = subject };

    VisionAnalyzer filtered(analyzer_config(query, context));
    (void)filtered.analyze_vision(as_container(frame0), 0);
    const auto some = filtered.analyze_vision(as_container(frame1), 0);

    ASSERT_TRUE(all.track_objects.has_value());
    ASSERT_TRUE(some.track_objects.has_value());
    ASSERT_FALSE(all.track_objects->tracks.empty());

    const float eps = 1.0F / static_cast<float>(size);
    for (const auto& t : some.track_objects->tracks) {
        EXPECT_GE(t.position.x, subject.x - eps);
        EXPECT_LE(t.position.x, subject.x + subject.w + eps);
        EXPECT_GE(t.position.y, subject.y - eps);
        EXPECT_LE(t.position.y, subject.y + subject.h + eps);
    }

    EXPECT_GT(some.track_objects->tracks.size(), 0U);
    EXPECT_LT(some.track_objects->tracks.size(), all.track_objects->tracks.size());
}

TEST_F(VisionAnalyzerTest, FlowInARegionStillCoversTheWholeFrame)
{
    constexpr uint32_t size = 256;
    const KV::BoundingBox region { .x = 0.25F, .y = 0.25F, .w = 0.5F, .h = 0.5F };

    const auto frame0 = wave_frame(size, 0.0F, 0.0F);
    const auto frame1 = wave_frame(size, 2.0F, 1.0F);
    ASSERT_TRUE(frame0 && frame1);

    KV::VisionQuery query;
    query.intents = KV::VisionIntent::TrackObjects | KV::VisionIntent::EstimateMotion;
    query.track_objects = KV::TrackObjectsParams {
        .harris = { .sigma = 1.5F },
        .peaks = { .threshold = 0.05F, .nms_radius = 6 },
        .track = { .error_threshold = 0.2F },
        .region = region,
    };
    query.estimate_motion = KV::EstimateMotionParams {};

    VisionAnalyzer analyzer(analyzer_config(query));
    (void)analyzer.analyze_vision(as_container(frame0), 0);
    const auto analysis = analyzer.analyze_vision(as_container(frame1), 0);

    ASSERT_NE(analysis.estimate_motion, nullptr);
    EXPECT_EQ(analysis.estimate_motion->get_width(), size);
    EXPECT_EQ(analysis.estimate_motion->get_height(), size);

    ASSERT_TRUE(analysis.track_objects.has_value());
    ASSERT_FALSE(analysis.track_objects->tracks.empty());

    const float margin = 8.0F / static_cast<float>(size);
    for (const auto& t : analysis.track_objects->tracks) {
        EXPECT_GE(t.position.x, region.x - margin);
        EXPECT_LE(t.position.x, region.x + region.w + margin);
        EXPECT_GE(t.position.y, region.y - margin);
        EXPECT_LE(t.position.y, region.y + region.h + margin);
    }
}

TEST_F(VisionAnalyzerTest, SegmentationBandIsolatesTheSquareItSelects)
{
    const auto image = red_blue_frame();
    ASSERT_TRUE(image);

    const auto run_band = [&](float hue_lo, float hue_hi) -> std::optional<KV::BoundingBox> {
        KV::VisionQuery query;
        query.intents = KV::VisionIntent::FindElements;
        query.find_elements = KV::FindElementsParams {
            .segmentation = {
                .space = KV::SegmentationSpace::Hsv,
                .method = KV::ThresholdParams {
                    .value = 0.0F,
                    .channels = KV::ChannelMask::Ch0 | KV::ChannelMask::Ch1 | KV::ChannelMask::Ch2,
                    .bands = { { { .lo = hue_lo, .hi = hue_hi }, { .lo = 0.5F, .hi = 1.0F }, { .lo = 0.5F, .hi = 1.0F } } },
                },
            },
        };
        VisionAnalyzer analyzer(analyzer_config(query));
        const auto analysis = analyzer.analyze_vision(as_container(image), 0);
        if (!analysis.find_elements || analysis.find_elements->contours.size() != 1 || analysis.find_elements->boxes.size() != 1)
            return std::nullopt;
        return analysis.find_elements->boxes.front();
    };

    const auto matches = [](const std::optional<KV::BoundingBox>& box, float px) {
        if (!box)
            return false;
        const auto fs = 32.0F;
        return std::abs(box->x * fs - px) <= 1.0F && std::abs(box->y * fs - px) <= 1.0F
            && std::abs(box->w * fs - 8.0F) <= 1.0F && std::abs(box->h * fs - 8.0F) <= 1.0F;
    };

    EXPECT_TRUE(matches(run_band(0.95F, 0.05F), 4.0F));
    EXPECT_TRUE(matches(run_band(0.6F, 0.75F), 20.0F));
}

TEST_F(VisionAnalyzerTest, RgbaSegmentationFindsOnlyTheRedSquare)
{
    constexpr uint32_t size = 32;
    const auto image = pattern_frame(size, [](uint32_t x, uint32_t y) {
        if (x >= 4 && x < 12 && y >= 4 && y < 12)
            return glm::vec4 { 1.0F, 0.0F, 0.0F, 1.0F };
        if (x >= 20 && x < 28 && y >= 20 && y < 28)
            return glm::vec4 { 0.0F, 1.0F, 0.0F, 1.0F };
        return glm::vec4 { 0.0F, 0.0F, 0.0F, 1.0F };
    });
    ASSERT_TRUE(image);

    KV::VisionQuery query;
    query.intents = KV::VisionIntent::FindElements;
    query.find_elements = KV::FindElementsParams {
        .segmentation = {
            .space = KV::SegmentationSpace::Rgba,
            .method = KV::ThresholdParams {
                .value = 0.0F,
                .channels = KV::ChannelMask::Ch0 | KV::ChannelMask::Ch1 | KV::ChannelMask::Ch2,
                .bands = { { { .lo = 0.5F, .hi = 1.0F }, { .lo = 0.0F, .hi = 0.2F }, { .lo = 0.0F, .hi = 0.2F } } },
            },
        },
    };

    VisionAnalyzer analyzer(analyzer_config(query));
    const auto analysis = analyzer.analyze_vision(as_container(image), 0);
    ASSERT_TRUE(analysis.find_elements.has_value());
    ASSERT_EQ(analysis.find_elements->boxes.size(), 1U);

    const auto& box = analysis.find_elements->boxes.front();
    const auto fs = static_cast<float>(size);
    EXPECT_NEAR(box.x * fs, 4.0F, 1.0F);
    EXPECT_NEAR(box.y * fs, 4.0F, 1.0F);
}

TEST_F(VisionAnalyzerTest, AdvanceCompletesOneLanePerCallAndFillsTheSameResultAsAnalyze)
{
    constexpr uint32_t blob_size = 16;
    const auto blobs = pattern_frame(blob_size, [](uint32_t x, uint32_t y) {
        const bool a = x >= 1 && x <= 3 && y >= 1 && y <= 3;
        const bool b = x >= 8 && x <= 10 && y >= 8 && y <= 10;
        const bool c = x >= 13 && x <= 14 && y >= 13 && y <= 14;
        return (a || b || c) ? glm::vec4 { 1.0F } : glm::vec4 { 0.0F, 0.0F, 0.0F, 1.0F };
    });
    const auto black = pattern_frame(blob_size, [](uint32_t, uint32_t) { return glm::vec4 { 0.0F, 0.0F, 0.0F, 1.0F }; });
    ASSERT_TRUE(blobs && black);

    KV::VisionQuery query;
    query.intents = KV::VisionIntent::FindElements | KV::VisionIntent::MeasureAppearance;
    query.find_elements = KV::FindElementsParams {};
    query.measure_appearance = KV::MeasureAppearanceParams {};

    VisionAnalyzer stepped(analyzer_config(query));
    const size_t lane_count = Kinesis::Vision::resolve(query).size();
    size_t lane_calls = 1;
    auto result = stepped.advance(blobs);
    while (!result && lane_calls < 16) {
        result = stepped.advance(black);
        ++lane_calls;
    }

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(lane_calls, lane_count);
    ASSERT_TRUE(result->find_elements.has_value());
    EXPECT_EQ(result->find_elements->boxes.size(), 3U);
    ASSERT_TRUE(result->measure_appearance.has_value());

    uint64_t histogram_total = 0;
    for (const auto bin : result->measure_appearance->histogram)
        histogram_total += bin;
    EXPECT_EQ(histogram_total, static_cast<uint64_t>(blob_size) * blob_size);
}

TEST_F(VisionAnalyzerTest, AdvanceTrackingOnTheSecondFrameMatchesTheSynchronousResult)
{
    constexpr uint32_t wave_size = 256;
    const auto frame0 = wave_frame(wave_size, 0.0F, 0.0F);
    const auto frame1 = wave_frame(wave_size, 3.0F, -2.0F);
    ASSERT_TRUE(frame0 && frame1);

    VisionAnalyzer stepped(analyzer_config(track_query()));
    const auto run_to_end = [&stepped](const ImageHandle& frame, size_t& calls) {
        calls = 0;
        std::optional<KV::VisionAnalysis> result;
        while (!result && calls < 1000) {
            result = stepped.advance(frame);
            ++calls;
        }
        return result;
    };

    size_t first_calls = 0;
    size_t second_calls = 0;
    (void)run_to_end(frame0, first_calls);
    const auto stepped_second = run_to_end(frame1, second_calls);
    ASSERT_TRUE(stepped_second.has_value());

    VisionAnalyzer synchronous(analyzer_config(track_query()));
    (void)synchronous.analyze_vision(frame0);
    const auto sync_second = synchronous.analyze_vision(frame1);

    ASSERT_TRUE(stepped_second->track_objects.has_value());
    ASSERT_TRUE(sync_second.track_objects.has_value());
    const uint32_t stepped_tracked = stepped_second->track_objects->tracked_count;
    EXPECT_GT(stepped_tracked, 0U);
    EXPECT_EQ(stepped_tracked, sync_second.track_objects->tracked_count);
    EXPECT_GE(second_calls, 2U);
}

namespace {

KV::VisionQuery feature_query()
{
    KV::VisionQuery query;
    query.intents = KV::VisionIntent::DetectFeatures;
    query.detect_features = KV::DetectFeaturesParams {
        .harris = { .sigma = 1.5F },
        .peaks = { .threshold = 0.05F, .nms_radius = 6 },
    };
    return query;
}

std::shared_ptr<Kakshya::SignalSourceContainer> two_layer_stack(uint32_t size, const ImageHandle& layer0, const ImageHandle& layer1)
{
    auto stack = std::make_shared<Kakshya::TextureContainer>(size, size, Portal::Graphics::ImageFormat::RGBA32F, 2);
    stack->from_image(layer0, 0);
    stack->from_image(layer1, 1);
    return stack;
}

} // namespace

TEST_F(VisionAnalyzerTest, OperationWithAnImageFacetFillsTheAnalysisFacet)
{
    const auto frame = wave_frame(256, 0.0F, 0.0F);
    ASSERT_TRUE(frame);

    VisionAnalyzer analyzer(analyzer_config(feature_query()));
    Yantra::ContainerIO input;
    input.facets.set(KV::vision_frame(), frame);

    const auto output = analyzer.apply_operation(input);
    const auto* analysis = output.facets.get<KV::VisionAnalysis>();
    ASSERT_NE(analysis, nullptr);
    ASSERT_TRUE(analysis->detect_features.has_value());
    EXPECT_FALSE(analysis->detect_features->keypoints.empty());
}

TEST_F(VisionAnalyzerTest, OperationWithAContainerAnalysesTheConfiguredFrameIndex)
{
    constexpr uint32_t size = 256;
    const auto black = pattern_frame(size, [](uint32_t, uint32_t) { return glm::vec4 { 0.0F, 0.0F, 0.0F, 1.0F }; });
    const auto wave = wave_frame(size, 0.0F, 0.0F);
    ASSERT_TRUE(black && wave);

    const auto stack = two_layer_stack(size, black, wave);

    VisionAnalyzer analyzer(analyzer_config(feature_query()));
    edit_config<Yantra::VisionAnalyzerConfig>(analyzer, [](auto& c) { c.frame_index = 1; });

    const auto output = analyzer.apply_operation(Yantra::ContainerIO(stack));
    const auto* analysis = output.facets.get<KV::VisionAnalysis>();
    ASSERT_NE(analysis, nullptr);
    ASSERT_TRUE(analysis->layer.has_value());
    EXPECT_EQ(*analysis->layer, 1U);
    ASSERT_TRUE(analysis->detect_features.has_value());
    EXPECT_FALSE(analysis->detect_features->keypoints.empty());
}

TEST_F(VisionAnalyzerTest, AnalysisOfATextureLayerIsTaggedWithItsIndex)
{
    constexpr uint32_t size = 256;
    const auto black = pattern_frame(size, [](uint32_t, uint32_t) { return glm::vec4 { 0.0F, 0.0F, 0.0F, 1.0F }; });
    const auto wave = wave_frame(size, 0.0F, 0.0F);
    ASSERT_TRUE(black && wave);

    const auto stack = two_layer_stack(size, black, wave);

    VisionAnalyzer analyzer(analyzer_config(feature_query()));
    const auto blank = analyzer.analyze_vision(stack, 0);
    EXPECT_TRUE(blank.layer.has_value());
    EXPECT_EQ(blank.layer.value_or(99U), 0U);
    EXPECT_TRUE(!blank.detect_features || blank.detect_features->keypoints.empty());

    const auto textured = analyzer.analyze_vision(stack, 1);
    ASSERT_TRUE(textured.layer.has_value());
    EXPECT_EQ(*textured.layer, 1U);
    ASSERT_TRUE(textured.detect_features.has_value());
    EXPECT_FALSE(textured.detect_features->keypoints.empty());
}

TEST_F(VisionAnalyzerTest, ResetDropsTheTrackingStateSoTheNextFrameSeedsAgain)
{
    constexpr uint32_t size = 256;
    const auto frame0 = wave_frame(size, 0.0F, 0.0F);
    const auto frame1 = wave_frame(size, 3.0F, -2.0F);
    ASSERT_TRUE(frame0 && frame1);

    VisionAnalyzer analyzer(analyzer_config(track_query()));
    (void)analyzer.analyze_vision(as_container(frame0), 0);
    const auto before = analyzer.analyze_vision(as_container(frame1), 0);
    ASSERT_TRUE(before.track_objects.has_value());
    ASSERT_GT(before.track_objects->tracked_count, 0U);

    analyzer.reset();

    const auto after = analyzer.analyze_vision(as_container(frame1), 0);
    EXPECT_TRUE(!after.track_objects || after.track_objects->tracks.empty());
}

TEST_F(VisionAnalyzerTest, ResultsHoldOneEntryPerLaneAndTheExecutorAppearsAfterTheFirstCall)
{
    const auto image = element_frame();
    ASSERT_TRUE(image);

    KV::VisionQuery query;
    query.intents = KV::VisionIntent::FindElements | KV::VisionIntent::MeasureAppearance;
    query.find_elements = KV::FindElementsParams {};
    query.measure_appearance = KV::MeasureAppearanceParams {};
    const size_t lane_count = Kinesis::Vision::resolve(query).size();

    VisionAnalyzer analyzer(analyzer_config(query));
    EXPECT_EQ(analyzer.get_executor(), nullptr);

    (void)analyzer.analyze_vision(as_container(image), 0);

    EXPECT_NE(analyzer.get_executor(), nullptr);
    EXPECT_EQ(analyzer.get_results().size(), lane_count);
    const auto shared = analyzer.get_shared_results();
    ASSERT_NE(shared, nullptr);
    EXPECT_EQ(shared->size(), lane_count);
}

TEST_F(VisionAnalyzerTest, SubjectTrackIdKeepsOnlyThatTrack)
{
    constexpr uint32_t size = 256;
    const auto frame0 = wave_frame(size, 0.0F, 0.0F);
    const auto frame1 = wave_frame(size, 3.0F, -2.0F);
    ASSERT_TRUE(frame0 && frame1);

    const auto query = track_query();

    VisionAnalyzer unfiltered(analyzer_config(query));
    (void)unfiltered.analyze_vision(as_container(frame0), 0);
    const auto all = unfiltered.analyze_vision(as_container(frame1), 0);
    ASSERT_TRUE(all.track_objects.has_value());
    ASSERT_GE(all.track_objects->tracks.size(), 2U);

    const uint32_t subject = all.track_objects->tracks.front().id;

    KV::VisionAnalysisContext context;
    context.track_objects = KV::TrackObjectsContext { .subject_track_id = subject };

    VisionAnalyzer filtered(analyzer_config(query, context));
    (void)filtered.analyze_vision(as_container(frame0), 0);
    const auto one = filtered.analyze_vision(as_container(frame1), 0);
    ASSERT_TRUE(one.track_objects.has_value());

    ASSERT_EQ(one.track_objects->tracks.size(), 1U);
    EXPECT_EQ(one.track_objects->tracks.front().id, subject);
}

TEST_F(VisionAnalyzerTest, DetectFeaturesBoundsKeepsOnlyKeypointsInsideThem)
{
    constexpr uint32_t size = 256;
    const KV::BoundingBox quadrant { .x = 0.0F, .y = 0.0F, .w = 0.5F, .h = 0.5F };

    const auto frame = wave_frame(size, 0.0F, 0.0F);
    ASSERT_TRUE(frame);

    VisionAnalyzer unfiltered(analyzer_config(feature_query()));
    const auto all = unfiltered.analyze_vision(as_container(frame), 0);
    ASSERT_TRUE(all.detect_features.has_value());
    ASSERT_FALSE(all.detect_features->keypoints.empty());

    const bool some_outside = std::ranges::any_of(all.detect_features->keypoints, [&](const auto& k) {
        return k.position.x > quadrant.x + quadrant.w || k.position.y > quadrant.y + quadrant.h;
    });
    ASSERT_TRUE(some_outside);

    KV::VisionAnalysisContext context;
    context.detect_features_bounds = quadrant;

    VisionAnalyzer filtered(analyzer_config(feature_query(), context));
    const auto some = filtered.analyze_vision(as_container(frame), 0);
    ASSERT_TRUE(some.detect_features.has_value());
    ASSERT_FALSE(some.detect_features->keypoints.empty());

    for (const auto& k : some.detect_features->keypoints) {
        EXPECT_GE(k.position.x, quadrant.x);
        EXPECT_LE(k.position.x, quadrant.x + quadrant.w);
        EXPECT_GE(k.position.y, quadrant.y);
        EXPECT_LE(k.position.y, quadrant.y + quadrant.h);
    }
    EXPECT_LT(some.detect_features->keypoints.size(), all.detect_features->keypoints.size());
}

TEST_F(VisionAnalyzerTest, NullImageGivesAnEmptyAnalysis)
{
    VisionAnalyzer analyzer(analyzer_config(feature_query()));
    const auto analysis = analyzer.analyze_vision(ImageHandle {});

    EXPECT_FALSE(analysis.detect_features.has_value());
    EXPECT_FALSE(analysis.layer.has_value());
}

TEST_F(VisionAnalyzerTest, AnalyzerHoldsANullHandleUntilItIsFilled)
{
    const auto frame = wave_frame(256, 0.0F, 0.0F);
    ASSERT_TRUE(frame);

    VisionAnalyzer analyzer(analyzer_config(feature_query()));

    ImageHandle held;
    EXPECT_FALSE(analyzer.analyze_vision(held).detect_features.has_value());

    held = frame;
    const auto analysis = analyzer.analyze_vision(held);
    ASSERT_TRUE(analysis.detect_features.has_value());
    EXPECT_FALSE(analysis.detect_features->keypoints.empty());
}

} // namespace MayaFlux::Test::Vision
