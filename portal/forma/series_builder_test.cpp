#include "../../test_config.h"

#include "MayaFlux/Kakshya/Source/PlotContainer.hpp"
#include "MayaFlux/Portal/Forma/Plot/PlotSource.hpp"
#include "MayaFlux/Portal/Forma/Plot/SeriesBuilder.hpp"

namespace MayaFlux::Test {

namespace Plot = Portal::Forma::Plot;
using Kakshya::MeshVertex;
using Kinesis::AABB2D;
using Plot::AxisRange;
using Role = Kakshya::DataDimension::Role;

namespace {

    struct SeriesData {
        Role role;
        std::vector<double> values;
    };

    std::shared_ptr<Kakshya::PlotContainer> make_container(const std::vector<SeriesData>& series)
    {
        Plot::Source source;
        for (const auto& s : series) {
            std::function<void(std::vector<double>&)> fill = [values = s.values](std::vector<double>& out) { out = values; };
            source.as("series", s.values.size(), s.role, Kakshya::DataModality::AUDIO_1D).from(std::move(fill));
        }
        return source.build();
    }

    std::vector<MeshVertex> decode(const std::vector<uint8_t>& bytes)
    {
        std::vector<MeshVertex> out(bytes.size() / sizeof(MeshVertex));
        std::memcpy(out.data(), bytes.data(), out.size() * sizeof(MeshVertex));
        return out;
    }

    std::vector<MeshVertex> run(const Plot::SeriesSpec& spec, const std::shared_ptr<Kakshya::PlotContainer>& container)
    {
        std::vector<uint8_t> out;
        Portal::Forma::Element element;
        spec.fn(container, out, element);
        return decode(out);
    }

    AxisRange fixed(float lo, float hi)
    {
        AxisRange range;
        range.range(lo, hi);
        return range;
    }

    const glm::vec3 k_red { 1.F, 0.F, 0.F };
    const glm::vec3 k_green { 0.F, 1.F, 0.F };
    const glm::vec3 k_blue { 0.F, 0.F, 1.F };
    const AABB2D k_plot { .min = { -0.5F, -0.25F }, .max = { 0.5F, 0.25F } };

}

// =============================================================================
// series_by_role
// =============================================================================

TEST(FormaPlotSeriesByRoleTest, ReturnsOnlyTheSeriesCarryingTheRoleInOrder)
{
    auto container = make_container({ { Role::SPATIAL_X, { 1.0, 2.0 } },
        { Role::SPATIAL_Y, { 3.0 } },
        { Role::SPATIAL_Y, { 4.0, 5.0 } } });
    container->process_default();

    const auto ys = Plot::series_by_role(*container, Role::SPATIAL_Y);
    const auto xs = Plot::series_by_role(*container, Role::SPATIAL_X);

    ASSERT_EQ(ys.size(), 2U);
    EXPECT_EQ(ys[0].size(), 1U);
    EXPECT_DOUBLE_EQ(ys[0][0], 3.0);
    EXPECT_EQ(ys[1].size(), 2U);
    EXPECT_DOUBLE_EQ(ys[1][1], 5.0);
    ASSERT_EQ(xs.size(), 1U);
    EXPECT_DOUBLE_EQ(xs[0][1], 2.0);
}

TEST(FormaPlotSeriesByRoleTest, AnAbsentRoleGivesNothing)
{
    auto container = make_container({ { Role::SPATIAL_Y, { 1.0 } } });
    container->process_default();

    EXPECT_TRUE(Plot::series_by_role(*container, Role::SPATIAL_Z).empty());
}

// =============================================================================
// Waveform
// =============================================================================

TEST(FormaPlotWaveformTest, SingleSeriesUsesUniformXAndRangeMappedY)
{
    auto container = make_container({ { Role::SPATIAL_Y, { -1.0, 0.0, 1.0 } } });
    const auto spec = Plot::Series {}.y(Role::SPATIAL_Y, fixed(-1.F, 1.F)).as_waveform().done();

    const auto verts = run(spec, container);

    ASSERT_EQ(verts.size(), 3U);
    const std::array<float, 3> xs { -1.F, 0.F, 1.F };
    const std::array<float, 3> ys { -1.F, 0.F, 1.F };
    for (size_t i = 0; i < 3; ++i) {
        EXPECT_FLOAT_EQ(verts[i].position.x, xs[i]);
        EXPECT_FLOAT_EQ(verts[i].position.y, ys[i]);
        EXPECT_FLOAT_EQ(verts[i].position.z, 0.F);
        EXPECT_EQ(verts[i].color, glm::vec3(1.F));
        EXPECT_FLOAT_EQ(verts[i].weight, 1.5F);
    }
}

TEST(FormaPlotWaveformTest, ThicknessIsConfigurable)
{
    auto container = make_container({ { Role::SPATIAL_Y, { 0.0, 1.0 } } });
    const auto spec = Plot::Series {}.y(Role::SPATIAL_Y).as_waveform().thickness(4.F).done();

    for (const auto& v : run(spec, container))
        EXPECT_FLOAT_EQ(v.weight, 4.F);
}

TEST(FormaPlotWaveformTest, SpecDeclaresALineStripAndItsCapacityArithmetic)
{
    const auto spec = Plot::Series {}.y(Role::SPATIAL_Y).as_waveform().done();

    EXPECT_EQ(spec.topology, Portal::Graphics::PrimitiveTopology::LINE_STRIP);
    EXPECT_EQ(spec.capacity_for(10), 12U * 60U);
}

TEST(FormaPlotWaveformTest, AnExplicitXSeriesProvidesTheSamplePositions)
{
    auto container = make_container({ { Role::SPATIAL_X, { 0.0, 1.0, 4.0 } },
        { Role::SPATIAL_Y, { 0.0, 0.0, 0.0 } } });
    const auto spec = Plot::Series {}
                          .x(Role::SPATIAL_X, fixed(0.F, 4.F))
                          .y(Role::SPATIAL_Y, fixed(-1.F, 1.F))
                          .as_waveform()
                          .done();

    const auto verts = run(spec, container);

    ASSERT_EQ(verts.size(), 3U);
    EXPECT_FLOAT_EQ(verts[0].position.x, -1.F);
    EXPECT_FLOAT_EQ(verts[1].position.x, -0.5F);
    EXPECT_FLOAT_EQ(verts[2].position.x, 1.F);
}

TEST(FormaPlotWaveformTest, AutoScaleStretchesYOverTheData)
{
    auto container = make_container({ { Role::SPATIAL_Y, { 0.0, 5.0, 10.0 } } });
    AxisRange range;
    range.auto_scale();
    const auto spec = Plot::Series {}.y(Role::SPATIAL_Y, range).as_waveform().done();

    const auto verts = run(spec, container);

    ASSERT_EQ(verts.size(), 3U);
    EXPECT_FLOAT_EQ(verts[0].position.y, -1.F);
    EXPECT_FLOAT_EQ(verts[1].position.y, 0.F);
    EXPECT_FLOAT_EQ(verts[2].position.y, 1.F);
}

TEST(FormaPlotWaveformTest, AZSeriesMapsOntoVertexZ)
{
    auto container = make_container({ { Role::SPATIAL_Y, { 0.0, 0.0, 0.0 } },
        { Role::SPATIAL_Z, { 0.0, 1.0, 2.0 } } });
    const auto spec = Plot::Series {}
                          .y(Role::SPATIAL_Y)
                          .z(Role::SPATIAL_Z, fixed(0.F, 2.F))
                          .as_waveform()
                          .done();

    const auto verts = run(spec, container);

    ASSERT_EQ(verts.size(), 3U);
    EXPECT_FLOAT_EQ(verts[0].position.z, -1.F);
    EXPECT_FLOAT_EQ(verts[1].position.z, 0.F);
    EXPECT_FLOAT_EQ(verts[2].position.z, 1.F);
}

TEST(FormaPlotWaveformTest, SeriesAreJoinedByTwoZeroThicknessSeparatorVertices)
{
    auto container = make_container({ { Role::SPATIAL_Y, { 0.0, 1.0 } },
        { Role::SPATIAL_Y, { 1.0, 0.0, -1.0 } } });
    const auto spec = Plot::Series {}.y(Role::SPATIAL_Y, fixed(-1.F, 1.F)).as_waveform().done();

    const auto verts = run(spec, container);

    ASSERT_EQ(verts.size(), 2U + 2U + 3U);
    EXPECT_EQ(verts[2].position, verts[1].position);
    EXPECT_EQ(verts[3].position, verts[1].position);
    EXPECT_FLOAT_EQ(verts[2].weight, 0.F);
    EXPECT_FLOAT_EQ(verts[3].weight, 0.F);
    EXPECT_FLOAT_EQ(verts[4].weight, 1.5F);
    EXPECT_FLOAT_EQ(verts[0].weight, 1.5F);
}

TEST(FormaPlotWaveformTest, MappingPaletteBeatsGlobalPaletteWhichBeatsWhite)
{
    auto container = make_container({ { Role::SPATIAL_Y, { 0.0, 1.0 } },
        { Role::SPATIAL_Y, { 0.0, 1.0 } } });

    const auto with_mapping = Plot::Series {}
                                  .color(k_blue)
                                  .y(Role::SPATIAL_Y, AxisRange {}, { k_red, k_green })
                                  .as_waveform()
                                  .done();
    const auto mapping_verts = run(with_mapping, container);
    EXPECT_EQ(mapping_verts[0].color, k_red);
    EXPECT_EQ(mapping_verts[4].color, k_green);

    const auto with_global = Plot::Series {}.color(k_blue).y(Role::SPATIAL_Y).as_waveform().done();
    const auto global_verts = run(with_global, container);
    EXPECT_EQ(global_verts[0].color, k_blue);
    EXPECT_EQ(global_verts[4].color, k_blue);

    const auto bare = Plot::Series {}.y(Role::SPATIAL_Y).as_waveform().done();
    EXPECT_EQ(run(bare, container)[0].color, glm::vec3(1.F));
}

TEST(FormaPlotWaveformTest, AMappingOfSeveralRolesCollectsEverySeriesOfThoseRoles)
{
    auto container = make_container({ { Role::SPATIAL_Y, { 0.0, 1.0 } },
        { Role::FREQUENCY, { 1.0, 0.0 } } });
    const auto spec = Plot::Series {}
                          .y(std::vector<Role> { Role::SPATIAL_Y, Role::FREQUENCY })
                          .as_waveform()
                          .done();

    EXPECT_EQ(run(spec, container).size(), 2U + 2U + 2U);
}

TEST(FormaPlotWaveformTest, ANullContainerOrNoYSeriesLeavesTheOutputUntouched)
{
    const auto spec = Plot::Series {}.y(Role::SPATIAL_Y).as_waveform().done();
    std::vector<uint8_t> out { 1, 2, 3 };
    Portal::Forma::Element element;

    spec.fn(nullptr, out, element);
    EXPECT_EQ(out, (std::vector<uint8_t> { 1, 2, 3 }));

    auto only_x = make_container({ { Role::SPATIAL_X, { 1.0, 2.0 } } });
    spec.fn(only_x, out, element);
    EXPECT_EQ(out, (std::vector<uint8_t> { 1, 2, 3 }));
}

TEST(FormaPlotWaveformTest, EveryCallRebuildsTheOutputFromScratch)
{
    auto container = make_container({ { Role::SPATIAL_Y, { 0.0, 1.0, 2.0 } } });
    const auto spec = Plot::Series {}.y(Role::SPATIAL_Y).as_waveform().done();
    std::vector<uint8_t> out;
    Portal::Forma::Element element;

    spec.fn(container, out, element);
    const size_t first = out.size();
    spec.fn(container, out, element);

    EXPECT_EQ(out.size(), first);
}

// =============================================================================
// Scatter
// =============================================================================

TEST(FormaPlotScatterTest, PointsPairXWithYAndCarryThePointSize)
{
    auto container = make_container({ { Role::SPATIAL_X, { 0.0, 1.0, 2.0 } },
        { Role::SPATIAL_Y, { -1.0, 0.0, 1.0 } } });
    const auto spec = Plot::Series {}
                          .x(Role::SPATIAL_X, fixed(0.F, 2.F))
                          .y(Role::SPATIAL_Y, fixed(-1.F, 1.F))
                          .as_scatter()
                          .done();

    const auto verts = run(spec, container);

    ASSERT_EQ(verts.size(), 3U);
    const std::array<float, 3> xs { -1.F, 0.F, 1.F };
    const std::array<float, 3> ys { -1.F, 0.F, 1.F };
    for (size_t i = 0; i < 3; ++i) {
        EXPECT_FLOAT_EQ(verts[i].position.x, xs[i]);
        EXPECT_FLOAT_EQ(verts[i].position.y, ys[i]);
        EXPECT_FLOAT_EQ(verts[i].weight, 4.F);
    }
}

TEST(FormaPlotScatterTest, PointSizeIsConfigurable)
{
    auto container = make_container({ { Role::SPATIAL_Y, { 0.0 } } });
    const auto spec = Plot::Series {}.y(Role::SPATIAL_Y).as_scatter().point_size(9.F).done();

    EXPECT_FLOAT_EQ(run(spec, container)[0].weight, 9.F);
}

TEST(FormaPlotScatterTest, WithoutXSeriesPointsSpreadUniformlyAcrossTheXRange)
{
    auto container = make_container({ { Role::SPATIAL_Y, { 0.0, 0.0, 0.0 } } });
    const auto spec = Plot::Series {}.y(Role::SPATIAL_Y).as_scatter().done();

    const auto verts = run(spec, container);

    ASSERT_EQ(verts.size(), 3U);
    EXPECT_FLOAT_EQ(verts[0].position.x, -1.F);
    EXPECT_FLOAT_EQ(verts[1].position.x, 0.F);
    EXPECT_FLOAT_EQ(verts[2].position.x, 1.F);
}

TEST(FormaPlotScatterTest, SeveralYSeriesShareASingleXSeries)
{
    auto container = make_container({ { Role::SPATIAL_X, { 0.0, 1.0 } },
        { Role::SPATIAL_Y, { 0.0, 1.0 } },
        { Role::SPATIAL_Y, { 1.0, 0.0 } } });
    const auto spec = Plot::Series {}
                          .x(Role::SPATIAL_X, fixed(0.F, 1.F))
                          .y(Role::SPATIAL_Y, fixed(0.F, 1.F))
                          .as_scatter()
                          .done();

    const auto verts = run(spec, container);

    ASSERT_EQ(verts.size(), 4U);
    EXPECT_FLOAT_EQ(verts[0].position.x, verts[2].position.x);
    EXPECT_FLOAT_EQ(verts[1].position.x, verts[3].position.x);
}

TEST(FormaPlotScatterTest, SpecDeclaresAPointListAndItsCapacityArithmetic)
{
    const auto spec = Plot::Series {}.y(Role::SPATIAL_Y).as_scatter().done();

    EXPECT_EQ(spec.topology, Portal::Graphics::PrimitiveTopology::POINT_LIST);
    EXPECT_EQ(spec.capacity_for(7), 7U * 60U);
}

// =============================================================================
// Bars
// =============================================================================

TEST(FormaPlotBarsTest, EachSampleBecomesTwoTrianglesOnTheBaseline)
{
    auto container = make_container({ { Role::SPATIAL_Y, { 0.5, -0.5 } } });
    const auto spec = Plot::Series {}.y(Role::SPATIAL_Y, fixed(-1.F, 1.F)).as_bars().done();

    const auto verts = run(spec, container);

    ASSERT_EQ(verts.size(), 12U);
    EXPECT_EQ(verts[0].position, glm::vec3(-1.F, 0.F, 0.F));
    EXPECT_EQ(verts[1].position, glm::vec3(0.F, 0.F, 0.F));
    EXPECT_EQ(verts[2].position, glm::vec3(-1.F, 0.5F, 0.F));
    EXPECT_EQ(verts[3].position, glm::vec3(0.F, 0.F, 0.F));
    EXPECT_EQ(verts[4].position, glm::vec3(0.F, 0.5F, 0.F));
    EXPECT_EQ(verts[5].position, glm::vec3(-1.F, 0.5F, 0.F));
    for (const auto& v : verts)
        EXPECT_FLOAT_EQ(v.weight, 0.F);
}

TEST(FormaPlotBarsTest, NegativeValuesHangBelowTheBaseline)
{
    auto container = make_container({ { Role::SPATIAL_Y, { 0.5, -0.5 } } });
    const auto spec = Plot::Series {}.y(Role::SPATIAL_Y, fixed(-1.F, 1.F)).as_bars().done();

    const auto verts = run(spec, container);

    EXPECT_EQ(verts[6].position, glm::vec3(0.F, 0.F, 0.F));
    EXPECT_EQ(verts[8].position, glm::vec3(0.F, -0.5F, 0.F));
    EXPECT_EQ(verts[10].position, glm::vec3(1.F, -0.5F, 0.F));
}

TEST(FormaPlotBarsTest, BarsTileTheXRangeWithoutGaps)
{
    auto container = make_container({ { Role::SPATIAL_Y, { 1.0, 1.0, 1.0, 1.0 } } });
    const auto spec = Plot::Series {}.y(Role::SPATIAL_Y, fixed(-1.F, 1.F)).as_bars().done();

    const auto verts = run(spec, container);

    ASSERT_EQ(verts.size(), 24U);
    for (size_t bar = 0; bar + 1 < 4; ++bar)
        EXPECT_FLOAT_EQ(verts[bar * 6 + 1].position.x, verts[(bar + 1) * 6].position.x);
    EXPECT_FLOAT_EQ(verts[0].position.x, -1.F);
    EXPECT_FLOAT_EQ(verts[3 * 6 + 1].position.x, 1.F);
}

TEST(FormaPlotBarsTest, SpecDeclaresATriangleListAndItsCapacityArithmetic)
{
    const auto spec = Plot::Series {}.y(Role::SPATIAL_Y).as_bars().done();

    EXPECT_EQ(spec.topology, Portal::Graphics::PrimitiveTopology::TRIANGLE_LIST);
    EXPECT_EQ(spec.capacity_for(4), 4U * 6U * 60U);
}

// =============================================================================
// Filled waveform
// =============================================================================

TEST(FormaPlotFilledWaveformTest, EverySamplePairsASignalVertexWithABaselineVertex)
{
    auto container = make_container({ { Role::SPATIAL_Y, { 1.0, 0.0, -1.0 } } });
    const auto spec = Plot::Series {}.y(Role::SPATIAL_Y, fixed(-1.F, 1.F)).as_filled_waveform().done();

    const auto verts = run(spec, container);

    ASSERT_EQ(verts.size(), 6U);
    const std::array<float, 3> xs { -1.F, 0.F, 1.F };
    const std::array<float, 3> ys { 1.F, 0.F, -1.F };
    for (size_t i = 0; i < 3; ++i) {
        EXPECT_FLOAT_EQ(verts[2 * i].position.x, xs[i]);
        EXPECT_FLOAT_EQ(verts[2 * i].position.y, ys[i]);
        EXPECT_FLOAT_EQ(verts[2 * i].weight, 1.F);
        EXPECT_FLOAT_EQ(verts[2 * i + 1].position.x, xs[i]);
        EXPECT_FLOAT_EQ(verts[2 * i + 1].position.y, 0.F);
        EXPECT_FLOAT_EQ(verts[2 * i + 1].weight, 0.F);
    }
}

TEST(FormaPlotFilledWaveformTest, TheBaselineCanBeMovedInDataCoordinates)
{
    auto container = make_container({ { Role::SPATIAL_Y, { 1.0, 0.0 } } });
    const auto spec = Plot::Series {}
                          .y(Role::SPATIAL_Y, fixed(-1.F, 1.F))
                          .as_filled_waveform()
                          .baseline(-1.F)
                          .done();

    const auto verts = run(spec, container);

    EXPECT_FLOAT_EQ(verts[1].position.y, -1.F);
    EXPECT_FLOAT_EQ(verts[3].position.y, -1.F);
}

TEST(FormaPlotBoundsTest, EveryEncodingMapsItsGeometryIntoThePlotBounds)
{
    const AABB2D bounds { .min = { -0.5F, 0.25F }, .max = { 0.5F, 0.75F } };
    auto container = make_container({ { Role::SPATIAL_Y, { 1.0, 0.0, -1.0, 0.5 } } });
    auto base = [&] { return Plot::Series {}.y(Role::SPATIAL_Y, fixed(-1.F, 1.F)).bounds(bounds); };

    const std::array<Plot::SeriesSpec, 4> specs {
        base().as_waveform().done(),
        base().as_scatter().done(),
        base().as_bars().done(),
        base().as_filled_waveform().done(),
    };

    for (const auto& spec : specs) {
        const auto verts = run(spec, container);
        ASSERT_FALSE(verts.empty());
        for (const auto& vertex : verts) {
            EXPECT_GE(vertex.position.x, bounds.min.x - 1e-5F);
            EXPECT_LE(vertex.position.x, bounds.max.x + 1e-5F);
            EXPECT_GE(vertex.position.y, bounds.min.y - 1e-5F);
            EXPECT_LE(vertex.position.y, bounds.max.y + 1e-5F);
        }
    }
}

TEST(FormaPlotBoundsTest, TheBoundsCornersAreWhereTheNdcCornersWere)
{
    const AABB2D bounds { .min = { -0.5F, 0.25F }, .max = { 0.5F, 0.75F } };
    auto container = make_container({ { Role::SPATIAL_Y, { 1.0, 0.0, -1.0 } } });
    const auto spec = Plot::Series {}.y(Role::SPATIAL_Y, fixed(-1.F, 1.F)).bounds(bounds).as_filled_waveform().done();

    const auto verts = run(spec, container);

    ASSERT_EQ(verts.size(), 6U);
    EXPECT_FLOAT_EQ(verts[0].position.x, bounds.min.x);
    EXPECT_FLOAT_EQ(verts[0].position.y, bounds.max.y);
    EXPECT_FLOAT_EQ(verts[4].position.x, bounds.max.x);
    EXPECT_FLOAT_EQ(verts[4].position.y, bounds.min.y);
    EXPECT_FLOAT_EQ(verts[2].position.y, 0.5F);
}

TEST(FormaPlotFilledWaveformTest, SeriesAreStitchedByRepeatingTheLastVertexAndTheNextFirstVertex)
{
    auto container = make_container({ { Role::SPATIAL_Y, { 1.0, 0.0 } },
        { Role::SPATIAL_Y, { 0.0, 1.0 } } });
    const auto spec = Plot::Series {}.y(Role::SPATIAL_Y, fixed(-1.F, 1.F)).as_filled_waveform().done();

    const auto verts = run(spec, container);

    ASSERT_EQ(verts.size(), 4U + 2U + 4U);
    EXPECT_EQ(verts[4].position, verts[3].position);
    EXPECT_FLOAT_EQ(verts[4].weight, verts[3].weight);
    EXPECT_EQ(verts[4].color, verts[3].color);
    EXPECT_EQ(verts[5].position, verts[6].position);
    EXPECT_FLOAT_EQ(verts[5].weight, verts[6].weight);
    EXPECT_EQ(verts[5].color, verts[6].color);
}

TEST(FormaPlotFilledWaveformTest, StitchingSurvivesAManySeriesRebuild)
{
    std::vector<SeriesData> series;
    for (int i = 0; i < 12; ++i)
        series.push_back({ Role::SPATIAL_Y, std::vector<double>(64, static_cast<double>(i)) });
    auto container = make_container(series);
    const auto spec = Plot::Series {}.y(Role::SPATIAL_Y, fixed(-1.F, 12.F)).as_filled_waveform().done();

    const auto verts = run(spec, container);

    ASSERT_EQ(verts.size(), 12U * 128U + 22U);
    for (size_t s = 0; s + 1 < 12; ++s) {
        const size_t tail = (s + 1) * 128 + 2 * s;
        EXPECT_EQ(verts[tail].position, verts[tail - 1].position) << "series " << s;
        EXPECT_EQ(verts[tail + 1].position, verts[tail + 2].position) << "series " << s;
    }
}

TEST(FormaPlotFilledWaveformTest, SpecDeclaresATriangleStripAndItsCapacityArithmetic)
{
    const auto spec = Plot::Series {}.y(Role::SPATIAL_Y).as_filled_waveform().done();

    EXPECT_EQ(spec.topology, Portal::Graphics::PrimitiveTopology::TRIANGLE_STRIP);
    EXPECT_EQ(spec.capacity_for(10), 10U * 2U * 60U + 128U);
}

// =============================================================================
// Adornments carried by the spec
// =============================================================================

TEST(FormaPlotSeriesAdornmentTest, BoundsBackgroundLabelsAndLegendReachTheSpec)
{
    const auto spec = Plot::Series {}
                          .bounds(k_plot)
                          .background(k_plot, k_red)
                          .label("title", k_plot)
                          .legend({ 0.1F, 0.2F }, { { "a", k_green } })
                          .as_waveform()
                          .done();

    ASSERT_TRUE(spec.plot_bounds.has_value());
    EXPECT_EQ(spec.plot_bounds->min, k_plot.min);
    ASSERT_EQ(spec.labels.size(), 1U);
    EXPECT_EQ(spec.labels[0].text, "title");
    ASSERT_TRUE(spec.legend.has_value());
    EXPECT_EQ(spec.legend->origin, glm::vec2(0.1F, 0.2F));
    ASSERT_EQ(spec.legend->entries.size(), 1U);
    EXPECT_EQ(spec.legend->entries[0].label, "a");
    ASSERT_TRUE(spec.background_fn.has_value());
}

TEST(FormaPlotSeriesAdornmentTest, NoBackgroundCallMeansNoBackgroundFunction)
{
    const auto spec = Plot::Series {}.y(Role::SPATIAL_Y).as_bars().done();

    EXPECT_FALSE(spec.background_fn.has_value());
    EXPECT_FALSE(spec.plot_bounds.has_value());
    EXPECT_TRUE(spec.labels.empty());
    EXPECT_FALSE(spec.legend.has_value());
}

TEST(FormaPlotSeriesAdornmentTest, TheBackgroundFunctionDrawsTheSolidQuadInTheGivenColour)
{
    const auto spec = Plot::Series {}.background(k_plot, k_blue).as_scatter().done();
    std::vector<uint8_t> out;
    Portal::Forma::Element element;

    (*spec.background_fn)(0.F, out, element);

    const auto verts = decode(out);
    ASSERT_EQ(verts.size(), 4U);
    EXPECT_EQ(verts[0].color, k_blue);
    EXPECT_EQ(verts[3].position, glm::vec3(0.5F, 0.25F, 0.F));
}

TEST(FormaPlotSeriesAdornmentTest, EveryEncodingCarriesTheSameAdornments)
{
    const Plot::Series series = Plot::Series {}.bounds(k_plot).label("t", k_plot).background(k_plot);

    EXPECT_TRUE(series.as_waveform().done().plot_bounds.has_value());
    EXPECT_TRUE(series.as_scatter().done().plot_bounds.has_value());
    EXPECT_TRUE(series.as_bars().done().plot_bounds.has_value());
    EXPECT_TRUE(series.as_filled_waveform().done().plot_bounds.has_value());
    EXPECT_EQ(series.as_scatter().done().labels.size(), 1U);
    EXPECT_TRUE(series.as_bars().done().background_fn.has_value());
}

// =============================================================================
// Tick resolution
// =============================================================================

TEST(FormaPlotSeriesTickTest, TicksNeedPlotBoundsToResolve)
{
    const auto series = Plot::Series {}.x(Role::SPATIAL_X, fixed(0.F, 1.F)).x_ticks(3);

    EXPECT_TRUE(series.resolved_tick_labels().empty());
}

TEST(FormaPlotSeriesTickTest, XTicksTakeTheRangeOfTheXMapping)
{
    const auto series = Plot::Series {}
                            .bounds(k_plot)
                            .x(Role::SPATIAL_X, fixed(0.F, 10.F))
                            .x_ticks(3);

    const auto ticks = series.resolved_tick_labels();

    ASSERT_EQ(ticks.size(), 1U);
    EXPECT_FLOAT_EQ(ticks[0].range.min, 0.F);
    EXPECT_FLOAT_EQ(ticks[0].range.max, 10.F);
    EXPECT_EQ(ticks[0].count, 3U);
    EXPECT_EQ(ticks[0].edge, Plot::TickEdge::Bottom);
    EXPECT_EQ(ticks[0].name_prefix, "x_tick");
    EXPECT_EQ(ticks[0].plot_bounds.min, k_plot.min);
}

TEST(FormaPlotSeriesTickTest, YTicksDefaultToTheLeftEdgeAndMergeEveryYMapping)
{
    const auto series = Plot::Series {}
                            .bounds(k_plot)
                            .y(Role::SPATIAL_Y, fixed(0.F, 5.F))
                            .y(Role::FREQUENCY, fixed(-2.F, 3.F))
                            .y_ticks(4);

    const auto ticks = series.resolved_tick_labels();

    ASSERT_EQ(ticks.size(), 1U);
    EXPECT_FLOAT_EQ(ticks[0].range.min, -2.F);
    EXPECT_FLOAT_EQ(ticks[0].range.max, 5.F);
    EXPECT_EQ(ticks[0].edge, Plot::TickEdge::Left);
    EXPECT_EQ(ticks[0].name_prefix, "y_tick");
}

TEST(FormaPlotSeriesTickTest, AnExplicitRangeOverridesTheMapping)
{
    const auto series = Plot::Series {}
                            .bounds(k_plot)
                            .x(Role::SPATIAL_X, fixed(0.F, 1.F))
                            .x_ticks(fixed(100.F, 200.F), 2);

    const auto ticks = series.resolved_tick_labels();

    ASSERT_EQ(ticks.size(), 1U);
    EXPECT_FLOAT_EQ(ticks[0].range.min, 100.F);
    EXPECT_FLOAT_EQ(ticks[0].range.max, 200.F);
}

TEST(FormaPlotSeriesTickTest, ExplicitEdgeTicksCarryTheirOwnSettings)
{
    const auto series = Plot::Series {}
                            .bounds(k_plot)
                            .ticks(Plot::TickEdge::Top, fixed(1.F, 2.F), 4, 1);

    const auto ticks = series.resolved_tick_labels();

    ASSERT_EQ(ticks.size(), 1U);
    EXPECT_EQ(ticks[0].edge, Plot::TickEdge::Top);
    EXPECT_EQ(ticks[0].count, 4U);
    EXPECT_EQ(ticks[0].decimal_places, 1U);
    EXPECT_EQ(ticks[0].name_prefix, "tick");
}

TEST(FormaPlotSeriesTickTest, ARequestForAnAxisWithNoMappingFallsBackToTheDefaultRange)
{
    const auto series = Plot::Series {}.bounds(k_plot).x_ticks(2);

    const auto ticks = series.resolved_tick_labels();

    ASSERT_EQ(ticks.size(), 1U);
    EXPECT_FLOAT_EQ(ticks[0].range.min, -1.F);
    EXPECT_FLOAT_EQ(ticks[0].range.max, 1.F);
}

TEST(FormaPlotSeriesTickTest, SeveralRequestsResolveInOrder)
{
    const auto series = Plot::Series {}
                            .bounds(k_plot)
                            .x(Role::SPATIAL_X, fixed(0.F, 1.F))
                            .y(Role::SPATIAL_Y, fixed(0.F, 2.F))
                            .x_ticks(3)
                            .y_ticks(5);

    const auto ticks = series.resolved_tick_labels();

    ASSERT_EQ(ticks.size(), 2U);
    EXPECT_EQ(ticks[0].name_prefix, "x_tick");
    EXPECT_EQ(ticks[1].name_prefix, "y_tick");
    EXPECT_FLOAT_EQ(ticks[1].range.max, 2.F);
}

TEST(FormaPlotSeriesTickTest, AMergedAxisKeepsAutoScalingAndThePredicateOfAnyMapping)
{
    AxisRange automatic;
    automatic.range(2.F, 6.F).scale_if([] { return true; });
    const auto series = Plot::Series {}
                            .bounds(k_plot)
                            .x(Role::SPATIAL_X, fixed(0.F, 4.F))
                            .x(Role::FREQUENCY, automatic)
                            .x_ticks(2);

    const auto ticks = series.resolved_tick_labels();

    ASSERT_EQ(ticks.size(), 1U);
    EXPECT_FLOAT_EQ(ticks[0].range.min, 0.F);
    EXPECT_FLOAT_EQ(ticks[0].range.max, 6.F);
    EXPECT_TRUE(ticks[0].range.auto_scaling);
    EXPECT_TRUE(static_cast<bool>(ticks[0].range.scale_predicate));
}

}
