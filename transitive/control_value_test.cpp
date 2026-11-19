#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kinesis/Spatial/Bounds.hpp"
#include "MayaFlux/Kinesis/VertexSampler.hpp"
#include "MayaFlux/Transitive/Facet/ControlValueStore.hpp"
#include "MayaFlux/Transitive/Facet/ControlValueText.hpp"

#include "control_value_fixtures.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    enum class SlotMode : uint8_t {
        OFF,
        SOFT,
        HARD
    };

    struct SlotBox {
        glm::vec3 min;
        glm::vec3 max;
    };

    using NumericStore = ControlValueStore<ControlValue>;
    using LabelStore = ControlValueStore<ControlSnapshot<std::string>>;

    static_assert(ControlSlot<ControlValue>);
    static_assert(NumericControlSlot<ControlValue>);
    static_assert(ControlSlot<ControlSnapshot<std::string>>);
    static_assert(!NumericControlSlot<ControlSnapshot<std::string>>);
    static_assert(ControlValue::is_value<glm::vec3>);
    static_assert(!ControlValue::is_value<glm::dvec3>);
    static_assert(ControlValue::is_box<SlotBox>);
    static_assert(!ControlValue::is_box<glm::vec3>);

    TEST(ControlValueSlotTest, AnEmptySlotHoldsNothingAndIsNeverDue)
    {
        ControlValue slot;

        EXPECT_FALSE(slot.has_value());
        EXPECT_EQ(slot.width(), 0U);
        EXPECT_FALSE(slot.due());
        EXPECT_FALSE(slot.pending().has_value());
    }

    TEST(ControlValueSlotTest, SettleDropsAPendingValueWithoutChangingIt)
    {
        ControlValue slot(2.0);

        slot.settle();

        EXPECT_FALSE(slot.due());
        EXPECT_DOUBLE_EQ(slot.value(), 2.0);
    }

    TEST(ControlValueSlotTest, ACopyKeepsTheValueAndWhetherItIsDue)
    {
        ControlValue original(0.25);
        const ControlValue pending(original);

        EXPECT_DOUBLE_EQ(pending.value(), 0.25);
        EXPECT_TRUE(pending.due());

        original.settle();
        const ControlValue settled(original);

        EXPECT_DOUBLE_EQ(settled.value(), 0.25);
        EXPECT_FALSE(settled.due());
    }

    TEST(ControlValueSlotTest, ANumberIsDueOnceUntilItChanges)
    {
        ControlValue slot(0.5);

        EXPECT_TRUE(slot.has_value());
        EXPECT_TRUE(slot.due());

        const auto first = slot.pending();
        ASSERT_TRUE(first.has_value());
        EXPECT_DOUBLE_EQ(*first, 0.5);

        EXPECT_FALSE(slot.due());
        EXPECT_FALSE(slot.pending().has_value());
        EXPECT_DOUBLE_EQ(slot.value(), 0.5);
        EXPECT_DOUBLE_EQ(static_cast<double>(slot), 0.5);
    }

    TEST(ControlValueSlotTest, AssigningMakesASettledSlotDueAgain)
    {
        ControlValue live(1.0);
        live.settle();
        ASSERT_FALSE(live.due());

        const ControlValue given(2.0);
        live.assign(given);

        EXPECT_TRUE(live.due());
        EXPECT_DOUBLE_EQ(live.value(), 2.0);
    }

    TEST(ControlValueSlotTest, AssigningANumberOverANumberIsAppliedOnce)
    {
        ControlValue live(1.0);
        live.settle();

        live.assign(ControlValue(2.0));

        EXPECT_TRUE(live.due());
        const auto applied = live.pending();
        ASSERT_TRUE(applied.has_value());
        EXPECT_DOUBLE_EQ(*applied, 2.0);
        EXPECT_FALSE(live.pending().has_value());
    }

    TEST(ControlValueSlotTest, ANumberAssignedFromAnotherThreadIsSeenInOrderAndNeverTorn)
    {
        ControlValue live(0.0);
        live.settle();
        constexpr int last = 20000;
        std::atomic<bool> done { false };

        std::thread writer([&] {
            for (int i = 1; i <= last; ++i) {
                live.assign(ControlValue(static_cast<double>(i)));
            }
            done.store(true, std::memory_order_release);
        });

        double previous = 0.0;
        bool ordered = true;
        for (;;) {
            const bool finished = done.load(std::memory_order_acquire);
            if (const auto value = live.pending()) {
                ordered = ordered && *value >= previous && *value == std::floor(*value);
                previous = *value;
            }
            if (finished) {
                break;
            }
        }
        writer.join();

        EXPECT_TRUE(ordered);
        EXPECT_DOUBLE_EQ(previous, static_cast<double>(last));
    }

    TEST(ControlValueTypeTest, AVectorIsHeldWholeAndReadBackAsItsType)
    {
        ControlValue slot(glm::vec3(1.0F, 2.0F, 3.0F));

        EXPECT_TRUE(slot.has_value());
        EXPECT_EQ(slot.value<glm::vec3>(), glm::vec3(1.0F, 2.0F, 3.0F));
        const auto taken = slot.pending<glm::vec3>();
        ASSERT_TRUE(taken.has_value());
        EXPECT_EQ(*taken, glm::vec3(1.0F, 2.0F, 3.0F));
        EXPECT_FALSE(slot.pending<glm::vec3>().has_value());
    }

    TEST(ControlValueTypeTest, EveryGlmVectorWidthRoundTrips)
    {
        EXPECT_EQ(ControlValue(glm::vec2(1.0F, 2.0F)).value<glm::vec2>(), glm::vec2(1.0F, 2.0F));
        EXPECT_EQ(ControlValue(glm::vec3(1.0F, 2.0F, 3.0F)).value<glm::vec3>(), glm::vec3(1.0F, 2.0F, 3.0F));
        EXPECT_EQ(ControlValue(glm::vec4(1.0F, 2.0F, 3.0F, 4.0F)).value<glm::vec4>(), glm::vec4(1.0F, 2.0F, 3.0F, 4.0F));
    }

    TEST(ControlValueTypeTest, IntegersBoolsAndEnumsRoundTripExactly)
    {
        EXPECT_EQ(ControlValue(7).value<int>(), 7);
        EXPECT_EQ(ControlValue(size_t { 9007199254740992ULL }).value<size_t>(), size_t { 9007199254740992ULL });
        EXPECT_TRUE(ControlValue(true).value<bool>());
        EXPECT_FALSE(ControlValue(false).value<bool>());
        EXPECT_EQ(ControlValue(SlotMode::HARD).value<SlotMode>(), SlotMode::HARD);
        EXPECT_FLOAT_EQ(ControlValue(0.25F).value<float>(), 0.25F);
    }

    TEST(ControlValueTypeTest, ReadingAnotherTypeThanWasSetFindsNothing)
    {
        ControlValue scalar(2.0);
        ControlValue vector(glm::vec3(1.0F));

        EXPECT_FALSE(scalar.pending<glm::vec3>().has_value());
        EXPECT_FALSE(vector.pending().has_value());
        EXPECT_FALSE(vector.pending<glm::vec2>().has_value());
        EXPECT_TRUE(scalar.pending().has_value());
        EXPECT_TRUE(vector.pending<glm::vec3>().has_value());
    }

    TEST(ControlValueTypeTest, ADoubleStillReadsAsADoubleWithNoType)
    {
        ControlValue slot(0.75);

        const auto taken = slot.pending();

        ASSERT_TRUE(taken.has_value());
        EXPECT_DOUBLE_EQ(*taken, 0.75);
        EXPECT_DOUBLE_EQ(slot.value(), 0.75);
        EXPECT_DOUBLE_EQ(static_cast<double>(slot), 0.75);
    }

    TEST(ControlValueTypeTest, AssigningAVectorOverANumberChangesTheTypeItReads)
    {
        ControlValue live(1.0);

        live.assign(ControlValue(glm::vec3(4.0F, 5.0F, 6.0F)));

        EXPECT_FALSE(live.pending().has_value());
        EXPECT_EQ(live.value<glm::vec3>(), glm::vec3(4.0F, 5.0F, 6.0F));
    }

    TEST(ControlValueTypeTest, AVectorMergedWithAnEmptyOneKeepsItsComponents)
    {
        ControlValue live(glm::vec3(1.0F, 2.0F, 3.0F));

        if (const ControlValue empty; empty.has_value()) {
            live.assign(empty);
        }

        EXPECT_EQ(live.value<glm::vec3>(), glm::vec3(1.0F, 2.0F, 3.0F));
    }

    TEST(ControlValueTypeTest, ObservingAVectorShowsTheOwnersValueWithoutMakingItDue)
    {
        ControlValue slot;

        slot.observe(glm::vec3(0.0F, -9.8F, 0.0F));

        EXPECT_TRUE(slot.has_value());
        EXPECT_EQ(slot.value<glm::vec3>(), glm::vec3(0.0F, -9.8F, 0.0F));
        EXPECT_FALSE(slot.due());
    }

    TEST(ControlValueTypeTest, ObservingAScalarOfAnyTypeShowsItAsThatType)
    {
        ControlValue slot;

        slot.observe(size_t { 12 });

        EXPECT_EQ(slot.value<size_t>(), size_t { 12 });
        EXPECT_DOUBLE_EQ(slot.value(), 12.0);
        EXPECT_FALSE(slot.due());
    }

    TEST(ControlValueTypeTest, ACopyOfAVectorKeepsEveryComponent)
    {
        const ControlValue original(glm::vec4(1.0F, 2.0F, 3.0F, 4.0F));

        const ControlValue copy = original;

        EXPECT_EQ(copy.value<glm::vec4>(), glm::vec4(1.0F, 2.0F, 3.0F, 4.0F));
        EXPECT_TRUE(copy.due());
    }

    TEST(ControlValueTypeTest, AnEnumPendsOnceAsItsOwnType)
    {
        ControlValue slot(SlotMode::SOFT);

        const auto taken = slot.pending<SlotMode>();

        ASSERT_TRUE(taken.has_value());
        EXPECT_EQ(*taken, SlotMode::SOFT);
        EXPECT_FALSE(slot.pending<SlotMode>().has_value());
    }

    TEST(ControlValueTypeTest, AVectorIsDueUntilSettledAndDueAgainWhenAssigned)
    {
        ControlValue live(glm::vec3(1.0F));

        EXPECT_TRUE(live.due());
        live.settle();
        EXPECT_FALSE(live.due());

        live.assign(ControlValue(glm::vec3(2.0F)));

        EXPECT_TRUE(live.due());
        EXPECT_EQ(live.value<glm::vec3>(), glm::vec3(2.0F));
    }

    TEST(ControlValueTypeTest, AnEmptySlotHoldsNoTypeAndPendsNothing)
    {
        ControlValue slot;

        EXPECT_FALSE(slot.has_value());
        EXPECT_FALSE(slot.pending().has_value());
        EXPECT_FALSE(slot.pending<glm::vec3>().has_value());
        EXPECT_FALSE(slot.pending<int>().has_value());
    }

    TEST(ControlValueTypeTest, AVectorSetFromAnotherThreadIsNeverSeenTorn)
    {
        ControlValue live(glm::vec3(0.0F));
        static_cast<void>(live.pending<glm::vec3>());
        constexpr int last = 20000;
        std::atomic<bool> done { false };

        std::thread writer([&] {
            for (int i = 1; i <= last; ++i) {
                const auto value = static_cast<float>(i);
                live.assign(ControlValue(glm::vec3(value, value, value)));
            }
            done.store(true, std::memory_order_release);
        });

        bool whole = true;
        float previous = 0.0F;
        for (;;) {
            const bool finished = done.load(std::memory_order_acquire);
            if (const auto taken = live.pending<glm::vec3>()) {
                whole = whole && taken->x == taken->y && taken->y == taken->z && taken->x >= previous;
                previous = taken->x;
            }
            if (finished) {
                break;
            }
        }
        writer.join();

        EXPECT_TRUE(whole);
        EXPECT_FLOAT_EQ(previous, static_cast<float>(last));
    }

    TEST(ControlValueBoxTest, ABoxIsHeldWholeAndReadBackAsItsType)
    {
        ControlValue slot(Kinesis::AABB3D { .min = { -1.0F, -2.0F, -3.0F }, .max = { 4.0F, 5.0F, 6.0F } });

        EXPECT_TRUE(slot.has_value());
        EXPECT_EQ(slot.width(), 6U);
        EXPECT_EQ(slot.value<Kinesis::AABB3D>().min, glm::vec3(-1.0F, -2.0F, -3.0F));
        EXPECT_EQ(slot.value<Kinesis::AABB3D>().max, glm::vec3(4.0F, 5.0F, 6.0F));

        const auto taken = slot.pending<Kinesis::AABB3D>();
        ASSERT_TRUE(taken.has_value());
        EXPECT_EQ(taken->min, glm::vec3(-1.0F, -2.0F, -3.0F));
        EXPECT_EQ(taken->max, glm::vec3(4.0F, 5.0F, 6.0F));
        EXPECT_FALSE(slot.pending<Kinesis::AABB3D>().has_value());
    }

    TEST(ControlValueBoxTest, SamplerBoundsRoundTripAndKeepTheirDerivedValues)
    {
        ControlValue slot(Kinesis::SamplerBounds { .min = glm::vec3(-2.0F), .max = glm::vec3(4.0F) });

        const auto bounds = slot.pending<Kinesis::SamplerBounds>();

        ASSERT_TRUE(bounds.has_value());
        EXPECT_EQ(bounds->center(), glm::vec3(1.0F));
        EXPECT_EQ(bounds->extent(), glm::vec3(6.0F));
    }

    TEST(ControlValueBoxTest, ABoxIsNeverReadAsANumberOrAVector)
    {
        ControlValue box(Kinesis::AABB3D { .min = glm::vec3(0.0F), .max = glm::vec3(1.0F) });
        ControlValue wide(glm::vec4(1.0F));

        EXPECT_FALSE(box.pending().has_value());
        EXPECT_FALSE(box.pending<glm::vec3>().has_value());
        EXPECT_FALSE(box.pending<glm::vec4>().has_value());
        EXPECT_FALSE(wide.pending<Kinesis::AABB3D>().has_value());
        EXPECT_TRUE(box.pending<Kinesis::AABB3D>().has_value());
    }

    TEST(ControlValueBoxTest, AssigningABoxOverAVectorChangesTheTypeItReads)
    {
        ControlValue live(glm::vec3(1.0F));

        live.assign(ControlValue(Kinesis::AABB3D { .min = glm::vec3(-1.0F), .max = glm::vec3(1.0F) }));

        EXPECT_FALSE(live.pending<glm::vec3>().has_value());
        EXPECT_EQ(live.value<Kinesis::AABB3D>().max, glm::vec3(1.0F));
    }

    TEST(ControlValueBoxTest, ObservingABoxShowsTheOwnersValueWithoutMakingItDue)
    {
        ControlValue slot;

        slot.observe(Kinesis::SamplerBounds { .min = glm::vec3(-10.0F), .max = glm::vec3(10.0F) });

        EXPECT_TRUE(slot.has_value());
        EXPECT_EQ(slot.value<Kinesis::SamplerBounds>().max, glm::vec3(10.0F));
        EXPECT_FALSE(slot.due());
    }

    TEST(ControlValueBoxTest, ACopyOfABoxKeepsBothCorners)
    {
        const ControlValue original(Kinesis::AABB3D { .min = { 1.0F, 2.0F, 3.0F }, .max = { 4.0F, 5.0F, 6.0F } });

        const ControlValue copy = original;

        EXPECT_EQ(copy.value<Kinesis::AABB3D>().min, glm::vec3(1.0F, 2.0F, 3.0F));
        EXPECT_EQ(copy.value<Kinesis::AABB3D>().max, glm::vec3(4.0F, 5.0F, 6.0F));
        EXPECT_TRUE(copy.due());
    }

    TEST(ControlValueBoxTest, ABoxSetFromAnotherThreadIsNeverSeenWithMismatchedCorners)
    {
        ControlValue live(Kinesis::AABB3D { .min = glm::vec3(0.0F), .max = glm::vec3(0.0F) });
        static_cast<void>(live.pending<Kinesis::AABB3D>());
        constexpr int last = 20000;
        std::atomic<bool> done { false };

        std::thread writer([&] {
            for (int i = 1; i <= last; ++i) {
                const auto value = static_cast<float>(i);
                live.assign(ControlValue(Kinesis::AABB3D { .min = glm::vec3(-value), .max = glm::vec3(value) }));
            }
            done.store(true, std::memory_order_release);
        });

        bool whole = true;
        float previous = 0.0F;
        for (;;) {
            const bool finished = done.load(std::memory_order_acquire);
            if (const auto taken = live.pending<Kinesis::AABB3D>()) {
                whole = whole && taken->min == -taken->max && taken->max.x == taken->max.z && taken->max.x >= previous;
                previous = taken->max.x;
            }
            if (finished) {
                break;
            }
        }
        writer.join();

        EXPECT_TRUE(whole);
        EXPECT_FLOAT_EQ(previous, static_cast<float>(last));
    }

    TEST(ControlValueStoreNestedTest, AFlatStructMergesOnlyWhatTheIncomingHolds)
    {
        NumericStore store;
        auto& live = store.declare<SlotFlatTuning>();

        SlotFlatTuning incoming;
        incoming.decay = ControlValue(0.6);
        store.merge(incoming);

        EXPECT_FALSE(live.scale.has_value());
        EXPECT_TRUE(live.decay.due());
        EXPECT_DOUBLE_EQ(live.decay.value(), 0.6);
    }

    TEST(ControlValueStoreNestedTest, AMemberOfANestedStructIsNamedByItsPath)
    {
        NumericStore store;
        auto& live = store.declare<SlotPadTuning>();

        EXPECT_TRUE(store.set_named("left.deadzone", 0.2));
        EXPECT_TRUE(store.set_named("right.invert", 1.0));

        EXPECT_DOUBLE_EQ(live.left.deadzone.value(), 0.2);
        EXPECT_TRUE(live.left.deadzone.due());
        EXPECT_DOUBLE_EQ(live.right.invert.value(), 1.0);
        EXPECT_FALSE(live.right.deadzone.has_value());
        EXPECT_FALSE(store.set_named("deadzone", 0.2));
        EXPECT_FALSE(store.set_named("left", 0.2));
        EXPECT_FALSE(store.set_named("left.deadzone.x", 0.2));
    }

    TEST(ControlValueStoreNestedTest, ReadingByPathFindsOnlyWhatWasSet)
    {
        NumericStore store;
        [[maybe_unused]] auto& live = store.declare<SlotPadTuning>();
        ASSERT_TRUE(store.set_named("left.deadzone", 0.2));
        const auto copy = store.snapshot<SlotPadTuning>();

        const auto found = NumericStore::read_named(copy, "left.deadzone");
        ASSERT_TRUE(found.has_value());
        EXPECT_DOUBLE_EQ(*found, 0.2);
        EXPECT_FALSE(NumericStore::read_named(copy, "right.deadzone").has_value());
        EXPECT_FALSE(NumericStore::read_named(copy, "left").has_value());
        EXPECT_FALSE(NumericStore::read_named(copy, "leftover.deadzone").has_value());
        EXPECT_FALSE(NumericStore::read_named(copy, "left.deadzone.x").has_value());
    }

    TEST(ControlValueStoreNestedTest, MergeReachesEveryDepthAndLeavesTheRestAlone)
    {
        NumericStore store;
        auto& live = store.declare<SlotPadTuning>();

        SlotPadTuning incoming;
        incoming.left.invert = ControlValue(true);
        store.merge(incoming);

        EXPECT_TRUE(live.left.invert.has_value());
        EXPECT_TRUE(live.left.invert.value<bool>());
        EXPECT_FALSE(live.left.deadzone.has_value());
        EXPECT_FALSE(live.right.invert.has_value());
        EXPECT_FALSE(live.scale.has_value());
    }

    TEST(ControlValueStoreNestedTest, ASnapshotIsACopyOfTheWholeNestedStruct)
    {
        NumericStore store;
        auto& live = store.declare<SlotPadTuning>();
        ASSERT_TRUE(store.set_named("left.deadzone", 0.2));

        auto copy = store.snapshot<SlotPadTuning>();
        copy.left.deadzone = ControlValue(0.9);

        EXPECT_DOUBLE_EQ(live.left.deadzone.value(), 0.2);
        EXPECT_DOUBLE_EQ(copy.left.deadzone.value(), 0.9);
    }

    TEST(ControlValueStoreNestedTest, TheScaleMirrorsOnlyTheTopLevelMemberAndOnlyWhenDue)
    {
        NumericStore store;
        [[maybe_unused]] auto& live = store.declare<SlotPadTuning>();
        double scale = 1.0;

        ASSERT_TRUE(store.set_named("scale", 2.0));
        store.refresh(&scale);
        EXPECT_DOUBLE_EQ(scale, 2.0);

        scale = 9.0;
        store.refresh(&scale);
        EXPECT_DOUBLE_EQ(scale, 9.0);
    }

    TEST(ControlValueStoreNestedTest, SettlingByNameDropsThePendingValue)
    {
        NumericStore store;
        auto& live = store.declare<SlotFlatTuning>();
        ASSERT_TRUE(store.set_named("decay", 0.5));

        EXPECT_TRUE(store.settle_named("decay"));
        EXPECT_FALSE(live.decay.due());
        EXPECT_FALSE(store.settle_named("missing"));
    }

    TEST(ControlValueStoreNestedTest, DeclaringAgainStartsFromAnEmptyStruct)
    {
        NumericStore store;
        [[maybe_unused]] auto& first = store.declare<SlotFlatTuning>();
        ASSERT_TRUE(store.set_named("decay", 1.0));

        auto& second = store.declare<SlotFlatTuning>();

        EXPECT_FALSE(second.decay.has_value());
        EXPECT_TRUE(store.set_named("decay", 0.5));
        EXPECT_DOUBLE_EQ(second.decay.value(), 0.5);
    }

    TEST(ControlValueStoreNestedTest, AStructOfAnotherTypeIsIgnoredOnMerge)
    {
        NumericStore store;
        auto& live = store.declare<SlotFlatTuning>();

        SlotPadTuning other;
        other.scale = ControlValue(3.0);
        store.merge(other);

        EXPECT_FALSE(live.scale.has_value());
    }

    TEST(ControlSnapshotTest, AnEmptySnapshotHoldsNothingAndIsNeverDue)
    {
        ControlSnapshot<std::string> slot;

        EXPECT_FALSE(slot.has_value());
        EXPECT_EQ(slot.value(), nullptr);
        EXPECT_FALSE(slot.due());
        EXPECT_EQ(slot.pending(), nullptr);
    }

    TEST(ControlSnapshotTest, AValueIsPendingOnceUntilItChanges)
    {
        ControlSnapshot<std::string> slot;

        slot.set("first");
        EXPECT_TRUE(slot.due());
        const auto taken = slot.pending();
        ASSERT_NE(taken, nullptr);
        EXPECT_EQ(*taken, "first");
        EXPECT_FALSE(slot.due());
        EXPECT_EQ(slot.pending(), nullptr);

        slot.set("second");
        ASSERT_NE(slot.pending(), nullptr);
    }

    TEST(ControlSnapshotTest, AReaderKeepsTheSnapshotItTook)
    {
        ControlSnapshot<std::string> slot("before");
        const auto kept = slot.value();

        slot.set("after");

        ASSERT_NE(kept, nullptr);
        EXPECT_EQ(*kept, "before");
        EXPECT_EQ(*slot.value(), "after");
    }

    TEST(ControlSnapshotTest, ACopyAndAnAssignmentCarryTheValueAndMakeItDue)
    {
        ControlSnapshot<std::vector<int>> original(std::vector<int> { 1, 2, 3 });
        const ControlSnapshot<std::vector<int>> copy(original);
        ControlSnapshot<std::vector<int>> assigned;

        assigned.assign(original);

        ASSERT_NE(copy.value(), nullptr);
        EXPECT_EQ(copy.value()->size(), 3U);
        EXPECT_TRUE(copy.due());
        ASSERT_NE(assigned.value(), nullptr);
        EXPECT_EQ(assigned.value()->at(2), 3);
        EXPECT_TRUE(assigned.due());
    }

    TEST(ControlSnapshotTest, AStoreHoldsASlotKindThatIsNotANumber)
    {
        LabelStore store;
        auto& live = store.declare<SlotLabelTuning>();

        SlotLabelTuning incoming;
        incoming.label.set("hello");
        store.merge(incoming);
        store.refresh();

        ASSERT_NE(live.label.value(), nullptr);
        EXPECT_EQ(*live.label.value(), "hello");
        EXPECT_FALSE(live.note.has_value());
        EXPECT_TRUE(store.settle_named("label"));
        EXPECT_FALSE(live.label.due());
    }

    TEST(ControlValueTableTest, EachKeyOwnsItsOwnStore)
    {
        ControlValueTable<int, ControlValue> table;
        auto& three = table.declare<SlotFlatTuning>(3);
        auto& one = table.declare<SlotFlatTuning>(1);

        ASSERT_NE(table.find(3), nullptr);
        EXPECT_EQ(table.find(2), nullptr);
        EXPECT_TRUE(table.find(3)->set_named("decay", 0.4));

        EXPECT_EQ(table.size(), 2U);
        EXPECT_DOUBLE_EQ(three.decay.value(), 0.4);
        EXPECT_FALSE(one.decay.has_value());
    }

    TEST(ControlValueTableTest, VisitingFollowsKeyOrderAndErasingDropsAStore)
    {
        ControlValueTable<int, ControlValue> table;
        [[maybe_unused]] auto& three = table.declare<SlotFlatTuning>(3);
        [[maybe_unused]] auto& one = table.declare<SlotFlatTuning>(1);

        std::vector<int> keys;
        table.for_each([&](int key, NumericStore&) { keys.push_back(key); });
        EXPECT_EQ(keys, (std::vector<int> { 1, 3 }));

        EXPECT_TRUE(table.erase(3));
        EXPECT_FALSE(table.erase(3));
        EXPECT_EQ(table.find(3), nullptr);
        table.refresh();
        EXPECT_EQ(table.size(), 1U);
    }

    TEST(ControlValueTableTest, DeclaringAKeyAgainReplacesItsStore)
    {
        ControlValueTable<int, ControlValue> table;
        [[maybe_unused]] auto& first = table.declare<SlotFlatTuning>(7);
        ASSERT_TRUE(table.find(7)->set_named("decay", 1.0));

        auto& second = table.declare<SlotFlatTuning>(7);

        EXPECT_EQ(table.size(), 1U);
        EXPECT_FALSE(second.decay.has_value());
    }

    TEST(ControlValueTextTest, ASlotIsTextDecodable)
    {
        static_assert(Reflect::TextDecodable<ControlValue>);
        EXPECT_EQ(Reflect::TextCodec<ControlValue>::extent, 0U);
    }

    TEST(ControlValueTextTest, TheCountOfNumbersPicksTheShape)
    {
        Reflect::TextError error = Reflect::TextError::Invalid;

        ControlValue number;
        EXPECT_TRUE(Reflect::TextCodec<ControlValue>::decode("0.25", number, error));
        EXPECT_EQ(number.width(), 1U);
        EXPECT_DOUBLE_EQ(number.value(), 0.25);
        EXPECT_TRUE(number.due());

        ControlValue vector;
        EXPECT_TRUE(Reflect::TextCodec<ControlValue>::decode("1 2 3", vector, error));
        EXPECT_EQ(vector.width(), 3U);
        EXPECT_FLOAT_EQ(vector.value<glm::vec3>().z, 3.F);

        ControlValue box;
        EXPECT_TRUE(Reflect::TextCodec<ControlValue>::decode("0 0 0 1 1 1", box, error));
        EXPECT_EQ(box.width(), 6U);
        EXPECT_FLOAT_EQ(box.value<SlotBox>().max.x, 1.F);
    }

    TEST(ControlValueTextTest, TrueAndFalseDecodeAsABool)
    {
        Reflect::TextError error = Reflect::TextError::Invalid;
        ControlValue slot;

        EXPECT_TRUE(Reflect::TextCodec<ControlValue>::decode("true", slot, error));
        EXPECT_TRUE(slot.value<bool>());
        EXPECT_TRUE(Reflect::TextCodec<ControlValue>::decode("false", slot, error));
        EXPECT_FALSE(slot.value<bool>());
    }

    TEST(ControlValueTextTest, AWrongCountOrABadTokenIsInvalidAndLeavesTheSlotAlone)
    {
        Reflect::TextError error = Reflect::TextError::Cardinality;
        ControlValue slot(0.5);
        slot.settle();

        EXPECT_FALSE(Reflect::TextCodec<ControlValue>::decode("1 2 3 4 5", slot, error));
        EXPECT_EQ(error, Reflect::TextError::Invalid);
        error = Reflect::TextError::Cardinality;
        EXPECT_FALSE(Reflect::TextCodec<ControlValue>::decode("wide", slot, error));
        EXPECT_EQ(error, Reflect::TextError::Invalid);
        EXPECT_FALSE(Reflect::TextCodec<ControlValue>::decode("", slot, error));

        EXPECT_DOUBLE_EQ(slot.value(), 0.5);
        EXPECT_FALSE(slot.due());
    }

}
}
