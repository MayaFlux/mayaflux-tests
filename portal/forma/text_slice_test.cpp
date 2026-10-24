#include "../../support/forma_rig.hpp"

#include "MayaFlux/Portal/Forma/Internal/TextRow.hpp"
#include "MayaFlux/Portal/Forma/Internal/TextSlice.hpp"
#include "MayaFlux/Portal/Forma/Primitives/TextField.hpp"

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

TEST_F(FormaTextSliceTest, ATextRowWithoutABatchGetsABufferOfItsOwn)
{
    const AABB2D row { .min = { -1.F, 0.F }, .max = { 1.F, 0.2F } };

    auto first = Forma::internal::add_text_row(*surface, "one", row, { 200, 24 });
    auto second = Forma::internal::add_text_row(*surface, "two", row, { 200, 24 });

    EXPECT_NE(first.second->buffer.buffer(), second.second->buffer.buffer());
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

}
