#include "gtest/gtest.h"

#include "../../support/vision_test_support.hpp"

namespace MayaFlux::Test::Vision {

using Kinesis::Vision::ChannelMask;
using Kinesis::Vision::ComponentResult;
using Kinesis::Vision::Contour;
using Kinesis::Vision::Keypoint;
using Kinesis::Vision::VisionSequence;
using Yantra::VisionGpuExecutor;

namespace {

struct DownsampleCase {
    const char* name;
    bool corners;
    bool downsample;
    bool gray;
    bool outer_blur;
};

struct DownsampleBounds {
    float min_x { 1.0F };
    float min_y { 1.0F };
    float max_x { 0.0F };
    float max_y { 0.0F };
    size_t count { 0 };

    void add(float x, float y)
    {
        min_x = std::min(min_x, x);
        min_y = std::min(min_y, y);
        max_x = std::max(max_x, x);
        max_y = std::max(max_y, y);
        ++count;
    }

    [[nodiscard]] bool matches(float x0, float y0, float x1, float y1, float tolerance) const
    {
        return count > 0
            && std::abs(min_x - x0) <= tolerance && std::abs(min_y - y0) <= tolerance
            && std::abs(max_x - x1) <= tolerance && std::abs(max_y - y1) <= tolerance;
    }
};

struct ExpectedBox {
    uint32_t x_min;
    uint32_t y_min;
    uint32_t x_max;
    uint32_t y_max;
};

std::vector<std::array<uint32_t, 4>> component_pixel_boxes(const ComponentResult& cc, uint32_t w, uint32_t h)
{
    std::vector<std::array<uint32_t, 4>> out;
    out.reserve(cc.boxes.size());
    for (const auto& b : cc.boxes) {
        out.push_back(pixel_box(b, w, h));
    }
    return out;
}

bool contains_box(const std::vector<std::array<uint32_t, 4>>& boxes, const ExpectedBox& expected)
{
    return std::ranges::any_of(boxes, [&](const std::array<uint32_t, 4>& b) {
        return b[0] == expected.x_min && b[1] == expected.y_min
            && b[2] == expected.x_max && b[3] == expected.y_max;
    });
}

} // namespace

class VisionGpuDownsampleTest : public VisionGpuFixture, public ::testing::WithParamInterface<DownsampleCase> { };

TEST_P(VisionGpuDownsampleTest, DownsampleLeavesTheSceneWhereItWas)
{
    const auto c = GetParam();
    constexpr uint32_t W = 256;
    constexpr uint32_t H = 128;
    constexpr float x0 = 0.125F;
    constexpr float y0 = 0.25F;
    constexpr float x1 = 0.5F;
    constexpr float y1 = 0.75F;
    constexpr float tolerance = 0.04F;

    std::vector<float> pixels(static_cast<size_t>(W) * H * 4, 0.0F);
    for (uint32_t y = 0; y < H; ++y) {
        for (uint32_t x = 0; x < W; ++x) {
            const float fx = static_cast<float>(x) / static_cast<float>(W);
            const float fy = static_cast<float>(y) / static_cast<float>(H);
            const float v = (fx >= x0 && fx < x1 && fy >= y0 && fy < y1) ? 1.0F : 0.0F;
            set_all_channels(pixels, W, x, y, v);
            pixels[(static_cast<size_t>(y) * W + x) * 4 + 3] = 1.0F;
        }
    }
    const auto image = rgba_image(W, H, pixels);
    ASSERT_NE(image, nullptr);

    VisionSequence::Builder builder;
    if (c.gray) {
        builder.rgba_to_gray();
    }
    if (c.downsample) {
        builder.downsample_2x();
    }
    if (c.corners) {
        if (c.outer_blur) {
            builder.gaussian_blur({ .sigma = 1.0F });
        }
        builder.harris_response().extract_peaks({ .threshold = 0.05F, .nms_radius = 4 });
    } else {
        builder.threshold({ .value = 0.5F }).connected_components({ .export_labels = true, .with_colors = true }).find_contours();
    }
    const auto sequence = builder.build();

    VisionGpuExecutor exec;
    const auto result = exec.run(sequence, image, W, H);

    DownsampleBounds bounds;
    if (c.corners) {
        const auto* keypoints = std::get_if<std::vector<Keypoint>>(&result.structured);
        ASSERT_NE(keypoints, nullptr);
        const std::array<glm::vec2, 4> expected { { { x0, y0 }, { x1, y0 }, { x0, y1 }, { x1, y1 } } };
        std::array<bool, 4> matched_corners {};
        bool near_corners = true;
        for (const auto& kp : *keypoints) {
            bounds.add(kp.position.x, kp.position.y);
            bool near = false;
            for (size_t i = 0; i < expected.size(); ++i) {
                const auto& corner = expected[i];
                if (std::abs(kp.position.x - corner.x) <= tolerance
                    && std::abs(kp.position.y - corner.y) <= tolerance) {
                    matched_corners[i] = true;
                    near = true;
                }
            }
            near_corners = near_corners && near;
        }
        EXPECT_EQ(bounds.count, 4U);
        EXPECT_TRUE(near_corners);
        EXPECT_TRUE(std::ranges::all_of(matched_corners, [](bool found) { return found; }));
    } else {
        const auto* contours = std::get_if<std::vector<Contour>>(&result.structured);
        ASSERT_NE(contours, nullptr);
        for (const auto& contour : *contours) {
            for (const auto& p : contour.points) {
                bounds.add(p.x, p.y);
            }
        }
        EXPECT_TRUE(bounds.matches(x0, y0, x1, y1, tolerance))
            << "x=[" << bounds.min_x << ", " << bounds.max_x << "] y=[" << bounds.min_y << ", " << bounds.max_y << "]";
    }
}

INSTANTIATE_TEST_SUITE_P(
    DownsampleCases,
    VisionGpuDownsampleTest,
    ::testing::Values(
        DownsampleCase { "contours_control", false, false, true, true },
        DownsampleCase { "contours_downsample", false, true, true, true },
        DownsampleCase { "corners_control", true, false, true, true },
        DownsampleCase { "corners_downsample", true, true, true, true },
        DownsampleCase { "corners_gray_harris", true, false, true, false },
        DownsampleCase { "corners_blur_harris", true, false, false, true },
        DownsampleCase { "corners_harris_only", true, false, false, false },
        DownsampleCase { "corners_gray_downsample_harris", true, true, true, false },
        DownsampleCase { "corners_downsample_harris", true, true, false, false }),
    [](const ::testing::TestParamInfo<DownsampleCase>& info) { return std::string(info.param.name); });

class VisionGpuOtsuTest : public VisionGpuFixture { };

TEST_F(VisionGpuOtsuTest, ThreeFramesOnOneExecutorIsolateTheSquareEachTime)
{
    constexpr uint32_t W = 16;
    constexpr uint32_t H = 16;

    const auto make_image = [&](float background, float foreground, const ExpectedBox& square) {
        return pattern_frame(W, [&](uint32_t x, uint32_t y) {
            const bool inside = x >= square.x_min && x <= square.x_max && y >= square.y_min && y <= square.y_max;
            const float v = inside ? foreground : background;
            return glm::vec4 { v, v, v, 1.0F };
        });
    };

    struct Case {
        float background;
        float foreground;
        ExpectedBox square;
        ExpectedBox expected;
    };
    const std::vector<Case> cases {
        { 0.2F, 0.9F, { 4, 4, 7, 7 }, { 4, 4, 7, 7 } },
        { 0.85F, 0.1F, { 1, 1, 2, 2 }, { 0, 0, 15, 15 } },
        { 0.15F, 0.95F, { 10, 8, 13, 11 }, { 10, 8, 13, 11 } },
    };

    const auto sequence = VisionSequence::Builder {}
                              .threshold_otsu()
                              .connected_components({ .export_labels = true, .with_colors = true })
                              .build();
    VisionGpuExecutor exec;
    for (const auto& c : cases) {
        const auto image = make_image(c.background, c.foreground, c.square);
        ASSERT_NE(image, nullptr);
        const auto result = exec.run(sequence, image, W, H);
        const auto* cc = std::get_if<ComponentResult>(&result.structured);
        ASSERT_NE(cc, nullptr);
        EXPECT_EQ(cc->count, 1U);
        EXPECT_TRUE(contains_box(component_pixel_boxes(*cc, W, H), c.expected));
    }
}

class VisionGpuMorphologyTest : public VisionGpuFixture { };

TEST_F(VisionGpuMorphologyTest, OpenRemovesTheSpeckAndKeepsTheSquareExactly)
{
    constexpr uint32_t W = 16;
    constexpr uint32_t H = 16;
    const ExpectedBox square { 2, 2, 9, 9 };

    std::vector<float> pixels(static_cast<size_t>(W) * H * 4, 0.0F);
    for (uint32_t y = square.y_min; y <= square.y_max; ++y) {
        for (uint32_t x = square.x_min; x <= square.x_max; ++x) {
            set_all_channels(pixels, W, x, y, 1.0F);
        }
    }
    set_all_channels(pixels, W, 13, 13, 1.0F);
    const auto image = rgba_image(W, H, pixels);
    ASSERT_NE(image, nullptr);

    const auto sequence = VisionSequence::Builder {}
                              .open({ .radius = 1 })
                              .connected_components({ .export_labels = true, .with_colors = true })
                              .build();
    VisionGpuExecutor exec;
    const auto result = exec.run(sequence, image, W, H);
    const auto* cc = std::get_if<ComponentResult>(&result.structured);
    ASSERT_NE(cc, nullptr);
    EXPECT_EQ(cc->count, 1U);
    EXPECT_TRUE(contains_box(component_pixel_boxes(*cc, W, H), square));
}

TEST_F(VisionGpuMorphologyTest, CloseKeepsTheSquareExactlyAndAddsNoComponent)
{
    constexpr uint32_t W = 16;
    constexpr uint32_t H = 16;
    const ExpectedBox square { 2, 2, 9, 9 };

    std::vector<float> pixels(static_cast<size_t>(W) * H * 4, 0.0F);
    for (uint32_t y = square.y_min; y <= square.y_max; ++y) {
        for (uint32_t x = square.x_min; x <= square.x_max; ++x) {
            set_all_channels(pixels, W, x, y, 1.0F);
        }
    }
    const auto image = rgba_image(W, H, pixels);
    ASSERT_NE(image, nullptr);

    const auto sequence = VisionSequence::Builder {}
                              .close({ .radius = 1 })
                              .connected_components({ .export_labels = true, .with_colors = true })
                              .build();
    VisionGpuExecutor exec;
    const auto result = exec.run(sequence, image, W, H);
    const auto* cc = std::get_if<ComponentResult>(&result.structured);
    ASSERT_NE(cc, nullptr);
    EXPECT_EQ(cc->count, 1U);
    EXPECT_TRUE(contains_box(component_pixel_boxes(*cc, W, H), square));
}

class VisionGpuCannyTest : public VisionGpuFixture { };

TEST_F(VisionGpuCannyTest, EdgesFormTheRingAroundTheSquareAndRepeatExactly)
{
    constexpr uint32_t W = 32;
    constexpr uint32_t H = 32;
    constexpr uint32_t sq0 = 8;
    constexpr uint32_t sq1 = 23;
    constexpr uint32_t band_lo = sq0 - 2;
    constexpr uint32_t band_hi = sq1 + 2;
    constexpr uint32_t inner_lo = sq0 + 2;
    constexpr uint32_t inner_hi = sq1 - 2;
    constexpr uint32_t side_len = inner_hi - inner_lo + 1;

    const auto image = pattern_frame(W, [](uint32_t x, uint32_t y) {
        const float v = (x >= sq0 && x <= sq1 && y >= sq0 && y <= sq1) ? 1.0F : 0.0F;
        return glm::vec4 { v, v, v, 1.0F };
    });
    ASSERT_NE(image, nullptr);

    const auto sequence = VisionSequence::Builder {}
                              .rgba_to_gray()
                              .canny({ .sigma = 1.0F, .low_threshold = 0.1F, .high_threshold = 0.3F })
                              .build();
    VisionGpuExecutor exec;
    const auto first_result = exec.run(sequence, image, W, H);
    ASSERT_NE(first_result.images.canny, nullptr);
    const auto first = read_normalised(first_result.images.canny);
    const auto second_result = exec.run(sequence, image, W, H);
    ASSERT_NE(second_result.images.canny, nullptr);
    const auto second = read_normalised(second_result.images.canny);
    ASSERT_GE(first.size(), static_cast<size_t>(W) * H * 4);

    const auto lit = [&](uint32_t x, uint32_t y) {
        return first[(static_cast<size_t>(y) * W + x) * 4] > 0.5F;
    };

    uint32_t total = 0;
    uint32_t stray = 0;
    for (uint32_t y = 0; y < H; ++y) {
        for (uint32_t x = 0; x < W; ++x) {
            if (!lit(x, y)) {
                continue;
            }
            ++total;
            const bool in_band = x >= band_lo && x <= band_hi && y >= band_lo && y <= band_hi;
            const bool in_interior = x >= inner_lo && x <= inner_hi && y >= inner_lo && y <= inner_hi;
            if (!in_band || in_interior) {
                ++stray;
            }
        }
    }

    const auto covered = [&](bool horizontal, uint32_t lo, uint32_t hi) {
        uint32_t count = 0;
        for (uint32_t t = inner_lo; t <= inner_hi; ++t) {
            for (uint32_t d = lo; d <= hi; ++d) {
                if (horizontal ? lit(t, d) : lit(d, t)) {
                    ++count;
                    break;
                }
            }
        }
        return count;
    };

    EXPECT_GT(total, 0U);
    EXPECT_EQ(stray, 0U);
    EXPECT_EQ(covered(true, band_lo, inner_lo - 1), side_len);
    EXPECT_EQ(covered(true, inner_hi + 1, band_hi), side_len);
    EXPECT_EQ(covered(false, band_lo, inner_lo - 1), side_len);
    EXPECT_EQ(covered(false, inner_hi + 1, band_hi), side_len);
    EXPECT_LE(max_abs_diff(first, second), 1e-6F);
}

class VisionGpuConfineTest : public VisionGpuFixture { };

namespace {

void expect_confine_centre_quarter(VisionGpuExecutor& exec, bool resize_to_source)
{
    constexpr uint32_t size = 32;
    constexpr uint32_t origin = size / 4;
    constexpr uint32_t region = size / 2;
    constexpr float tolerance = 0.005F;

    const auto image = pattern_frame(size, [](uint32_t x, uint32_t y) {
        return glm::vec4 {
            static_cast<float>(x) / static_cast<float>(size),
            static_cast<float>(y) / static_cast<float>(size),
            0.0F,
            1.0F,
        };
    });
    ASSERT_NE(image, nullptr);

    const auto sequence = VisionSequence::Builder {}
                              .confine({
                                  .bounds = { .x = 0.25F, .y = 0.25F, .w = 0.5F, .h = 0.5F },
                                  .resize_to_source = resize_to_source,
                              })
                              .build();
    const auto result = exec.run(sequence, image, size, size);
    const auto& confined = result.images.confine;
    ASSERT_NE(confined, nullptr);

    const uint32_t expected_dim = resize_to_source ? size : region;
    EXPECT_EQ(confined->get_width(), expected_dim);
    EXPECT_EQ(confined->get_height(), expected_dim);
    if (confined->get_width() != expected_dim || confined->get_height() != expected_dim) {
        return;
    }

    const auto out = read_normalised(confined);
    ASSERT_GE(out.size(), static_cast<size_t>(expected_dim) * expected_dim * 4);
    uint32_t mismatches = 0;
    for (uint32_t y = 0; y < expected_dim; ++y) {
        for (uint32_t x = 0; x < expected_dim; ++x) {
            const uint32_t sx = origin + (x * region) / expected_dim;
            const uint32_t sy = origin + (y * region) / expected_dim;
            const size_t idx = (static_cast<size_t>(y) * expected_dim + x) * 4;
            const float er = static_cast<float>(sx) / static_cast<float>(size);
            const float eg = static_cast<float>(sy) / static_cast<float>(size);
            if (std::abs(out[idx + 0] - er) > tolerance || std::abs(out[idx + 1] - eg) > tolerance) {
                ++mismatches;
            }
        }
    }
    EXPECT_EQ(mismatches, 0U);
}

} // namespace

TEST_F(VisionGpuConfineTest, ConfineWithoutResizeIsAOneToOneCopyOfTheRegion)
{
    VisionGpuExecutor exec;
    expect_confine_centre_quarter(exec, false);
}

TEST_F(VisionGpuConfineTest, ConfineWithResizeNearestSamplesBackToTheFrameSize)
{
    VisionGpuExecutor exec;
    expect_confine_centre_quarter(exec, true);
}

class VisionGpuThresholdTest : public VisionGpuFixture { };

namespace {

void expect_mask_samples(const std::vector<float>& pixels, uint32_t size,
    const std::vector<std::array<uint32_t, 3>>& samples)
{
    for (const auto& s : samples) {
        EXPECT_NEAR(red_at(pixels, s[0], s[1], size), static_cast<float>(s[2]), 0.01F)
            << "(" << s[0] << "," << s[1] << ")";
    }
}

} // namespace

TEST_F(VisionGpuThresholdTest, LegacyThresholdReadsChannelZeroOnly)
{
    constexpr uint32_t size = 16;
    const auto image = pattern_frame(size, [](uint32_t x, uint32_t) {
        return x < size / 2 ? glm::vec4 { 0.8F, 0.0F, 0.0F, 1.0F } : glm::vec4 { 0.2F, 1.0F, 0.0F, 1.0F };
    });
    ASSERT_NE(image, nullptr);

    const auto sequence = VisionSequence::Builder {}.threshold({ .value = 0.5F }).build();
    VisionGpuExecutor exec;
    const auto result = exec.run(sequence, image, size, size);
    ASSERT_NE(result.images.threshold, nullptr);
    expect_mask_samples(read_floats(result.images.threshold), size, { { 2, 8, 1 }, { 13, 8, 0 } });
}

TEST_F(VisionGpuThresholdTest, BandsOnTwoChannelsAreAndedAcrossQuadrants)
{
    constexpr uint32_t size = 16;
    const auto image = pattern_frame(size, [](uint32_t x, uint32_t y) {
        const bool left = x < size / 2;
        const bool top = y < size / 2;
        if (top && left) {
            return glm::vec4 { 0.2F, 0.8F, 0.1F, 1.0F };
        }
        if (top) {
            return glm::vec4 { 0.2F, 0.2F, 0.1F, 1.0F };
        }
        if (left) {
            return glm::vec4 { 0.9F, 0.8F, 0.1F, 1.0F };
        }
        return glm::vec4 { 0.25F, 0.75F, 0.9F, 1.0F };
    });
    ASSERT_NE(image, nullptr);

    const auto sequence = VisionSequence::Builder {}
                              .threshold({
                                  .value = 0.0F,
                                  .channels = ChannelMask::Ch0 | ChannelMask::Ch1,
                                  .bands = { { { .lo = 0.1F, .hi = 0.3F }, { .lo = 0.7F, .hi = 0.9F }, {} } },
                              })
                              .build();
    VisionGpuExecutor exec;
    const auto result = exec.run(sequence, image, size, size);
    ASSERT_NE(result.images.threshold, nullptr);
    expect_mask_samples(read_floats(result.images.threshold), size,
        { { 3, 3, 1 }, { 12, 3, 0 }, { 3, 12, 0 }, { 12, 12, 1 } });
}

TEST_F(VisionGpuThresholdTest, WrappingBandSelectsBothEndsOfTheRange)
{
    constexpr uint32_t size = 15;
    const auto image = pattern_frame(size, [](uint32_t x, uint32_t) {
        if (x < 5) {
            return glm::vec4 { 0.95F, 0.0F, 0.0F, 1.0F };
        }
        if (x < 10) {
            return glm::vec4 { 0.5F, 0.0F, 0.0F, 1.0F };
        }
        return glm::vec4 { 0.05F, 0.0F, 0.0F, 1.0F };
    });
    ASSERT_NE(image, nullptr);

    const auto sequence = VisionSequence::Builder {}
                              .threshold({
                                  .value = 0.0F,
                                  .channels = ChannelMask::Ch0,
                                  .bands = { { { .lo = 0.9F, .hi = 0.1F }, {}, {} } },
                              })
                              .build();
    VisionGpuExecutor exec;
    const auto result = exec.run(sequence, image, size, size);
    ASSERT_NE(result.images.threshold, nullptr);
    expect_mask_samples(read_floats(result.images.threshold), size, { { 2, 7, 1 }, { 7, 7, 0 }, { 12, 7, 1 } });
}

TEST_F(VisionGpuThresholdTest, AdaptiveThresholdTestsEverySelectedChannel)
{
    constexpr uint32_t size = 16;
    const auto image = pattern_frame(size, [](uint32_t x, uint32_t y) {
        const float g = (x == 8 && y == 8) ? 1.0F : 0.2F;
        return glm::vec4 { 0.5F, g, 0.0F, 1.0F };
    });
    ASSERT_NE(image, nullptr);

    VisionGpuExecutor exec;
    const auto multi_sequence = VisionSequence::Builder {}
                                    .threshold_adaptive({ .block_size = 3, .offset = 0.01F, .channels = ChannelMask::Ch0 | ChannelMask::Ch1 })
                                    .build();
    const auto multi = exec.run(multi_sequence, image, size, size);
    ASSERT_NE(multi.images.threshold_adaptive, nullptr);
    expect_mask_samples(read_floats(multi.images.threshold_adaptive), size,
        { { 8, 8, 1 }, { 9, 8, 0 }, { 2, 2, 1 } });

    const auto legacy_sequence = VisionSequence::Builder {}
                                     .threshold_adaptive({ .block_size = 3, .offset = 0.01F })
                                     .build();
    const auto legacy = exec.run(legacy_sequence, image, size, size);
    ASSERT_NE(legacy.images.threshold_adaptive, nullptr);
    expect_mask_samples(read_floats(legacy.images.threshold_adaptive), size,
        { { 8, 8, 1 }, { 9, 8, 1 }, { 2, 2, 1 } });
}

TEST_F(VisionGpuThresholdTest, OtsuOnTwoChannelsSeparatesEachQuadrantByItsOwnColour)
{
    constexpr uint32_t size = 16;
    const auto image = pattern_frame(size, [](uint32_t x, uint32_t y) {
        return glm::vec4 { x < size / 2 ? 1.0F : 0.0F, y < size / 2 ? 1.0F : 0.0F, 0.0F, 1.0F };
    });
    ASSERT_NE(image, nullptr);

    VisionGpuExecutor exec;
    const auto multi_sequence = VisionSequence::Builder {}
                                    .threshold_otsu({ .channels = ChannelMask::Ch0 | ChannelMask::Ch1 })
                                    .build();
    const auto multi = exec.run(multi_sequence, image, size, size);
    ASSERT_NE(multi.images.threshold_otsu, nullptr);
    expect_mask_samples(read_floats(multi.images.threshold_otsu), size,
        { { 3, 3, 1 }, { 12, 3, 0 }, { 3, 12, 0 }, { 12, 12, 0 } });

    const auto legacy_sequence = VisionSequence::Builder {}.threshold_otsu().build();
    const auto legacy = exec.run(legacy_sequence, image, size, size);
    ASSERT_NE(legacy.images.threshold_otsu, nullptr);
    expect_mask_samples(read_floats(legacy.images.threshold_otsu), size,
        { { 3, 3, 1 }, { 12, 3, 0 }, { 3, 12, 1 }, { 12, 12, 0 } });
}

} // namespace MayaFlux::Test::Vision
