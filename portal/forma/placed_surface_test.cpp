#include "gtest/gtest.h"
#include "../../support/test_metadata.hpp"

#include "MayaFlux/Kinesis/Spatial/HitTest.hpp"
#include "MayaFlux/Portal/Forma/Layer.hpp"

#include <glm/gtc/matrix_transform.hpp>

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    namespace Forma = Portal::Forma;

    Kinesis::ViewTransform placed_view_camera()
    {
        return Kinesis::look_at_perspective({ 0.F, 0.F, 5.F }, { 0.F, 0.F, 0.F }, 1.5707964F, 1.F, 0.1F, 100.F);
    }

    std::function<glm::mat4()> placed_at(const glm::mat4& model)
    {
        return [model] { return model; };
    }

    uint32_t add_whole(Forma::Layer& layer)
    {
        return layer.add(Forma::Element {}.with_rect({ -1.F, -1.F }, { 1.F, 1.F }));
    }

    const glm::mat4 k_turned_away = glm::rotate(glm::mat4(1.F), glm::pi<float>(), { 0.F, 1.F, 0.F });

}

TEST(PlacedLayerTest, APlacedElementIsHitWhereItsPlaneShowsAndCarriesItsDistance)
{
    Forma::Layer layer;
    const uint32_t id = add_whole(layer);
    layer.set_placement(placed_view_camera, placed_at(glm::translate(glm::mat4(1.F), { 2.F, 0.F, 0.F })));

    const glm::vec2 shown { 0.4F, 0.F };
    const auto local = layer.local_point(id, shown);

    ASSERT_TRUE(local.has_value());
    EXPECT_NEAR(local->x, 0.F, 1e-3F);
    EXPECT_EQ(layer.hit_test(shown), id);
    EXPECT_FALSE(layer.hit_test({ 0.F, 0.F }).has_value());
    ASSERT_TRUE(layer.hit_depth(shown).has_value());
    EXPECT_GT(*layer.hit_depth(shown), 0.F);
}

TEST(PlacedLayerTest, AFlatElementReportsTheLowestDistance)
{
    Forma::Layer layer;
    (void)add_whole(layer);

    EXPECT_EQ(layer.hit_depth({ 0.F, 0.F }), std::numeric_limits<float>::lowest());
}

TEST(PlacedLayerTest, SeenFromBehindNothingIsHitOrDrawn)
{
    Forma::Layer layer;
    (void)add_whole(layer);
    layer.set_placement(placed_view_camera, placed_at(k_turned_away));

    EXPECT_FALSE(layer.hit_test({ 0.F, 0.F }).has_value());
    EXPECT_TRUE(layer.drawn_view().projection == glm::mat4(0.F));
}

TEST(PlacedLayerTest, TheDrawnViewPutsElementCoordinatesWhereThePointerFindsThem)
{
    const glm::mat4 model = glm::translate(glm::mat4(1.F), { -1.F, 0.5F, 0.F })
        * glm::rotate(glm::mat4(1.F), 0.5F, { 0.F, 1.F, 0.F });
    Forma::Layer layer;
    const uint32_t id = add_whole(layer);
    layer.set_placement(placed_view_camera, placed_at(model));

    const auto drawn = layer.drawn_view();
    const glm::vec4 clip = drawn.projection * drawn.view * glm::vec4(0.3F, -0.6F, 0.F, 1.F);
    const auto local = layer.local_point(id, glm::vec2(clip) / clip.w);

    ASSERT_TRUE(local.has_value());
    EXPECT_NEAR(local->x, 0.3F, 1e-3F);
    EXPECT_NEAR(local->y, -0.6F, 1e-3F);
}

TEST(PlacedLayerTest, RemovingThePlacementOrSettingAViewMakesTheLayerFlatAgain)
{
    Forma::Layer layer;
    (void)add_whole(layer);

    layer.set_placement(placed_view_camera, placed_at(k_turned_away));
    layer.set_placement({}, {});
    EXPECT_FALSE(layer.placed());
    EXPECT_TRUE(layer.hit_test({ 0.F, 0.F }).has_value());

    layer.set_placement(placed_view_camera, placed_at(k_turned_away));
    layer.set_view(std::nullopt);
    EXPECT_FALSE(layer.placed());
    EXPECT_TRUE(layer.hit_test({ 0.F, 0.F }).has_value());
}

TEST(PlacedLayerTest, AFlattenedPlacementIsNeverHit)
{
    Forma::Layer layer;
    (void)add_whole(layer);
    layer.set_placement(placed_view_camera, placed_at(glm::mat4(0.F)));

    EXPECT_FALSE(layer.hit_test({ 0.F, 0.F }).has_value());
}

}
