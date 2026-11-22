#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kinesis/Spatial/Bounds.hpp"
#include "MayaFlux/Nexus/Quantity.hpp"
#include "MayaFlux/Transitive/Facet/ControlValueStore.hpp"

#include <glm/gtc/quaternion.hpp>

#include "../transitive/control_store_fixtures.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Nexus::Quantity;
    using QuantityStore = ControlValueStore<Quantity>;

    static_assert(ControlSlot<Quantity>);
    static_assert(NumericControlSlot<Quantity>);

    TEST(NexusQuantityTest, ANewQuantityIsEmptyAndDoesNotAskForAClear)
    {
        const Quantity quantity;

        EXPECT_FALSE(quantity.has_value());
        EXPECT_FALSE(quantity.clear_requested());
        EXPECT_FALSE(quantity.due());
    }

    TEST(NexusQuantityTest, AnUnsetQuantityIsEmptyAndAsksForAClear)
    {
        const Quantity quantity = Quantity::unset();

        EXPECT_FALSE(quantity.has_value());
        EXPECT_TRUE(quantity.clear_requested());
        EXPECT_FALSE(quantity.due());
    }

    TEST(NexusQuantityTest, ItConvertsFromEveryTypeAControlValueHolds)
    {
        const Quantity number = 2.0;
        const Quantity flag = true;
        const Quantity vector = glm::vec3(1.0F, 2.0F, 3.0F);
        const Quantity rotation = glm::quat(1.0F, 0.0F, 0.0F, 0.0F);
        const Quantity box = Kinesis::AABB3D { .min = glm::vec3(-1.0F), .max = glm::vec3(1.0F) };

        EXPECT_DOUBLE_EQ(number.value(), 2.0);
        EXPECT_TRUE(flag.value<bool>());
        EXPECT_EQ(vector.value<glm::vec3>(), glm::vec3(1.0F, 2.0F, 3.0F));
        EXPECT_FLOAT_EQ(rotation.value<glm::quat>().w, 1.0F);
        EXPECT_EQ(box.value<Kinesis::AABB3D>().max, glm::vec3(1.0F));
    }

    TEST(NexusQuantityTest, AStructOfQuantitiesMergesOnlyWhatTheIncomingHolds)
    {
        QuantityStore store;
        auto& live = store.declare<StoreQuantityTuning>();

        StoreQuantityTuning incoming;
        incoming.intensity = 0.5;
        store.merge(incoming);

        EXPECT_FALSE(live.position.has_value());
        EXPECT_TRUE(live.intensity.due());
        EXPECT_DOUBLE_EQ(live.intensity.value(), 0.5);
    }

    TEST(NexusQuantityTest, AnUnsetMemberEmptiesTheLiveOneAndLeavesTheRestAlone)
    {
        QuantityStore store;
        auto& live = store.declare<StoreQuantityTuning>();
        StoreQuantityTuning first;
        first.position = glm::vec3(1.0F, 2.0F, 3.0F);
        first.intensity = 0.5;
        store.merge(first);
        ASSERT_TRUE(live.position.has_value());

        StoreQuantityTuning second;
        second.position = Quantity::unset();
        store.merge(second);

        EXPECT_FALSE(live.position.has_value());
        EXPECT_FALSE(live.position.due());
        EXPECT_TRUE(live.intensity.has_value());
        EXPECT_DOUBLE_EQ(live.intensity.value(), 0.5);
    }

    TEST(NexusQuantityTest, AnEmptyMemberNeverClearsTheLiveOne)
    {
        QuantityStore store;
        auto& live = store.declare<StoreQuantityTuning>();
        StoreQuantityTuning first;
        first.position = glm::vec3(1.0F);
        store.merge(first);

        store.merge(StoreQuantityTuning {});

        EXPECT_TRUE(live.position.has_value());
        EXPECT_EQ(live.position.value<glm::vec3>(), glm::vec3(1.0F));
    }

    TEST(NexusQuantityTest, AMemberCanBeSetAgainAfterItWasUnset)
    {
        QuantityStore store;
        auto& live = store.declare<StoreQuantityTuning>();
        StoreQuantityTuning clear;
        clear.position = Quantity::unset();
        store.merge(clear);

        StoreQuantityTuning again;
        again.position = glm::vec3(4.0F, 5.0F, 6.0F);
        store.merge(again);

        EXPECT_TRUE(live.position.has_value());
        EXPECT_TRUE(live.position.due());
        EXPECT_FALSE(live.position.clear_requested());
        EXPECT_EQ(live.position.value<glm::vec3>(), glm::vec3(4.0F, 5.0F, 6.0F));
    }

    TEST(NexusQuantityTest, AnOrientationTravelsThroughTheStoreAsAQuaternion)
    {
        QuantityStore store;
        auto& live = store.declare<StoreQuantityTuning>();

        StoreQuantityTuning incoming;
        incoming.orientation = glm::quat(0.5F, 0.1F, 0.2F, 0.3F);
        store.merge(incoming);

        const auto taken = live.orientation.pending<glm::quat>();
        ASSERT_TRUE(taken.has_value());
        EXPECT_FLOAT_EQ(taken->w, 0.5F);
        EXPECT_FLOAT_EQ(taken->x, 0.1F);
        EXPECT_FLOAT_EQ(taken->y, 0.2F);
        EXPECT_FLOAT_EQ(taken->z, 0.3F);
    }

    TEST(NexusQuantityTest, ANumberIsSetAndReadByName)
    {
        QuantityStore store;
        [[maybe_unused]] auto& live = store.declare<StoreQuantityTuning>();

        EXPECT_TRUE(store.set_named("intensity", 0.75));

        const auto found = store.number("intensity");
        ASSERT_TRUE(found.has_value());
        EXPECT_DOUBLE_EQ(*found, 0.75);
        EXPECT_FALSE(store.number("position").has_value());
    }

    TEST(NexusQuantityTest, AWholeQuantitySetByNameCanBeAVector)
    {
        QuantityStore store;
        auto& live = store.declare<StoreQuantityTuning>();

        EXPECT_TRUE(store.set_named("position", Quantity(glm::vec3(1.0F, 2.0F, 3.0F))));

        EXPECT_EQ(live.position.value<glm::vec3>(), glm::vec3(1.0F, 2.0F, 3.0F));
    }

    TEST(NexusQuantityTest, ASnapshotCopiesWhatTheStoreHoldsNow)
    {
        QuantityStore store;
        auto& live = store.declare<StoreQuantityTuning>();
        StoreQuantityTuning incoming;
        incoming.intensity = 0.5;
        store.merge(incoming);

        auto copy = store.snapshot<StoreQuantityTuning>();
        copy.intensity = 0.9;

        EXPECT_DOUBLE_EQ(copy.intensity.value(), 0.9);
        EXPECT_DOUBLE_EQ(live.intensity.value(), 0.5);
    }

}
}
