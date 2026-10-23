#include "../../test_config.h"

#include "MayaFlux/Portal/Forma/Primitives/FormFactory.hpp"

#include "MayaFlux/Kakshya/NDData/VertexFormats.hpp"

namespace MayaFlux::Test {

namespace Forma = Portal::Forma;
namespace Geo = Portal::Forma::Geometry;

using Kakshya::LineVertex;
using Kakshya::MeshVertex;
using Kakshya::PointVertex;
using Kinesis::AABB2D;
using Portal::Graphics::PrimitiveTopology;

namespace {

    constexpr float k_eps = 1e-5F;

    template <typename V>
    std::vector<V> decode(const std::vector<uint8_t>& bytes)
    {
        std::vector<V> out(bytes.size() / sizeof(V));
        std::memcpy(out.data(), bytes.data(), out.size() * sizeof(V));
        return out;
    }

    struct Rendered {
        std::vector<uint8_t> bytes;
        Forma::Element element;
    };

    template <typename T>
    Rendered render(const Geo::Form<T>& form, const T& value)
    {
        Rendered r;
        form.geometry(value, r.bytes, r.element);
        return r;
    }

    void expect_near(glm::vec2 a, glm::vec2 b)
    {
        EXPECT_NEAR(a.x, b.x, k_eps);
        EXPECT_NEAR(a.y, b.y, k_eps);
    }

    void expect_near(const AABB2D& a, const AABB2D& b)
    {
        expect_near(a.min, b.min);
        expect_near(a.max, b.max);
    }

    const AABB2D k_bounds { .min = { -0.5F, -0.2F }, .max = { 0.5F, 0.2F } };

}

// =============================================================================
// write_verts and at
// =============================================================================

TEST(FormaWriteVertsTest, VectorOfStructsIsCopiedByteForByte)
{
    const std::vector<PointVertex> verts {
        { .position = { 1.F, 2.F, 3.F } },
        { .position = { 4.F, 5.F, 6.F } },
    };
    std::vector<uint8_t> out(7, 0xFF);

    Geo::write_verts(out, verts);

    ASSERT_EQ(out.size(), 2 * sizeof(PointVertex));
    const auto back = decode<PointVertex>(out);
    EXPECT_EQ(back[0].position, glm::vec3(1.F, 2.F, 3.F));
    EXPECT_EQ(back[1].position, glm::vec3(4.F, 5.F, 6.F));
}

TEST(FormaWriteVertsTest, ArrayAndSingleStructOverloadsResizeExactly)
{
    const std::array<LineVertex, 3> arr {};
    std::vector<uint8_t> out;

    Geo::write_verts(out, arr);
    EXPECT_EQ(out.size(), 3 * sizeof(LineVertex));

    Geo::write_verts(out, PointVertex { .position = { 7.F, 8.F, 9.F } });
    ASSERT_EQ(out.size(), sizeof(PointVertex));
    EXPECT_EQ(decode<PointVertex>(out)[0].position, glm::vec3(7.F, 8.F, 9.F));
}

TEST(FormaWriteVertsTest, EmptyVectorClearsTheBuffer)
{
    std::vector<uint8_t> out(32, 1);
    Geo::write_verts(out, std::vector<MeshVertex> {});
    EXPECT_TRUE(out.empty());
}

TEST(FormaGeometryAtTest, ForwardsTheAnchorBoundsToTheFactory)
{
    Forma::Mapped<float> anchor;
    anchor.element.bounds_hint = k_bounds;

    const auto got = Geo::at(anchor, [](AABB2D b, float scale) {
        return AABB2D { .min = b.min * scale, .max = b.max * scale };
    },
        2.F);

    expect_near(got, AABB2D { .min = { -1.F, -0.4F }, .max = { 1.F, 0.4F } });
}

TEST(FormaGeometryAtTest, AnchorWithoutBoundsGivesAZeroRegion)
{
    Forma::Mapped<float> anchor;

    const auto got = Geo::at(anchor, [](AABB2D b) { return b; });

    expect_near(got, AABB2D { .min = { 0.F, 0.F }, .max = { 0.F, 0.F } });
}

// =============================================================================
// Form declarations: topology, capacity and wiring
// =============================================================================

TEST(FormaFormDeclarationTest, TopologyAndInteractionMatchEachFactory)
{
    const std::array<glm::vec2, 2> path { glm::vec2(0.F), glm::vec2(1.F, 0.F) };

    EXPECT_EQ(Geo::horizontal_fader(k_bounds, 0.1F).topology, PrimitiveTopology::TRIANGLE_STRIP);
    EXPECT_EQ(Geo::vertical_fader(k_bounds, 0.1F).topology, PrimitiveTopology::TRIANGLE_STRIP);
    EXPECT_EQ(Geo::radial(k_bounds, 0.F, 1.F).topology, PrimitiveTopology::LINE_LIST);
    EXPECT_EQ(Geo::point().topology, PrimitiveTopology::POINT_LIST);
    EXPECT_EQ(Geo::position_picker(k_bounds).topology, PrimitiveTopology::POINT_LIST);
    EXPECT_EQ(Geo::stroke_slider(path, nullptr).topology, PrimitiveTopology::LINE_LIST);
    EXPECT_EQ(Geo::toggle(k_bounds).topology, PrimitiveTopology::TRIANGLE_STRIP);
    EXPECT_EQ(Geo::level_meter(k_bounds).topology, PrimitiveTopology::TRIANGLE_STRIP);
    EXPECT_EQ(Geo::crosshair().topology, PrimitiveTopology::LINE_LIST);
    EXPECT_EQ(Geo::drawable_canvas(k_bounds).topology, PrimitiveTopology::LINE_LIST);
    EXPECT_EQ(Geo::scroll_indicator(k_bounds).topology, PrimitiveTopology::TRIANGLE_STRIP);
}

TEST(FormaFormDeclarationTest, OnlyInteractiveFormsCarryWiring)
{
    const std::array<glm::vec2, 2> path { glm::vec2(0.F), glm::vec2(1.F, 0.F) };

    EXPECT_TRUE(static_cast<bool>(Geo::horizontal_fader(k_bounds, 0.1F).wire));
    EXPECT_TRUE(static_cast<bool>(Geo::vertical_fader(k_bounds, 0.1F).wire));
    EXPECT_TRUE(static_cast<bool>(Geo::radial(k_bounds, 0.F, 1.F).wire));
    EXPECT_TRUE(static_cast<bool>(Geo::point().wire));
    EXPECT_TRUE(static_cast<bool>(Geo::position_picker(k_bounds).wire));
    EXPECT_TRUE(static_cast<bool>(Geo::stroke_slider(path, nullptr).wire));
    EXPECT_TRUE(static_cast<bool>(Geo::toggle(k_bounds).wire));
    EXPECT_TRUE(static_cast<bool>(Geo::crosshair().wire));
    EXPECT_TRUE(static_cast<bool>(Geo::drawable_canvas(k_bounds).wire));

    EXPECT_FALSE(static_cast<bool>(Geo::level_meter(k_bounds).wire));
    EXPECT_FALSE(static_cast<bool>(Geo::scroll_indicator(k_bounds).wire));
}

TEST(FormaFormDeclarationTest, BareGeometryFnAdoptsDefaults)
{
    Geo::Form<float> form(Forma::GeometryFn<float> { [](float, std::vector<uint8_t>&, Forma::Element&) { } });

    EXPECT_EQ(form.topology, PrimitiveTopology::TRIANGLE_STRIP);
    EXPECT_EQ(form.capacity, 4096U);
    EXPECT_FALSE(static_cast<bool>(form.wire));
}

// =============================================================================
// Faders
// =============================================================================

class FormaFaderValueTest : public ::testing::TestWithParam<float> { };

INSTANTIATE_TEST_SUITE_P(FaderValues, FormaFaderValueTest,
    ::testing::Values(0.F, 0.25F, 0.5F, 0.75F, 1.F));

TEST_P(FormaFaderValueTest, HorizontalHandleTravelsTrackMinusOneHandle)
{
    const float v = GetParam();
    const float handle_w = 0.1F;
    const auto form = Geo::horizontal_fader(k_bounds, handle_w);

    const Rendered r = render(form, v);

    ASSERT_TRUE(r.element.bounds_hint.has_value());
    const float expected_x = k_bounds.min.x + v * (k_bounds.width() - handle_w);
    expect_near(*r.element.bounds_hint,
        AABB2D { .min = { expected_x, k_bounds.min.y }, .max = { expected_x + handle_w, k_bounds.max.y } });
}

TEST_P(FormaFaderValueTest, HorizontalGeometryFitsDeclaredCapacity)
{
    const auto form = Geo::horizontal_fader(k_bounds, 0.1F);

    const Rendered r = render(form, GetParam());

    EXPECT_EQ(r.bytes.size(), 16 * sizeof(MeshVertex));
    EXPECT_LE(r.bytes.size(), form.capacity);
}

TEST_P(FormaFaderValueTest, HorizontalProjectionInvertsHandlePlacement)
{
    const float v = GetParam();
    const float handle_w = 0.1F;
    const auto form = Geo::horizontal_fader(k_bounds, handle_w);
    const auto project = Kinesis::axis_fraction(k_bounds, handle_w, true);

    const Rendered r = render(form, v);

    EXPECT_NEAR(project(r.element.bounds_hint->center()), v, 1e-4F);
}

TEST_P(FormaFaderValueTest, HorizontalTrackIsTheCentreBandOfTheBounds)
{
    const auto form = Geo::horizontal_fader(k_bounds, 0.1F, glm::vec3(0.3F), glm::vec3(0.9F));

    const auto verts = decode<MeshVertex>(render(form, GetParam()).bytes);

    ASSERT_EQ(verts.size(), 16U);
    EXPECT_NEAR(verts[0].position.x, k_bounds.min.x, k_eps);
    EXPECT_NEAR(verts[3].position.x, k_bounds.max.x, k_eps);
    EXPECT_NEAR(verts[0].position.y, k_bounds.min.y + 0.35F * k_bounds.height(), k_eps);
    EXPECT_NEAR(verts[3].position.y, k_bounds.min.y + 0.65F * k_bounds.height(), k_eps);
    EXPECT_EQ(verts[0].color, glm::vec3(0.3F));
    EXPECT_EQ(verts[12].color, glm::vec3(0.9F));
}

TEST_P(FormaFaderValueTest, VerticalHandleTravelsTrackMinusOneHandle)
{
    const float v = GetParam();
    const float handle_h = 0.05F;
    const auto form = Geo::vertical_fader(k_bounds, handle_h);

    const Rendered r = render(form, v);

    ASSERT_TRUE(r.element.bounds_hint.has_value());
    const float expected_y = k_bounds.min.y + v * (k_bounds.height() - handle_h);
    expect_near(*r.element.bounds_hint,
        AABB2D { .min = { k_bounds.min.x, expected_y }, .max = { k_bounds.max.x, expected_y + handle_h } });
}

TEST_P(FormaFaderValueTest, VerticalProjectionInvertsHandlePlacement)
{
    const float v = GetParam();
    const float handle_h = 0.05F;
    const auto form = Geo::vertical_fader(k_bounds, handle_h);
    const auto project = Kinesis::axis_fraction(k_bounds, handle_h, false);

    const Rendered r = render(form, v);

    EXPECT_NEAR(project(r.element.bounds_hint->center()), v, 1e-4F);
    EXPECT_EQ(r.bytes.size(), 16 * sizeof(MeshVertex));
    EXPECT_LE(r.bytes.size(), form.capacity);
}

TEST(FormaFaderTest, LiveOffsetShiftsGeometryAndHitBounds)
{
    const glm::vec2 shift { 0.3F, -0.1F };
    const auto still = Geo::horizontal_fader(k_bounds, 0.1F);
    const auto moved = Geo::horizontal_fader(k_bounds, 0.1F, glm::vec3(0.3F), glm::vec3(0.9F), [shift] { return shift; });

    const Rendered a = render(still, 0.5F);
    const Rendered b = render(moved, 0.5F);

    expect_near(*b.element.bounds_hint, a.element.bounds_hint->translated(shift));
    const auto va = decode<MeshVertex>(a.bytes);
    const auto vb = decode<MeshVertex>(b.bytes);
    ASSERT_EQ(va.size(), vb.size());
    for (size_t i = 0; i < va.size(); ++i)
        expect_near(glm::vec2(vb[i].position), glm::vec2(va[i].position) + shift);
}

TEST(FormaFaderTest, ValuesOutsideUnitRangeAreNotClampedByTheGeometry)
{
    const auto form = Geo::horizontal_fader(k_bounds, 0.1F);

    const Rendered over = render(form, 2.F);

    EXPECT_GT(over.element.bounds_hint->max.x, k_bounds.max.x);
}

TEST(FormaFaderTest, HandleHitRegionDefersToItsBoundsHint)
{
    const auto form = Geo::horizontal_fader(k_bounds, 0.1F);
    Forma::Element el;
    el.contains = [](glm::vec2) { return true; };
    std::vector<uint8_t> bytes;

    form.geometry(0.5F, bytes, el);

    EXPECT_FALSE(static_cast<bool>(el.contains));
}

// =============================================================================
// Radial
// =============================================================================

class FormaRadialValueTest : public ::testing::TestWithParam<float> { };

INSTANTIATE_TEST_SUITE_P(RadialValues, FormaRadialValueTest,
    ::testing::Values(0.F, 0.25F, 0.5F, 0.75F, 1.F));

TEST_P(FormaRadialValueTest, IndicatorRunsFromCentreToTheSweptAngle)
{
    const float v = GetParam();
    const float start = -2.F;
    const float end = 2.F;
    const auto form = Geo::radial(k_bounds, start, end);
    const glm::vec2 centre = k_bounds.center();
    const float radius = std::min(k_bounds.width(), k_bounds.height()) * 0.5F;

    const auto verts = decode<LineVertex>(render(form, v).bytes);

    ASSERT_EQ(verts.size(), 2U);
    expect_near(glm::vec2(verts[0].position), centre);
    const float angle = start + v * (end - start);
    expect_near(glm::vec2(verts[1].position), centre + radius * glm::vec2(std::cos(angle), std::sin(angle)));
}

TEST_P(FormaRadialValueTest, ProjectionInvertsTheIndicatorAngle)
{
    const float v = GetParam();
    const auto form = Geo::radial(k_bounds, -2.F, 2.F);
    const auto project = Kinesis::angle_fraction(k_bounds, -2.F, 2.F);

    const auto verts = decode<LineVertex>(render(form, v).bytes);

    EXPECT_NEAR(project(glm::vec2(verts[1].position)), v, 1e-4F);
}

TEST(FormaRadialTest, HitRegionIsTheCircleAroundTheRegionCentre)
{
    const auto form = Geo::radial(k_bounds, 0.F, 1.F);
    const float radius = std::min(k_bounds.width(), k_bounds.height()) * 0.5F;

    const Rendered r = render(form, 0.5F);

    ASSERT_TRUE(r.element.contains);
    ASSERT_TRUE(r.element.bounds_hint.has_value());
    expect_near(*r.element.bounds_hint, AABB2D::from_ndc(k_bounds.center(), glm::vec2(radius)));
    EXPECT_TRUE(r.element.contains(k_bounds.center()));
    EXPECT_TRUE(r.element.contains(k_bounds.center() + glm::vec2(0.9F * radius, 0.F)));
    EXPECT_FALSE(r.element.contains(k_bounds.center() + glm::vec2(1.1F * radius, 0.F)));
}

TEST(FormaRadialTest, GeometryMatchesDeclaredCapacityExactly)
{
    const auto form = Geo::radial(k_bounds, 0.F, 1.F);

    EXPECT_EQ(render(form, 0.3F).bytes.size(), form.capacity);
}

TEST(FormaRadialTest, DescendingSweepMovesTheOtherWay)
{
    const auto form = Geo::radial(k_bounds, 1.F, -1.F);
    const auto project = Kinesis::angle_fraction(k_bounds, 1.F, -1.F);

    const auto verts = decode<LineVertex>(render(form, 0.75F).bytes);

    EXPECT_NEAR(project(glm::vec2(verts[1].position)), 0.75F, 1e-4F);
}

// =============================================================================
// Point, position picker, crosshair
// =============================================================================

TEST(FormaPointTest, VertexSitsAtTheValueWithItsDeclaredSize)
{
    const auto form = Geo::point(glm::vec3(0.1F, 0.2F, 0.3F), 12.F, 0.08F);

    const Rendered r = render(form, glm::vec2(0.25F, -0.5F));

    const auto verts = decode<PointVertex>(r.bytes);
    ASSERT_EQ(verts.size(), 1U);
    EXPECT_EQ(verts[0].position, glm::vec3(0.25F, -0.5F, 0.F));
    EXPECT_EQ(verts[0].color, glm::vec3(0.1F, 0.2F, 0.3F));
    EXPECT_FLOAT_EQ(verts[0].size, 12.F);
    EXPECT_EQ(r.bytes.size(), form.capacity);
}

TEST(FormaPointTest, HitRegionIsACircleOfTheHitRadius)
{
    const auto form = Geo::point(glm::vec3(1.F), 10.F, 0.08F);
    const glm::vec2 pos { 0.25F, -0.5F };

    const Rendered r = render(form, pos);

    expect_near(*r.element.bounds_hint, AABB2D::from_ndc(pos, glm::vec2(0.08F)));
    EXPECT_TRUE(r.element.contains(pos));
    EXPECT_TRUE(r.element.contains(pos + glm::vec2(0.07F, 0.F)));
    EXPECT_FALSE(r.element.contains(pos + glm::vec2(0.09F, 0.F)));
    EXPECT_FALSE(r.element.contains(pos + glm::vec2(0.06F, 0.06F)));
}

TEST(FormaPositionPickerTest, UnitSquareValueMapsOntoTheBounds)
{
    const auto form = Geo::position_picker(k_bounds, glm::vec3(0.5F), 6.F);

    const Rendered r = render(form, glm::vec2(0.25F, 0.75F));

    const auto verts = decode<PointVertex>(r.bytes);
    ASSERT_EQ(verts.size(), 1U);
    expect_near(glm::vec2(verts[0].position),
        { k_bounds.min.x + 0.25F * k_bounds.width(), k_bounds.min.y + 0.75F * k_bounds.height() });
    EXPECT_FLOAT_EQ(verts[0].size, 6.F);
    EXPECT_EQ(r.bytes.size(), form.capacity);
}

TEST(FormaPositionPickerTest, WholeRegionIsTheHitAreaAndProjectionInvertsPlacement)
{
    const auto form = Geo::position_picker(k_bounds);
    const auto project = Kinesis::unit_square(k_bounds);
    const glm::vec2 v { 0.2F, 0.9F };

    const Rendered r = render(form, v);

    expect_near(*r.element.bounds_hint, k_bounds);
    ASSERT_TRUE(r.element.contains);
    EXPECT_TRUE(r.element.contains(k_bounds.center()));
    EXPECT_TRUE(r.element.contains(k_bounds.min + glm::vec2(0.01F)));
    EXPECT_FALSE(r.element.contains(k_bounds.max + glm::vec2(0.01F)));
    const auto verts = decode<PointVertex>(r.bytes);
    expect_near(project(glm::vec2(verts[0].position)), v);
}

TEST(FormaPositionPickerTest, LiveOffsetShiftsPlacementAndHitArea)
{
    const glm::vec2 shift { -0.2F, 0.1F };
    const auto form = Geo::position_picker(k_bounds, glm::vec3(1.F), 8.F, [shift] { return shift; });

    const Rendered r = render(form, glm::vec2(0.5F));

    expect_near(*r.element.bounds_hint, k_bounds.translated(shift));
    expect_near(glm::vec2(decode<PointVertex>(r.bytes)[0].position), k_bounds.center() + shift);
    EXPECT_TRUE(r.element.contains(k_bounds.center() + shift));
    EXPECT_FALSE(r.element.contains(k_bounds.max + shift + glm::vec2(0.05F)));
}

TEST(FormaCrosshairTest, FourArmsCentredOnTheValue)
{
    const auto form = Geo::crosshair(0.1F, glm::vec3(0.4F), 3.F, 0.06F);
    const glm::vec2 pos { -0.3F, 0.2F };

    const Rendered r = render(form, pos);

    const auto verts = decode<LineVertex>(r.bytes);
    ASSERT_EQ(verts.size(), 4U);
    EXPECT_EQ(verts[0].position, glm::vec3(pos.x - 0.1F, pos.y, 0.F));
    EXPECT_EQ(verts[1].position, glm::vec3(pos.x + 0.1F, pos.y, 0.F));
    EXPECT_EQ(verts[2].position, glm::vec3(pos.x, pos.y - 0.1F, 0.F));
    EXPECT_EQ(verts[3].position, glm::vec3(pos.x, pos.y + 0.1F, 0.F));
    for (const auto& v : verts) {
        EXPECT_EQ(v.color, glm::vec3(0.4F));
        EXPECT_FLOAT_EQ(v.thickness, 3.F);
    }
    EXPECT_EQ(r.bytes.size(), form.capacity);
}

TEST(FormaCrosshairTest, BoundsFollowArmLengthWhileHitsFollowHitRadius)
{
    const auto form = Geo::crosshair(0.1F, glm::vec3(1.F), 1.F, 0.03F);
    const glm::vec2 pos { 0.F, 0.F };

    const Rendered r = render(form, pos);

    expect_near(*r.element.bounds_hint, AABB2D::from_ndc(pos, glm::vec2(0.1F)));
    EXPECT_TRUE(r.element.contains({ 0.02F, 0.F }));
    EXPECT_FALSE(r.element.contains({ 0.05F, 0.F }));
}

// =============================================================================
// Stroke slider
// =============================================================================

namespace {

    const std::array<glm::vec2, 3> k_path { glm::vec2(0.F, 0.F), glm::vec2(1.F, 0.F), glm::vec2(1.F, 1.F) };

    struct StrokeCase {
        float value;
        size_t vertex_count;
        glm::vec2 last;
    };

}

class FormaStrokeSliderValueTest : public ::testing::TestWithParam<StrokeCase> { };

INSTANTIATE_TEST_SUITE_P(StrokeValues, FormaStrokeSliderValueTest,
    ::testing::Values(
        StrokeCase { 0.F, 4U, { 1.F, 1.F } },
        StrokeCase { 0.5F, 6U, { 1.F, 0.F } },
        StrokeCase { 0.75F, 8U, { 1.F, 0.5F } },
        StrokeCase { 1.F, 8U, { 1.F, 1.F } }));

TEST_P(FormaStrokeSliderValueTest, FillRunsFromThePathStartToTheHandle)
{
    const StrokeCase c = GetParam();
    const auto form = Geo::stroke_slider(k_path, nullptr, 0.05F);

    const auto verts = decode<LineVertex>(render(form, c.value).bytes);

    ASSERT_EQ(verts.size(), c.vertex_count);
    if (c.value > 0.F)
        expect_near(glm::vec2(verts.back().position), c.last);
}

TEST_P(FormaStrokeSliderValueTest, GeometryFitsDeclaredCapacity)
{
    const auto form = Geo::stroke_slider(k_path, nullptr, 0.05F);

    EXPECT_LE(render(form, GetParam().value).bytes.size(), form.capacity);
}

TEST_P(FormaStrokeSliderValueTest, ProjectionInvertsTheHandlePosition)
{
    const StrokeCase c = GetParam();
    if (c.value <= 0.F)
        GTEST_SKIP() << "the handle is not part of the output at value 0";
    const auto form = Geo::stroke_slider(k_path, nullptr, 0.05F);
    const auto project = Kinesis::path_fraction(k_path);

    const auto verts = decode<LineVertex>(render(form, c.value).bytes);

    EXPECT_NEAR(project(glm::vec2(verts.back().position)), c.value, 1e-4F);
}

TEST(FormaStrokeSliderTest, HitRegionIsThePathWithItsHalfThickness)
{
    const auto form = Geo::stroke_slider(k_path, nullptr, 0.05F);

    const Rendered r = render(form, 0.5F);

    expect_near(*r.element.bounds_hint,
        AABB2D { .min = { -0.05F, -0.05F }, .max = { 1.05F, 1.05F } });
    ASSERT_TRUE(r.element.contains);
    EXPECT_TRUE(r.element.contains({ 0.5F, 0.04F }));
    EXPECT_FALSE(r.element.contains({ 0.5F, 0.2F }));
    EXPECT_TRUE(r.element.contains({ 1.04F, 0.5F }));
}

TEST(FormaStrokeSliderTest, ValueIsClampedToTheUnitRange)
{
    const auto form = Geo::stroke_slider(k_path, nullptr, 0.05F);

    const auto over = decode<LineVertex>(render(form, 5.F).bytes);
    const auto at_one = decode<LineVertex>(render(form, 1.F).bytes);
    const auto under = decode<LineVertex>(render(form, -3.F).bytes);
    const auto at_zero = decode<LineVertex>(render(form, 0.F).bytes);

    EXPECT_EQ(over.size(), at_one.size());
    EXPECT_EQ(under.size(), at_zero.size());
}

TEST(FormaStrokeSliderTest, FewerThanTwoPointsProducesNoGeometry)
{
    const std::array<glm::vec2, 1> single { glm::vec2(0.5F) };
    const auto form = Geo::stroke_slider(single, nullptr);
    Rendered r;
    r.bytes.assign(64, 0xAB);

    form.geometry(0.5F, r.bytes, r.element);

    EXPECT_TRUE(r.bytes.empty());
}

TEST(FormaStrokeSliderTest, LiveOffsetTranslatesEveryVertexAndTheBounds)
{
    const glm::vec2 shift { 0.2F, -0.3F };
    const auto still = Geo::stroke_slider(k_path, nullptr, 0.05F);
    const auto moved = Geo::stroke_slider(k_path, nullptr, 0.05F, glm::vec3(0.3F), glm::vec3(0.2F, 0.6F, 1.F), glm::vec3(0.95F), 10.F, [shift] { return shift; });

    const Rendered a = render(still, 0.75F);
    const Rendered b = render(moved, 0.75F);

    const auto va = decode<LineVertex>(a.bytes);
    const auto vb = decode<LineVertex>(b.bytes);
    ASSERT_EQ(va.size(), vb.size());
    for (size_t i = 0; i < va.size(); ++i)
        expect_near(glm::vec2(vb[i].position), glm::vec2(va[i].position) + shift);
    expect_near(*b.element.bounds_hint, a.element.bounds_hint->translated(shift));
}

// =============================================================================
// Toggle and level meter
// =============================================================================

TEST(FormaToggleTest, ColourFollowsTheBoolAndTheQuadFillsTheRegion)
{
    const auto form = Geo::toggle(k_bounds, glm::vec3(0.1F), glm::vec3(0.8F, 0.2F, 0.2F));

    const Rendered off = render(form, false);
    const Rendered on = render(form, true);

    const auto voff = decode<MeshVertex>(off.bytes);
    const auto von = decode<MeshVertex>(on.bytes);
    ASSERT_EQ(voff.size(), 4U);
    ASSERT_EQ(von.size(), 4U);
    for (const auto& v : voff)
        EXPECT_EQ(v.color, glm::vec3(0.1F));
    for (const auto& v : von)
        EXPECT_EQ(v.color, glm::vec3(0.8F, 0.2F, 0.2F));
    expect_near(glm::vec2(voff[0].position), k_bounds.min);
    expect_near(glm::vec2(voff[3].position), k_bounds.max);
    expect_near(*on.element.bounds_hint, k_bounds);
    EXPECT_EQ(on.bytes.size(), form.capacity);
}

TEST(FormaToggleTest, LiveOffsetShiftsTheQuad)
{
    const glm::vec2 shift { 0.1F, 0.1F };
    const auto form = Geo::toggle(k_bounds, glm::vec3(0.1F), glm::vec3(0.9F), [shift] { return shift; });

    const Rendered r = render(form, true);

    expect_near(*r.element.bounds_hint, k_bounds.translated(shift));
    expect_near(glm::vec2(decode<MeshVertex>(r.bytes)[0].position), k_bounds.min + shift);
}

class FormaLevelMeterValueTest : public ::testing::TestWithParam<float> { };

INSTANTIATE_TEST_SUITE_P(MeterValues, FormaLevelMeterValueTest,
    ::testing::Values(-1.F, 0.F, 0.25F, 1.F, 3.F));

TEST_P(FormaLevelMeterValueTest, HorizontalSplitIsTheClampedFraction)
{
    const float v = GetParam();
    const float t = std::clamp(v, 0.F, 1.F);
    const auto form = Geo::level_meter(k_bounds, true, glm::vec3(0.2F, 0.7F, 0.3F), glm::vec3(0.15F));

    const Rendered r = render(form, v);

    const auto verts = decode<MeshVertex>(r.bytes);
    ASSERT_EQ(verts.size(), 8U);
    const float split = k_bounds.min.x + t * k_bounds.width();
    EXPECT_NEAR(verts[3].position.x, split, k_eps);
    EXPECT_NEAR(verts[4].position.x, split, k_eps);
    EXPECT_NEAR(verts[7].position.x, k_bounds.max.x, k_eps);
    EXPECT_EQ(verts[0].color, glm::vec3(0.2F, 0.7F, 0.3F));
    EXPECT_EQ(verts[4].color, glm::vec3(0.15F));
    expect_near(*r.element.bounds_hint, k_bounds);
    EXPECT_EQ(r.bytes.size(), form.capacity);
}

TEST_P(FormaLevelMeterValueTest, VerticalSplitIsTheClampedFraction)
{
    const float v = GetParam();
    const float t = std::clamp(v, 0.F, 1.F);
    const auto form = Geo::level_meter(k_bounds, false);

    const auto verts = decode<MeshVertex>(render(form, v).bytes);

    ASSERT_EQ(verts.size(), 8U);
    const float split = k_bounds.min.y + t * k_bounds.height();
    EXPECT_NEAR(verts[3].position.y, split, k_eps);
    EXPECT_NEAR(verts[4].position.y, split, k_eps);
    EXPECT_NEAR(verts[7].position.y, k_bounds.max.y, k_eps);
}

// =============================================================================
// Drawable canvas
// =============================================================================

TEST(FormaDrawableCanvasTest, SamplesSpanTheBoundsAsLineSegments)
{
    const auto form = Geo::drawable_canvas(k_bounds, glm::vec3(0.8F), 2.F);
    const std::vector<float> samples { 0.F, 0.5F, 1.F, 0.25F, 0.75F };

    const Rendered r = render(form, samples);

    const auto verts = decode<LineVertex>(r.bytes);
    ASSERT_EQ(verts.size(), 2 * (samples.size() - 1));
    const float step = k_bounds.width() / static_cast<float>(samples.size() - 1);
    for (size_t i = 0; i + 1 < samples.size(); ++i) {
        EXPECT_NEAR(verts[2 * i].position.x, k_bounds.min.x + static_cast<float>(i) * step, k_eps);
        EXPECT_NEAR(verts[2 * i + 1].position.x, k_bounds.min.x + static_cast<float>(i + 1) * step, k_eps);
        EXPECT_NEAR(verts[2 * i].position.y, k_bounds.min.y + samples[i] * k_bounds.height(), k_eps);
        EXPECT_NEAR(verts[2 * i + 1].position.y, k_bounds.min.y + samples[i + 1] * k_bounds.height(), k_eps);
        EXPECT_FLOAT_EQ(verts[2 * i].thickness, 2.F);
    }
}

TEST(FormaDrawableCanvasTest, SamplesOutsideTheUnitRangeAreClampedOnTheYAxis)
{
    const auto form = Geo::drawable_canvas(k_bounds);

    const auto verts = decode<LineVertex>(render(form, std::vector<float> { -4.F, 9.F }).bytes);

    ASSERT_EQ(verts.size(), 2U);
    EXPECT_NEAR(verts[0].position.y, k_bounds.min.y, k_eps);
    EXPECT_NEAR(verts[1].position.y, k_bounds.max.y, k_eps);
}

TEST(FormaDrawableCanvasTest, TooFewSamplesClearOutputButKeepTheHitArea)
{
    const auto form = Geo::drawable_canvas(k_bounds);

    for (const std::vector<float>& samples : { std::vector<float> {}, std::vector<float> { 0.5F } }) {
        Rendered r;
        r.bytes.assign(32, 1);
        form.geometry(samples, r.bytes, r.element);

        EXPECT_TRUE(r.bytes.empty());
        expect_near(*r.element.bounds_hint, k_bounds);
        ASSERT_TRUE(r.element.contains);
        EXPECT_TRUE(r.element.contains(k_bounds.center()));
        EXPECT_FALSE(r.element.contains(k_bounds.max + glm::vec2(0.1F)));
    }
}

TEST(FormaDrawableCanvasTest, ModestSampleCountFitsDeclaredCapacity)
{
    const auto form = Geo::drawable_canvas(k_bounds);

    const Rendered r = render(form, std::vector<float>(16, 0.5F));

    EXPECT_LE(r.bytes.size(), form.capacity);
}

TEST(FormaDrawableCanvasTest, CapacityCoversAFullResolutionCanvas)
{
    const auto form = Geo::drawable_canvas(k_bounds, glm::vec3(0.8F), 1.5F, {}, 128);

    const Rendered r = render(form, std::vector<float>(128, 0.5F));

    EXPECT_EQ(r.bytes.size(), form.capacity);
}

TEST(FormaDrawableCanvasTest, ZeroSamplesKeepsTheDefaultCapacity)
{
    EXPECT_EQ(Geo::drawable_canvas(k_bounds).capacity, 4096U);
    EXPECT_EQ(Geo::drawable_canvas(k_bounds, glm::vec3(0.8F), 1.5F, {}, 1).capacity, 4096U);
}

// =============================================================================
// Scroll indicator
// =============================================================================

TEST(FormaScrollIndicatorTest, BarLengthIsTheExtentFractionOfTheTrack)
{
    const AABB2D track { .min = { 0.9F, -1.F }, .max = { 1.F, 1.F } };
    const auto form = Geo::scroll_indicator(track);

    const Rendered r = render(form, glm::vec2(0.F, 0.25F));

    const auto verts = decode<MeshVertex>(r.bytes);
    ASSERT_EQ(verts.size(), 4U);
    EXPECT_NEAR(verts[3].position.y - verts[0].position.y, 0.5F, k_eps);
    EXPECT_EQ(r.bytes.size(), form.capacity);
    expect_near(*r.element.bounds_hint, track);
}

TEST(FormaScrollIndicatorTest, PositionZeroPinsTheBarToTheTopAndOneToTheBottom)
{
    const AABB2D track { .min = { 0.9F, -1.F }, .max = { 1.F, 1.F } };
    const auto form = Geo::scroll_indicator(track);

    const auto top = decode<MeshVertex>(render(form, glm::vec2(0.F, 0.5F)).bytes);
    const auto bottom = decode<MeshVertex>(render(form, glm::vec2(1.F, 0.5F)).bytes);

    EXPECT_NEAR(top[3].position.y, track.max.y, k_eps);
    EXPECT_NEAR(bottom[0].position.y, track.min.y, k_eps);
}

TEST(FormaScrollIndicatorTest, OutOfRangeInputsStayInsideTheTrack)
{
    const AABB2D track { .min = { 0.9F, -1.F }, .max = { 1.F, 1.F } };
    const auto form = Geo::scroll_indicator(track);

    for (const glm::vec2 extent : { glm::vec2(-2.F, -2.F), glm::vec2(5.F, 5.F), glm::vec2(0.5F, 9.F) }) {
        const auto verts = decode<MeshVertex>(render(form, extent).bytes);
        EXPECT_GE(verts[0].position.y, track.min.y - k_eps);
        EXPECT_LE(verts[3].position.y, track.max.y + k_eps);
        EXPECT_NEAR(verts[0].position.x, track.min.x, k_eps);
        EXPECT_NEAR(verts[3].position.x, track.max.x, k_eps);
    }
}

}
