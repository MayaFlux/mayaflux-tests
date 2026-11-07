#include "gtest/gtest.h"

#include "MayaFlux/Kinesis/Vision/Pyramid.hpp"

namespace MayaFlux::Test {

using Kinesis::Vision::PyramidLayout;
using Kinesis::Vision::pyramid_layout;

TEST(PyramidLayoutTest, SingleLevelIsTheFrameItself)
{
    const auto layout = pyramid_layout(256, 128, 1);
    EXPECT_EQ(layout.levels, 1U);
    EXPECT_EQ(layout.level[0].ox, 0U);
    EXPECT_EQ(layout.level[0].oy, 0U);
    EXPECT_EQ(layout.level[0].w, 256U);
    EXPECT_EQ(layout.level[0].h, 128U);
    EXPECT_EQ(layout.atlas_w, 256U);
    EXPECT_EQ(layout.atlas_h, 128U);
}

TEST(PyramidLayoutTest, LevelsHalveAndStackInTheAtlasColumn)
{
    const auto layout = pyramid_layout(256, 128, 4);
    ASSERT_EQ(layout.levels, 4U);

    // Level 1 sits to the right of the base frame.
    EXPECT_EQ(layout.level[1].ox, 256U);
    EXPECT_EQ(layout.level[1].oy, 0U);
    EXPECT_EQ(layout.level[1].w, 128U);
    EXPECT_EQ(layout.level[1].h, 64U);

    // Each further level is stacked under the previous ones in that column.
    EXPECT_EQ(layout.level[2].ox, 256U);
    EXPECT_EQ(layout.level[2].oy, 64U);
    EXPECT_EQ(layout.level[2].w, 64U);
    EXPECT_EQ(layout.level[2].h, 32U);

    EXPECT_EQ(layout.level[3].ox, 256U);
    EXPECT_EQ(layout.level[3].oy, 96U);
    EXPECT_EQ(layout.level[3].w, 32U);
    EXPECT_EQ(layout.level[3].h, 16U);

    // Atlas: widest level in the column, and the taller of the frame and the stack.
    EXPECT_EQ(layout.atlas_w, 256U + 128U);
    EXPECT_EQ(layout.atlas_h, 128U);
}

TEST(PyramidLayoutTest, StackCanBeTallerThanTheFrame)
{
    // A 3-pixel-high frame: rounding each half up makes the stack 2 + 1 + 1 = 4 rows.
    const auto layout = pyramid_layout(64, 3, 4, 1);
    ASSERT_EQ(layout.levels, 4U);
    EXPECT_EQ(layout.level[1].oy, 0U);
    EXPECT_EQ(layout.level[1].h, 2U);
    EXPECT_EQ(layout.level[2].oy, 2U);
    EXPECT_EQ(layout.level[3].oy, 3U);
    EXPECT_EQ(layout.atlas_h, 4U);
    EXPECT_EQ(layout.atlas_w, 64U + 32U);
}

TEST(PyramidLayoutTest, OddSizesRoundUpWhenHalved)
{
    const auto layout = pyramid_layout(33, 17, 2, 1);
    ASSERT_EQ(layout.levels, 2U);
    EXPECT_EQ(layout.level[1].w, 17U);
    EXPECT_EQ(layout.level[1].h, 9U);
    EXPECT_EQ(layout.atlas_w, 33U + 17U);
    EXPECT_EQ(layout.atlas_h, 17U);
}

TEST(PyramidLayoutTest, MinExtentStopsTheSequenceBeforeTheLevelIsTooSmall)
{
    // 64 -> 32 -> 16 is allowed at min_extent 16; the next level, 8, is not.
    const auto layout = pyramid_layout(64, 64, 6, 16);
    EXPECT_EQ(layout.levels, 3U);
    EXPECT_EQ(layout.level[2].w, 16U);
    EXPECT_EQ(layout.level[2].h, 16U);
}

TEST(PyramidLayoutTest, FrameSmallerThanMinExtentHasOnlyTheBaseLevel)
{
    const auto layout = pyramid_layout(16, 16, 3);
    EXPECT_EQ(layout.levels, 1U);
    EXPECT_EQ(layout.atlas_w, 16U);
    EXPECT_EQ(layout.atlas_h, 16U);
}

TEST(PyramidLayoutTest, RequestedLevelsBelowOneClampToOne)
{
    EXPECT_EQ(pyramid_layout(256, 256, 0).levels, 1U);
}

TEST(PyramidLayoutTest, RequestedLevelsAboveTheMaximumClampToIt)
{
    const auto layout = pyramid_layout(1024, 1024, 100, 1);
    EXPECT_EQ(layout.levels, PyramidLayout::k_max_levels);
    EXPECT_EQ(layout.level[PyramidLayout::k_max_levels - 1].w, 1024U >> (PyramidLayout::k_max_levels - 1));
}

TEST(PyramidLayoutTest, EqualityComparesLevelCountAtlasAndBaseSize)
{
    EXPECT_EQ(pyramid_layout(128, 128, 3), pyramid_layout(128, 128, 3));
    EXPECT_NE(pyramid_layout(128, 128, 3), pyramid_layout(128, 128, 2));
    EXPECT_NE(pyramid_layout(128, 128, 3), pyramid_layout(64, 64, 3));
}

} // namespace MayaFlux::Test
