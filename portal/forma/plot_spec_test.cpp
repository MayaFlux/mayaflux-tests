#include "gtest/gtest.h"

#include "MayaFlux/Core/Backends/Graphics/Vulkan/VKImage.hpp"
#include "MayaFlux/Kakshya/NDData/VertexLayout.hpp"
#include "MayaFlux/Portal/Forma/Plot/PlotSpec.hpp"

namespace MayaFlux::Test {

namespace Plot = Portal::Forma::Plot;
using Kakshya::LineVertex;
using Kakshya::MeshVertex;
using Kinesis::AABB2D;
using Plot::AxisRange;

namespace {

    template <typename V>
    std::vector<V> decode(const std::vector<uint8_t>& bytes)
    {
        std::vector<V> out(bytes.size() / sizeof(V));
        std::memcpy(out.data(), bytes.data(), out.size() * sizeof(V));
        return out;
    }

    std::vector<std::span<const double>> as_spans(const std::vector<std::vector<double>>& series)
    {
        std::vector<std::span<const double>> spans;
        for (const auto& s : series)
            spans.emplace_back(s.data(), s.size());
        return spans;
    }

    const AABB2D k_plot { .min = { -0.5F, -0.25F }, .max = { 0.5F, 0.25F } };

}

// =============================================================================
// AxisRange
// =============================================================================

TEST(FormaPlotAxisRangeTest, DefaultsToTheSignedUnitRangeWithoutAutoScaling)
{
    const AxisRange range;

    EXPECT_FLOAT_EQ(range.min, -1.F);
    EXPECT_FLOAT_EQ(range.max, 1.F);
    EXPECT_FALSE(range.auto_scaling);
    EXPECT_FALSE(static_cast<bool>(range.scale_predicate));
}

TEST(FormaPlotAxisRangeTest, NormaliseMapsTheRangeOntoZeroToOne)
{
    AxisRange range;
    range.range(10.F, 20.F);

    EXPECT_FLOAT_EQ(range.normalise(10.F), 0.F);
    EXPECT_FLOAT_EQ(range.normalise(15.F), 0.5F);
    EXPECT_FLOAT_EQ(range.normalise(20.F), 1.F);
    EXPECT_FLOAT_EQ(range.normalise(30.F), 2.F);
    EXPECT_FLOAT_EQ(range.normalise(0.F), -1.F);
}

TEST(FormaPlotAxisRangeTest, ToNdcMapsTheRangeOntoMinusOneToOne)
{
    AxisRange range;
    range.range(0.F, 100.F);

    EXPECT_FLOAT_EQ(range.to_ndc(0.F), -1.F);
    EXPECT_FLOAT_EQ(range.to_ndc(50.F), 0.F);
    EXPECT_FLOAT_EQ(range.to_ndc(100.F), 1.F);
}

TEST(FormaPlotAxisRangeTest, ADegenerateRangeNormalisesEverythingToZero)
{
    AxisRange range;
    range.range(3.F, 3.F);

    EXPECT_FLOAT_EQ(range.normalise(3.F), 0.F);
    EXPECT_FLOAT_EQ(range.normalise(99.F), 0.F);
    EXPECT_FLOAT_EQ(range.to_ndc(99.F), -1.F);
}

TEST(FormaPlotAxisRangeTest, AnInvertedRangeFlipsTheAxis)
{
    AxisRange range;
    range.range(1.F, -1.F);

    EXPECT_FLOAT_EQ(range.to_ndc(1.F), -1.F);
    EXPECT_FLOAT_EQ(range.to_ndc(-1.F), 1.F);
}

TEST(FormaPlotAxisRangeTest, FluentSettersChainOnTheSameObject)
{
    AxisRange range;

    AxisRange& chained = range.range(2.F, 4.F).auto_scale();

    EXPECT_EQ(&chained, &range);
    EXPECT_FLOAT_EQ(range.min, 2.F);
    EXPECT_FLOAT_EQ(range.max, 4.F);
    EXPECT_TRUE(range.auto_scaling);
    EXPECT_FALSE(range.auto_scale(false).auto_scaling);
}

TEST(FormaPlotAxisRangeTest, ScaleIfStoresThePredicateAndEnablesAutoScaling)
{
    AxisRange range;

    range.scale_if([] { return true; });

    EXPECT_TRUE(range.auto_scaling);
    ASSERT_TRUE(static_cast<bool>(range.scale_predicate));
    EXPECT_TRUE(range.scale_predicate());
}

// =============================================================================
// data_range and apply_auto_scale
// =============================================================================

TEST(FormaPlotDataRangeTest, EmptySeriesGivesTheUnitRange)
{
    const auto [lo, hi] = Plot::data_range({});

    EXPECT_FLOAT_EQ(lo, 0.F);
    EXPECT_FLOAT_EQ(hi, 1.F);
}

TEST(FormaPlotDataRangeTest, ReturnsTheMinimumAndMaximum)
{
    const std::vector<double> series { 3.0, -2.0, 7.5, 0.0 };

    const auto [lo, hi] = Plot::data_range(series);

    EXPECT_FLOAT_EQ(lo, -2.F);
    EXPECT_FLOAT_EQ(hi, 7.5F);
}

TEST(FormaPlotDataRangeTest, AConstantSeriesGetsAUnitWideRangeSoTheAxisIsNotDegenerate)
{
    const std::vector<double> series { 4.0, 4.0, 4.0 };

    const auto [lo, hi] = Plot::data_range(series);

    EXPECT_FLOAT_EQ(lo, 4.F);
    EXPECT_FLOAT_EQ(hi, 5.F);
}

TEST(FormaPlotAutoScaleTest, DoesNothingUnlessAutoScalingIsOn)
{
    AxisRange range;
    range.range(-1.F, 1.F);
    const std::vector<std::vector<double>> data { { 10.0, 20.0 } };

    Plot::apply_auto_scale(range, as_spans(data));

    EXPECT_FLOAT_EQ(range.min, -1.F);
    EXPECT_FLOAT_EQ(range.max, 1.F);
}

TEST(FormaPlotAutoScaleTest, DoesNothingForNoSeries)
{
    AxisRange range;
    range.range(-3.F, 3.F).auto_scale();

    Plot::apply_auto_scale(range, {});

    EXPECT_FLOAT_EQ(range.min, -3.F);
    EXPECT_FLOAT_EQ(range.max, 3.F);
}

TEST(FormaPlotAutoScaleTest, TakesTheUnionOfEverySeries)
{
    AxisRange range;
    range.auto_scale();
    const std::vector<std::vector<double>> data { { 1.0, 2.0 }, { -4.0, 0.5 }, { 3.0, 9.0 } };

    Plot::apply_auto_scale(range, as_spans(data));

    EXPECT_FLOAT_EQ(range.min, -4.F);
    EXPECT_FLOAT_EQ(range.max, 9.F);
}

TEST(FormaPlotAutoScaleTest, ARejectingPredicateKeepsTheCurrentRange)
{
    AxisRange range;
    range.range(-1.F, 1.F).scale_if([] { return false; });
    const std::vector<std::vector<double>> data { { 10.0, 20.0 } };

    Plot::apply_auto_scale(range, as_spans(data));

    EXPECT_FLOAT_EQ(range.min, -1.F);
    EXPECT_FLOAT_EQ(range.max, 1.F);
}

TEST(FormaPlotAutoScaleTest, AnAcceptingPredicateIsEvaluatedOncePerCall)
{
    int evaluations = 0;
    AxisRange range;
    range.scale_if([&evaluations] {
        ++evaluations;
        return true;
    });
    const std::vector<std::vector<double>> data { { 1.0, 2.0 }, { 3.0, 4.0 } };

    Plot::apply_auto_scale(range, as_spans(data));

    EXPECT_EQ(evaluations, 1);
    EXPECT_FLOAT_EQ(range.min, 1.F);
    EXPECT_FLOAT_EQ(range.max, 4.F);
}

// =============================================================================
// Palette
// =============================================================================

TEST(FormaPlotPaletteTest, CyclesThroughTheColours)
{
    const std::vector<glm::vec3> palette { { 1.F, 0.F, 0.F }, { 0.F, 1.F, 0.F } };

    EXPECT_EQ(Plot::palette_color(palette, 0), palette[0]);
    EXPECT_EQ(Plot::palette_color(palette, 1), palette[1]);
    EXPECT_EQ(Plot::palette_color(palette, 2), palette[0]);
    EXPECT_EQ(Plot::palette_color(palette, 5), palette[1]);
}

TEST(FormaPlotPaletteTest, AnEmptyPaletteIsWhite)
{
    EXPECT_EQ(Plot::palette_color({}, 3), glm::vec3(1.F));
}

TEST(FormaPlotPaletteTest, ASingleColourIsUniform)
{
    const std::vector<glm::vec3> palette { { 0.2F, 0.4F, 0.6F } };

    for (size_t i = 0; i < 4; ++i)
        EXPECT_EQ(Plot::palette_color(palette, i), palette[0]);
}

// =============================================================================
// background
// =============================================================================

TEST(FormaPlotBackgroundTest, SolidQuadCoversTheBoundsAsATriangleStrip)
{
    const auto fn = Plot::background(k_plot, glm::vec3(0.1F, 0.2F, 0.3F));
    std::vector<uint8_t> bytes { 1, 2, 3 };
    Portal::Forma::Element el;

    fn(0.F, bytes, el);

    const auto verts = decode<MeshVertex>(bytes);
    ASSERT_EQ(verts.size(), 4U);
    EXPECT_EQ(verts[0].position, glm::vec3(-0.5F, -0.25F, 0.F));
    EXPECT_EQ(verts[1].position, glm::vec3(0.5F, -0.25F, 0.F));
    EXPECT_EQ(verts[2].position, glm::vec3(-0.5F, 0.25F, 0.F));
    EXPECT_EQ(verts[3].position, glm::vec3(0.5F, 0.25F, 0.F));
    for (const auto& v : verts) {
        EXPECT_EQ(v.color, glm::vec4(glm::vec3(0.1F, 0.2F, 0.3F), 1.F));
        EXPECT_FLOAT_EQ(v.weight, 0.F);
        EXPECT_EQ(v.normal, glm::vec3(0.F, 0.F, 1.F));
        EXPECT_EQ(v.tangent, glm::vec4(glm::vec3(1.F, 0.F, 0.F), 1.F));
    }
}

TEST(FormaPlotBackgroundTest, PublishesTheBoundsAndIgnoresTheValue)
{
    const auto fn = Plot::background(k_plot);
    std::vector<uint8_t> first;
    std::vector<uint8_t> second;
    Portal::Forma::Element el;

    fn(0.F, first, el);
    fn(123.F, second, el);

    ASSERT_TRUE(el.bounds_hint.has_value());
    EXPECT_EQ(el.bounds_hint->min, k_plot.min);
    EXPECT_EQ(el.bounds_hint->max, k_plot.max);
    EXPECT_EQ(first, second);
}

TEST(FormaPlotBackgroundTest, TexturedQuadCarriesPositionsAndUvOnTheTexturedStride)
{
    auto texture = std::make_shared<Core::VKImage>(4, 4, 1, vk::Format::eR8G8B8A8Unorm, Core::VKImage::Usage::TEXTURE_2D);
    const auto fn = Plot::background(k_plot, glm::vec3(1.F), texture);
    std::vector<uint8_t> bytes;
    Portal::Forma::Element el;

    fn(0.F, bytes, el);

    const uint32_t stride = Kakshya::VertexLayout::for_textured_quad().stride_bytes;
    ASSERT_EQ(bytes.size(), 4U * stride);
    const std::array<glm::vec2, 4> uvs { glm::vec2(0.F, 1.F), glm::vec2(1.F, 1.F), glm::vec2(0.F, 0.F), glm::vec2(1.F, 0.F) };
    const std::array<glm::vec3, 4> positions { glm::vec3(-0.5F, -0.25F, 0.F), glm::vec3(0.5F, -0.25F, 0.F), glm::vec3(-0.5F, 0.25F, 0.F), glm::vec3(0.5F, 0.25F, 0.F) };
    for (size_t i = 0; i < 4; ++i) {
        glm::vec3 position;
        glm::vec2 uv;
        std::memcpy(&position, bytes.data() + i * stride, sizeof(position));
        std::memcpy(&uv, bytes.data() + i * stride + 12, sizeof(uv));
        EXPECT_EQ(position, positions[i]);
        EXPECT_EQ(uv, uvs[i]);
    }
}

// =============================================================================
// plot_grid
// =============================================================================

TEST(FormaPlotGridTest, EmitsOneSegmentPerDivisionOnEachAxis)
{
    const auto verts = Plot::plot_grid(k_plot, 3, 2);

    EXPECT_EQ(verts.size(), (3U + 2U) * 2U);
}

TEST(FormaPlotGridTest, VerticalLinesSpanTheHeightAtEvenlySpacedX)
{
    const auto verts = Plot::plot_grid(k_plot, 3, 0);

    ASSERT_EQ(verts.size(), 6U);
    const std::array<float, 3> xs { -0.5F, 0.F, 0.5F };
    for (size_t i = 0; i < 3; ++i) {
        EXPECT_FLOAT_EQ(verts[2 * i].position.x, xs[i]);
        EXPECT_FLOAT_EQ(verts[2 * i + 1].position.x, xs[i]);
        EXPECT_FLOAT_EQ(verts[2 * i].position.y, k_plot.min.y);
        EXPECT_FLOAT_EQ(verts[2 * i + 1].position.y, k_plot.max.y);
    }
}

TEST(FormaPlotGridTest, HorizontalLinesSpanTheWidthAtEvenlySpacedY)
{
    const auto verts = Plot::plot_grid(k_plot, 0, 3);

    ASSERT_EQ(verts.size(), 6U);
    const std::array<float, 3> ys { -0.25F, 0.F, 0.25F };
    for (size_t i = 0; i < 3; ++i) {
        EXPECT_FLOAT_EQ(verts[2 * i].position.y, ys[i]);
        EXPECT_FLOAT_EQ(verts[2 * i].position.x, k_plot.min.x);
        EXPECT_FLOAT_EQ(verts[2 * i + 1].position.x, k_plot.max.x);
    }
}

TEST(FormaPlotGridTest, ASingleDivisionSitsInTheMiddle)
{
    const auto verts = Plot::plot_grid(k_plot, 1, 1);

    ASSERT_EQ(verts.size(), 4U);
    EXPECT_FLOAT_EQ(verts[0].position.x, 0.F);
    EXPECT_FLOAT_EQ(verts[2].position.y, 0.F);
}

TEST(FormaPlotGridTest, ZeroDivisionsGiveNoGeometry)
{
    EXPECT_TRUE(Plot::plot_grid(k_plot, 0, 0).empty());
}

TEST(FormaPlotGridTest, ColourAndThicknessApplyToEveryVertex)
{
    const auto verts = Plot::plot_grid(k_plot, 2, 2, glm::vec3(0.4F, 0.5F, 0.6F), 3.F);

    for (const auto& v : verts) {
        EXPECT_EQ(v.color, glm::vec4(glm::vec3(0.4F, 0.5F, 0.6F), 1.F));
        EXPECT_FLOAT_EQ(v.thickness, 3.F);
        EXPECT_FLOAT_EQ(v.position.z, 0.F);
    }
}

// =============================================================================
// plot_cursor
// =============================================================================

TEST(FormaPlotCursorTest, VerticalCursorMapsTheValueOntoTheXAxis)
{
    const auto fn = Plot::plot_cursor(k_plot, true, glm::vec3(0.7F), 2.F);
    std::vector<uint8_t> bytes;
    Portal::Forma::Element el;

    fn(0.25F, bytes, el);

    const auto verts = decode<LineVertex>(bytes);
    ASSERT_EQ(verts.size(), 2U);
    const float x = -0.5F + 0.25F * 1.0F;
    EXPECT_EQ(verts[0].position, glm::vec3(x, -0.25F, 0.F));
    EXPECT_EQ(verts[1].position, glm::vec3(x, 0.25F, 0.F));
    EXPECT_EQ(verts[0].color, glm::vec4(glm::vec3(0.7F), 1.F));
    EXPECT_FLOAT_EQ(verts[0].thickness, 2.F);
    ASSERT_TRUE(el.bounds_hint.has_value());
    EXPECT_NEAR(el.bounds_hint->min.x, x - 0.01F, 1e-6F);
    EXPECT_NEAR(el.bounds_hint->max.x, x + 0.01F, 1e-6F);
    EXPECT_FLOAT_EQ(el.bounds_hint->min.y, -0.25F);
    EXPECT_FLOAT_EQ(el.bounds_hint->max.y, 0.25F);
}

TEST(FormaPlotCursorTest, HorizontalCursorMapsTheValueOntoTheYAxis)
{
    const auto fn = Plot::plot_cursor(k_plot, false);
    std::vector<uint8_t> bytes;
    Portal::Forma::Element el;

    fn(0.5F, bytes, el);

    const auto verts = decode<LineVertex>(bytes);
    ASSERT_EQ(verts.size(), 2U);
    EXPECT_EQ(verts[0].position, glm::vec3(-0.5F, 0.F, 0.F));
    EXPECT_EQ(verts[1].position, glm::vec3(0.5F, 0.F, 0.F));
    EXPECT_FLOAT_EQ(el.bounds_hint->min.x, -0.5F);
    EXPECT_FLOAT_EQ(el.bounds_hint->max.x, 0.5F);
    EXPECT_NEAR(el.bounds_hint->min.y, -0.01F, 1e-6F);
    EXPECT_NEAR(el.bounds_hint->max.y, 0.01F, 1e-6F);
}

TEST(FormaPlotCursorTest, ValuesOutsideZeroToOneClampToTheEdges)
{
    const auto fn = Plot::plot_cursor(k_plot, true);
    std::vector<uint8_t> low;
    std::vector<uint8_t> high;
    Portal::Forma::Element el;

    fn(-4.F, low, el);
    fn(9.F, high, el);

    EXPECT_FLOAT_EQ(decode<LineVertex>(low)[0].position.x, -0.5F);
    EXPECT_FLOAT_EQ(decode<LineVertex>(high)[0].position.x, 0.5F);
}

// =============================================================================
// Labels
// =============================================================================

TEST(FormaPlotLabelTest, BuildsAPassiveLabelWithTheGivenFields)
{
    const auto label = Plot::plot_label("rate", k_plot, { 1.F, 0.F, 0.F, 1.F }, "title");

    EXPECT_EQ(label.text, "rate");
    EXPECT_EQ(label.bounds.min, k_plot.min);
    EXPECT_EQ(label.color, glm::vec4(1.F, 0.F, 0.F, 1.F));
    EXPECT_EQ(label.name, "title");
    EXPECT_FALSE(label.interactive);
}

TEST(FormaPlotTickLabelsTest, ValuesInterpolateAcrossTheRangeWithTheRequestedPrecision)
{
    AxisRange range;
    range.range(0.F, 1.F);

    const auto labels = Plot::plot_tick_labels(k_plot, range, 5, Plot::TickEdge::Bottom, { 1.F, 1.F, 1.F, 1.F }, 2);

    ASSERT_EQ(labels.size(), 5U);
    const std::array<std::string, 5> expected { "0.00", "0.25", "0.50", "0.75", "1.00" };
    for (size_t i = 0; i < 5; ++i)
        EXPECT_EQ(labels[i].text, expected[i]);
}

TEST(FormaPlotTickLabelsTest, DecimalPlacesControlThePrecisionIncludingZero)
{
    AxisRange range;
    range.range(-2.F, 2.F);

    const auto whole = Plot::plot_tick_labels(k_plot, range, 3, Plot::TickEdge::Bottom, { 1.F, 1.F, 1.F, 1.F }, 0);
    const auto fine = Plot::plot_tick_labels(k_plot, range, 3, Plot::TickEdge::Bottom, { 1.F, 1.F, 1.F, 1.F }, 3);

    EXPECT_EQ(whole[0].text, "-2");
    EXPECT_EQ(whole[1].text, "0");
    EXPECT_EQ(whole[2].text, "2");
    EXPECT_EQ(fine[0].text, "-2.000");
}

TEST(FormaPlotTickLabelsTest, CountsBelowTwoAreRaisedToTwo)
{
    AxisRange range;
    range.range(0.F, 1.F);

    EXPECT_EQ(Plot::plot_tick_labels(k_plot, range, 0).size(), 2U);
    EXPECT_EQ(Plot::plot_tick_labels(k_plot, range, 1).size(), 2U);
}

TEST(FormaPlotTickLabelsTest, LabelsAreNamedByPrefixAndIndexAndAreNotInteractive)
{
    Plot::TickLabelsSpec spec;
    spec.plot_bounds = k_plot;
    spec.count = 3;
    spec.name_prefix = "axis";

    const auto labels = Plot::plot_tick_labels(spec);

    ASSERT_EQ(labels.size(), 3U);
    for (size_t i = 0; i < labels.size(); ++i) {
        EXPECT_EQ(labels[i].name, "axis_" + std::to_string(i));
        EXPECT_FALSE(labels[i].interactive);
    }
}

TEST(FormaPlotTickLabelsTest, BottomLabelsSitBelowThePlotCentredOnTheirTick)
{
    Plot::TickLabelsSpec spec;
    spec.plot_bounds = k_plot;
    spec.count = 3;
    spec.edge = Plot::TickEdge::Bottom;
    spec.label_h = 0.1F;

    const auto labels = Plot::plot_tick_labels(spec);

    const float half_w = k_plot.width() / 3.F * 0.5F;
    const std::array<float, 3> centres { -0.5F, 0.F, 0.5F };
    for (size_t i = 0; i < 3; ++i) {
        EXPECT_NEAR(labels[i].bounds.min.x, centres[i] - half_w, 1e-6F);
        EXPECT_NEAR(labels[i].bounds.max.x, centres[i] + half_w, 1e-6F);
        EXPECT_FLOAT_EQ(labels[i].bounds.max.y, k_plot.min.y);
        EXPECT_NEAR(labels[i].bounds.min.y, k_plot.min.y - 0.1F, 1e-6F);
    }
}

TEST(FormaPlotTickLabelsTest, TopLabelsSitAboveThePlot)
{
    Plot::TickLabelsSpec spec;
    spec.plot_bounds = k_plot;
    spec.edge = Plot::TickEdge::Top;
    spec.label_h = 0.1F;

    for (const auto& label : Plot::plot_tick_labels(spec)) {
        EXPECT_FLOAT_EQ(label.bounds.min.y, k_plot.max.y);
        EXPECT_NEAR(label.bounds.max.y, k_plot.max.y + 0.1F, 1e-6F);
    }
}

TEST(FormaPlotTickLabelsTest, LeftLabelsRunBottomToTopAlongTheLeftEdge)
{
    AxisRange range;
    range.range(10.F, 20.F);
    Plot::TickLabelsSpec spec;
    spec.plot_bounds = k_plot;
    spec.range = range;
    spec.count = 3;
    spec.edge = Plot::TickEdge::Left;
    spec.label_w = 0.2F;
    spec.decimal_places = 0;

    const auto labels = Plot::plot_tick_labels(spec);

    ASSERT_EQ(labels.size(), 3U);
    EXPECT_EQ(labels[0].text, "10");
    EXPECT_EQ(labels[2].text, "20");
    const float half_h = k_plot.height() / 3.F * 0.5F;
    EXPECT_NEAR(labels[0].bounds.min.y, k_plot.min.y - half_h, 1e-6F);
    EXPECT_NEAR(labels[2].bounds.max.y, k_plot.max.y + half_h, 1e-6F);
    for (const auto& label : labels) {
        EXPECT_FLOAT_EQ(label.bounds.max.x, k_plot.min.x);
        EXPECT_NEAR(label.bounds.min.x, k_plot.min.x - 0.2F, 1e-6F);
    }
}

TEST(FormaPlotTickLabelsTest, RightLabelsSitBesideTheRightEdge)
{
    Plot::TickLabelsSpec spec;
    spec.plot_bounds = k_plot;
    spec.edge = Plot::TickEdge::Right;
    spec.label_w = 0.2F;

    for (const auto& label : Plot::plot_tick_labels(spec)) {
        EXPECT_FLOAT_EQ(label.bounds.min.x, k_plot.max.x);
        EXPECT_NEAR(label.bounds.max.x, k_plot.max.x + 0.2F, 1e-6F);
    }
}

TEST(FormaPlotTickLabelsTest, ConvenienceOverloadMatchesTheSpecOverload)
{
    AxisRange range;
    range.range(0.F, 8.F);
    Plot::TickLabelsSpec spec;
    spec.plot_bounds = k_plot;
    spec.range = range;
    spec.count = 4;
    spec.edge = Plot::TickEdge::Left;
    spec.decimal_places = 1;
    spec.label_h = 0.07F;
    spec.label_w = 0.15F;

    const auto direct = Plot::plot_tick_labels(spec);
    const auto convenient = Plot::plot_tick_labels(k_plot, range, 4, Plot::TickEdge::Left, spec.color, 1, 0.07F, 0.15F);

    ASSERT_EQ(direct.size(), convenient.size());
    for (size_t i = 0; i < direct.size(); ++i) {
        EXPECT_EQ(direct[i].text, convenient[i].text);
        EXPECT_EQ(direct[i].bounds.min, convenient[i].bounds.min);
        EXPECT_EQ(direct[i].bounds.max, convenient[i].bounds.max);
    }
}

// =============================================================================
// Legend
// =============================================================================

TEST(FormaPlotLegendTest, SpecPairsLabelsWithColoursAndStopsAtTheShorterList)
{
    const std::vector<std::string> labels { "a", "b", "c" };
    const std::vector<glm::vec3> colors { { 1.F, 0.F, 0.F }, { 0.F, 1.F, 0.F } };

    const auto spec = Plot::plot_legend({ 0.1F, 0.2F }, labels, colors, 0.09F, 0.05F, { 1.F, 1.F, 1.F, 1.F });

    ASSERT_EQ(spec.entries.size(), 2U);
    EXPECT_EQ(spec.entries[0].label, "a");
    EXPECT_EQ(spec.entries[1].color, glm::vec3(0.F, 1.F, 0.F));
    EXPECT_EQ(spec.origin, glm::vec2(0.1F, 0.2F));
    EXPECT_FLOAT_EQ(spec.row_h, 0.09F);
    EXPECT_FLOAT_EQ(spec.swatch_w, 0.05F);
}

TEST(FormaPlotLegendTest, LayoutStacksRowsDownwardFromTheOriginWithTheGap)
{
    Plot::LegendSpec spec;
    spec.origin = { 0.F, 0.5F };
    spec.row_h = 0.1F;
    spec.gap = 0.02F;
    spec.swatch_w = 0.05F;
    spec.text_w = 0.3F;
    spec.entries = { { "one", { 1.F, 0.F, 0.F } }, { "two", { 0.F, 1.F, 0.F } }, { "three", { 0.F, 0.F, 1.F } } };

    const auto layout = Plot::layout_legend(spec);

    ASSERT_EQ(layout.swatches.size(), 3U);
    ASSERT_EQ(layout.labels.size(), 3U);
    for (size_t i = 0; i < 3; ++i) {
        const float top = 0.5F - static_cast<float>(i) * 0.12F;
        EXPECT_NEAR(layout.swatches[i].bounds.max.y, top, 1e-6F);
        EXPECT_NEAR(layout.swatches[i].bounds.min.y, top - 0.1F, 1e-6F);
        EXPECT_FLOAT_EQ(layout.swatches[i].bounds.min.x, 0.F);
        EXPECT_FLOAT_EQ(layout.swatches[i].bounds.max.x, 0.05F);
        EXPECT_NEAR(layout.labels[i].bounds.min.x, 0.07F, 1e-6F);
        EXPECT_NEAR(layout.labels[i].bounds.max.x, 0.37F, 1e-6F);
        EXPECT_NEAR(layout.labels[i].bounds.max.y, top, 1e-6F);
    }
    EXPECT_EQ(layout.swatches[2].color, glm::vec3(0.F, 0.F, 1.F));
    EXPECT_EQ(layout.labels[1].text, "two");
}

TEST(FormaPlotLegendTest, LayoutNamesAndInteractivityFollowTheSpec)
{
    Plot::LegendSpec spec;
    spec.name_prefix = "key";
    spec.interactive = true;
    spec.text_color = { 0.1F, 0.2F, 0.3F, 1.F };
    spec.entries = { { "x", glm::vec3(1.F) }, { "y", glm::vec3(1.F) } };

    const auto layout = Plot::layout_legend(spec);

    EXPECT_EQ(layout.swatches[1].name, "key_swatch_1");
    EXPECT_EQ(layout.labels[0].name, "key_label_0");
    EXPECT_TRUE(layout.swatches[0].interactive);
    EXPECT_TRUE(layout.labels[1].interactive);
    EXPECT_EQ(layout.labels[0].color, glm::vec4(0.1F, 0.2F, 0.3F, 1.F));
}

TEST(FormaPlotLegendTest, AnEmptyLegendLaysOutNothing)
{
    const auto layout = Plot::layout_legend(Plot::LegendSpec {});

    EXPECT_TRUE(layout.swatches.empty());
    EXPECT_TRUE(layout.labels.empty());
}

}
