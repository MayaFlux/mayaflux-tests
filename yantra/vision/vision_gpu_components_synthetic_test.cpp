#include "gtest/gtest.h"
#include "../../support/test_metadata.hpp"

#include "../../support/vision_test_support.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))

namespace MayaFlux::Test::Vision {

using Kinesis::Vision::ComponentResult;
using Kinesis::Vision::Contour;
using Kinesis::Vision::VisionSequence;
using Yantra::VisionGpuExecutor;

namespace {

using PixelBox = std::array<uint32_t, 4>;

bool matches_any(const PixelBox& box, const std::vector<PixelBox>& expected)
{
    return std::ranges::any_of(expected, [&](const PixelBox& e) { return e == box; });
}

} // namespace

class VisionGpuComponentsTest : public VisionGpuFixture { };

TEST_F(VisionGpuComponentsTest, ConnectedComponentsLabelsEveryBlobWithItsExactBox)
{
    constexpr uint32_t W = 16;
    constexpr uint32_t H = 16;

    std::vector<float> pixels(static_cast<size_t>(W) * H * 4, 0.0F);
    for (uint32_t y = 1; y <= 3; ++y) {
        for (uint32_t x = 1; x <= 3; ++x) {
            set_all_channels(pixels, W, x, y, 1.0F);
        }
    }
    for (uint32_t y = 1; y <= 2; ++y) {
        set_all_channels(pixels, W, 8, y, 1.0F);
        set_all_channels(pixels, W, 9, y, 1.0F);
    }
    for (uint32_t x = 8; x <= 10; ++x) {
        set_all_channels(pixels, W, x, 3, 1.0F);
    }
    set_all_channels(pixels, W, 10, 4, 1.0F);
    set_all_channels(pixels, W, 13, 13, 1.0F);

    const auto image = rgba_image(W, H, pixels);
    ASSERT_NE(image, nullptr);

    const std::vector<PixelBox> expected {
        { 1, 1, 3, 3 },
        { 8, 1, 10, 4 },
        { 13, 13, 13, 13 },
    };

    const auto sequence = VisionSequence::Builder {}
                              .connected_components({ .export_labels = true, .with_colors = true })
                              .build();
    VisionGpuExecutor exec;
    const auto result = exec.run(sequence, image, W, H);
    const auto* cc = std::get_if<ComponentResult>(&result.structured);
    ASSERT_NE(cc, nullptr);
    EXPECT_EQ(cc->count, 3U);
    for (const auto& b : cc->boxes) {
        EXPECT_TRUE(matches_any(pixel_box(b, W, H), expected));
    }
}

TEST_F(VisionGpuComponentsTest, AStaircaseOfTouchingStepsIsOneComponentWithItsFullExtent)
{
    constexpr uint32_t W = 256;
    constexpr uint32_t H = 256;

    std::vector<float> pixels(static_cast<size_t>(W) * H * 4, 0.0F);
    uint32_t x = 0;
    uint32_t y = 0;
    while (x < W - 4 && y < H - 4) {
        set_all_channels(pixels, W, x, y, 1.0F);
        set_all_channels(pixels, W, x + 1, y, 1.0F);
        set_all_channels(pixels, W, x, y + 1, 1.0F);
        set_all_channels(pixels, W, x + 1, y + 1, 1.0F);
        x += 2;
        y += 2;
    }

    const auto image = rgba_image(W, H, pixels);
    ASSERT_NE(image, nullptr);

    const auto sequence = VisionSequence::Builder {}
                              .connected_components({ .export_labels = true, .with_colors = true })
                              .build();
    VisionGpuExecutor exec;
    const auto result = exec.run(sequence, image, W, H);
    const auto* cc = std::get_if<ComponentResult>(&result.structured);
    ASSERT_NE(cc, nullptr);
    EXPECT_EQ(cc->count, 1U);
    ASSERT_FALSE(cc->boxes.empty());
    EXPECT_EQ(pixel_box(cc->boxes[0], W, H), (PixelBox { 0, 0, 251, 251 }));
}

class VisionGpuDenseLabelTest : public VisionGpuFixture { };

TEST_F(VisionGpuDenseLabelTest, LabelsStayExactAcrossAlternatingFramesOnOneExecutor)
{
    constexpr uint32_t W = 128;
    constexpr uint32_t H = 128;
    constexpr uint32_t runs = 32;

    std::vector<float> pixels(static_cast<size_t>(W) * H * 4, 0.0F);
    for (uint32_t y = 4; y <= 123; ++y) {
        for (uint32_t x = 4; x <= 63; ++x) {
            set_all_channels(pixels, W, x, y, 1.0F);
        }
    }
    std::vector<PixelBox> expected { { 4, 4, 63, 123 } };
    for (uint32_t row = 0; row < 4; ++row) {
        for (uint32_t col = 0; col < 4; ++col) {
            const uint32_t x0 = 72 + col * 12;
            const uint32_t y0 = 8 + row * 24;
            expected.push_back({ x0, y0, x0 + 2, y0 + 2 });
            for (uint32_t y = y0; y <= y0 + 2; ++y) {
                for (uint32_t x = x0; x <= x0 + 2; ++x) {
                    set_all_channels(pixels, W, x, y, 1.0F);
                }
            }
        }
    }

    const auto image = rgba_image(W, H, pixels);
    const auto blank_image = rgba_image(W, H, std::vector<float>(pixels.size(), 0.0F));
    const auto full_image = rgba_image(W, H, std::vector<float>(pixels.size(), 1.0F));
    ASSERT_NE(image, nullptr);
    ASSERT_NE(blank_image, nullptr);
    ASSERT_NE(full_image, nullptr);

    const std::vector<PixelBox> blank_expected;
    const std::vector<PixelBox> full_expected { { 0, 0, W - 1, H - 1 } };
    const auto sequence = VisionSequence::Builder {}
                              .connected_components({ .export_labels = true, .with_colors = true })
                              .build();
    VisionGpuExecutor exec;

    for (uint32_t run = 0; run < runs; ++run) {
        const auto& input = run % 3 == 0 ? image : run % 3 == 1 ? blank_image : full_image;
        const auto& target = run % 3 == 0 ? expected : run % 3 == 1 ? blank_expected : full_expected;
        const auto result = exec.run(sequence, input, W, H);
        ASSERT_NE(result.images.component_colors, nullptr) << "run " << run;
        ASSERT_TRUE(result.images.component_colors->is_initialized()) << "run " << run;

        const auto* cc = std::get_if<ComponentResult>(&result.structured);
        ASSERT_NE(cc, nullptr) << "run " << run;
        ASSERT_EQ(cc->count, target.size()) << "run " << run;
        ASSERT_EQ(cc->boxes.size(), target.size()) << "run " << run;

        std::vector<bool> seen_ids(target.size() + 1, false);
        for (const auto& b : cc->boxes) {
            ASSERT_GT(b.label_id, 0U) << "run " << run;
            ASSERT_LE(b.label_id, target.size()) << "run " << run;
            EXPECT_FALSE(seen_ids[b.label_id]) << "run " << run << " duplicate label " << b.label_id;
            seen_ids[b.label_id] = true;
            EXPECT_TRUE(matches_any(pixel_box(b, W, H), target)) << "run " << run;
        }
    }
}

class VisionGpuContoursTest : public VisionGpuFixture { };

TEST_F(VisionGpuContoursTest, SingleSquareTracesOneContourWithItsNormalisedArea)
{
    constexpr uint32_t W = 8;
    constexpr uint32_t H = 8;

    std::vector<float> pixels(static_cast<size_t>(W) * H * 4, 0.0F);
    for (uint32_t y = 2; y <= 4; ++y) {
        for (uint32_t x = 2; x <= 4; ++x) {
            set_all_channels(pixels, W, x, y, 1.0F);
        }
    }
    const auto image = rgba_image(W, H, pixels);
    ASSERT_NE(image, nullptr);

    const auto sequence = VisionSequence::Builder {}
                              .connected_components({ .export_labels = true, .with_colors = true })
                              .find_contours()
                              .build();
    VisionGpuExecutor exec;
    const auto result = exec.run(sequence, image, W, H);
    const auto* contours = std::get_if<std::vector<Contour>>(&result.structured);
    ASSERT_NE(contours, nullptr);
    ASSERT_EQ(contours->size(), 1U);

    const float expected_area = 9.0F / (static_cast<float>(W) * static_cast<float>(H));
    EXPECT_NEAR((*contours)[0].area, expected_area, 0.01F);
}

TEST_F(VisionGpuContoursTest, AHoleInsideAnObjectIsTracedAsASecondContour)
{
    constexpr uint32_t W = 12;
    constexpr uint32_t H = 12;

    std::vector<float> pixels(static_cast<size_t>(W) * H * 4, 0.0F);
    for (uint32_t y = 2; y <= 9; ++y) {
        for (uint32_t x = 2; x <= 9; ++x) {
            set_all_channels(pixels, W, x, y, 1.0F);
        }
    }
    for (uint32_t y = 5; y <= 6; ++y) {
        for (uint32_t x = 5; x <= 6; ++x) {
            const size_t idx = (static_cast<size_t>(y) * W + x) * 4;
            pixels[idx + 0] = 0.0F;
            pixels[idx + 1] = 0.0F;
            pixels[idx + 2] = 0.0F;
            pixels[idx + 3] = 1.0F;
        }
    }
    const auto image = rgba_image(W, H, pixels);
    ASSERT_NE(image, nullptr);

    const auto sequence = VisionSequence::Builder {}
                              .connected_components({ .export_labels = true, .with_colors = true })
                              .find_contours()
                              .build();
    VisionGpuExecutor exec;
    const auto result = exec.run(sequence, image, W, H);
    const auto* contours = std::get_if<std::vector<Contour>>(&result.structured);
    ASSERT_NE(contours, nullptr);
    EXPECT_EQ(contours->size(), 2U);
}

TEST_F(VisionGpuContoursTest, EightyScatteredSquaresGiveEightyContoursAndAnEmptyFrameStaysEmpty)
{
    constexpr uint32_t W = 128;
    constexpr uint32_t H = 128;
    constexpr float expected_area = 9.0F / static_cast<float>(W * H);

    const auto grid = square_grid(W, H, 10, 8, 4, 12, 14, 3);
    const auto image = rgba_image(W, H, grid.pixels);
    const auto blank_image = rgba_image(W, H, std::vector<float>(grid.pixels.size(), 0.0F));
    ASSERT_NE(image, nullptr);
    ASSERT_NE(blank_image, nullptr);

    const auto sequence = VisionSequence::Builder {}
                              .connected_components({ .export_labels = true })
                              .find_contours()
                              .build();
    VisionGpuExecutor exec;

    const auto empty_before = exec.run(sequence, blank_image, W, H);
    const auto* first_empty = std::get_if<std::vector<Contour>>(&empty_before.structured);
    ASSERT_NE(first_empty, nullptr);
    EXPECT_TRUE(first_empty->empty());

    const auto result = exec.run(sequence, image, W, H);
    const auto* contours = std::get_if<std::vector<Contour>>(&result.structured);
    ASSERT_NE(contours, nullptr);
    ASSERT_EQ(contours->size(), grid.boxes.size());

    std::vector<bool> matched(grid.boxes.size(), false);
    for (const auto& contour : *contours) {
        EXPECT_EQ(contour.points.size(), 8U);
        EXPECT_NEAR(contour.area, expected_area, 1e-5F);
        const auto box = contour_pixel_box(contour, W, H);
        bool found = false;
        for (size_t i = 0; i < grid.boxes.size(); ++i) {
            if (!matched[i] && box == grid.boxes[i]) {
                matched[i] = true;
                found = true;
                break;
            }
        }
        EXPECT_TRUE(found) << "unexpected contour bounds";
    }

    const auto empty_after = exec.run(sequence, blank_image, W, H);
    const auto* last_empty = std::get_if<std::vector<Contour>>(&empty_after.structured);
    ASSERT_NE(last_empty, nullptr);
    EXPECT_TRUE(last_empty->empty());
}

namespace {

    constexpr uint32_t k_ranked_size = 64;
    constexpr std::array<uint32_t, 4> k_ranked_sides { 5, 9, 3, 7 };

    std::vector<float> ranked_squares()
    {
        std::vector<float> pixels(static_cast<size_t>(k_ranked_size) * k_ranked_size * 4, 0.0F);
        uint32_t x0 = 4;
        for (const uint32_t side : k_ranked_sides) {
            for (uint32_t y = 4; y < 4 + side; ++y) {
                for (uint32_t x = x0; x < x0 + side; ++x) {
                    set_all_channels(pixels, k_ranked_size, x, y, 1.0F);
                }
            }
            x0 += side + 4;
        }
        return pixels;
    }

    float ranked_area(uint32_t side)
    {
        return static_cast<float>(side * side) / static_cast<float>(k_ranked_size * k_ranked_size);
    }

} // namespace

TEST_F(VisionGpuContoursTest, MaxContoursKeepsTheLargestSquaresInDescendingArea)
{
    const auto image = rgba_image(k_ranked_size, k_ranked_size, ranked_squares());
    ASSERT_NE(image, nullptr);

    const auto sequence = VisionSequence::Builder {}
                              .connected_components({ .export_labels = true })
                              .find_contours({ .max_contours = 2 })
                              .build();
    VisionGpuExecutor exec;
    const auto result = exec.run(sequence, image, k_ranked_size, k_ranked_size);
    const auto* contours = std::get_if<std::vector<Contour>>(&result.structured);
    ASSERT_NE(contours, nullptr);
    ASSERT_EQ(contours->size(), 2U);

    EXPECT_NEAR((*contours)[0].area, ranked_area(9), 1e-5F);
    EXPECT_NEAR((*contours)[1].area, ranked_area(7), 1e-5F);
}

TEST_F(VisionGpuContoursTest, MaxContoursAboveTheCountReturnsEveryContourLargestFirst)
{
    const auto image = rgba_image(k_ranked_size, k_ranked_size, ranked_squares());
    ASSERT_NE(image, nullptr);

    const auto sequence = VisionSequence::Builder {}
                              .connected_components({ .export_labels = true })
                              .find_contours({ .max_contours = 10 })
                              .build();
    VisionGpuExecutor exec;
    const auto result = exec.run(sequence, image, k_ranked_size, k_ranked_size);
    const auto* contours = std::get_if<std::vector<Contour>>(&result.structured);
    ASSERT_NE(contours, nullptr);
    ASSERT_EQ(contours->size(), k_ranked_sides.size());

    const std::array<uint32_t, 4> expected { 9, 7, 5, 3 };
    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_NEAR((*contours)[i].area, ranked_area(expected[i]), 1e-5F) << "rank " << i;
    }
}

TEST_F(VisionGpuContoursTest, MaxContoursWithAnImageDrawsOnlyTheLargestOutlines)
{
    const auto image = rgba_image(k_ranked_size, k_ranked_size, ranked_squares());
    ASSERT_NE(image, nullptr);

    const auto all_sequence = VisionSequence::Builder {}
                                  .connected_components({ .export_labels = true })
                                  .find_contours({ .as_image = true })
                                  .build();
    const auto top_sequence = VisionSequence::Builder {}
                                  .connected_components({ .export_labels = true })
                                  .find_contours({ .max_contours = 1, .as_image = true })
                                  .build();

    VisionGpuExecutor all_exec;
    const auto all_result = all_exec.run(all_sequence, image, k_ranked_size, k_ranked_size);
    ASSERT_NE(all_result.images.contour_image, nullptr);

    VisionGpuExecutor top_exec;
    const auto top_result = top_exec.run(top_sequence, image, k_ranked_size, k_ranked_size);
    ASSERT_NE(top_result.images.contour_image, nullptr);

    size_t ring_pixels = 0;
    for (const uint32_t side : k_ranked_sides) {
        ring_pixels += 4U * (side - 1U);
    }
    EXPECT_EQ(count_lit(read_floats(all_result.images.contour_image)), ring_pixels);
    EXPECT_EQ(count_lit(read_floats(top_result.images.contour_image)), size_t { 4U * (9U - 1U) });
}

} // namespace MayaFlux::Test::Vision
