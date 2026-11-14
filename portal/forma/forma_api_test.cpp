#include "../../support/forma_rig.hpp"
#include "../../support/test_metadata.hpp"

#include "MayaFlux/Portal/Forma/Inspect/Inspector.hpp"
#include "MayaFlux/Portal/Forma/Primitives/Geometry.hpp"
#include "MayaFlux/Portal/Forma/Primitives/Scrollable.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::system_font, ::MayaFlux::Test::Meta::Capability::vulkan>() }))

namespace MayaFlux::Test {

namespace Forma = Portal::Forma;
using Kinesis::AABB2D;
using Portal::Graphics::PrimitiveTopology;

namespace {

    Forma::GeometryFn<float> counting_geometry(std::shared_ptr<int> calls, std::optional<AABB2D> hint = {})
    {
        return [calls = std::move(calls), hint](float, std::vector<uint8_t>& bytes, Forma::Element& element) {
            ++*calls;
            bytes.assign(sizeof(Kakshya::MeshVertex) * 4, 0);
            if (hint)
                element.bounds_hint = hint;
        };
    }

    const AABB2D k_origin_hint { .min = { -0.5F, -0.5F }, .max = { 0.5F, 0.5F } };

}

class FormaApiTest : public FormaRigTest { };

// =============================================================================
// Lifecycle and construction
// =============================================================================

TEST_F(FormaApiTest, ShutdownAndInitializeToggleTheWorkshop)
{
    EXPECT_TRUE(Forma::is_initialized());
    EXPECT_NO_THROW((void)Forma::bridge());
    EXPECT_NO_THROW((void)Forma::inspector());

    Forma::shutdown();
    EXPECT_FALSE(Forma::is_initialized());
    EXPECT_THROW((void)Forma::bridge(), std::runtime_error);
    EXPECT_THROW((void)Forma::inspector(), std::runtime_error);

    ASSERT_TRUE(start_forma());
    EXPECT_TRUE(Forma::is_initialized());
}

TEST_F(FormaApiTest, InspectorResultsDoNotSurviveAShutdownAndInitialize)
{
    auto first_surface = make_surface("api-inspect-first");
    Forma::LayoutCursor first_cursor;
    (void)Forma::inspector().node_graph_manager(first_surface, first_cursor);

    Forma::shutdown();
    ASSERT_TRUE(start_forma());
    auto second_surface = make_surface("api-inspect-second");
    Forma::LayoutCursor second_cursor;
    auto& rebuilt = Forma::inspector().node_graph_manager(second_surface, second_cursor);

    EXPECT_GT(second_surface.layer().size(), 0U);
    Forma::inspector().destroy(rebuilt);
}

TEST_F(FormaApiTest, CreateLayerAndSurfaceDelegateToTheWorkshop)
{
    auto [layer, ctx] = Forma::create_layer(window, "api-layer");
    ASSERT_NE(layer, nullptr);
    ASSERT_NE(ctx, nullptr);

    auto surface = make_surface("api-surface");
    EXPECT_NE(surface.layer_ptr(), nullptr);
    EXPECT_EQ(surface.window(), window);
}

TEST_F(FormaApiTest, CreateBufferWithATextureBindingUsesTheDefaultCapacity)
{
    auto buffer = Forma::create_buffer({ .target_window = window, .default_texture_binding = "texSampler" });

    ASSERT_NE(buffer, nullptr);
    EXPECT_EQ(buffer->get_size_bytes(), Forma::internal::k_capacity_bytes);
    EXPECT_TRUE(buffer->get_render_processor()->get_config().bindings.contains("texSampler"));
}

TEST_F(FormaApiTest, CreateBufferWithAdditionalTexturesDeclaresTheArrayBinding)
{
    auto buffer = Forma::create_buffer({ .target_window = window, .additional_textures = { { "a", nullptr }, { "b", nullptr } } });

    ASSERT_NE(buffer, nullptr);
    const auto& bindings = buffer->get_render_processor()->get_config().bindings;
    ASSERT_TRUE(bindings.contains("textures"));
    EXPECT_EQ(bindings.at("textures").count, 2U);
}

// =============================================================================
// destroy
// =============================================================================

TEST_F(FormaApiTest, DestroyRemovesTheElementItsRelatedElementsAndTheirBindings)
{
    auto surface = make_surface("api-destroy");
    auto calls = std::make_shared<int>(0);
    auto mapped = Forma::create_element<float>(surface, counting_geometry(calls, k_origin_hint), 0.F);
    const uint32_t child = surface.add(Forma::Element {}.with_bounds({ .min = { 0.6F, 0.6F }, .max = { 0.9F, 0.9F } }));
    surface.layer().relate(mapped.element.id, child);

    int presses = 0;
    surface.ctx().on_press(mapped.element.id, IO::MouseButtons::Left, [&](uint32_t, glm::vec2) { ++presses; });
    int sampled = 0;
    Forma::bridge().bind({ surface.layer_ptr(), mapped.element.id }, std::function<float()>([&] {
        ++sampled;
        return 1.F;
    }));
    frames.tick();
    ASSERT_EQ(sampled, 1);

    Forma::destroy(surface, mapped.element.id);

    EXPECT_EQ(surface.layer().get(mapped.element.id), nullptr);
    EXPECT_EQ(surface.layer().get(child), nullptr);

    window->move_to_ndc({ 0.F, 0.F });
    window->press(IO::MouseButtons::Left);
    window->release(IO::MouseButtons::Left);
    EXPECT_EQ(presses, 0);

    frames.tick(3);
    EXPECT_EQ(sampled, 1);
}

// =============================================================================
// create
// =============================================================================

TEST_F(FormaApiTest, CreateWiresTheFormOnceAfterSyncingItsBoundsToTheLayer)
{
    auto surface = make_surface("api-create");
    auto calls = std::make_shared<int>(0);
    int wired = 0;
    uint32_t wired_id = 0;
    Forma::Geometry::Form<float> form { counting_geometry(calls, k_origin_hint) };
    form.wire = [&](Forma::Context&, uint32_t id, std::shared_ptr<Forma::MappedState<float>>) {
        ++wired;
        wired_id = id;
    };

    auto mapped = Forma::create<float>(surface, std::move(form), 0.25F);

    EXPECT_EQ(wired, 1);
    EXPECT_EQ(wired_id, mapped.element.id);
    EXPECT_GE(*calls, 1);
    EXPECT_EQ(surface.layer().hit_test({ 0.F, 0.F }), mapped.element.id);
    EXPECT_FLOAT_EQ(mapped.state->value, 0.25F);
}

// =============================================================================
// Tend
// =============================================================================

TEST_F(FormaApiTest, TendPlacesTheFormAndKeepsTheSurfaceAndResult)
{
    auto surface = make_surface("api-tend");
    auto calls = std::make_shared<int>(0);
    Forma::Tend<float> tend;
    tend.form = Forma::Geometry::Form<float> { counting_geometry(calls, k_origin_hint) };
    tend.initial = 0.5F;

    tend.place(surface);

    ASSERT_TRUE(tend.result.has_value());
    ASSERT_TRUE(tend.surface.has_value());
    EXPECT_EQ(tend.surface->layer_ptr(), surface.layer_ptr());
    EXPECT_FLOAT_EQ(tend.result->state->value, 0.5F);
    EXPECT_EQ(surface.layer().hit_test({ 0.F, 0.F }), tend.result->element.id);
}

TEST_F(FormaApiTest, CreateOnABatchDrawsTheElementAsASliceAndWiresTheForm)
{
    auto surface = make_surface("api-create-batch");
    auto batch = Forma::create_buffer(surface.render_config());
    auto calls = std::make_shared<int>(0);
    int wired = 0;
    Forma::Geometry::Form<float> form { counting_geometry(calls, k_origin_hint) };
    form.wire = [&](Forma::Context&, uint32_t, std::shared_ptr<Forma::MappedState<float>>) { ++wired; };

    auto mapped = Forma::create<float>(surface, batch, std::move(form), 0.25F);

    EXPECT_EQ(wired, 1);
    EXPECT_GE(*calls, 1);
    EXPECT_EQ(mapped.element.buffer.buffer(), batch);
    EXPECT_NE(mapped.element.buffer.slice(), 0U);
    EXPECT_EQ(batch->slice_count(), 1U);
    EXPECT_EQ(surface.layer().hit_test({ 0.F, 0.F }), mapped.element.id);
}

TEST_F(FormaApiTest, FormsCreatedOnOneBatchShareItsBuffer)
{
    auto surface = make_surface("api-batch-shared");
    auto batch = Forma::create_buffer(surface.render_config());
    Forma::Geometry::Form<float> first { counting_geometry(std::make_shared<int>(0)) };
    Forma::Geometry::Form<float> second { counting_geometry(std::make_shared<int>(0)) };
    second.topology = PrimitiveTopology::TRIANGLE_LIST;

    auto a = Forma::create<float>(surface, batch, std::move(first), 0.F);
    auto b = Forma::create<float>(surface, batch, std::move(second), 0.F);

    EXPECT_EQ(a.element.buffer.buffer(), b.element.buffer.buffer());
    EXPECT_NE(a.element.buffer.slice(), b.element.buffer.slice());
    EXPECT_EQ(batch->slice_count(), 2U);
}

TEST_F(FormaApiTest, TendWithABatchPlacesItsElementAsASliceOfIt)
{
    auto surface = make_surface("api-tend-batch");
    auto batch = Forma::create_buffer(surface.render_config());
    Forma::Tend<float> tend;
    tend.form = Forma::Geometry::Form<float> { counting_geometry(std::make_shared<int>(0), k_origin_hint) };
    tend.initial = 0.5F;
    tend.batch = batch;

    tend.place(surface);

    ASSERT_TRUE(tend.result.has_value());
    EXPECT_EQ(tend.result->element.buffer.buffer(), batch);
    EXPECT_NE(tend.result->element.buffer.slice(), 0U);
    EXPECT_EQ(surface.layer().hit_test({ 0.F, 0.F }), tend.result->element.id);
}

TEST_F(FormaApiTest, ScrollableOnABatchScissorsTheBatchOnceForEveryTrackedSlice)
{
    const AABB2D viewport { .min = { -0.5F, -0.5F }, .max = { 0.5F, 0.5F } };
    auto surface = make_surface("api-scroll-batch");
    auto batch = Forma::create_buffer(surface.render_config());
    auto panel = Forma::make_scrollable(
        Forma::FormaTarget::slice_of(batch, PrimitiveTopology::TRIANGLE_STRIP), surface, viewport);
    ASSERT_NE(batch->get_render_processor(), nullptr);
    EXPECT_EQ(batch->get_render_processor()->get_scissor(), Kinesis::Scissor::from(viewport));

    Forma::Geometry::Form<float> form { counting_geometry(std::make_shared<int>(0), k_origin_hint) };
    auto child = Forma::create<float>(surface, batch, std::move(form), 0.F);
    Forma::internal::atelier().track<float>(panel, surface.layer(), child, viewport);

    EXPECT_EQ(child.element.buffer.buffer(), batch);
    EXPECT_EQ(batch->get_render_processor()->get_scissor(), Kinesis::Scissor::from(viewport));
    EXPECT_EQ(batch->slice_count(), 2U);
    const auto related = surface.layer().related_ids(panel.viewport_id);
    EXPECT_NE(std::ranges::find(related, child.element.id), related.end());
}

TEST_F(FormaApiTest, ScrollableStillScissorsATrackedElementThatHasABufferOfItsOwn)
{
    const AABB2D viewport { .min = { -0.5F, -0.5F }, .max = { 0.5F, 0.5F } };
    auto surface = make_surface("api-scroll-own-buffer");
    auto batch = Forma::create_buffer(surface.render_config());
    auto panel = Forma::make_scrollable(
        Forma::FormaTarget::slice_of(batch, PrimitiveTopology::TRIANGLE_STRIP), surface, viewport);

    auto own = Forma::create_buffer(surface.render_config());
    Forma::Geometry::Form<float> form { counting_geometry(std::make_shared<int>(0), k_origin_hint) };
    auto child = Forma::create<float>(surface, own, std::move(form), 0.F);
    Forma::internal::atelier().track<float>(panel, surface.layer(), child, viewport);

    EXPECT_EQ(child.element.buffer.buffer(), own);
    EXPECT_NE(child.element.buffer.buffer(), batch);
    ASSERT_NE(own->get_render_processor(), nullptr);
    EXPECT_EQ(own->get_render_processor()->get_scissor(), Kinesis::Scissor::from(viewport));
}

TEST_F(FormaApiTest, ScrollableMovesATrackedElementOutOfTheSurfaceBufferIntoItsOwn)
{
    const AABB2D viewport { .min = { -0.5F, -0.5F }, .max = { 0.5F, 0.5F } };
    auto surface = make_surface("api-scroll-move");
    auto panel = Forma::Scrollable {}.place(surface, viewport);
    auto panel_buffer = panel.buf.buffer();

    Forma::Geometry::Form<float> form { counting_geometry(std::make_shared<int>(0), k_origin_hint) };
    auto child = Forma::create<float>(surface, std::move(form), 0.F);
    const auto shared = surface.layer().batch();
    ASSERT_NE(shared, nullptr);
    ASSERT_EQ(child.element.buffer.buffer(), shared);

    Forma::internal::atelier().track<float>(panel, surface.layer(), child, viewport);

    EXPECT_EQ(child.element.buffer.buffer(), panel_buffer);
    EXPECT_NE(panel_buffer, shared);
    EXPECT_EQ(panel_buffer->slice_count(), 2U);
    EXPECT_TRUE(shared->needs_removal());
    ASSERT_NE(panel_buffer->get_render_processor(), nullptr);
    EXPECT_EQ(panel_buffer->get_render_processor()->get_scissor(), Kinesis::Scissor::from(viewport));
    EXPECT_GT(panel_buffer->get_render_config().draw_priority, 0);
}

TEST_F(FormaApiTest, ATrackedElementMovesAlongsideTheOthersLeftOnTheSurfaceBuffer)
{
    const AABB2D viewport { .min = { -0.5F, -0.5F }, .max = { 0.5F, 0.5F } };
    auto surface = make_surface("api-scroll-move-one");
    auto panel = Forma::Scrollable {}.place(surface, viewport);

    Forma::Geometry::Form<float> tracked_form { counting_geometry(std::make_shared<int>(0), k_origin_hint) };
    Forma::Geometry::Form<float> other_form { counting_geometry(std::make_shared<int>(0), k_origin_hint) };
    auto tracked = Forma::create<float>(surface, std::move(tracked_form), 0.F);
    auto other = Forma::create<float>(surface, std::move(other_form), 0.F);
    const auto shared = surface.layer().batch();

    Forma::internal::atelier().track<float>(panel, surface.layer(), tracked, viewport);

    EXPECT_EQ(other.element.buffer.buffer(), shared);
    EXPECT_FALSE(shared->needs_removal());
    EXPECT_EQ(shared->slice_count(), 1U);
    EXPECT_EQ(tracked.element.buffer.buffer(), panel.buf.buffer());
    ASSERT_NE(shared->get_render_processor(), nullptr);
    EXPECT_FALSE(shared->get_render_processor()->get_scissor().has_value());
}

TEST_F(FormaApiTest, FormsCreatedWithoutABufferShareTheSurfaceBuffer)
{
    auto surface = make_surface("api-default-batch");

    auto a = Forma::create<float>(
        surface, Forma::Geometry::Form<float> { counting_geometry(std::make_shared<int>(0)) }, 0.F);
    auto b = Forma::create<float>(
        surface, Forma::Geometry::Form<float> { counting_geometry(std::make_shared<int>(0)) }, 0.F);

    ASSERT_NE(surface.layer().batch(), nullptr);
    EXPECT_EQ(a.element.buffer.buffer(), surface.layer().batch());
    EXPECT_EQ(b.element.buffer.buffer(), surface.layer().batch());
    EXPECT_NE(a.element.buffer.slice(), b.element.buffer.slice());
    EXPECT_EQ(surface.layer().batch()->slice_count(), 2U);
}

TEST_F(FormaApiTest, SliceGivesATargetInTheSurfaceBufferWithTheRequestedTopology)
{
    auto surface = make_surface("api-slice");

    const auto first = Forma::slice(surface);
    const auto second = Forma::slice(surface, PrimitiveTopology::POINT_LIST);

    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(first.buffer(), surface.layer().batch());
    EXPECT_EQ(second.buffer(), surface.layer().batch());
    EXPECT_NE(first.slice(), second.slice());
    const auto read = second.buffer()->read_slice(second.slice());
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(read->topology, PrimitiveTopology::POINT_LIST);
}

TEST_F(FormaApiTest, TendWithoutABufferPlacesItsElementOnTheSurfaceBuffer)
{
    auto surface = make_surface("api-tend-default-batch");
    Forma::Tend<float> tend;
    tend.form = Forma::Geometry::Form<float> { counting_geometry(std::make_shared<int>(0)) };
    tend.place(surface);

    EXPECT_EQ(tend.result->element.buffer.buffer(), surface.layer().batch());
    EXPECT_NE(tend.result->element.buffer.slice(), 0U);
}

TEST_F(FormaApiTest, DestroyingTheLastElementRetiresTheSurfaceBufferAndTheNextOneGetsANewBuffer)
{
    auto surface = make_surface("api-batch-retire");
    auto first = Forma::create<float>(
        surface, Forma::Geometry::Form<float> { counting_geometry(std::make_shared<int>(0)) }, 0.F);
    const auto old_batch = surface.layer().batch();

    Forma::destroy(surface, first.element.id);

    EXPECT_TRUE(old_batch->needs_removal());

    auto second = Forma::create<float>(
        surface, Forma::Geometry::Form<float> { counting_geometry(std::make_shared<int>(0)) }, 0.F);

    EXPECT_NE(second.element.buffer.buffer(), old_batch);
    EXPECT_EQ(second.element.buffer.buffer(), surface.layer().batch());
}

TEST_F(FormaApiTest, RaisingAnElementDrawsItsSliceLastInTheSurfaceBuffer)
{
    auto surface = make_surface("api-raise-order");
    auto first = Forma::create<float>(
        surface, Forma::Geometry::Form<float> { counting_geometry(std::make_shared<int>(0)) }, 0.F);
    std::vector<Forma::Mapped<float>> others;
    for (int i = 0; i < 2; ++i) {
        others.push_back(Forma::create<float>(
            surface, Forma::Geometry::Form<float> { counting_geometry(std::make_shared<int>(0)) }, 0.F));
    }
    const auto batch = surface.layer().batch();
    ASSERT_NE(batch, nullptr);
    ASSERT_EQ(batch->slice_count(), 3U);

    ASSERT_TRUE(surface.layer().bring_to_front(first.element.id));
    batch->process_default();

    ASSERT_NE(batch->get_render_processor(), nullptr);
    const auto& runs = batch->get_render_processor()->get_runs();
    ASSERT_EQ(runs.size(), 3U);
    EXPECT_EQ(runs[0].vertex_offset, 4U);
    EXPECT_EQ(runs[1].vertex_offset, 8U);
    EXPECT_EQ(runs[2].vertex_offset, 0U);
}

TEST_F(FormaApiTest, ARaisedBufferElementDrawsAboveEveryScrollBatchMadeBeforeItAndBelowThoseAfter)
{
    const AABB2D viewport { .min = { -0.5F, -0.5F }, .max = { 0.5F, 0.5F } };
    auto surface = make_surface("api-raise-priority");
    auto first_panel = Forma::Scrollable {}.place(surface, viewport);

    auto own = Forma::create_buffer(surface.render_config());
    auto element = Forma::create_element<float>(
        surface, Forma::FormaTarget(own), counting_geometry(std::make_shared<int>(0), k_origin_hint), 0.F);
    ASSERT_TRUE(surface.layer().bring_to_front(element.element.id));

    auto second_panel = Forma::Scrollable {}.place(surface, viewport);

    const auto priority_of = [](const std::shared_ptr<Buffers::FormaBuffer>& buffer) {
        return buffer->get_render_config().draw_priority;
    };
    EXPECT_GT(priority_of(own), priority_of(first_panel.buf.buffer()));
    EXPECT_GT(priority_of(second_panel.buf.buffer()), priority_of(own));
}

TEST_F(FormaApiTest, ClearingALayerRetiresTheSurfaceBuffer)
{
    auto surface = make_surface("api-batch-clear");
    auto element = Forma::create<float>(
        surface, Forma::Geometry::Form<float> { counting_geometry(std::make_shared<int>(0)) }, 0.F);
    const auto old_batch = surface.layer().batch();
    ASSERT_NE(old_batch, nullptr);

    surface.layer().clear();

    EXPECT_TRUE(old_batch->needs_removal());
    EXPECT_EQ(surface.layer().batch(), nullptr);
    EXPECT_TRUE(surface.layer().empty());
}

TEST_F(FormaApiTest, TendBridgeReturnsTheHandleOfItsElement)
{
    auto surface = make_surface("api-tend-bridge");
    Forma::Tend<float> tend;
    tend.form = Forma::Geometry::Form<float> { counting_geometry(std::make_shared<int>(0)) };
    tend.initial = 0.75F;
    tend.place(surface);
    std::vector<float> received;

    auto handle = tend.bridge();
    handle.write(std::function<void(std::span<const float>)>([&](std::span<const float> values) {
        received.assign(values.begin(), values.end());
    }));
    frames.tick();

    EXPECT_EQ(handle.id(), tend.result->element.id);
    EXPECT_EQ(received, std::vector<float> { 0.75F });
}

TEST_F(FormaApiTest, TendTrackReflowsItsGeometryOnScroll)
{
    auto surface = make_surface("api-tend-track");
    auto panel_buffer = std::make_shared<Buffers::FormaBuffer>(4096, PrimitiveTopology::TRIANGLE_STRIP);
    auto panel = Forma::make_scrollable(std::move(panel_buffer), surface, k_origin_hint, glm::vec3(0.1F), 0.1F);
    const AABB2D content { .min = { -0.4F, -1.5F }, .max = { 0.4F, 1.5F } };
    auto calls = std::make_shared<int>(0);
    Forma::Tend<float> tend;
    tend.form = Forma::Geometry::Form<float> { counting_geometry(calls, content) };
    tend.place(surface);
    const int placed = *calls;

    EXPECT_EQ(&tend.track(panel, surface.layer(), content), &tend);
    panel.scroll_by(surface.layer(), { 0.F, 0.3F });

    EXPECT_EQ(*calls, placed + 1);
}

// =============================================================================
// Text field
// =============================================================================

TEST_F(FormaApiTest, CreateTextFieldRegistersAnElementOnTheSurface)
{
    auto surface = make_surface("api-text");

    auto field = Forma::create_text_field(surface, k_origin_hint, nullptr, "hello");

    EXPECT_NE(field.element_id, 0U);
    EXPECT_NE(surface.layer().get(field.element_id), nullptr);
    EXPECT_NE(field.buf, nullptr);
    EXPECT_EQ(field.state->value.text, "hello");
}

TEST_F(FormaApiTest, AScrollableTextFieldNeedsNoBufferAndDrawsInThePanelsOwn)
{
    const AABB2D viewport { .min = { -0.5F, -0.5F }, .max = { 0.5F, 0.5F } };
    auto surface = make_surface("api-text-panel");
    auto panel = Forma::Scrollable {}.place(surface, viewport);

    auto field = Forma::TextField {}.scrollable(surface, panel, nullptr, "hello");

    EXPECT_NE(field.element_id, 0U);
    EXPECT_EQ(field.buf.buffer(), panel.buf.buffer());
    EXPECT_NE(field.buf.slice(), panel.buf.slice());
    EXPECT_EQ(field.state->value.text, "hello");
    EXPECT_FALSE(surface.layer().batch());
    ASSERT_NE(panel.buf->get_render_processor(), nullptr);
    EXPECT_EQ(panel.buf->get_render_processor()->get_scissor(), Kinesis::Scissor::from(viewport));
}

TEST_F(FormaApiTest, AScrollableTextFieldOnAnUnplacedScrollerDoesNothing)
{
    auto surface = make_surface("api-text-unplaced");
    Forma::Scrollable unplaced;

    auto field = Forma::TextField {}.scrollable(surface, unplaced, nullptr, "hello");

    EXPECT_EQ(field.element_id, 0U);
    EXPECT_TRUE(surface.layer().empty());
}

TEST_F(FormaApiTest, DestroyingAScrollableTextFieldTearsDownTheViewportToo)
{
    auto surface = make_surface("api-text-scroll");

    auto field = Forma::create_text_field(surface, k_origin_hint, nullptr, "hello", true);
    ASSERT_GT(surface.layer().size(), 1U);

    const auto& elements = surface.layer().elements();
    const auto viewport = std::ranges::find_if(elements, [](const Forma::Element& el) {
        return el.name == "scrollable_viewport";
    });
    ASSERT_NE(viewport, elements.end());
    EXPECT_EQ(viewport->buffer.buffer(), field.buf.buffer());

    Forma::destroy(surface, field.element_id);

    EXPECT_TRUE(surface.layer().empty());
}

}
