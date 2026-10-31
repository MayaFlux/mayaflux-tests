#include "gtest/gtest.h"

#include "MayaFlux/Core/Backends/Graphics/Vulkan/VKImage.hpp"
#include "MayaFlux/Portal/Forma/Element.hpp"
#include "MayaFlux/Portal/Text/InkPress.hpp"

namespace MayaFlux::Test {

namespace Forma = Portal::Forma;
using Kinesis::AABB2D;

TEST(FormaElementTest, DefaultsToAnInteractiveVisibleRegionWithNoShape)
{
    const Forma::Element el;

    EXPECT_EQ(el.id, 0U);
    EXPECT_FALSE(el.bounds_hint.has_value());
    EXPECT_FALSE(static_cast<bool>(el.contains));
    EXPECT_FALSE(el.clip.has_value());
    EXPECT_EQ(el.buffer, nullptr);
    EXPECT_EQ(el.texture, nullptr);
    EXPECT_TRUE(el.interactive);
    EXPECT_TRUE(el.visible);
    EXPECT_TRUE(el.name.empty());
}

TEST(FormaElementTest, BoundsAndRectSetTheFastRejectBox)
{
    const auto el = Forma::Element {}.with_bounds(AABB2D { .min = { -1.F, -2.F }, .max = { 3.F, 4.F } });
    const auto rect = Forma::Element {}.with_rect({ 0.F, 1.F }, { 2.F, 3.F });

    ASSERT_TRUE(el.bounds_hint.has_value());
    EXPECT_EQ(el.bounds_hint->min, glm::vec2(-1.F, -2.F));
    EXPECT_EQ(el.bounds_hint->max, glm::vec2(3.F, 4.F));
    ASSERT_TRUE(rect.bounds_hint.has_value());
    EXPECT_EQ(rect.bounds_hint->min, glm::vec2(0.F, 1.F));
    EXPECT_EQ(rect.bounds_hint->max, glm::vec2(2.F, 3.F));
    EXPECT_FALSE(static_cast<bool>(el.contains));
}

TEST(FormaElementTest, WithContainsStoresTheGivenPredicate)
{
    const auto el = Forma::Element {}.with_contains([](glm::vec2 p) { return p.x > 0.F; });

    ASSERT_TRUE(static_cast<bool>(el.contains));
    EXPECT_TRUE(el.contains({ 1.F, 0.F }));
    EXPECT_FALSE(el.contains({ -1.F, 0.F }));
}

TEST(FormaElementTest, WithCircleSetsACircularPredicateAndLeavesTheBoundsAlone)
{
    const auto el = Forma::Element {}.with_circle({ 1.F, 1.F }, 0.5F);

    ASSERT_TRUE(static_cast<bool>(el.contains));
    EXPECT_TRUE(el.contains({ 1.F, 1.F }));
    EXPECT_TRUE(el.contains({ 1.4F, 1.F }));
    EXPECT_FALSE(el.contains({ 1.6F, 1.F }));
    EXPECT_FALSE(el.bounds_hint.has_value());
}

TEST(FormaElementTest, WithPolygonSetsAWindingPredicate)
{
    const std::array<glm::vec2, 3> triangle { glm::vec2(0.F, 0.F), glm::vec2(4.F, 0.F), glm::vec2(0.F, 4.F) };

    const auto el = Forma::Element {}.with_polygon(triangle);

    ASSERT_TRUE(static_cast<bool>(el.contains));
    EXPECT_TRUE(el.contains({ 1.F, 1.F }));
    EXPECT_FALSE(el.contains({ 3.F, 3.F }));
}

TEST(FormaElementTest, WithStrokeSetsADistanceToThePathPredicate)
{
    const std::array<glm::vec2, 2> path { glm::vec2(0.F, 0.F), glm::vec2(2.F, 0.F) };

    const auto el = Forma::Element {}.with_stroke(path, 0.1F);

    ASSERT_TRUE(static_cast<bool>(el.contains));
    EXPECT_TRUE(el.contains({ 1.F, 0.05F }));
    EXPECT_FALSE(el.contains({ 1.F, 0.5F }));
}

TEST(FormaElementTest, WithBufferStoresAndClearsTheBuffer)
{
    auto buffer = std::make_shared<Buffers::FormaBuffer>(
        4096, Portal::Graphics::PrimitiveTopology::TRIANGLE_LIST);

    auto el = Forma::Element {}.with_buffer(buffer);
    EXPECT_EQ(el.buffer, buffer);

    el.with_buffer(nullptr);
    EXPECT_EQ(el.buffer, nullptr);
}

TEST(FormaElementTest, FlagSettersChainOnTheSameObject)
{
    Forma::Element el;

    Forma::Element& chained = el.non_interactive().hidden().with_name("probe");

    EXPECT_EQ(&chained, &el);
    EXPECT_FALSE(el.interactive);
    EXPECT_FALSE(el.visible);
    EXPECT_EQ(el.name, "probe");
}

TEST(FormaElementTest, EverySetterReturnsTheSameObject)
{
    Forma::Element el;

    EXPECT_EQ(&el.with_bounds({}), &el);
    EXPECT_EQ(&el.with_rect({}, {}), &el);
    EXPECT_EQ(&el.with_contains({}), &el);
    EXPECT_EQ(&el.with_circle({}, 1.F), &el);
    EXPECT_EQ(&el.with_buffer(nullptr), &el);
}

TEST(FormaElementTest, TextureAndTextHelpersAreNoOpsWithoutABuffer)
{
    Forma::Element el;
    auto image = std::make_shared<Core::VKImage>(4, 4, 1, vk::Format::eR8G8B8A8Unorm, Core::VKImage::Usage::TEXTURE_2D);

    EXPECT_EQ(&el.with_texture(image), &el);
    EXPECT_EQ(&el.with_text("hello", std::nullopt), &el);
    EXPECT_EQ(&el.retarget(AABB2D { .min = { 0.F, 0.F }, .max = { 1.F, 1.F } }), &el);
    el.set_text("again", std::nullopt);

    EXPECT_EQ(el.texture, nullptr);
}

TEST(FormaElementTest, CopiesCarryEveryField)
{
    Forma::Element el = Forma::Element {}.with_rect({ 0.F, 0.F }, { 1.F, 1.F }).with_name("a").non_interactive();
    el.clip = AABB2D { .min = { 0.F, 0.F }, .max = { 0.5F, 0.5F } };
    el.id = 7;

    const Forma::Element copy = el;

    EXPECT_EQ(copy.id, 7U);
    EXPECT_EQ(copy.name, "a");
    EXPECT_FALSE(copy.interactive);
    ASSERT_TRUE(copy.clip.has_value());
    EXPECT_EQ(copy.clip->max, glm::vec2(0.5F));
    ASSERT_TRUE(copy.bounds_hint.has_value());
}

}
