#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kinesis/GeometryPrimitives.hpp"

#include <glm/gtc/quaternion.hpp>

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

namespace {

    void primitives_expect_near(glm::vec3 actual, glm::vec3 expected)
    {
        EXPECT_NEAR(actual.x, expected.x, 1e-4F);
        EXPECT_NEAR(actual.y, expected.y, 1e-4F);
        EXPECT_NEAR(actual.z, expected.z, 1e-4F);
    }

    void primitives_expect_closed_mesh_indices(const Kakshya::MeshData& mesh, uint32_t triangles)
    {
        EXPECT_TRUE(mesh.is_valid());
        EXPECT_EQ(mesh.face_count(), triangles);
        for (const uint32_t index : std::get<std::vector<uint32_t>>(mesh.index_variant))
            ASSERT_LT(index, mesh.vertex_count());
    }

}

TEST(KinesisPrimitivesTest, ACircleClosesOnItsRadiusWithAtLeastThreeSegments)
{
    const glm::vec3 centre { 1.F, 2.F, 3.F };
    const auto circle = Kinesis::generate_circle(centre, 2.F, 8);

    ASSERT_EQ(circle.size(), 9U);
    primitives_expect_near(circle.front(), circle.back());
    for (const auto& point : circle) {
        EXPECT_NEAR(glm::distance(point, centre), 2.F, 1e-4F);
        EXPECT_NEAR(point.z, centre.z, 1e-4F);
    }
    EXPECT_EQ(Kinesis::generate_circle(centre, 2.F, 1).size(), 4U);
    EXPECT_EQ(Kinesis::generate_regular_polygon(centre, 1.F, 5).size(), 6U);
    EXPECT_EQ(Kinesis::generate_rectangle(centre, 2.F, 1.F).size(), 5U);
}

TEST(KinesisPrimitivesTest, PivotAndBoxTransformsLandWhereTheySay)
{
    const glm::vec3 pivot { 1.F, 0.F, 0.F };
    const glm::vec3 target { 0.F, 5.F, 0.F };
    const auto turned = Kinesis::pivot_transform(pivot, target, glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.F, 0.F, 1.F)));
    primitives_expect_near(glm::vec3(turned * glm::vec4(pivot, 1.F)), target);

    const Kinesis::AABB3D source { .min = glm::vec3(0.F), .max = glm::vec3(1.F, 2.F, 0.F) };
    const Kinesis::AABB3D wanted { .min = glm::vec3(-1.F), .max = glm::vec3(1.F) };
    const auto fitted = Kinesis::box_transform(source, wanted);
    primitives_expect_near(glm::vec3(fitted * glm::vec4(source.min, 1.F)), { -1.F, -1.F, 0.F });
    primitives_expect_near(glm::vec3(fitted * glm::vec4(source.max, 1.F)), { 1.F, 1.F, 0.F });
}

TEST(KinesisPrimitivesTest, ProjectionAndHullActOnThePlaneAsked)
{
    std::vector<glm::vec3> points { { 1.F, 2.F, 3.F } };
    Kinesis::project_onto_plane(points, glm::vec3(0.F), { 0.F, 0.F, 4.F });
    primitives_expect_near(points.front(), { 1.F, 2.F, 0.F });

    const std::vector<glm::vec3> square {
        { 0.F, 0.F, 0.F }, { 1.F, 0.F, 0.F }, { 0.5F, 0.5F, 0.F }, { 1.F, 1.F, 0.F }, { 0.F, 1.F, 0.F }
    };
    const auto hull = Kinesis::compute_convex_hull_2d(square);

    ASSERT_EQ(hull.size(), 5U);
    primitives_expect_near(hull.front(), hull.back());
    for (const auto& point : hull)
        EXPECT_NE(point, glm::vec3(0.5F, 0.5F, 0.F));
}

TEST(KinesisPrimitivesTest, CurveSamplingIncludesBothEndsAndArcLengthSpacesEvenly)
{
    const auto samples = Kinesis::sample_parametric_curve([](float t) { return glm::vec3(t, 0.F, 0.F); }, 3);
    ASSERT_EQ(samples.size(), 3U);
    primitives_expect_near(samples[1], { 0.5F, 0.F, 0.F });

    const std::vector<Kakshya::LineVertex> path {
        { .position = { 0.F, 0.F, 0.F } }, { .position = { 1.F, 0.F, 0.F } }, { .position = { 4.F, 0.F, 0.F } }
    };
    const auto even = Kinesis::reparameterize_by_arc_length(path, 5);

    ASSERT_EQ(even.size(), 5U);
    for (size_t i = 0; i < even.size(); ++i)
        EXPECT_NEAR(even[i].position.x, static_cast<float>(i), 1e-4F);
}

TEST(KinesisPrimitivesTest, ColourHelpersKeepPositionsAndRefuseAMismatch)
{
    const std::vector<glm::vec3> positions { glm::vec3(0.F), glm::vec3(1.F), glm::vec3(2.F) };
    const auto graded = Kinesis::apply_color_gradient(positions, { glm::vec4(0.F), glm::vec4(1.F) });

    ASSERT_EQ(graded.size(), 3U);
    EXPECT_NEAR(graded[1].color.x, 0.5F, 1e-4F);
    EXPECT_EQ(Kinesis::apply_uniform_color(positions, glm::vec4(1.F)).size(), 3U);
    EXPECT_TRUE(Kinesis::apply_vertex_colors(positions, { glm::vec4(1.F) }).empty());
}

TEST(KinesisPrimitivesTest, QuadsAndWireframesCoverTheirRegion)
{
    const Kinesis::AABB2D region { .min = { -0.5F, 0.F }, .max = { 0.5F, 1.F } };

    const auto filled = Kinesis::filled_rect(region);
    primitives_expect_near(filled.front().position, { -0.5F, 0.F, 0.F });
    primitives_expect_near(filled.back().position, { 0.5F, 1.F, 0.F });
    EXPECT_EQ(Kinesis::textured_rect(region).front().texcoord, glm::vec2(0.F, 1.F));
    EXPECT_FLOAT_EQ(Kinesis::textured_mesh_rect(region, 2.F).back().weight, 2.F);
    EXPECT_EQ(Kinesis::cuboid_wireframe(glm::vec3(0.F), glm::vec3(1.F)).size(), 24U);
}

TEST(KinesisPrimitivesTest, GridTriangulatesBothFacesAndSurfacesOne)
{
    primitives_expect_closed_mesh_indices(Kinesis::generate_grid(glm::vec3(0.F), 2.F, 2.F, 3, 4), 48U);
    primitives_expect_closed_mesh_indices(
        Kinesis::generate_parametric_surface([](float u, float v) { return glm::vec3(u, v, 0.F); }, 5, 2), 20U);

    const std::vector<float> heights(12, 0.5F);
    primitives_expect_closed_mesh_indices(Kinesis::generate_explicit_surface(heights, 4, 3, glm::vec2(1.F)), 12U);
    EXPECT_FALSE(Kinesis::generate_explicit_surface(heights, 5, 5, glm::vec2(1.F)).is_valid());
}

}
