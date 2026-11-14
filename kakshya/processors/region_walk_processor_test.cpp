#include "gtest/gtest.h"
#include "../../support/test_metadata.hpp"

#include "../../support/audio_container.hpp"

#include "MayaFlux/Kakshya/Processors/RegionWalkProcessor.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Kakshya::DataVariant;
    using Kakshya::OrganizationStrategy;
    using Kakshya::ProcessingState;
    using Kakshya::Region;
    using Kakshya::RegionDelivery;
    using Kakshya::RegionGroup;
    using Kakshya::RegionSelectionPattern;
    using Kakshya::RegionTransition;
    using Kakshya::RegionWalkProcessor;
    using Kakshya::SoundFileContainer;

    using Samples = std::vector<double>;
    using Contributions = std::vector<std::array<uint64_t, 3>>;

    constexpr uint64_t k_frames = 100;
    constexpr uint32_t k_rate = 1000;

    std::shared_ptr<SoundFileContainer> stereo(OrganizationStrategy organization = OrganizationStrategy::PLANAR)
    {
        std::vector<Samples> channels(2, Samples(k_frames));
        for (uint64_t channel = 0; channel < 2; ++channel) {
            for (uint64_t frame = 0; frame < k_frames; ++frame) {
                channels[channel][frame] = static_cast<double>(1000 * channel + frame);
            }
        }
        return make_audio_container(std::move(channels), k_rate, organization);
    }

    RegionGroup walk_group(RegionSelectionPattern pattern)
    {
        RegionGroup group("walk",
            { Region::audio_span(0, 9, 0, 1, "A"),
                Region::audio_span(20, 29, 0, 1, "B"),
                Region::audio_span(50, 59, 0, 1, "C") });
        group.region_selection_pattern = pattern;
        return group;
    }

    Samples range(uint64_t first, uint64_t count, double offset = 0.0)
    {
        Samples out(count);
        for (uint64_t i = 0; i < count; ++i) {
            out[i] = offset + static_cast<double>(first + i);
        }
        return out;
    }

    Samples joined(Samples a, const Samples& b)
    {
        a.insert(a.end(), b.begin(), b.end());
        return a;
    }

    Samples lane(const std::shared_ptr<SoundFileContainer>& container, size_t index)
    {
        return std::get<Samples>(container->get_processed_data().at(index));
    }

    Contributions contributions_of(const RegionWalkProcessor& processor)
    {
        Contributions out;
        for (const auto& c : processor.contributions()) {
            out.push_back({ c.region_index, c.first, c.count });
        }
        return out;
    }

    struct Walk {
        std::shared_ptr<SoundFileContainer> container;
        RegionWalkProcessor processor;

        Walk(RegionGroup group, RegionDelivery delivery, uint64_t block, OrganizationStrategy organization = OrganizationStrategy::PLANAR)
            : container(stereo(organization))
            , processor("walk", delivery, block)
        {
            container->add_region_group(group);
        }

        void attach() { processor.on_attach(container); }
        void step() { processor.process(container); }
    };

    // ---- block delivery --------------------------------------------------------------------

    TEST(RegionWalkBlockTest, TheFirstBlockIsTheStartOfTheFirstRegion)
    {
        Walk walk(walk_group(RegionSelectionPattern::SEQUENTIAL), RegionDelivery::BLOCK, 4);
        walk.attach();

        walk.step();

        EXPECT_EQ(lane(walk.container, 0), range(0, 4));
        EXPECT_EQ(lane(walk.container, 1), range(0, 4, 1000));
        EXPECT_EQ(contributions_of(walk.processor), (Contributions { { 0, 0, 4 } }));
        EXPECT_EQ(walk.container->get_processing_state(), ProcessingState::PROCESSED);
        EXPECT_EQ(walk.processor.current_region(), std::optional<size_t> { 0 });
    }

    TEST(RegionWalkBlockTest, ABlockContinuesIntoTheNextRegionAcrossABoundary)
    {
        Walk walk(walk_group(RegionSelectionPattern::SEQUENTIAL), RegionDelivery::BLOCK, 4);
        walk.attach();

        walk.step();
        walk.step();
        walk.step();

        EXPECT_EQ(lane(walk.container, 0), (Samples { 8, 9, 20, 21 }));
        EXPECT_EQ(lane(walk.container, 1), (Samples { 1008, 1009, 1020, 1021 }));
        EXPECT_EQ(contributions_of(walk.processor), (Contributions { { 0, 0, 2 }, { 1, 2, 2 } }));
        EXPECT_EQ(walk.processor.current_region(), std::optional<size_t> { 1 });
    }

    TEST(RegionWalkBlockTest, TheWalkEndsOnceAndTheRestIsSilence)
    {
        Walk walk(walk_group(RegionSelectionPattern::SEQUENTIAL), RegionDelivery::BLOCK, 30);
        int ended = 0;
        walk.processor.set_on_end([&ended] { ++ended; });
        walk.attach();

        walk.step();
        EXPECT_EQ(lane(walk.container, 0), joined(joined(range(0, 10), range(20, 10)), range(50, 10)));
        EXPECT_EQ(ended, 0);

        walk.step();
        EXPECT_EQ(lane(walk.container, 0), Samples(30, 0.0));
        EXPECT_EQ(lane(walk.container, 1), Samples(30, 0.0));
        EXPECT_TRUE(walk.processor.contributions().empty());
        EXPECT_EQ(ended, 1);

        walk.step();
        EXPECT_EQ(ended, 1);
    }

    TEST(RegionWalkBlockTest, ARoundRobinWalkWraps)
    {
        Walk walk(walk_group(RegionSelectionPattern::ROUND_ROBIN), RegionDelivery::BLOCK, 30);
        walk.attach();

        walk.step();
        const auto first = lane(walk.container, 0);
        walk.step();

        EXPECT_EQ(lane(walk.container, 0), first);
    }

    TEST(RegionWalkBlockTest, ARegionBeyondTheDataHoldsTheWalkAndDeliversSilence)
    {
        auto group = walk_group(RegionSelectionPattern::SEQUENTIAL);
        group.regions = { Region::audio_span(0, 9, 0, 1, "A"), Region::audio_span(200, 209, 0, 1, "late") };
        Walk walk(group, RegionDelivery::BLOCK, 10);
        walk.attach();

        walk.step();
        EXPECT_EQ(lane(walk.container, 0), range(0, 10));

        walk.step();
        EXPECT_EQ(lane(walk.container, 0), Samples(10, 0.0));
        EXPECT_TRUE(walk.processor.contributions().empty());
        EXPECT_EQ(walk.processor.current_region(), std::optional<size_t> { 1 });
    }

    TEST(RegionWalkBlockTest, AGatedTransitionInsertsSilenceOfItsDuration)
    {
        auto group = walk_group(RegionSelectionPattern::SEQUENTIAL);
        group.transition_type = RegionTransition::GATED;
        group.transition_duration_ms = 2.0;
        Walk walk(group, RegionDelivery::BLOCK, 12);
        walk.attach();

        walk.step();
        EXPECT_EQ(lane(walk.container, 0), joined(range(0, 10), Samples { 0, 0 }));
        EXPECT_EQ(contributions_of(walk.processor), (Contributions { { 0, 0, 10 } }));

        walk.step();
        EXPECT_EQ(lane(walk.container, 0), joined(range(20, 10), Samples { 0, 0 }));
    }

    TEST(RegionWalkBlockTest, AnInterleavedContainerGetsAnInterleavedBlock)
    {
        Walk walk(walk_group(RegionSelectionPattern::SEQUENTIAL), RegionDelivery::BLOCK, 3, OrganizationStrategy::INTERLEAVED);
        walk.attach();

        walk.step();

        ASSERT_EQ(walk.container->get_processed_data().size(), 1U);
        EXPECT_EQ(lane(walk.container, 0), (Samples { 0, 1000, 1, 1001, 2, 1002 }));
    }

    TEST(RegionWalkBlockTest, ASeedRepeatsARandomWalk)
    {
        const auto indices_for = [](uint64_t seed) {
            Walk walk(walk_group(RegionSelectionPattern::RANDOM), RegionDelivery::BLOCK, 10);
            walk.processor.set_seed(seed);
            walk.attach();
            std::vector<size_t> out;
            for (int i = 0; i < 12; ++i) {
                walk.step();
                out.push_back(walk.processor.contributions().front().region_index);
            }
            return out;
        };

        const auto first = indices_for(11);

        EXPECT_EQ(indices_for(11), first);
        EXPECT_TRUE(std::ranges::all_of(first, [](size_t index) { return index < 3; }));
    }

    // ---- whole-region delivery -------------------------------------------------------------

    TEST(RegionWalkWholeTest, EachCycleDeliversTheNextRegionAndThenTheWalkEnds)
    {
        Walk walk(walk_group(RegionSelectionPattern::SEQUENTIAL), RegionDelivery::WHOLE, 0);
        int ended = 0;
        walk.processor.set_on_end([&ended] { ++ended; });
        walk.attach();

        walk.step();
        EXPECT_EQ(lane(walk.container, 0), range(0, 10));
        EXPECT_EQ(lane(walk.container, 1), range(0, 10, 1000));
        EXPECT_EQ(contributions_of(walk.processor), (Contributions { { 0, 0, 10 } }));

        walk.step();
        EXPECT_EQ(lane(walk.container, 0), range(20, 10));
        EXPECT_EQ(contributions_of(walk.processor), (Contributions { { 1, 0, 10 } }));

        walk.step();
        EXPECT_EQ(lane(walk.container, 0), range(50, 10));

        walk.step();
        EXPECT_TRUE(walk.processor.contributions().empty());
        EXPECT_EQ(walk.container->get_processing_state(), ProcessingState::IDLE);
        EXPECT_EQ(ended, 1);
    }

    TEST(RegionWalkWholeTest, WeightedChoosesByTheWeightFacet)
    {
        const Facet<double> weight;
        auto group = walk_group(RegionSelectionPattern::WEIGHTED);
        group.regions[0].set(weight, 0.0);
        group.regions[1].set(weight, 1.0);
        group.regions[2].set(weight, 0.0);
        Walk walk(group, RegionDelivery::WHOLE, 0);
        walk.processor.set_weight_key(weight);
        walk.attach();

        for (int i = 0; i < 6; ++i) {
            walk.step();
            EXPECT_EQ(contributions_of(walk.processor), (Contributions { { 1, 0, 10 } }));
        }
    }

    TEST(RegionWalkWholeTest, ExclusiveRepeatsTheGroupsCurrentRegion)
    {
        auto group = walk_group(RegionSelectionPattern::EXCLUSIVE);
        group.current_region_index = 2;
        Walk walk(group, RegionDelivery::WHOLE, 0);
        walk.attach();

        for (int i = 0; i < 3; ++i) {
            walk.step();
            EXPECT_EQ(lane(walk.container, 0), range(50, 10));
        }
    }

    TEST(RegionWalkWholeTest, CustomPicksWhatTheChooserReturns)
    {
        Walk walk(walk_group(RegionSelectionPattern::CUSTOM), RegionDelivery::WHOLE, 0);
        walk.processor.set_chooser([](const RegionGroup&, std::optional<size_t>) -> std::optional<size_t> { return 1; });
        walk.attach();

        walk.step();
        walk.step();

        EXPECT_EQ(lane(walk.container, 0), range(20, 10));
    }

    TEST(RegionWalkWholeTest, ARegionBeyondTheDataIsWaitedOnWithNothingDelivered)
    {
        auto group = walk_group(RegionSelectionPattern::SEQUENTIAL);
        group.regions = { Region::audio_span(200, 209, 0, 1, "late") };
        Walk walk(group, RegionDelivery::WHOLE, 0);
        walk.attach();

        walk.step();
        walk.step();

        EXPECT_TRUE(walk.processor.contributions().empty());
        EXPECT_EQ(walk.processor.current_region(), std::optional<size_t> { 0 });
        EXPECT_EQ(walk.container->get_processing_state(), ProcessingState::IDLE);
    }

    // ---- callbacks and attribution ---------------------------------------------------------

    TEST(RegionWalkCallbackTest, TheGroupsCallbacksFireInTheOrderOfTheWalk)
    {
        std::vector<std::string> events;
        auto group = walk_group(RegionSelectionPattern::SEQUENTIAL);
        group.on_region_start = [&events](const Region& r) { events.push_back("start " + *r.label); };
        group.on_region_end = [&events](const Region& r) { events.push_back("end " + *r.label); };
        group.on_transition = [&events](const Region& from, const Region& to) {
            events.push_back("move " + *from.label + ">" + *to.label);
        };
        Walk walk(group, RegionDelivery::WHOLE, 0);
        walk.attach();

        for (int i = 0; i < 3; ++i) {
            walk.step();
        }

        EXPECT_EQ(events, (std::vector<std::string> { "start A", "end A", "move A>B", "start B", "end B", "move B>C", "start C", "end C" }));
    }

    // ---- refresh and binding ---------------------------------------------------------------

    TEST(RegionWalkRefreshTest, ARefreshedGroupIsWalkedFromItsStart)
    {
        Walk walk(walk_group(RegionSelectionPattern::SEQUENTIAL), RegionDelivery::WHOLE, 0);
        walk.attach();
        walk.step();
        walk.step();

        RegionGroup replacement("walk", { Region::audio_span(70, 79, 0, 1, "X") });
        walk.container->add_region_group(replacement);
        walk.processor.refresh();
        walk.step();

        EXPECT_EQ(lane(walk.container, 0), range(70, 10));
        EXPECT_EQ(contributions_of(walk.processor), (Contributions { { 0, 0, 10 } }));
    }

    TEST(RegionWalkRefreshTest, WithoutARefreshTheContainersEditsAreNotSeen)
    {
        Walk walk(walk_group(RegionSelectionPattern::SEQUENTIAL), RegionDelivery::WHOLE, 0);
        walk.attach();

        walk.container->add_region_group(RegionGroup("walk", { Region::audio_span(70, 79, 0, 1, "X") }));
        walk.step();

        EXPECT_EQ(lane(walk.container, 0), range(0, 10));
    }

    TEST(RegionWalkRefreshTest, AMissingGroupIsAnEmptyWalk)
    {
        auto container = stereo();
        RegionWalkProcessor processor("absent", RegionDelivery::BLOCK, 4);
        processor.on_attach(container);

        processor.process(container);

        EXPECT_EQ(std::get<Samples>(container->get_processed_data().at(0)), Samples(4, 0.0));
    }

    TEST(RegionWalkBindingTest, ASoundContainerSuppliesTheSampleRateForAGate)
    {
        auto group = walk_group(RegionSelectionPattern::SEQUENTIAL);
        group.transition_type = RegionTransition::GATED;
        group.transition_duration_ms = 1.0;
        Walk walk(group, RegionDelivery::BLOCK, 12);

        EXPECT_NO_THROW(walk.attach());
    }

    TEST(RegionWalkBindingTest, GroupsThatCannotBeWalkedAreRejectedWhenBound)
    {
        const auto attach_with = [](RegionGroup group, RegionDelivery delivery) {
            Walk walk(std::move(group), delivery, 4);
            walk.attach();
        };

        EXPECT_THROW(attach_with(walk_group(RegionSelectionPattern::OVERLAP), RegionDelivery::BLOCK), std::invalid_argument);
        EXPECT_THROW(attach_with(walk_group(RegionSelectionPattern::ALL), RegionDelivery::WHOLE), std::invalid_argument);
        EXPECT_THROW(attach_with(walk_group(RegionSelectionPattern::CUSTOM), RegionDelivery::WHOLE), std::invalid_argument);

        auto crossfade = walk_group(RegionSelectionPattern::SEQUENTIAL);
        crossfade.transition_type = RegionTransition::CROSSFADE;
        EXPECT_THROW(attach_with(crossfade, RegionDelivery::BLOCK), std::invalid_argument);

        auto gated = walk_group(RegionSelectionPattern::SEQUENTIAL);
        gated.transition_type = RegionTransition::GATED;
        EXPECT_THROW(attach_with(gated, RegionDelivery::WHOLE), std::invalid_argument);
    }

    TEST(RegionWalkBindingTest, AttachingToNothingThrows)
    {
        RegionWalkProcessor processor("walk");

        EXPECT_THROW(processor.on_attach(nullptr), std::invalid_argument);
    }

    TEST(RegionWalkBindingTest, ADetachedProcessorWalksNothing)
    {
        Walk walk(walk_group(RegionSelectionPattern::SEQUENTIAL), RegionDelivery::WHOLE, 0);
        walk.attach();
        walk.processor.on_detach(walk.container);

        walk.step();

        EXPECT_TRUE(walk.processor.contributions().empty());
        EXPECT_EQ(walk.container->get_processing_state(), ProcessingState::IDLE);
    }

} // namespace
} // namespace MayaFlux::Test
