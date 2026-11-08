#include "gtest/gtest.h"

#include "../../support/vision_test_support.hpp"
#include "MayaFlux/Yantra/Extractors/VisionExtractor.hpp"

namespace MayaFlux::Test::Vision {

namespace KV = MayaFlux::Kinesis::Vision;
using Yantra::VisionExtraction;
using Yantra::VisionExtractMode;
using Yantra::VisionExtractor;
using Yantra::VisionExtractorConfig;
using Yantra::VisionGpuExecutor;

namespace {

    using Fixture = VisionGpuFixture;

    bool color_close(glm::vec4 a, glm::vec4 b, float tolerance)
    {
        return std::abs(a.r - b.r) < tolerance && std::abs(a.g - b.g) < tolerance
            && std::abs(a.b - b.b) < tolerance && std::abs(a.a - b.a) < tolerance;
    }

    glm::vec4 read_pixel(const ImageHandle& image, uint32_t x, uint32_t y, uint32_t w)
    {
        Kakshya::TextureContainer readback(image, Portal::Graphics::ImageFormat::RGBA8);
        const auto pixels = readback.as_normalised_float(0);
        const size_t idx = (static_cast<size_t>(y) * w + x) * 4;
        if (idx + 3 >= pixels.size())
            return {};
        return { pixels[idx + 0], pixels[idx + 1], pixels[idx + 2], pixels[idx + 3] };
    }

    VisionExtractorConfig extractor_config(VisionExtractMode mode)
    {
        return VisionExtractorConfig { .method = mode };
    }

    struct ThreeElementScene {
        ImageHandle frame;
        KV::VisionAnalysis analysis;
        std::array<glm::vec4, 3> colors {
            { { 1.0F, 0.0F, 0.0F, 1.0F }, { 0.0F, 1.0F, 0.0F, 1.0F }, { 0.0F, 0.0F, 1.0F, 1.0F } }
        };
    };

    ThreeElementScene make_three_element_scene()
    {
        constexpr uint32_t size = 64;
        ThreeElementScene scene;

        const std::array<std::array<uint32_t, 4>, 3> rects { {
            { 8, 8, 16, 16 },
            { 32, 8, 48, 24 },
            { 16, 32, 48, 48 },
        } };

        scene.frame = pattern_frame(size, [&](uint32_t x, uint32_t y) {
            for (size_t i = 0; i < rects.size(); ++i) {
                if (x >= rects[i][0] && x < rects[i][2] && y >= rects[i][1] && y < rects[i][3])
                    return scene.colors[i];
            }
            return glm::vec4 { 0.0F, 0.0F, 0.0F, 1.0F };
        });

        KV::FindElementsAnalysis elements;
        for (size_t i = 0; i < rects.size(); ++i) {
            const auto fs = static_cast<float>(size);
            elements.boxes.push_back({
                .x = static_cast<float>(rects[i][0]) / fs,
                .y = static_cast<float>(rects[i][1]) / fs,
                .w = static_cast<float>(rects[i][2] - rects[i][0]) / fs,
                .h = static_cast<float>(rects[i][3] - rects[i][1]) / fs,
                .label_id = static_cast<uint32_t>(i + 1),
            });
        }
        scene.analysis.find_elements = std::move(elements);
        return scene;
    }

    std::shared_ptr<Kakshya::SignalSourceContainer> layer_stack(uint32_t size, const ImageHandle& layer0, const ImageHandle& layer1)
    {
        auto stack = std::make_shared<Kakshya::TextureContainer>(size, size, Portal::Graphics::ImageFormat::RGBA32F, 2);
        stack->from_image(layer0, 0);
        stack->from_image(layer1, 1);
        return stack;
    }

} // namespace

class VisionExtractorTest : public Fixture { };

TEST_F(VisionExtractorTest, CropReturnsTheFocusedRegionAtItsCentre)
{
    constexpr uint32_t size = 256;
    constexpr uint32_t out_size = 32;
    const KV::BoundingBox region { .x = 0.25F, .y = 0.25F, .w = 0.25F, .h = 0.25F };
    const glm::vec4 red { 1.0F, 0.0F, 0.0F, 1.0F };

    const auto frame = solid_region_frame(size, region, red);
    ASSERT_TRUE(frame);

    KV::VisionAnalysis analysis;
    analysis.find_elements = KV::FindElementsAnalysis { .boxes = { region } };

    VisionExtractor extractor;
    const auto cropped = extractor.crop(frame, analysis, out_size, out_size);
    ASSERT_TRUE(cropped);

    EXPECT_TRUE(color_close(read_pixel(cropped, out_size / 2, out_size / 2, out_size), red, 0.05F));
}

TEST_F(VisionExtractorTest, SampleReportsMeanMinAndMaxOfTheRegion)
{
    constexpr uint32_t size = 256;
    const KV::BoundingBox region { .x = 0.25F, .y = 0.25F, .w = 0.25F, .h = 0.25F };
    const glm::vec4 blue { 0.0F, 0.0F, 1.0F, 1.0F };

    const auto frame = solid_region_frame(size, region, blue);
    ASSERT_TRUE(frame);

    KV::VisionAnalysis analysis;
    analysis.find_elements = KV::FindElementsAnalysis { .boxes = { region } };

    VisionExtractor extractor;
    const auto result = extractor.sample(frame, analysis, size, size);

    EXPECT_TRUE(color_close(result.mean, blue, 0.05F));
    EXPECT_TRUE(color_close(result.min_val, blue, 0.05F));
    EXPECT_TRUE(color_close(result.max_val, blue, 0.05F));
}

TEST_F(VisionExtractorTest, PatchesHoldOneTilePerKeypointInOrder)
{
    constexpr uint32_t size = 256;
    constexpr uint32_t patch = 16;
    const std::array<glm::vec2, 3> centers { { { 0.2F, 0.2F }, { 0.5F, 0.5F }, { 0.8F, 0.8F } } };
    const std::array<glm::vec4, 3> colors { { { 1.0F, 0.0F, 0.0F, 1.0F }, { 0.0F, 1.0F, 0.0F, 1.0F }, { 0.0F, 0.0F, 1.0F, 1.0F } } };

    const auto frame = pattern_frame(size, [&](uint32_t x, uint32_t y) -> glm::vec4 {
        for (size_t i = 0; i < centers.size(); ++i) {
            const auto cx = static_cast<uint32_t>(centers[i].x * size);
            const auto cy = static_cast<uint32_t>(centers[i].y * size);
            if (x >= cx - patch / 2 && x < cx + patch / 2 && y >= cy - patch / 2 && y < cy + patch / 2)
                return colors[i];
        }
        return glm::vec4 { 0.0F };
    });
    ASSERT_TRUE(frame);

    std::vector<KV::Keypoint> keypoints;
    for (const auto& c : centers)
        keypoints.push_back({ .position = c, .response = 1.0F, .scale = 1.0F, .angle = 0.0F });

    KV::VisionAnalysis analysis;
    analysis.detect_features = KV::DetectFeaturesAnalysis { .keypoints = keypoints };

    VisionExtractor extractor;
    const auto atlas = extractor.patches(frame, analysis, patch, patch, size, size);
    ASSERT_TRUE(atlas);

    for (size_t i = 0; i < centers.size(); ++i) {
        const auto px = static_cast<uint32_t>(i) * patch + patch / 2;
        EXPECT_TRUE(color_close(read_pixel(atlas, px, patch / 2, patch * static_cast<uint32_t>(centers.size())), colors[i], 0.05F))
            << "patch " << i;
    }
}

TEST_F(VisionExtractorTest, MaskKeepsOnlyTheTargetLabelsSilhouette)
{
    constexpr uint32_t W = 16;
    constexpr uint32_t H = 16;

    const auto image = pattern_frame(W, [](uint32_t x, uint32_t y) -> glm::vec4 {
        const bool square = x >= 1 && x <= 3 && y >= 1 && y <= 3;
        const bool block = x >= 8 && x <= 10 && y >= 8 && y <= 10;
        return square || block ? glm::vec4 { 1.0F } : glm::vec4 { 0.0F };
    });
    ASSERT_TRUE(image);

    KV::VisionQuery query;
    query.intents = KV::VisionIntent::FindElements;
    query.find_elements = KV::FindElementsParams {
        .components = { .export_label_buffer = true },
    };

    const auto sequences = KV::resolve(query);
    ASSERT_EQ(sequences.size(), 1U);

    VisionGpuExecutor executor;
    const auto result = executor.run(sequences[0].sequence, image, W, H);

    const auto* contours = std::get_if<std::vector<KV::Contour>>(&result.structured);
    ASSERT_NE(contours, nullptr);
    ASSERT_FALSE(contours->empty());

    KV::VisionAnalysis analysis;
    analysis.find_elements = KV::FindElementsAnalysis { .contours = *contours };

    VisionExtractor extractor;
    const auto masked = extractor.mask(executor, image, analysis, W, H);
    ASSERT_TRUE(masked);

    Kakshya::TextureContainer readback(masked, Portal::Graphics::ImageFormat::RGBA32F);
    const auto pixels = readback.as_normalised_float(0);

    const auto alpha_at = [&](uint32_t x, uint32_t y) {
        return pixels[(static_cast<size_t>(y) * W + x) * 4 + 3];
    };

    EXPECT_GT(alpha_at(2, 2), 0.5F);
    EXPECT_LT(alpha_at(9, 9), 0.5F);

    uint32_t opaque = 0;
    for (uint32_t y = 0; y < H; ++y) {
        for (uint32_t x = 0; x < W; ++x) {
            if (alpha_at(x, y) > 0.5F)
                ++opaque;
        }
    }
    EXPECT_EQ(opaque, 9U);
}

TEST_F(VisionExtractorTest, CropsLaysEveryBoxSideBySideInBoxOrder)
{
    auto scene = make_three_element_scene();
    ASSERT_TRUE(scene.frame);

    constexpr uint32_t tile = 8;
    VisionExtractor extractor;
    const auto atlas = extractor.crops(scene.frame, scene.analysis, tile, tile);
    ASSERT_TRUE(atlas);
    EXPECT_EQ(atlas->get_width(), tile * 3);

    for (uint32_t i = 0; i < 3; ++i) {
        EXPECT_TRUE(color_close(read_pixel(atlas, i * tile + tile / 2, tile / 2, tile * 3), scene.colors[i], 0.05F))
            << "tile " << i;
    }
}

TEST_F(VisionExtractorTest, SampleRegionsReportsEachBoxSeparately)
{
    auto scene = make_three_element_scene();
    ASSERT_TRUE(scene.frame);

    VisionExtractor extractor;
    const auto samples = extractor.sample_regions(scene.frame, scene.analysis, 64, 64);
    ASSERT_EQ(samples.size(), 3U);

    for (size_t i = 0; i < 3; ++i) {
        EXPECT_TRUE(color_close(samples[i].mean, scene.colors[i], 0.05F)) << "region " << i;
        EXPECT_TRUE(color_close(samples[i].min_val, scene.colors[i], 0.05F)) << "region " << i;
        EXPECT_TRUE(color_close(samples[i].max_val, scene.colors[i], 0.05F)) << "region " << i;
    }
}

TEST_F(VisionExtractorTest, SamplePointsReadsTheWindowUnderEachKeypoint)
{
    auto scene = make_three_element_scene();
    ASSERT_TRUE(scene.frame);

    KV::VisionAnalysis analysis;
    analysis.detect_features = KV::DetectFeaturesAnalysis {
        .keypoints = {
            { .position = { 12.0F / 64.0F, 12.0F / 64.0F }, .response = 1.0F },
            { .position = { 40.0F / 64.0F, 16.0F / 64.0F }, .response = 1.0F },
            { .position = { 4.0F / 64.0F, 60.0F / 64.0F }, .response = 1.0F },
        },
    };

    VisionExtractor extractor;
    const auto means = extractor.sample_points(scene.frame, analysis, 1, 64, 64);
    ASSERT_EQ(means.size(), 3U);

    const glm::vec4 black { 0.0F, 0.0F, 0.0F, 1.0F };
    EXPECT_TRUE(color_close(means[0], scene.colors[0], 0.05F));
    EXPECT_TRUE(color_close(means[1], scene.colors[1], 0.05F));
    EXPECT_TRUE(color_close(means[2], black, 0.05F));
}

TEST_F(VisionExtractorTest, SilhouetteFillsTheOuterContourAndLeavesTheHoleEmpty)
{
    constexpr uint32_t size = 32;
    const auto frame = pattern_frame(size, [](uint32_t, uint32_t) { return glm::vec4 { 1.0F }; });
    ASSERT_TRUE(frame);

    const auto square = [](float lo, float hi) {
        return std::vector<glm::vec2> { { lo, lo }, { hi, lo }, { hi, hi }, { lo, hi } };
    };

    KV::FindElementsAnalysis elements;
    elements.boxes.push_back({ .x = 0.1F, .y = 0.1F, .w = 0.8F, .h = 0.8F, .label_id = 1 });
    elements.contours.push_back({ .points = square(0.2F, 0.8F), .area = 0.0F, .perimeter = 0.0F, .label_id = 1 });
    elements.contours.push_back({ .points = square(0.4F, 0.6F), .area = 0.0F, .perimeter = 0.0F, .parent_label = 1, .label_id = 0 });

    KV::VisionAnalysis analysis;
    analysis.find_elements = std::move(elements);

    VisionExtractor extractor;
    const auto tile = extractor.silhouettes(frame, analysis, size, size);
    ASSERT_TRUE(tile);

    EXPECT_GT(read_pixel(tile, 8, 8, size).a, 0.9F);
    EXPECT_LT(read_pixel(tile, 16, 16, size).a, 0.1F);
    EXPECT_LT(read_pixel(tile, 1, 1, size).a, 0.1F);
    EXPECT_LT(read_pixel(tile, 30, 30, size).a, 0.1F);

    const auto single = extractor.silhouette(frame, analysis, size, size);
    ASSERT_TRUE(single);
    EXPECT_GT(read_pixel(single, 8, 8, size).a, 0.9F);
}

TEST_F(VisionExtractorTest, SelectElementRanksByEachCriterion)
{
    auto scene = make_three_element_scene();
    ASSERT_TRUE(scene.frame);

    auto& elements = *scene.analysis.find_elements;
    elements.boxes = {
        { .x = 0.05F, .y = 0.05F, .w = 0.1F, .h = 0.1F, .label_id = 1 },
        { .x = 0.4F, .y = 0.4F, .w = 0.2F, .h = 0.2F, .label_id = 2 },
        { .x = 0.75F, .y = 0.8F, .w = 0.2F, .h = 0.2F, .label_id = 3 },
    };
    elements.contours = {
        { .area = 0.1F, .perimeter = 0.0F, .label_id = 1 },
        { .area = 0.3F, .perimeter = 0.0F, .label_id = 2 },
        { .area = 0.2F, .perimeter = 0.0F, .label_id = 3 },
    };
    elements.shapes = { { 0.5F, 1.0F }, { 0.9F, 3.0F }, { 0.7F, 2.0F } };
    elements.nearest_neighbor_distance_px = { 10.0F, 5.0F, 20.0F };

    VisionExtractor extractor;
    const auto pick = [&](Yantra::ElementCriterion criterion, bool largest, glm::vec2 point = { 0.5F, 0.5F }) {
        const auto index = extractor.select_element(scene.analysis, { .criterion = criterion, .largest = largest, .point = point });
        return index ? static_cast<int>(*index) : -1;
    };

    EXPECT_EQ(pick(Yantra::ElementCriterion::Area, true), 1);
    EXPECT_EQ(pick(Yantra::ElementCriterion::Area, false), 0);
    EXPECT_EQ(pick(Yantra::ElementCriterion::Compactness, true), 1);
    EXPECT_EQ(pick(Yantra::ElementCriterion::AspectRatio, false), 0);
    EXPECT_EQ(pick(Yantra::ElementCriterion::Isolation, true), 2);
    EXPECT_EQ(pick(Yantra::ElementCriterion::Proximity, true, { 0.9F, 0.9F }), 2);
}

TEST_F(VisionExtractorTest, FocusedCropFollowsTheRankingNotTheIndex)
{
    auto scene = make_three_element_scene();
    ASSERT_TRUE(scene.frame);

    auto& elements = *scene.analysis.find_elements;
    elements.contours = {
        { .area = 0.1F, .perimeter = 0.0F, .label_id = 1 },
        { .area = 0.3F, .perimeter = 0.0F, .label_id = 2 },
        { .area = 0.2F, .perimeter = 0.0F, .label_id = 3 },
    };
    elements.shapes = { { 0.5F, 1.0F }, { 0.9F, 3.0F }, { 0.7F, 2.0F } };
    elements.nearest_neighbor_distance_px = { 10.0F, 5.0F, 20.0F };

    VisionExtractor extractor;
    edit_config<VisionExtractorConfig>(extractor, [](auto& c) {
        c.focus = Yantra::ElementFocus { .criterion = Yantra::ElementCriterion::Isolation, .largest = true };
    });
    const auto isolated = extractor.crop(scene.frame, scene.analysis, 8, 8);
    ASSERT_TRUE(isolated);
    const auto isolated_centre = read_pixel(isolated, 4, 4, 8);

    edit_config<VisionExtractorConfig>(extractor, [](auto& c) {
        c.focus = std::nullopt;
        c.element_index = 1;
    });
    const auto indexed = extractor.crop(scene.frame, scene.analysis, 8, 8);
    ASSERT_TRUE(indexed);
    const auto indexed_centre = read_pixel(indexed, 4, 4, 8);

    EXPECT_TRUE(color_close(isolated_centre, scene.colors[2], 0.05F));
    EXPECT_TRUE(color_close(indexed_centre, scene.colors[1], 0.05F));
}

TEST_F(VisionExtractorTest, EqualizeAndExposeScaleBrightnessFromTheMeasuredHistogram)
{
    constexpr uint32_t size = 16;
    const auto frame = pattern_frame(size, [](uint32_t x, uint32_t) {
        const float v = x < size / 2 ? 0.2F : 0.4F;
        return glm::vec4 { v, v, v, 1.0F };
    });
    ASSERT_TRUE(frame);

    KV::MeasureAppearanceAnalysis appearance;
    appearance.histogram[51] = size * size / 2;
    appearance.histogram[102] = size * size / 2;
    appearance.mean_brightness = 0.3F;

    KV::VisionAnalysis analysis;
    analysis.measure_appearance = appearance;

    VisionExtractor extractor;

    const auto equalized = extractor.equalize(frame, analysis, size, size);
    ASSERT_TRUE(equalized);
    EXPECT_LT(read_pixel(equalized, 3, 8, size).r, 0.03F);
    EXPECT_GT(read_pixel(equalized, 12, 8, size).r, 0.97F);

    const auto exposed = extractor.expose(frame, analysis, 0.6F, size, size);
    ASSERT_TRUE(exposed);
    EXPECT_LT(std::abs(read_pixel(exposed, 3, 8, size).r - 0.4F), 0.03F);
    EXPECT_LT(std::abs(read_pixel(exposed, 12, 8, size).r - 0.8F), 0.03F);

    EXPECT_EQ(extractor.equalize(frame, KV::VisionAnalysis {}, size, size), nullptr);
}

TEST_F(VisionExtractorTest, EdgesKeepTheSourceOnlyWhereAnEdgeWasDetected)
{
    constexpr uint32_t size = 32;
    const auto frame = pattern_frame(size, [](uint32_t, uint32_t) { return glm::vec4 { 1.0F, 0.0F, 0.0F, 1.0F }; });
    const auto edge_image = pattern_frame(size, [](uint32_t x, uint32_t) {
        const float v = x == 16 ? 1.0F : 0.0F;
        return glm::vec4 { v, v, v, 1.0F };
    });
    ASSERT_TRUE(frame);
    ASSERT_TRUE(edge_image);

    KV::VisionAnalysis analysis;
    analysis.detect_edges = edge_image;

    VisionExtractor extractor;
    const auto kept = extractor.edges(frame, analysis, size, size);
    ASSERT_TRUE(kept);

    EXPECT_TRUE(color_close(read_pixel(kept, 16, 10, size), { 1.0F, 0.0F, 0.0F, 1.0F }, 0.05F));
    EXPECT_LT(read_pixel(kept, 5, 10, size).a, 0.1F);
    EXPECT_EQ(extractor.edges(frame, KV::VisionAnalysis {}, size, size), nullptr);
}

TEST_F(VisionExtractorTest, MotionMaskKeepsFastConfidentPixelsAndGatesOnActivity)
{
    constexpr uint32_t size = 30;
    const auto frame = pattern_frame(size, [](uint32_t, uint32_t) { return glm::vec4 { 0.0F, 1.0F, 0.0F, 1.0F }; });
    const auto flow = pattern_frame(size, [](uint32_t x, uint32_t) {
        if (x < 10)
            return glm::vec4 { 3.0F, 0.0F, 1.0F, 0.0F };
        if (x < 20)
            return glm::vec4 { 0.1F, 0.0F, 1.0F, 0.0F };
        return glm::vec4 { 3.0F, 0.0F, 0.0F, 0.0F };
    });
    ASSERT_TRUE(frame);
    ASSERT_TRUE(flow);

    KV::VisionAnalysis analysis;
    analysis.estimate_motion = flow;
    analysis.motion_activity = 0.5F;

    VisionExtractor extractor;
    edit_config<VisionExtractorConfig>(extractor, [](auto& c) {
        c.min_speed = 1.0F;
        c.min_confidence = 0.5F;
        c.min_activity = 0.0F;
    });
    const auto moving = extractor.motion_mask(frame, analysis, size, size);
    ASSERT_TRUE(moving);

    EXPECT_TRUE(color_close(read_pixel(moving, 4, 10, size), { 0.0F, 1.0F, 0.0F, 1.0F }, 0.05F));
    EXPECT_LT(read_pixel(moving, 14, 10, size).a, 0.1F);
    EXPECT_LT(read_pixel(moving, 25, 10, size).a, 0.1F);

    edit_config<VisionExtractorConfig>(extractor, [](auto& c) { c.min_activity = 0.9F; });
    EXPECT_EQ(extractor.motion_mask(frame, analysis, size, size), nullptr);
    EXPECT_EQ(extractor.motion_mask(frame, KV::VisionAnalysis {}, size, size), nullptr);
}

TEST_F(VisionExtractorTest, AnnotateDrawsBoxesAndTrackArrowsInTheirColours)
{
    constexpr uint32_t size = 64;
    const auto frame = pattern_frame(size, [](uint32_t, uint32_t) { return glm::vec4 { 0.0F, 0.0F, 0.0F, 1.0F }; });
    ASSERT_TRUE(frame);

    KV::VisionAnalysis analysis;
    KV::FindElementsAnalysis elements;
    elements.boxes.push_back({ .x = 0.25F, .y = 0.25F, .w = 0.5F, .h = 0.5F, .label_id = 1 });
    analysis.find_elements = std::move(elements);

    KV::TrackObjectsAnalysis tracks;
    tracks.tracks.push_back({ .position = { 0.9F, 0.1F }, .tracked = true, .previous = { 0.1F, 0.1F } });
    tracks.tracks.push_back({ .position = { 0.9F, 0.9F }, .tracked = false, .previous = { 0.1F, 0.9F } });
    tracks.bounds = { .x = 0.0F, .y = 0.0F, .w = 0.0F, .h = 0.0F };
    analysis.track_objects = tracks;

    VisionExtractor extractor;
    const auto overlay = extractor.annotate(frame, analysis, size, size);
    ASSERT_TRUE(overlay);

    EXPECT_TRUE(color_close(read_pixel(overlay, 16, 32, size), { 1.0F, 1.0F, 0.0F, 1.0F }, 0.05F));
    EXPECT_TRUE(color_close(read_pixel(overlay, 32, 32, size), { 0.0F, 0.0F, 0.0F, 1.0F }, 0.05F));
    EXPECT_TRUE(color_close(read_pixel(overlay, 32, 6, size), { 0.0F, 1.0F, 0.0F, 1.0F }, 0.05F));
    EXPECT_TRUE(color_close(read_pixel(overlay, 32, 58, size), { 1.0F, 0.0F, 0.0F, 1.0F }, 0.05F));
}

TEST_F(VisionExtractorTest, OperationPathStoresTheCropInTheExtractionFacet)
{
    constexpr uint32_t size = 256;
    constexpr uint32_t out_size = 32;
    const KV::BoundingBox region { .x = 0.25F, .y = 0.25F, .w = 0.25F, .h = 0.25F };
    const glm::vec4 green { 0.0F, 1.0F, 0.0F, 1.0F };

    const auto frame = solid_region_frame(size, region, green);
    ASSERT_TRUE(frame);

    auto container = std::make_shared<Kakshya::TextureContainer>(frame, Portal::Graphics::ImageFormat::RGBA32F);

    VisionExtractor extractor(extractor_config(VisionExtractMode::Crop));
    edit_config<VisionExtractorConfig>(extractor, [](auto& c) {
        c.output_width = out_size;
        c.output_height = out_size;
    });

    KV::VisionAnalysis analysis;
    analysis.find_elements = KV::FindElementsAnalysis { .boxes = { region } };

    Yantra::ContainerIO input;
    input.data = container;
    input.facets.set(analysis);

    const auto output = extractor.apply_operation(input);
    const auto* extraction = output.facets.get<VisionExtraction>();
    ASSERT_NE(extraction, nullptr);
    ASSERT_TRUE(extraction->image);
    EXPECT_TRUE(color_close(read_pixel(extraction->image, out_size / 2, out_size / 2, out_size), green, 0.05F));
}

TEST_F(VisionExtractorTest, OperationModesFillTheirOwnExtractionMembers)
{
    auto scene = make_three_element_scene();
    ASSERT_TRUE(scene.frame);

    auto container = std::make_shared<Kakshya::TextureContainer>(scene.frame, Portal::Graphics::ImageFormat::RGBA32F);

    Yantra::ContainerIO input;
    input.data = container;
    input.facets.set(scene.analysis);

    VisionExtractor crops_extractor(extractor_config(VisionExtractMode::Crops));
    edit_config<VisionExtractorConfig>(crops_extractor, [](auto& c) {
        c.tile_width = 8;
        c.tile_height = 8;
    });
    const auto crops_out = crops_extractor.apply_operation(input);
    const auto* crops = crops_out.facets.get<VisionExtraction>();
    ASSERT_NE(crops, nullptr);
    ASSERT_TRUE(crops->image);
    EXPECT_EQ(crops->image->get_width(), 24U);

    VisionExtractor regions_extractor(extractor_config(VisionExtractMode::SampleRegions));
    const auto regions_out = regions_extractor.apply_operation(input);
    const auto* regions = regions_out.facets.get<VisionExtraction>();
    ASSERT_NE(regions, nullptr);
    ASSERT_TRUE(regions->samples);
    EXPECT_EQ(regions->samples->size(), 3U);

    VisionExtractor select_extractor(extractor_config(VisionExtractMode::SelectElement));
    edit_config<VisionExtractorConfig>(select_extractor, [](auto& c) {
        c.focus = Yantra::ElementFocus { .criterion = Yantra::ElementCriterion::Proximity, .point = { 0.9F, 0.9F } };
    });
    const auto select_out = select_extractor.apply_operation(input);
    const auto* selected = select_out.facets.get<VisionExtraction>();
    ASSERT_NE(selected, nullptr);
    ASSERT_TRUE(selected->element);
    EXPECT_EQ(*selected->element, 2U);
}

TEST_F(VisionExtractorTest, CropFallsBackToTrackBoundsWithoutElements)
{
    constexpr uint32_t size = 256;
    constexpr uint32_t out_size = 32;
    const KV::BoundingBox region { .x = 0.25F, .y = 0.25F, .w = 0.25F, .h = 0.25F };
    const glm::vec4 green { 0.0F, 1.0F, 0.0F, 1.0F };

    const auto frame = solid_region_frame(size, region, green);
    ASSERT_TRUE(frame);

    KV::VisionAnalysis analysis;
    analysis.track_objects = KV::TrackObjectsAnalysis { .bounds = region };

    VisionExtractor extractor;
    const auto cropped = extractor.crop(frame, analysis, out_size, out_size);
    ASSERT_TRUE(cropped);
    EXPECT_TRUE(color_close(read_pixel(cropped, out_size / 2, out_size / 2, out_size), green, 0.05F));
}

TEST_F(VisionExtractorTest, CropFallsBackToKeypointBoundsWithoutTracksOrElements)
{
    constexpr uint32_t size = 256;
    constexpr uint32_t out_size = 32;
    const KV::BoundingBox region { .x = 0.25F, .y = 0.25F, .w = 0.25F, .h = 0.25F };
    const glm::vec4 green { 0.0F, 1.0F, 0.0F, 1.0F };

    const auto frame = solid_region_frame(size, region, green);
    ASSERT_TRUE(frame);

    KV::VisionAnalysis analysis;
    analysis.detect_features = KV::DetectFeaturesAnalysis { .bounds = region };

    VisionExtractor extractor;
    const auto cropped = extractor.crop(frame, analysis, out_size, out_size);
    ASSERT_TRUE(cropped);
    EXPECT_TRUE(color_close(read_pixel(cropped, out_size / 2, out_size / 2, out_size), green, 0.05F));
}

TEST_F(VisionExtractorTest, CropReturnsNothingWithoutAnyRegion)
{
    const auto frame = pattern_frame(64, [](uint32_t, uint32_t) { return glm::vec4 { 0.0F, 0.0F, 0.0F, 1.0F }; });
    ASSERT_TRUE(frame);

    VisionExtractor extractor;
    EXPECT_FALSE(extractor.crop(frame, KV::VisionAnalysis {}, 8, 8));
}

TEST_F(VisionExtractorTest, CropFallsBackToTrackBoundsWhenTheElementIndexIsOutOfRange)
{
    constexpr uint32_t size = 256;
    constexpr uint32_t out_size = 32;
    const KV::BoundingBox box { .x = 0.05F, .y = 0.05F, .w = 0.2F, .h = 0.2F };
    const KV::BoundingBox track { .x = 0.25F, .y = 0.25F, .w = 0.25F, .h = 0.25F };
    const glm::vec4 green { 0.0F, 1.0F, 0.0F, 1.0F };

    const auto frame = solid_region_frame(size, track, green);
    ASSERT_TRUE(frame);

    KV::VisionAnalysis analysis;
    analysis.find_elements = KV::FindElementsAnalysis { .boxes = { box } };
    analysis.track_objects = KV::TrackObjectsAnalysis { .bounds = track };

    VisionExtractor extractor;
    edit_config<VisionExtractorConfig>(extractor, [](auto& c) { c.element_index = 5; });

    const auto cropped = extractor.crop(frame, analysis, out_size, out_size);
    ASSERT_TRUE(cropped);
    EXPECT_TRUE(color_close(read_pixel(cropped, out_size / 2, out_size / 2, out_size), green, 0.05F));
}

TEST_F(VisionExtractorTest, PatchesReturnNothingWithoutKeypointsOrTracks)
{
    const auto frame = pattern_frame(64, [](uint32_t, uint32_t) { return glm::vec4 { 0.0F, 0.0F, 0.0F, 1.0F }; });
    ASSERT_TRUE(frame);

    VisionExtractor extractor;
    EXPECT_FALSE(extractor.patches(frame, KV::VisionAnalysis {}, 8, 8, 64, 64));

    KV::VisionAnalysis empty;
    empty.detect_features = KV::DetectFeaturesAnalysis {};
    EXPECT_FALSE(extractor.patches(frame, empty, 8, 8, 64, 64));
}

TEST_F(VisionExtractorTest, SilhouettesReturnNothingWithoutElements)
{
    const auto frame = pattern_frame(32, [](uint32_t, uint32_t) { return glm::vec4 { 1.0F }; });
    ASSERT_TRUE(frame);

    VisionExtractor extractor;
    EXPECT_FALSE(extractor.silhouettes(frame, KV::VisionAnalysis {}, 32, 32));

    KV::VisionAnalysis empty;
    empty.find_elements = KV::FindElementsAnalysis {};
    EXPECT_FALSE(extractor.silhouettes(frame, empty, 32, 32));
}

TEST_F(VisionExtractorTest, SilhouetteReturnsNothingForAnOutOfRangeElementIndex)
{
    const auto frame = pattern_frame(32, [](uint32_t, uint32_t) { return glm::vec4 { 1.0F }; });
    ASSERT_TRUE(frame);

    KV::VisionAnalysis analysis;
    analysis.find_elements = KV::FindElementsAnalysis {
        .boxes = { { .x = 0.1F, .y = 0.1F, .w = 0.8F, .h = 0.8F, .label_id = 1 } },
    };

    VisionExtractor extractor;
    edit_config<VisionExtractorConfig>(extractor, [](auto& c) { c.element_index = 5; });
    EXPECT_FALSE(extractor.silhouette(frame, analysis, 32, 32));
}

TEST_F(VisionExtractorTest, MaskReturnsNothingWithoutALabelledContour)
{
    constexpr uint32_t W = 16;
    constexpr uint32_t H = 16;
    const auto image = pattern_frame(W, [](uint32_t, uint32_t) { return glm::vec4 { 1.0F }; });
    ASSERT_TRUE(image);

    VisionGpuExecutor executor;
    VisionExtractor extractor;
    EXPECT_FALSE(extractor.mask(executor, image, KV::VisionAnalysis {}, W, H));

    KV::VisionAnalysis unlabelled;
    unlabelled.find_elements = KV::FindElementsAnalysis {
        .contours = { { .area = 0.2F, .perimeter = 0.0F, .label_id = 0 } },
        .boxes = { { .x = 0.1F, .y = 0.1F, .w = 0.5F, .h = 0.5F, .label_id = 1 } },
    };
    EXPECT_FALSE(extractor.mask(executor, image, unlabelled, W, H));
}

TEST_F(VisionExtractorTest, SelectElementReturnsNothingWithoutBoxes)
{
    VisionExtractor extractor;
    EXPECT_FALSE(extractor.select_element(KV::VisionAnalysis {}, Yantra::ElementFocus {}).has_value());

    KV::VisionAnalysis empty;
    empty.find_elements = KV::FindElementsAnalysis {};
    EXPECT_FALSE(extractor.select_element(empty, Yantra::ElementFocus {}).has_value());
}

TEST_F(VisionExtractorTest, SampleRegionsAndPointsAreEmptyWithoutRegionsOrPoints)
{
    const auto frame = pattern_frame(64, [](uint32_t, uint32_t) { return glm::vec4 { 0.0F, 0.0F, 0.0F, 1.0F }; });
    ASSERT_TRUE(frame);

    VisionExtractor extractor;
    EXPECT_TRUE(extractor.sample_regions(frame, KV::VisionAnalysis {}, 64, 64).empty());
    EXPECT_TRUE(extractor.sample_points(frame, KV::VisionAnalysis {}, 1, 64, 64).empty());
}

TEST_F(VisionExtractorTest, OperationWithoutAFrameOrContainerProducesNoExtraction)
{
    KV::VisionAnalysis analysis;
    analysis.find_elements = KV::FindElementsAnalysis { .boxes = { { .x = 0.1F, .y = 0.1F, .w = 0.5F, .h = 0.5F, .label_id = 1 } } };

    VisionExtractor extractor(extractor_config(VisionExtractMode::Crop));

    Yantra::ContainerIO input;
    input.facets.set(analysis);

    const auto output = extractor.apply_operation(input);
    EXPECT_EQ(output.facets.get<VisionExtraction>(), nullptr);
}

TEST_F(VisionExtractorTest, OperationWithoutAnAnalysisFacetProducesNoExtraction)
{
    const auto frame = pattern_frame(64, [](uint32_t, uint32_t) { return glm::vec4 { 0.0F, 0.0F, 0.0F, 1.0F }; });
    ASSERT_TRUE(frame);

    VisionExtractor extractor(extractor_config(VisionExtractMode::Crop));

    Yantra::ContainerIO input(as_container(frame));

    const auto output = extractor.apply_operation(input);
    EXPECT_EQ(output.facets.get<VisionExtraction>(), nullptr);
}

TEST_F(VisionExtractorTest, OperationWithAnImageFacetCropsTheSuppliedFrame)
{
    constexpr uint32_t size = 256;
    constexpr uint32_t out_size = 32;
    const KV::BoundingBox region { .x = 0.25F, .y = 0.25F, .w = 0.25F, .h = 0.25F };
    const glm::vec4 green { 0.0F, 1.0F, 0.0F, 1.0F };

    const auto frame = solid_region_frame(size, region, green);
    ASSERT_TRUE(frame);

    VisionExtractor extractor(extractor_config(VisionExtractMode::Crop));
    edit_config<VisionExtractorConfig>(extractor, [](auto& c) {
        c.output_width = out_size;
        c.output_height = out_size;
    });

    KV::VisionAnalysis analysis;
    analysis.find_elements = KV::FindElementsAnalysis { .boxes = { region } };

    Yantra::ContainerIO input;
    input.facets.set(KV::vision_frame(), frame);
    input.facets.set(analysis);

    const auto output = extractor.apply_operation(input);
    const auto* extraction = output.facets.get<VisionExtraction>();
    ASSERT_NE(extraction, nullptr);
    ASSERT_TRUE(extraction->image);
    EXPECT_TRUE(color_close(read_pixel(extraction->image, out_size / 2, out_size / 2, out_size), green, 0.05F));
}

TEST_F(VisionExtractorTest, OperationCropsTheStackLayerTheAnalysisNames)
{
    constexpr uint32_t size = 256;
    constexpr uint32_t out_size = 32;
    const KV::BoundingBox region { .x = 0.25F, .y = 0.25F, .w = 0.25F, .h = 0.25F };
    const glm::vec4 red { 1.0F, 0.0F, 0.0F, 1.0F };

    const auto black = pattern_frame(size, [](uint32_t, uint32_t) { return glm::vec4 { 0.0F, 0.0F, 0.0F, 1.0F }; });
    const auto marked = solid_region_frame(size, region, red);
    ASSERT_TRUE(black && marked);

    const auto stack = layer_stack(size, black, marked);

    VisionExtractor extractor(extractor_config(VisionExtractMode::Crop));
    edit_config<VisionExtractorConfig>(extractor, [](auto& c) {
        c.output_width = out_size;
        c.output_height = out_size;
    });

    KV::VisionAnalysis analysis;
    analysis.find_elements = KV::FindElementsAnalysis { .boxes = { region } };
    analysis.layer = 1U;

    Yantra::ContainerIO input(stack);
    input.facets.set(analysis);

    const auto output = extractor.apply_operation(input);
    const auto* extraction = output.facets.get<VisionExtraction>();
    ASSERT_NE(extraction, nullptr);
    ASSERT_TRUE(extraction->image);
    EXPECT_TRUE(color_close(read_pixel(extraction->image, out_size / 2, out_size / 2, out_size), red, 0.05F));
}

} // namespace MayaFlux::Test::Vision
