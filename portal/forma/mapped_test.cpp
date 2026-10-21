#include "../../test_config.h"

#include "MayaFlux/Portal/Forma/Primitives/FormFactory.hpp"

namespace MayaFlux::Test {

namespace Forma = Portal::Forma;

namespace {

    const Kinesis::AABB2D k_track { .min = { -0.8F, -0.05F }, .max = { 0.8F, 0.05F } };
    constexpr float k_handle = 0.1F;

    struct FaderRig {
        Forma::Layer layer;
        Forma::Mapped<float> mapped;

        FaderRig()
        {
            auto form = Forma::Geometry::horizontal_fader(k_track, k_handle);
            auto buffer = std::make_shared<Buffers::FormaBuffer>(form.capacity, form.topology);
            mapped = Forma::make_mapped<float>(0.F, form.geometry, std::move(buffer));
            mapped.element.id = layer.add(mapped.element);
        }

        [[nodiscard]] glm::vec2 handle_centre() const { return mapped.element.bounds_hint->center(); }
        [[nodiscard]] glm::vec2 layer_handle_centre() const { return layer.get(mapped.element.id)->bounds_hint->center(); }
    };

}

TEST(FormaMappedStateTest, WriteStoresTheValueAndBumpsTheVersion)
{
    Forma::MappedState<float> state;

    state.write(0.25F);
    state.write(0.5F);

    EXPECT_FLOAT_EQ(state.value, 0.5F);
    EXPECT_EQ(state.version, 2U);
}

TEST(FormaMappedTest, MakeMappedStartsWithAPendingFirstGeneration)
{
    FaderRig rig;

    EXPECT_FLOAT_EQ(rig.mapped.state->value, 0.F);
    EXPECT_EQ(rig.mapped.state->version, 1U);
    EXPECT_NE(rig.mapped.last_version, rig.mapped.state->version);
    EXPECT_FALSE(rig.mapped.element.bounds_hint.has_value());
}

TEST(FormaMappedTest, SyncGeneratesGeometryOnlyWhenTheVersionAdvances)
{
    int calls = 0;
    auto form = Forma::Geometry::horizontal_fader(k_track, k_handle);
    Forma::GeometryFn<float> counting = [&calls, inner = form.geometry](float v, std::vector<uint8_t>& out, Forma::Element& el) {
        ++calls;
        inner(v, out, el);
    };
    auto buffer = std::make_shared<Buffers::FormaBuffer>(form.capacity, form.topology);
    auto mapped = Forma::make_mapped<float>(0.F, counting, std::move(buffer));

    mapped.sync();
    mapped.sync();
    EXPECT_EQ(calls, 1);

    mapped.state->write(0.5F);
    mapped.sync();
    mapped.sync();
    EXPECT_EQ(calls, 2);
}

TEST(FormaMappedTest, ForceRedrawRegeneratesOnEverySync)
{
    int calls = 0;
    auto form = Forma::Geometry::horizontal_fader(k_track, k_handle);
    Forma::GeometryFn<float> counting = [&calls, inner = form.geometry](float v, std::vector<uint8_t>& out, Forma::Element& el) {
        ++calls;
        inner(v, out, el);
    };
    auto buffer = std::make_shared<Buffers::FormaBuffer>(form.capacity, form.topology);
    auto mapped = Forma::make_mapped<float>(0.F, counting, std::move(buffer));
    mapped.force_redraw_on_sync = true;

    mapped.sync();
    mapped.sync();
    mapped.sync();

    EXPECT_EQ(calls, 3);
}

TEST(FormaMappedTest, SyncIsANoOpWithoutStateGeometryOrBuffer)
{
    FaderRig rig;

    auto no_state = rig.mapped;
    no_state.state.reset();
    no_state.sync();
    EXPECT_FALSE(no_state.element.bounds_hint.has_value());

    auto no_geometry = rig.mapped;
    no_geometry.geometry_fn = nullptr;
    no_geometry.sync();
    EXPECT_FALSE(no_geometry.element.bounds_hint.has_value());

    auto no_buffer = rig.mapped;
    no_buffer.element.buffer.reset();
    no_buffer.sync();
    EXPECT_FALSE(no_buffer.element.bounds_hint.has_value());
}

TEST(FormaMappedTest, SyncWithALayerPublishesTheHandleBoundsAndContains)
{
    FaderRig rig;

    rig.mapped.sync(&rig.layer);

    ASSERT_TRUE(rig.layer.get(rig.mapped.element.id)->bounds_hint.has_value());
    EXPECT_EQ(rig.layer_handle_centre(), rig.handle_centre());
    EXPECT_EQ(rig.layer.hit_test(rig.handle_centre()), rig.mapped.element.id);
}

TEST(FormaMappedTest, SyncWithALayerMovesTheHitRegionWithTheHandle)
{
    FaderRig rig;
    rig.mapped.sync(&rig.layer);
    const glm::vec2 before = rig.handle_centre();

    rig.mapped.state->write(1.F);
    rig.mapped.sync(&rig.layer);
    const glm::vec2 after = rig.handle_centre();

    ASSERT_GT(after.x, before.x + 1.F);
    EXPECT_EQ(rig.layer.hit_test(after), rig.mapped.element.id);
    EXPECT_FALSE(rig.layer.hit_test(before).has_value());
}

TEST(FormaMappedTest, SyncWithoutALayerLeavesTheLayerCopyAtItsFirstPosition)
{
    FaderRig rig;
    rig.mapped.sync(&rig.layer);
    const glm::vec2 first = rig.handle_centre();

    rig.mapped.state->write(1.F);
    rig.mapped.sync();

    EXPECT_GT(rig.handle_centre().x, first.x + 1.F);
    EXPECT_EQ(rig.layer_handle_centre(), first);
    EXPECT_FALSE(rig.layer.hit_test(rig.handle_centre()).has_value());
    EXPECT_EQ(rig.layer.hit_test(first), rig.mapped.element.id);
}

TEST(FormaMappedTest, SyncPublishesAContainsPredicateWhenTheGeometrySetsOne)
{
    Forma::Layer layer;
    auto form = Forma::Geometry::radial(k_track, 0.F, 1.F);
    auto buffer = std::make_shared<Buffers::FormaBuffer>(form.capacity, form.topology);
    auto mapped = Forma::make_mapped<float>(0.5F, form.geometry, std::move(buffer));
    mapped.element.id = layer.add(mapped.element);
    ASSERT_FALSE(static_cast<bool>(layer.get(mapped.element.id)->contains));

    mapped.sync(&layer);

    EXPECT_TRUE(static_cast<bool>(layer.get(mapped.element.id)->contains));
}

}
