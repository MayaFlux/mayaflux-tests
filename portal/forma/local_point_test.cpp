#include "gtest/gtest.h"
#include "../../support/test_metadata.hpp"

#include "MayaFlux/Kakshya/NDData/MeshData.hpp"
#include "MayaFlux/Kinesis/Spatial/HitTest.hpp"
#include "MayaFlux/Portal/Forma/Layer.hpp"
#include "MayaFlux/Portal/Forma/LocalPoint.hpp"

#include <glm/gtc/matrix_transform.hpp>

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    namespace Forma = Portal::Forma;

    const Kinesis::AABB2D k_host { .min = { -0.5F, -0.25F }, .max = { 0.5F, 0.75F } };

    Kinesis::ViewTransform local_point_camera()
    {
        return Kinesis::look_at_perspective({ 0.F, 0.F, 5.F }, { 0.F, 0.F, 0.F }, 1.5707964F, 1.F, 0.1F, 100.F);
    }

    std::function<glm::mat4()> held(const glm::mat4& model)
    {
        return [model] { return model; };
    }

    /** A square from -1 to 1 whose uv has v = 0 along its top edge. */
    std::shared_ptr<Kakshya::MeshData> uv_square()
    {
        const std::vector<Kakshya::Vertex> vertices {
            { .position = { -1.F, -1.F, 0.F }, .uv = { 0.F, 1.F } },
            { .position = { 1.F, -1.F, 0.F }, .uv = { 1.F, 1.F } },
            { .position = { 1.F, 1.F, 0.F }, .uv = { 1.F, 0.F } },
            { .position = { -1.F, 1.F, 0.F }, .uv = { 0.F, 0.F } }
        };
        const std::vector<uint32_t> indices { 0, 1, 2, 0, 2, 3 };
        auto mesh = Kakshya::MeshData::from_vertices(vertices, indices);
        return mesh ? std::make_shared<Kakshya::MeshData>(std::move(*mesh)) : nullptr;
    }

    /** Screen position of a world point through local_point_camera. */
    glm::vec2 on_screen(const glm::vec3& world)
    {
        const auto vt = local_point_camera();
        const glm::vec4 clip = vt.projection * vt.view * glm::vec4(world, 1.F);
        return glm::vec2(clip) / clip.w;
    }

}

TEST(PlaneToLocalTest, ThePointIsInPlaneCoordinatesOfThePlacedModel)
{
    const glm::mat4 model = glm::translate(glm::mat4(1.F), { 1.F, 0.5F, -1.F })
        * glm::rotate(glm::mat4(1.F), 0.6F, { 0.F, 1.F, 0.F })
        * glm::scale(glm::mat4(1.F), { 2.F, 0.5F, 1.F });
    const auto to_local = Forma::plane_to_local(local_point_camera, held(model));
    const glm::vec3 world = glm::vec3(model * glm::vec4(0.3F, -0.4F, 0.F, 1.F));

    const auto local = to_local(on_screen(world));

    ASSERT_TRUE(local.has_value());
    EXPECT_NEAR(local->x, 0.3F, 1e-3F);
    EXPECT_NEAR(local->y, -0.4F, 1e-3F);
    const Kinesis::Ray ray = Kinesis::ray_through(on_screen(world), local_point_camera());
    EXPECT_NEAR(local->z, glm::distance(ray.origin, world), 1e-3F);
}

TEST(PlaneToLocalTest, ThePlaneIsNotHitFromBehindOrWhenTheModelIsFlattened)
{
    const glm::mat4 turned = glm::rotate(glm::mat4(1.F), glm::pi<float>(), { 0.F, 1.F, 0.F });

    EXPECT_FALSE(Forma::plane_to_local(local_point_camera, held(turned))({ 0.F, 0.F }));
    EXPECT_FALSE(Forma::plane_to_local(local_point_camera, held(glm::mat4(0.F)))({ 0.F, 0.F }));
}

TEST(PlaneToLocalTest, TheViewAndModelAreReadOnEveryCall)
{
    auto model = std::make_shared<glm::mat4>(1.F);
    const auto to_local = Forma::plane_to_local(local_point_camera, [model] { return *model; });
    const float before = to_local({ 0.F, 0.F }).value().x;

    *model = glm::translate(glm::mat4(1.F), { 1.F, 0.F, 0.F });

    EXPECT_NEAR(before, 0.F, 1e-4F);
    EXPECT_NEAR(to_local({ 0.F, 0.F }).value().x, -1.F, 1e-4F);
}

TEST(PlaneToLocalTest, ALayerPicksTheNearerPlaneInWorldDistanceWhateverTheScaleOrOrder)
{
    const glm::mat4 front = glm::translate(glm::mat4(1.F), { 0.F, 0.F, 1.F })
        * glm::scale(glm::mat4(1.F), glm::vec3(10.F));
    const glm::mat4 back = glm::scale(glm::mat4(1.F), glm::vec3(0.1F));
    const Kinesis::AABB2D everywhere { .min = glm::vec2(-100.F), .max = glm::vec2(100.F) };

    for (const bool front_first : { true, false }) {
        Forma::Layer layer;
        auto near_plane = Forma::Element {}.with_bounds(everywhere);
        near_plane.to_local = Forma::plane_to_local(local_point_camera, held(front));
        auto far_plane = Forma::Element {}.with_bounds(everywhere);
        far_plane.to_local = Forma::plane_to_local(local_point_camera, held(back));

        uint32_t near_id {};
        if (front_first) {
            near_id = layer.add(near_plane);
            (void)layer.add(far_plane);
        } else {
            (void)layer.add(far_plane);
            near_id = layer.add(near_plane);
        }

        EXPECT_EQ(layer.hit_test({ 0.F, 0.F }), near_id);
    }
}

TEST(MeshToLocalTest, TheHitUvLandsInTheHostRectangleWithVRunningDown)
{
    const auto mesh = uv_square();
    ASSERT_NE(mesh, nullptr);
    const auto to_local = Forma::mesh_to_local(local_point_camera, held(glm::mat4(1.F)), mesh, k_host);

    const auto upper_left = to_local(on_screen({ -0.5F, 0.5F, 0.F }));
    const auto lower_right = to_local(on_screen({ 0.8F, -0.6F, 0.F }));

    ASSERT_TRUE(upper_left.has_value());
    ASSERT_TRUE(lower_right.has_value());
    EXPECT_NEAR(upper_left->x, -0.25F, 1e-3F);
    EXPECT_NEAR(upper_left->y, 0.5F, 1e-3F);
    EXPECT_NEAR(lower_right->x, 0.4F, 1e-3F);
    EXPECT_NEAR(lower_right->y, -0.05F, 1e-3F);
}

TEST(MeshToLocalTest, OffTheMeshIsAMiss)
{
    const auto to_local = Forma::mesh_to_local(local_point_camera, held(glm::mat4(1.F)), uv_square(), k_host);

    EXPECT_FALSE(to_local(on_screen({ 1.5F, 0.F, 0.F })));
}

TEST(MeshToLocalTest, ALaterChangeToTheMeshIsFollowed)
{
    const auto mesh = uv_square();
    ASSERT_NE(mesh, nullptr);
    const auto to_local = Forma::mesh_to_local(local_point_camera, held(glm::mat4(1.F)), mesh, k_host);
    ASSERT_TRUE(to_local({ 0.F, 0.F }));

    std::get<std::vector<uint32_t>>(mesh->index_variant).clear();

    EXPECT_FALSE(to_local({ 0.F, 0.F }));
}

TEST(MeshToLocalTest, NoMeshAForeignLayoutAndTruncatedVerticesNeverHit)
{
    const auto foreign = uv_square();
    ASSERT_NE(foreign, nullptr);
    foreign->layout.stride_bytes = 32;

    const auto truncated = uv_square();
    ASSERT_NE(truncated, nullptr);
    std::get<std::vector<uint8_t>>(truncated->vertex_variant).resize(sizeof(Kakshya::Vertex) + 7);

    for (const auto& mesh : { std::shared_ptr<Kakshya::MeshData> {}, foreign, truncated }) {
        const auto to_local = Forma::mesh_to_local(local_point_camera, held(glm::mat4(1.F)), mesh, k_host);

        EXPECT_FALSE(to_local({ 0.F, 0.F }));
        EXPECT_FALSE(to_local({ 0.F, 0.F }));
    }
}

}
