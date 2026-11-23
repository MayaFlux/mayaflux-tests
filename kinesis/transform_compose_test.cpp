#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kinesis/MatrixTransforms.hpp"

#include <glm/gtc/matrix_transform.hpp>

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    bool close(const glm::vec3& a, const glm::vec3& b, float tolerance = 1e-5F)
    {
        return glm::length(a - b) <= tolerance;
    }

    glm::quat quarter_turn_about_z()
    {
        return glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0F, 0.0F, 1.0F));
    }

}

TEST(TransformComposeTest, ATransformAppliesScaleThenRotationThenTranslation)
{
    const auto matrix = Kinesis::compose_transform(
        glm::vec3(10.0F, 0.0F, 0.0F), quarter_turn_about_z(), glm::vec3(2.0F, 1.0F, 1.0F));

    const glm::vec3 moved = glm::vec3(matrix * glm::vec4(1.0F, 0.0F, 0.0F, 1.0F));

    EXPECT_TRUE(close(moved, glm::vec3(10.0F, 2.0F, 0.0F)));
}

TEST(TransformComposeTest, ComposingThenDecomposingGivesTheParts)
{
    const glm::vec3 translation(1.0F, -2.0F, 3.0F);
    const glm::vec3 scale(2.0F, 0.5F, 4.0F);

    const auto parts = Kinesis::decompose_transform(
        Kinesis::compose_transform(translation, quarter_turn_about_z(), scale));

    ASSERT_TRUE(parts.has_value());
    const auto& [t, r, s] = *parts;
    EXPECT_TRUE(close(t, translation));
    EXPECT_TRUE(close(s, scale));
    EXPECT_NEAR(std::abs(glm::dot(r, quarter_turn_about_z())), 1.0F, 1e-5F);
}

TEST(TransformComposeTest, AMirrorIsReportedAsANegativeXScale)
{
    const auto parts = Kinesis::decompose_transform(
        Kinesis::compose_transform(glm::vec3(0.0F), glm::quat(1.0F, 0.0F, 0.0F, 0.0F), glm::vec3(-1.0F, 1.0F, 1.0F)));

    ASSERT_TRUE(parts.has_value());
    EXPECT_LT(std::get<2>(*parts).x, 0.0F);
    EXPECT_GT(std::get<2>(*parts).y, 0.0F);
}

TEST(TransformComposeTest, AColumnWithNoLengthCannotBeDecomposed)
{
    glm::mat4 matrix(1.0F);
    matrix[1] = glm::vec4(0.0F);

    EXPECT_FALSE(Kinesis::decompose_transform(matrix).has_value());
}

TEST(TransformComposeTest, AnUnnormalisedRotationIsNormalisedBeforeUse)
{
    const auto matrix = Kinesis::compose_transform(
        glm::vec3(0.0F), quarter_turn_about_z() * 3.0F, glm::vec3(1.0F));

    EXPECT_NEAR(glm::length(glm::vec3(matrix[0])), 1.0F, 1e-5F);
}

}
