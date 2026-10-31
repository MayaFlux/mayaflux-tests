#include "gtest/gtest.h"

#include "../../support/scripted_window.hpp"

#include "MayaFlux/Portal/Forma/Context.hpp"
#include "MayaFlux/Portal/Forma/Primitives/Collapsible.hpp"
#include "MayaFlux/Portal/Forma/Surface.hpp"
#include "MayaFlux/Vruta/EventManager.hpp"

namespace MayaFlux::Test {

namespace Forma = Portal::Forma;
using IO::MouseButtons;
using Kinesis::AABB2D;
using Portal::Graphics::PrimitiveTopology;

namespace {

    void expect_near(const AABB2D& a, const AABB2D& b)
    {
        EXPECT_NEAR(a.min.x, b.min.x, 1e-5F);
        EXPECT_NEAR(a.min.y, b.min.y, 1e-5F);
        EXPECT_NEAR(a.max.x, b.max.x, 1e-5F);
        EXPECT_NEAR(a.max.y, b.max.y, 1e-5F);
    }

}

class FormaCollapsibleTest : public ::testing::Test {
protected:
    Vruta::EventManager manager;
    std::shared_ptr<ScriptedWindow> window = std::make_shared<ScriptedWindow>(200U, 100U);
    std::shared_ptr<Forma::Layer> layer = std::make_shared<Forma::Layer>();
    std::shared_ptr<Forma::Context> ctx = std::make_shared<Forma::Context>(layer, window, manager, "collapse");
    Forma::Surface surface { Forma::SurfaceConfig { .window = window, .layer = layer, .ctx = ctx, .detach_on_close = false } };
    Forma::LayoutCursor cursor;

    const glm::vec2 header_centre { 0.F, 0.9F };
    const AABB2D body_bounds { .min = { -1.F, -0.5F }, .max = { 1.F, 0.F } };
    const glm::vec2 body_centre { 0.F, -0.25F };

    static std::shared_ptr<Buffers::FormaBuffer> make_buffer()
    {
        return std::make_shared<Buffers::FormaBuffer>(4096, PrimitiveTopology::TRIANGLE_LIST);
    }

    Forma::Collapsible place_header(bool initially_open = true)
    {
        return Forma::Collapsible {}
            .initially_open(initially_open)
            .place(make_buffer(), surface, cursor, -1.F, 1.F, 0.2F);
    }

    uint32_t add_body()
    {
        return surface.add(Forma::Element {}.with_bounds(body_bounds));
    }

    void click(glm::vec2 ndc, MouseButtons button = MouseButtons::Left)
    {
        window->move_to_ndc(ndc);
        window->press(button);
        window->release(button);
    }
};

// =============================================================================
// Placement
// =============================================================================

TEST_F(FormaCollapsibleTest, PlaceRegistersTheHeaderRowAndAdvancesTheCursor)
{
    auto header = place_header();

    ASSERT_NE(header.header_id, 0U);
    expect_near(header.header_bounds, AABB2D { .min = { -1.F, 0.8F }, .max = { 1.F, 1.F } });
    expect_near(header.bounds(), header.header_bounds);
    ASSERT_NE(layer->get(header.header_id), nullptr);
    expect_near(*layer->get(header.header_id)->bounds_hint, header.header_bounds);
    EXPECT_NEAR(cursor.y(), 0.8F, 1e-6F);
    EXPECT_NEAR(header.cursor_out.y(), 0.8F, 1e-6F);
    EXPECT_NE(header.buf, nullptr);
}

TEST_F(FormaCollapsibleTest, PlaceWithoutExplicitColumnUsesTheCursorColumn)
{
    Forma::LayoutCursor column(1.F, -0.5F, 0.25F);

    auto header = Forma::Collapsible {}.place(make_buffer(), surface, column, 0.2F);

    expect_near(header.header_bounds, AABB2D { .min = { -0.5F, 0.8F }, .max = { 0.25F, 1.F } });
}

TEST_F(FormaCollapsibleTest, TwoHeadersStackDownTheSameCursor)
{
    auto first = place_header();
    auto second = place_header();

    EXPECT_NEAR(second.header_bounds.max.y, first.header_bounds.min.y, 1e-6F);
    EXPECT_NEAR(cursor.y(), 0.6F, 1e-6F);
    EXPECT_NE(first.header_id, second.header_id);
}

TEST_F(FormaCollapsibleTest, OpenStateStartsAsConfiguredAndCarriesTheHeaderId)
{
    auto open_header = place_header(true);
    auto closed_header = place_header(false);

    EXPECT_TRUE(open_header.open->value);
    EXPECT_FALSE(closed_header.open->value);
    EXPECT_EQ(open_header.open->id, open_header.header_id);
    EXPECT_EQ(closed_header.open->id, closed_header.header_id);
}

TEST_F(FormaCollapsibleTest, SettersChainOnTheSameObject)
{
    Forma::Collapsible header;

    Forma::Collapsible& chained = header.initially_open(false)
                                      .closed_color({ 0.1F, 0.1F, 0.1F })
                                      .open_color({ 0.9F, 0.9F, 0.9F })
                                      .label("section")
                                      .label_color({ 1.F, 0.F, 0.F, 1.F });

    EXPECT_EQ(&chained, &header);
    EXPECT_FALSE(header.m_initially_open);
    EXPECT_EQ(header.m_color_closed, glm::vec3(0.1F));
    EXPECT_EQ(header.m_color_open, glm::vec3(0.9F));
    EXPECT_EQ(header.m_label_text, "section");
    EXPECT_EQ(header.m_label_color, glm::vec4(1.F, 0.F, 0.F, 1.F));
}

TEST_F(FormaCollapsibleTest, MakeCollapsibleAppliesItsArguments)
{
    auto header = Forma::make_collapsible(make_buffer(), surface, cursor, -0.5F, 0.5F, 0.1F,
        false, glm::vec3(0.2F), glm::vec3(0.7F));

    EXPECT_FALSE(header.open->value);
    EXPECT_EQ(header.m_color_closed, glm::vec3(0.2F));
    EXPECT_EQ(header.m_color_open, glm::vec3(0.7F));
    expect_near(header.header_bounds, AABB2D { .min = { -0.5F, 0.9F }, .max = { 0.5F, 1.F } });
}

// =============================================================================
// Toggling
// =============================================================================

TEST_F(FormaCollapsibleTest, ClickingTheHeaderTogglesTheStateEachTime)
{
    auto header = place_header(true);

    click(header_centre);
    EXPECT_FALSE(header.open->value);
    click(header_centre);
    EXPECT_TRUE(header.open->value);
}

TEST_F(FormaCollapsibleTest, OnlyTheLeftButtonToggles)
{
    auto header = place_header(true);

    click(header_centre, MouseButtons::Right);
    click(header_centre, MouseButtons::Middle);

    EXPECT_TRUE(header.open->value);
}

TEST_F(FormaCollapsibleTest, ClickingOutsideTheHeaderDoesNotToggle)
{
    auto header = place_header(true);

    click({ 0.F, -0.9F });

    EXPECT_TRUE(header.open->value);
}

TEST_F(FormaCollapsibleTest, ToggleBumpsTheStateVersion)
{
    auto header = place_header(true);
    const auto before = header.open->version;

    click(header_centre);

    EXPECT_GT(header.open->version, before);
}

TEST_F(FormaCollapsibleTest, TheHeaderKeepsItsHitBoundsWhileToggling)
{
    auto header = place_header(true);

    click(header_centre);
    click(header_centre);

    expect_near(*layer->get(header.header_id)->bounds_hint, header.header_bounds);
}

// =============================================================================
// Attached bodies
// =============================================================================

TEST_F(FormaCollapsibleTest, AttachRelatesTheBodyAndAppliesTheCurrentState)
{
    auto open_header = place_header(true);
    auto closed_header = place_header(false);
    const uint32_t open_body = add_body();
    const uint32_t closed_body = add_body();

    open_header.attach(*layer, open_body);
    closed_header.attach(*layer, closed_body);

    EXPECT_EQ(layer->related_ids(open_header.header_id), std::vector<uint32_t> { open_body });
    EXPECT_TRUE(layer->get(open_body)->visible);
    EXPECT_FALSE(layer->get(closed_body)->visible);
}

TEST_F(FormaCollapsibleTest, ClickingShowsAndHidesEveryAttachedBody)
{
    auto header = place_header(true);
    const uint32_t first = add_body();
    const uint32_t second = add_body();
    header.attach(*layer, first);
    header.attach(*layer, second);

    click(header_centre);
    EXPECT_FALSE(layer->get(first)->visible);
    EXPECT_FALSE(layer->get(second)->visible);

    click(header_centre);
    EXPECT_TRUE(layer->get(first)->visible);
    EXPECT_TRUE(layer->get(second)->visible);
}

TEST_F(FormaCollapsibleTest, TheToggleCascadesToBodiesRelatedUnderABody)
{
    auto header = place_header(true);
    const uint32_t body = add_body();
    const uint32_t inner = add_body();
    header.attach(*layer, body);
    ASSERT_TRUE(layer->relate(body, inner));

    click(header_centre);

    EXPECT_FALSE(layer->get(inner)->visible);
}

TEST_F(FormaCollapsibleTest, AClosedBodyCannotBeHitAndAnOpenOneCan)
{
    auto header = place_header(false);
    const uint32_t body = add_body();
    header.attach(*layer, body);
    ASSERT_FALSE(layer->hit_test(body_centre).has_value());

    click(header_centre);

    EXPECT_EQ(layer->hit_test(body_centre), body);
}

TEST_F(FormaCollapsibleTest, AttachingAnUnknownBodyIsHarmless)
{
    auto header = place_header(true);

    header.attach(*layer, 9999);

    EXPECT_TRUE(layer->related_ids(header.header_id).empty());
}

TEST_F(FormaCollapsibleTest, AttachBeforePlaceDoesNothing)
{
    Forma::Collapsible unplaced;
    const uint32_t body = add_body();

    unplaced.attach(*layer, body);

    EXPECT_TRUE(layer->get(body)->visible);
}

}
