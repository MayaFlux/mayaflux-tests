#include "../../support/forma_rig.hpp"

#include "MayaFlux/Kinesis/ColorSpace.hpp"
#include "MayaFlux/Portal/Forma/Forma.hpp"
#include "MayaFlux/Portal/Forma/Primitives/ColorPicker.hpp"

namespace MayaFlux::Test {

namespace Forma = Portal::Forma;

namespace {

    void expect_near(glm::vec3 actual, glm::vec3 expected, float tolerance = 1e-4F)
    {
        EXPECT_NEAR(actual.x, expected.x, tolerance);
        EXPECT_NEAR(actual.y, expected.y, tolerance);
        EXPECT_NEAR(actual.z, expected.z, tolerance);
    }

}

class FormaColorPickerTest : public FormaRigTest {
protected:
    void SetUp() override
    {
        FormaRigTest::SetUp();
        if (IsSkipped())
            return;
        surface.emplace(make_surface("color-picker"));
    }

    void TearDown() override
    {
        surface.reset();
        FormaRigTest::TearDown();
    }

    Forma::ColorPicker place_picker(glm::vec3 initial = { 1.F, 0.F, 0.F }, std::function<void(glm::vec3)> on_change = {})
    {
        return Forma::ColorPicker {}
            .initial(initial)
            .on_change(std::move(on_change))
            .place(*surface, cursor, -1.F, 1.F, 0.8F);
    }

    void click(glm::vec2 ndc)
    {
        window->move_to_ndc(ndc);
        window->press(IO::MouseButtons::Left);
        window->release(IO::MouseButtons::Left);
    }

    static glm::vec2 square_point(float saturation, float value)
    {
        return { -1.F + saturation * 1.24F, 0.2F + value * 0.8F };
    }

    static glm::vec2 hue_point(float hue)
    {
        return { 0.44F, 0.2F + hue * 0.8F };
    }

    std::optional<Forma::Surface> surface;
    Forma::LayoutCursor cursor;
};

// =============================================================================
// Placement
// =============================================================================

TEST_F(FormaColorPickerTest, PlaceRegistersThePickerAndAdvancesTheCursor)
{
    auto picker = place_picker();

    ASSERT_NE(picker.picker_id, 0U);
    EXPECT_NE(surface->layer().get(picker.picker_id), nullptr);
    EXPECT_NEAR(picker.bounds().min.y, 0.2F, 1e-5F);
    EXPECT_NEAR(picker.bounds().max.y, 1.F, 1e-5F);
    EXPECT_NEAR(cursor.y(), 0.2F, 1e-6F);
    EXPECT_NEAR(picker.cursor_out.y(), 0.2F, 1e-6F);
}

TEST_F(FormaColorPickerTest, TheInitialColourIsReportedAsRgbAndHsv)
{
    auto picker = place_picker({ 0.2F, 0.6F, 0.4F });

    expect_near(picker.color->value, { 0.2F, 0.6F, 0.4F });
    expect_near(picker.hsv->value, Kinesis::rgb_to_hsv({ 0.2F, 0.6F, 0.4F }));
}

// =============================================================================
// Picking
// =============================================================================

TEST_F(FormaColorPickerTest, PressingTheSquareSetsSaturationAndValueAndKeepsTheHue)
{
    auto picker = place_picker({ 0.F, 1.F, 0.F });
    const float hue = picker.hsv->value.x;

    click(square_point(0.5F, 0.25F));

    EXPECT_NEAR(picker.hsv->value.x, hue, 1e-5F);
    EXPECT_NEAR(picker.hsv->value.y, 0.5F, 1e-2F);
    EXPECT_NEAR(picker.hsv->value.z, 0.25F, 1e-2F);
}

TEST_F(FormaColorPickerTest, PressingTheHueStripSetsTheHueAndKeepsSaturationAndValue)
{
    auto picker = place_picker({ 0.5F, 0.2F, 0.2F });
    const glm::vec3 before = picker.hsv->value;

    click(hue_point(0.25F));

    EXPECT_NEAR(picker.hsv->value.x, 0.25F, 1e-2F);
    EXPECT_NEAR(picker.hsv->value.y, before.y, 1e-5F);
    EXPECT_NEAR(picker.hsv->value.z, before.z, 1e-5F);
}

TEST_F(FormaColorPickerTest, ColorAlwaysEqualsTheRgbOfHsv)
{
    auto picker = place_picker();

    click(hue_point(0.6F));
    click(square_point(0.3F, 0.9F));

    expect_near(picker.color->value, Kinesis::hsv_to_rgb(picker.hsv->value));
}

TEST_F(FormaColorPickerTest, TheHueSurvivesPickingAGrey)
{
    auto picker = place_picker();
    click(hue_point(0.7F));
    const float hue = picker.hsv->value.x;

    click(square_point(0.F, 0.5F));

    EXPECT_NEAR(picker.hsv->value.y, 0.F, 1e-2F);
    EXPECT_NEAR(picker.hsv->value.x, hue, 1e-5F);
}

TEST_F(FormaColorPickerTest, DraggingPastTheSquareClampsToItsEdge)
{
    auto picker = place_picker();

    window->move_to_ndc(square_point(0.5F, 0.5F));
    window->press(IO::MouseButtons::Left);
    window->move_to_ndc({ 0.9F, 0.9F });
    window->release(IO::MouseButtons::Left);

    EXPECT_NEAR(picker.hsv->value.y, 1.F, 1e-5F);
    EXPECT_NEAR(picker.hsv->value.z, 0.875F, 1e-2F);
}

TEST_F(FormaColorPickerTest, OnChangeReceivesTheRgbOfEachPick)
{
    std::vector<glm::vec3> seen;
    auto picker = place_picker({ 1.F, 0.F, 0.F }, [&](glm::vec3 rgb) { seen.push_back(rgb); });

    click(square_point(0.F, 1.F));
    click(square_point(0.F, 0.F));

    ASSERT_GE(seen.size(), 2U);
    expect_near(seen.front(), { 1.F, 1.F, 1.F }, 1e-2F);
    expect_near(seen.back(), { 0.F, 0.F, 0.F }, 1e-2F);
}

// =============================================================================
// Bridge and teardown
// =============================================================================

TEST_F(FormaColorPickerTest, TheColourReachesABridgeSinkAsThreeFloats)
{
    auto picker = place_picker();
    std::vector<float> seen;
    Forma::bridge().at(picker.color).write(std::function<void(std::span<const float>)>([&](std::span<const float> values) {
        seen.assign(values.begin(), values.end());
    }));
    click(square_point(1.F, 1.F));

    frames.tick();

    ASSERT_EQ(seen.size(), 3U);
    expect_near({ seen[0], seen[1], seen[2] }, picker.color->value);
}

TEST_F(FormaColorPickerTest, DestroyingThePickerRemovesItsElements)
{
    auto picker = place_picker();
    ASSERT_GT(surface->layer().size(), 1U);

    Forma::destroy(*surface, picker.picker_id);

    EXPECT_EQ(surface->layer().size(), 0U);
}

}
