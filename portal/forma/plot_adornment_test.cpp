#include "../../support/forma_rig.hpp"

#include "MayaFlux/Portal/Forma/Internal/TextSlice.hpp"
#include "MayaFlux/Portal/Forma/Plot/Plot.hpp"
#include "MayaFlux/Portal/Forma/Plot/SeriesBuilder.hpp"

namespace MayaFlux::Test {

namespace Forma = Portal::Forma;
using Kinesis::AABB2D;
using Portal::Graphics::PrimitiveTopology;

namespace {

    bool contains(const std::vector<uint32_t>& ids, uint32_t id)
    {
        return std::ranges::find(ids, id) != ids.end();
    }

    Forma::Plot::LabelSpec label(std::string text, float x)
    {
        Forma::Plot::LabelSpec spec;
        spec.text = std::move(text);
        spec.bounds = AABB2D { .min = { x, -0.5F }, .max = { x + 0.3F, -0.4F } };
        return spec;
    }

}

class FormaPlotAdornmentTest : public FormaRigTest {
protected:
    void SetUp() override
    {
        FormaRigTest::SetUp();
        if (IsSkipped())
            return;
        surface.emplace(make_surface("adornments"));
        parent = surface->add(Forma::Element {}.with_bounds({ .min = { -1.F, -1.F }, .max = { 1.F, 1.F } }));
    }

    void TearDown() override
    {
        surface.reset();
        FormaRigTest::TearDown();
    }

    size_t adorn(const Forma::Plot::SeriesSpec& spec)
    {
        const size_t before = surface->layer().size();
        Forma::internal::atelier().place_adornments(*surface, spec, parent);
        return surface->layer().size() - before;
    }

    std::optional<Forma::Surface> surface;
    uint32_t parent {};
};

// =============================================================================
// place_adornments
// =============================================================================

TEST_F(FormaPlotAdornmentTest, ExplicitLabelsBecomePassiveTextElementsRelatedToTheSeries)
{
    Forma::Plot::SeriesSpec spec;
    spec.labels = { label("x axis", -0.9F), label("y axis", -0.4F) };

    EXPECT_EQ(adorn(spec), 2U);

    const auto related = surface->layer().related_ids(parent);
    ASSERT_EQ(related.size(), 2U);
    for (const auto id : related) {
        const auto* element = surface->layer().get(id);
        ASSERT_NE(element, nullptr);
        EXPECT_FALSE(element->interactive);
        EXPECT_NE(element->text_slice, nullptr);
        EXPECT_NE(element->buffer, nullptr);
    }
}

TEST_F(FormaPlotAdornmentTest, TickLabelsAddOneElementPerGeneratedLabel)
{
    Forma::Plot::TickLabelsSpec ticks;
    ticks.plot_bounds = AABB2D { .min = { -0.8F, -0.6F }, .max = { 0.8F, 0.6F } };
    ticks.count = 4;
    Forma::Plot::SeriesSpec spec;
    spec.tick_labels = { ticks };
    const size_t expected = Forma::Plot::plot_tick_labels(ticks).size();
    ASSERT_GT(expected, 0U);

    EXPECT_EQ(adorn(spec), expected);
    EXPECT_EQ(surface->layer().related_ids(parent).size(), expected);
}

TEST_F(FormaPlotAdornmentTest, ALegendAddsASwatchAndALabelPerEntry)
{
    Forma::Plot::LegendSpec legend;
    legend.origin = { -0.9F, 0.9F };
    legend.entries = { { "first", { 1.F, 0.F, 0.F } }, { "second", { 0.F, 1.F, 0.F } } };
    Forma::Plot::SeriesSpec spec;
    spec.legend = legend;
    const auto layout = Forma::Plot::layout_legend(legend);
    ASSERT_EQ(layout.swatches.size(), 2U);
    ASSERT_EQ(layout.labels.size(), 2U);

    EXPECT_EQ(adorn(spec), 4U);

    std::shared_ptr<Buffers::FormaBuffer> batch;
    size_t textured = 0;
    size_t flat = 0;
    for (const auto id : surface->layer().related_ids(parent)) {
        const auto* element = surface->layer().get(id);
        ASSERT_NE(element, nullptr);
        EXPECT_FALSE(element->interactive);
        if (!batch)
            batch = element->buffer.buffer();
        EXPECT_EQ(element->buffer.buffer(), batch);
        (element->text_slice ? textured : flat)++;
    }
    EXPECT_EQ(textured, 2U);
    EXPECT_EQ(flat, 2U);
    ASSERT_NE(batch, nullptr);
    EXPECT_EQ(batch->slice_count(), 4U);
}

TEST_F(FormaPlotAdornmentTest, LegendSwatchesAndLabelsShareTheGivenBatch)
{
    Forma::Plot::LegendSpec legend;
    legend.origin = { -0.9F, 0.9F };
    legend.entries = { { "first", { 1.F, 0.F, 0.F } }, { "second", { 0.F, 1.F, 0.F } } };
    Forma::Plot::SeriesSpec spec;
    spec.legend = legend;
    auto batch = Forma::internal::atelier().create_buffer(surface->render_config({
        .fragment_shader = "forma_plot.frag.spv",
        .additional_textures = { { "text", nullptr } },
    }));

    Forma::internal::atelier().place_adornments(*surface, spec, parent, batch);

    EXPECT_EQ(batch->slice_count(), 4U);
    std::vector<Forma::FormaTarget::SliceId> slices;
    size_t labels = 0;
    for (const auto id : surface->layer().related_ids(parent)) {
        const auto* element = surface->layer().get(id);
        ASSERT_NE(element, nullptr);
        EXPECT_EQ(element->buffer.buffer(), batch);
        EXPECT_NE(element->buffer.slice(), 0U);
        slices.push_back(element->buffer.slice());
        if (element->text_slice)
            ++labels;
    }
    std::ranges::sort(slices);
    EXPECT_EQ(std::ranges::unique(slices).begin(), slices.end());
    EXPECT_EQ(labels, 2U);
}

TEST_F(FormaPlotAdornmentTest, EveryDeclaredKindIsPlacedTogether)
{
    Forma::Plot::TickLabelsSpec ticks;
    ticks.plot_bounds = AABB2D { .min = { -0.8F, -0.6F }, .max = { 0.8F, 0.6F } };
    Forma::Plot::LegendSpec legend;
    legend.entries = { { "only", { 1.F, 1.F, 1.F } } };
    Forma::Plot::SeriesSpec spec;
    spec.labels = { label("title", -0.2F) };
    spec.tick_labels = { ticks };
    spec.legend = legend;
    const size_t expected = 1 + Forma::Plot::plot_tick_labels(ticks).size()
        + Forma::Plot::layout_legend(legend).swatches.size()
        + Forma::Plot::layout_legend(legend).labels.size();

    EXPECT_EQ(adorn(spec), expected);
}

TEST_F(FormaPlotAdornmentTest, RemovingTheSeriesElementRemovesItsAdornments)
{
    Forma::Plot::SeriesSpec spec;
    spec.labels = { label("a", -0.9F), label("b", -0.4F) };
    ASSERT_EQ(adorn(spec), 2U);

    Forma::destroy(*surface, parent);

    EXPECT_TRUE(surface->layer().empty());
}

// =============================================================================
// place_label
// =============================================================================

TEST_F(FormaPlotAdornmentTest, ALabelWithoutRenderBoundsIsLaidOutAgainWhenTheWindowResizes)
{
    auto buffer = Forma::create_buffer(surface->render_config({ .additional_textures = { { "text", nullptr } } }));
    const uint32_t id = Forma::Plot::place_label(*surface, buffer, label("resizable", -0.9F), parent);
    const auto slice = surface->layer().get(id)->text_slice;
    ASSERT_NE(slice, nullptr);
    const auto before = slice->run().params().render_bounds;

    window->resize(600, 400);

    EXPECT_NE(slice->run().params().render_bounds, before);
}

TEST_F(FormaPlotAdornmentTest, ALabelWithExplicitRenderBoundsIgnoresResizes)
{
    auto buffer = Forma::create_buffer(surface->render_config({ .additional_textures = { { "text", nullptr } } }));
    auto spec = label("fixed", -0.9F);
    spec.render_bounds = { 96, 24 };
    const uint32_t id = Forma::Plot::place_label(*surface, buffer, spec, parent);
    const auto slice = surface->layer().get(id)->text_slice;
    ASSERT_NE(slice, nullptr);

    window->resize(600, 400);

    EXPECT_EQ(slice->run().params().render_bounds, glm::uvec2(96, 24));
}

TEST_F(FormaPlotAdornmentTest, ALabelCarriesItsNameBoundsAndInteractivity)
{
    auto buffer = Forma::create_buffer(surface->render_config({ .additional_textures = { { "text", nullptr } } }));
    auto spec = label("named", -0.5F);
    spec.name = "axis_title";
    spec.interactive = true;

    const uint32_t id = Forma::Plot::place_label(*surface, buffer, spec, 0);

    const auto* element = surface->layer().get(id);
    ASSERT_NE(element, nullptr);
    EXPECT_EQ(element->name, "axis_title");
    EXPECT_TRUE(element->interactive);
    ASSERT_TRUE(element->bounds_hint.has_value());
    EXPECT_NEAR(element->bounds_hint->min.x, -0.5F, 1e-6F);
    EXPECT_TRUE(surface->layer().related_ids(parent).empty());
}

// =============================================================================
// place_rect
// =============================================================================

TEST_F(FormaPlotAdornmentTest, ARectIsAPassiveElementRelatedToItsParentWhenAsked)
{
    auto buffer = Forma::FormaTarget::slice_of(
        Forma::create_buffer(surface->render_config()), PrimitiveTopology::TRIANGLE_STRIP);
    Forma::Plot::RectSpec spec;
    spec.bounds = AABB2D { .min = { -0.2F, -0.2F }, .max = { 0.2F, 0.2F } };
    spec.name = "swatch";

    const uint32_t id = Forma::Plot::place_rect(*surface, buffer, spec, parent);

    const auto* element = surface->layer().get(id);
    ASSERT_NE(element, nullptr);
    EXPECT_FALSE(element->interactive);
    EXPECT_EQ(element->name, "swatch");
    EXPECT_EQ(element->buffer, buffer);
    EXPECT_TRUE(contains(surface->layer().related_ids(parent), id));
}

TEST_F(FormaPlotAdornmentTest, AnInteractiveRectIsHitTestable)
{
    auto buffer = Forma::FormaTarget::slice_of(
        Forma::create_buffer(surface->render_config()), PrimitiveTopology::TRIANGLE_STRIP);
    Forma::Plot::RectSpec spec;
    spec.bounds = AABB2D { .min = { 0.3F, 0.3F }, .max = { 0.6F, 0.6F } };
    spec.interactive = true;

    const uint32_t id = Forma::Plot::place_rect(*surface, buffer, spec, 0);

    EXPECT_EQ(surface->layer().hit_test({ 0.4F, 0.4F }), id);
    EXPECT_NE(surface->layer().hit_test({ -0.9F, -0.9F }), id);
}

}
