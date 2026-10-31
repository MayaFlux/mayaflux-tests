#include "gtest/gtest.h"

#include "../../support/scripted_window.hpp"

#include "MayaFlux/Portal/Forma/Context.hpp"
#include "MayaFlux/Portal/Forma/Surface.hpp"
#include "MayaFlux/Vruta/EventManager.hpp"

namespace MayaFlux::Test {

namespace Forma = Portal::Forma;
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

class FormaSurfaceTest : public ::testing::Test {
protected:
    Vruta::EventManager manager;
    std::shared_ptr<ScriptedWindow> window = std::make_shared<ScriptedWindow>(200U, 100U);
    std::shared_ptr<Forma::Layer> layer = std::make_shared<Forma::Layer>();
    std::shared_ptr<Forma::Context> ctx = std::make_shared<Forma::Context>(layer, window, manager, "surface");
    Forma::Surface surface { Forma::SurfaceConfig { .window = window, .layer = layer, .ctx = ctx, .detach_on_close = false } };
};

// =============================================================================
// Ownership and passthroughs
// =============================================================================

TEST_F(FormaSurfaceTest, TheAccessorsReturnTheComponentsItWasBuiltFrom)
{
    EXPECT_EQ(&surface.layer(), layer.get());
    EXPECT_EQ(&surface.ctx(), ctx.get());
    EXPECT_EQ(surface.layer_ptr(), layer);
    EXPECT_EQ(surface.ctx_ptr(), ctx);
    EXPECT_EQ(surface.window(), window);
}

TEST_F(FormaSurfaceTest, CopiesShareTheSameLayerContextAndWindow)
{
    const Forma::Surface copy = surface;

    EXPECT_EQ(copy.layer_ptr(), surface.layer_ptr());
    EXPECT_EQ(copy.ctx_ptr(), surface.ctx_ptr());
    EXPECT_EQ(copy.window(), surface.window());
}

TEST_F(FormaSurfaceTest, AddAndRemovePassThroughToTheLayer)
{
    const uint32_t id = surface.add(Forma::Element {}.with_rect({ -1.F, -1.F }, { 1.F, 1.F }));

    EXPECT_EQ(layer->size(), 1U);
    EXPECT_NE(layer->get(id), nullptr);

    surface.remove(id);
    EXPECT_TRUE(layer->empty());

    surface.remove(id);
    EXPECT_TRUE(layer->empty());
}

TEST_F(FormaSurfaceTest, AddReturnsAChainableSlot)
{
    const uint32_t parent = surface.add(Forma::Element {}.with_rect({ -1.F, -1.F }, { 0.F, 0.F }));

    const uint32_t child = surface.add(Forma::Element {}.with_rect({ 0.F, 0.F }, { 1.F, 1.F }))
                               .relate_to(parent)
                               .id();

    EXPECT_EQ(layer->related_ids(parent), std::vector<uint32_t> { child });
}

TEST_F(FormaSurfaceTest, ClosingTheWindowWithoutDetachLeavesTheWindowOwned)
{
    window->close();

    EXPECT_EQ(surface.window(), window);
}

// =============================================================================
// Close handling rule
// =============================================================================

TEST(FormaSurfaceConfigTest, DetachOnCloseIsOnByDefault)
{
    const Forma::SurfaceConfig config;

    EXPECT_TRUE(config.detach_on_close);
    EXPECT_TRUE(config.should_detach_on_close());
}

TEST(FormaSurfaceConfigTest, DetachCanBeSwitchedOffWhenThereIsNoCloseHook)
{
    Forma::SurfaceConfig config;
    config.detach_on_close = false;

    EXPECT_FALSE(config.should_detach_on_close());
}

TEST(FormaSurfaceConfigTest, ACloseHookForcesDetachSoItCanNeverLoseTheWindowRelease)
{
    Forma::SurfaceConfig config;
    config.detach_on_close = false;
    config.on_close = [] { };

    EXPECT_TRUE(config.should_detach_on_close());
}

// =============================================================================
// Named regions
// =============================================================================

TEST_F(FormaSurfaceTest, CornerRegionsSitInsideTheScreenByTheMargin)
{
    expect_near(surface.top_left(0.5F, 0.25F), AABB2D { .min = { -0.98F, 0.73F }, .max = { -0.48F, 0.98F } });
    expect_near(surface.top_right(0.5F, 0.25F), AABB2D { .min = { 0.48F, 0.73F }, .max = { 0.98F, 0.98F } });
    expect_near(surface.bottom_left(0.5F, 0.25F), AABB2D { .min = { -0.98F, -0.98F }, .max = { -0.48F, -0.73F } });
    expect_near(surface.bottom_right(0.5F, 0.25F), AABB2D { .min = { 0.48F, -0.98F }, .max = { 0.98F, -0.73F } });
}

TEST_F(FormaSurfaceTest, CornerRegionsHonourAnExplicitMargin)
{
    expect_near(surface.top_left(0.5F, 0.25F, 0.1F), AABB2D { .min = { -0.9F, 0.65F }, .max = { -0.4F, 0.9F } });
    expect_near(surface.bottom_right(0.5F, 0.25F, 0.1F), AABB2D { .min = { 0.4F, -0.9F }, .max = { 0.9F, -0.65F } });
}

TEST_F(FormaSurfaceTest, StripsSpanTheFullEdgeInsetByTheMargin)
{
    expect_near(surface.top_strip(0.2F), AABB2D { .min = { -1.F, 0.8F }, .max = { 1.F, 1.F } });
    expect_near(surface.bottom_strip(0.2F), AABB2D { .min = { -1.F, -1.F }, .max = { 1.F, -0.8F } });
    expect_near(surface.left_strip(0.3F), AABB2D { .min = { -1.F, -1.F }, .max = { -0.7F, 1.F } });
    expect_near(surface.right_strip(0.3F), AABB2D { .min = { 0.7F, -1.F }, .max = { 1.F, 1.F } });
}

TEST_F(FormaSurfaceTest, StripsInsetFromTheEdgesByAMargin)
{
    expect_near(surface.top_strip(0.2F, 0.05F), AABB2D { .min = { -0.95F, 0.75F }, .max = { 0.95F, 0.95F } });
    expect_near(surface.left_strip(0.3F, 0.05F), AABB2D { .min = { -0.95F, -0.95F }, .max = { -0.65F, 0.95F } });
}

TEST_F(FormaSurfaceTest, CentreRectAndFullCoverTheMiddleAndTheWholeSurface)
{
    expect_near(surface.center_rect(0.5F, 1.F), AABB2D { .min = { -0.25F, -0.5F }, .max = { 0.25F, 0.5F } });
    expect_near(surface.full(), AABB2D { .min = { -1.F, -1.F }, .max = { 1.F, 1.F } });
}

TEST_F(FormaSurfaceTest, NamedRegionsAreHittableOnceUsedAsElementBounds)
{
    const uint32_t id = surface.add(Forma::Element {}.with_bounds(surface.top_left(0.5F, 0.25F)));

    EXPECT_EQ(layer->hit_test({ -0.7F, 0.85F }), id);
    EXPECT_FALSE(layer->hit_test({ 0.7F, -0.85F }).has_value());
}

}
