#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kinesis/Viewport/ViewpointOperations.hpp"

#include <glm/gtc/matrix_transform.hpp>

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Kinesis::AspectFit;
    using Kinesis::Frustum;
    using Kinesis::Viewpoint;

    constexpr float k_aspect = 1.6F;

    /** A tilted, rolled camera, so nothing is axis aligned by luck. */
    glm::mat4 placed_camera()
    {
        const glm::mat4 level = glm::inverse(glm::lookAt(
            glm::vec3(3.F, 2.F, 5.F), glm::vec3(0.F), glm::vec3(0.F, 1.F, 0.F)));
        return level * glm::rotate(glm::mat4(1.F), 0.4F, glm::vec3(0.F, 0.F, 1.F));
    }

    std::vector<Viewpoint> lenses()
    {
        const Frustum symmetric = Kinesis::frustum_from_fov(1.0, k_aspect, 0.1, 100.0);

        Frustum off_axis = symmetric;
        off_axis.window = { .min = { -0.2F, -0.5F }, .max = { 0.9F, 0.3F } };
        off_axis.fit = AspectFit::Cover;

        Frustum flat = symmetric;
        flat.window = { .min = { -3.F, -2.F }, .max = { 3.F, 2.F } };
        flat.orthographic = true;
        flat.fit = AspectFit::Contain;

        return { { placed_camera(), symmetric }, { placed_camera(), off_axis }, { placed_camera(), flat } };
    }

    glm::vec2 image_of(const Viewpoint& view, const glm::vec3& point, float aspect = k_aspect)
    {
        const auto vt = Kinesis::view_from_frustum(view.first, view.second, aspect);
        const glm::vec4 clip = vt.projection * vt.view * glm::vec4(point, 1.F);
        return glm::vec2(clip) / clip.w;
    }

    glm::vec3 eye_of(const Viewpoint& view) { return glm::vec3(view.first[3]); }

    glm::vec3 forward_of_view(const Viewpoint& view) { return -glm::normalize(glm::vec3(view.first[2])); }

    void expect_same_lens(const Viewpoint& a, const Viewpoint& b)
    {
        EXPECT_TRUE(Kinesis::same_viewpoint({ glm::mat4(1.F), a.second }, { glm::mat4(1.F), b.second }));
    }

    void expect_close(const glm::vec3& a, const glm::vec3& b, float tolerance = 1.0e-4F)
    {
        EXPECT_NEAR(a.x, b.x, tolerance);
        EXPECT_NEAR(a.y, b.y, tolerance);
        EXPECT_NEAR(a.z, b.z, tolerance);
    }

    const glm::vec3 k_target { 0.4F, -0.3F, 0.6F };

}

TEST(ViewpointOperationsTest, RotatingAboutAPointKeepsItsImageDistanceAndTheLens)
{
    for (const Viewpoint& view : lenses()) {
        const Viewpoint turned = Kinesis::rotate_viewpoint_about(view, k_target, 1.1F, -0.3F);

        const glm::vec2 before = image_of(view, k_target);
        const glm::vec2 after = image_of(turned, k_target);
        EXPECT_NEAR(after.x, before.x, 1e-4F);
        EXPECT_NEAR(after.y, before.y, 1e-4F);
        EXPECT_NEAR(glm::distance(eye_of(turned), k_target), glm::distance(eye_of(view), k_target), 1e-4F);
        EXPECT_GT(glm::distance(eye_of(turned), eye_of(view)), 0.5F);
        expect_same_lens(view, turned);
    }
}

TEST(ViewpointOperationsTest, RotatingInPlaceLeavesTheEyeAndTurnsByTheRightHandRule)
{
    const Viewpoint ahead { glm::mat4(1.F), Frustum {} };

    const Viewpoint left = Kinesis::rotate_viewpoint(ahead, glm::half_pi<float>(), 0.F);
    const Viewpoint raised = Kinesis::rotate_viewpoint(ahead, 0.F, 0.5F);

    expect_close(eye_of(left), glm::vec3(0.F));
    expect_close(forward_of_view(left), { -1.F, 0.F, 0.F });
    expect_close(forward_of_view(raised), { 0.F, std::sin(0.5F), -std::cos(0.5F) });
}

TEST(ViewpointOperationsTest, ElevationStopsShortOfTheAxisAndNeverFlipsOver)
{
    const Viewpoint ahead { glm::mat4(1.F), Frustum {} };

    Viewpoint view = ahead;
    for (int i = 0; i < 20; ++i)
        view = Kinesis::rotate_viewpoint(view, 0.F, 0.7F);

    EXPECT_NEAR(std::asin(forward_of_view(view).y), glm::radians(89.F), 1e-3F);
    EXPECT_LT(forward_of_view(view).z, 0.F);
    EXPECT_GT(view.first[1].y, 0.F);
}

TEST(ViewpointOperationsTest, ElevationFollowsTheAxisEvenWhenTheCameraIsRolled)
{
    const Viewpoint rolled {
        glm::rotate(glm::mat4(1.F), glm::half_pi<float>(), glm::vec3(0.F, 0.F, 1.F)), Frustum {}
    };

    const Viewpoint raised = Kinesis::rotate_viewpoint(rolled, 0.F, 0.5F);

    EXPECT_NEAR(std::asin(forward_of_view(raised).y), 0.5F, 1e-4F);
    expect_close(glm::vec3(raised.first[1]), { -1.F, 0.F, 0.F });
}

TEST(ViewpointOperationsTest, ACameraLookingStraightAlongTheAxisCanLeaveItWithoutAJump)
{
    const Viewpoint down {
        glm::rotate(glm::mat4(1.F), -glm::half_pi<float>(), glm::vec3(1.F, 0.F, 0.F)), Frustum {}
    };

    const Viewpoint still = Kinesis::rotate_viewpoint(down, 0.F, 0.F);
    const Viewpoint raised = Kinesis::rotate_viewpoint(down, 0.F, 0.5F);

    expect_close(forward_of_view(still), { 0.F, -1.F, 0.F });
    EXPECT_NEAR(std::asin(forward_of_view(raised).y), -glm::half_pi<float>() + 0.5F, 1e-3F);
}

TEST(ViewpointOperationsTest, TranslatingToAnImagePutsThePointThereWithoutTurning)
{
    const glm::vec2 wanted { -0.6F, 0.45F };

    for (const Viewpoint& view : lenses()) {
        const Viewpoint moved = Kinesis::translate_viewpoint_to_image(view, k_target, wanted, k_aspect);

        const glm::vec2 after = image_of(moved, k_target);
        EXPECT_NEAR(after.x, wanted.x, 1e-4F);
        EXPECT_NEAR(after.y, wanted.y, 1e-4F);
        expect_close(forward_of_view(moved), forward_of_view(view));
        EXPECT_NEAR(glm::dot(eye_of(moved) - eye_of(view), forward_of_view(view)), 0.F, 1e-4F);
        expect_same_lens(view, moved);
    }
}

TEST(ViewpointOperationsTest, APointBehindAPerspectiveEyeCannotBeDragged)
{
    const Viewpoint view = lenses().front();
    const glm::vec3 behind = eye_of(view) - forward_of_view(view) * 2.F;

    const Viewpoint moved = Kinesis::translate_viewpoint_to_image(view, behind, { 0.5F, 0.5F }, k_aspect);

    EXPECT_TRUE(Kinesis::same_viewpoint(view, moved));
}

TEST(ViewpointOperationsTest, TranslatingTowardAPointCoversThatFraction)
{
    const Viewpoint view = lenses().front();
    const float distance = glm::distance(eye_of(view), k_target);

    const Viewpoint nearer = Kinesis::translate_viewpoint_toward(view, k_target, 0.25F);
    const Viewpoint farther = Kinesis::translate_viewpoint_toward(view, k_target, -1.F);

    EXPECT_NEAR(glm::distance(eye_of(nearer), k_target), distance * 0.75F, 1e-4F);
    EXPECT_NEAR(glm::distance(eye_of(farther), k_target), distance * 2.F, 1e-4F);
    expect_close(forward_of_view(nearer), forward_of_view(view));
    expect_same_lens(view, nearer);
}

TEST(ViewpointOperationsTest, TheEyeNeverReachesOrPassesThePoint)
{
    const Viewpoint view = lenses().front();

    for (const float fraction : { 1.F, 2.F, std::numeric_limits<float>::quiet_NaN() }) {
        EXPECT_TRUE(Kinesis::same_viewpoint(view, Kinesis::translate_viewpoint_toward(view, k_target, fraction)));
    }
}

TEST(ViewpointOperationsTest, ScalingTheWindowFixesWhatIsUnderThePositionForEveryFit)
{
    const glm::vec2 cursor { 0.7F, -0.55F };

    for (Viewpoint view : lenses()) {
        for (const auto fit : { AspectFit::Cover, AspectFit::Width, AspectFit::Height, AspectFit::Contain }) {
            for (const float aspect : { 0.5F, 2.5F }) {
                view.second.fit = fit;
                const Viewpoint seen = Kinesis::translate_viewpoint_to_image(view, k_target, cursor, aspect);

                const Viewpoint scaled = Kinesis::scale_viewpoint_window(seen, 0.4F, cursor, aspect);

                const glm::vec2 after = image_of(scaled, k_target, aspect);
                EXPECT_NEAR(after.x, cursor.x, 1e-3F);
                EXPECT_NEAR(after.y, cursor.y, 1e-3F);
                EXPECT_NEAR(scaled.second.window.width(), seen.second.window.width() * 0.4F, 1e-4F);
                EXPECT_NEAR(scaled.second.window.height(), seen.second.window.height() * 0.4F, 1e-4F);
                EXPECT_TRUE(scaled.first == seen.first);
            }
        }
    }
}

TEST(ViewpointOperationsTest, ANonPositiveFactorChangesNothing)
{
    const Viewpoint view = lenses().front();

    for (const float factor : { 0.F, -1.F, std::numeric_limits<float>::quiet_NaN() }) {
        EXPECT_TRUE(Kinesis::same_viewpoint(view, Kinesis::scale_viewpoint_window(view, factor, { 0.5F, 0.5F }, k_aspect)));
    }
}

TEST(ViewpointOperationsTest, ADestinationWithNoUsableSizeChangesNothing)
{
    const float aspects[] {
        0.F, -1.F, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()
    };

    for (const Viewpoint& view : lenses()) {
        for (const float aspect : aspects) {
            EXPECT_TRUE(Kinesis::same_viewpoint(view,
                Kinesis::translate_viewpoint_to_image(view, k_target, { 0.5F, 0.5F }, aspect)));
            EXPECT_TRUE(Kinesis::same_viewpoint(view,
                Kinesis::scale_viewpoint_window(view, 0.5F, { 0.5F, 0.5F }, aspect)));
        }
    }
}

TEST(ViewpointOperationsTest, AnEmptyWindowChangesNothing)
{
    Viewpoint view = lenses().front();
    view.second.window = { .min = { 0.F, 0.F }, .max = { 0.F, 0.F } };

    EXPECT_TRUE(Kinesis::same_viewpoint(view,
        Kinesis::translate_viewpoint_to_image(view, k_target, { 0.5F, 0.5F }, k_aspect)));
    EXPECT_TRUE(Kinesis::same_viewpoint(view,
        Kinesis::scale_viewpoint_window(view, 0.5F, { 0.5F, 0.5F }, k_aspect)));
}

TEST(ViewpointOperationsTest, TranslatingMovesAlongTheViewpointsOwnAxesWhateverItsPlacementScale)
{
    for (Viewpoint view : lenses()) {
        view.first = view.first * glm::scale(glm::mat4(1.F), glm::vec3(3.F));
        const glm::vec3 right = glm::normalize(glm::vec3(view.first[0]));
        const glm::vec3 up = glm::normalize(glm::vec3(view.first[1]));

        const Viewpoint moved = Kinesis::translate_viewpoint(view, { 0.5F, -1.F, 2.F });

        expect_close(eye_of(moved) - eye_of(view), right * 0.5F - up - forward_of_view(view) * 2.F);
        expect_close(forward_of_view(moved), forward_of_view(view));
        expect_same_lens(view, moved);
    }
}

TEST(ViewpointOperationsTest, AligningPutsTheCentreAheadAtItsFormerDistanceAndLevel)
{
    for (const Viewpoint& view : lenses()) {
        const float distance = glm::distance(eye_of(view), k_target);

        const Viewpoint aligned = Kinesis::align_viewpoint(view, k_target, { -4.F, 0.F, 0.F });

        expect_close(forward_of_view(aligned), { -1.F, 0.F, 0.F });
        expect_close(eye_of(aligned), k_target + glm::vec3(distance, 0.F, 0.F));
        expect_close(glm::vec3(aligned.first[1]), { 0.F, 1.F, 0.F });
        expect_same_lens(view, aligned);
    }
}

TEST(ViewpointOperationsTest, AligningStraightDownTheAxisFromAheadPutsTheFarSideAtTheTop)
{
    const Viewpoint ahead { glm::translate(glm::mat4(1.F), { 0.F, 0.F, 5.F }), Frustum {} };

    const Viewpoint top = Kinesis::align_viewpoint(ahead, glm::vec3(0.F), { 0.F, -1.F, 0.F });

    expect_close(eye_of(top), { 0.F, 5.F, 0.F });
    expect_close(glm::vec3(top.first[0]), { 1.F, 0.F, 0.F });
    expect_close(glm::vec3(top.first[1]), { 0.F, 0.F, -1.F });
}

TEST(ViewpointOperationsTest, AligningWithTheRightDirectionOnTheAxisStaysOrthonormal)
{
    const Viewpoint rolled {
        glm::rotate(glm::mat4(1.F), glm::half_pi<float>(), glm::vec3(0.F, 0.F, 1.F)), Frustum {}
    };

    const Viewpoint top = Kinesis::align_viewpoint(rolled, { 0.F, 0.F, -2.F }, { 0.F, -1.F, 0.F });

    const glm::mat3 basis(top.first);
    EXPECT_NEAR(glm::determinant(basis), 1.F, 1e-4F);
    expect_close(forward_of_view(top), { 0.F, -1.F, 0.F });
}

TEST(ViewpointOperationsTest, AligningToNoDirectionChangesNothing)
{
    const Viewpoint view = lenses().front();

    EXPECT_TRUE(Kinesis::same_viewpoint(view, Kinesis::align_viewpoint(view, k_target, glm::vec3(0.F))));
}

}
