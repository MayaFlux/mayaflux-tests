#include "gtest/gtest.h"

#include "MayaFlux/Portal/Forma/Primitives/LayoutCursor.hpp"

namespace MayaFlux::Test {

namespace Forma = Portal::Forma;
using Forma::LayoutCursor;
using Kinesis::AABB2D;

namespace {

    void expect_near(const AABB2D& a, const AABB2D& b)
    {
        EXPECT_NEAR(a.min.x, b.min.x, 1e-6F);
        EXPECT_NEAR(a.min.y, b.min.y, 1e-6F);
        EXPECT_NEAR(a.max.x, b.max.x, 1e-6F);
        EXPECT_NEAR(a.max.y, b.max.y, 1e-6F);
    }

}

TEST(FormaLayoutCursorTest, DefaultCursorStartsAtTheTopOfTheScreenAcrossFullWidth)
{
    LayoutCursor cursor;

    EXPECT_FLOAT_EQ(cursor.y(), 1.F);
    EXPECT_FLOAT_EQ(cursor.x_min(), -1.F);
    EXPECT_FLOAT_EQ(cursor.x_max(), 1.F);
}

TEST(FormaLayoutCursorTest, AdvanceReturnsTheBandJustPlacedAndMovesDown)
{
    LayoutCursor cursor;

    const AABB2D first = cursor.advance(0.2F);
    const AABB2D second = cursor.advance(0.3F);

    expect_near(first, AABB2D { .min = { -1.F, 0.8F }, .max = { 1.F, 1.F } });
    expect_near(second, AABB2D { .min = { -1.F, 0.5F }, .max = { 1.F, 0.8F } });
    EXPECT_NEAR(cursor.y(), 0.5F, 1e-6F);
}

TEST(FormaLayoutCursorTest, ColumnExtentsScopeTheReturnedBounds)
{
    LayoutCursor cursor(0.5F, -0.25F, 0.75F);

    const AABB2D band = cursor.advance(0.1F);

    expect_near(band, AABB2D { .min = { -0.25F, 0.4F }, .max = { 0.75F, 0.5F } });
    EXPECT_FLOAT_EQ(cursor.x_min(), -0.25F);
    EXPECT_FLOAT_EQ(cursor.x_max(), 0.75F);
}

TEST(FormaLayoutCursorTest, SkipMovesTheBaselineWithoutReturningBounds)
{
    LayoutCursor cursor;

    cursor.skip(0.25F);

    EXPECT_NEAR(cursor.y(), 0.75F, 1e-6F);
    EXPECT_NEAR(cursor.advance(0.25F).max.y, 0.75F, 1e-6F);
}

TEST(FormaLayoutCursorTest, ResetReturnsToTheGivenBaselineOrTheTop)
{
    LayoutCursor cursor;
    cursor.advance(0.6F);

    cursor.reset(0.2F);
    EXPECT_FLOAT_EQ(cursor.y(), 0.2F);

    cursor.reset();
    EXPECT_FLOAT_EQ(cursor.y(), 1.F);
}

TEST(FormaLayoutCursorTest, CopiesShareOneBaseline)
{
    LayoutCursor original;
    LayoutCursor copy = original;

    copy.advance(0.4F);

    EXPECT_NEAR(original.y(), 0.6F, 1e-6F);
    EXPECT_EQ(original.state(), copy.state());
}

TEST(FormaLayoutCursorTest, EveryMovementBumpsTheSharedStateVersion)
{
    LayoutCursor cursor;
    const auto state = cursor.state();
    const auto start = state->version;

    cursor.advance(0.1F);
    cursor.skip(0.1F);
    cursor.reset();

    EXPECT_EQ(state->version, start + 3);
}

TEST(FormaLayoutCursorTest, SyncToPutsBothCursorsOnTheLowerBaseline)
{
    LayoutCursor left(1.F, -1.F, 0.F);
    LayoutCursor right(1.F, 0.F, 1.F);
    left.advance(0.3F);
    right.advance(0.7F);

    left.sync_to(right);

    EXPECT_NEAR(left.y(), 0.3F, 1e-6F);
    EXPECT_NEAR(right.y(), 0.3F, 1e-6F);
}

TEST(FormaLayoutCursorTest, SyncToBetweenCopiesLeavesTheBaselineAlone)
{
    LayoutCursor cursor;
    LayoutCursor copy = cursor;
    cursor.advance(0.5F);

    cursor.sync_to(copy);

    EXPECT_NEAR(cursor.y(), 0.5F, 1e-6F);
}

TEST(FormaLayoutCursorTest, BoundScrollOffsetTranslatesReturnedBoundsOnly)
{
    LayoutCursor cursor;
    auto offset = std::make_shared<Forma::MappedState<glm::vec2>>();
    offset->write({ 0.F, 0.3F });
    cursor.bind_scroll(offset);

    const AABB2D band = cursor.advance(0.2F);

    expect_near(band, AABB2D { .min = { -1.F, 1.1F }, .max = { 1.F, 1.3F } });
    EXPECT_NEAR(cursor.y(), 0.8F, 1e-6F);
}

TEST(FormaLayoutCursorTest, ScrollOffsetIsReadLiveOnEachAdvance)
{
    LayoutCursor cursor;
    auto offset = std::make_shared<Forma::MappedState<glm::vec2>>();
    cursor.bind_scroll(offset);

    const AABB2D before = cursor.advance(0.1F);
    offset->write({ 0.F, 0.5F });
    const AABB2D after = cursor.advance(0.1F);

    EXPECT_NEAR(before.max.y, 1.F, 1e-6F);
    EXPECT_NEAR(after.max.y, 0.9F + 0.5F, 1e-6F);
}

TEST(FormaLayoutCursorTest, BoundScrollIsReachableThroughTheAccessor)
{
    LayoutCursor cursor;
    EXPECT_EQ(cursor.scroll_offset(), nullptr);

    auto offset = std::make_shared<Forma::MappedState<glm::vec2>>();
    cursor.bind_scroll(offset);

    EXPECT_EQ(cursor.scroll_offset(), offset);
}

TEST(FormaLayoutCursorTest, BoundsAlreadyHandedOutAreSnapshots)
{
    LayoutCursor cursor;
    auto offset = std::make_shared<Forma::MappedState<glm::vec2>>();
    cursor.bind_scroll(offset);
    const AABB2D band = cursor.advance(0.1F);

    offset->write({ 0.F, 1.F });

    expect_near(band, AABB2D { .min = { -1.F, 0.9F }, .max = { 1.F, 1.F } });
}

}
