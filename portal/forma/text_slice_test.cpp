#include "../../support/forma_rig.hpp"
#include "../../support/test_metadata.hpp"

#include "MayaFlux/Portal/Graphics/ImageTarget.hpp"
#include "MayaFlux/Portal/Graphics/TextureLoom.hpp"
#include "MayaFlux/Portal/Forma/Internal/TextRow.hpp"
#include "MayaFlux/Portal/Forma/Internal/TextSlice.hpp"
#include "MayaFlux/Portal/Forma/Primitives/TextField.hpp"
#include "MayaFlux/Portal/Text/FontFace.hpp"
#include "MayaFlux/Portal/Text/GlyphAtlas.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::system_font, ::MayaFlux::Test::Meta::Capability::vulkan>() }))

namespace MayaFlux::Test {

namespace Forma = Portal::Forma;
using Kinesis::AABB2D;
using Portal::Graphics::PrimitiveTopology;

namespace {

    const AABB2D k_region { .min = { -0.5F, -0.2F }, .max = { 0.5F, 0.2F } };

    Portal::Text::PressParams params_for(Portal::Text::GlyphAtlas* atlas = nullptr)
    {
        Portal::Text::PressParams params;
        params.atlas = atlas;
        params.render_bounds = { 400, 80 };
        return params;
    }

    uint32_t vertex_count(const std::shared_ptr<Buffers::FormaBuffer>& buffer)
    {
        buffer->process_default();
        const auto layout = buffer->get_vertex_layout();
        return layout ? layout->vertex_count : 0U;
    }

}

class FormaTextSliceTest : public FormaRigTest {
protected:
    void SetUp() override
    {
        FormaRigTest::SetUp();
        if (IsSkipped())
            return;
        surface.emplace(make_surface("text-slice"));
    }

    void TearDown() override
    {
        surface.reset();
        FormaRigTest::TearDown();
    }

    std::shared_ptr<Buffers::FormaBuffer> text_buffer()
    {
        return Forma::create_buffer(surface->render_config({ .additional_textures = { { "text", nullptr } } }));
    }

    std::optional<Forma::Surface> surface;
};

TEST_F(FormaTextSliceTest, WithTextDrawsOneQuadPerGlyphAndBindsTheAtlasImage)
{
    auto buffer = text_buffer();
    Forma::Element el;

    el.with_buffer(Forma::FormaTarget::slice_of(buffer, PrimitiveTopology::TRIANGLE_LIST))
        .with_text("ab", params_for(), k_region);

    ASSERT_NE(el.text_slice, nullptr);
    EXPECT_EQ(vertex_count(buffer), 12U);
    auto* atlas = Portal::Text::TypeFaceFoundry::instance().get_default_glyph_atlas();
    ASSERT_NE(atlas, nullptr);
    auto& atlas_image = Forma::internal::atelier().atlas_image(*atlas);
    EXPECT_NE(atlas_image.image(), nullptr);
    EXPECT_EQ(atlas_image.generation(), 1U);
}

TEST_F(FormaTextSliceTest, ABatchUsesTheBatchShaderAndDeclaresItsTextSlots)
{
    auto batch = Forma::internal::atelier().create_batch(*surface);

    const auto config = batch->get_render_config();
    EXPECT_EQ(config.fragment_shader, "forma_plot.frag.spv");
    ASSERT_EQ(config.additional_textures.size(), Forma::internal::k_text_slots);
    EXPECT_EQ(config.additional_textures.front().name, "text");
    for (const auto& slot : config.additional_textures)
        EXPECT_TRUE(slot.name.starts_with("text"));
}

TEST_F(FormaTextSliceTest, EveryAtlasKeepsOneSlotAndTheDefaultHoldsTheFirst)
{
    auto* fallback = Portal::Text::TypeFaceFoundry::instance().get_default_glyph_atlas();
    ASSERT_NE(fallback, nullptr);
    auto& atelier = Forma::internal::atelier();
    Portal::Text::FontFace face;
    const auto& path = system_font_path();
    ASSERT_TRUE(path.has_value());
    ASSERT_TRUE(face.load(*path));
    Portal::Text::GlyphAtlas first(face, 24, 128);
    Portal::Text::GlyphAtlas second(face, 32, 128);

    EXPECT_EQ(atelier.atlas_slot(*fallback), 0U);
    const uint32_t first_slot = atelier.atlas_slot(first);
    const uint32_t second_slot = atelier.atlas_slot(second);

    EXPECT_NE(first_slot, 0U);
    EXPECT_NE(second_slot, 0U);
    EXPECT_NE(first_slot, second_slot);
    EXPECT_EQ(atelier.atlas_slot(first), first_slot);
}

TEST_F(FormaTextSliceTest, ABatchBindsTheDefaultAtlasImageAndTextKeepsIt)
{
    auto* atlas = Portal::Text::TypeFaceFoundry::instance().get_default_glyph_atlas();
    ASSERT_NE(atlas, nullptr);

    auto batch = Forma::internal::atelier().create_batch(*surface);
    auto& atlas_image = Forma::internal::atelier().atlas_image(*atlas);
    const auto image = atlas_image.image();
    ASSERT_NE(image, nullptr);
    EXPECT_EQ(batch->get_render_config().additional_textures.front().image, image);

    Forma::Element el;
    el.with_buffer(Forma::FormaTarget::slice_of(batch, PrimitiveTopology::TRIANGLE_LIST))
        .with_text("ab", params_for(), k_region);

    EXPECT_EQ(atlas_image.image(), image);
    EXPECT_EQ(atlas_image.generation(), 1U);
}

TEST_F(FormaTextSliceTest, CreateBatchMakesAnUnregisteredBufferEachTime)
{
    auto first = Forma::internal::atelier().create_batch(*surface);
    auto second = Forma::internal::atelier().create_batch(*surface);

    EXPECT_NE(first, second);
    EXPECT_EQ(surface->layer().batch(), nullptr);
}

TEST_F(FormaTextSliceTest, TheSurfaceBatchIsMadeOnceAndReplacedAfterItsLastSliceIsRetired)
{
    auto& atelier = Forma::internal::atelier();
    EXPECT_EQ(surface->layer().batch(), nullptr);

    auto first = atelier.surface_batch(*surface);
    EXPECT_EQ(atelier.surface_batch(*surface), first);
    EXPECT_EQ(surface->layer().batch(), first);

    const auto slice = atelier.batch_slice(*surface, PrimitiveTopology::TRIANGLE_LIST);
    EXPECT_EQ(slice.buffer(), first);
    slice.retire();
    EXPECT_TRUE(first->needs_removal());

    auto second = atelier.surface_batch(*surface);
    EXPECT_NE(second, first);
    EXPECT_FALSE(second->needs_removal());
}

TEST_F(FormaTextSliceTest, AnImageBatchDrawsIntoItsTargetAndNotTheWindow)
{
    auto& atelier = Forma::internal::atelier();
    const auto target = Portal::Graphics::get_texture_manager().create_image_target({ .width = 64, .height = 64 });
    ASSERT_NE(target, nullptr);

    const auto batch = atelier.surface_batch(*surface, target);

    ASSERT_NE(batch, nullptr);
    const auto config = batch->get_render_config();
    EXPECT_EQ(config.target_window, nullptr);
    ASSERT_EQ(config.target_images.size(), 1U);
    EXPECT_EQ(config.target_images.front(), target);
}

TEST_F(FormaTextSliceTest, AnImageBatchIsHeldByTheLayerSeparatelyFromTheWindowBatch)
{
    auto& atelier = Forma::internal::atelier();
    const auto target = Portal::Graphics::get_texture_manager().create_image_target({ .width = 64, .height = 64 });
    ASSERT_NE(target, nullptr);

    const auto window_batch = atelier.surface_batch(*surface);
    const auto image_batch = atelier.surface_batch(*surface, target);

    EXPECT_NE(window_batch, image_batch);
    EXPECT_EQ(surface->layer().batch(), window_batch);
    EXPECT_EQ(surface->layer().batch(target), image_batch);
    EXPECT_EQ(atelier.surface_batch(*surface, target), image_batch);
    EXPECT_EQ(atelier.surface_batch(*surface), window_batch);
}

TEST_F(FormaTextSliceTest, EachTargetGetsItsOwnBatch)
{
    auto& atelier = Forma::internal::atelier();
    auto& loom = Portal::Graphics::get_texture_manager();
    const auto first = loom.create_image_target({ .width = 64, .height = 64 });
    const auto second = loom.create_image_target({ .width = 64, .height = 64 });
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);

    const auto first_batch = atelier.surface_batch(*surface, first);
    const auto second_batch = atelier.surface_batch(*surface, second);

    EXPECT_NE(first_batch, second_batch);
    EXPECT_EQ(surface->layer().batch(first), first_batch);
    EXPECT_EQ(surface->layer().batch(second), second_batch);
}

TEST_F(FormaTextSliceTest, AnImageBatchIsReplacedAfterItsLastSliceIsRetired)
{
    auto& atelier = Forma::internal::atelier();
    const auto target = Portal::Graphics::get_texture_manager().create_image_target({ .width = 64, .height = 64 });
    ASSERT_NE(target, nullptr);

    const auto first = atelier.surface_batch(*surface, target);
    const auto slice = atelier.batch_slice(*surface, target, PrimitiveTopology::TRIANGLE_LIST);
    EXPECT_EQ(slice.buffer(), first);
    slice.retire();
    ASSERT_TRUE(first->needs_removal());

    const auto second = atelier.surface_batch(*surface, target);
    EXPECT_NE(second, first);
    EXPECT_FALSE(second->needs_removal());
    EXPECT_EQ(surface->layer().batch(target), second);
}

TEST_F(FormaTextSliceTest, ALayerHasNoBatchForATargetItNeverDrewInto)
{
    const auto target = Portal::Graphics::get_texture_manager().create_image_target({ .width = 8, .height = 8 });
    ASSERT_NE(target, nullptr);

    EXPECT_EQ(surface->layer().batch(target), nullptr);
    EXPECT_EQ(surface->layer().batch(nullptr), nullptr);
}

TEST_F(FormaTextSliceTest, ClearingTheLayerDropsItsImageBatches)
{
    auto& atelier = Forma::internal::atelier();
    const auto target = Portal::Graphics::get_texture_manager().create_image_target({ .width = 64, .height = 64 });
    ASSERT_NE(target, nullptr);
    (void)atelier.surface_batch(*surface, target);
    ASSERT_NE(surface->layer().batch(target), nullptr);

    surface->layer().clear();

    EXPECT_EQ(surface->layer().batch(target), nullptr);
}

TEST_F(FormaTextSliceTest, ANullTargetIsRefusedForAnImageBatch)
{
    EXPECT_THROW((void)Forma::internal::atelier().surface_batch(*surface, nullptr), std::invalid_argument);
}

TEST_F(FormaTextSliceTest, AnImageConfigKeepsTheRestOfTheBaseAndDropsTheWindow)
{
    const auto target = Portal::Graphics::get_texture_manager().create_image_target({ .width = 8, .height = 8 });
    ASSERT_NE(target, nullptr);

    const auto config = surface->image_config(target, { .fragment_shader = "x.frag.spv" });

    EXPECT_EQ(config.target_window, nullptr);
    ASSERT_EQ(config.target_images.size(), 1U);
    EXPECT_EQ(config.target_images.front(), target);
    EXPECT_EQ(config.fragment_shader, "x.frag.spv");
}

TEST_F(FormaTextSliceTest, ScrollBatchesDrawAboveTheSurfaceBatchInTheOrderTheyWereMade)
{
    auto& atelier = Forma::internal::atelier();

    auto shared = atelier.surface_batch(*surface);
    auto first_scroll = atelier.create_scroll_batch(*surface);
    auto second_scroll = atelier.create_scroll_batch(*surface);

    EXPECT_EQ(shared->get_render_config().draw_priority, 0);
    EXPECT_GT(first_scroll->get_render_config().draw_priority, shared->get_render_config().draw_priority);
    EXPECT_GT(second_scroll->get_render_config().draw_priority, first_scroll->get_render_config().draw_priority);
    EXPECT_EQ(surface->layer().batch(), shared);
}

TEST_F(FormaTextSliceTest, SetTextRedrawsTheGlyphsForTheNewText)
{
    auto buffer = text_buffer();
    Forma::Element el;
    el.with_buffer(Forma::FormaTarget::slice_of(buffer, PrimitiveTopology::TRIANGLE_LIST))
        .with_text("ab", params_for(), k_region);

    el.set_text("abcd", params_for());

    EXPECT_EQ(vertex_count(buffer), 24U);
}

TEST_F(FormaTextSliceTest, ABackgroundWithAlphaAddsOneQuadBeneathTheGlyphs)
{
    auto buffer = text_buffer();
    auto params = params_for();
    params.background = { 0.2F, 0.2F, 0.2F, 1.F };
    Forma::Element el;

    el.with_buffer(Forma::FormaTarget::slice_of(buffer, PrimitiveTopology::TRIANGLE_LIST))
        .with_text("ab", params, k_region);

    EXPECT_EQ(vertex_count(buffer), 18U);
}

TEST_F(FormaTextSliceTest, RetargetMovesTheTextAndKeepsItsGlyphs)
{
    auto buffer = text_buffer();
    Forma::Element el;
    el.with_buffer(Forma::FormaTarget::slice_of(buffer, PrimitiveTopology::TRIANGLE_LIST))
        .with_text("ab", params_for(), k_region);
    const AABB2D moved { .min = { 0.1F, 0.1F }, .max = { 0.9F, 0.5F } };

    el.retarget(moved);

    EXPECT_EQ(el.text_slice->bounds().min, moved.min);
    EXPECT_EQ(el.text_slice->bounds().max, moved.max);
    EXPECT_EQ(vertex_count(buffer), 12U);
}

TEST_F(FormaTextSliceTest, ARegionShorterThanALineFitsOneLineInsideIt)
{
    auto buffer = text_buffer();
    auto params = params_for();
    params.render_bounds = { 400, 4 };
    const AABB2D row { .min = { -1.F, 0.F }, .max = { 1.F, 0.1F } };
    Forma::Element el;
    el.with_buffer(Forma::FormaTarget::slice_of(buffer, PrimitiveTopology::TRIANGLE_LIST))
        .with_text("ab", params, row);

    const auto map = el.text_slice->map();
    const auto extent = el.text_slice->run().extent();

    EXPECT_FLOAT_EQ(map(glm::vec2(0.F)).y, row.max.y);
    EXPECT_NEAR(map(extent).y, row.min.y, 1e-5F);
}

TEST_F(FormaTextSliceTest, TextOnSlicesOfOneBufferDrawsTogether)
{
    auto buffer = text_buffer();
    Forma::Element first;
    Forma::Element second;

    first.with_buffer(Forma::FormaTarget::slice_of(buffer, PrimitiveTopology::TRIANGLE_LIST))
        .with_text("ab", params_for(), k_region);
    second.with_buffer(Forma::FormaTarget::slice_of(buffer, PrimitiveTopology::TRIANGLE_LIST))
        .with_text("cde", params_for(), k_region);

    EXPECT_EQ(buffer->slice_count(), 2U);
    EXPECT_EQ(vertex_count(buffer), 30U);
}

TEST_F(FormaTextSliceTest, AGrownAtlasWritesEveryLiveSliceAgainstTheNewImage)
{
    auto* face = Portal::Text::TypeFaceFoundry::instance().get_default_face();
    ASSERT_NE(face, nullptr);
    Portal::Text::GlyphAtlas small(*face, 16, 32);

    auto buffer = text_buffer();
    Forma::Element first;
    Forma::Element second;
    first.with_buffer(Forma::FormaTarget::slice_of(buffer, PrimitiveTopology::TRIANGLE_LIST))
        .with_text("a", params_for(&small), k_region);
    second.with_buffer(Forma::FormaTarget::slice_of(buffer, PrimitiveTopology::TRIANGLE_LIST))
        .with_text("b", params_for(&small), k_region);
    const uint32_t before = small.atlas_size();

    first.text_slice->set("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789");

    ASSERT_GT(small.atlas_size(), before);
    EXPECT_FALSE(second.text_slice->run().stale());
    EXPECT_EQ(Forma::internal::atelier().atlas_image(small).generation(), 2U);
    const auto* metrics = small.get_or_rasterize(static_cast<FT_ULong>('b'));
    ASSERT_NE(metrics, nullptr);
    ASSERT_EQ(second.text_slice->run().layout().quads.size(), 1U);
    EXPECT_FLOAT_EQ(second.text_slice->run().layout().quads.front().uv_x1, metrics->uv_x1);
}

TEST_F(FormaTextSliceTest, ASliceWhoseTargetWasRemovedIsNotWrittenAgain)
{
    auto buffer = text_buffer();
    Forma::Element el;
    auto target = Forma::FormaTarget::slice_of(buffer, PrimitiveTopology::TRIANGLE_LIST);
    el.with_buffer(target).with_text("ab", params_for(), k_region);
    target.retire();

    el.set_text("abcd", params_for());

    EXPECT_EQ(buffer->slice_count(), 0U);
}

TEST_F(FormaTextSliceTest, TextRowsWithoutABatchAreSlicesOfTheSurfaceBuffer)
{
    const AABB2D row { .min = { -1.F, 0.F }, .max = { 1.F, 0.2F } };

    auto first = Forma::internal::add_text_row(*surface, "one", row, { 200, 24 });
    auto second = Forma::internal::add_text_row(*surface, "two", row, { 200, 24 });

    ASSERT_NE(surface->layer().batch(), nullptr);
    EXPECT_EQ(first.second->buffer.buffer(), surface->layer().batch());
    EXPECT_EQ(second.second->buffer.buffer(), surface->layer().batch());
    EXPECT_NE(first.second->buffer.slice(), second.second->buffer.slice());
    EXPECT_EQ(surface->layer().batch()->slice_count(), 2U);
}

TEST_F(FormaTextSliceTest, TextRowsOfABatchAreSlicesOfOneBuffer)
{
    const AABB2D row { .min = { -1.F, 0.F }, .max = { 1.F, 0.2F } };
    auto batch = text_buffer();

    auto first = Forma::internal::add_text_row(*surface, "one", row, { 200, 24 }, batch);
    auto second = Forma::internal::add_text_row(*surface, "two", row, { 200, 24 }, batch);

    EXPECT_EQ(first.second->buffer.buffer(), batch);
    EXPECT_EQ(second.second->buffer.buffer(), batch);
    EXPECT_NE(first.second->buffer.slice(), second.second->buffer.slice());
    EXPECT_EQ(batch->slice_count(), 2U);
}

TEST_F(FormaTextSliceTest, ATextFieldDrawsItsCaretInTheSliceOfItsGlyphs)
{
    auto buffer = text_buffer();
    auto params = std::make_shared<Portal::Text::PressParams>(params_for());

    Forma::TextField field;
    field.place(Forma::FormaTarget(buffer), *surface, k_region, params, "hi");

    EXPECT_EQ(vertex_count(buffer), 18U);

    field.reposition({ .min = { -0.4F, -0.3F }, .max = { 0.6F, 0.1F } });

    EXPECT_EQ(vertex_count(buffer), 18U);
}

namespace {

    constexpr IO::Modifiers k_shortcut =
#ifdef MAYAFLUX_PLATFORM_MACOS
        IO::Modifiers::Super;
#else
        IO::Modifiers::Ctrl;
#endif

}

class FormaTextFieldSelectionDrawTest : public FormaTextSliceTest {
protected:
    Forma::TextField focused_field(
        const std::shared_ptr<Buffers::FormaBuffer>& buffer,
        const std::shared_ptr<Portal::Text::PressParams>& params,
        std::string text)
    {
        Forma::TextField field;
        field.place(Forma::FormaTarget(buffer), *surface, k_region, params, std::move(text));
        window->move_to_ndc({ 0.49F, 0.19F });
        window->press(IO::MouseButtons::Left);
        window->release(IO::MouseButtons::Left);
        return field;
    }

    std::shared_ptr<Portal::Text::PressParams> tall_params(uint32_t lines, uint32_t width = 400)
    {
        auto params = std::make_shared<Portal::Text::PressParams>(params_for());
        const auto* atlas = Portal::Text::TypeFaceFoundry::instance().get_default_glyph_atlas();
        params->render_bounds = { width, atlas->line_height() * lines };
        return params;
    }

    void select_all()
    {
        window->key_down(IO::Keys::A, k_shortcut);
        window->key_up(IO::Keys::A, k_shortcut);
    }

    void shift_left()
    {
        window->key_down(IO::Keys::Left, IO::Modifiers::Shift);
        window->key_up(IO::Keys::Left, IO::Modifiers::Shift);
    }

    void plain_left()
    {
        window->key_down(IO::Keys::Left);
        window->key_up(IO::Keys::Left);
    }
};

TEST_F(FormaTextFieldSelectionDrawTest, ASelectedFieldDrawsAnExtraQuadPerLine)
{
    auto buffer = text_buffer();
    auto field = focused_field(buffer, tall_params(1), "hello");
    const uint32_t plain = vertex_count(buffer);
    EXPECT_EQ(plain, 36U);

    select_all();
    EXPECT_EQ(vertex_count(buffer), plain + 6U);

    plain_left();
    EXPECT_EQ(vertex_count(buffer), plain);
}

TEST_F(FormaTextFieldSelectionDrawTest, ASelectionAcrossALineBreakDrawsOneQuadPerLine)
{
    auto buffer = text_buffer();
    auto field = focused_field(buffer, tall_params(3), "ab\ncd");
    const uint32_t plain = vertex_count(buffer);

    select_all();

    EXPECT_EQ(vertex_count(buffer), plain + 12U);
}

TEST_F(FormaTextFieldSelectionDrawTest, ASelectionAcrossAWrapDrawsOneQuadPerLine)
{
    auto buffer = text_buffer();
    auto field = focused_field(buffer, tall_params(8, 120), "abc abc abc abc abc abc");
    const uint32_t plain = vertex_count(buffer);

    select_all();

    const uint32_t extra = vertex_count(buffer) - plain;
    EXPECT_EQ(extra % 6U, 0U);
    EXPECT_GE(extra / 6U, 2U);
}

TEST_F(FormaTextFieldSelectionDrawTest, ASelectionOnlyCoversTheSelectedCharacters)
{
    auto buffer = text_buffer();
    auto field = focused_field(buffer, tall_params(3), "ab\ncd");
    const uint32_t plain = vertex_count(buffer);

    shift_left();

    EXPECT_EQ(vertex_count(buffer), plain + 6U);
}

}
