#include "gtest/gtest.h"
#include "../../support/test_metadata.hpp"

#include "MayaFlux/Kakshya/Region/RegionSequencer.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Kakshya::Region;
    using Kakshya::RegionGroup;
    using Kakshya::RegionSelectionPattern;
    using Kakshya::RegionSequencer;
    using Kakshya::RegionState;
    using Kakshya::RegionTransition;

    using Indices = std::vector<std::optional<size_t>>;

    RegionGroup group_of(size_t count, RegionSelectionPattern pattern)
    {
        RegionGroup group("walk");
        for (size_t i = 0; i < count; ++i) {
            group.add_region(Region::time_span(10 * i, 10 * i + 9, "r" + std::to_string(i)));
        }
        group.region_selection_pattern = pattern;
        return group;
    }

    Indices draw(RegionSequencer& sequencer, size_t count)
    {
        Indices out;
        for (size_t i = 0; i < count; ++i) {
            out.push_back(sequencer.next());
        }
        return out;
    }

    TEST(RegionSequencerTest, SequentialWalksTheGroupInOrderAndThenEnds)
    {
        RegionSequencer sequencer(group_of(3, RegionSelectionPattern::SEQUENTIAL));

        EXPECT_EQ(draw(sequencer, 5), (Indices { 0, 1, 2, std::nullopt, std::nullopt }));
        EXPECT_EQ(sequencer.current(), std::optional<size_t> { 2 });
    }

    TEST(RegionSequencerTest, ResetStartsTheWalkAgain)
    {
        RegionSequencer sequencer(group_of(3, RegionSelectionPattern::SEQUENTIAL));
        (void)draw(sequencer, 4);

        sequencer.reset();

        EXPECT_FALSE(sequencer.current().has_value());
        EXPECT_EQ(draw(sequencer, 2), (Indices { 0, 1 }));
    }

    TEST(RegionSequencerTest, RoundRobinWraps)
    {
        RegionSequencer sequencer(group_of(3, RegionSelectionPattern::ROUND_ROBIN));

        EXPECT_EQ(draw(sequencer, 7), (Indices { 0, 1, 2, 0, 1, 2, 0 }));
    }

    TEST(RegionSequencerTest, ExclusiveStaysOnTheGroupsCurrentIndex)
    {
        auto group = group_of(3, RegionSelectionPattern::EXCLUSIVE);
        group.current_region_index = 1;
        RegionSequencer sequencer(group);

        EXPECT_EQ(draw(sequencer, 3), (Indices { 1, 1, 1 }));
    }

    TEST(RegionSequencerTest, ExclusiveClampsAnIndexPastTheEnd)
    {
        auto group = group_of(3, RegionSelectionPattern::EXCLUSIVE);
        group.current_region_index = 9;
        RegionSequencer sequencer(group);

        EXPECT_EQ(sequencer.next(), std::optional<size_t> { 2 });
    }

    TEST(RegionSequencerTest, RandomRepeatsForASeedAndStaysInRange)
    {
        RegionSequencer first(group_of(5, RegionSelectionPattern::RANDOM), 42);
        RegionSequencer second(group_of(5, RegionSelectionPattern::RANDOM), 42);

        const auto a = draw(first, 40);
        const auto b = draw(second, 40);

        EXPECT_EQ(a, b);
        for (const auto& index : a) {
            ASSERT_TRUE(index.has_value());
            EXPECT_LT(*index, 5U);
        }
    }

    TEST(RegionSequencerTest, RandomDiffersBetweenSeedsAndReachesEveryRegion)
    {
        RegionSequencer first(group_of(3, RegionSelectionPattern::RANDOM), 1);
        RegionSequencer second(group_of(3, RegionSelectionPattern::RANDOM), 2);

        const auto a = draw(first, 60);
        const auto b = draw(second, 60);

        EXPECT_NE(a, b);
        for (size_t region = 0; region < 3; ++region) {
            EXPECT_NE(std::ranges::find(a, std::optional<size_t> { region }), a.end()) << region;
        }
    }

    TEST(RegionSequencerTest, ResetRepeatsARandomWalk)
    {
        RegionSequencer sequencer(group_of(4, RegionSelectionPattern::RANDOM), 9);
        const auto first = draw(sequencer, 20);

        sequencer.reset();

        EXPECT_EQ(draw(sequencer, 20), first);
    }

    TEST(RegionSequencerTest, WeightedFollowsTheWeightOfEachRegion)
    {
        const Facet<double> weight;
        auto group = group_of(3, RegionSelectionPattern::WEIGHTED);
        group.regions[0].set(weight, 0.0);
        group.regions[1].set(weight, 5.0);
        group.regions[2].set(weight, 0.0);
        RegionSequencer sequencer(group, 3);
        sequencer.set_weight_key(weight);

        const auto drawn = draw(sequencer, 25);

        for (const auto& index : drawn) {
            EXPECT_EQ(index, std::optional<size_t> { 1 });
        }
    }

    TEST(RegionSequencerTest, WeightedWithoutAKeyIsUniformOverTheGroup)
    {
        RegionSequencer sequencer(group_of(3, RegionSelectionPattern::WEIGHTED), 5);

        const auto drawn = draw(sequencer, 60);

        for (size_t region = 0; region < 3; ++region) {
            EXPECT_NE(std::ranges::find(drawn, std::optional<size_t> { region }), drawn.end()) << region;
        }
    }

    TEST(RegionSequencerTest, WeightedWithEveryWeightZeroEndsTheWalk)
    {
        const Facet<double> weight;
        auto group = group_of(2, RegionSelectionPattern::WEIGHTED);
        group.regions[0].set(weight, 0.0);
        group.regions[1].set(weight, 0.0);
        RegionSequencer sequencer(group);
        sequencer.set_weight_key(weight);

        EXPECT_FALSE(sequencer.next().has_value());
    }

    TEST(RegionSequencerTest, CustomTakesItsChoiceFromTheChooserWhichSeesTheCurrentIndex)
    {
        RegionSequencer sequencer(group_of(4, RegionSelectionPattern::CUSTOM));
        std::vector<std::optional<size_t>> seen;
        sequencer.set_chooser([&seen](const RegionGroup&, std::optional<size_t> current) -> std::optional<size_t> {
            seen.push_back(current);
            return current ? (*current + 2) % 4 : 3;
        });

        EXPECT_EQ(draw(sequencer, 3), (Indices { 3, 1, 3 }));
        EXPECT_EQ(seen, (Indices { std::nullopt, 3, 1 }));
    }

    TEST(RegionSequencerTest, CustomEndsWhenTheChooserDoesOrPicksOutOfRange)
    {
        RegionSequencer ends(group_of(2, RegionSelectionPattern::CUSTOM));
        ends.set_chooser([](const RegionGroup&, std::optional<size_t>) -> std::optional<size_t> { return std::nullopt; });
        RegionSequencer outside(group_of(2, RegionSelectionPattern::CUSTOM));
        outside.set_chooser([](const RegionGroup&, std::optional<size_t>) -> std::optional<size_t> { return 7; });
        RegionSequencer none(group_of(2, RegionSelectionPattern::CUSTOM));

        EXPECT_FALSE(ends.next().has_value());
        EXPECT_FALSE(outside.next().has_value());
        EXPECT_FALSE(none.next().has_value());
    }

    TEST(RegionSequencerTest, AnEmptyGroupHasNothingToWalk)
    {
        RegionSequencer sequencer(group_of(0, RegionSelectionPattern::ROUND_ROBIN));

        EXPECT_FALSE(sequencer.next().has_value());
    }

    TEST(RegionSequencerTest, JumpMakesARegionCurrentAndTheWalkContinuesFromIt)
    {
        RegionSequencer sequencer(group_of(4, RegionSelectionPattern::SEQUENTIAL));

        sequencer.jump(1);

        EXPECT_EQ(sequencer.current(), std::optional<size_t> { 1 });
        EXPECT_EQ(sequencer.next(), std::optional<size_t> { 2 });
        EXPECT_THROW(sequencer.jump(4), std::out_of_range);
    }

    TEST(RegionSequencerTest, EachRegionHasARecordCarryingTheGroupAndRegionSettings)
    {
        const Facet<int> from_group;
        const Facet<int> from_region;
        auto group = group_of(2, RegionSelectionPattern::SEQUENTIAL);
        group.transition_type = RegionTransition::GATED;
        group.transition_duration_ms = 12.5;
        group.set(from_group, 7);
        group.regions[1].set(from_region, 9);

        RegionSequencer sequencer(group);

        const auto& records = sequencer.regions();
        ASSERT_EQ(records.size(), 2U);
        EXPECT_EQ(records[1].group_name, "walk");
        EXPECT_EQ(records[1].region_index, 1U);
        EXPECT_EQ(records[1].transition_type, RegionTransition::GATED);
        EXPECT_DOUBLE_EQ(records[1].transition_duration_ms, 12.5);
        EXPECT_EQ(records[1].selection_pattern, RegionSelectionPattern::SEQUENTIAL);
        EXPECT_EQ(records[1].current_position, group.regions[1].start_coordinates);
        ASSERT_NE(records[1].get(from_group), nullptr);
        EXPECT_EQ(*records[1].get(from_group), 7);
        ASSERT_NE(records[1].get(from_region), nullptr);
        EXPECT_EQ(*records[1].get(from_region), 9);
        EXPECT_EQ(records[0].get(from_region), nullptr);
    }

    TEST(RegionSequencerTest, TheRecordsFollowTheWalkThroughTheirStates)
    {
        RegionSequencer sequencer(group_of(2, RegionSelectionPattern::SEQUENTIAL));
        EXPECT_EQ(sequencer.regions()[0].state, RegionState::READY);

        (void)sequencer.next();
        EXPECT_EQ(sequencer.regions()[0].state, RegionState::ACTIVE);

        (void)sequencer.next();
        EXPECT_EQ(sequencer.regions()[0].state, RegionState::READY);
        EXPECT_EQ(sequencer.regions()[1].state, RegionState::ACTIVE);
    }

    TEST(RegionSequencerTest, UnsupportedPatternsAndTransitionsAreRejectedWithAReason)
    {
        for (const auto pattern : { RegionSelectionPattern::ALL, RegionSelectionPattern::OVERLAP }) {
            const auto group = group_of(2, pattern);
            EXPECT_FALSE(RegionSequencer::unsupported_reason(group).empty()) << static_cast<int>(pattern);
            EXPECT_THROW(RegionSequencer { group }, std::invalid_argument) << static_cast<int>(pattern);
        }
        for (const auto transition : { RegionTransition::CROSSFADE, RegionTransition::OVERLAP }) {
            auto group = group_of(2, RegionSelectionPattern::SEQUENTIAL);
            group.transition_type = transition;
            EXPECT_FALSE(RegionSequencer::unsupported_reason(group).empty()) << static_cast<int>(transition);
            EXPECT_THROW(RegionSequencer { group }, std::invalid_argument) << static_cast<int>(transition);
        }
    }

    TEST(RegionSequencerTest, EverySupportedPatternAndTransitionIsAccepted)
    {
        for (const auto pattern : { RegionSelectionPattern::SEQUENTIAL, RegionSelectionPattern::ROUND_ROBIN,
                 RegionSelectionPattern::RANDOM, RegionSelectionPattern::WEIGHTED,
                 RegionSelectionPattern::EXCLUSIVE, RegionSelectionPattern::CUSTOM }) {
            EXPECT_TRUE(RegionSequencer::unsupported_reason(group_of(2, pattern)).empty()) << static_cast<int>(pattern);
        }
        for (const auto transition : { RegionTransition::IMMEDIATE, RegionTransition::GATED, RegionTransition::CALLBACK }) {
            auto group = group_of(2, RegionSelectionPattern::SEQUENTIAL);
            group.transition_type = transition;
            EXPECT_TRUE(RegionSequencer::unsupported_reason(group).empty()) << static_cast<int>(transition);
        }
    }

} // namespace
} // namespace MayaFlux::Test
