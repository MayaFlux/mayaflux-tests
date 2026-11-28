#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kinesis/Spatial/BoxHierarchy.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Kinesis::BoxHierarchy;
    using Kinesis::Ray;

    /** A repeatable cloud of small, overlapping, variously turned triangles. */
    std::pair<std::vector<glm::vec3>, std::vector<uint32_t>> triangle_cloud(uint32_t count)
    {
        std::vector<glm::vec3> vertices;
        std::vector<uint32_t> indices;
        uint32_t seed = 7U;
        const auto next = [&seed] {
            seed = seed * 1664525U + 1013904223U;
            return static_cast<float>(seed >> 16U) / 65535.F * 2.F - 1.F;
        };

        for (uint32_t i = 0; i < count; ++i) {
            const glm::vec3 centre { next() * 4.F, next() * 4.F, next() * 4.F };
            for (int corner = 0; corner < 3; ++corner) {
                vertices.push_back(centre + glm::vec3(next(), next(), next()) * 0.4F);
                indices.push_back(static_cast<uint32_t>(vertices.size() - 1));
            }
        }
        return { vertices, indices };
    }

}

TEST(BoxHierarchyTest, ItFindsWhatTheLinearSearchFindsOverManyRays)
{
    const auto [vertices, indices] = triangle_cloud(400);
    const BoxHierarchy hierarchy(Kinesis::triangle_boxes(vertices, indices));
    ASSERT_EQ(hierarchy.size(), 400U);

    int met = 0;
    for (int x = -10; x <= 10; ++x) {
        for (int y = -10; y <= 10; ++y) {
            const Ray ray {
                .origin = { static_cast<float>(x) * 0.4F, static_cast<float>(y) * 0.4F, 9.F },
                .direction = glm::normalize(glm::vec3(0.03F * static_cast<float>(y), -0.02F, -1.F))
            };

            const auto linear = Kinesis::ray_triangles_parameter(ray, vertices, std::span<const uint32_t>(indices));
            const auto quick = Kinesis::ray_triangles_parameter(ray, hierarchy, vertices, std::span<const uint32_t>(indices));

            ASSERT_EQ(quick.has_value(), linear.has_value()) << x << "," << y;
            if (linear) {
                ++met;
                EXPECT_EQ(quick->first, linear->first);
                EXPECT_FLOAT_EQ(quick->second.x, linear->second.x);
            }
        }
    }
    EXPECT_GT(met, 20);
}

TEST(BoxHierarchyTest, AxisAlignedRaysMeetFlatTriangles)
{
    const std::vector<glm::vec3> vertices { { -1.F, -1.F, 0.F }, { 1.F, -1.F, 0.F }, { 1.F, 1.F, 0.F } };
    const std::vector<uint32_t> indices { 0, 1, 2 };
    const BoxHierarchy hierarchy(Kinesis::triangle_boxes(vertices, indices));

    const Ray down { .origin = { 0.5F, -0.5F, 3.F }, .direction = { 0.F, 0.F, -1.F } };

    EXPECT_TRUE(Kinesis::ray_triangles_parameter(down, hierarchy, vertices, std::span<const uint32_t>(indices)));
}

TEST(BoxHierarchyTest, EmptyAndOutOfRangeInputsAreNeverMet)
{
    const std::vector<glm::vec3> vertices { { -1.F, -1.F, 0.F }, { 1.F, -1.F, 0.F }, { 1.F, 1.F, 0.F } };
    const std::vector<uint32_t> bad { 0, 1, 99 };
    const Ray down { .origin = { 0.F, 0.F, 3.F }, .direction = { 0.F, 0.F, -1.F } };

    const BoxHierarchy empty { std::span<const Kinesis::AABB3D> {} };
    const BoxHierarchy broken(Kinesis::triangle_boxes(vertices, bad));

    EXPECT_FALSE(Kinesis::ray_triangles_parameter(down, empty, vertices, std::span<const uint32_t>(bad)));
    EXPECT_FALSE(Kinesis::ray_triangles_parameter(down, broken, vertices, std::span<const uint32_t>(bad)));
}

TEST(BoxHierarchyTest, ManyCoincidentItemsGiveTheLowestNumber)
{
    std::vector<glm::vec3> vertices { { -1.F, -1.F, 0.F }, { 1.F, -1.F, 0.F }, { 0.F, 1.F, 0.F } };
    std::vector<uint32_t> indices;
    for (int i = 0; i < 40; ++i)
        indices.insert(indices.end(), { 0, 1, 2 });
    const BoxHierarchy hierarchy(Kinesis::triangle_boxes(vertices, indices));

    const Ray down { .origin = { 0.F, 0.F, 3.F }, .direction = { 0.F, 0.F, -1.F } };
    const auto hit = Kinesis::ray_triangles_parameter(down, hierarchy, vertices, std::span<const uint32_t>(indices));

    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit->first, 0U);
}

}
