#include "../../support/forma_rig.hpp"

#include "MayaFlux/Portal/Forma/Primitives/Collapsible.hpp"

namespace MayaFlux::Test {

namespace Forma = Portal::Forma;
using Kinesis::AABB2D;
using Portal::Graphics::PrimitiveTopology;

class FormaLabelledCollapsibleTest : public FormaRigTest {
protected:
    void SetUp() override
    {
        FormaRigTest::SetUp();
        if (IsSkipped())
            return;
        surface.emplace(make_surface("labelled-collapsible"));
    }

    void TearDown() override
    {
        surface.reset();
        FormaRigTest::TearDown();
    }

    Forma::Collapsible place_header(bool initially_open = true)
    {
        return Forma::Collapsible {}
            .initially_open(initially_open)
            .label("section")
            .place(Forma::create_buffer(window, PrimitiveTopology::TRIANGLE_LIST,
                       std::vector<std::pair<std::string, std::shared_ptr<Core::VKImage>>> { { "text", nullptr } }),
                *surface, cursor, -1.F, 1.F, 0.2F);
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
};

TEST_F(FormaLabelledCollapsibleTest, PlaceRegistersTheHeaderRowAndAdvancesTheCursor)
{
    auto header = place_header();

    ASSERT_NE(header.header_id, 0U);
    const auto* element = surface->layer().get(header.header_id);
    ASSERT_NE(element, nullptr);
    ASSERT_TRUE(element->bounds_hint.has_value());
    EXPECT_NEAR(element->bounds_hint->min.y, 0.8F, 1e-5F);
    EXPECT_NEAR(element->bounds_hint->max.y, 1.F, 1e-5F);
    EXPECT_NEAR(cursor.y(), 0.8F, 1e-6F);
    EXPECT_NE(header.buf, nullptr);
}

TEST_F(FormaLabelledCollapsibleTest, PressingTheHeaderTogglesTheOpenStateAndTheAttachedBody)
{
    auto header = place_header(true);
    const uint32_t body = surface->add(Forma::Element {}.with_bounds({ .min = { -1.F, -0.5F }, .max = { 1.F, 0.F } }));
    header.attach(surface->layer(), body);
    ASSERT_TRUE(surface->layer().get(body)->visible);

    click(header_centre);
    EXPECT_FALSE(header.open->value);
    EXPECT_FALSE(surface->layer().get(body)->visible);

    click(header_centre);
    EXPECT_TRUE(header.open->value);
    EXPECT_TRUE(surface->layer().get(body)->visible);
}

TEST_F(FormaLabelledCollapsibleTest, ACollapsedHeaderHidesItsBodyFromTheStart)
{
    auto header = place_header(false);
    const uint32_t body = surface->add(Forma::Element {}.with_bounds({ .min = { -1.F, -0.5F }, .max = { 1.F, 0.F } }));

    header.attach(surface->layer(), body);

    EXPECT_FALSE(header.open->value);
    EXPECT_FALSE(surface->layer().get(body)->visible);
}

TEST_F(FormaLabelledCollapsibleTest, ResizingTheWindowKeepsTheHeaderAndItsState)
{
    auto header = place_header(false);
    click(header_centre);
    ASSERT_TRUE(header.open->value);

    EXPECT_NO_THROW(window->resize(640, 480));

    EXPECT_TRUE(header.open->value);
    EXPECT_NE(surface->layer().get(header.header_id), nullptr);
}

}
