#include "MayaFlux/Yantra/Analyzers/EnergyAnalyzer.hpp"
#include "MayaFlux/Yantra/Analyzers/StatisticalAnalyzer.hpp"
#include "MayaFlux/Yantra/ComputeGrammar.hpp"
#include "MayaFlux/Yantra/Sorters/RegionKeySorter.hpp"
#include "MayaFlux/Yantra/Transformers/MathematicalTransformer.hpp"
#include "MayaFlux/Yantra/Transformers/TemporalTransformer.hpp"
#include "transformer_helpers.hpp"

#include <gtest/gtest.h>

namespace MayaFlux::Test {
namespace {

    using namespace Yantra;

    enum class WorkflowStage { Sort };

    template <typename Config>
    concept HasSelection = !std::is_void_v<selection_t<Config>>;

    TEST(OperationSelectionTest, DerivesTheEnumFromTheSelectorField)
    {
        static_assert(std::same_as<selection_t<MathematicalTransformerConfig>, MathematicalOperation>);
        static_assert(std::same_as<selection_t<TemporalTransformerConfig>, TemporalOperation>);
        static_assert(std::same_as<selection_t<StatisticalAnalyzerConfig>, StatisticalMethod>);
        SUCCEED();
    }

    TEST(OperationSelectionTest, ConfigWithoutSelectorHasNoSelection)
    {
        static_assert(!HasSelection<RegionKeySorterConfig>);
        SUCCEED();
    }

    TEST(OperationSupportsTest, SingleConfigClassDefaultsToItsOnlyConfig)
    {
        static_assert(std::same_as<default_config_t<RegionKeySorter>, RegionKeySorterConfig>);
        static_assert(Supports<RegionKeySorterConfig>::has<RegionKeySorterConfig>);
        static_assert(!Supports<RegionKeySorterConfig>::has<MathematicalTransformerConfig>);
        SUCCEED();
    }

    TEST(DefaultRegistryTest, ResolvesLibraryEnumsWithoutRegistration)
    {
        const auto registry = make_default_registry();

        EXPECT_FALSE(registry.resolve(ComputationSignature(MathematicalOperation::GAIN), nullptr).empty());
        EXPECT_FALSE(registry.resolve(ComputationSignature(TemporalOperation::TIME_REVERSE), nullptr).empty());
        EXPECT_FALSE(registry.resolve(ComputationSignature(StatisticalMethod::MEAN), nullptr).empty());
        EXPECT_FALSE(registry.resolve(ComputationSignature(EnergyMethod::RMS), nullptr).empty());
    }

    TEST(DefaultRegistryTest, DoesNotListWorkflowOnlyOperations)
    {
        const auto registry = make_default_registry();
        EXPECT_TRUE(registry.resolve(ComputationSignature(WorkflowStage::Sort), nullptr).empty());
    }

    TEST(DefaultRegistryTest, WorkflowOperationJoinsThroughASelectionOfItsOwnEnum)
    {
        auto registry = make_default_registry();
        registry.catalog<RegionKeySorter>(WorkflowStage::Sort);
        EXPECT_FALSE(registry.resolve(ComputationSignature(WorkflowStage::Sort), nullptr).empty());
    }

    TEST(RegistryLessGrammarTest, BuilderFindsLibraryOperations)
    {
        const auto grammar = ComputationGrammar::Builder()
                                 .rule(MathematicalOperation::GAIN, MathematicalTransformerConfig { .gain_factor = 2.0 })
                                 .rule(TemporalOperation::TIME_REVERSE)
                                 .build();

        const auto result = grammar->process(to_datum({ { 1.0, 2.0, 3.0 } }));
        EXPECT_EQ(channel_samples(result, 0), (std::vector<double> { 6.0, 4.0, 2.0 }));
    }

    TEST(RegistryLessGrammarTest, AssembleKeepsDistinctConfigsForRepeatedStages)
    {
        const auto grammar = ComputationGrammar::assemble(
            MathematicalOperation::GAIN | MathematicalOperation::GAIN,
            { MathematicalTransformerConfig { .gain_factor = 2.0 },
                MathematicalTransformerConfig { .gain_factor = 3.0 } });

        const auto result = grammar->process(to_datum({ { 1.0, 2.0 } }));
        EXPECT_EQ(channel_samples(result, 0), (std::vector<double> { 6.0, 12.0 }));
    }

    TEST(RegistryLessGrammarTest, UnknownSelectionNamesTheMissingImplementation)
    {
        EXPECT_THROW(ComputationGrammar::Builder().rule(WorkflowStage::Sort).build(), std::invalid_argument);
        EXPECT_THROW(ComputationGrammar::assemble(ComputationSignature(WorkflowStage::Sort), {}), std::invalid_argument);
    }

    TEST(RegistryLessGrammarTest, UsesSuppliesAnImplementationForAnUnregisteredSelection)
    {
        const auto grammar = ComputationGrammar::Builder()
                                 .rule(WorkflowStage::Sort)
                                 .uses(std::make_shared<RegionKeySorter>())
                                 .build();
        EXPECT_NE(grammar, nullptr);
    }

} // namespace
} // namespace MayaFlux::Test
