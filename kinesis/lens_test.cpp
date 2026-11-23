#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kinesis/Viewport/Lens.hpp"

#include <glm/gtc/matrix_transform.hpp>

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Kinesis::FilmFit;
    using Kinesis::Lens;
    using Kinesis::ScreenWindow;

    constexpr float k_tolerance = 1.0e-4F;

    /** The reach of a 35 mm lens on a 3.6 cm film back: tan of half the horizontal field of view. */
    constexpr double k_reach = 36.0 / 70.0;

    TEST(LensTest, TheDefaultsAreAlembicsLensOnAThirtyFiveMillimetreFilmBack)
    {
        const Lens lens;

        EXPECT_DOUBLE_EQ(lens.focal_length, 35.0);
        EXPECT_DOUBLE_EQ(lens.horizontal_aperture, 3.6);
        EXPECT_DOUBLE_EQ(lens.vertical_aperture, 2.4);
        EXPECT_DOUBLE_EQ(lens.window.left, -1.0);
        EXPECT_DOUBLE_EQ(lens.window.right, 1.0);
        EXPECT_NEAR(lens.window.top, 2.0 / 3.0, 1e-12);
        EXPECT_DOUBLE_EQ(lens.window.width(), 2.0);
    }

    TEST(LensTest, TheFieldsOfViewFollowTheFilmBackAndTheFocalLength)
    {
        const Lens lens;

        EXPECT_NEAR(Kinesis::lens_half_tangent(lens), k_reach, 1e-12);
        EXPECT_NEAR(Kinesis::lens_horizontal_fov(lens), 2.0 * std::atan(k_reach), 1e-12);
        EXPECT_NEAR(Kinesis::lens_vertical_fov(lens), 2.0 * std::atan(24.0 / 70.0), 1e-12);
    }

    TEST(LensTest, AFilmBackOfTheOutputsOwnAspectIsSymmetricAndNeedsNoFit)
    {
        const glm::mat4 projection = Kinesis::lens_projection(Lens {}, 1.5F);

        EXPECT_NEAR(projection[0][0], static_cast<float>(1.0 / k_reach), k_tolerance);
        EXPECT_NEAR(projection[1][1], static_cast<float>(1.0 / ((2.0 / 3.0) * k_reach)), k_tolerance);
        EXPECT_NEAR(projection[2][0], 0.0F, k_tolerance);
        EXPECT_NEAR(projection[2][1], 0.0F, k_tolerance);
        EXPECT_FLOAT_EQ(projection[2][3], -1.0F);
    }

    TEST(LensTest, FillCoversTheOutputAndCropsWhatOverhangs)
    {
        const glm::mat4 wider = Kinesis::lens_projection(Lens {}, 2.0F, FilmFit::Fill);
        EXPECT_NEAR(wider[0][0], static_cast<float>(1.0 / k_reach), k_tolerance);
        EXPECT_NEAR(wider[1][1], static_cast<float>(1.0 / (0.5 * k_reach)), k_tolerance);

        const glm::mat4 taller = Kinesis::lens_projection(Lens {}, 1.0F, FilmFit::Fill);
        EXPECT_NEAR(taller[1][1], static_cast<float>(1.0 / ((2.0 / 3.0) * k_reach)), k_tolerance);
        EXPECT_NEAR(taller[0][0], static_cast<float>(1.0 / ((2.0 / 3.0) * k_reach)), k_tolerance);
    }

    TEST(LensTest, OverscanShowsTheWholeFilmInsideTheOutput)
    {
        const glm::mat4 wider = Kinesis::lens_projection(Lens {}, 2.0F, FilmFit::Overscan);
        EXPECT_NEAR(wider[1][1], static_cast<float>(1.0 / ((2.0 / 3.0) * k_reach)), k_tolerance);
        EXPECT_NEAR(wider[0][0], static_cast<float>(1.0 / ((4.0 / 3.0) * k_reach)), k_tolerance);

        const glm::mat4 taller = Kinesis::lens_projection(Lens {}, 1.0F, FilmFit::Overscan);
        EXPECT_NEAR(taller[0][0], static_cast<float>(1.0 / k_reach), k_tolerance);
    }

    TEST(LensTest, HorizontalAndVerticalFitHoldOneSideWhateverTheOutput)
    {
        const glm::mat4 horizontal = Kinesis::lens_projection(Lens {}, 1.0F, FilmFit::Horizontal);
        EXPECT_NEAR(horizontal[0][0], static_cast<float>(1.0 / k_reach), k_tolerance);
        EXPECT_NEAR(horizontal[1][1], static_cast<float>(1.0 / k_reach), k_tolerance);

        const glm::mat4 vertical = Kinesis::lens_projection(Lens {}, 1.0F, FilmFit::Vertical);
        EXPECT_NEAR(vertical[1][1], static_cast<float>(1.0 / ((2.0 / 3.0) * k_reach)), k_tolerance);
        EXPECT_NEAR(vertical[0][0], static_cast<float>(1.0 / ((2.0 / 3.0) * k_reach)), k_tolerance);
    }

    TEST(LensTest, AWindowOffCentreMakesAnOffAxisFrustum)
    {
        Lens lens;
        lens.window = ScreenWindow { .left = -0.5, .right = 1.5, .bottom = -2.0 / 3.0, .top = 2.0 / 3.0 };

        const glm::mat4 projection = Kinesis::lens_projection(lens, 1.5F, FilmFit::Horizontal);

        EXPECT_NEAR(projection[2][0], 0.5F, k_tolerance);
        EXPECT_NEAR(projection[0][0], static_cast<float>(1.0 / k_reach), k_tolerance);
    }

    TEST(LensTest, ALensMadeFromAFieldOfViewProjectsLikeAPerspective)
    {
        const double fov = glm::radians(60.0);
        const Lens lens = Kinesis::lens_from_fov(fov, 16.0 / 9.0, 0.1, 50.0);

        const glm::mat4 made = Kinesis::lens_projection(lens, 16.0F / 9.0F);
        const glm::mat4 expected = glm::perspective(static_cast<float>(fov), 16.0F / 9.0F, 0.1F, 50.0F);

        for (int column = 0; column < 4; ++column) {
            for (int row = 0; row < 4; ++row) {
                EXPECT_NEAR(made[column][row], expected[column][row], 1.0e-3F) << column << "," << row;
            }
        }
    }

    TEST(LensTest, ALensThatCannotProjectGivesTheIdentity)
    {
        Lens no_focus;
        no_focus.focal_length = 0.0;
        Lens no_depth;
        no_depth.far = no_depth.near;
        Lens no_window;
        no_window.window = ScreenWindow { .left = 1.0, .right = 1.0, .bottom = 0.0, .top = 1.0 };

        EXPECT_EQ(Kinesis::lens_projection(no_focus, 1.5F), glm::mat4(1.0F));
        EXPECT_EQ(Kinesis::lens_projection(no_depth, 1.5F), glm::mat4(1.0F));
        EXPECT_EQ(Kinesis::lens_projection(no_window, 1.5F), glm::mat4(1.0F));
        EXPECT_EQ(Kinesis::lens_projection(Lens {}, 0.0F), glm::mat4(1.0F));
    }

    TEST(LensTest, TheViewIsTheInverseOfTheCamerasPlacement)
    {
        const glm::mat4 world = glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 0.0F, 5.0F));

        const auto view = Kinesis::view_from_lens(world, Lens {}, 1.5F);

        const glm::vec4 origin = view.view * glm::vec4(0.0F, 0.0F, 0.0F, 1.0F);
        EXPECT_NEAR(origin.z, -5.0F, k_tolerance);
        const glm::vec4 camera = view.view * glm::vec4(0.0F, 0.0F, 5.0F, 1.0F);
        EXPECT_NEAR(glm::length(glm::vec3(camera)), 0.0F, k_tolerance);
    }

    TEST(LensTest, ACameraLooksDownItsOwnNegativeZ)
    {
        const glm::mat4 world = glm::rotate(glm::mat4(1.0F), glm::radians(90.0F), glm::vec3(0.0F, 1.0F, 0.0F));

        const auto view = Kinesis::view_from_lens(world, Lens {}, 1.5F);

        const glm::vec4 ahead = view.view * glm::vec4(-5.0F, 0.0F, 0.0F, 1.0F);
        EXPECT_NEAR(ahead.x, 0.0F, k_tolerance);
        EXPECT_NEAR(ahead.z, -5.0F, k_tolerance);
    }

    TEST(LensTest, ScaleAndShearInAPlacementNeverReachTheView)
    {
        const glm::mat4 plain = glm::translate(glm::mat4(1.0F), glm::vec3(1.0F, 2.0F, 3.0F));
        const glm::mat4 scaled = plain * glm::scale(glm::mat4(1.0F), glm::vec3(4.0F, 4.0F, 4.0F));

        const auto a = Kinesis::view_from_lens(plain, Lens {}, 1.5F);
        const auto b = Kinesis::view_from_lens(scaled, Lens {}, 1.5F);

        for (int column = 0; column < 4; ++column) {
            for (int row = 0; row < 4; ++row) {
                EXPECT_NEAR(a.view[column][row], b.view[column][row], k_tolerance);
            }
        }
    }

    TEST(LensTest, TheProjectionOfTheViewIsTheLensProjection)
    {
        Lens lens;
        lens.focal_length = 50.0;

        const auto view = Kinesis::view_from_lens(glm::mat4(1.0F), lens, 2.0F, FilmFit::Overscan);

        EXPECT_EQ(view.projection, Kinesis::lens_projection(lens, 2.0F, FilmFit::Overscan));
    }

} // namespace
} // namespace MayaFlux::Test
