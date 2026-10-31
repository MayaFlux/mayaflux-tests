#include "gtest/gtest.h"

#include "MayaFlux/Kinesis/ColorSpace.hpp"

namespace MayaFlux::Test {

namespace {

    void expect_near(glm::vec3 actual, glm::vec3 expected, float tolerance = 1e-5F)
    {
        EXPECT_NEAR(actual.x, expected.x, tolerance);
        EXPECT_NEAR(actual.y, expected.y, tolerance);
        EXPECT_NEAR(actual.z, expected.z, tolerance);
    }

}

TEST(KinesisColorSpaceTest, TheSixPrimariesAndSecondariesLandOnTheirHues)
{
    expect_near(Kinesis::hsv_to_rgb({ 0.F, 1.F, 1.F }), { 1.F, 0.F, 0.F });
    expect_near(Kinesis::hsv_to_rgb({ 1.F / 6.F, 1.F, 1.F }), { 1.F, 1.F, 0.F });
    expect_near(Kinesis::hsv_to_rgb({ 2.F / 6.F, 1.F, 1.F }), { 0.F, 1.F, 0.F });
    expect_near(Kinesis::hsv_to_rgb({ 3.F / 6.F, 1.F, 1.F }), { 0.F, 1.F, 1.F });
    expect_near(Kinesis::hsv_to_rgb({ 4.F / 6.F, 1.F, 1.F }), { 0.F, 0.F, 1.F });
    expect_near(Kinesis::hsv_to_rgb({ 5.F / 6.F, 1.F, 1.F }), { 1.F, 0.F, 1.F });
}

TEST(KinesisColorSpaceTest, ZeroSaturationIsAGreyAtTheValue)
{
    expect_near(Kinesis::hsv_to_rgb({ 0.37F, 0.F, 0.6F }), { 0.6F, 0.6F, 0.6F });
}

TEST(KinesisColorSpaceTest, ZeroValueIsBlackWhateverTheHue)
{
    expect_near(Kinesis::hsv_to_rgb({ 0.8F, 1.F, 0.F }), { 0.F, 0.F, 0.F });
}

TEST(KinesisColorSpaceTest, HueWrapsAndOtherComponentsClamp)
{
    expect_near(Kinesis::hsv_to_rgb({ 1.F, 1.F, 1.F }), Kinesis::hsv_to_rgb({ 0.F, 1.F, 1.F }));
    expect_near(Kinesis::hsv_to_rgb({ 1.25F, 1.F, 1.F }), Kinesis::hsv_to_rgb({ 0.25F, 1.F, 1.F }));
    expect_near(Kinesis::hsv_to_rgb({ 0.F, 2.F, 2.F }), { 1.F, 0.F, 0.F });
}

TEST(KinesisColorSpaceTest, AGreyHasZeroHueAndZeroSaturation)
{
    expect_near(Kinesis::rgb_to_hsv({ 0.4F, 0.4F, 0.4F }), { 0.F, 0.F, 0.4F });
    expect_near(Kinesis::rgb_to_hsv({ 0.F, 0.F, 0.F }), { 0.F, 0.F, 0.F });
}

TEST(KinesisColorSpaceTest, RgbSurvivesAHsvRoundTrip)
{
    const std::array<glm::vec3, 5> samples { {
        { 0.9F, 0.4F, 0.1F },
        { 0.1F, 0.7F, 0.3F },
        { 0.2F, 0.2F, 0.8F },
        { 1.F, 1.F, 0.F },
        { 0.3F, 0.3F, 0.3F },
    } };

    for (const auto& rgb : samples)
        expect_near(Kinesis::hsv_to_rgb(Kinesis::rgb_to_hsv(rgb)), rgb, 1e-4F);
}

}
