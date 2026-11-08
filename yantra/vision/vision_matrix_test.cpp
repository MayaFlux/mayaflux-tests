#include "gtest/gtest.h"

#include "../../support/vision_test_support.hpp"
#include "MayaFlux/Yantra/Workflows/Vision/VisionMatrix.hpp"

namespace MayaFlux::Test::Vision {

namespace KV = MayaFlux::Kinesis::Vision;
using Yantra::Vision::VisionMatrix;
using Yantra::VisionExtractMode;
using Yantra::VisionExtractorConfig;

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

/** @brief Three blobs on a 16x16 frame: two 3x3 squares and a 2x2 block. */
ImageHandle blob_frame()
{
    return pattern_frame(16, [](uint32_t x, uint32_t y) {
        const bool a = x >= 1 && x <= 3 && y >= 1 && y <= 3;
        const bool b = x >= 8 && x <= 10 && y >= 8 && y <= 10;
        const bool c = x >= 13 && x <= 14 && y >= 13 && y <= 14;
        return a || b || c ? glm::vec4 { 1.0F } : glm::vec4 { 0.0F };
    });
}

KV::VisionQuery track_matrix_query()
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

/** @brief Three coloured boxes on a 128x128 frame, matching the grey boxes of the 64x64 analysed frame. */
struct DisjointScene {
    ImageHandle analysed;
    ImageHandle extracted;
    ImageHandle blank;
    std::array<glm::vec4, 3> colours { {
        { 1.0F, 0.0F, 0.0F, 1.0F },
        { 0.0F, 1.0F, 0.0F, 1.0F },
        { 0.0F, 0.0F, 1.0F, 1.0F },
    } };
};

DisjointScene make_disjoint_scene()
{
    constexpr uint32_t small = 64;
    constexpr uint32_t large = 128;

    const std::array<std::array<float, 4>, 3> rects { {
        { 0.125F, 0.125F, 0.25F, 0.25F },
        { 0.5F, 0.125F, 0.75F, 0.375F },
        { 0.25F, 0.5F, 0.75F, 0.75F },
    } };
    const std::array<float, 3> grays { 0.6F, 0.8F, 1.0F };

    const auto inside = [&](uint32_t x, uint32_t y, uint32_t size) -> std::optional<size_t> {
        const float fx = (static_cast<float>(x) + 0.5F) / static_cast<float>(size);
        const float fy = (static_cast<float>(y) + 0.5F) / static_cast<float>(size);
        for (size_t i = 0; i < rects.size(); ++i) {
            if (fx >= rects[i][0] && fx < rects[i][2] && fy >= rects[i][1] && fy < rects[i][3])
                return i;
        }
        return std::nullopt;
    };

    DisjointScene scene;
    scene.analysed = pattern_frame(small, [&](uint32_t x, uint32_t y) {
        const auto i = inside(x, y, small);
        return i ? glm::vec4 { grays[*i], grays[*i], grays[*i], 1.0F } : glm::vec4 { 0.0F, 0.0F, 0.0F, 1.0F };
    });
    scene.extracted = pattern_frame(large, [&](uint32_t x, uint32_t y) {
        const auto i = inside(x, y, large);
        return i ? scene.colours[*i] : glm::vec4 { 0.0F, 0.0F, 0.0F, 1.0F };
    });
    scene.blank = pattern_frame(small, [](uint32_t, uint32_t) { return glm::vec4 { 0.0F, 0.0F, 0.0F, 1.0F }; });
    return scene;
}

/** @brief True when the atlas holds all three colours among its tile centres. */
bool tiles_match(const ImageHandle& atlas, const std::array<glm::vec4, 3>& colours, uint32_t tile)
{
    if (!atlas || atlas->get_width() != tile * 3)
        return false;
    std::array<bool, 3> seen {};
    for (uint32_t t = 0; t < 3; ++t) {
        const auto centre = read_pixel(atlas, t * tile + tile / 2, tile / 2, tile * 3);
        for (size_t c = 0; c < colours.size(); ++c)
            seen[c] = seen[c] || color_close(centre, colours[c], 0.05F);
    }
    return seen[0] && seen[1] && seen[2];
}

} // namespace

class VisionMatrixTest : public Fixture { };

TEST_F(VisionMatrixTest, ExecuteAnalysesTheFrameAndKeepsItWithItsAnalysis)
{
    const auto blobs = blob_frame();
    ASSERT_TRUE(blobs);

    const auto matrix = VisionMatrix::create({ .intents = KV::VisionIntent::FindElements }, VisionExtractMode::Crops);

    EXPECT_FALSE(matrix->frame());
    EXPECT_FALSE(matrix->analysis().find_elements);

    matrix->execute(blobs);
    const auto& analysis = matrix->analysis();
    ASSERT_TRUE(analysis.find_elements.has_value());
    EXPECT_EQ(analysis.find_elements->boxes.size(), 3U);
    EXPECT_EQ(matrix->frame(), blobs);

    const auto atlas = matrix->extractor()->crops(matrix->frame(), analysis, 8, 8);
    ASSERT_TRUE(atlas);
    EXPECT_EQ(atlas->get_width(), 24U);
    EXPECT_EQ(atlas->get_height(), 8U);
}

TEST_F(VisionMatrixTest, ResultReportsOnlyTheIntentsTheQuerySets)
{
    const auto blobs = blob_frame();
    ASSERT_TRUE(blobs);

    const auto matrix = VisionMatrix::create({ .intents = KV::VisionIntent::FindElements });
    matrix->execute(blobs);

    EXPECT_TRUE(matrix->result(KV::VisionIntent::FindElements));
    EXPECT_FALSE(matrix->result(KV::VisionIntent::TrackObjects));
}

TEST_F(VisionMatrixTest, TrackingReportsMotionOnlyFromTheSecondCall)
{
    const auto frame0 = wave_frame(256, 0.0F, 0.0F);
    const auto frame1 = wave_frame(256, 3.0F, -2.0F);
    ASSERT_TRUE(frame0 && frame1);

    const auto matrix = VisionMatrix::create(track_matrix_query());
    EXPECT_FALSE(matrix->extractor());

    matrix->execute(frame0);
    const uint32_t first_tracked = matrix->analysis().track_objects ? matrix->analysis().track_objects->tracked_count : 0;
    EXPECT_EQ(first_tracked, 0U);

    matrix->execute(frame1);
    const uint32_t second_tracked = matrix->analysis().track_objects ? matrix->analysis().track_objects->tracked_count : 0;
    EXPECT_GT(second_tracked, 0U);
}

TEST_F(VisionMatrixTest, ExecuteWithNullClearsTheFrameAndAnalysis)
{
    const auto frame0 = wave_frame(256, 0.0F, 0.0F);
    const auto frame1 = wave_frame(256, 3.0F, -2.0F);
    ASSERT_TRUE(frame0 && frame1);

    const auto matrix = VisionMatrix::create(track_matrix_query());
    matrix->execute(frame0);
    matrix->execute(frame1);
    ASSERT_TRUE(matrix->analysis().track_objects.has_value());

    matrix->execute(nullptr);
    EXPECT_FALSE(matrix->frame());
    EXPECT_FALSE(matrix->analysis().track_objects);
}

TEST_F(VisionMatrixTest, CropsFromAnotherFrameUseTheAnalysisOfTheFirst)
{
    const auto scene = make_disjoint_scene();
    ASSERT_TRUE(scene.analysed && scene.extracted && scene.blank);

    constexpr uint32_t tile = 8;
    const auto matrix = VisionMatrix::create({ .intents = KV::VisionIntent::FindElements }, VisionExtractMode::Crops);
    const auto analysis = matrix->analyze(scene.analysed);
    ASSERT_TRUE(analysis.find_elements.has_value());
    EXPECT_EQ(analysis.find_elements->boxes.size(), 3U);

    EXPECT_TRUE(tiles_match(matrix->extractor()->crops(scene.extracted, analysis, tile, tile), scene.colours, tile));

    (void)matrix->analyze(scene.blank);
    EXPECT_TRUE(tiles_match(matrix->extractor()->crops(scene.extracted, analysis, tile, tile), scene.colours, tile));
}

TEST_F(VisionMatrixTest, ExtractKeepsItsImageUntilTheNextExecute)
{
    const auto scene = make_disjoint_scene();
    ASSERT_TRUE(scene.analysed && scene.extracted && scene.blank);

    constexpr uint32_t tile = 8;
    const auto matrix = VisionMatrix::create({ .intents = KV::VisionIntent::FindElements }, VisionExtractMode::Crops);
    edit_config<VisionExtractorConfig>(*matrix->extractor(), [](auto& c) {
        c.tile_width = tile;
        c.tile_height = tile;
    });

    const auto analysis = matrix->analyze(scene.analysed);
    matrix->extract(analysis, scene.extracted);
    EXPECT_EQ(matrix->frame(), scene.extracted);
    EXPECT_TRUE(tiles_match(matrix->extraction(), scene.colours, tile));

    matrix->execute(scene.blank);
    EXPECT_FALSE(matrix->extraction());
}

} // namespace MayaFlux::Test::Vision
