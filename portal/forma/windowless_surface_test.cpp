#include "../../support/forma_rig.hpp"
#include "../../support/test_metadata.hpp"

#include "MayaFlux/Kinesis/Spatial/HitTest.hpp"
#include "MayaFlux/Portal/Forma/Internal/Atelier.hpp"
#include "MayaFlux/Portal/Forma/Plot/Plot.hpp"
#include "MayaFlux/Portal/Forma/Primitives/Entry.hpp"
#include "MayaFlux/Portal/Forma/Surface.hpp"
#include "MayaFlux/Portal/Graphics/ImageTarget.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::system_font, ::MayaFlux::Test::Meta::Capability::vulkan>() }))

namespace MayaFlux::Test {

namespace Forma = Portal::Forma;
using IO::Keys;
using IO::Modifiers;
using IO::MouseButtons;

namespace {

    void expect_near(glm::vec2 a, glm::vec2 b)
    {
        EXPECT_NEAR(a.x, b.x, 1e-4F);
        EXPECT_NEAR(a.y, b.y, 1e-4F);
    }

}

class FormaWindowlessFixture : public FormaRigTest {
protected:
    void SetUp() override
    {
        FormaRigTest::SetUp();
        if (IsSkipped())
            return;

        target = Portal::Graphics::get_texture_manager().create_image_target({ .width = 200, .height = 100 });
        ASSERT_NE(target, nullptr);
        offscreen.emplace(Forma::create_surface(Forma::SurfaceConfig { .image = target }));
        host.emplace(make_surface("windowless-host"));
    }

    void TearDown() override
    {
        offscreen.reset();
        host.reset();
        target.reset();
        FormaRigTest::TearDown();
    }

    uint32_t add_to(Forma::Surface& surface, glm::vec2 min, glm::vec2 max)
    {
        return surface.add(Forma::Element {}.with_rect(min, max)).id();
    }

    uint32_t whole_offscreen() { return add_to(*offscreen, { -1.F, -1.F }, { 1.F, 1.F }); }

    uint32_t left_offscreen() { return add_to(*offscreen, { -1.F, -1.F }, { -0.1F, 1.F }); }

    std::shared_ptr<Portal::Graphics::ImageTarget> target;
    std::optional<Forma::Surface> offscreen;
    std::optional<Forma::Surface> host;
};

// =============================================================================
// A Surface with no window
// =============================================================================

TEST_F(FormaWindowlessFixture, ASurfaceMadeFromAnImageHasNoWindowAndDrawsIntoTheImage)
{
    EXPECT_EQ(offscreen->window(), nullptr);
    EXPECT_EQ(offscreen->image(), target);

    const auto config = offscreen->render_config();
    EXPECT_EQ(config.target_window, nullptr);
    ASSERT_EQ(config.target_images.size(), 1U);
    EXPECT_EQ(config.target_images.front(), target);
}

TEST_F(FormaWindowlessFixture, ASurfaceWithAWindowIgnoresTheImageItWasGiven)
{
    Forma::SurfaceConfig config;
    config.window = window;
    config.image = target;
    config.name = "windowed-with-image";
    config.detach_on_close = false;

    const auto surface = Forma::create_surface(std::move(config));

    EXPECT_EQ(surface.window(), window);
    EXPECT_EQ(surface.image(), nullptr);
    EXPECT_EQ(surface.render_config().target_window, window);
    EXPECT_TRUE(surface.render_config().target_images.empty());
}

TEST_F(FormaWindowlessFixture, TheExtentIsTheImagesSizeAndFollowsItsResize)
{
    EXPECT_EQ(offscreen->extent(), glm::uvec2(200U, 100U));
    EXPECT_EQ(offscreen->ctx().extent(), glm::uvec2(200U, 100U));
    EXPECT_EQ(host->extent(), glm::uvec2(400U, 300U));

    ASSERT_TRUE(target->resize(100, 50));

    EXPECT_EQ(offscreen->extent(), glm::uvec2(100U, 50U));
    EXPECT_EQ(offscreen->ctx().extent(), glm::uvec2(100U, 50U));
}

TEST_F(FormaWindowlessFixture, ExtentOfReadsTheWindowBeforeTheImage)
{
    EXPECT_EQ(Forma::Surface::extent_of(window, target), glm::uvec2(400U, 300U));
    EXPECT_EQ(Forma::Surface::extent_of(nullptr, target), glm::uvec2(200U, 100U));
    EXPECT_EQ(Forma::Surface::extent_of(nullptr, nullptr), glm::uvec2(0U, 0U));
}

TEST_F(FormaWindowlessFixture, AnImageBatchOfASurfaceWithoutAWindowDrawsIntoTheImage)
{
    const auto batch = Forma::internal::atelier().surface_batch(*offscreen);

    ASSERT_NE(batch, nullptr);
    const auto config = batch->get_render_config();
    EXPECT_EQ(config.target_window, nullptr);
    ASSERT_EQ(config.target_images.size(), 1U);
    EXPECT_EQ(config.target_images.front(), target);
}

TEST_F(FormaWindowlessFixture, RowPixelDimsMeasuresAnExtentTheSameWayAsAWindow)
{
    const auto from_window = Forma::row_pixel_dims(window, -1.F, 1.F, 0.5F);
    const auto from_extent = Forma::row_pixel_dims(glm::uvec2 { 400U, 300U }, -1.F, 1.F, 0.5F);

    EXPECT_EQ(from_window, from_extent);
    EXPECT_EQ(from_extent.x, 400U);
    EXPECT_EQ(Forma::row_pixel_dims(offscreen->extent(), -1.F, 1.F, 0.5F).x, 200U);
}

TEST_F(FormaWindowlessFixture, ALabelPlacedWithoutAWindowSizesItsTextFromTheImageAndSurvivesAResize)
{
    const auto slice = Forma::internal::atelier().batch_slice(*offscreen, Portal::Graphics::PrimitiveTopology::TRIANGLE_LIST);
    Forma::Plot::LabelSpec spec;
    spec.text = "label";
    spec.bounds = Kinesis::AABB2D { .min = { -0.5F, -0.5F }, .max = { 0.5F, -0.3F } };

    uint32_t id {};
    ASSERT_NO_THROW(id = Forma::Plot::place_label(*offscreen, slice, spec, 0));
    const auto* element = offscreen->layer().get(id);
    ASSERT_NE(element, nullptr);
    EXPECT_NE(element->text_slice, nullptr);

    ASSERT_TRUE(target->resize(100, 50));
    EXPECT_NO_THROW(offscreen->ctx().feed_move(1.0, 1.0));
}

TEST_F(FormaWindowlessFixture, ANullImageIsRefusedByAContextWithoutAWindow)
{
    EXPECT_THROW(Forma::Context(std::make_shared<Forma::Layer>(), nullptr), std::invalid_argument);
}

TEST_F(FormaWindowlessFixture, ADestroyedWindowlessContextCancelsNothingAndIsHarmless)
{
    auto layer = std::make_shared<Forma::Layer>();
    auto ctx = std::make_unique<Forma::Context>(layer, target);
    const uint32_t id = layer->add(Forma::Element {}.with_rect({ -1.F, -1.F }, { 1.F, 1.F }));
    ctx->on_press(id, Keys::A, [](uint32_t) { });

    EXPECT_NO_THROW(ctx.reset());
}

// =============================================================================
// Fed pointer input
// =============================================================================

TEST_F(FormaWindowlessFixture, AFedMoveHoversTheElementUnderThePixelAndLeavesIt)
{
    const uint32_t id = left_offscreen();
    int entered = 0;
    int left = 0;
    offscreen->ctx().on_enter(id, [&](uint32_t) { ++entered; });
    offscreen->ctx().on_leave(id, [&](uint32_t) { ++left; });

    offscreen->ctx().feed_move(50.0, 50.0);
    EXPECT_EQ(offscreen->ctx().hovered(), id);
    EXPECT_EQ(entered, 1);

    offscreen->ctx().feed_move(60.0, 50.0);
    EXPECT_EQ(entered, 1);

    offscreen->ctx().feed_move(150.0, 50.0);
    EXPECT_FALSE(offscreen->ctx().hovered().has_value());
    EXPECT_EQ(left, 1);
}

TEST_F(FormaWindowlessFixture, AFedMoveReportsThePointerInNdcOfTheExtent)
{
    const uint32_t id = whole_offscreen();
    glm::vec2 seen {};
    offscreen->ctx().on_move(id, [&](uint32_t, glm::vec2 ndc) { seen = ndc; });

    offscreen->ctx().feed_move(50.0, 25.0);

    expect_near(seen, { -0.5F, 0.5F });
}

TEST_F(FormaWindowlessFixture, AFedPressAndReleaseReachTheElementAndPressTakesFocus)
{
    const uint32_t id = whole_offscreen();
    glm::vec2 pressed {};
    glm::vec2 released {};
    int presses = 0;
    int releases = 0;
    offscreen->ctx().on_press(id, MouseButtons::Left, [&](uint32_t, glm::vec2 ndc) { ++presses; pressed = ndc; });
    offscreen->ctx().on_release(id, MouseButtons::Left, [&](uint32_t, glm::vec2 ndc) { ++releases; released = ndc; });

    offscreen->ctx().feed_press(100.0, 50.0, MouseButtons::Left);
    EXPECT_EQ(presses, 1);
    expect_near(pressed, { 0.F, 0.F });
    EXPECT_EQ(offscreen->ctx().focused(), id);
    EXPECT_TRUE(offscreen->ctx().button_down(MouseButtons::Left));

    offscreen->ctx().feed_release(100.0, 50.0, MouseButtons::Left);
    EXPECT_EQ(releases, 1);
    expect_near(released, { 0.F, 0.F });
    EXPECT_FALSE(offscreen->ctx().button_down(MouseButtons::Left));
}

TEST_F(FormaWindowlessFixture, AFedPressOnNothingClearsFocus)
{
    const uint32_t id = left_offscreen();
    offscreen->ctx().feed_press(50.0, 50.0, MouseButtons::Left);
    ASSERT_EQ(offscreen->ctx().focused(), id);

    offscreen->ctx().feed_press(190.0, 50.0, MouseButtons::Left);

    EXPECT_FALSE(offscreen->ctx().focused().has_value());
}

TEST_F(FormaWindowlessFixture, AFedDragFollowsTheElementOutsideItsBoundsUntilTheRelease)
{
    const uint32_t id = left_offscreen();
    std::vector<glm::vec2> drags;
    offscreen->ctx().on_drag(id, MouseButtons::Left, [&](uint32_t, glm::vec2 ndc) { drags.push_back(ndc); });

    offscreen->ctx().feed_press(50.0, 50.0, MouseButtons::Left);
    offscreen->ctx().feed_move(190.0, 50.0);

    ASSERT_FALSE(drags.empty());
    EXPECT_NEAR(drags.back().x, 0.9F, 1e-4F);

    offscreen->ctx().feed_release(190.0, 50.0, MouseButtons::Left);
    const size_t count = drags.size();
    offscreen->ctx().feed_move(195.0, 50.0);

    EXPECT_EQ(drags.size(), count);
}

TEST_F(FormaWindowlessFixture, AMoveWithoutAHeldButtonIsNotADrag)
{
    const uint32_t id = whole_offscreen();
    int drags = 0;
    offscreen->ctx().on_drag(id, MouseButtons::Left, [&](uint32_t, glm::vec2) { ++drags; });

    offscreen->ctx().feed_move(50.0, 50.0);

    EXPECT_EQ(drags, 0);
}

TEST_F(FormaWindowlessFixture, ACancelledButtonEndsADragWithoutARelease)
{
    const uint32_t id = whole_offscreen();
    int drags = 0;
    int releases = 0;
    offscreen->ctx().on_drag(id, MouseButtons::Left, [&](uint32_t, glm::vec2) { ++drags; });
    offscreen->ctx().on_release(id, MouseButtons::Left, [&](uint32_t, glm::vec2) { ++releases; });
    offscreen->ctx().feed_press(50.0, 50.0, MouseButtons::Left);
    offscreen->ctx().feed_move(60.0, 50.0);
    ASSERT_GT(drags, 0);

    offscreen->ctx().feed_cancel(MouseButtons::Left);
    const int before = drags;
    offscreen->ctx().feed_move(70.0, 50.0);

    EXPECT_FALSE(offscreen->ctx().button_down(MouseButtons::Left));
    EXPECT_EQ(drags, before);
    EXPECT_EQ(releases, 0);
}

TEST_F(FormaWindowlessFixture, AFedScrollReachesTheHoveredElement)
{
    const uint32_t id = whole_offscreen();
    double dx = 0.0;
    double dy = 0.0;
    int scrolls = 0;
    offscreen->ctx().on_scroll(id, [&](uint32_t, glm::vec2, double x, double y) { ++scrolls; dx = x; dy = y; });

    offscreen->ctx().feed_scroll(1.0, 2.0);
    EXPECT_EQ(scrolls, 0);

    offscreen->ctx().feed_move(50.0, 50.0);
    offscreen->ctx().feed_scroll(1.0, 2.0);

    EXPECT_EQ(scrolls, 1);
    EXPECT_DOUBLE_EQ(dx, 1.0);
    EXPECT_DOUBLE_EQ(dy, 2.0);
}

TEST_F(FormaWindowlessFixture, AFedLeaveLeavesTheHoveredElement)
{
    const uint32_t id = whole_offscreen();
    int left = 0;
    offscreen->ctx().on_leave(id, [&](uint32_t) { ++left; });
    offscreen->ctx().feed_move(50.0, 50.0);
    ASSERT_EQ(offscreen->ctx().hovered(), id);

    offscreen->ctx().feed_leave();
    EXPECT_FALSE(offscreen->ctx().hovered().has_value());
    EXPECT_EQ(left, 1);

    offscreen->ctx().feed_leave();
    EXPECT_EQ(left, 1);
}

TEST_F(FormaWindowlessFixture, SubscribersReceiveFedPointerEventsWithoutAWindow)
{
    const uint32_t id = whole_offscreen();
    int presses = 0;
    Vruta::WindowEventFilter filter;
    filter.event_type = Core::WindowEventType::MOUSE_BUTTON_PRESSED;
    ASSERT_TRUE(offscreen->ctx().subscribe(id, filter, [&](uint32_t, const Core::WindowEvent&, glm::vec2) { ++presses; }));

    offscreen->ctx().feed_press(50.0, 50.0, MouseButtons::Left);

    EXPECT_EQ(presses, 1);
}

TEST_F(FormaWindowlessFixture, TheResizeCallbacksRunWhenTheImageChangedSizeAtTheNextFedEvent)
{
    const uint32_t id = whole_offscreen();
    std::vector<glm::uvec2> sizes;
    offscreen->ctx().on_resize(id, [&](uint32_t w, uint32_t h) { sizes.emplace_back(w, h); });

    ASSERT_TRUE(target->resize(100, 50));
    EXPECT_TRUE(sizes.empty());

    offscreen->ctx().feed_move(10.0, 10.0);

    ASSERT_EQ(sizes.size(), 1U);
    EXPECT_EQ(sizes.front(), glm::uvec2(100U, 50U));
}

// =============================================================================
// Fed keys and text
// =============================================================================

TEST_F(FormaWindowlessFixture, FedTextReachesOnlyTheFocusedElement)
{
    const uint32_t id = whole_offscreen();
    std::vector<uint32_t> typed;
    offscreen->ctx().on_text(id, [&](uint32_t, uint32_t codepoint) { typed.push_back(codepoint); });

    offscreen->ctx().feed_text('a');
    EXPECT_TRUE(typed.empty());

    offscreen->ctx().feed_press(50.0, 50.0, MouseButtons::Left);
    offscreen->ctx().feed_text('b');

    ASSERT_EQ(typed.size(), 1U);
    EXPECT_EQ(typed.front(), static_cast<uint32_t>('b'));
}

TEST_F(FormaWindowlessFixture, AFedKeyPressReleaseAndRepeatReachTheFocusedElementsKeyCallbacks)
{
    const uint32_t id = whole_offscreen();
    int pressed = 0;
    int released = 0;
    int held = 0;
    offscreen->ctx().on_press(id, Keys::A, [&](uint32_t) { ++pressed; });
    offscreen->ctx().on_release(id, Keys::A, [&](uint32_t) { ++released; });
    offscreen->ctx().on_held(id, Keys::A, [&](uint32_t) { ++held; });
    offscreen->ctx().feed_press(50.0, 50.0, MouseButtons::Left);

    offscreen->ctx().feed_key(Keys::A, Core::WindowEventType::KEY_PRESSED);
    EXPECT_EQ(pressed, 1);
    EXPECT_EQ(held, 1);

    offscreen->ctx().feed_key(Keys::A, Core::WindowEventType::KEY_REPEAT);
    EXPECT_EQ(pressed, 1);
    EXPECT_EQ(held, 2);

    offscreen->ctx().feed_key(Keys::A, Core::WindowEventType::KEY_RELEASED);
    EXPECT_EQ(released, 1);
    EXPECT_EQ(held, 2);
}

TEST_F(FormaWindowlessFixture, AFedKeyWithoutFocusReachesNothing)
{
    const uint32_t id = whole_offscreen();
    int pressed = 0;
    offscreen->ctx().on_press(id, Keys::A, [&](uint32_t) { ++pressed; });

    offscreen->ctx().feed_key(Keys::A, Core::WindowEventType::KEY_PRESSED);

    EXPECT_EQ(pressed, 0);
}

TEST_F(FormaWindowlessFixture, AChordMatchesOnlyTheModifiersHeldWhileABareKeyMatchesAny)
{
    const uint32_t id = whole_offscreen();
    int bare = 0;
    int chord = 0;
    offscreen->ctx().on_press(id, Keys::V, [&](uint32_t) { ++bare; });
    offscreen->ctx().on_press(id, Keys::V | Modifiers::Ctrl, [&](uint32_t) { ++chord; });
    offscreen->ctx().feed_press(50.0, 50.0, MouseButtons::Left);

    offscreen->ctx().feed_key(Keys::V, Core::WindowEventType::KEY_PRESSED);
    EXPECT_EQ(bare, 1);
    EXPECT_EQ(chord, 0);

    offscreen->ctx().feed_key(Keys::V, Core::WindowEventType::KEY_PRESSED, Modifiers::Ctrl);
    EXPECT_EQ(bare, 2);
    EXPECT_EQ(chord, 1);

    offscreen->ctx().feed_key(Keys::V, Core::WindowEventType::KEY_PRESSED, Modifiers::Ctrl | Modifiers::Shift);
    EXPECT_EQ(chord, 1);
}

TEST_F(FormaWindowlessFixture, ALockKeyHeldDoesNotChangeAChordMatch)
{
    const uint32_t id = whole_offscreen();
    int chord = 0;
    offscreen->ctx().on_press(id, Keys::V | Modifiers::Ctrl, [&](uint32_t) { ++chord; });
    offscreen->ctx().feed_press(50.0, 50.0, MouseButtons::Left);

    offscreen->ctx().feed_key(Keys::V, Core::WindowEventType::KEY_PRESSED, Modifiers::Ctrl | Modifiers::CapsLock);

    EXPECT_EQ(chord, 1);
}

TEST_F(FormaWindowlessFixture, TheModifiersAreTheOnesOfTheLastFedKey)
{
    EXPECT_EQ(offscreen->ctx().modifiers(), Modifiers::None);

    offscreen->ctx().feed_key(Keys::A, Core::WindowEventType::KEY_PRESSED, Modifiers::Shift);
    EXPECT_EQ(offscreen->ctx().modifiers(), Modifiers::Shift);

    offscreen->ctx().feed_key(Keys::A, Core::WindowEventType::KEY_RELEASED);
    EXPECT_EQ(offscreen->ctx().modifiers(), Modifiers::None);
}

TEST_F(FormaWindowlessFixture, AWindowedContextReportsItsWindowsModifiers)
{
    EXPECT_EQ(host->ctx().modifiers(), host->ctx().event_source().modifiers());
}

TEST_F(FormaWindowlessFixture, AKeySubscriptionReceivesFedKeysOfTheFocusedElement)
{
    const uint32_t id = whole_offscreen();
    std::vector<Core::WindowEventType> seen;
    Vruta::WindowEventFilter filter;
    filter.key_code = Keys::A;
    ASSERT_TRUE(offscreen->ctx().subscribe(id, filter, [&](uint32_t, const Core::WindowEvent& event, glm::vec2) { seen.push_back(event.type); }));
    offscreen->ctx().feed_press(50.0, 50.0, MouseButtons::Left);

    offscreen->ctx().feed_key(Keys::A, Core::WindowEventType::KEY_PRESSED);
    offscreen->ctx().feed_key(Keys::B, Core::WindowEventType::KEY_PRESSED);
    offscreen->ctx().feed_key(Keys::A, Core::WindowEventType::KEY_RELEASED);

    ASSERT_EQ(seen.size(), 2U);
    EXPECT_EQ(seen.front(), Core::WindowEventType::KEY_PRESSED);
    EXPECT_EQ(seen.back(), Core::WindowEventType::KEY_RELEASED);
}

TEST_F(FormaWindowlessFixture, AFedEventThatIsNotAKeyEventIsIgnored)
{
    const uint32_t id = whole_offscreen();
    int pressed = 0;
    offscreen->ctx().on_press(id, Keys::A, [&](uint32_t) { ++pressed; });
    offscreen->ctx().feed_press(50.0, 50.0, MouseButtons::Left);

    offscreen->ctx().feed_key(Keys::A, Core::WindowEventType::MOUSE_MOTION);

    EXPECT_EQ(pressed, 0);
}

// =============================================================================
// Under a view
// =============================================================================

TEST(ViewPlanePointTest, NoViewMovementReturnsTheScreenPosition)
{
    const auto point = Kinesis::view_plane_point({ 0.3F, -0.4F }, Kinesis::ViewTransform {});

    ASSERT_TRUE(point.has_value());
    expect_near(*point, { 0.3F, -0.4F });
}

TEST(ViewPlanePointTest, ATranslatedAndScaledViewIsInverted)
{
    Kinesis::ViewTransform vt;
    vt.view[0][0] = 2.0F;
    vt.view[1][1] = 2.0F;
    vt.view[3][0] = 0.5F;

    const auto point = Kinesis::view_plane_point({ 0.5F, 0.4F }, vt);

    ASSERT_TRUE(point.has_value());
    expect_near(*point, { 0.0F, 0.2F });
}

TEST(ViewPlanePointTest, AViewSeeingThePlaneEdgeOnHasNoPoint)
{
    Kinesis::ViewTransform vt;
    vt.view[2][2] = 0.0F;
    vt.view[0][0] = 0.0F;
    vt.view[2][0] = 1.0F;
    vt.view[0][2] = 1.0F;

    EXPECT_FALSE(Kinesis::view_plane_point({ 0.5F, 0.0F }, vt).has_value());
}

TEST_F(FormaWindowlessFixture, ThePointerIsMappedThroughTheViewForHitsAndCallbacks)
{
    const uint32_t id = add_to(*offscreen, { -1.F, -1.F }, { -0.5F, 1.F });
    glm::vec2 pressed {};
    int presses = 0;
    offscreen->ctx().on_press(id, MouseButtons::Left, [&](uint32_t, glm::vec2 ndc) { ++presses; pressed = ndc; });
    Kinesis::ViewTransform vt;
    vt.view[3][0] = 0.5F;
    offscreen->set_view(vt);

    offscreen->ctx().feed_press(25.0, 50.0, MouseButtons::Left);
    EXPECT_EQ(presses, 0);

    offscreen->ctx().feed_press(75.0, 50.0, MouseButtons::Left);
    EXPECT_EQ(presses, 1);
    expect_near(pressed, { -0.75F, 0.F });
}

TEST_F(FormaWindowlessFixture, RemovingTheViewPutsTheHitRegionsBack)
{
    const uint32_t id = add_to(*offscreen, { -1.F, -1.F }, { -0.5F, 1.F });
    Kinesis::ViewTransform vt;
    vt.view[3][0] = 0.5F;
    offscreen->set_view(vt);
    offscreen->set_view(std::nullopt);

    offscreen->ctx().feed_move(25.0, 50.0);

    EXPECT_EQ(offscreen->ctx().hovered(), id);
    EXPECT_FALSE(offscreen->view().has_value());
}

TEST_F(FormaWindowlessFixture, AWindowedSurfaceMapsItsWindowPointerThroughTheViewToo)
{
    const uint32_t id = add_to(*host, { -1.F, -1.F }, { -0.5F, 1.F });
    Kinesis::ViewTransform vt;
    vt.view[3][0] = 0.5F;
    host->set_view(vt);

    window->move_to_ndc({ -0.25F, 0.F });

    EXPECT_EQ(host->ctx().hovered(), id);
}

TEST_F(FormaWindowlessFixture, TheBatchOfASurfaceFollowsTheViewOfItsLayer)
{
    const auto batch = Forma::internal::atelier().surface_batch(*offscreen);
    ASSERT_NE(batch, nullptr);
    const auto source = batch->get_render_processor()->get_view_transform_source();
    ASSERT_TRUE(static_cast<bool>(source));
    EXPECT_EQ(source().view[3][0], 0.F);

    Kinesis::ViewTransform vt;
    vt.view[3][0] = 0.5F;
    offscreen->set_view(vt);

    EXPECT_EQ(source().view[3][0], 0.5F);
}

TEST_F(FormaWindowlessFixture, ASurfaceMadeForAnAreaHasItsSizeAndDrawsIntoIt)
{
    const Kinesis::ViewportArea right {
        .area = Kinesis::Scissor::from(Kinesis::AABB2D { .min = { 0.F, -1.F }, .max = { 1.F, 1.F } })
    };
    auto partial = Forma::create_surface(Forma::SurfaceConfig { .image = target, .area = right });

    EXPECT_EQ(partial.extent(), glm::uvec2(100U, 100U));
    EXPECT_EQ(partial.ctx().extent(), glm::uvec2(100U, 100U));

    const auto batch = Forma::internal::atelier().surface_batch(partial);
    ASSERT_NE(batch, nullptr);
    EXPECT_TRUE(batch->get_render_processor()->get_target_area(target) == right);
}

TEST_F(FormaWindowlessFixture, FedPixelsOfTheImageAreMappedIntoTheAreaOfTheSurface)
{
    const Kinesis::ViewportArea right {
        .area = Kinesis::Scissor::from(Kinesis::AABB2D { .min = { 0.F, -1.F }, .max = { 1.F, 1.F } })
    };
    auto partial = Forma::create_surface(Forma::SurfaceConfig { .image = target, .area = right });
    const uint32_t id = partial.add(Forma::Element {}.with_rect({ -1.F, -1.F }, { 1.F, 1.F })).id();
    glm::vec2 pressed {};
    int presses = 0;
    partial.ctx().on_press(id, MouseButtons::Left, [&](uint32_t, glm::vec2 ndc) { ++presses; pressed = ndc; });

    partial.ctx().feed_press(50.0, 50.0, MouseButtons::Left);
    EXPECT_EQ(presses, 0);

    partial.ctx().feed_press(150.0, 50.0, MouseButtons::Left);
    EXPECT_EQ(presses, 1);
    expect_near(pressed, { 0.F, 0.F });
}

// =============================================================================
// Routing input from an on-screen element
// =============================================================================

class FormaRoutedInputFixture : public FormaWindowlessFixture {
protected:
    void SetUp() override
    {
        FormaWindowlessFixture::SetUp();
        if (IsSkipped())
            return;

        shown = add_to(*host, { -0.5F, -0.5F }, { 0.5F, 0.5F });
        inner = whole_offscreen();
        ASSERT_TRUE(offscreen->route_input_from(*host, shown));
    }

    uint32_t shown {};
    uint32_t inner {};
};

TEST_F(FormaRoutedInputFixture, TheHostPointerIsMappedFromTheElementBoundsToTheImageExtent)
{
    glm::vec2 seen {};
    int moves = 0;
    offscreen->ctx().on_move(inner, [&](uint32_t, glm::vec2 ndc) { ++moves; seen = ndc; });

    window->move_to_ndc({ -0.25F, 0.25F });

    EXPECT_GT(moves, 0);
    expect_near(seen, { -0.5F, 0.5F });
}

TEST_F(FormaRoutedInputFixture, TheTopOfTheElementIsTheTopRowOfTheImage)
{
    glm::vec2 seen {};
    offscreen->ctx().on_move(inner, [&](uint32_t, glm::vec2 ndc) { seen = ndc; });

    window->move_to_ndc({ 0.F, 0.45F });

    EXPECT_GT(seen.y, 0.8F);
}

TEST_F(FormaRoutedInputFixture, PressAndReleaseOnTheElementReachTheImageSurface)
{
    int presses = 0;
    int releases = 0;
    glm::vec2 pressed {};
    offscreen->ctx().on_press(inner, MouseButtons::Left, [&](uint32_t, glm::vec2 ndc) { ++presses; pressed = ndc; });
    offscreen->ctx().on_release(inner, MouseButtons::Left, [&](uint32_t, glm::vec2) { ++releases; });

    window->move_to_ndc({ 0.F, 0.F });
    window->press(MouseButtons::Left);
    window->release(MouseButtons::Left);

    EXPECT_EQ(presses, 1);
    EXPECT_EQ(releases, 1);
    expect_near(pressed, { 0.F, 0.F });
}

TEST_F(FormaRoutedInputFixture, APressOnTheElementGivesFocusToTheElementUnderThePointerInTheImage)
{
    window->move_to_ndc({ 0.F, 0.F });
    window->press(MouseButtons::Left);

    EXPECT_EQ(offscreen->ctx().focused(), inner);
}

TEST_F(FormaRoutedInputFixture, ScrollOnTheElementReachesTheHoveredImageElement)
{
    double dy = 0.0;
    int scrolls = 0;
    offscreen->ctx().on_scroll(inner, [&](uint32_t, glm::vec2, double, double y) { ++scrolls; dy = y; });

    window->move_to_ndc({ 0.F, 0.F });
    window->scroll(0.0, 3.0);

    EXPECT_EQ(scrolls, 1);
    EXPECT_DOUBLE_EQ(dy, 3.0);
}

TEST_F(FormaRoutedInputFixture, TextTypedWhileTheElementHasFocusReachesTheFocusedImageElement)
{
    std::vector<uint32_t> typed;
    offscreen->ctx().on_text(inner, [&](uint32_t, uint32_t codepoint) { typed.push_back(codepoint); });

    window->move_to_ndc({ 0.F, 0.F });
    window->press(MouseButtons::Left);
    window->type('z');

    ASSERT_EQ(typed.size(), 1U);
    EXPECT_EQ(typed.front(), static_cast<uint32_t>('z'));
}

TEST_F(FormaRoutedInputFixture, ADragThatBeganOnTheElementKeepsArrivingOutsideIt)
{
    std::vector<glm::vec2> drags;
    offscreen->ctx().on_drag(inner, MouseButtons::Left, [&](uint32_t, glm::vec2 ndc) { drags.push_back(ndc); });

    window->move_to_ndc({ 0.F, 0.F });
    window->press(MouseButtons::Left);
    window->move_to_ndc({ 0.9F, 0.F });

    ASSERT_FALSE(drags.empty());
    EXPECT_NEAR(drags.back().x, 1.8F, 1e-3F);
}

TEST_F(FormaRoutedInputFixture, AReleaseOutsideTheElementLetsGoOfTheImageButtonWithoutAReleaseCallback)
{
    int releases = 0;
    offscreen->ctx().on_release(inner, MouseButtons::Left, [&](uint32_t, glm::vec2) { ++releases; });

    window->move_to_ndc({ 0.F, 0.F });
    window->press(MouseButtons::Left);
    window->move_to_ndc({ 0.9F, 0.F });
    window->release(MouseButtons::Left);
    ASSERT_TRUE(offscreen->ctx().button_down(MouseButtons::Left));

    window->move_to_ndc({ 0.F, 0.F });

    EXPECT_FALSE(offscreen->ctx().button_down(MouseButtons::Left));
    EXPECT_EQ(releases, 0);
}

TEST_F(FormaRoutedInputFixture, LeavingTheElementLeavesTheHoveredImageElement)
{
    int left = 0;
    offscreen->ctx().on_leave(inner, [&](uint32_t) { ++left; });
    window->move_to_ndc({ 0.F, 0.F });
    ASSERT_EQ(offscreen->ctx().hovered(), inner);

    window->move_to_ndc({ 0.9F, 0.9F });

    EXPECT_EQ(left, 1);
    EXPECT_FALSE(offscreen->ctx().hovered().has_value());
}

TEST_F(FormaRoutedInputFixture, TheHostElementLosingFocusClearsFocusInTheImageSurface)
{
    const uint32_t elsewhere = add_to(*host, { 0.6F, 0.6F }, { 0.9F, 0.9F });
    window->move_to_ndc({ 0.F, 0.F });
    window->press(MouseButtons::Left);
    ASSERT_EQ(offscreen->ctx().focused(), inner);

    window->move_to_ndc({ 0.75F, 0.75F });
    window->press(MouseButtons::Left);
    ASSERT_EQ(host->ctx().focused(), elsewhere);

    EXPECT_FALSE(offscreen->ctx().focused().has_value());
}

TEST_F(FormaRoutedInputFixture, AMovedHostElementIsFollowedByTheMapping)
{
    glm::vec2 seen {};
    offscreen->ctx().on_move(inner, [&](uint32_t, glm::vec2 ndc) { seen = ndc; });
    ASSERT_TRUE(host->layer().set_bounds(shown, { .min = { 0.0F, 0.0F }, .max = { 1.0F, 1.0F } }));

    window->move_to_ndc({ 0.25F, 0.75F });

    expect_near(seen, { -0.5F, 0.5F });
}

TEST_F(FormaRoutedInputFixture, UnbindingTheHostElementStopsTheRouting)
{
    int moves = 0;
    offscreen->ctx().on_move(inner, [&](uint32_t, glm::vec2) { ++moves; });
    host->ctx().unbind(shown);

    window->move_to_ndc({ 0.F, 0.F });

    EXPECT_EQ(moves, 0);
}

TEST_F(FormaRoutedInputFixture, ASurfaceWithAWindowRefusesToRouteInput)
{
    const uint32_t other = add_to(*host, { 0.6F, 0.6F }, { 0.9F, 0.9F });

    EXPECT_FALSE(host->route_input_from(*host, other));
}

TEST_F(FormaRoutedInputFixture, AMissingHostElementRoutesNothing)
{
    EXPECT_FALSE(offscreen->route_input_from(*host, 99999U));
}

TEST_F(FormaRoutedInputFixture, AHostElementWithoutBoundsRoutesNothing)
{
    const uint32_t unbounded = host->add(Forma::Element {}).id();

    EXPECT_FALSE(offscreen->route_input_from(*host, unbounded));
}

} // namespace MayaFlux::Test
