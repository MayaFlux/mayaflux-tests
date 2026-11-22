#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Transitive/Facet/ControlBind.hpp"
#include "MayaFlux/Transitive/Facet/ControlValueStore.hpp"

#include <glm/gtc/quaternion.hpp>

#include "control_store_fixtures.hpp"
#include "control_value_fixtures.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using NumericStore = ControlValueStore<ControlValue>;

    class UnsettableSlot : public ControlValue {
    public:
        UnsettableSlot() = default;

        template <typename T>
            requires is_value<T>
        UnsettableSlot(T value) noexcept
            : ControlValue(value)
        {
        }

        [[nodiscard]] static UnsettableSlot unset() noexcept
        {
            UnsettableSlot slot;
            slot.mark_cleared();
            return slot;
        }
    };

    static_assert(ControlValue::is_value<glm::quat>);
    static_assert(ControlValue::is_rotation<glm::quat>);
    static_assert(!ControlValue::is_rotation<glm::vec4>);
    static_assert(ControlSlot<UnsettableSlot>);
    static_assert(NumericControlSlot<UnsettableSlot>);

    struct StoreFakeHost {
        using Controls = StoreBloomTuning;

        StoreFakeHost()
            : live(store.declare<StoreBloomTuning>())
        {
        }

        void set(const Controls& incoming) { store.merge(incoming); }

        [[nodiscard]] Controls get() const { return store.snapshot<Controls>(); }

        NumericStore store;
        StoreBloomTuning& live;
    };

    TEST(ControlValueRotationTest, AQuaternionIsHeldWholeAndReadBackAsItself)
    {
        const glm::quat rotation(0.5F, 0.1F, 0.2F, 0.3F);
        ControlValue slot(rotation);

        EXPECT_TRUE(slot.has_value());
        EXPECT_EQ(slot.width(), 4U);

        const auto taken = slot.pending<glm::quat>();
        ASSERT_TRUE(taken.has_value());
        EXPECT_FLOAT_EQ(taken->w, 0.5F);
        EXPECT_FLOAT_EQ(taken->x, 0.1F);
        EXPECT_FLOAT_EQ(taken->y, 0.2F);
        EXPECT_FLOAT_EQ(taken->z, 0.3F);
        EXPECT_FALSE(slot.pending<glm::quat>().has_value());
    }

    TEST(ControlValueRotationTest, AQuaternionIsStoredInXyzwOrderAndReadsAsAVec4)
    {
        ControlValue slot(glm::quat(0.5F, 0.1F, 0.2F, 0.3F));

        const auto vector = slot.value<glm::vec4>();

        EXPECT_FLOAT_EQ(vector.x, 0.1F);
        EXPECT_FLOAT_EQ(vector.y, 0.2F);
        EXPECT_FLOAT_EQ(vector.z, 0.3F);
        EXPECT_FLOAT_EQ(vector.w, 0.5F);
    }

    TEST(ControlValueRotationTest, AQuaternionIsNeverReadAsANumberOrAThreeVector)
    {
        ControlValue slot(glm::quat(1.0F, 0.0F, 0.0F, 0.0F));

        EXPECT_FALSE(slot.pending().has_value());
        EXPECT_FALSE(slot.pending<glm::vec3>().has_value());
        EXPECT_TRUE(slot.pending<glm::quat>().has_value());
    }

    TEST(ControlValueRotationTest, ObservingAQuaternionShowsItWithoutMakingItDue)
    {
        ControlValue slot;

        slot.observe(glm::quat(0.0F, 1.0F, 0.0F, 0.0F));

        EXPECT_TRUE(slot.has_value());
        EXPECT_FLOAT_EQ(slot.value<glm::quat>().x, 1.0F);
        EXPECT_FALSE(slot.due());
    }

    TEST(ControlValueRotationTest, ACopyOfAQuaternionKeepsEveryComponent)
    {
        const ControlValue original(glm::quat(0.5F, 0.1F, 0.2F, 0.3F));

        const ControlValue copy = original;

        EXPECT_FLOAT_EQ(copy.value<glm::quat>().w, 0.5F);
        EXPECT_FLOAT_EQ(copy.value<glm::quat>().z, 0.3F);
        EXPECT_TRUE(copy.due());
    }

    TEST(ControlValueClearTest, AnUnsetSlotRequestsAClearAndNoOtherSlotDoes)
    {
        EXPECT_TRUE(UnsettableSlot::unset().clear_requested());
        EXPECT_FALSE(UnsettableSlot().clear_requested());
        EXPECT_FALSE(ControlValue().clear_requested());
        EXPECT_FALSE(ControlValue(1.0).clear_requested());
    }

    TEST(ControlValueClearTest, AnUnsetSlotHoldsNothingAndIsNeverDue)
    {
        const UnsettableSlot unset = UnsettableSlot::unset();

        EXPECT_FALSE(unset.has_value());
        EXPECT_EQ(unset.width(), 0U);
        EXPECT_FALSE(unset.due());
    }

    TEST(ControlValueClearTest, AssigningAnUnsetSlotEmptiesAValueAndLeavesItNotDue)
    {
        UnsettableSlot live(2.0);

        live.assign(UnsettableSlot::unset());

        EXPECT_FALSE(live.has_value());
        EXPECT_FALSE(live.due());
        EXPECT_FALSE(live.pending().has_value());
    }

    TEST(ControlValueClearTest, ASlotHoldsAValueAgainAfterAClearAndStopsRequesting)
    {
        UnsettableSlot live(1.0);
        live.assign(UnsettableSlot::unset());
        ASSERT_TRUE(live.clear_requested());

        live.assign(ControlValue(3.0));

        EXPECT_TRUE(live.has_value());
        EXPECT_TRUE(live.due());
        EXPECT_FALSE(live.clear_requested());
        EXPECT_DOUBLE_EQ(live.value(), 3.0);
    }

    TEST(ControlValueClearTest, ObservingAfterAClearHoldsTheValueAndStopsRequesting)
    {
        UnsettableSlot live(1.0);
        live.assign(UnsettableSlot::unset());

        live.observe(4.0);

        EXPECT_TRUE(live.has_value());
        EXPECT_FALSE(live.clear_requested());
        EXPECT_FALSE(live.due());
    }

    TEST(ControlValueClearTest, ACopyOfAnUnsetSlotKeepsTheRequest)
    {
        const UnsettableSlot original = UnsettableSlot::unset();

        const UnsettableSlot copy = original;

        EXPECT_TRUE(copy.clear_requested());
        EXPECT_FALSE(copy.has_value());
    }

    TEST(ControlValueStoreMixedTest, AStructOfTwoSlotTypesMergesBoth)
    {
        NumericStore store;
        auto& live = store.declare<StoreMixedTuning>();

        StoreMixedTuning incoming;
        incoming.gain = ControlValue(0.5);
        incoming.label.set(std::string("lead"));
        store.merge(incoming);

        EXPECT_DOUBLE_EQ(live.gain.value(), 0.5);
        EXPECT_TRUE(live.gain.due());
        ASSERT_TRUE(live.label.has_value());
        EXPECT_EQ(*live.label.value(), "lead");
        EXPECT_TRUE(live.label.due());
    }

    TEST(ControlValueStoreMixedTest, AnEmptyMemberOfEitherTypeIsLeftAlone)
    {
        NumericStore store;
        auto& live = store.declare<StoreMixedTuning>();
        live.gain.assign(ControlValue(0.5));
        live.label.set(std::string("lead"));

        store.merge(StoreMixedTuning {});

        EXPECT_DOUBLE_EQ(live.gain.value(), 0.5);
        EXPECT_EQ(*live.label.value(), "lead");
    }

    TEST(ControlValueStoreMixedTest, ASnapshotCopiesBothSlotTypes)
    {
        NumericStore store;
        auto& live = store.declare<StoreMixedTuning>();
        live.gain.assign(ControlValue(0.5));
        live.label.set(std::string("lead"));

        auto copy = store.snapshot<StoreMixedTuning>();
        copy.label.set(std::string("pad"));

        EXPECT_DOUBLE_EQ(copy.gain.value(), 0.5);
        EXPECT_EQ(*copy.label.value(), "pad");
        EXPECT_EQ(*live.label.value(), "lead");
    }

    TEST(ControlValueStoreMixedTest, SettlingByNameReachesAMemberOfAnotherSlotType)
    {
        NumericStore store;
        auto& live = store.declare<StoreMixedTuning>();
        live.label.set(std::string("lead"));
        ASSERT_TRUE(live.label.due());

        EXPECT_TRUE(store.settle_named("label"));

        EXPECT_FALSE(live.label.due());
    }

    TEST(ControlValueStoreMixedTest, AMemberOfAnotherSlotTypeHasNoNamedNumberAccess)
    {
        NumericStore store;
        [[maybe_unused]] auto& live = store.declare<StoreMixedTuning>();

        EXPECT_FALSE(store.set_named("label", 1.0));
        EXPECT_EQ(store.find("label"), nullptr);
        EXPECT_FALSE(store.number("label").has_value());
        EXPECT_TRUE(store.set_named("gain", 1.0));
    }

    TEST(ControlValueStoreExtendTest, AnExtensionIsNamedByItsStructAndAMember)
    {
        NumericStore store;
        [[maybe_unused]] auto& primary = store.declare<StoreMixedTuning>();
        auto& bloom = store.extend<StoreBloomTuning>();

        EXPECT_TRUE(store.set_named("StoreBloomTuning.rate", 0.3));

        EXPECT_DOUBLE_EQ(bloom.rate.value(), 0.3);
        EXPECT_TRUE(bloom.rate.due());
        EXPECT_FALSE(store.set_named("rate", 0.3));
        EXPECT_FALSE(store.set_named("StoreBloomTuning.missing", 0.3));
    }

    TEST(ControlValueStoreExtendTest, TheDeclaredStructKeepsItsUnprefixedNames)
    {
        NumericStore store;
        auto& primary = store.declare<StoreMixedTuning>();
        [[maybe_unused]] auto& bloom = store.extend<StoreBloomTuning>();

        EXPECT_TRUE(store.set_named("gain", 2.0));

        EXPECT_DOUBLE_EQ(primary.gain.value(), 2.0);
    }

    TEST(ControlValueStoreExtendTest, ExtendingATypeAlreadyHeldReturnsTheSameStruct)
    {
        NumericStore store;
        auto& primary = store.declare<StoreMixedTuning>();

        auto& first = store.extend<StoreBloomTuning>();
        auto& second = store.extend<StoreBloomTuning>();
        auto& again = store.extend<StoreMixedTuning>();

        EXPECT_EQ(&first, &second);
        EXPECT_EQ(&again, &primary);
    }

    TEST(ControlValueStoreExtendTest, MergeAndSnapshotFindAnExtensionByItsType)
    {
        NumericStore store;
        [[maybe_unused]] auto& primary = store.declare<StoreMixedTuning>();
        auto& bloom = store.extend<StoreBloomTuning>();

        StoreBloomTuning incoming;
        incoming.angle = ControlValue(0.7);
        store.merge(incoming);

        EXPECT_DOUBLE_EQ(bloom.angle.value(), 0.7);
        EXPECT_FALSE(bloom.rate.has_value());
        const auto copy = store.snapshot<StoreBloomTuning>();
        EXPECT_DOUBLE_EQ(copy.angle.value(), 0.7);
        EXPECT_FALSE(store.snapshot<StoreMixedTuning>().gain.has_value());
    }

    TEST(ControlValueStoreExtendTest, MergingATypeTheStoreNeverHeldChangesNothing)
    {
        NumericStore store;
        auto& primary = store.declare<StoreMixedTuning>();

        StoreBloomTuning incoming;
        incoming.rate = ControlValue(1.0);
        store.merge(incoming);

        EXPECT_FALSE(primary.gain.has_value());
        EXPECT_EQ(store.live<StoreBloomTuning>(), nullptr);
        EXPECT_FALSE(store.snapshot<StoreBloomTuning>().rate.has_value());
    }

    TEST(ControlValueStoreExtendTest, OnlyTheDeclaredStructsScaleIsMirrored)
    {
        NumericStore store;
        [[maybe_unused]] auto& primary = store.declare<SlotFlatTuning>();
        [[maybe_unused]] auto& extra = store.extend<StoreScaleExtension>();
        double scale = 1.0;

        ASSERT_TRUE(store.set_named("StoreScaleExtension.scale", 5.0));
        store.refresh(&scale);
        EXPECT_DOUBLE_EQ(scale, 1.0);

        ASSERT_TRUE(store.set_named("scale", 2.0));
        store.refresh(&scale);
        EXPECT_DOUBLE_EQ(scale, 2.0);
    }

    TEST(ControlValueStoreExtendTest, DeclaringAgainDropsEveryExtension)
    {
        NumericStore store;
        [[maybe_unused]] auto& primary = store.declare<StoreMixedTuning>();
        [[maybe_unused]] auto& bloom = store.extend<StoreBloomTuning>();
        ASSERT_NE(store.live<StoreBloomTuning>(), nullptr);

        [[maybe_unused]] auto& fresh = store.declare<StoreMixedTuning>();

        EXPECT_EQ(store.live<StoreBloomTuning>(), nullptr);
        EXPECT_FALSE(store.set_named("StoreBloomTuning.rate", 0.3));
    }

    TEST(ControlValueStoreAccessTest, LiveIsTheHeldStructAndNotACopy)
    {
        NumericStore store;
        auto& declared = store.declare<SlotFlatTuning>();

        EXPECT_EQ(store.live<SlotFlatTuning>(), &declared);
        EXPECT_EQ(store.live<SlotPadTuning>(), nullptr);
    }

    TEST(ControlValueStoreAccessTest, FindAnswersTheSlotOfAName)
    {
        NumericStore store;
        auto& declared = store.declare<SlotPadTuning>();

        EXPECT_EQ(store.find("scale"), &declared.scale);
        EXPECT_EQ(store.find("left.invert"), &declared.left.invert);
        EXPECT_EQ(store.find("left"), nullptr);
        EXPECT_EQ(store.find("missing"), nullptr);
    }

    TEST(ControlValueStoreAccessTest, ForEachVisitsEveryMemberInDeclarationOrder)
    {
        NumericStore store;
        [[maybe_unused]] auto& declared = store.declare<SlotPadTuning>();
        [[maybe_unused]] auto& bloom = store.extend<StoreBloomTuning>();

        std::vector<std::string> names;
        store.for_each([&names](std::string_view name, const ControlValue&) { names.emplace_back(name); });

        const std::vector<std::string> expected {
            "scale", "left.deadzone", "left.invert", "right.deadzone", "right.invert",
            "StoreBloomTuning.rate", "StoreBloomTuning.angle"
        };
        EXPECT_EQ(names, expected);
    }

    TEST(ControlValueStoreAccessTest, NumberReadsTheLiveValueByName)
    {
        NumericStore store;
        [[maybe_unused]] auto& declared = store.declare<SlotFlatTuning>();
        ASSERT_TRUE(store.set_named("decay", 0.5));

        const auto found = store.number("decay");

        ASSERT_TRUE(found.has_value());
        EXPECT_DOUBLE_EQ(*found, 0.5);
        EXPECT_FALSE(store.number("scale").has_value());
        EXPECT_FALSE(store.number("missing").has_value());
    }

    TEST(ControlValueStoreAccessTest, NumberReadsAnExtensionMemberAndNeverAVector)
    {
        NumericStore store;
        [[maybe_unused]] auto& primary = store.declare<SlotFlatTuning>();
        auto& bloom = store.extend<StoreBloomTuning>();
        ASSERT_TRUE(store.set_named("StoreBloomTuning.rate", 0.25));
        bloom.angle.assign(ControlValue(glm::vec3(1.0F)));

        const auto rate = store.number("StoreBloomTuning.rate");

        ASSERT_TRUE(rate.has_value());
        EXPECT_DOUBLE_EQ(*rate, 0.25);
        EXPECT_FALSE(store.number("StoreBloomTuning.angle").has_value());
    }

    TEST(ControlValueStoreAccessTest, AWholeSlotSetByNameKeepsItsWidth)
    {
        NumericStore store;
        auto& declared = store.declare<SlotFlatTuning>();

        EXPECT_TRUE(store.set_named("decay", ControlValue(glm::vec3(1.0F, 2.0F, 3.0F))));

        EXPECT_EQ(declared.decay.value<glm::vec3>(), glm::vec3(1.0F, 2.0F, 3.0F));
        EXPECT_TRUE(declared.decay.due());
    }

    TEST(ControlValueStoreAccessTest, AWholeSlotOfAnotherWidthIsRefusedByAMemberThatHoldsAValue)
    {
        NumericStore store;
        auto& declared = store.declare<SlotFlatTuning>();
        ASSERT_TRUE(store.set_named("decay", ControlValue(glm::vec3(1.0F))));

        EXPECT_FALSE(store.set_named("decay", ControlValue(0.5)));
        EXPECT_EQ(declared.decay.value<glm::vec3>(), glm::vec3(1.0F));
        EXPECT_TRUE(store.set_named("scale", ControlValue(glm::vec2(1.0F, 2.0F))));
        EXPECT_FALSE(store.set_named("missing", ControlValue(1.0)));
    }

    TEST(ControlHeldTest, AHeldCopyKeepsTheValuesOfTheMomentItWasTaken)
    {
        NumericStore store;
        auto& declared = store.declare<SlotFlatTuning>();
        ASSERT_TRUE(store.set_named("decay", 0.5));

        const ControlHeld held = store.held();
        ASSERT_TRUE(store.set_named("decay", 0.9));

        EXPECT_DOUBLE_EQ(held.get<SlotFlatTuning>().decay.value(), 0.5);
        EXPECT_DOUBLE_EQ(declared.decay.value(), 0.9);
    }

    TEST(ControlHeldTest, AHeldCopyIsReadBackByTypeForEveryStruct)
    {
        NumericStore store;
        [[maybe_unused]] auto& primary = store.declare<SlotFlatTuning>();
        [[maybe_unused]] auto& bloom = store.extend<StoreBloomTuning>();
        ASSERT_TRUE(store.set_named("decay", 0.5));
        ASSERT_TRUE(store.set_named("StoreBloomTuning.rate", 0.25));

        const ControlHeld held = store.held();

        EXPECT_DOUBLE_EQ(held.get<SlotFlatTuning>().decay.value(), 0.5);
        EXPECT_DOUBLE_EQ(held.get<StoreBloomTuning>().rate.value(), 0.25);
    }

    TEST(ControlHeldTest, ATypeThatWasNotHeldReadsAsADefaultStruct)
    {
        NumericStore store;
        [[maybe_unused]] auto& primary = store.declare<SlotFlatTuning>();

        const ControlHeld held = store.held();

        EXPECT_FALSE(held.get<StoreBloomTuning>().rate.has_value());
        EXPECT_FALSE(held.empty());
    }

    TEST(ControlHeldTest, AStoreThatDeclaredNothingHoldsNothing)
    {
        NumericStore store;

        const ControlHeld held = store.held();

        EXPECT_TRUE(held.empty());
        EXPECT_FALSE(held.get<SlotFlatTuning>().decay.has_value());
    }

    TEST(ControlHeldTest, AHeldCopyOutlivesTheStructItWasTakenFrom)
    {
        NumericStore store;
        [[maybe_unused]] auto& declared = store.declare<SlotFlatTuning>();
        ASSERT_TRUE(store.set_named("decay", 0.5));
        const ControlHeld held = store.held();

        [[maybe_unused]] auto& fresh = store.declare<SlotFlatTuning>();

        EXPECT_DOUBLE_EQ(held.get<SlotFlatTuning>().decay.value(), 0.5);
    }

    TEST(ControlBindTest, AWriterSetsOnlyItsOwnMember)
    {
        const auto host = std::make_shared<StoreFakeHost>();

        const auto write = control_writer<&StoreBloomTuning::rate, double>(host);
        write(0.4);

        EXPECT_DOUBLE_EQ(host->live.rate.value(), 0.4);
        EXPECT_TRUE(host->live.rate.due());
        EXPECT_FALSE(host->live.angle.has_value());
    }

    TEST(ControlBindTest, AWriterSetsAVectorMember)
    {
        const auto host = std::make_shared<StoreFakeHost>();

        const auto write = control_writer<&StoreBloomTuning::angle, glm::vec2>(host);
        write(glm::vec2(1.0F, 2.0F));

        EXPECT_EQ(host->live.angle.value<glm::vec2>(), glm::vec2(1.0F, 2.0F));
        EXPECT_FALSE(host->live.rate.has_value());
    }

    TEST(ControlBindTest, AWriterKeepsItsHostAlive)
    {
        std::weak_ptr<StoreFakeHost> watch;
        std::function<void(const double&)> write;
        {
            const auto host = std::make_shared<StoreFakeHost>();
            watch = host;
            write = control_writer<&StoreBloomTuning::rate, double>(host);
        }

        EXPECT_FALSE(watch.expired());
        write = nullptr;
        EXPECT_TRUE(watch.expired());
    }

    TEST(ControlBindTest, AReaderAnswersNothingUntilTheMemberHoldsAValue)
    {
        const auto host = std::make_shared<StoreFakeHost>();
        const auto read = control_reader<&StoreBloomTuning::rate, double>(host);

        EXPECT_FALSE(read().has_value());

        host->set({ .rate = ControlValue(0.6) });

        const auto found = read();
        ASSERT_TRUE(found.has_value());
        EXPECT_DOUBLE_EQ(*found, 0.6);
    }

    TEST(ControlBindTest, AWriterAndAReaderOnOneMemberAgree)
    {
        const auto host = std::make_shared<StoreFakeHost>();
        const auto write = control_writer<&StoreBloomTuning::angle, glm::vec3>(host);
        const auto read = control_reader<&StoreBloomTuning::angle, glm::vec3>(host);

        write(glm::vec3(1.0F, 2.0F, 3.0F));

        const auto found = read();
        ASSERT_TRUE(found.has_value());
        EXPECT_EQ(*found, glm::vec3(1.0F, 2.0F, 3.0F));
    }

}
}
