#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kinesis/Spatial/HitRegion.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Kinesis::AABB1D;
    using Kinesis::AABB2D;
    using Kinesis::AABB3D;
    using Kinesis::HitRegion1D;
    using Kinesis::HitRegion2D;
    using Kinesis::HitRegion3D;

    TEST(HitRegion2DTest, ARegionWithNeitherBoundsNorContainsNeverHits)
    {
        const HitRegion2D region;
        EXPECT_FALSE(region.hits({ 0.0F, 0.0F }));
        EXPECT_FALSE(region.hits({ 1.0F, 1.0F }));
    }

    TEST(HitRegion2DTest, BoundsAloneDecideWhenThereIsNoContainsTest)
    {
        const HitRegion2D region { .bounds = AABB2D::from_origin_size({ 0.0F, 0.0F }, { 2.0F, 1.0F }) };
        EXPECT_TRUE(region.hits({ 1.0F, 0.5F }));
        EXPECT_TRUE(region.hits({ 2.0F, 1.0F }));
        EXPECT_FALSE(region.hits({ 2.1F, 0.5F }));
    }

    TEST(HitRegion2DTest, ContainsDecidesInsideTheBoundsAndBoundsRejectOutsideThem)
    {
        const HitRegion2D disc {
            .bounds = AABB2D::from_ndc({ 0.0F, 0.0F }, { 0.5F, 0.5F }),
            .contains = Kinesis::circular_bounds({ 0.0F, 0.0F }, 0.5F),
        };
        EXPECT_TRUE(disc.hits({ 0.1F, 0.1F }));
        EXPECT_FALSE(disc.hits({ 0.45F, 0.45F }));
        EXPECT_FALSE(disc.hits({ 0.9F, 0.0F }));

        const HitRegion2D too_small {
            .bounds = AABB2D::from_ndc({ 0.0F, 0.0F }, { 0.1F, 0.1F }),
            .contains = [](glm::vec2) { return true; },
        };
        EXPECT_TRUE(too_small.hits({ 0.05F, 0.05F }));
        EXPECT_FALSE(too_small.hits({ 0.3F, 0.3F }));
    }

    TEST(HitRegion2DTest, ContainsWorksWithoutBounds)
    {
        const HitRegion2D region { .contains = [](glm::vec2 p) { return p.x > 0.0F; } };
        EXPECT_TRUE(region.hits({ 5.0F, -3.0F }));
        EXPECT_FALSE(region.hits({ -1.0F, 0.0F }));
    }

    TEST(HitRegion2DTest, ClipRejectsOutsideItRegardlessOfTheOtherParts)
    {
        const HitRegion2D region {
            .contains = [](glm::vec2) { return true; },
            .clip = AABB2D::from_origin_size({ 0.0F, 0.0F }, { 1.0F, 1.0F }),
        };
        EXPECT_TRUE(region.hits({ 0.5F, 0.5F }));
        EXPECT_FALSE(region.hits({ 1.5F, 0.5F }));

        const HitRegion2D boxed {
            .bounds = AABB2D::from_origin_size({ 0.0F, 0.0F }, { 4.0F, 4.0F }),
            .clip = AABB2D::from_origin_size({ 0.0F, 0.0F }, { 1.0F, 1.0F }),
        };
        EXPECT_TRUE(boxed.hits({ 0.5F, 0.5F }));
        EXPECT_FALSE(boxed.hits({ 2.0F, 2.0F }));
    }

    TEST(HitRegion2DTest, ComposesWithTheContainmentFamily)
    {
        const HitRegion2D ring {
            .bounds = AABB2D::from_ndc({ 0.0F, 0.0F }, { 1.0F, 1.0F }),
            .contains = Kinesis::subtract_bounds(
                Kinesis::circular_bounds({ 0.0F, 0.0F }, 1.0F),
                Kinesis::circular_bounds({ 0.0F, 0.0F }, 0.5F)),
        };
        EXPECT_TRUE(ring.hits({ 0.75F, 0.0F }));
        EXPECT_FALSE(ring.hits({ 0.0F, 0.0F }));
        EXPECT_FALSE(ring.hits({ 1.5F, 0.0F }));
    }

    TEST(HitRegion1DTest, ASpanHitsInsideItsBounds)
    {
        const HitRegion1D span { .bounds = AABB1D { .min = 2.0, .max = 5.0 } };
        EXPECT_TRUE(span.hits(3.5));
        EXPECT_FALSE(span.hits(6.0));

        const HitRegion1D wrapped {
            .bounds = AABB1D { .min = 0.0, .max = 10.0 },
            .contains = [](double t) { return std::fmod(t, 2.0) < 1.0; },
        };
        EXPECT_TRUE(wrapped.hits(4.5));
        EXPECT_FALSE(wrapped.hits(5.5));
    }

    TEST(HitRegion3DTest, ABoxWithASphereHitsInsideTheSphereOnly)
    {
        const HitRegion3D region {
            .bounds = AABB3D { .min = glm::vec3(-1.0F), .max = glm::vec3(1.0F) },
            .contains = Kinesis::circular_bounds(glm::vec3(0.0F), 1.0F),
        };
        EXPECT_TRUE(region.hits({ 0.2F, 0.2F, 0.2F }));
        EXPECT_FALSE(region.hits({ 0.9F, 0.9F, 0.9F }));
        EXPECT_FALSE(region.hits({ 2.0F, 0.0F, 0.0F }));
    }

}
}
