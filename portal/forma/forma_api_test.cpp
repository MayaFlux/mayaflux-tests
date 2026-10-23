#include "../../support/forma_rig.hpp"

#include "MayaFlux/Portal/Forma/Inspect/Inspector.hpp"
#include "MayaFlux/Portal/Forma/Primitives/Geometry.hpp"
#include "MayaFlux/Portal/Forma/Primitives/Scrollable.hpp"

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
    auto buffer = Forma::create_buffer(window, PrimitiveTopology::TRIANGLE_STRIP, "texSampler");

    ASSERT_NE(buffer, nullptr);
    EXPECT_EQ(buffer->get_size_bytes(), Forma::internal::k_capacity_bytes);
    EXPECT_TRUE(buffer->get_render_processor()->get_config().bindings.contains("texSampler"));
}

TEST_F(FormaApiTest, CreateBufferWithAdditionalTexturesDeclaresTheArrayBinding)
{
    auto buffer = Forma::create_buffer(window, PrimitiveTopology::TRIANGLE_LIST,
        std::vector<std::pair<std::string, std::shared_ptr<Core::VKImage>>> { { "a", nullptr }, { "b", nullptr } });

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

TEST_F(FormaApiTest, DestroyingAScrollableTextFieldTearsDownTheViewportToo)
{
    auto surface = make_surface("api-text-scroll");

    auto field = Forma::create_text_field(surface, k_origin_hint, nullptr, "hello", true);
    ASSERT_GT(surface.layer().size(), 1U);

    Forma::destroy(surface, field.element_id);

    EXPECT_TRUE(surface.layer().empty());
}

}
