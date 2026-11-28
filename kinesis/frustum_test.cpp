#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kakshya/NDData/MeshAssemblyData.hpp"
#include "MayaFlux/Kinesis/Viewport/Frustum.hpp"

#include <glm/gtc/matrix_transform.hpp>

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Kakshya::MeshAssemblyLens;
    using Kinesis::AspectFit;
    using Kinesis::Frustum;

    constexpr float k_tolerance = 1.0e-4F;

    /** The reach of a 35 mm lens on a 3.6 cm film back: tan of half the horizontal field of view. */
    constexpr double k_reach = 36.0 / 70.0;

    glm::mat4 fitted(AspectFit fit, float aspect)
    {
        Frustum frustum;
        frustum.fit = fit;
        return Kinesis::frustum_projection(frustum, aspect);
    }

    TEST(FrustumTest, TheDefaultsOfALensAreAlembicsOnAThirtyFiveMillimetreFilmBack)
    {
        const MeshAssemblyLens lens;

        EXPECT_DOUBLE_EQ(lens.focal_length, 35.0);
        EXPECT_DOUBLE_EQ(lens.horizontal_aperture, 3.6);
        EXPECT_DOUBLE_EQ(lens.vertical_aperture, 2.4);
        EXPECT_FLOAT_EQ(lens.window.min.x, -1.0F);
        EXPECT_FLOAT_EQ(lens.window.max.x, 1.0F);
        EXPECT_NEAR(lens.window.max.y, 2.0F / 3.0F, k_tolerance);
    }

    TEST(FrustumTest, ALensGivesTheFrustumOfItsFilmBackAndFocalLength)
    {
        const Frustum made = Kakshya::frustum_of(MeshAssemblyLens {});
        const Frustum stock;

        EXPECT_NEAR(Kakshya::lens_half_tangent(MeshAssemblyLens {}), k_reach, 1e-12);
        EXPECT_NEAR(made.window.max.x, static_cast<float>(k_reach), k_tolerance);
        EXPECT_NEAR(made.window.max.x, stock.window.max.x, k_tolerance);
        EXPECT_NEAR(made.window.max.y, stock.window.max.y, k_tolerance);
        EXPECT_NEAR(Kinesis::frustum_horizontal_fov(made), 2.0 * std::atan(k_reach), 1e-6);
        EXPECT_NEAR(Kinesis::frustum_vertical_fov(made), 2.0 * std::atan(24.0 / 70.0), 1e-6);
    }

    TEST(FrustumTest, AWindowOfTheOutputsOwnAspectIsSymmetricAndNeedsNoFit)
    {
        const glm::mat4 projection = Kinesis::frustum_projection(Frustum {}, 1.5F);

        EXPECT_NEAR(projection[0][0], static_cast<float>(1.0 / k_reach), k_tolerance);
        EXPECT_NEAR(projection[1][1], static_cast<float>(1.0 / ((2.0 / 3.0) * k_reach)), k_tolerance);
        EXPECT_NEAR(projection[2][0], 0.0F, k_tolerance);
        EXPECT_NEAR(projection[2][1], 0.0F, k_tolerance);
        EXPECT_FLOAT_EQ(projection[2][3], -1.0F);
    }

    TEST(FrustumTest, CoverFillsTheOutputAndCropsWhatOverhangs)
    {
        const glm::mat4 wider = fitted(AspectFit::Cover, 2.0F);
        EXPECT_NEAR(wider[0][0], static_cast<float>(1.0 / k_reach), k_tolerance);
        EXPECT_NEAR(wider[1][1], static_cast<float>(1.0 / (0.5 * k_reach)), k_tolerance);

        const glm::mat4 taller = fitted(AspectFit::Cover, 1.0F);
        EXPECT_NEAR(taller[1][1], static_cast<float>(1.0 / ((2.0 / 3.0) * k_reach)), k_tolerance);
        EXPECT_NEAR(taller[0][0], static_cast<float>(1.0 / ((2.0 / 3.0) * k_reach)), k_tolerance);
    }

    TEST(FrustumTest, ContainShowsTheWholeWindowInsideTheOutput)
    {
        const glm::mat4 wider = fitted(AspectFit::Contain, 2.0F);
        EXPECT_NEAR(wider[1][1], static_cast<float>(1.0 / ((2.0 / 3.0) * k_reach)), k_tolerance);
        EXPECT_NEAR(wider[0][0], static_cast<float>(1.0 / ((4.0 / 3.0) * k_reach)), k_tolerance);

        const glm::mat4 taller = fitted(AspectFit::Contain, 1.0F);
        EXPECT_NEAR(taller[0][0], static_cast<float>(1.0 / k_reach), k_tolerance);
    }

    TEST(FrustumTest, WidthAndHeightHoldOneSideWhateverTheOutput)
    {
        const glm::mat4 by_width = fitted(AspectFit::Width, 1.0F);
        EXPECT_NEAR(by_width[0][0], static_cast<float>(1.0 / k_reach), k_tolerance);
        EXPECT_NEAR(by_width[1][1], static_cast<float>(1.0 / k_reach), k_tolerance);

        const glm::mat4 by_height = fitted(AspectFit::Height, 1.0F);
        EXPECT_NEAR(by_height[1][1], static_cast<float>(1.0 / ((2.0 / 3.0) * k_reach)), k_tolerance);
        EXPECT_NEAR(by_height[0][0], static_cast<float>(1.0 / ((2.0 / 3.0) * k_reach)), k_tolerance);
    }

    TEST(FrustumTest, AWindowOffCentreMakesAnOffAxisFrustum)
    {
        MeshAssemblyLens lens;
        lens.window = { .min = { -0.5F, -2.0F / 3.0F }, .max = { 1.5F, 2.0F / 3.0F } };
        Frustum frustum = Kakshya::frustum_of(lens);
        frustum.fit = AspectFit::Width;

        const glm::mat4 projection = Kinesis::frustum_projection(frustum, 1.5F);

        EXPECT_NEAR(projection[2][0], 0.5F, k_tolerance);
        EXPECT_NEAR(projection[0][0], static_cast<float>(1.0 / k_reach), k_tolerance);
    }

    TEST(FrustumTest, AFrustumMadeFromAFieldOfViewProjectsLikeAPerspective)
    {
        const double fov = glm::radians(60.0);
        const Frustum frustum = Kinesis::frustum_from_fov(fov, 16.0 / 9.0, 0.1, 50.0);

        const glm::mat4 made = Kinesis::frustum_projection(frustum, 16.0F / 9.0F);
        const glm::mat4 expected = glm::perspectiveRH_ZO(static_cast<float>(fov), 16.0F / 9.0F, 0.1F, 50.0F);

        for (int column = 0; column < 4; ++column) {
            for (int row = 0; row < 4; ++row) {
                EXPECT_NEAR(made[column][row], expected[column][row], 1.0e-3F) << column << "," << row;
            }
        }
    }

    TEST(FrustumTest, NearAndFarProjectToZeroAndOneForBothKindsOfLens)
    {
        Frustum frustum = Kinesis::frustum_from_fov(1.0, 1.5, 0.5, 40.0);

        for (const bool orthographic : { false, true }) {
            frustum.orthographic = orthographic;
            const glm::mat4 projection = Kinesis::frustum_projection(frustum, 1.5F);
            const glm::vec4 at_near = projection * glm::vec4(0.0F, 0.0F, -0.5F, 1.0F);
            const glm::vec4 at_far = projection * glm::vec4(0.0F, 0.0F, -40.0F, 1.0F);

            EXPECT_NEAR(at_near.z / at_near.w, 0.0F, k_tolerance);
            EXPECT_NEAR(at_far.z / at_far.w, 1.0F, k_tolerance);
        }
    }

    TEST(FrustumTest, AFrustumThatCannotProjectGivesTheIdentity)
    {
        MeshAssemblyLens no_focus;
        no_focus.focal_length = 0.0;
        Frustum no_depth;
        no_depth.far = no_depth.near;
        Frustum no_window;
        no_window.window = { .min = { 1.0F, 0.0F }, .max = { 1.0F, 1.0F } };

        EXPECT_EQ(Kinesis::frustum_projection(Kakshya::frustum_of(no_focus), 1.5F), glm::mat4(1.0F));
        EXPECT_EQ(Kinesis::frustum_projection(no_depth, 1.5F), glm::mat4(1.0F));
        EXPECT_EQ(Kinesis::frustum_projection(no_window, 1.5F), glm::mat4(1.0F));
        EXPECT_EQ(Kinesis::frustum_projection(Frustum {}, 0.0F), glm::mat4(1.0F));
    }

    TEST(FrustumTest, TheViewIsTheInverseOfThePlacement)
    {
        const glm::mat4 world = glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 0.0F, 5.0F));

        const auto view = Kinesis::view_from_frustum(world, Frustum {}, 1.5F);

        const glm::vec4 origin = view.view * glm::vec4(0.0F, 0.0F, 0.0F, 1.0F);
        EXPECT_NEAR(origin.z, -5.0F, k_tolerance);
        const glm::vec4 camera = view.view * glm::vec4(0.0F, 0.0F, 5.0F, 1.0F);
        EXPECT_NEAR(glm::length(glm::vec3(camera)), 0.0F, k_tolerance);
    }

    TEST(FrustumTest, AViewerLooksDownItsOwnNegativeZ)
    {
        const glm::mat4 world = glm::rotate(glm::mat4(1.0F), glm::radians(90.0F), glm::vec3(0.0F, 1.0F, 0.0F));

        const auto view = Kinesis::view_from_frustum(world, Frustum {}, 1.5F);

        const glm::vec4 ahead = view.view * glm::vec4(-5.0F, 0.0F, 0.0F, 1.0F);
        EXPECT_NEAR(ahead.x, 0.0F, k_tolerance);
        EXPECT_NEAR(ahead.z, -5.0F, k_tolerance);
    }

    TEST(FrustumTest, ScaleAndShearInAPlacementNeverReachTheView)
    {
        const glm::mat4 plain = glm::translate(glm::mat4(1.0F), glm::vec3(1.0F, 2.0F, 3.0F));
        const glm::mat4 scaled = plain * glm::scale(glm::mat4(1.0F), glm::vec3(4.0F, 4.0F, 4.0F));

        const auto a = Kinesis::view_from_frustum(plain, Frustum {}, 1.5F);
        const auto b = Kinesis::view_from_frustum(scaled, Frustum {}, 1.5F);

        for (int column = 0; column < 4; ++column) {
            for (int row = 0; row < 4; ++row) {
                EXPECT_NEAR(a.view[column][row], b.view[column][row], k_tolerance);
            }
        }
    }

    TEST(FrustumTest, TheProjectionOfTheViewIsTheFrustumsProjection)
    {
        Frustum frustum;
        frustum.fit = AspectFit::Contain;

        const auto view = Kinesis::view_from_frustum(glm::mat4(1.0F), frustum, 2.0F);

        EXPECT_EQ(view.projection, Kinesis::frustum_projection(frustum, 2.0F));
    }

} // namespace
} // namespace MayaFlux::Test
