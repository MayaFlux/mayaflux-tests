#include "gtest/gtest.h"

#include "../../support/vision_test_support.hpp"

namespace MayaFlux::Test::Vision {

using Kinesis::Vision::BoundingBox;
using Kinesis::Vision::Keypoint;
using Kinesis::Vision::TrackResult;
using Kinesis::Vision::VisionSequence;
using Yantra::VisionGpuExecutor;

class VisionGpuRegionTest : public VisionGpuFixture { };

TEST_F(VisionGpuRegionTest, HarrisResponseIsZeroOutsideTheRegionAndKeypointsStayInside)
{
    constexpr uint32_t size = 256;
    const BoundingBox region { .x = 0.25F, .y = 0.25F, .w = 0.5F, .h = 0.5F };

    const auto frame = wave_frame(size, 0.0F, 0.0F);
    ASSERT_NE(frame, nullptr);

    const auto sequence = VisionSequence::Builder {}
                              .rgba_to_gray()
                              .harris_response({ .sigma = 1.5F, .region = region })
                              .extract_peaks({ .threshold = 0.05F, .nms_radius = 6, .region = region })
                              .build();

    VisionGpuExecutor exec;
    const auto result = exec.run(sequence, frame, size, size);
    const auto* keypoints = std::get_if<std::vector<Keypoint>>(&result.structured);
    ASSERT_NE(keypoints, nullptr);
    ASSERT_FALSE(keypoints->empty());
    ASSERT_NE(result.images.harris_response, nullptr);

    const float eps = 1.0F / static_cast<float>(size);
    for (const auto& k : *keypoints) {
        EXPECT_GE(k.position.x, region.x - eps);
        EXPECT_LE(k.position.x, region.x + region.w + eps);
        EXPECT_GE(k.position.y, region.y - eps);
        EXPECT_LE(k.position.y, region.y + region.h + eps);
    }

    const auto response = read_floats(result.images.harris_response);
    const auto x_lo = static_cast<uint32_t>(region.x * size);
    const auto x_hi = static_cast<uint32_t>((region.x + region.w) * size);
    const auto y_lo = static_cast<uint32_t>(region.y * size);
    const auto y_hi = static_cast<uint32_t>((region.y + region.h) * size);
    float max_outside = 0.0F;
    float max_inside = 0.0F;
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            const float v = red_at(response, x, y, size);
            const bool in = x >= x_lo && x < x_hi && y >= y_lo && y < y_hi;
            if (in) {
                max_inside = std::max(max_inside, v);
            } else {
                max_outside = std::max(max_outside, v);
            }
        }
    }
    EXPECT_EQ(max_outside, 0.0F);
    EXPECT_GT(max_inside, 0.99F);
}

TEST_F(VisionGpuRegionTest, PeakExtractionRestrictedToARegionKeepsExactlyTheInteriorPeaks)
{
    constexpr uint32_t size = 256;
    const BoundingBox region { .x = 0.25F, .y = 0.25F, .w = 0.5F, .h = 0.5F };

    const auto frame = wave_frame(size, 0.0F, 0.0F);
    ASSERT_NE(frame, nullptr);

    const auto run = [&](const std::optional<BoundingBox>& peaks_region) {
        const auto sequence = VisionSequence::Builder {}
                                  .rgba_to_gray()
                                  .harris_response({ .sigma = 1.5F })
                                  .extract_peaks({ .threshold = 0.05F, .nms_radius = 6, .region = peaks_region })
                                  .build();
        VisionGpuExecutor exec;
        const auto result = exec.run(sequence, frame, size, size);
        const auto* keypoints = std::get_if<std::vector<Keypoint>>(&result.structured);
        return keypoints ? *keypoints : std::vector<Keypoint> {};
    };

    const auto full = run(std::nullopt);
    const auto masked = run(region);
    const auto inside = [&](const Keypoint& k) {
        return k.position.x >= region.x && k.position.x < region.x + region.w
            && k.position.y >= region.y && k.position.y < region.y + region.h;
    };
    const auto expected = static_cast<size_t>(std::ranges::count_if(full, inside));

    EXPECT_GT(expected, 0U);
    EXPECT_EQ(masked.size(), expected);
    EXPECT_TRUE(std::ranges::all_of(masked, inside));
}

TEST_F(VisionGpuRegionTest, TrackingInsideARegionFollowsTheShiftWithinOnePixel)
{
    constexpr uint32_t size = 256;
    constexpr float dx = 3.0F;
    constexpr float dy = -2.0F;
    const BoundingBox region { .x = 0.25F, .y = 0.25F, .w = 0.5F, .h = 0.5F };

    const auto frame0 = wave_frame(size, 0.0F, 0.0F);
    const auto frame1 = wave_frame(size, dx, dy);
    ASSERT_NE(frame0, nullptr);
    ASSERT_NE(frame1, nullptr);

    const auto sequence = VisionSequence::Builder {}
                              .rgba_to_gray()
                              .harris_response({ .sigma = 1.5F, .region = region })
                              .extract_peaks({ .threshold = 0.05F, .nms_radius = 6, .region = region })
                              .track_keypoints({ .error_threshold = 0.2F })
                              .build();
    VisionGpuExecutor exec;
    static_cast<void>(exec.run(sequence, frame0, size, size));
    const auto result = exec.run(sequence, frame1, size, size);
    const auto* tracks = std::get_if<std::vector<TrackResult>>(&result.structured);
    ASSERT_NE(tracks, nullptr);
    ASSERT_GE(tracks->size(), 4U);

    const float margin = 8.0F / static_cast<float>(size);
    glm::vec2 mean_shift { 0.0F, 0.0F };
    for (const auto& t : *tracks) {
        mean_shift += t.position - t.previous;
        EXPECT_GE(t.position.x, region.x - margin);
        EXPECT_LE(t.position.x, region.x + region.w + margin);
        EXPECT_GE(t.position.y, region.y - margin);
        EXPECT_LE(t.position.y, region.y + region.h + margin);
    }
    mean_shift *= static_cast<float>(size) / static_cast<float>(tracks->size());
    EXPECT_NEAR(mean_shift.x, dx, 1.0F);
    EXPECT_NEAR(mean_shift.y, dy, 1.0F);
}

} // namespace MayaFlux::Test::Vision
