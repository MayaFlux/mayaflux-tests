#include "gtest/gtest.h"

#include "../../support/scripted_window.hpp"

#include "MayaFlux/Portal/Forma/Context.hpp"
#include "MayaFlux/Portal/Forma/Primitives/FormFactory.hpp"
#include "MayaFlux/Portal/Forma/Primitives/Scrollable.hpp"
#include "MayaFlux/Portal/Forma/Surface.hpp"
#include "MayaFlux/Vruta/EventManager.hpp"

namespace MayaFlux::Test {

namespace Forma = Portal::Forma;
using IO::MouseButtons;
using Kinesis::AABB2D;
using Portal::Graphics::PrimitiveTopology;

namespace {

    const AABB2D k_viewport { .min = { -0.5F, -0.5F }, .max = { 0.5F, 0.5F } };
    constexpr float k_row_h = 0.25F;
    constexpr size_t k_rows = 8;

    void expect_near(const AABB2D& a, const AABB2D& b)
    {
        EXPECT_NEAR(a.min.x, b.min.x, 1e-5F);
        EXPECT_NEAR(a.min.y, b.min.y, 1e-5F);
        EXPECT_NEAR(a.max.x, b.max.x, 1e-5F);
        EXPECT_NEAR(a.max.y, b.max.y, 1e-5F);
    }

    struct Row {
        uint32_t id {};
        AABB2D base {};
        std::shared_ptr<AABB2D> drawn;
        std::shared_ptr<int> repositions;
    };

}

class FormaScrollableTest : public ::testing::Test {
protected:
    Vruta::EventManager manager;
    std::shared_ptr<ScriptedWindow> window = std::make_shared<ScriptedWindow>(200U, 100U);
    std::shared_ptr<Forma::Layer> layer = std::make_shared<Forma::Layer>();
    std::shared_ptr<Forma::Context> ctx = std::make_shared<Forma::Context>(layer, window, manager, "scroll");
    Forma::Surface surface { Forma::SurfaceConfig { .window = window, .layer = layer, .ctx = ctx, .detach_on_close = false } };

    Forma::Scrollable make_panel(float wheel_speed = 0.1F)
    {
        auto buf = std::make_shared<Buffers::FormaBuffer>(4096, PrimitiveTopology::TRIANGLE_STRIP);
        return Forma::make_scrollable(std::move(buf), surface, k_viewport, glm::vec3(0.1F), wheel_speed);
    }

    Row add_row(Forma::Scrollable& panel, bool interactive = false)
    {
        Row row;
        row.base = panel.cursor_out.advance(k_row_h);
        row.drawn = std::make_shared<AABB2D>(row.base);
        row.repositions = std::make_shared<int>(0);

        Forma::Element el = Forma::Element {}.with_bounds(row.base);
        el.interactive = interactive;
        row.id = surface.add(std::move(el));

        panel.track(surface.layer(), row.id, row.base,
            [drawn = row.drawn, count = row.repositions](AABB2D shifted) {
                *drawn = shifted;
                ++*count;
            });
        return row;
    }

    std::vector<Row> add_rows(Forma::Scrollable& panel, size_t count, bool interactive = false)
    {
        std::vector<Row> rows;
        for (size_t i = 0; i < count; ++i)
            rows.push_back(add_row(panel, interactive));
        return rows;
    }
};

// =============================================================================
// Placement
// =============================================================================

TEST_F(FormaScrollableTest, PlaceRegistersTheViewportAsAnInteractiveElement)
{
    auto panel = make_panel();

    ASSERT_NE(panel.viewport_id, 0U);
    ASSERT_NE(layer->get(panel.viewport_id), nullptr);
    expect_near(*layer->get(panel.viewport_id)->bounds_hint, k_viewport);
    EXPECT_EQ(layer->hit_test(glm::vec2(0.F)), panel.viewport_id);
    EXPECT_FALSE(layer->hit_test(glm::vec2(0.9F)).has_value());
    expect_near(panel.bounds(), k_viewport);
}

TEST_F(FormaScrollableTest, PlacePopulatesTheSharedScrollState)
{
    auto panel = make_panel();

    ASSERT_NE(panel.scroll, nullptr);
    ASSERT_NE(panel.offset, nullptr);
    EXPECT_EQ(panel.scroll->offset, glm::vec2(0.F));
    expect_near(panel.scroll->viewport_bounds, k_viewport);
    expect_near(panel.scroll->content_bounds, k_viewport);
    EXPECT_EQ(panel.offset->value, glm::vec2(0.F));
}

TEST_F(FormaScrollableTest, CursorStartsAtTheViewportTopAndStacksDownwardInItsColumn)
{
    auto panel = make_panel();

    const AABB2D first = panel.cursor_out.advance(k_row_h);
    const AABB2D second = panel.cursor_out.advance(k_row_h);

    expect_near(first, AABB2D { .min = { -0.5F, 0.25F }, .max = { 0.5F, 0.5F } });
    expect_near(second, AABB2D { .min = { -0.5F, 0.F }, .max = { 0.5F, 0.25F } });
}

TEST_F(FormaScrollableTest, SettersConfigureBeforePlace)
{
    Forma::Scrollable panel;
    panel.background_color({ 0.2F, 0.3F, 0.4F }).wheel_speed(0.3F);

    EXPECT_EQ(panel.m_background, glm::vec3(0.2F, 0.3F, 0.4F));
    EXPECT_FLOAT_EQ(panel.m_wheel_speed, 0.3F);
}

// =============================================================================
// Tracking
// =============================================================================

TEST_F(FormaScrollableTest, TrackGrowsTheContentBoundsToCoverEveryRow)
{
    auto panel = make_panel();

    add_rows(panel, k_rows);

    EXPECT_NEAR(panel.scroll->content_bounds.min.y, 0.5F - k_row_h * static_cast<float>(k_rows), 1e-5F);
    EXPECT_NEAR(panel.scroll->content_bounds.max.y, 0.5F, 1e-5F);
    EXPECT_NEAR(panel.scroll->content_bounds.min.x, -0.5F, 1e-5F);
    EXPECT_NEAR(panel.scroll->content_bounds.max.x, 0.5F, 1e-5F);
}

TEST_F(FormaScrollableTest, TrackRelatesRowsUnderTheViewport)
{
    auto panel = make_panel();

    const auto rows = add_rows(panel, 3);

    EXPECT_EQ(layer->related_ids(panel.viewport_id),
        (std::vector<uint32_t> { rows[0].id, rows[1].id, rows[2].id }));
}

TEST_F(FormaScrollableTest, RemovingTheViewportRemovesEveryTrackedRow)
{
    auto panel = make_panel();
    const auto rows = add_rows(panel, 3);
    const size_t before = layer->size();

    ASSERT_TRUE(layer->remove(panel.viewport_id));

    EXPECT_EQ(layer->size(), before - 4);
    for (const auto& row : rows)
        EXPECT_EQ(layer->get(row.id), nullptr);
}

TEST_F(FormaScrollableTest, TrackAndScrollByBeforePlaceDoNothing)
{
    Forma::Scrollable unplaced;
    const uint32_t id = surface.add(Forma::Element {}.with_bounds(k_viewport));

    unplaced.track(*layer, id, k_viewport, [](AABB2D) { });
    unplaced.scroll_by(*layer, { 0.F, 1.F });

    EXPECT_TRUE(layer->related_ids(id).empty());
    EXPECT_EQ(unplaced.scroll, nullptr);
}

TEST_F(FormaScrollableTest, TrackingWhileScrolledStoresTheUnscrolledAnchor)
{
    auto panel = make_panel();
    add_rows(panel, k_rows);
    panel.scroll_by(*layer, { 0.F, 0.3F });

    const AABB2D placed = panel.cursor_out.advance(k_row_h);
    const uint32_t id = surface.add(Forma::Element {}.with_bounds(placed).non_interactive());
    auto drawn = std::make_shared<AABB2D>(placed);
    panel.track(*layer, id, placed, [drawn](AABB2D shifted) { *drawn = shifted; });
    panel.scroll_by(*layer, { 0.F, 0.F });

    expect_near(*drawn, placed);
    expect_near(*layer->get(id)->bounds_hint, placed);
}

// =============================================================================
// Scrolling
// =============================================================================

TEST_F(FormaScrollableTest, ScrollByShiftsEveryTrackedRowAndTellsItsCallback)
{
    auto panel = make_panel();
    const auto rows = add_rows(panel, k_rows);

    panel.scroll_by(*layer, { 0.F, 0.3F });

    for (const auto& row : rows) {
        const AABB2D expected = row.base.translated({ 0.F, 0.3F });
        expect_near(*layer->get(row.id)->bounds_hint, expected);
        expect_near(*row.drawn, expected);
        EXPECT_EQ(*row.repositions, 1);
    }
    EXPECT_NEAR(panel.scroll->offset.y, 0.3F, 1e-6F);
    EXPECT_NEAR(panel.offset->value.y, 0.3F, 1e-6F);
}

TEST_F(FormaScrollableTest, ScrollByAccumulatesAndClampsToTheContent)
{
    auto panel = make_panel();
    add_rows(panel, k_rows);
    const float max_offset = (k_row_h * static_cast<float>(k_rows)) - k_viewport.height();

    panel.scroll_by(*layer, { 0.F, 0.4F });
    panel.scroll_by(*layer, { 0.F, 0.4F });
    EXPECT_NEAR(panel.scroll->offset.y, 0.8F, 1e-6F);

    panel.scroll_by(*layer, { 0.F, 50.F });
    EXPECT_NEAR(panel.scroll->offset.y, max_offset, 1e-6F);

    panel.scroll_by(*layer, { 0.F, -50.F });
    EXPECT_FLOAT_EQ(panel.scroll->offset.y, 0.F);
}

TEST_F(FormaScrollableTest, ScrollByPublishesTheOffsetThroughTheSharedState)
{
    auto panel = make_panel();
    add_rows(panel, k_rows);
    const auto before = panel.offset->version;

    panel.scroll_by(*layer, { 0.F, 0.2F });

    EXPECT_GT(panel.offset->version, before);
}

TEST_F(FormaScrollableTest, ContentThatFitsTheViewportDoesNotScroll)
{
    auto panel = make_panel();
    add_rows(panel, 2);

    panel.scroll_by(*layer, { 0.F, 1.F });

    EXPECT_FLOAT_EQ(panel.scroll->offset.y, 0.F);
}

TEST_F(FormaScrollableTest, RowsScrolledOutOfTheViewportAreNotHittableOutsideIt)
{
    auto panel = make_panel();
    const auto rows = add_rows(panel, k_rows, true);
    panel.scroll_by(*layer, { 0.F, 0.5F });
    const glm::vec2 above_panel { 0.F, 0.9F };
    ASSERT_GT(rows[0].drawn->min.y, k_viewport.max.y);
    ASSERT_TRUE(rows[0].drawn->contains(above_panel));

    EXPECT_NE(layer->hit_test(above_panel), rows[0].id);
}

TEST_F(FormaScrollableTest, RowsBelowTheViewportAreNotHittableBeforeAnyScroll)
{
    auto panel = make_panel();
    const auto rows = add_rows(panel, k_rows, true);
    const glm::vec2 below_panel { 0.F, -0.875F };
    ASSERT_TRUE(rows[5].base.contains(below_panel));

    EXPECT_FALSE(layer->hit_test(below_panel).has_value());
}

TEST_F(FormaScrollableTest, PartlyVisibleRowsAreHittableOnlyInsideTheViewport)
{
    auto panel = make_panel();
    const auto rows = add_rows(panel, k_rows, true);
    panel.scroll_by(*layer, { 0.F, 0.1F });
    const glm::vec2 inside { 0.F, 0.45F };
    const glm::vec2 outside { 0.F, 0.55F };
    ASSERT_TRUE(rows[0].drawn->contains(inside));
    ASSERT_TRUE(rows[0].drawn->contains(outside));

    EXPECT_EQ(layer->hit_test(inside), rows[0].id);
    EXPECT_FALSE(layer->hit_test(outside).has_value());
}

TEST_F(FormaScrollableTest, TrackedRowsKeepTheViewportAsTheirClip)
{
    auto panel = make_panel();
    const auto rows = add_rows(panel, 3);

    for (const auto& row : rows) {
        ASSERT_TRUE(layer->get(row.id)->clip.has_value());
        expect_near(*layer->get(row.id)->clip, k_viewport);
    }
    EXPECT_FALSE(layer->get(panel.viewport_id)->clip.has_value());
}

TEST_F(FormaScrollableTest, TrackingABufferThatHasNoRenderProcessorYetStillClipsHits)
{
    auto panel = make_panel();
    auto buf = std::make_shared<Buffers::FormaBuffer>(4096, PrimitiveTopology::TRIANGLE_STRIP);
    ASSERT_EQ(buf->get_render_processor(), nullptr);
    const AABB2D bounds = panel.cursor_out.advance(k_row_h);
    const uint32_t id = surface.add(Forma::Element {}.with_bounds(bounds).with_buffer(buf));

    panel.track(*layer, id, bounds, [](AABB2D) { }, buf);

    ASSERT_TRUE(layer->get(id)->clip.has_value());
    expect_near(*layer->get(id)->clip, k_viewport);
}

TEST_F(FormaScrollableTest, TheClipSurvivesAGeometryFunctionRewritingTheBounds)
{
    auto panel = make_panel();
    const auto rows = add_rows(panel, k_rows, true);
    panel.scroll_by(*layer, { 0.F, 0.5F });
    const glm::vec2 above_panel { 0.F, 0.9F };

    layer->set_bounds(rows[0].id, *rows[0].drawn);
    layer->set_contains(rows[0].id, [](glm::vec2) { return true; });

    EXPECT_FALSE(layer->hit_test(above_panel).has_value());
}

TEST_F(FormaScrollableTest, DrawnBoundsAreNeverClipped)
{
    auto panel = make_panel();
    const auto rows = add_rows(panel, k_rows);

    panel.scroll_by(*layer, { 0.F, 0.5F });

    expect_near(*rows[0].drawn, rows[0].base.translated({ 0.F, 0.5F }));
    EXPECT_GT(rows[0].drawn->max.y, k_viewport.max.y);
}

// =============================================================================
// Wheel input
// =============================================================================

TEST_F(FormaScrollableTest, WheelOverTheViewportScrollsByDeltaTimesSpeed)
{
    auto panel = make_panel(0.1F);
    add_rows(panel, k_rows);

    window->move_to_ndc({ 0.F, 0.F });
    window->scroll(0.0, 2.0);

    EXPECT_NEAR(panel.scroll->offset.y, 0.2F, 1e-6F);
    EXPECT_NEAR(panel.offset->value.y, 0.2F, 1e-6F);
}

TEST_F(FormaScrollableTest, WheelOutsideTheViewportDoesNothing)
{
    auto panel = make_panel(0.1F);
    add_rows(panel, k_rows);

    window->move_to_ndc({ 0.9F, 0.9F });
    window->scroll(0.0, 2.0);

    EXPECT_FLOAT_EQ(panel.scroll->offset.y, 0.F);
}

TEST_F(FormaScrollableTest, WheelOverAnInteractiveRowNeedsTheWheelHandlerRegisteredOnIt)
{
    auto panel = make_panel(0.1F);
    const auto rows = add_rows(panel, k_rows, true);
    const glm::vec2 on_first_row { 0.F, 0.375F };

    window->move_to_ndc(on_first_row);
    window->scroll(0.0, 2.0);
    EXPECT_FLOAT_EQ(panel.scroll->offset.y, 0.F);

    ctx->on_scroll(rows[0].id, panel.wheel_handler(surface));
    window->move_to_ndc(on_first_row);
    window->scroll(0.0, 2.0);

    EXPECT_NEAR(panel.scroll->offset.y, 0.2F, 1e-6F);
}

TEST_F(FormaScrollableTest, WheelHandlerSurvivesTheScrollableValueItCameFrom)
{
    std::shared_ptr<Kinesis::ScrollState> scroll;
    {
        auto panel = make_panel(0.1F);
        const auto rows = add_rows(panel, k_rows, true);
        ctx->on_scroll(rows[0].id, panel.wheel_handler(surface));
        scroll = panel.scroll;
    }

    window->move_to_ndc({ 0.F, 0.375F });
    window->scroll(0.0, 1.0);

    EXPECT_NEAR(scroll->offset.y, 0.1F, 1e-6F);
}

// =============================================================================
// Indicator
// =============================================================================

class FormaScrollableIndicatorTest : public FormaScrollableTest {
protected:
    const AABB2D track { .min = { 0.6F, -0.5F }, .max = { 0.7F, 0.5F } };

    Forma::Mapped<glm::vec2> make_indicator()
    {
        auto form = Forma::Geometry::scroll_indicator(track);
        auto buf = std::make_shared<Buffers::FormaBuffer>(form.capacity, form.topology);
        auto mapped = Forma::make_mapped<glm::vec2>(glm::vec2(0.F, 1.F), form.geometry, std::move(buf));
        mapped.element.id = surface.add(mapped.element);
        return mapped;
    }
};

TEST_F(FormaScrollableIndicatorTest, IndicatorStartsAtTheTopWithTheVisibleFraction)
{
    auto panel = make_panel();
    add_rows(panel, k_rows);
    auto indicator = make_indicator();
    const auto state = indicator.state;

    panel.indicator(surface, std::move(indicator), track);

    EXPECT_NEAR(state->value.x, 0.F, 1e-6F);
    EXPECT_NEAR(state->value.y, k_viewport.height() / (k_row_h * static_cast<float>(k_rows)), 1e-6F);
}

TEST_F(FormaScrollableIndicatorTest, ContentThatFitsReportsFullExtentAtPositionZero)
{
    auto panel = make_panel();
    auto indicator = make_indicator();
    const auto state = indicator.state;

    panel.indicator(surface, std::move(indicator), track);

    EXPECT_EQ(state->value, glm::vec2(0.F, 1.F));
}

TEST_F(FormaScrollableIndicatorTest, TrackingMoreRowsRefreshesTheExtent)
{
    auto panel = make_panel();
    add_rows(panel, 2);
    auto indicator = make_indicator();
    const auto state = indicator.state;
    panel.indicator(surface, std::move(indicator), track);
    ASSERT_FLOAT_EQ(state->value.y, 1.F);

    add_rows(panel, 6);

    EXPECT_NEAR(state->value.y, 0.5F, 1e-6F);
}

TEST_F(FormaScrollableIndicatorTest, ScrollingMovesThePositionFractionFromZeroToOne)
{
    auto panel = make_panel();
    add_rows(panel, k_rows);
    auto indicator = make_indicator();
    const auto state = indicator.state;
    panel.indicator(surface, std::move(indicator), track);
    const float max_offset = (k_row_h * static_cast<float>(k_rows)) - k_viewport.height();

    panel.scroll_by(*layer, { 0.F, max_offset * 0.5F });
    EXPECT_NEAR(state->value.x, 0.5F, 1e-5F);

    panel.scroll_by(*layer, { 0.F, max_offset });
    EXPECT_NEAR(state->value.x, 1.F, 1e-5F);
}

TEST_F(FormaScrollableIndicatorTest, WheelScrollingUpdatesTheIndicator)
{
    auto panel = make_panel(0.1F);
    add_rows(panel, k_rows);
    auto indicator = make_indicator();
    const auto state = indicator.state;
    panel.indicator(surface, std::move(indicator), track);

    window->move_to_ndc({ 0.F, 0.F });
    window->scroll(0.0, 5.0);

    EXPECT_NEAR(state->value.x, 0.5F, 1e-5F);
}

TEST_F(FormaScrollableIndicatorTest, DraggingTheIndicatorMapsTheCursorOntoTheScrollRange)
{
    auto panel = make_panel();
    add_rows(panel, k_rows);
    auto indicator = make_indicator();
    panel.indicator(surface, std::move(indicator), track);
    const float max_offset = (k_row_h * static_cast<float>(k_rows)) - k_viewport.height();

    window->move_to_ndc({ 0.65F, 0.5F });
    window->press(MouseButtons::Left);
    window->move_to_ndc({ 0.65F, 0.F });

    EXPECT_NEAR(panel.scroll->offset.y, max_offset * 0.5F, 1e-5F);

    window->move_to_ndc({ 0.65F, -0.5F });
    EXPECT_NEAR(panel.scroll->offset.y, max_offset, 1e-5F);

    window->move_to_ndc({ 0.65F, 2.F });
    EXPECT_NEAR(panel.scroll->offset.y, 0.F, 1e-5F);
}

TEST_F(FormaScrollableIndicatorTest, DraggingTheIndicatorReflowsTheRows)
{
    auto panel = make_panel();
    const auto rows = add_rows(panel, k_rows);
    auto indicator = make_indicator();
    panel.indicator(surface, std::move(indicator), track);

    window->move_to_ndc({ 0.65F, 0.5F });
    window->press(MouseButtons::Left);
    window->move_to_ndc({ 0.65F, 0.F });

    expect_near(*rows[0].drawn, rows[0].base.translated(panel.scroll->offset));
}

TEST_F(FormaScrollableIndicatorTest, IndicatorBeforePlaceIsIgnored)
{
    Forma::Scrollable unplaced;
    auto indicator = make_indicator();
    const auto state = indicator.state;
    const auto version = state->version;

    unplaced.indicator(surface, std::move(indicator), track);

    EXPECT_EQ(state->version, version);
}

}
