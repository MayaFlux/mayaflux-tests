#pragma once

#include "gtest/gtest.h"

#include <glm/glm.hpp>

namespace MayaFlux::Test::Polyhedral {

struct Surface {
    std::vector<glm::vec3> positions;
    std::vector<uint32_t> sizes;
    std::vector<uint32_t> corners;
};

inline Surface unit_cube()
{
    return {
        .positions = {
            glm::vec3(0.0F, 0.0F, 0.0F), glm::vec3(1.0F, 0.0F, 0.0F),
            glm::vec3(1.0F, 1.0F, 0.0F), glm::vec3(0.0F, 1.0F, 0.0F),
            glm::vec3(0.0F, 0.0F, 1.0F), glm::vec3(1.0F, 0.0F, 1.0F),
            glm::vec3(1.0F, 1.0F, 1.0F), glm::vec3(0.0F, 1.0F, 1.0F) },
        .sizes = { 4, 4, 4, 4, 4, 4 },
        .corners = { 0, 3, 2, 1, 4, 5, 6, 7, 0, 1, 5, 4, 3, 7, 6, 2, 0, 4, 7, 3, 1, 2, 6, 5 }
    };
}

inline std::array<glm::vec3, 6> unit_cube_normals()
{
    return { glm::vec3(0.0F, 0.0F, -1.0F), glm::vec3(0.0F, 0.0F, 1.0F),
        glm::vec3(0.0F, -1.0F, 0.0F), glm::vec3(0.0F, 1.0F, 0.0F),
        glm::vec3(-1.0F, 0.0F, 0.0F), glm::vec3(1.0F, 0.0F, 0.0F) };
}

inline Surface unit_grid(uint32_t columns, uint32_t rows)
{
    Surface grid;
    for (uint32_t j = 0; j <= rows; ++j) {
        for (uint32_t i = 0; i <= columns; ++i) {
            grid.positions.emplace_back(static_cast<float>(i), static_cast<float>(j), 0.0F);
        }
    }
    for (uint32_t j = 0; j < rows; ++j) {
        for (uint32_t i = 0; i < columns; ++i) {
            const uint32_t v00 = j * (columns + 1) + i;
            grid.sizes.push_back(4);
            grid.corners.insert(grid.corners.end(), { v00, v00 + 1, v00 + columns + 2, v00 + columns + 1 });
        }
    }
    return grid;
}

inline Surface single_face(std::vector<glm::vec3> ring)
{
    Surface face;
    face.sizes = { static_cast<uint32_t>(ring.size()) };
    for (uint32_t i = 0; i < ring.size(); ++i) {
        face.corners.push_back(i);
    }
    face.positions = std::move(ring);
    return face;
}

inline std::vector<uint32_t> ring_of(size_t count)
{
    std::vector<uint32_t> ring(count);
    for (uint32_t i = 0; i < count; ++i) {
        ring[i] = i;
    }
    return ring;
}

inline std::vector<glm::vec3> l_shape()
{
    return { glm::vec3(0.0F, 0.0F, 0.0F), glm::vec3(2.0F, 0.0F, 0.0F), glm::vec3(2.0F, 1.0F, 0.0F),
        glm::vec3(1.0F, 1.0F, 0.0F), glm::vec3(1.0F, 2.0F, 0.0F), glm::vec3(0.0F, 2.0F, 0.0F) };
}

inline std::vector<glm::vec3> regular(uint32_t sides, float height = 0.0F)
{
    std::vector<glm::vec3> ring;
    for (uint32_t i = 0; i < sides; ++i) {
        const float angle = 2.0F * std::numbers::pi_v<float> * static_cast<float>(i) / static_cast<float>(sides);
        ring.emplace_back(std::cos(angle), std::sin(angle), height);
    }
    return ring;
}

inline double triangle_list_area(std::span<const glm::vec3> positions, std::span<const uint32_t> indices)
{
    double total = 0.0;
    for (size_t t = 0; t + 2 < indices.size(); t += 3) {
        const glm::dvec3 a(positions[indices[t]]);
        const glm::dvec3 b(positions[indices[t + 1]]);
        const glm::dvec3 c(positions[indices[t + 2]]);
        total += 0.5 * glm::length(glm::cross(b - a, c - a));
    }
    return total;
}

inline ::testing::AssertionResult approx(const glm::vec3& actual, const glm::vec3& expected, float tolerance)
{
    if (glm::length(actual - expected) <= tolerance) {
        return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure()
        << "(" << actual.x << ", " << actual.y << ", " << actual.z << ") is not within " << tolerance
        << " of (" << expected.x << ", " << expected.y << ", " << expected.z << ")";
}

}
