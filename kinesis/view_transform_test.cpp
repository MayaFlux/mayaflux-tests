#include "gtest/gtest.h"

#include "MayaFlux/Kinesis/Spatial/Bounds.hpp"

namespace MayaFlux::Test {

using Kinesis::AABB2D;

TEST(ViewTransformHelperTest, ToNdcMapsWindowCornersWithAFlippedY)
{
    EXPECT_EQ(Kinesis::to_ndc(0.0, 0.0, 200, 100), glm::vec3(-1.F, 1.F, 0.F));
    EXPECT_EQ(Kinesis::to_ndc(200.0, 100.0, 200, 100), glm::vec3(1.F, -1.F, 0.F));
    EXPECT_EQ(Kinesis::to_ndc(100.0, 50.0, 200, 100), glm::vec3(0.F, 0.F, 0.F));
    EXPECT_EQ(Kinesis::to_ndc(50.0, 25.0, 200, 100), glm::vec3(-0.5F, 0.5F, 0.F));
}

TEST(ViewTransformHelperTest, ToWindowInvertsToNdc)
{
    for (const glm::vec2 pixel : { glm::vec2(0.F, 0.F), glm::vec2(50.F, 25.F), glm::vec2(199.F, 99.F) }) {
        const glm::vec3 ndc = Kinesis::to_ndc(pixel.x, pixel.y, 200, 100);
        const glm::vec2 back = Kinesis::to_window(ndc, 200, 100);
        EXPECT_NEAR(back.x, pixel.x, 1e-4F);
        EXPECT_NEAR(back.y, pixel.y, 1e-4F);
    }
}

TEST(ViewTransformHelperTest, ToNdcAspectStretchesTheLongerAxisBeyondOne)
{
    const glm::vec3 landscape = Kinesis::to_ndc_aspect(200.0, 50.0, 200, 100);
    EXPECT_FLOAT_EQ(landscape.x, 2.F);
    EXPECT_FLOAT_EQ(landscape.y, 0.F);

    const glm::vec3 portrait = Kinesis::to_ndc_aspect(50.0, 0.0, 100, 200);
    EXPECT_FLOAT_EQ(portrait.x, 0.F);
    EXPECT_FLOAT_EQ(portrait.y, 2.F);

    const glm::vec3 square = Kinesis::to_ndc_aspect(50.0, 0.0, 100, 100);
    EXPECT_EQ(square, glm::vec3(0.F, 1.F, 0.F));
}

TEST(ViewTransformHelperTest, AspectRatioIsWidthOverHeight)
{
    EXPECT_FLOAT_EQ(Kinesis::aspect_ratio(200, 100), 2.F);
    EXPECT_FLOAT_EQ(Kinesis::aspect_ratio(100, 200), 0.5F);
}

TEST(ViewTransformHelperTest, InBoundsIncludesTheOriginAndExcludesTheFarEdges)
{
    EXPECT_TRUE(Kinesis::in_bounds(0.0, 0.0, 200, 100));
    EXPECT_TRUE(Kinesis::in_bounds(199.9, 99.9, 200, 100));
    EXPECT_FALSE(Kinesis::in_bounds(200.0, 50.0, 200, 100));
    EXPECT_FALSE(Kinesis::in_bounds(50.0, 100.0, 200, 100));
    EXPECT_FALSE(Kinesis::in_bounds(-0.1, 50.0, 200, 100));
}

TEST(ViewTransformHelperTest, NdcSizeToPixelsScalesTruncatesAndNeverReturnsZero)
{
    EXPECT_EQ(Kinesis::ndc_size_to_pixels(glm::vec2(2.F, 1.F), 200, 100), glm::uvec2(200U, 50U));
    EXPECT_EQ(Kinesis::ndc_size_to_pixels(glm::vec2(0.51F, 0.51F), 100, 100), glm::uvec2(25U, 25U));
    EXPECT_EQ(Kinesis::ndc_size_to_pixels(glm::vec2(0.F, 0.F), 200, 100), glm::uvec2(1U, 1U));
}

TEST(ViewTransformHelperTest, NdcSizeToPixelsOfARegionUsesItsExtent)
{
    const AABB2D region { .min = { -0.5F, -0.25F }, .max = { 0.5F, 0.25F } };

    EXPECT_EQ(Kinesis::ndc_size_to_pixels(region, 200, 100), glm::uvec2(100U, 25U));
}

TEST(ViewTransformHelperTest, LookAtPerspectiveMovesTheEyeToTheViewOrigin)
{
    const auto vt = Kinesis::look_at_perspective({ 0.F, 0.F, 5.F }, { 0.F, 0.F, 0.F }, 1.5707964F, 1.F, 0.1F, 100.F);

    const glm::vec4 eye = vt.view * glm::vec4(0.F, 0.F, 5.F, 1.F);
    const glm::vec4 target = vt.view * glm::vec4(0.F, 0.F, 0.F, 1.F);

    EXPECT_NEAR(eye.x, 0.F, 1e-5F);
    EXPECT_NEAR(eye.y, 0.F, 1e-5F);
    EXPECT_NEAR(eye.z, 0.F, 1e-5F);
    EXPECT_NEAR(target.z, -5.F, 1e-5F);
    EXPECT_NE(vt.projection, glm::mat4(1.F));
}

TEST(ViewTransformHelperTest, ScreenToRayPointsDownTheViewAxisAtTheCentreAndSideways)
{
    const auto vt = Kinesis::look_at_perspective({ 0.F, 0.F, 5.F }, { 0.F, 0.F, 0.F }, 1.5707964F, 1.F, 0.1F, 100.F);

    const Kinesis::Ray centre = Kinesis::screen_to_ray(100.0, 100.0, 200, 200, vt);
    const Kinesis::Ray right = Kinesis::screen_to_ray(180.0, 100.0, 200, 200, vt);
    const Kinesis::Ray up = Kinesis::screen_to_ray(100.0, 20.0, 200, 200, vt);

    EXPECT_NEAR(centre.direction.x, 0.F, 1e-4F);
    EXPECT_NEAR(centre.direction.y, 0.F, 1e-4F);
    EXPECT_NEAR(centre.direction.z, -1.F, 1e-4F);
    EXPECT_GT(right.direction.x, 0.1F);
    EXPECT_GT(up.direction.y, 0.1F);
    EXPECT_NEAR(glm::length(right.direction), 1.F, 1e-4F);
}

}
