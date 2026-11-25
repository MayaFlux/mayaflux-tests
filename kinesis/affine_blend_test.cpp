#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kinesis/Polyhedral/AffineBlend.hpp"

#include <glm/gtc/matrix_transform.hpp>

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    constexpr float k_palette_tolerance = 1.0e-4F;

    void expect_near(const glm::mat4& a, const glm::mat4& b)
    {
        for (int column = 0; column < 4; ++column) {
            for (int row = 0; row < 4; ++row) {
                EXPECT_NEAR(a[column][row], b[column][row], k_palette_tolerance) << column << "," << row;
            }
        }
    }

    glm::mat4 joint_at(float x, float y, float z, float degrees = 0.0F)
    {
        return glm::translate(glm::mat4(1.0F), glm::vec3(x, y, z))
            * glm::rotate(glm::mat4(1.0F), glm::radians(degrees), glm::vec3(0.0F, 0.0F, 1.0F));
    }

    TEST(AffineBlendTest, AJointStillInItsBindPoseGivesTheIdentity)
    {
        const std::vector<glm::mat4> placements { joint_at(1.0F, 2.0F, 3.0F, 30.0F), joint_at(0.0F, 4.0F, 0.0F) };
        const std::vector<glm::mat4> inverse_bind { glm::inverse(placements[0]), glm::inverse(placements[1]) };

        const auto palette = Kinesis::relative_transforms(glm::mat4(1.0F), placements, inverse_bind);

        ASSERT_EQ(palette.size(), 2U);
        expect_near(palette[0], glm::mat4(1.0F));
        expect_near(palette[1], glm::mat4(1.0F));
    }

    TEST(AffineBlendTest, AMovedJointCarriesTheMeshByTheMoveItMade)
    {
        const glm::mat4 bind = joint_at(0.0F, 1.0F, 0.0F);
        const glm::mat4 moved = joint_at(0.0F, 1.0F, 0.0F, 90.0F);
        const std::vector<glm::mat4> placements { moved };
        const std::vector<glm::mat4> inverse_bind { glm::inverse(bind) };

        const auto palette = Kinesis::relative_transforms(glm::mat4(1.0F), placements, inverse_bind);

        ASSERT_EQ(palette.size(), 1U);
        const glm::vec4 carried = palette[0] * glm::vec4(1.0F, 1.0F, 0.0F, 1.0F);
        EXPECT_NEAR(carried.x, 0.0F, k_palette_tolerance);
        EXPECT_NEAR(carried.y, 2.0F, k_palette_tolerance);
    }

    TEST(AffineBlendTest, TheMeshsOwnPlacementCancelsSoItIsDrawnWhereTheJointsPutIt)
    {
        const glm::mat4 mesh = joint_at(5.0F, 0.0F, 0.0F, 45.0F) * glm::scale(glm::mat4(1.0F), glm::vec3(3.0F));
        const std::vector<glm::mat4> placements { joint_at(1.0F, 2.0F, 0.0F, 20.0F) };
        const std::vector<glm::mat4> inverse_bind { glm::inverse(joint_at(0.0F, 1.0F, 0.0F)) };

        const auto palette = Kinesis::relative_transforms(mesh, placements, inverse_bind);

        ASSERT_EQ(palette.size(), 1U);
        expect_near(mesh * palette[0], placements[0] * inverse_bind[0]);
    }

    TEST(AffineBlendTest, ASizeMismatchOrAMeshThatCannotBeInvertedGivesNothing)
    {
        const std::vector<glm::mat4> placements { glm::mat4(1.0F), glm::mat4(1.0F) };
        const std::vector<glm::mat4> inverse_bind { glm::mat4(1.0F) };
        const glm::mat4 flat = glm::scale(glm::mat4(1.0F), glm::vec3(1.0F, 1.0F, 0.0F));

        EXPECT_TRUE(Kinesis::relative_transforms(glm::mat4(1.0F), placements, inverse_bind).empty());
        EXPECT_TRUE(Kinesis::relative_transforms(flat, inverse_bind, inverse_bind).empty());
    }

    TEST(AffineBlendTest, NoJointsGiveAnEmptyPalette)
    {
        EXPECT_TRUE(Kinesis::relative_transforms(glm::mat4(1.0F), {}, {}).empty());
    }

} // namespace
} // namespace MayaFlux::Test
