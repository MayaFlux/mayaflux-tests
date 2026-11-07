#include "gtest/gtest.h"

#include "MayaFlux/Kinesis/Vision/VisionContext.hpp"

namespace MayaFlux::Test {

using Kinesis::Vision::CpuVisionPass;
using Kinesis::Vision::VisionOp;
using Kinesis::Vision::VisionSequence;

namespace {

    VisionSequence three_step_sequence()
    {
        return VisionSequence::Builder {}.rgba_to_gray().sobel().scharr().build();
    }

} // namespace

TEST(VisionPassWalkTest, BeginResetsTheWalkAndSetsGeometry)
{
    const auto sequence = three_step_sequence();
    CpuVisionPass pass;
    pass.index = 2;
    pass.begin(sequence, 64, 32);

    EXPECT_EQ(pass.sequence, &sequence);
    EXPECT_EQ(pass.index, 0U);
    EXPECT_EQ(pass.channels, 4U);
    EXPECT_EQ(pass.w, 64U);
    EXPECT_EQ(pass.h, 32U);
    EXPECT_EQ(pass.plane_size(), 64U * 32U);
}

TEST(VisionPassWalkTest, SetGeometryUpdatesTheResultToo)
{
    CpuVisionPass pass;
    pass.set_geometry(128, 16);
    EXPECT_EQ(pass.w, 128U);
    EXPECT_EQ(pass.h, 16U);
    EXPECT_EQ(pass.result.w, 128U);
    EXPECT_EQ(pass.result.h, 16U);
}

TEST(VisionPassWalkTest, StepReadsTheCurrentIndex)
{
    const auto sequence = three_step_sequence();
    CpuVisionPass pass;
    pass.begin(sequence, 8, 8);
    pass.index = 1;
    EXPECT_EQ(pass.step().op, VisionOp::Sobel);
}

TEST(VisionPassWalkTest, AheadStopsAtTheEndOfTheSequence)
{
    const auto sequence = three_step_sequence();
    CpuVisionPass pass;
    pass.begin(sequence, 8, 8);

    pass.index = 0;
    ASSERT_NE(pass.ahead(), nullptr);
    EXPECT_EQ(pass.ahead()->op, VisionOp::Sobel);
    ASSERT_NE(pass.ahead(2), nullptr);
    EXPECT_EQ(pass.ahead(2)->op, VisionOp::Scharr);
    EXPECT_EQ(pass.ahead(3), nullptr);

    pass.index = 2;
    EXPECT_EQ(pass.ahead(), nullptr);
}

TEST(VisionPassWalkTest, BehindStopsAtTheStartOfTheSequence)
{
    const auto sequence = three_step_sequence();
    CpuVisionPass pass;
    pass.begin(sequence, 8, 8);

    pass.index = 0;
    EXPECT_EQ(pass.behind(), nullptr);

    pass.index = 1;
    ASSERT_NE(pass.behind(), nullptr);
    EXPECT_EQ(pass.behind()->op, VisionOp::RgbaToGray);
    EXPECT_EQ(pass.behind(2), nullptr);
}

TEST(VisionPassWalkTest, BeginClearsTheMemoisedOutputs)
{
    const auto sequence = three_step_sequence();
    CpuVisionPass pass;
    pass.begin(sequence, 8, 8);
    pass.completed[7] = { .output = 10, .input = 20 };

    pass.begin(sequence, 8, 8);
    EXPECT_TRUE(pass.completed.empty());
}

TEST(VisionPassMemoTest, HitsOnlyWhenTheInputIsTheSameHandle)
{
    CpuVisionPass pass;
    pass.completed[7] = { .output = 10, .input = 20 };

    const auto* hit = pass.memo(7, 20);
    ASSERT_NE(hit, nullptr);
    EXPECT_EQ(*hit, 10U);

    EXPECT_EQ(pass.memo(7, 21), nullptr);
    EXPECT_EQ(pass.memo(8, 20), nullptr);
}

TEST(VisionPassRetentionTest, SameSizeBeginKeepsTheRetainedHandles)
{
    const auto sequence = three_step_sequence();
    CpuVisionPass pass;
    pass.begin(sequence, 64, 32);
    pass.prev = 5;
    pass.prev_cache = 6;
    pass.prev_keypoints.push_back({});

    pass.begin(sequence, 64, 32);
    EXPECT_EQ(pass.prev, 5U);
    EXPECT_EQ(pass.prev_cache, 6U);
    EXPECT_EQ(pass.prev_keypoints.size(), 1U);
}

TEST(VisionPassRetentionTest, SizeChangeForgetsTheRetainedHandles)
{
    const auto sequence = three_step_sequence();
    CpuVisionPass pass;
    pass.begin(sequence, 64, 32);
    pass.prev = 5;
    pass.prev_cache = 6;
    pass.prev_keypoints.push_back({});

    pass.begin(sequence, 128, 32);
    EXPECT_EQ(pass.prev, 0U);
    EXPECT_EQ(pass.prev_cache, 0U);
    EXPECT_TRUE(pass.prev_keypoints.empty());
}

TEST(VisionPassRetentionTest, ForgetClearsTheRetainedHandlesDirectly)
{
    CpuVisionPass pass;
    pass.prev = 5;
    pass.prev_cache = 6;
    pass.prev_keypoints.push_back({});
    pass.forget();
    EXPECT_EQ(pass.prev, 0U);
    EXPECT_EQ(pass.prev_cache, 0U);
    EXPECT_TRUE(pass.prev_keypoints.empty());
}

} // namespace MayaFlux::Test
