#include "../test_config.h"

#include "MayaFlux/Kinesis/Viewport/ScrollState.hpp"

namespace MayaFlux::Test {

using Kinesis::AABB2D;
using Kinesis::ScrollState;

namespace {

    ScrollState tall_content()
    {
        ScrollState state;
        state.viewport_bounds = AABB2D { .min = { -1.F, -1.F }, .max = { 1.F, 1.F } };
        state.content_bounds = AABB2D { .min = { -1.F, -5.F }, .max = { 1.F, 1.F } };
        return state;
    }

}

TEST(ScrollStateTest, DefaultStateHasNothingToScroll)
{
    ScrollState state;

    Kinesis::apply_scroll(state, { 3.F, 3.F });

    EXPECT_EQ(state.offset, glm::vec2(0.F));
}

TEST(DragToOffsetDeltaTest, ScalesTheViewportFractionByTheVisibleExtent)
{
    const glm::vec2 delta = Kinesis::drag_to_offset_delta({ 100.F, 50.F }, { 2.F, 1.F }, { 200.F, 100.F });

    EXPECT_FLOAT_EQ(delta.x, 1.F);
    EXPECT_FLOAT_EQ(delta.y, 0.5F);
}

TEST(DragToOffsetDeltaTest, MoreVisibleContentMovesFurtherPerPixel)
{
    const glm::vec2 narrow = Kinesis::drag_to_offset_delta({ 10.F, 10.F }, { 1.F, 1.F }, { 100.F, 100.F });
    const glm::vec2 wide = Kinesis::drag_to_offset_delta({ 10.F, 10.F }, { 4.F, 4.F }, { 100.F, 100.F });

    EXPECT_FLOAT_EQ(wide.x, narrow.x * 4.F);
    EXPECT_FLOAT_EQ(wide.y, narrow.y * 4.F);
}

TEST(DragToOffsetDeltaTest, ZeroDragIsZeroDelta)
{
    EXPECT_EQ(Kinesis::drag_to_offset_delta({ 0.F, 0.F }, { 2.F, 2.F }, { 100.F, 100.F }), glm::vec2(0.F));
}

TEST(ClampOffsetToContentTest, OffsetsInsideTheRangeAreUnchanged)
{
    const AABB2D content { .min = { -1.F, -3.F }, .max = { 2.F, 1.F } };

    EXPECT_EQ(Kinesis::clamp_offset_to_content({ 1.F, 2.F }, { 2.F, 2.F }, content), glm::vec2(1.F, 2.F));
}

TEST(ClampOffsetToContentTest, NegativeOffsetsClampToZero)
{
    const AABB2D content { .min = { -1.F, -3.F }, .max = { 2.F, 1.F } };

    EXPECT_EQ(Kinesis::clamp_offset_to_content({ -4.F, -0.5F }, { 2.F, 2.F }, content), glm::vec2(0.F));
}

TEST(ClampOffsetToContentTest, OffsetsPastTheFarEdgeClampToContentMinusViewport)
{
    const AABB2D content { .min = { -1.F, -3.F }, .max = { 2.F, 1.F } };

    EXPECT_EQ(Kinesis::clamp_offset_to_content({ 9.F, 9.F }, { 2.F, 2.F }, content), glm::vec2(1.F, 2.F));
}

TEST(ClampOffsetToContentTest, AnAxisWithNoExcessContentClampsToZero)
{
    const AABB2D content { .min = { 0.F, 0.F }, .max = { 1.F, 6.F } };

    const glm::vec2 clamped = Kinesis::clamp_offset_to_content({ 3.F, 3.F }, { 2.F, 2.F }, content);

    EXPECT_FLOAT_EQ(clamped.x, 0.F);
    EXPECT_FLOAT_EQ(clamped.y, 3.F);
}

TEST(ClampOffsetToContentTest, ContentEqualToTheViewportAllowsNoScroll)
{
    const AABB2D content { .min = { -1.F, -1.F }, .max = { 1.F, 1.F } };

    EXPECT_EQ(Kinesis::clamp_offset_to_content({ 0.7F, 0.7F }, { 2.F, 2.F }, content), glm::vec2(0.F));
}

TEST(ApplyScrollTest, AddsTheDeltaAndAccumulates)
{
    ScrollState state = tall_content();

    Kinesis::apply_scroll(state, { 0.F, 1.F });
    Kinesis::apply_scroll(state, { 0.F, 1.5F });

    EXPECT_EQ(state.offset, glm::vec2(0.F, 2.5F));
}

TEST(ApplyScrollTest, ClampsAtBothEnds)
{
    ScrollState state = tall_content();

    Kinesis::apply_scroll(state, { 0.F, 100.F });
    EXPECT_EQ(state.offset, glm::vec2(0.F, 4.F));

    Kinesis::apply_scroll(state, { 0.F, -100.F });
    EXPECT_EQ(state.offset, glm::vec2(0.F, 0.F));
}

TEST(ApplyScrollTest, ShrinkingContentPullsTheOffsetBackOnTheNextScroll)
{
    ScrollState state = tall_content();
    Kinesis::apply_scroll(state, { 0.F, 4.F });
    state.content_bounds.min.y = -3.F;

    Kinesis::apply_scroll(state, { 0.F, 0.F });

    EXPECT_EQ(state.offset, glm::vec2(0.F, 2.F));
}

TEST(ApplyScrollTest, HorizontalDeltaIsIgnoredWhenContentIsNoWiderThanTheViewport)
{
    ScrollState state = tall_content();

    Kinesis::apply_scroll(state, { 5.F, 1.F });

    EXPECT_FLOAT_EQ(state.offset.x, 0.F);
    EXPECT_FLOAT_EQ(state.offset.y, 1.F);
}

TEST(ApplyWheelScrollTest, ScalesTheWheelDeltaBySpeed)
{
    ScrollState state = tall_content();

    Kinesis::apply_wheel_scroll(state, 0.0, 2.0, 0.5F);

    EXPECT_EQ(state.offset, glm::vec2(0.F, 1.F));
}

TEST(ApplyWheelScrollTest, DefaultSpeedIsEightHundredthsPerTick)
{
    ScrollState state = tall_content();

    Kinesis::apply_wheel_scroll(state, 0.0, 1.0);

    EXPECT_FLOAT_EQ(state.offset.y, 0.08F);
}

TEST(ApplyWheelScrollTest, PositiveWheelDeltaIncreasesTheOffsetAndClamps)
{
    ScrollState state = tall_content();

    Kinesis::apply_wheel_scroll(state, 0.0, 1000.0, 0.5F);
    EXPECT_FLOAT_EQ(state.offset.y, 4.F);

    Kinesis::apply_wheel_scroll(state, 0.0, -1000.0, 0.5F);
    EXPECT_FLOAT_EQ(state.offset.y, 0.F);
}

TEST(ScrollViewTransformTest, TranslatesPointsByTheOffsetWithAnIdentityProjection)
{
    ScrollState state = tall_content();
    state.offset = { 0.25F, 1.5F };

    const auto transform = Kinesis::scroll_view_transform(state);
    const glm::vec4 moved = transform.view * glm::vec4(0.25F, 0.5F, 0.F, 1.F);

    EXPECT_FLOAT_EQ(moved.x, 0.5F);
    EXPECT_FLOAT_EQ(moved.y, 2.F);
    EXPECT_FLOAT_EQ(moved.z, 0.F);
    EXPECT_FLOAT_EQ(moved.w, 1.F);
    EXPECT_EQ(transform.projection, glm::mat4(1.F));
}

TEST(ScrollViewTransformTest, ZeroOffsetIsTheIdentityView)
{
    EXPECT_EQ(Kinesis::scroll_view_transform(ScrollState {}).view, glm::mat4(1.F));
}

}
