#include "gtest/gtest.h"

#include "MayaFlux/Kinesis/Discrete/Boolean.hpp"
#include "MayaFlux/Nodes/Generators/Polynomial.hpp"
#include "MayaFlux/Nodes/Network/RelationNetwork.hpp"


namespace MayaFlux::Test {

namespace Discrete = Kinesis::Discrete;
using Kinesis::Stochastic::Weights;
using Nodes::Generator::Polynomial;
using Nodes::Network::AdvanceOperator;
using Nodes::Network::CombineOperator;
using Nodes::Network::DeriveOperator;
using Nodes::Network::RelationNetwork;
using Nodes::Network::RelationOperator;
using Nodes::Network::RelationSlot;
using Nodes::Network::Source;
using Nodes::Network::StepOperator;

namespace {

    constexpr unsigned int FRAMES = 8;

    std::vector<double> render(RelationNetwork& network, unsigned int frames)
    {
        network.process_batch(frames);
        return network.get_audio_buffer().value_or(std::vector<double> {});
    }

    bool all_equal(std::span<const double> values, double expected, double tolerance = 1e-12)
    {
        return !values.empty()
            && std::ranges::all_of(values, [=](double v) { return std::abs(v - expected) <= tolerance; });
    }

    std::shared_ptr<Nodes::Node> constant(double value)
    {
        auto source = std::make_shared<Polynomial>([value](double) { return value; });
        source->process_sample(0.0);
        return source;
    }

    std::shared_ptr<Nodes::Node> ramp(double step)
    {
        auto position = std::make_shared<size_t>(0);
        return std::make_shared<Polynomial>([position, step](double) {
            return static_cast<double>((*position)++) * step;
        });
    }

    std::shared_ptr<Nodes::Node> sequence(std::vector<double> values)
    {
        auto position = std::make_shared<size_t>(0);
        return std::make_shared<Polynomial>([position, values = std::move(values)](double) {
            return values[(*position)++ % values.size()];
        });
    }

    std::shared_ptr<RelationNetwork> network_with(std::initializer_list<double> levels)
    {
        auto network = std::make_shared<RelationNetwork>();
        for (const double level : levels) {
            network->add_slot({ .level = level });
        }
        return network;
    }

    struct FindCase {
        const char* name;
        const char* slot;
        std::optional<uint32_t> expected;
    };

    struct RuleCase {
        const char* name;
        std::vector<double> levels;
        DeriveOperator::Function function;
        double expected;
    };

    struct CoefficientCase {
        const char* name;
        const char* parameter;
        std::vector<double> expected;
    };

    struct SourceCase {
        const char* name;
        std::function<Source()> make;
        double expected;
    };

    struct OperatorCase {
        const char* name;
        std::function<std::shared_ptr<RelationOperator>()> make;
        const char* label;
        bool publishes;
    };

class RelationFindTest : public ::testing::TestWithParam<FindCase> { };

TEST_P(RelationFindTest, SlotsAreFoundByNameOrReportedAbsent)
{
    RelationNetwork network;
    network.add_slot({ .name = "carrier" });
    network.add_slot({ .name = "product" });
    const auto& config = GetParam();

    EXPECT_EQ(network.find_slot_index(config.slot), config.expected);
    const auto found = network.find_slot(config.slot);
    ASSERT_EQ(found.has_value(), config.expected.has_value());
    if (found) {
        EXPECT_EQ(found->get().name, config.slot);
        EXPECT_EQ(found->get().index, *config.expected);
    }
}

INSTANTIATE_TEST_SUITE_P(
    Names,
    RelationFindTest,
    ::testing::Values(
        FindCase { "First", "carrier", 0U },
        FindCase { "Second", "product", 1U },
        FindCase { "Absent", "missing", std::nullopt }),
    [](const ::testing::TestParamInfo<FindCase>& info) { return std::string(info.param.name); });

TEST(RelationNetworkTest, ANewNetworkIsEmptyAndPublishesNothing)
{
    RelationNetwork network;

    EXPECT_EQ(network.get_output_mode(), Nodes::Network::OutputMode::AUDIO_SINK);
    EXPECT_EQ(network.get_node_count(), 0U);
    EXPECT_EQ(network.slot_count(), 0U);
    EXPECT_EQ(network.get_output_count(), 0U);
    EXPECT_FALSE(network.get_output_block(0).has_value());
    EXPECT_TRUE(render(network, FRAMES).empty());
}

TEST(RelationNetworkTest, SlotsReceiveSequentialIndicesWhateverTheCallerSets)
{
    RelationNetwork network;

    EXPECT_EQ(network.add_slot({ .index = 9, .name = "a" }), 0U);
    EXPECT_EQ(network.add_slot({ .index = 9, .name = "b" }), 1U);
    const auto more = network.add_slots({ { .name = "c" }, { .name = "d" } });

    EXPECT_EQ(more, (std::vector<uint32_t> { 2, 3 }));
    EXPECT_EQ(network.slot_count(), 4U);
    EXPECT_EQ(network.get_node_count(), 4U);
    for (uint32_t i = 0; i < 4; ++i) {
        EXPECT_EQ(network.get_slot(i).index, i);
    }
}

TEST(RelationNetworkTest, SlotAccessorsExposeLevelAndBlockAndRejectOutOfRange)
{
    auto network = network_with({ 0.25, 0.5 });
    network->get_slot(1).level = 0.75;
    network->add<AdvanceOperator>();
    network->process_batch(FRAMES);
    const auto& view = *network;

    EXPECT_DOUBLE_EQ(view.get_slot(0).level, 0.25);
    EXPECT_DOUBLE_EQ(network->get_node_output(1).value(), 0.75);
    ASSERT_TRUE(network->get_node_audio_buffer(0).has_value());
    EXPECT_TRUE(all_equal(*network->get_node_audio_buffer(0), 0.25));
    EXPECT_FALSE(network->get_node_output(2).has_value());
    EXPECT_FALSE(network->get_node_audio_buffer(2).has_value());
}

TEST(RelationNetworkTest, MetadataCountsSlotsOutputsAndOperators)
{
    auto network = network_with({ 1.0, 1.0 });
    network->add<AdvanceOperator>();
    network->add<CombineOperator>(2, 2);

    const auto metadata = network->get_metadata();

    EXPECT_EQ(metadata.at("slot_count"), "2");
    EXPECT_EQ(metadata.at("output_count"), "2");
    EXPECT_EQ(metadata.at("operator_count"), "2");
}

TEST(RelationNetworkTest, OperatorsAddedBeforeSlotsStillSeeThemAfterGrowth)
{
    RelationNetwork network;
    network.add<AdvanceOperator>();
    network.add<CombineOperator>(5, 1);
    for (const double level : { 1.0, 2.0, 3.0, 4.0, 5.0 }) {
        network.add_slot({ .level = level });
    }

    const auto audio = render(network, FRAMES);

    EXPECT_TRUE(all_equal(audio, 15.0));
}

TEST(RelationSlotTest, ActivityCallbacksFireOnlyOnARealTransition)
{
    RelationSlot slot;
    int entered = 0;
    int left = 0;
    slot.on_enter.push_back([&] { ++entered; });
    slot.on_leave.push_back([&] { ++left; });

    slot.set_active(true);
    slot.set_active(false);
    slot.set_active(false);
    slot.set_active(true);

    EXPECT_EQ(entered, 1);
    EXPECT_EQ(left, 1);
    EXPECT_TRUE(slot.active);
}

TEST(RelationSlotTest, PrepareBlockReturnsAZeroFilledBlockOfTheRequestedSize)
{
    RelationSlot slot;
    slot.block = { 1.0, 2.0, 3.0 };

    const auto block = slot.prepare_block(5);

    ASSERT_EQ(block.size(), 5U);
    EXPECT_TRUE(all_equal(block, 0.0));
}

class RelationAdvanceHoldTest : public ::testing::TestWithParam<bool> { };

TEST_P(RelationAdvanceHoldTest, ANodelessSlotRepeatsItsLevelUnlessMuted)
{
    auto network = network_with({ 0.4 });
    network->get_slot(0).active = GetParam();
    network->add<AdvanceOperator>();

    network->process_batch(FRAMES);

    const auto block = network->get_node_audio_buffer(0).value();
    ASSERT_EQ(block.size(), FRAMES);
    EXPECT_TRUE(std::ranges::all_of(block, [&](double v) { return v == (GetParam() ? 0.4 : 0.0); }));
    EXPECT_DOUBLE_EQ(network->get_node_output(0).value(), 0.4);
}

INSTANTIATE_TEST_SUITE_P(
    Activity,
    RelationAdvanceHoldTest,
    ::testing::Values(true, false));

class RelationAdvanceNodeTest : public ::testing::TestWithParam<bool> { };

TEST_P(RelationAdvanceNodeTest, ANodeSlotPublishesItsSamplesAndTracksItsLevelEvenWhenMuted)
{
    RelationNetwork network;
    network.add_slot({ .node = ramp(0.1), .active = GetParam() });
    network.add<AdvanceOperator>();

    network.process_batch(FRAMES);

    const auto block = network.get_node_audio_buffer(0).value();
    ASSERT_EQ(block.size(), FRAMES);
    for (size_t i = 0; i < block.size(); ++i) {
        EXPECT_NEAR(block[i], GetParam() ? 0.1 * static_cast<double>(i) : 0.0, 1e-12) << "frame " << i;
    }
    EXPECT_NEAR(network.get_node_output(0).value(), 0.1 * (FRAMES - 1), 1e-12);
}

INSTANTIATE_TEST_SUITE_P(
    Activity,
    RelationAdvanceNodeTest,
    ::testing::Values(true, false));

TEST(RelationAdvanceTest, SlotsAdvanceIndependently)
{
    RelationNetwork network;
    network.add_slot({ .node = ramp(1.0) });
    network.add_slot({ .node = constant(0.5) });
    network.add<AdvanceOperator>();

    network.process_batch(FRAMES);

    EXPECT_DOUBLE_EQ(network.get_node_output(0).value(), static_cast<double>(FRAMES - 1));
    EXPECT_TRUE(all_equal(*network.get_node_audio_buffer(1), 0.5));
}

TEST(RelationAdvanceTest, NodeHooksFireOncePerFrameWhileTheSlotAdvances)
{
    RelationNetwork network;
    int ticks = 0;
    auto node = ramp(0.1);
    node->on_tick([&ticks](Nodes::NodeContext&) { ++ticks; });
    network.add_slot({ .node = node });
    network.add<AdvanceOperator>();

    network.process_batch(FRAMES);

    EXPECT_EQ(ticks, static_cast<int>(FRAMES));
}

class RelationDeriveRuleTest : public ::testing::TestWithParam<RuleCase> { };

TEST_P(RelationDeriveRuleTest, TheTargetBlockIsTheRuleOverTheInputs)
{
    const auto& config = GetParam();
    RelationNetwork network;
    std::vector<size_t> inputs;
    for (const double level : config.levels) {
        inputs.push_back(network.add_slot({ .level = level }));
    }
    const size_t target = network.add_slot();
    network.add<AdvanceOperator>();
    network.add<DeriveOperator>(target, inputs, config.function);

    network.process_batch(FRAMES);

    EXPECT_TRUE(all_equal(*network.get_node_audio_buffer(target), config.expected));
    EXPECT_DOUBLE_EQ(network.get_node_output(target).value(), config.expected);
    EXPECT_DOUBLE_EQ(network.get_node_output(0).value(), config.levels[0]);
}

INSTANTIATE_TEST_SUITE_P(
    Rules,
    RelationDeriveRuleTest,
    ::testing::Values(
        RuleCase { "Sum", { 1.0, 2.0, 3.0 }, [](std::span<const double> v) { return v[0] + v[1] + v[2]; }, 6.0 },
        RuleCase { "Product", { 0.5, 4.0 }, [](std::span<const double> v) { return v[0] * v[1]; }, 2.0 },
        RuleCase { "Difference", { 5.0, 2.0 }, [](std::span<const double> v) { return v[0] - v[1]; }, 3.0 },
        RuleCase { "Largest", { 0.1, 0.9, 0.4 }, [](std::span<const double> v) { return *std::ranges::max_element(v); }, 0.9 }),
    [](const ::testing::TestParamInfo<RuleCase>& info) { return std::string(info.param.name); });

TEST(RelationDeriveTest, WithoutAnAdvanceStageInputsAreReadFromTheirLevels)
{
    auto network = network_with({ 2.0, 3.0, 0.0 });
    network->add<DeriveOperator>(2, std::vector<size_t> { 0, 1 },
        [](std::span<const double> v) { return v[0] * v[1]; });

    network->process_batch(FRAMES);

    EXPECT_TRUE(all_equal(*network->get_node_audio_buffer(2), 6.0));
}

TEST(RelationDeriveTest, ATargetInputReadsItsPreviousFrameAndPreviousBlock)
{
    auto network = network_with({ 1.0, 0.0 });
    network->add<AdvanceOperator>();
    network->add<DeriveOperator>(1, std::vector<size_t> { 0, 1 },
        [](std::span<const double> v) { return v[0] + v[1]; });

    network->process_batch(4);
    const std::vector<double> first(network->get_node_audio_buffer(1)->begin(), network->get_node_audio_buffer(1)->end());
    network->process_batch(4);
    const std::vector<double> second(network->get_node_audio_buffer(1)->begin(), network->get_node_audio_buffer(1)->end());

    EXPECT_EQ(first, (std::vector<double> { 1.0, 2.0, 3.0, 4.0 }));
    EXPECT_EQ(second, (std::vector<double> { 5.0, 6.0, 7.0, 8.0 }));
}

TEST(RelationDeriveTest, AnEmptyRuleLeavesTheTargetAsAdvanceWroteIt)
{
    auto network = network_with({ 0.3, 0.8 });
    network->add<AdvanceOperator>();
    network->add<DeriveOperator>(1, std::vector<size_t> { 0 }, DeriveOperator::Function {});

    network->process_batch(FRAMES);

    EXPECT_TRUE(all_equal(*network->get_node_audio_buffer(1), 0.8));
}

TEST(RelationDeriveTest, CoefficientsRetuneThroughMappingAndStateQueries)
{
    auto network = network_with({ 0.5, 0.0 });
    network->add<AdvanceOperator>();
    const auto derive = network->add<DeriveOperator>(1, std::vector<size_t> { 0 },
        DeriveOperator::CoefFunction([](std::span<const double> v, std::span<double> c) { return v[0] * c[0] + c[1]; }),
        std::vector<double> { 2.0, 1.0 });

    network->process_batch(FRAMES);
    EXPECT_TRUE(all_equal(*network->get_node_audio_buffer(1), 2.0));

    derive->map("c0", 4.0);
    network->process_batch(FRAMES);

    EXPECT_TRUE(all_equal(*network->get_node_audio_buffer(1), 3.0));
    EXPECT_DOUBLE_EQ(derive->query_state("c0").value(), 4.0);
    EXPECT_DOUBLE_EQ(derive->query_state("c1").value(), 1.0);
    EXPECT_EQ(derive->get_coefficients(), (std::vector<double> { 4.0, 1.0 }));
}

class RelationCoefficientNameTest : public ::testing::TestWithParam<CoefficientCase> { };

TEST_P(RelationCoefficientNameTest, OnlyWellFormedInRangeNamesRetuneACoefficient)
{
    auto derive = std::make_shared<DeriveOperator>(0, std::vector<size_t> {},
        DeriveOperator::CoefFunction([](std::span<const double>, std::span<double>) { return 0.0; }),
        std::vector<double> { 10.0, 20.0 });

    derive->set_parameter(GetParam().parameter, 99.0);

    EXPECT_EQ(derive->get_coefficients(), GetParam().expected);
}

INSTANTIATE_TEST_SUITE_P(
    Names,
    RelationCoefficientNameTest,
    ::testing::Values(
        CoefficientCase { "First", "c0", { 99.0, 20.0 } },
        CoefficientCase { "Second", "c1", { 10.0, 99.0 } },
        CoefficientCase { "PastTheEnd", "c2", { 10.0, 20.0 } },
        CoefficientCase { "WrongPrefix", "x0", { 10.0, 20.0 } },
        CoefficientCase { "NoIndex", "c", { 10.0, 20.0 } },
        CoefficientCase { "TrailingText", "c1x", { 10.0, 20.0 } },
        CoefficientCase { "Negative", "c-1", { 10.0, 20.0 } }),
    [](const ::testing::TestParamInfo<CoefficientCase>& info) { return std::string(info.param.name); });

TEST(RelationDeriveTest, ReplacedCoefficientsTakeEffectAndQueriesPastTheArrayFail)
{
    auto derive = std::make_shared<DeriveOperator>(0, std::vector<size_t> {},
        DeriveOperator::CoefFunction([](std::span<const double>, std::span<double>) { return 0.0; }),
        std::vector<double> { 1.0 });

    derive->set_coefficients({ 5.0, 6.0, 7.0 });

    EXPECT_EQ(derive->get_coefficients().size(), 3U);
    EXPECT_DOUBLE_EQ(derive->query_state("c2").value(), 7.0);
    EXPECT_FALSE(derive->query_state("c3").has_value());
    EXPECT_FALSE(derive->query_state("gain").has_value());
}

TEST(RelationDeriveTest, AGateRuleWritesZeroOrOneAndDrivesTheTargetActivity)
{
    RelationNetwork network;
    network.add_slot({ .node = sequence({ 0.0, 0.0, 1.0, 1.0, 0.0, 0.0, 1.0, 0.0 }) });
    network.add_slot({ .active = false });
    int entered = 0;
    int left = 0;
    network.get_slot(1).on_enter.push_back([&] { ++entered; });
    network.get_slot(1).on_leave.push_back([&] { ++left; });
    network.add<AdvanceOperator>();
    network.add<DeriveOperator>(1, std::vector<size_t> { 0 },
        [](const std::vector<double>& v, std::span<double>) { return v[0] > 0.5; },
        std::vector<double> {});

    network.process_batch(FRAMES);

    const std::vector<double> block(network.get_node_audio_buffer(1)->begin(), network.get_node_audio_buffer(1)->end());
    EXPECT_EQ(block, (std::vector<double> { 0.0, 0.0, 1.0, 1.0, 0.0, 0.0, 1.0, 0.0 }));
    EXPECT_EQ(entered, 2);
    EXPECT_EQ(left, 2);
    EXPECT_FALSE(network.get_slot(1).active);
}

TEST(RelationDeriveTest, TheKinesisComparatorGatesWithHysteresis)
{
    auto network = network_with({ 0.0, 0.0, 0.0 });
    network->get_slot(2).active = false;
    network->add<AdvanceOperator>();
    network->add<DeriveOperator>(2, std::vector<size_t> { 0, 1 },
        Discrete::comparator, Discrete::comparator_state(0.2));
    const auto step = [&](double high, double low) {
        network->get_slot(0).level = high;
        network->get_slot(1).level = low;
        network->process_batch(4);
        return network->get_node_output(2).value();
    };

    EXPECT_DOUBLE_EQ(step(1.0, 0.0), 1.0);
    EXPECT_DOUBLE_EQ(step(0.5, 0.45), 1.0);
    EXPECT_DOUBLE_EQ(step(0.0, 1.0), 0.0);
    EXPECT_DOUBLE_EQ(step(0.5, 0.45), 0.0);
    EXPECT_FALSE(network->get_slot(2).active);
}

TEST(RelationCombineTest, ByDefaultEverySlotFeedsTheFirstOutputWithUnitWeight)
{
    auto network = network_with({ 1.0, 2.0, 3.0 });
    network->add<AdvanceOperator>();
    const auto combine = network->add<CombineOperator>(3, 1);

    const auto audio = render(*network, FRAMES);

    ASSERT_EQ(audio.size(), FRAMES);
    EXPECT_TRUE(all_equal(audio, 6.0));
    EXPECT_EQ(combine->weights()->rows(), 3U);
    EXPECT_EQ(combine->weights()->cols(), 1U);
    EXPECT_TRUE(combine->publishes_output());
    EXPECT_EQ(combine->get_output_count(), 1U);
}

TEST(RelationCombineTest, WeightsRetuneTheMixAndFurtherOutputsStartSilent)
{
    auto network = network_with({ 1.0, 3.0 });
    network->add<AdvanceOperator>();
    const auto combine = network->add<CombineOperator>(2, 2);

    network->process_batch(FRAMES);
    EXPECT_TRUE(all_equal(combine->get_output_block(0), 4.0));
    ASSERT_EQ(combine->get_output_block(1).size(), FRAMES);
    EXPECT_TRUE(all_equal(combine->get_output_block(1), 0.0));

    combine->weight(0, 0) = 0.0;
    combine->weight(0, 1) = 2.0;
    network->process_batch(FRAMES);

    EXPECT_TRUE(all_equal(combine->get_output_block(0), 3.0));
    EXPECT_TRUE(all_equal(combine->get_output_block(1), 2.0));
}

class RelationCombineRangeTest : public ::testing::TestWithParam<std::pair<size_t, size_t>> { };

TEST_P(RelationCombineRangeTest, WeightCellsOutsideTheTableAreRejected)
{
    CombineOperator combine(2, 1);
    const auto [slot, output] = GetParam();

    EXPECT_THROW((void)combine.weight(slot, output), std::out_of_range);
}

INSTANTIATE_TEST_SUITE_P(
    OutsideTheTable,
    RelationCombineRangeTest,
    ::testing::Values(
        std::pair<size_t, size_t> { 2, 0 },
        std::pair<size_t, size_t> { 0, 1 },
        std::pair<size_t, size_t> { 5, 5 }));

TEST(RelationCombineTest, ASharedTableDrivesTheOutputsAndANullTableIsRejected)
{
    auto table = std::make_shared<Weights>(2, 2);
    (*table)(0, 0) = 1.0;
    (*table)(1, 1) = 1.0;
    auto network = network_with({ 2.0, 5.0 });
    network->add<AdvanceOperator>();
    const auto combine = network->add<CombineOperator>(table);

    network->process_batch(FRAMES);
    EXPECT_EQ(combine->get_output_count(), 2U);
    EXPECT_TRUE(all_equal(combine->get_output_block(0), 2.0));
    EXPECT_TRUE(all_equal(combine->get_output_block(1), 5.0));

    (*table)(1, 0) = 1.0;
    network->process_batch(FRAMES);

    EXPECT_TRUE(all_equal(combine->get_output_block(0), 7.0));
    EXPECT_THROW((void)std::make_shared<CombineOperator>(std::shared_ptr<Weights> {}), std::invalid_argument);
}

TEST(RelationCombineTest, ACombineFunctionReplacesTheWeightedSumUntilCleared)
{
    auto network = network_with({ 1.0, 4.0, 2.0 });
    network->add<AdvanceOperator>();
    const auto combine = network->add<CombineOperator>(3, 1);
    combine->output(0).combine([](std::span<const double> v) { return *std::ranges::max_element(v); });

    network->process_batch(FRAMES);
    EXPECT_TRUE(combine->output(0).has_combine());
    EXPECT_TRUE(all_equal(combine->get_output_block(0), 4.0));

    combine->output(0).clear_combine();
    network->process_batch(FRAMES);

    EXPECT_FALSE(combine->output(0).has_combine());
    EXPECT_TRUE(all_equal(combine->get_output_block(0), 7.0));
}

TEST(RelationCombineTest, SlotsBeyondTheTableRowsAreLeftOut)
{
    auto network = network_with({ 1.0, 2.0, 4.0 });
    network->add<AdvanceOperator>();
    network->add<CombineOperator>(2, 1);

    EXPECT_TRUE(all_equal(render(*network, FRAMES), 3.0));
}

TEST(RelationCombineTest, OutputsOfEveryPublisherAreIndexedInChainOrder)
{
    auto network = network_with({ 1.0, 3.0 });
    network->add<AdvanceOperator>();
    const auto first = network->add<CombineOperator>(2, 1);
    const auto second = network->add<CombineOperator>(2, 2);
    second->weight(0, 0) = 0.0;
    second->weight(0, 1) = 2.0;

    const auto audio = render(*network, FRAMES);

    EXPECT_EQ(network->get_output_count(), 3U);
    EXPECT_TRUE(all_equal(audio, 4.0));
    EXPECT_TRUE(all_equal(*network->get_output_block(0), 4.0));
    EXPECT_TRUE(all_equal(*network->get_output_block(1), 3.0));
    EXPECT_TRUE(all_equal(*network->get_output_block(2), 2.0));
    EXPECT_FALSE(network->get_output_block(3).has_value());
    EXPECT_TRUE(first->publishes_output());
}

TEST(RelationCombineTest, OutputScaleShapesTheAudioBufferButNotThePublishedBlocks)
{
    auto network = network_with({ 1.0, 3.0 });
    network->add<AdvanceOperator>();
    network->add<CombineOperator>(2, 1);
    network->set_output_scale(0.5);

    const auto audio = render(*network, FRAMES);

    EXPECT_TRUE(all_equal(audio, 2.0));
    EXPECT_TRUE(all_equal(*network->get_output_block(0), 4.0));
}

TEST(RelationChainTest, ANodeDrivenProductReachesTheAudioBuffer)
{
    RelationNetwork network;
    network.add_slot({ .name = "carrier", .node = ramp(0.5) });
    network.add_slot({ .name = "gain", .node = constant(0.25) });
    const uint32_t product = network.add_slot({ .name = "product" });
    network.add<AdvanceOperator>();
    network.add<DeriveOperator>(product, std::vector<size_t> { 0, 1 },
        [](std::span<const double> v) { return v[0] * v[1]; });
    const auto combine = network.add<CombineOperator>(3, 1);
    combine->weight(0, 0) = 0.0;
    combine->weight(1, 0) = 0.0;

    const auto audio = render(network, FRAMES);

    ASSERT_EQ(audio.size(), FRAMES);
    for (size_t i = 0; i < audio.size(); ++i) {
        EXPECT_NEAR(audio[i], 0.5 * static_cast<double>(i) * 0.25, 1e-12) << "frame " << i;
    }
}

TEST(RelationStepTest, TheBodyRunsEachBlockWithTheSlotsAndFrameCount)
{
    auto network = network_with({ 0.2, 0.9 });
    uint32_t seen_frames = 0;
    size_t seen_slots = 0;
    network->add<StepOperator>([&](const std::shared_ptr<StepOperator>&, std::span<RelationSlot> slots, uint32_t frames) {
        seen_frames = frames;
        seen_slots = slots.size();
        for (auto& slot : slots) {
            if (slot.level > 0.5) {
                slot.set_active(false);
            }
        }
    });

    network->process_batch(FRAMES);

    EXPECT_EQ(seen_frames, FRAMES);
    EXPECT_EQ(seen_slots, 2U);
    EXPECT_TRUE(network->get_slot(0).active);
    EXPECT_FALSE(network->get_slot(1).active);
}

TEST(RelationStepTest, AnEmptyBodyIsHarmless)
{
    auto network = network_with({ 0.2 });
    network->add<StepOperator>(StepOperator::Body {});

    EXPECT_NO_THROW(network->process_batch(FRAMES));
}

TEST(RelationStepTest, ParametersAreStoredByNameAndDefaultToZero)
{
    StepOperator step(StepOperator::Body {});

    EXPECT_DOUBLE_EQ(step.parameter("limit"), 0.0);
    EXPECT_FALSE(step.query_state("limit").has_value());

    step.set_parameter("limit", 0.6);

    EXPECT_DOUBLE_EQ(step.parameter("limit"), 0.6);
    EXPECT_DOUBLE_EQ(step.query_state("limit").value(), 0.6);
}

class RelationSourceTest : public ::testing::TestWithParam<SourceCase> { };

TEST_P(RelationSourceTest, ASourceEvaluatesToTheValueItWraps)
{
    EXPECT_DOUBLE_EQ(GetParam().make().evaluate(), GetParam().expected);
}

INSTANTIATE_TEST_SUITE_P(
    Forms,
    RelationSourceTest,
    ::testing::Values(
        SourceCase { "Constant", [] { return Source(0.25); }, 0.25 },
        SourceCase { "Node", [] { return Source(constant(0.75)); }, 0.75 },
        SourceCase { "EmptyNode", [] { return Source(std::shared_ptr<Polynomial> {}); }, 0.0 },
        SourceCase { "NetworkIndex", [] {
                        auto network = network_with({ 0.6 });
                        return Source(network, 0); },
            0.6 },
        SourceCase { "MissingIndex", [] {
                        auto network = network_with({ 0.6 });
                        return Source(network, 5); },
            0.0 },
        SourceCase { "NullNetwork", [] { return Source(std::shared_ptr<RelationNetwork> {}, 0); }, 0.0 },
        SourceCase { "Callable", [] { return Source([] { return 1.5; }); }, 1.5 },
        SourceCase { "EmptyCallable", [] { return Source(Source::Function {}); }, 0.0 }),
    [](const ::testing::TestParamInfo<SourceCase>& info) { return std::string(info.param.name); });

TEST(RelationMappingTest, AMappingIsEvaluatedOncePerBlock)
{
    auto network = network_with({ 0.0 });
    const auto step = network->add<StepOperator>(StepOperator::Body {});
    const auto calls = std::make_shared<int>(0);
    step->map("x", Source([calls] { return static_cast<double>(++*calls); }));

    network->process_batch(FRAMES);
    EXPECT_DOUBLE_EQ(step->parameter("x"), 1.0);
    network->process_batch(FRAMES);
    EXPECT_DOUBLE_EQ(step->parameter("x"), 2.0);
    EXPECT_EQ(*calls, 2);
}

TEST(RelationMappingTest, MappingAgainReplacesTheSourceAndUnmapStopsUpdates)
{
    auto network = network_with({ 0.0 });
    const auto step = network->add<StepOperator>(StepOperator::Body {});
    const auto calls = std::make_shared<int>(0);
    step->map("x", Source([calls] { return static_cast<double>(++*calls); }));
    step->map("x", 9.0);

    network->process_batch(FRAMES);
    EXPECT_DOUBLE_EQ(step->parameter("x"), 9.0);
    EXPECT_EQ(*calls, 0);

    step->unmap("x");
    step->unmap("absent");
    step->set_parameter("x", 3.0);
    network->process_batch(FRAMES);

    EXPECT_DOUBLE_EQ(step->parameter("x"), 3.0);
}

TEST(RelationMappingTest, ASlotLevelCanDriveAnotherOperatorsParameter)
{
    auto network = network_with({ 0.5, 0.0 });
    network->add<AdvanceOperator>();
    const auto derive = network->add<DeriveOperator>(1, std::vector<size_t> { 0 },
        DeriveOperator::CoefFunction([](std::span<const double> v, std::span<double> c) { return v[0] * c[0]; }),
        std::vector<double> { 1.0 });
    derive->map("c0", Source(network, 0));

    network->process_batch(FRAMES);

    EXPECT_TRUE(all_equal(*network->get_node_audio_buffer(1), 0.25));
}

class RelationOperatorTypeTest : public ::testing::TestWithParam<OperatorCase> { };

TEST_P(RelationOperatorTypeTest, EachOperatorNamesItselfAndOnlyCombinePublishes)
{
    const auto op = GetParam().make();

    EXPECT_EQ(op->get_type_name(), GetParam().label);
    EXPECT_EQ(op->publishes_output(), GetParam().publishes);
    if (!GetParam().publishes) {
        EXPECT_EQ(op->get_output_count(), 0U);
        EXPECT_TRUE(op->get_output_block(0).empty());
    }
}

INSTANTIATE_TEST_SUITE_P(
    Operators,
    RelationOperatorTypeTest,
    ::testing::Values(
        OperatorCase { "Step", [] { return std::make_shared<StepOperator>(StepOperator::Body {}); }, "Step", false },
        OperatorCase { "Advance", [] { return std::make_shared<AdvanceOperator>(); }, "Advance", false },
        OperatorCase { "Derive", [] { return std::make_shared<DeriveOperator>(0, std::vector<size_t> {}, DeriveOperator::Function {}); }, "Derive", false },
        OperatorCase { "Combine", [] { return std::make_shared<CombineOperator>(1); }, "Combine", true }),
    [](const ::testing::TestParamInfo<OperatorCase>& info) { return std::string(info.param.name); });

}

}
