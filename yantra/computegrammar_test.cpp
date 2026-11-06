#include "MayaFlux/Yantra/ComputePipeline.hpp"
#include "MayaFlux/Yantra/FunctionalOperation.hpp"
#include "MayaFlux/Yantra/Transformers/MathematicalTransformer.hpp"
#include "MayaFlux/Yantra/Transformers/TemporalTransformer.hpp"
#include "transformer_helpers.hpp"

#include <gtest/gtest.h>

namespace MayaFlux::Test {
namespace {

    using namespace Yantra;
    using Channels = std::vector<std::vector<double>>;
    using Samples = std::vector<double>;
    using SampleDatum = Datum<Samples>;

    enum class Construction { Fluent, InitializerList, Vector };
    enum class UserMethod { Accumulate, Missing };
    enum class ForeignMethod { Accumulate };
    enum class BridgeMethod { Summarize, Project, Fail };

    const Facet<int> implementation_id {};

    struct AccumulatorConfig : OperationConfig {
        static constexpr auto describe_facets()
        {
            return std::tuple { FacetMember<&AccumulatorConfig::delta> {} };
        }

        double delta { 2.0 };
    };

    class Accumulator final : public ComputeOperation<Samples> {
    public:
        using supports = Supports<AccumulatorConfig>;

        explicit Accumulator(AccumulatorConfig config = {}, size_t minimum_samples = 1, int implementation = 1)
            : m_minimum_samples(minimum_samples)
            , m_implementation(implementation)
        {
            set_config(std::move(config));
        }

        OperationType get_operation_type() const override { return OperationType::CUSTOM; }

        bool validate_input(const input_type& input) const override
        {
            return input.data.size() >= m_minimum_samples
                && std::ranges::all_of(input.data, [](double value) { return std::isfinite(value); });
        }

    protected:
        output_type operation_function(const input_type& input) override
        {
            auto output = input;
            const double offset = config<AccumulatorConfig>().delta *static_cast<double>(++m_calls);
            for (double& sample : output.data) {
                sample += offset;
            }
            output.facets.set(implementation_id, m_implementation);
            return output;
        }

    private:
        size_t m_minimum_samples;
        int m_implementation;
        size_t m_calls {};
    };

    struct Admission {
        static constexpr auto describe_facets()
        {
            return std::tuple { FacetMember<&Admission::enabled> {} };
        }

        bool enabled {};
    };

    bool admitted(const std::any&, const ExecutionContext& context)
    {
        const auto* admission = context.facets.get<Admission>();
        return admission && admission->enabled;
    }

    std::shared_ptr<OperationRegistry> registry()
    {
        auto result = std::make_shared<OperationRegistry>();
        result->catalog<MathematicalOperation, MathematicalTransformer<>>();
        result->catalog<TemporalOperation, TemporalTransformer<>>();
        result->catalog<Accumulator>(UserMethod::Accumulate);
        return result;
    }

    std::shared_ptr<ComputationGrammar> construct(
        Construction construction,
        std::shared_ptr<const OperationRegistry> operations,
        ComputationSignature signature,
        std::initializer_list<OperationConfig> configs)
    {
        switch (construction) {
        case Construction::Fluent: {
            ComputationGrammar::Builder builder(std::move(operations));
            size_t stage = 0;
            for (const auto& config : configs) {
                builder.rule(signature.at(stage++), config);
            }
            return builder.build();
        }
        case Construction::InitializerList:
            return ComputationGrammar::assemble(std::move(operations), std::move(signature), configs);
        case Construction::Vector: {
            std::vector<OperationConfig> retained(configs);
            return ComputationGrammar::assemble(std::move(operations), std::move(signature), retained);
        }
        }
        throw std::logic_error("Unknown construction form");
    }

    std::string construction_name(Construction construction)
    {
        switch (construction) {
        case Construction::Fluent:
            return "Fluent";
        case Construction::InitializerList:
            return "InitializerList";
        case Construction::Vector:
            return "Vector";
        }
        return "Unknown";
    }

    void expect_channels(const DataIO& actual, const Channels& expected)
    {
        ASSERT_EQ(actual.data.size(), expected.size());
        for (size_t channel = 0; channel < expected.size(); ++channel) {
            SCOPED_TRACE(channel);
            ASSERT_TRUE(std::holds_alternative<Samples>(actual.data[channel]));
            const auto& samples = std::get<Samples>(actual.data[channel]);
            ASSERT_EQ(samples.size(), expected[channel].size());
            for (size_t i = 0; i < samples.size(); ++i) {
                EXPECT_NEAR(samples[i], expected[channel][i], 1e-12) << "Sample " << i;
            }
        }
    }

    struct ChannelCase {
        const char* name;
        Channels input;
        Channels expected;
    };

    const ChannelCase channel_cases[] {
        { "Mono", { { -3.0, 1.0, 4.0, 0.5 } }, { { 0.0, 7.0, 1.0, -7.0 } } },
        { "UnequalChannels", { { -2.0, 0.0, 3.0 }, { 5.0, -1.0 } }, { { 5.0, -1.0, -5.0 }, { -3.0, 9.0 } } },
        { "SingleSamples", { { 2.0 }, { -3.0 } }, { { 3.0 }, { -7.0 } } },
    };

    using CompositionParam = std::tuple<Construction, ChannelCase>;

    class GrammarCompositionTest : public ::testing::TestWithParam<CompositionParam> {
    protected:
        std::shared_ptr<ComputationGrammar> grammar() const
        {
            return construct(std::get<0>(GetParam()), registry(),
                MathematicalOperation::OFFSET | MathematicalOperation::GAIN
                    | TemporalOperation::TIME_REVERSE | MathematicalOperation::OFFSET,
                {
                    MathematicalTransformerConfig { .operation = MathematicalOperation::POWER, .offset_value = 1.0 },
                    MathematicalTransformerConfig { .operation = MathematicalOperation::OFFSET, .gain_factor = 2.0 },
                    TemporalTransformerConfig {},
                    MathematicalTransformerConfig { .operation = MathematicalOperation::GAIN, .offset_value = -3.0 },
                });
        }
    };

    INSTANTIATE_TEST_SUITE_P(ConstructionAndChannels, GrammarCompositionTest,
        ::testing::Combine(
            ::testing::Values(Construction::Fluent, Construction::InitializerList, Construction::Vector),
            ::testing::ValuesIn(channel_cases)),
        [](const ::testing::TestParamInfo<CompositionParam>& info) {
            return construction_name(std::get<0>(info.param)) + "_" + std::get<1>(info.param).name;
        });

    TEST_P(GrammarCompositionTest, ExecutesInOrderWithDistinctConfigsForRepeatedSelections)
    {
        const auto& data = std::get<1>(GetParam());
        const auto result = grammar()->process(to_datum(data.input));

        expect_channels(result, data.expected);
    }

    TEST_P(GrammarCompositionTest, BufferedStagesLeaveEveryInputSampleUnchanged)
    {
        const auto& data = std::get<1>(GetParam());
        const auto input = to_datum(data.input);

        const auto result = grammar()->process(input);

        expect_channels(input, data.input);
        expect_channels(result, data.expected);
    }

    TEST_P(GrammarCompositionTest, PipelineConsumesTheAssembledSignatureWithoutRepeatingIt)
    {
        const auto& data = std::get<1>(GetParam());
        ComputationPipeline<> pipeline(grammar());

        expect_channels(pipeline.process(to_datum(data.input)), data.expected);
    }

    TEST_P(GrammarCompositionTest, RetainsConfigsAfterTheirSourceCollectionsHaveExpired)
    {
        const auto& data = std::get<1>(GetParam());
        const auto assembled = grammar();

        expect_channels(assembled->process(to_datum(data.input)), data.expected);
        expect_channels(assembled->process(to_datum(data.input)), data.expected);
    }

    class GrammarLifecycleTest : public ::testing::TestWithParam<Construction> {
    };

    INSTANTIATE_TEST_SUITE_P(ConstructionForms, GrammarLifecycleTest,
        ::testing::Values(Construction::Fluent, Construction::InitializerList, Construction::Vector),
        [](const ::testing::TestParamInfo<Construction>& info) { return construction_name(info.param); });

    TEST_P(GrammarLifecycleTest, CustomRegisteredOperationRetainsStateAcrossCalls)
    {
        const auto grammar = construct(GetParam(), registry(), UserMethod::Accumulate,
            { AccumulatorConfig { .delta = 2.0 } });
        const SampleDatum input(Samples { 10.0, -2.0 });

        EXPECT_EQ(grammar->process(input).data, (Samples { 12.0, 0.0 }));
        EXPECT_EQ(grammar->process(input).data, (Samples { 14.0, 2.0 }));
    }

    TEST_P(GrammarLifecycleTest, RepeatedSelectionsOwnIndependentStateAndSettings)
    {
        const auto grammar = construct(GetParam(), registry(),
            UserMethod::Accumulate | UserMethod::Accumulate,
            { AccumulatorConfig { .delta = 2.0 }, AccumulatorConfig { .delta = 5.0 } });
        const SampleDatum input(Samples { 10.0, -2.0 });

        EXPECT_EQ(grammar->process(input).data, (Samples { 17.0, 5.0 }));
        EXPECT_EQ(grammar->process(input).data, (Samples { 24.0, 12.0 }));
    }

    TEST_P(GrammarLifecycleTest, GrammarsSharingARegistryDoNotShareMutableInstances)
    {
        const auto operations = registry();
        const auto first = construct(GetParam(), operations, UserMethod::Accumulate,
            { AccumulatorConfig { .delta = 2.0 } });
        const auto second = construct(GetParam(), operations, UserMethod::Accumulate,
            { AccumulatorConfig { .delta = 5.0 } });
        const SampleDatum input(Samples { 10.0, -2.0 });

        EXPECT_EQ(first->process(input).data, (Samples { 12.0, 0.0 }));
        EXPECT_EQ(second->process(input).data, (Samples { 15.0, 3.0 }));
        EXPECT_EQ(first->process(input).data, (Samples { 14.0, 2.0 }));
        EXPECT_EQ(second->process(input).data, (Samples { 20.0, 8.0 }));
    }

    TEST_P(GrammarLifecycleTest, TypedExecutionSettingsReachTheSelectedLiveOperation)
    {
        const auto grammar = construct(GetParam(), registry(), UserMethod::Accumulate,
            { AccumulatorConfig { .delta = 2.0 } });
        ExecutionContext context;
        context.facets.set(AccumulatorConfig { .delta = 7.0 });

        EXPECT_EQ(grammar->process(SampleDatum(Samples { 1.0, -1.0 }), context).data,
            (Samples { 8.0, 6.0 }));
    }

    TEST_P(GrammarLifecycleTest, UnknownSignaturesCannotExecuteAnOtherwiseCompatibleOperation)
    {
        const auto grammar = construct(GetParam(), registry(), UserMethod::Accumulate,
            { AccumulatorConfig {} });
        const SampleDatum input(Samples { 10.0 });

        EXPECT_THROW(grammar->process<Samples>(input, ForeignMethod::Accumulate), std::runtime_error);
        EXPECT_THROW(grammar->process<Samples>(input, UserMethod::Missing), std::runtime_error);
        EXPECT_EQ(grammar->process(input).data, (Samples { 12.0 }));
    }

    struct RoutingCase {
        const char* name;
        Samples input;
        Samples expected;
        int implementation;
    };

    const RoutingCase routing_cases[] {
        { "BelowBoundary", { 1.0, 2.0, 3.0 }, { 3.0, 4.0, 5.0 }, 1 },
        { "AtBoundary", { 1.0, 2.0, 3.0, 4.0 }, { 3.0, 4.0, 5.0, 6.0 }, 2 },
        { "AboveBoundary", { 1.0, 2.0, 3.0, 4.0, 5.0 }, { 3.0, 4.0, 5.0, 6.0, 7.0 }, 2 },
    };

    using RoutingParam = std::tuple<Construction, RoutingCase>;

    class GrammarRoutingTest : public ::testing::TestWithParam<RoutingParam> {
    };

    INSTANTIATE_TEST_SUITE_P(ConstructionAndApplicability, GrammarRoutingTest,
        ::testing::Combine(
            ::testing::Values(Construction::Fluent, Construction::InitializerList, Construction::Vector),
            ::testing::ValuesIn(routing_cases)),
        [](const ::testing::TestParamInfo<RoutingParam>& info) {
            return construction_name(std::get<0>(info.param)) + "_" + std::get<1>(info.param).name;
        });

    TEST_P(GrammarRoutingTest, ChoosesTheHighestPriorityCandidateThatActuallyAcceptsInput)
    {
        auto operations = registry();
        operations->catalog<Accumulator>(UserMethod::Accumulate,
            [](AccumulatorConfig config) { return std::make_shared<Accumulator>(config, 4, 2); }, 100);
        const auto grammar = construct(std::get<0>(GetParam()), operations, UserMethod::Accumulate,
            { AccumulatorConfig {} });
        const auto& data = std::get<1>(GetParam());

        const auto result = grammar->process(SampleDatum(data.input));

        EXPECT_EQ(result.data, data.expected);
        const auto* implementation = result.facets.get(implementation_id);
        ASSERT_NE(implementation, nullptr);
        EXPECT_EQ(*implementation, data.implementation);
    }

    TEST_P(GrammarLifecycleTest, UnselectedCandidatesDoNotAdvanceTheirState)
    {
        auto operations = registry();
        operations->catalog<Accumulator>(UserMethod::Accumulate,
            [](AccumulatorConfig config) { return std::make_shared<Accumulator>(config, 4, 2); }, 100);
        const auto grammar = construct(GetParam(), operations, UserMethod::Accumulate,
            { AccumulatorConfig {} });

        EXPECT_EQ(grammar->process(SampleDatum(Samples { 1.0 })).data, (Samples { 3.0 }));
        EXPECT_EQ(grammar->process(SampleDatum(Samples { 1.0, 2.0, 3.0, 4.0 })).data,
            (Samples { 3.0, 4.0, 5.0, 6.0 }));
        EXPECT_EQ(grammar->process(SampleDatum(Samples { 1.0 })).data, (Samples { 5.0 }));
    }

    enum class InvalidAssembly {
        TooFewConfigs,
        TooManyConfigs,
        ReversedConfigTypes,
        UnknownEnumWithSameInteger,
        UncatalogedMember,
        WrongConfigType,
        MissingLaterStage,
        NullRegistry,
        NullFactoryResult
    };

    using InvalidAssemblyParam = std::tuple<bool, InvalidAssembly>;

    class GrammarInvalidAssemblyTest : public ::testing::TestWithParam<InvalidAssemblyParam> {
    protected:
        void assemble(std::shared_ptr<const OperationRegistry> operations,
            ComputationSignature signature, std::initializer_list<OperationConfig> configs)
        {
            if (std::get<0>(GetParam())) {
                const std::vector<OperationConfig> retained(configs);
                static_cast<void>(ComputationGrammar::assemble(std::move(operations), std::move(signature), retained));
            } else {
                static_cast<void>(ComputationGrammar::assemble(std::move(operations), std::move(signature), configs));
            }
        }
    };

    INSTANTIATE_TEST_SUITE_P(ConfigCollections, GrammarInvalidAssemblyTest,
        ::testing::Combine(::testing::Bool(),
            ::testing::Values(
                InvalidAssembly::TooFewConfigs,
                InvalidAssembly::TooManyConfigs,
                InvalidAssembly::ReversedConfigTypes,
                InvalidAssembly::UnknownEnumWithSameInteger,
                InvalidAssembly::UncatalogedMember,
                InvalidAssembly::WrongConfigType,
                InvalidAssembly::MissingLaterStage,
                InvalidAssembly::NullRegistry,
                InvalidAssembly::NullFactoryResult)),
        [](const ::testing::TestParamInfo<InvalidAssemblyParam>& info) {
            return std::string(std::get<0>(info.param) ? "Vector_" : "InitializerList_")
                + std::string(Reflect::enum_to_string(std::get<1>(info.param)));
        });

    TEST_P(GrammarInvalidAssemblyTest, RejectsDeclarationsThatCannotProduceAnExecutableGrammar)
    {
        const auto operations = registry();
        switch (std::get<1>(GetParam())) {
        case InvalidAssembly::TooFewConfigs:
            EXPECT_THROW(assemble(operations, MathematicalOperation::GAIN | TemporalOperation::TIME_REVERSE,
                             { MathematicalTransformerConfig {} }),
                std::invalid_argument);
            break;
        case InvalidAssembly::TooManyConfigs:
            EXPECT_THROW(assemble(operations, MathematicalOperation::GAIN,
                             { MathematicalTransformerConfig {}, MathematicalTransformerConfig {} }),
                std::invalid_argument);
            break;
        case InvalidAssembly::ReversedConfigTypes:
            EXPECT_THROW(assemble(operations, MathematicalOperation::GAIN | TemporalOperation::TIME_REVERSE,
                             { TemporalTransformerConfig {}, MathematicalTransformerConfig {} }),
                std::invalid_argument);
            break;
        case InvalidAssembly::UnknownEnumWithSameInteger:
            EXPECT_THROW(assemble(operations, ForeignMethod::Accumulate, { AccumulatorConfig {} }),
                std::invalid_argument);
            break;
        case InvalidAssembly::UncatalogedMember:
            EXPECT_THROW(assemble(operations, UserMethod::Missing, { AccumulatorConfig {} }),
                std::invalid_argument);
            break;
        case InvalidAssembly::WrongConfigType:
            EXPECT_THROW(assemble(operations, UserMethod::Accumulate, { TemporalTransformerConfig {} }),
                std::invalid_argument);
            break;
        case InvalidAssembly::MissingLaterStage:
            EXPECT_THROW(assemble(operations, MathematicalOperation::GAIN | ForeignMethod::Accumulate,
                             { MathematicalTransformerConfig {}, AccumulatorConfig {} }),
                std::invalid_argument);
            break;
        case InvalidAssembly::NullRegistry:
            EXPECT_THROW(assemble(nullptr, UserMethod::Accumulate, { AccumulatorConfig {} }),
                std::invalid_argument);
            break;
        case InvalidAssembly::NullFactoryResult:
            operations->catalog<Accumulator>(UserMethod::Accumulate,
                [](AccumulatorConfig) -> std::shared_ptr<Accumulator> { return nullptr; });
            EXPECT_THROW(assemble(operations, UserMethod::Accumulate, { AccumulatorConfig {} }),
                std::invalid_argument);
            break;
        }
    }

    class GrammarRefinementTest : public ::testing::TestWithParam<bool> {
    };

    INSTANTIATE_TEST_SUITE_P(Admission, GrammarRefinementTest, ::testing::Bool(),
        [](const ::testing::TestParamInfo<bool>& info) { return info.param ? "Admitted" : "Rejected"; });

    TEST_P(GrammarRefinementTest, WhenGuardsExecutionWithoutDiscardingTheInputContract)
    {
        const auto grammar = ComputationGrammar::Builder(registry())
            .rule(UserMethod::Accumulate, AccumulatorConfig {})
            .when(Matcher::combine_and({ Matcher::create_type_matcher<Samples>(), admitted }))
            .build();
        ExecutionContext context;
        context.facets.set(Admission { GetParam() });
        const SampleDatum input(Samples { 10.0 });

        if (GetParam()) {
            EXPECT_EQ(grammar->process(input, context).data, (Samples { 12.0 }));
        } else {
            EXPECT_THROW(grammar->process(input, context), std::runtime_error);
        }

        context.facets.set(Admission { true });
        EXPECT_EQ(grammar->process(input, context).data,
            (GetParam() ? Samples { 14.0 } : Samples { 12.0 }));
        EXPECT_THROW(grammar->process(DataIO {}, context), std::runtime_error);
    }

    TEST_P(GrammarRefinementTest, CombinedRuleConsumesExactlyItsSignatureOrFallsBackToIndividualStages)
    {
        ComputationGrammar grammar;
        grammar.add_operation_rule<MathematicalTransformer<>>(
            "gain", MathematicalOperation::GAIN, MathematicalTransformerConfig { .gain_factor = 2.0 });
        grammar.add_operation_rule<MathematicalTransformer<>>(
            "offset", MathematicalOperation::OFFSET, MathematicalTransformerConfig { .offset_value = 3.0 });
        auto combined = std::make_shared<FunctionalOperation<>>([](const DataIO& input) {
            auto output = input;
            for (auto& channel : output.data) {
                Kinesis::Discrete::linear(std::get<Samples>(channel), 2.0, 3.0);
            }
            output.facets.set(implementation_id, 2);
            return output;
        });
        grammar.create_rule("combined")
            .matches(MathematicalOperation::GAIN | MathematicalOperation::OFFSET)
            .matches_custom(admitted)
            .with_priority(100)
            .uses(combined)
            .build();
        ExecutionContext context;
        context.facets.set(Admission { GetParam() });

        const auto result = grammar.process<std::vector<Kakshya::DataVariant>>(
            to_datum({ { -2.0, 0.0, 3.0 } }),
            MathematicalOperation::GAIN | MathematicalOperation::OFFSET, context);

        expect_channels(result, { { -1.0, 3.0, 9.0 } });
        const auto* implementation = result.facets.get(implementation_id);
        if (GetParam()) {
            ASSERT_NE(implementation, nullptr);
            EXPECT_EQ(*implementation, 2);
        } else {
            EXPECT_EQ(implementation, nullptr);
        }
    }

    TEST_P(GrammarRefinementTest, UsesReplacesTheRegisteredImplementationAndKeepsItsCondition)
    {
        const auto replacement = std::make_shared<Accumulator>(AccumulatorConfig {}, 1, 9);
        const auto grammar = ComputationGrammar::Builder(registry())
            .rule(UserMethod::Accumulate, AccumulatorConfig { .delta = 7.0 })
            .when(admitted)
            .uses(replacement)
            .build();
        ExecutionContext context;
        context.facets.set(Admission { GetParam() });

        if (!GetParam()) {
            EXPECT_THROW(grammar->process(SampleDatum(Samples { 1.0 }), context), std::runtime_error);
            context.facets.set(Admission { true });
        }
        const auto result = grammar->process(SampleDatum(Samples { 1.0 }), context);

        EXPECT_EQ(result.data, (Samples { 8.0 }));
        const auto* implementation = result.facets.get(implementation_id);
        ASSERT_NE(implementation, nullptr);
        EXPECT_EQ(*implementation, 9);
    }

    TEST_P(GrammarRefinementTest, PriorityRefinementAppliesToEveryRegisteredCandidate)
    {
        auto operations = registry();
        operations->catalog<Accumulator>(UserMethod::Accumulate,
            [](AccumulatorConfig config) { return std::make_shared<Accumulator>(config, 1, 2); }, 100);
        const auto grammar = ComputationGrammar::Builder(operations)
            .rule(UserMethod::Accumulate, AccumulatorConfig {})
            .priority(10)
            .build();
        grammar->create_rule("override")
            .matches(UserMethod::Accumulate)
            .matches_custom(admitted)
            .with_priority(50)
            .uses(std::make_shared<Accumulator>(AccumulatorConfig {}, 1, 9))
            .build();
        ExecutionContext context;
        context.facets.set(Admission { GetParam() });

        const auto result = grammar->process(SampleDatum(Samples { 1.0 }), context);

        EXPECT_EQ(result.data, (Samples { 3.0 }));
        const auto* implementation = result.facets.get(implementation_id);
        ASSERT_NE(implementation, nullptr);
        EXPECT_EQ(*implementation, GetParam() ? 9 : 1);
    }

    class GrammarDefaultsTest : public ::testing::TestWithParam<bool> {
    };

    INSTANTIATE_TEST_SUITE_P(ConstructionForms, GrammarDefaultsTest, ::testing::Bool(),
        [](const ::testing::TestParamInfo<bool>& info) { return info.param ? "Fluent" : "Assembled"; });

    TEST_P(GrammarDefaultsTest, EnumOnlyStagesReceiveTheSelectedMethodsAndTheirDefaultSettings)
    {
        const auto operations = registry();
        const auto grammar = GetParam()
            ? ComputationGrammar::Builder(operations)
                  .rule(MathematicalOperation::NORMALIZE)
                  .rule(TemporalOperation::TIME_REVERSE)
                  .build()
            : ComputationGrammar::assemble(operations,
                  MathematicalOperation::NORMALIZE | TemporalOperation::TIME_REVERSE);

        expect_channels(grammar->process(to_datum({ { -2.0, 0.0, 2.0 } })), { { 1.0, 0.0, -1.0 } });
    }

    enum class InvalidInput { NoChannels, NotANumber, Infinity };
    using InvalidInputParam = std::tuple<Construction, InvalidInput>;

    class GrammarInvalidInputTest : public ::testing::TestWithParam<InvalidInputParam> {
    };

    INSTANTIATE_TEST_SUITE_P(ConstructionAndInvalidData, GrammarInvalidInputTest,
        ::testing::Combine(
            ::testing::Values(Construction::Fluent, Construction::InitializerList, Construction::Vector),
            ::testing::Values(InvalidInput::NoChannels, InvalidInput::NotANumber, InvalidInput::Infinity)),
        [](const ::testing::TestParamInfo<InvalidInputParam>& info) {
            return construction_name(std::get<0>(info.param)) + "_"
                + std::string(Reflect::enum_to_string(std::get<1>(info.param)));
        });

    TEST_P(GrammarInvalidInputTest, RejectsInvalidDataInsteadOfReturningAnUnprocessedSuccess)
    {
        const auto grammar = construct(std::get<0>(GetParam()), registry(), MathematicalOperation::GAIN,
            { MathematicalTransformerConfig { .gain_factor = 2.0 } });
        DataIO input;
        switch (std::get<1>(GetParam())) {
        case InvalidInput::NoChannels:
            break;
        case InvalidInput::NotANumber:
            input = to_datum({ { 1.0, std::numeric_limits<double>::quiet_NaN() } });
            break;
        case InvalidInput::Infinity:
            input = to_datum({ { 1.0, std::numeric_limits<double>::infinity() } });
            break;
        }

        EXPECT_THROW(grammar->process(input), std::runtime_error);
    }

    enum class GrammarConsumer { Pipeline, Matrix };

    class GrammarConsumerTest : public ::testing::TestWithParam<GrammarConsumer> {
    protected:
        void SetUp() override
        {
            shared = std::make_shared<ComputationGrammar>();
            for (size_t i = 0; i < 2; ++i) {
                if (GetParam() == GrammarConsumer::Pipeline) {
                    pipelines[i] = std::make_unique<ComputationPipeline<>>(shared);
                } else {
                    matrices[i] = std::make_unique<GrammarAwareComputeMatrix>(shared);
                }
            }
        }

        DataIO process(size_t consumer, const DataIO& input, const ExecutionContext& context = {})
        {
            return GetParam() == GrammarConsumer::Pipeline
                ? pipelines[consumer]->process(input, context)
                : matrices[consumer]->execute_with_grammar(input, context);
        }

        void replace(size_t consumer, std::shared_ptr<ComputationGrammar> grammar)
        {
            if (GetParam() == GrammarConsumer::Pipeline) {
                pipelines[consumer]->set_grammar(std::move(grammar));
            } else {
                matrices[consumer]->set_grammar(std::move(grammar));
            }
        }

        std::shared_ptr<ComputationGrammar> shared;
        std::array<std::unique_ptr<ComputationPipeline<>>, 2> pipelines;
        std::array<std::unique_ptr<GrammarAwareComputeMatrix>, 2> matrices;
    };

    INSTANTIATE_TEST_SUITE_P(Consumers, GrammarConsumerTest,
        ::testing::Values(GrammarConsumer::Pipeline, GrammarConsumer::Matrix),
        [](const ::testing::TestParamInfo<GrammarConsumer>& info) {
            return info.param == GrammarConsumer::Pipeline ? "Pipeline" : "Matrix";
        });

    TEST_P(GrammarConsumerTest, LiveRuleUpdatesReachEveryConsumerSharingTheGrammar)
    {
        const auto input = to_datum({ { -2.0, 1.0, 3.0 } });
        shared->add_operation_rule<MathematicalTransformer<>>(
            "double", Matcher::create_type_matcher<std::vector<Kakshya::DataVariant>>(),
            MathematicalTransformerConfig { .gain_factor = 2.0 }, 50);

        expect_channels(process(0, input), { { -4.0, 2.0, 6.0 } });
        expect_channels(process(1, input), { { -4.0, 2.0, 6.0 } });

        shared->add_operation_rule<MathematicalTransformer<>>(
            "quadruple", Matcher::create_type_matcher<std::vector<Kakshya::DataVariant>>(),
            MathematicalTransformerConfig { .gain_factor = 4.0 }, 100);

        expect_channels(process(0, input), { { -8.0, 4.0, 12.0 } });
        expect_channels(process(1, input), { { -8.0, 4.0, 12.0 } });
    }

    TEST_P(GrammarConsumerTest, ReplacingOneConsumersGrammarLeavesTheOtherConsumerUnchanged)
    {
        const auto input = to_datum({ { -2.0, 1.0, 3.0 } });
        shared->add_operation_rule<MathematicalTransformer<>>(
            "double", Matcher::create_type_matcher<std::vector<Kakshya::DataVariant>>(),
            MathematicalTransformerConfig { .gain_factor = 2.0 });
        auto replacement = std::make_shared<ComputationGrammar>();
        replacement->add_operation_rule<MathematicalTransformer<>>(
            "quintuple", Matcher::create_type_matcher<std::vector<Kakshya::DataVariant>>(),
            MathematicalTransformerConfig { .gain_factor = 5.0 });

        replace(0, replacement);

        expect_channels(process(0, input), { { -10.0, 5.0, 15.0 } });
        expect_channels(process(1, input), { { -4.0, 2.0, 6.0 } });
    }

    TEST_P(GrammarConsumerTest, ARejectedPrepassPreservesInputUntilItsConditionIsSatisfied)
    {
        shared->create_rule("gated")
            .matches_custom(admitted)
            .uses(std::make_shared<MathematicalTransformer<>>(
                MathematicalTransformerConfig { .gain_factor = 2.0 }))
            .build();
        const auto input = to_datum({ { -2.0, 1.0, 3.0 } });

        expect_channels(process(0, input), { { -2.0, 1.0, 3.0 } });
        ExecutionContext context;
        context.facets.set(Admission { true });
        expect_channels(process(0, input, context), { { -4.0, 2.0, 6.0 } });
    }

    enum class PipelineMutation { Reconfigure, RemoveFirstDuplicate, Clear };
    using PipelineMutationParam = std::tuple<bool, PipelineMutation>;

    class GrammarPipelineMutationTest : public ::testing::TestWithParam<PipelineMutationParam> {
    };

    INSTANTIATE_TEST_SUITE_P(PrepassAndMutation, GrammarPipelineMutationTest,
        ::testing::Combine(::testing::Bool(),
            ::testing::Values(PipelineMutation::Reconfigure, PipelineMutation::RemoveFirstDuplicate,
                PipelineMutation::Clear)),
        [](const ::testing::TestParamInfo<PipelineMutationParam>& info) {
            return std::string(std::get<0>(info.param) ? "WithPrepass_" : "WithoutPrepass_")
                + std::string(Reflect::enum_to_string(std::get<1>(info.param)));
        });

    TEST_P(GrammarPipelineMutationTest, MutatingTheLiveChainChangesResultsWithoutRemovingItsGrammarPrepass)
    {
        const bool prepass = std::get<0>(GetParam());
        auto grammar = std::make_shared<ComputationGrammar>();
        if (prepass) {
            grammar->add_operation_rule<MathematicalTransformer<>>(
                "halve", Matcher::create_type_matcher<std::vector<Kakshya::DataVariant>>(),
                MathematicalTransformerConfig { .gain_factor = 0.5 });
        }
        ComputationPipeline<> pipeline(grammar);
        pipeline.create_operation<MathematicalTransformer<>>("same",
            MathematicalTransformerConfig { .gain_factor = 2.0 });
        pipeline.create_operation<MathematicalTransformer<>>("same",
            MathematicalTransformerConfig { .operation = MathematicalOperation::OFFSET, .offset_value = 3.0 });
        const auto input = to_datum({ { -2.0, 1.0, 3.0 } });

        expect_channels(pipeline.process(input), prepass ? Channels { { 1.0, 4.0, 6.0 } } : Channels { { -1.0, 5.0, 9.0 } });
        ComputationPipeline<> peer(grammar);
        expect_channels(peer.process(input), prepass ? Channels { { -1.0, 0.5, 1.5 } } : Channels { { -2.0, 1.0, 3.0 } });

        switch (std::get<1>(GetParam())) {
        case PipelineMutation::Reconfigure:
            ASSERT_TRUE(pipeline.configure_operation<MathematicalTransformer<>>("same", [](auto operation) {
                operation->set_config(MathematicalTransformerConfig { .gain_factor = 4.0 });
            }));
            expect_channels(pipeline.process(input), prepass ? Channels { { -1.0, 5.0, 9.0 } } : Channels { { -5.0, 7.0, 15.0 } });
            break;
        case PipelineMutation::RemoveFirstDuplicate:
            ASSERT_TRUE(pipeline.remove_operation("same"));
            expect_channels(pipeline.process(input), prepass ? Channels { { 2.0, 3.5, 4.5 } } : Channels { { 1.0, 4.0, 6.0 } });
            break;
        case PipelineMutation::Clear:
            pipeline.clear_operations();
            expect_channels(pipeline.process(input), prepass ? Channels { { -1.0, 0.5, 1.5 } } : Channels { { -2.0, 1.0, 3.0 } });
            break;
        }
        expect_channels(peer.process(input), prepass ? Channels { { -1.0, 0.5, 1.5 } } : Channels { { -2.0, 1.0, 3.0 } });
    }

    enum class Transition { RoundTrip, IncompatibleIntermediate, WrongFinalType, ThrowingStage };

    class GrammarTransitionTest : public ::testing::TestWithParam<Transition> {
    };

    INSTANTIATE_TEST_SUITE_P(ErasedStageContracts, GrammarTransitionTest,
        ::testing::Values(Transition::RoundTrip, Transition::IncompatibleIntermediate,
            Transition::WrongFinalType, Transition::ThrowingStage),
        [](const ::testing::TestParamInfo<Transition>& info) {
            return std::string(Reflect::enum_to_string(info.param));
        });

    TEST_P(GrammarTransitionTest, ChecksActualIntermediateAndFinalTypesAndPropagatesExecutionFailure)
    {
        const auto operations = std::make_shared<OperationRegistry>();
        auto summarize = std::make_shared<FunctionalOperation<Samples, Eigen::VectorXd>>(
            [](const SampleDatum& input) {
                Eigen::VectorXd summary(2);
                summary[0] = std::accumulate(input.data.begin(), input.data.end(), 0.0);
                summary[1] = static_cast<double>(input.data.size());
                return Datum<Eigen::VectorXd>(std::move(summary));
            });
        auto project = std::make_shared<FunctionalOperation<Eigen::VectorXd, Samples>>(
            [](const Datum<Eigen::VectorXd>& input) {
                return SampleDatum(Samples { input.data[0] / input.data[1], input.data[0] });
            });
        const SampleDatum input(Samples { 1.0, 3.0, 5.0 });

        switch (GetParam()) {
        case Transition::RoundTrip: {
            const auto grammar = ComputationGrammar::Builder(operations)
                .rule(BridgeMethod::Summarize).uses(summarize)
                .rule(BridgeMethod::Project).uses(project)
                .build();
            EXPECT_EQ(grammar->process(input).data, (Samples { 3.0, 9.0 }));
            break;
        }
        case Transition::IncompatibleIntermediate: {
            const auto grammar = ComputationGrammar::Builder(operations)
                .rule(BridgeMethod::Summarize).uses(summarize)
                .rule(UserMethod::Accumulate).uses(std::make_shared<Accumulator>())
                .build();
            EXPECT_THROW(grammar->process(input), std::runtime_error);
            break;
        }
        case Transition::WrongFinalType: {
            const auto grammar = ComputationGrammar::Builder(operations)
                .rule(BridgeMethod::Summarize).uses(summarize)
                .build();
            EXPECT_THROW(grammar->process(input), std::runtime_error);
            const auto summary = grammar->process<Eigen::VectorXd>(input, grammar->signature());
            ASSERT_EQ(summary.data.size(), 2);
            EXPECT_DOUBLE_EQ(summary.data[0], 9.0);
            EXPECT_DOUBLE_EQ(summary.data[1], 3.0);
            break;
        }
        case Transition::ThrowingStage: {
            auto failure = std::make_shared<FunctionalOperation<Samples>>(
                [](const SampleDatum&) -> SampleDatum { throw std::domain_error("Unavailable computation"); });
            auto downstream = std::make_shared<Accumulator>();
            const auto grammar = ComputationGrammar::Builder(operations)
                .rule(BridgeMethod::Fail).uses(failure)
                .rule(UserMethod::Accumulate).uses(downstream)
                .build();
            EXPECT_THROW(grammar->process(input), std::domain_error);
            EXPECT_EQ(downstream->apply_operation(input).data, (Samples { 3.0, 5.0, 7.0 }));
            break;
        }
        }
    }

}
}
