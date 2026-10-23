#include "../../support/forma_rig.hpp"

#include "MayaFlux/Portal/Forma/Forma.hpp"
#include "MayaFlux/Portal/Forma/Primitives/Dropdown.hpp"

namespace MayaFlux::Test {

namespace Forma = Portal::Forma;
using Kinesis::AABB2D;

class FormaDropdownTest : public FormaRigTest {
protected:
    void SetUp() override
    {
        FormaRigTest::SetUp();
        if (IsSkipped())
            return;
        surface.emplace(make_surface("dropdown"));
    }

    void TearDown() override
    {
        surface.reset();
        FormaRigTest::TearDown();
    }

    static std::vector<std::string> labels(size_t count)
    {
        std::vector<std::string> out;
        for (size_t i = 0; i < count; ++i)
            out.push_back("option " + std::to_string(i));
        return out;
    }

    Forma::Dropdown place_dropdown(size_t count = 4, size_t initial = 0, std::function<void(size_t)> on_select = {})
    {
        return Forma::Dropdown {}
            .options(labels(count))
            .initial(initial)
            .on_select(std::move(on_select))
            .place(*surface, cursor, -1.F, 1.F, 0.2F);
    }

    std::vector<uint32_t> rows_of(const Forma::Dropdown& dropdown) const
    {
        return surface->layer().related_ids(dropdown.header_id);
    }

    bool all_rows(const Forma::Dropdown& dropdown, bool visible) const
    {
        const auto rows = rows_of(dropdown);
        return !rows.empty() && std::ranges::all_of(rows, [&](uint32_t id) {
            return surface->layer().get(id)->visible == visible;
        });
    }

    void click(glm::vec2 ndc)
    {
        window->move_to_ndc(ndc);
        window->press(IO::MouseButtons::Left);
        window->release(IO::MouseButtons::Left);
    }

    std::optional<Forma::Surface> surface;
    Forma::LayoutCursor cursor;
    const glm::vec2 header_centre { 0.F, 0.9F };

    static glm::vec2 row_centre(size_t index)
    {
        return { 0.F, 0.7F - 0.2F * static_cast<float>(index) };
    }
};

// =============================================================================
// Placement
// =============================================================================

TEST_F(FormaDropdownTest, PlaceRegistersTheHeaderAndAdvancesTheCursor)
{
    auto dropdown = place_dropdown();

    ASSERT_NE(dropdown.header_id, 0U);
    const auto* element = surface->layer().get(dropdown.header_id);
    ASSERT_NE(element, nullptr);
    ASSERT_TRUE(element->bounds_hint.has_value());
    EXPECT_NEAR(element->bounds_hint->min.y, 0.8F, 1e-5F);
    EXPECT_NEAR(element->bounds_hint->max.y, 1.F, 1e-5F);
    EXPECT_NEAR(dropdown.bounds().min.y, 0.8F, 1e-5F);
    EXPECT_NEAR(cursor.y(), 0.8F, 1e-6F);
    EXPECT_NEAR(dropdown.cursor_out.y(), 0.8F, 1e-6F);
}

TEST_F(FormaDropdownTest, WithNoOptionsPlaceLeavesTheStructAndCursorUntouched)
{
    auto dropdown = Forma::Dropdown {}.place(*surface, cursor, -1.F, 1.F, 0.2F);

    EXPECT_EQ(dropdown.header_id, 0U);
    EXPECT_EQ(dropdown.selected, nullptr);
    EXPECT_NEAR(cursor.y(), 1.F, 1e-6F);
    EXPECT_EQ(surface->layer().size(), 0U);
}

TEST_F(FormaDropdownTest, TheInitialSelectionIsTheConfiguredIndex)
{
    auto dropdown = place_dropdown(4, 2);

    ASSERT_NE(dropdown.selected, nullptr);
    EXPECT_EQ(dropdown.selected->value, 2U);
}

TEST_F(FormaDropdownTest, AnInitialIndexPastTheEndIsClampedToTheLastOption)
{
    auto dropdown = place_dropdown(3, 99);

    EXPECT_EQ(dropdown.selected->value, 2U);
}

TEST_F(FormaDropdownTest, TheSelectedStateIsRegisteredWithTheBridgeOnItsLayer)
{
    auto dropdown = place_dropdown();

    EXPECT_EQ(dropdown.selected->id, dropdown.header_id);
    EXPECT_EQ(dropdown.selected->layer.lock(), surface->layer_ptr());
}

// =============================================================================
// Opening and closing
// =============================================================================

TEST_F(FormaDropdownTest, TheListStartsClosedWithOneHiddenRowPerOption)
{
    auto dropdown = place_dropdown(4);

    EXPECT_FALSE(dropdown.open->value);
    EXPECT_EQ(rows_of(dropdown).size(), 4U);
    EXPECT_TRUE(all_rows(dropdown, false));
    EXPECT_TRUE(surface->layer().get(dropdown.header_id)->visible);
}

TEST_F(FormaDropdownTest, PressingTheHeaderOpensAndClosesTheList)
{
    auto dropdown = place_dropdown();

    click(header_centre);
    EXPECT_TRUE(dropdown.open->value);
    EXPECT_TRUE(all_rows(dropdown, true));

    click(header_centre);
    EXPECT_FALSE(dropdown.open->value);
    EXPECT_TRUE(all_rows(dropdown, false));
}

TEST_F(FormaDropdownTest, AClosedRowCannotBeChosen)
{
    auto dropdown = place_dropdown();

    click(row_centre(2));

    EXPECT_EQ(dropdown.selected->value, 0U);
}

// =============================================================================
// Choosing
// =============================================================================

TEST_F(FormaDropdownTest, ChoosingARowSelectsItAndClosesTheList)
{
    auto dropdown = place_dropdown();
    click(header_centre);

    click(row_centre(2));

    EXPECT_EQ(dropdown.selected->value, 2U);
    EXPECT_FALSE(dropdown.open->value);
    EXPECT_TRUE(all_rows(dropdown, false));
}

TEST_F(FormaDropdownTest, ChoosingARowBumpsTheSelectedVersion)
{
    auto dropdown = place_dropdown();
    const auto before = dropdown.selected->version;
    click(header_centre);

    click(row_centre(1));

    EXPECT_GT(dropdown.selected->version, before);
}

TEST_F(FormaDropdownTest, OnSelectReceivesTheChosenIndexOncePerChoice)
{
    std::vector<size_t> chosen;
    auto dropdown = place_dropdown(4, 0, [&](size_t index) { chosen.push_back(index); });

    click(header_centre);
    click(row_centre(3));
    click(header_centre);
    click(row_centre(1));

    EXPECT_EQ(chosen, (std::vector<size_t> { 3, 1 }));
}

TEST_F(FormaDropdownTest, OpeningTheListLiftsItAboveElementsAddedLater)
{
    auto dropdown = place_dropdown();
    surface->add(Forma::Element {}.with_bounds({ .min = { -1.F, 0.4F }, .max = { 1.F, 0.6F } }));
    click(header_centre);

    click(row_centre(1));

    EXPECT_EQ(dropdown.selected->value, 1U);
}

TEST_F(FormaDropdownTest, TheChosenIndexReachesABridgeSink)
{
    auto dropdown = place_dropdown();
    std::vector<float> seen;
    Forma::bridge().at(dropdown.selected).write(std::function<void(std::span<const float>)>([&](std::span<const float> values) {
        seen.assign(values.begin(), values.end());
    }));
    click(header_centre);
    click(row_centre(2));

    frames.tick();

    EXPECT_EQ(seen, std::vector<float> { 2.F });
}

// =============================================================================
// Scrolling
// =============================================================================

TEST_F(FormaDropdownTest, ALongListHangsOneScrollViewportUnderTheHeader)
{
    auto dropdown = Forma::Dropdown {}
                        .options(labels(10))
                        .max_visible(3)
                        .place(*surface, cursor, -1.F, 1.F, 0.2F);

    EXPECT_EQ(rows_of(dropdown).size(), 1U);
    EXPECT_FALSE(surface->layer().get(rows_of(dropdown).front())->visible);
}

TEST_F(FormaDropdownTest, ALongListShowsOnlyTheVisibleRowsAndClipsTheRest)
{
    auto dropdown = Forma::Dropdown {}
                        .options(labels(10))
                        .max_visible(3)
                        .place(*surface, cursor, -1.F, 1.F, 0.2F);
    click(header_centre);

    click(row_centre(3));
    EXPECT_EQ(dropdown.selected->value, 0U);
    EXPECT_TRUE(dropdown.open->value);

    click(row_centre(1));
    EXPECT_EQ(dropdown.selected->value, 1U);
    EXPECT_FALSE(dropdown.open->value);
}

}
