#include "gtest/gtest.h"
#include "../support/radiometry_support.hpp"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kinesis/Stochastic/Directions.hpp"
#include "MayaFlux/Kinesis/Stochastic/Sequences.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

namespace {

    namespace St = Kinesis::Stochastic;

    constexpr uint32_t k_stochastic_count = 1U << 16U;

    glm::vec2 stochastic_point(uint32_t i) { return St::hammersley_point(i, k_stochastic_count); }

}

TEST(StochasticSequenceTest, EveryWordStaysBelowOne)
{
    EXPECT_LT(St::unit_from_bits(0xFFFFFFFFU), 1.0F);
    EXPECT_EQ(St::unit_from_bits(0U), 0.0F);
    EXPECT_LT(St::radical_inverse(0xFFFFFFFFU), 1.0F);
    EXPECT_LT(St::radical_inverse(0U, 0xFFFFFFFFU), 1.0F);
    for (const uint32_t index : { 0U, 1U, 1000000U, 0x7FFFFFFFU, 0xFFFFFFFFU }) {
        const glm::vec2 p = St::additive_recurrence(index);
        EXPECT_GE(std::min(p.x, p.y), 0.0F) << index;
        EXPECT_LT(std::max(p.x, p.y), 1.0F) << index;
    }
}

TEST(StochasticSequenceTest, RadicalInverseMirrorsTheBits)
{
    EXPECT_EQ(St::radical_inverse(1U), 0.5F);
    EXPECT_EQ(St::radical_inverse(2U), 0.25F);
    EXPECT_EQ(St::radical_inverse(3U), 0.75F);
    EXPECT_EQ(St::radical_inverse(4U), 0.125F);
}

TEST(StochasticSequenceTest, HashDoesNotCollideAndSeedsDiffer)
{
    std::vector<uint32_t> words(100000);
    for (uint32_t i = 0; i < words.size(); ++i) {
        words[i] = St::pcg_hash(i);
    }
    std::ranges::sort(words);
    EXPECT_EQ(std::ranges::adjacent_find(words), words.end());

    double sum = 0.0;
    uint32_t same = 0;
    for (uint32_t i = 0; i < k_stochastic_count; ++i) {
        const float a = St::hashed_unit(i, 0);
        sum += a;
        same += a == St::hashed_unit(i, 1) ? 1U : 0U;
    }
    EXPECT_NEAR(sum / k_stochastic_count, 0.5, 5.0e-3);
    EXPECT_LT(same, 4U);
}

TEST(StochasticDirectionTest, BasisIsARotationEverywhereIncludingBothPoles)
{
    std::vector<glm::vec3> normals = {
        { 0.0F, 0.0F, 1.0F }, { 0.0F, 0.0F, -1.0F }, { 1.0F, 0.0F, -0.0F }, { 1.0F, 0.0F, 0.0F },
        glm::normalize(glm::vec3(1.0e-4F, -1.0e-4F, -1.0F))
    };
    for (uint32_t i = 0; i < 1000; ++i) {
        normals.push_back(St::square_to_sphere({ St::hashed_unit(i, 5), St::hashed_unit(i, 6) }));
    }
    for (const auto& normal : normals) {
        const glm::mat3 basis = St::orthonormal_basis(normal);
        const glm::mat3 gram = glm::transpose(basis) * basis;
        for (int c = 0; c < 3; ++c) {
            for (int r = 0; r < 3; ++r) {
                ASSERT_NEAR(gram[c][r], c == r ? 1.0F : 0.0F, 1.0e-5F) << normal.x << ' ' << normal.z;
            }
        }
        ASSERT_NEAR(glm::determinant(basis), 1.0F, 1.0e-5F);
        ASSERT_EQ(basis[2], normal);
    }
}

TEST(StochasticDirectionTest, MapsStayOnTheirSetsAtTheSquaresEdges)
{
    for (const float x : { 0.0F, 0.5F, 0x1.fffffep-1F }) {
        for (const float y : { 0.0F, 0.5F, 0x1.fffffep-1F }) {
            const glm::vec2 u(x, y);
            EXPECT_LE(glm::length(St::square_to_disk(u)), 1.0F + 1.0e-6F);
            EXPECT_NEAR(glm::length(St::square_to_sphere(u)), 1.0F, 1.0e-3F);
            EXPECT_GE(St::square_to_hemisphere(u).z, 0.0F);
            EXPECT_GE(St::square_to_cosine_hemisphere(u).z, 0.0F);
            EXPECT_NEAR(glm::length(St::square_to_cosine_hemisphere(u)), 1.0F, 1.0e-5F);
            EXPECT_GE(St::square_to_cone(u, 0.9F).z, 0.9F - 1.0e-6F);
            EXPECT_NEAR(St::square_to_cone(u, 1.0F).z, 1.0F, 1.0e-6F);

            const glm::vec3 b = St::square_to_barycentric(u);
            EXPECT_NEAR(b.x + b.y + b.z, 1.0F, 1.0e-6F);
            EXPECT_GE(std::min({ b.x, b.y, b.z }), -1.0e-6F);
        }
    }
}

TEST(StochasticDirectionTest, MapsAreUniformInTheirOwnMeasure)
{
    glm::dvec3 sphere(0.0);
    glm::dvec3 hemisphere(0.0);
    glm::dvec3 cosine(0.0);
    glm::dvec3 barycentric(0.0);
    double disk_radius_sq = 0.0;
    for (uint32_t i = 0; i < k_stochastic_count; ++i) {
        sphere += glm::dvec3(St::square_to_sphere(stochastic_point(i)));
        hemisphere += glm::dvec3(St::square_to_hemisphere(stochastic_point(i)));
        cosine += glm::dvec3(St::square_to_cosine_hemisphere(stochastic_point(i)));
        barycentric += glm::dvec3(St::square_to_barycentric(stochastic_point(i)));
        const glm::vec2 d = St::square_to_disk(stochastic_point(i));
        disk_radius_sq += glm::dot(d, d);
    }
    EXPECT_NEAR(glm::length(sphere) / k_stochastic_count, 0.0, 2.0e-3);
    EXPECT_NEAR(hemisphere.z / k_stochastic_count, 0.5, 2.0e-3);
    EXPECT_NEAR(cosine.z / k_stochastic_count, 2.0 / 3.0, 2.0e-3);
    EXPECT_NEAR(barycentric.x / k_stochastic_count, 1.0 / 3.0, 2.0e-3);
    EXPECT_NEAR(barycentric.y / k_stochastic_count, 1.0 / 3.0, 2.0e-3);
    EXPECT_NEAR(disk_radius_sq / k_stochastic_count, 0.5, 2.0e-3);
}

TEST(StochasticDirectionTest, DensitiesIntegrateToOne)
{
    const double cosine = Radiometry::integrate_hemisphere(
        [](const glm::vec3& d) { return St::cosine_hemisphere_density(d.z); });
    EXPECT_NEAR(cosine, 1.0, 1.0e-4);

    const double hemisphere = Radiometry::integrate_hemisphere([](const glm::vec3&) { return St::cone_density(0.0F); });
    EXPECT_NEAR(hemisphere, 1.0, 1.0e-5);

    EXPECT_TRUE(std::isfinite(St::cone_density(1.0F)));
    EXPECT_EQ(St::cosine_hemisphere_density(-0.5F), 0.0F);
}

TEST(StochasticDirectionTest, PowerHeuristicSharesTheWeightAtAnyScale)
{
    EXPECT_EQ(St::power_heuristic(0.0F, 0.0F), 0.0F);
    EXPECT_EQ(St::power_heuristic(1.0F, 0.0F), 1.0F);
    EXPECT_EQ(St::power_heuristic(0.0F, 1.0F), 0.0F);
    for (const auto& [a, b] : { std::pair { 3.0F, 5.0F }, std::pair { 1.0e25F, 1.0e25F }, std::pair { 1.0e-30F, 2.0e-30F } }) {
        EXPECT_NEAR(St::power_heuristic(a, b) + St::power_heuristic(b, a), 1.0F, 1.0e-5F) << a << ' ' << b;
    }
}

} // namespace MayaFlux::Test
