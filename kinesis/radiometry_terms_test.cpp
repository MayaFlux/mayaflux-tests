#include "gtest/gtest.h"
#include "../support/radiometry_support.hpp"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kinesis/ColorSpace.hpp"
#include "MayaFlux/Kinesis/Radiometry/Attenuation.hpp"
#include "MayaFlux/Kinesis/Radiometry/Fresnel.hpp"
#include "MayaFlux/Kinesis/Radiometry/ToneCurve.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

TEST(RadiometryAttenuationTest, InverseSquareStaysFiniteAtAndInsideItsMinimum)
{
    EXPECT_FLOAT_EQ(Kinesis::inverse_square_falloff(2.0F), 0.25F);
    EXPECT_FLOAT_EQ(Kinesis::inverse_square_falloff(0.0F), Kinesis::inverse_square_falloff(0.01F));
    EXPECT_FLOAT_EQ(Kinesis::inverse_square_falloff(0.001F, 0.5F), 4.0F);
    EXPECT_FLOAT_EQ(Kinesis::inverse_square_falloff(-2.0F), 0.25F);
}

TEST(RadiometryAttenuationTest, RangeWindowFallsFromOneToExactlyZeroAndStaysThere)
{
    EXPECT_FLOAT_EQ(Kinesis::range_window(0.0F, 3.0F), 1.0F);
    float previous = 1.0F;
    for (int i = 1; i <= 100; ++i) {
        const float value = Kinesis::range_window(3.0F * static_cast<float>(i) / 100.0F, 3.0F);
        EXPECT_LE(value, previous);
        EXPECT_GE(value, 0.0F);
        previous = value;
    }
    EXPECT_EQ(Kinesis::range_window(3.0F, 3.0F), 0.0F);
    EXPECT_EQ(Kinesis::range_window(1.0e9F, 3.0F), 0.0F);
}

TEST(RadiometryAttenuationTest, ARangeThatIsNotAboveZeroGivesNothing)
{
    EXPECT_EQ(Kinesis::range_window(0.0F, 0.0F), 0.0F);
    EXPECT_EQ(Kinesis::range_window(1.0F, -1.0F), 0.0F);
    EXPECT_EQ(Kinesis::range_window(1.0F, std::numeric_limits<float>::quiet_NaN()), 0.0F);
}

TEST(RadiometryAttenuationTest, AngularWindowSurvivesEqualAndInvertedCones)
{
    EXPECT_FLOAT_EQ(Kinesis::angular_window(0.95F, 0.9F, 0.8F), 1.0F);
    EXPECT_FLOAT_EQ(Kinesis::angular_window(0.7F, 0.9F, 0.8F), 0.0F);
    EXPECT_NEAR(Kinesis::angular_window(0.85F, 0.9F, 0.8F), 0.25F, 1.0e-5F);

    for (const float cos_angle : { -1.0F, 0.5F, 0.8F, 1.0F }) {
        for (const auto& [inner, outer] : { std::pair { 0.8F, 0.8F }, std::pair { 0.5F, 0.9F } }) {
            const float value = Kinesis::angular_window(cos_angle, inner, outer);
            EXPECT_GE(value, 0.0F);
            EXPECT_LE(value, 1.0F);
        }
    }
}

TEST(RadiometryAttenuationTest, PolynomialFalloffNeverDividesByNothing)
{
    EXPECT_FLOAT_EQ(Kinesis::polynomial_falloff(2.0F, { 1.0F, 2.0F, 3.0F }), 1.0F / 17.0F);
    EXPECT_FLOAT_EQ(Kinesis::polynomial_falloff(5.0F, glm::vec3(0.0F)), 1.0F);
    EXPECT_FLOAT_EQ(Kinesis::polynomial_falloff(5.0F, { -1.0F, 0.0F, 0.0F }), 1.0F);
}

TEST(RadiometryAttenuationTest, WeightCurvesClampOutsideTheUnitInterval)
{
    for (const auto curve : { Kinesis::falloff_linear, Kinesis::falloff_cosine,
             Kinesis::falloff_exponential, Kinesis::falloff_inverse_square }) {
        EXPECT_NEAR(curve(0.0F), 1.0F, 1.0e-6F);
        EXPECT_FLOAT_EQ(curve(-5.0F), curve(0.0F));
        EXPECT_FLOAT_EQ(curve(5.0F), curve(1.0F));
    }
    EXPECT_NEAR(Kinesis::falloff_linear(1.0F), 0.0F, 1.0e-6F);
    EXPECT_NEAR(Kinesis::falloff_cosine(0.5F), 0.5F, 1.0e-6F);
    EXPECT_NEAR(Kinesis::falloff_exponential(1.0F), std::exp(-4.0F), 1.0e-7F);
    EXPECT_NEAR(Kinesis::falloff_inverse_square(1.0F), 1.0F / 17.0F, 1.0e-7F);
}

TEST(RadiometryFresnelTest, SchlickRunsFromF0ToOneAndClampsItsCosine)
{
    const glm::vec3 f0(0.04F, 0.5F, 0.9F);
    EXPECT_EQ(Kinesis::schlick_fresnel(f0, 1.0F), f0);
    EXPECT_EQ(Kinesis::schlick_fresnel(f0, 7.0F), f0);
    EXPECT_NEAR(Kinesis::schlick_fresnel(f0, 0.0F).y, 1.0F, 1.0e-6F);
    EXPECT_NEAR(Kinesis::schlick_fresnel(f0, -3.0F).x, 1.0F, 1.0e-6F);
}

TEST(RadiometryFresnelTest, SchlickAverageIsTheCosineWeightedIntegral)
{
    const glm::vec3 f0(0.04F, 0.5F, 0.9F);
    glm::dvec3 sum(0.0);
    constexpr int steps = 20000;
    for (int i = 0; i < steps; ++i) {
        const double mu = (i + 0.5) / steps;
        sum += 2.0 * mu * glm::dvec3(Kinesis::schlick_fresnel(f0, static_cast<float>(mu))) / static_cast<double>(steps);
    }
    const glm::vec3 average = Kinesis::schlick_average(f0);
    EXPECT_NEAR(average.x, sum.x, 1.0e-5);
    EXPECT_NEAR(average.y, sum.y, 1.0e-5);
    EXPECT_NEAR(average.z, sum.z, 1.0e-5);
}

TEST(RadiometryFresnelTest, DielectricNormalIncidenceBrewsterAndTotalReflection)
{
    EXPECT_NEAR(Kinesis::dielectric_fresnel(1.0F, 1.5F), 0.04F, 1.0e-6F);
    EXPECT_NEAR(Kinesis::dielectric_fresnel(0.0F, 1.5F), 1.0F, 1.0e-5F);

    const float brewster = std::cos(std::atan(1.5F));
    const float transmitted = std::sqrt(1.0F - (1.0F - brewster * brewster) / 2.25F);
    const float across = (brewster - 1.5F * transmitted) / (brewster + 1.5F * transmitted);
    EXPECT_NEAR(Kinesis::dielectric_fresnel(brewster, 1.5F), 0.5F * across * across, 1.0e-6F);

    const float critical = std::sqrt(1.0F - 1.0F / 2.25F);
    EXPECT_FLOAT_EQ(Kinesis::dielectric_fresnel(critical - 0.01F, 1.0F / 1.5F), 1.0F);
    EXPECT_LT(Kinesis::dielectric_fresnel(critical + 0.01F, 1.0F / 1.5F), 1.0F);
}

TEST(RadiometryFresnelTest, DielectricReflectsTheSameFromEitherSide)
{
    for (const float cos_i : { 0.05F, 0.3F, 0.7F, 0.99F }) {
        const float cos_t = std::sqrt(1.0F - (1.0F - cos_i * cos_i) / 2.25F);
        EXPECT_NEAR(Kinesis::dielectric_fresnel(cos_i, 1.5F),
            Kinesis::dielectric_fresnel(cos_t, 1.0F / 1.5F), 1.0e-5F)
            << cos_i;
    }
}

TEST(RadiometryFresnelTest, ConductorWithoutAbsorptionIsTheDielectric)
{
    for (const float eta : { 0.5F, 0.8F, 1.33F, 1.5F, 2.4F }) {
        for (int i = 1; i <= 20; ++i) {
            const float cos_theta = static_cast<float>(i) / 20.0F;
            if (eta < 1.0F && std::abs(cos_theta - std::sqrt(1.0F - eta * eta)) < 0.02F) {
                continue;
            }
            const glm::vec3 conductor = Kinesis::conductor_fresnel(cos_theta, glm::vec3(eta), glm::vec3(0.0F));
            EXPECT_NEAR(conductor.x, Kinesis::dielectric_fresnel(cos_theta, eta), 1.0e-4F)
                << eta << ' ' << cos_theta;
        }
    }
}

TEST(RadiometryFresnelTest, ConductorStaysFiniteAndBoundedAtGrazingAndMatchedIndex)
{
    for (const float eta : { 0.2F, 1.0F, 3.0F }) {
        for (const float k : { 0.0F, 1.0F, 8.0F }) {
            for (const float cos_theta : { 0.0F, 1.0e-6F, 0.5F, 1.0F }) {
                const glm::vec3 r = Kinesis::conductor_fresnel(cos_theta, glm::vec3(eta), glm::vec3(k));
                ASSERT_TRUE(Radiometry::finite(r)) << eta << ' ' << k << ' ' << cos_theta;
                EXPECT_GE(r.x, -1.0e-5F);
                EXPECT_LE(r.x, 1.0F + 1.0e-5F);
            }
        }
    }
}

TEST(RadiometryFresnelTest, ConductorNormalIncidence)
{
    const glm::vec3 eta(0.2F, 0.9F, 1.5F);
    const glm::vec3 k(3.9F, 2.4F, 1.9F);
    const glm::vec3 expected = ((eta - 1.0F) * (eta - 1.0F) + k * k) / ((eta + 1.0F) * (eta + 1.0F) + k * k);
    const glm::vec3 r = Kinesis::conductor_fresnel(1.0F, eta, k);
    EXPECT_NEAR(r.x, expected.x, 1.0e-5F);
    EXPECT_NEAR(r.y, expected.y, 1.0e-5F);
    EXPECT_NEAR(r.z, expected.z, 1.0e-5F);
}

TEST(RadiometryToneCurveTest, StopsDoubleAndHalve)
{
    EXPECT_EQ(Kinesis::scale_by_stops(glm::vec3(1.0F), 1.0F), glm::vec3(2.0F));
    EXPECT_EQ(Kinesis::scale_by_stops(glm::vec3(1.0F), -2.0F), glm::vec3(0.25F));
}

TEST(RadiometryToneCurveTest, ReinhardSendsBlackToBlackAndWhiteToOne)
{
    EXPECT_EQ(Kinesis::reinhard_extended(glm::vec3(0.0F)), glm::vec3(0.0F));
    EXPECT_NEAR(Kinesis::reinhard_extended(glm::vec3(4.0F), 4.0F).x, 1.0F, 1.0e-6F);
}

TEST(RadiometryToneCurveTest, NeutralOnlyShiftsColorsBelowTheShoulder)
{
    const glm::vec3 base(0.5F, 0.3F, 0.2F);
    const glm::vec3 mapped = Kinesis::pbr_neutral(base);
    EXPECT_NEAR(mapped.x, 0.46F, 1.0e-6F);
    EXPECT_NEAR(mapped.y, 0.26F, 1.0e-6F);
    EXPECT_NEAR(mapped.z, 0.16F, 1.0e-6F);
}

TEST(RadiometryToneCurveTest, NeutralStaysInRangeAndRisesAlongGreys)
{
    float previous = -1.0F;
    for (int i = 0; i <= 4000; ++i) {
        const float grey = static_cast<float>(i) * 0.005F;
        const float mapped = Kinesis::pbr_neutral(glm::vec3(grey)).x;
        ASSERT_GE(mapped, previous - 1.0e-6F) << grey;
        ASSERT_LE(mapped, 1.0F) << grey;
        ASSERT_GE(mapped, 0.0F) << grey;
        if (i > 0) {
            ASSERT_LT(mapped - previous, 0.01F) << grey;
        }
        previous = mapped;
    }
    for (const glm::vec3 color : { glm::vec3(1.0e6F, 0.0F, 0.0F), glm::vec3(0.0F), glm::vec3(1.0e6F) }) {
        const glm::vec3 mapped = Kinesis::pbr_neutral(color);
        ASSERT_TRUE(Radiometry::finite(mapped));
        EXPECT_GE(std::min({ mapped.x, mapped.y, mapped.z }), -1.0e-6F);
        EXPECT_LE(std::max({ mapped.x, mapped.y, mapped.z }), 1.0F + 1.0e-6F);
    }
}

TEST(RadiometryTransferTest, SrgbRoundTripsAndMeetsAtItsKnee)
{
    for (int i = 0; i <= 1000; ++i) {
        const glm::vec3 value(static_cast<float>(i) / 1000.0F);
        EXPECT_NEAR(Kinesis::linear_to_srgb(Kinesis::srgb_to_linear(value)).x, value.x, 1.0e-5F);
    }
    EXPECT_NEAR(Kinesis::srgb_to_linear(glm::vec3(0.5F)).x, 0.2140411F, 1.0e-6F);
    EXPECT_NEAR(Kinesis::srgb_to_linear(glm::vec3(0.04045F)).x,
        Kinesis::srgb_to_linear(glm::vec3(0.040451F)).x, 1.0e-5F);
    EXPECT_TRUE(Radiometry::finite(Kinesis::linear_to_srgb(glm::vec3(-1.0F))));
    EXPECT_TRUE(Radiometry::finite(Kinesis::srgb_to_linear(glm::vec3(-1.0F))));
}

} // namespace MayaFlux::Test
