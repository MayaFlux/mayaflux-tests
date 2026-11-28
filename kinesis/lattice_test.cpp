#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kinesis/Spatial/Lattice.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

TEST(KinesisLatticeTest, CellsAreAddressedRowMajorAndCountedOnce)
{
    const Kinesis::Lattice3D lattice { .resolution = { 2U, 3U, 4U } };

    EXPECT_EQ(lattice.cell_count(), 24U);
    EXPECT_EQ(lattice.corner_count(), 60U);
    EXPECT_EQ(lattice.index({ 0U, 0U, 0U }), 0U);
    EXPECT_EQ(lattice.index({ 1U, 2U, 3U }), 23U);
    EXPECT_TRUE(lattice.in_bounds({ 1U, 2U, 3U }));
    EXPECT_FALSE(lattice.in_bounds({ 2U, 0U, 0U }));
}

TEST(KinesisLatticeTest, ACellCentreMapsBackToItsCellAndOutsidePointsClamp)
{
    const Kinesis::Lattice3D lattice { .resolution = { 4U, 4U, 4U } };
    const glm::uvec3 cell { 1U, 2U, 3U };

    EXPECT_EQ(lattice.cell_at(lattice.cell_center(cell)), cell);
    EXPECT_EQ(lattice.cell_at(glm::vec3(-50.F)), glm::uvec3(0U));
    EXPECT_EQ(lattice.cell_at(glm::vec3(50.F)), glm::uvec3(3U));
    EXPECT_EQ(lattice.corner_position({ 4U, 4U, 4U }), glm::vec3(1.F));
}

TEST(KinesisLatticeTest, TrilinearSamplingBlendsCentresAndReturnsOutsideBeyondTheBounds)
{
    const Kinesis::Lattice3D lattice { .resolution = { 2U, 1U, 1U } };
    const std::vector<float> values { 1.F, 3.F };
    const auto sample = [&](glm::vec3 p) {
        return Kinesis::sample_trilinear<float>(lattice, std::span<const float>(values), p, -1.F);
    };

    EXPECT_FLOAT_EQ(sample({ -0.5F, 0.F, 0.F }), 1.F);
    EXPECT_FLOAT_EQ(sample({ 0.F, 0.F, 0.F }), 2.F);
    EXPECT_FLOAT_EQ(sample({ 0.5F, 0.F, 0.F }), 3.F);
    EXPECT_FLOAT_EQ(sample({ 1.5F, 0.F, 0.F }), -1.F);
    EXPECT_FLOAT_EQ(Kinesis::sample_trilinear<float>(lattice, std::span<const float> {}, glm::vec3(0.F), -1.F), -1.F);
}

TEST(KinesisLatticeTest, TheLowerDimensionsFollowTheSameRules)
{
    const auto quadrants = Kinesis::Lattice2D::ndc_quadrants();
    EXPECT_EQ(quadrants.cell_count(), 4U);
    EXPECT_EQ(quadrants.cell_at({ 0.5F, -0.5F }), (glm::uvec2 { 1U, 0U }));

    const Kinesis::Lattice1D line { .resolution = 4U };
    EXPECT_EQ(line.cell_at(line.cell_center(2U)), 2U);
    EXPECT_EQ(line.cell_at(9.0), 3U);
    EXPECT_EQ(line.resampled(8U).cell_count(), 8U);
}

}
