#include "MayaFlux/Yantra/ComputeMatrix.hpp"
#include "MayaFlux/Yantra/Transformers/MathematicalTransformer.hpp"
#include "transformer_helpers.hpp"

#include <gtest/gtest.h>

namespace MayaFlux::Test {
namespace {

    using namespace Yantra;
    using Samples = std::vector<double>;
    using SampleDatum = Datum<Samples>;
    using Variants = std::vector<Kakshya::DataVariant>;
    using Channels = std::vector<Samples>;

    enum class ExecutionPath { Direct, Named, Supplied, Async, NamedAsync, Fluent };
    enum class Lifetime { Fresh, Named, Supplied, AsyncFresh, AsyncNamed };
    enum class PoolMutation { DuplicateAdd, DuplicateCreate, WrongType, Remove, Clear, Null };
    enum class ChainPath { Direct, Named, Fluent };

    class AffineOperation final : public ComputeOperation<Samples> {
    public:
        explicit AffineOperation(double scale = 2.0, double offset = 1.0, bool reject_negative = false)
            : m_scale(scale)
            , m_offset(offset)
            , m_reject_negative(reject_negative)
        {
        }

        OperationType get_operation_type() const override { return OperationType::TRANSFORMER; }

    protected:
        output_type operation_function(const input_type& input) override
        {
            auto output = input;
            for (double& sample : output.data) {
                if (!std::isfinite(sample) || (m_reject_negative && sample < 0.0)) {
                    throw std::domain_error("Sample is outside the operation domain");
                }
                sample = sample * m_scale + m_offset;
            }
            return output;
        }

    private:
        const double m_scale;
        const double m_offset;
        const bool m_reject_negative;
    };

    class RunningOffset final : public ComputeOperation<Samples> {
    public:
        explicit RunningOffset(double delta = 2.0)
            : m_delta(delta)
        {
        }

        OperationType get_operation_type() const override { return OperationType::CUSTOM; }

    protected:
        output_type operation_function(const input_type& input) override
        {
            auto output = input;
            const double offset = m_delta * static_cast<double>(++m_calls);
            for (double& sample : output.data) {
                sample += offset;
            }
            return output;
        }

    private:
        double m_delta;
        size_t m_calls {};
    };

    class Summarize final : public ComputeOperation<Samples, Eigen::VectorXd> {
    public:
        OperationType get_operation_type() const override { return OperationType::ANALYZER; }

    protected:
        output_type operation_function(const input_type& input) override
        {
            if (input.data.empty()) {
                throw std::domain_error("Cannot summarize an empty input");
            }
            Eigen::VectorXd result(2);
            result[0] = std::accumulate(input.data.begin(), input.data.end(), 0.0);
            result[1] = static_cast<double>(input.data.size());
            return output_type(std::move(result));
        }
    };

    class MeanAndSum final : public ComputeOperation<Eigen::VectorXd, Samples> {
    public:
        OperationType get_operation_type() const override { return OperationType::TRANSFORMER; }

    protected:
        output_type operation_function(const input_type& input) override
        {
            if (input.data.size() != 2 || input.data[1] <= 0.0) {
                throw std::domain_error("Invalid summary");
            }
            return output_type(Samples { input.data[0] / input.data[1], input.data[0] });
        }
    };

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
        { "Mono", { { -3.0, 1.0, 4.0, 0.5 } }, { { -6.0, 2.0, 8.0, 1.0 } } },
        { "UnequalChannels", { { -2.0, 0.0, 3.0 }, { 5.0, -1.0 } }, { { -4.0, 0.0, 6.0 }, { 10.0, -2.0 } } },
        { "SingleSamples", { { 2.0 }, { -3.0 } }, { { 4.0 }, { -6.0 } } },
    };

    using ExecutionParam = std::tuple<ExecutionPath, ChannelCase>;

    class MatrixExecutionTest : public ::testing::TestWithParam<ExecutionParam> {
    protected:
        std::optional<DataIO> execute(const std::shared_ptr<ComputeMatrix>& matrix, const DataIO& input)
        {
            const MathematicalTransformerConfig config { .gain_factor = 2.0 };
            switch (std::get<0>(GetParam())) {
            case ExecutionPath::Direct:
                return matrix->execute<MathematicalTransformer<>, Variants>(input, config);
            case ExecutionPath::Named:
                return matrix->execute_named<MathematicalTransformer<>, Variants>("gain", input);
            case ExecutionPath::Supplied:
                return matrix->execute_with(std::make_shared<MathematicalTransformer<>>(config), input);
            case ExecutionPath::Async:
                return matrix->execute_async<MathematicalTransformer<>, Variants>(input, config).get();
            case ExecutionPath::NamedAsync:
                return matrix->execute_named_async<MathematicalTransformer<>, Variants>("gain", input).get();
            case ExecutionPath::Fluent:
                return matrix->with(input).then<MathematicalTransformer<>>("gain").to_io();
            }
            throw std::logic_error("Unknown execution path");
        }
    };

    INSTANTIATE_TEST_SUITE_P(PathsAndChannels, MatrixExecutionTest,
        ::testing::Combine(
            ::testing::Values(ExecutionPath::Direct, ExecutionPath::Named, ExecutionPath::Supplied,
                ExecutionPath::Async, ExecutionPath::NamedAsync, ExecutionPath::Fluent),
            ::testing::ValuesIn(channel_cases)),
        [](const ::testing::TestParamInfo<ExecutionParam>& info) {
            return std::string(Reflect::enum_to_string(std::get<0>(info.param))) + "_" + std::get<1>(info.param).name;
        });

    TEST_P(MatrixExecutionTest, EveryExecutionPathProducesEveryExpectedSample)
    {
        const auto matrix = ComputeMatrix::create();
        matrix->create_operation<MathematicalTransformer<>>("gain",
            MathematicalTransformerConfig { .gain_factor = 2.0 });
        const auto& data = std::get<1>(GetParam());

        const auto result = execute(matrix, to_datum(data.input));

        ASSERT_TRUE(result.has_value());
        expect_channels(*result, data.expected);
    }

    TEST_P(MatrixExecutionTest, BufferedExecutionLeavesInputUnchangedAndCanBeRepeated)
    {
        const auto matrix = ComputeMatrix::create();
        matrix->create_operation<MathematicalTransformer<>>("gain",
            MathematicalTransformerConfig { .gain_factor = 2.0 });
        const auto& data = std::get<1>(GetParam());
        const auto input = to_datum(data.input);

        const auto first = execute(matrix, input);
        const auto second = execute(matrix, input);

        ASSERT_TRUE(first.has_value());
        ASSERT_TRUE(second.has_value());
        expect_channels(*first, data.expected);
        expect_channels(*second, data.expected);
        expect_channels(input, data.input);
    }

    class MatrixLifetimeTest : public ::testing::TestWithParam<Lifetime> {
    };

    INSTANTIATE_TEST_SUITE_P(InstanceLifetimes, MatrixLifetimeTest,
        ::testing::Values(Lifetime::Fresh, Lifetime::Named, Lifetime::Supplied, Lifetime::AsyncFresh, Lifetime::AsyncNamed),
        [](const ::testing::TestParamInfo<Lifetime>& info) { return std::string(Reflect::enum_to_string(info.param)); });

    TEST_P(MatrixLifetimeTest, FreshExecutionAndPersistentExecutionHaveDistinctStateLifetimes)
    {
        const auto matrix = ComputeMatrix::create();
        matrix->create_operation<RunningOffset>("state");
        const auto supplied = std::make_shared<RunningOffset>();
        const SampleDatum input(Samples { 10.0, -2.0 });
        auto execute = [&]() -> std::optional<SampleDatum> {
            switch (GetParam()) {
            case Lifetime::Fresh:
                return matrix->execute<RunningOffset, Samples>(input);
            case Lifetime::Named:
                return matrix->execute_named<RunningOffset, Samples>("state", input);
            case Lifetime::Supplied:
                return matrix->execute_with(supplied, input);
            case Lifetime::AsyncFresh:
                return matrix->execute_async<RunningOffset, Samples>(input).get();
            case Lifetime::AsyncNamed:
                return matrix->execute_named_async<RunningOffset, Samples>("state", input).get();
            }
            throw std::logic_error("Unknown lifetime");
        };

        const auto first = execute();
        const auto second = execute();

        ASSERT_TRUE(first.has_value());
        ASSERT_TRUE(second.has_value());
        EXPECT_EQ(first->data, (Samples { 12.0, 0.0 }));
        const bool fresh = GetParam() == Lifetime::Fresh || GetParam() == Lifetime::AsyncFresh;
        EXPECT_EQ(second->data, (fresh ? Samples { 12.0, 0.0 } : Samples { 14.0, 2.0 }));
    }

    class MatrixPoolTest : public ::testing::TestWithParam<PoolMutation> {
    };

    INSTANTIATE_TEST_SUITE_P(OwnershipBoundaries, MatrixPoolTest,
        ::testing::Values(PoolMutation::DuplicateAdd, PoolMutation::DuplicateCreate, PoolMutation::WrongType,
            PoolMutation::Remove, PoolMutation::Clear, PoolMutation::Null),
        [](const ::testing::TestParamInfo<PoolMutation>& info) { return std::string(Reflect::enum_to_string(info.param)); });

    TEST_P(MatrixPoolTest, PoolMutationsPreserveOrRemoveTheIntendedLiveOperation)
    {
        const auto matrix = ComputeMatrix::create();
        matrix->create_operation<AffineOperation>("active", 2.0, 1.0);
        matrix->create_operation<AffineOperation>("other", 3.0, -2.0);
        const SampleDatum input(Samples { -2.0, 1.0, 3.0 });

        switch (GetParam()) {
        case PoolMutation::DuplicateAdd:
            EXPECT_FALSE(matrix->add_operation("active", std::make_shared<AffineOperation>(100.0, 100.0)));
            break;
        case PoolMutation::DuplicateCreate:
            EXPECT_EQ(matrix->create_operation<AffineOperation>("active", 100.0, 100.0), nullptr);
            break;
        case PoolMutation::WrongType:
            EXPECT_FALSE((matrix->execute_named<RunningOffset, Samples>("active", input)).has_value());
            break;
        case PoolMutation::Remove:
            ASSERT_TRUE(matrix->remove_operation("active"));
            EXPECT_FALSE((matrix->execute_named<AffineOperation, Samples>("active", input)).has_value());
            ASSERT_NE(matrix->create_operation<AffineOperation>("active", 2.0, 1.0), nullptr);
            break;
        case PoolMutation::Clear:
            matrix->clear_operations();
            EXPECT_FALSE((matrix->execute_named<AffineOperation, Samples>("active", input)).has_value());
            EXPECT_FALSE((matrix->execute_named<AffineOperation, Samples>("other", input)).has_value());
            return;
        case PoolMutation::Null: {
            std::shared_ptr<AffineOperation> absent;
            EXPECT_FALSE(matrix->add_operation("null", absent));
            EXPECT_FALSE(matrix->execute_with(absent, input).has_value());
            break;
        }
        }
        const auto active = matrix->execute_named<AffineOperation, Samples>("active", input);
        const auto other = matrix->execute_named<AffineOperation, Samples>("other", input);

        ASSERT_TRUE(active.has_value());
        ASSERT_TRUE(other.has_value());
        EXPECT_EQ(active->data, (Samples { -3.0, 3.0, 7.0 }));
        EXPECT_EQ(other->data, (Samples { -8.0, 1.0, 7.0 }));
    }

    class MatrixDetachedOperationTest : public ::testing::TestWithParam<bool> {
    };

    INSTANTIATE_TEST_SUITE_P(RemovalForms, MatrixDetachedOperationTest, ::testing::Bool(),
        [](const ::testing::TestParamInfo<bool>& info) { return info.param ? "Clear" : "Remove"; });

    TEST_P(MatrixDetachedOperationTest, RemovingThePoolEntryPreservesAnExternallyHeldLiveInstance)
    {
        const auto matrix = ComputeMatrix::create();
        matrix->create_operation<RunningOffset>("state");
        const auto retained = matrix->get_operation<RunningOffset>("state");
        ASSERT_NE(retained, nullptr);
        const SampleDatum input(Samples { 10.0 });
        const auto first = matrix->execute_named<RunningOffset, Samples>("state", input);
        ASSERT_TRUE(first.has_value());
        EXPECT_EQ(first->data, (Samples { 12.0 }));

        if (GetParam()) {
            matrix->clear_operations();
        } else {
            ASSERT_TRUE(matrix->remove_operation("state"));
        }
        EXPECT_FALSE((matrix->execute_named<RunningOffset, Samples>("state", input)).has_value());
        const auto second = matrix->execute_with(retained, input);

        ASSERT_TRUE(second.has_value());
        EXPECT_EQ(second->data, (Samples { 14.0 }));
    }

    struct BatchCase {
        const char* name;
        std::vector<Samples> inputs;
        std::vector<std::optional<Samples>> expected;
        bool reject_negative;
    };

    const BatchCase batch_cases[] {
        { "Empty", {}, {}, false },
        { "VariedSizes", { { 1.0, -2.0 }, {}, { 3.0 } },
            { Samples { 3.0, -3.0 }, Samples {}, Samples { 7.0 } }, false },
        { "FailureFirst", { { -1.0 }, { 3.0, 4.0 }, { 5.0, 6.0 } },
            { std::nullopt, Samples { 7.0, 9.0 }, Samples { 11.0, 13.0 } }, true },
        { "FailureMiddle", { { 1.0, 2.0 }, { -1.0 }, { 5.0, 6.0 } },
            { Samples { 3.0, 5.0 }, std::nullopt, Samples { 11.0, 13.0 } }, true },
        { "FailureLast", { { 1.0, 2.0 }, { 3.0, 4.0 }, { -1.0 } },
            { Samples { 3.0, 5.0 }, Samples { 7.0, 9.0 }, std::nullopt }, true },
    };

    using BatchParam = std::tuple<bool, BatchCase>;

    class MatrixBatchTest : public ::testing::TestWithParam<BatchParam> {
    };

    INSTANTIATE_TEST_SUITE_P(ExecutionAndFailurePositions, MatrixBatchTest,
        ::testing::Combine(::testing::Bool(), ::testing::ValuesIn(batch_cases)),
        [](const ::testing::TestParamInfo<BatchParam>& info) {
            return std::string(std::get<0>(info.param) ? "Parallel_" : "Sequential_") + std::get<1>(info.param).name;
        });

    TEST_P(MatrixBatchTest, PreservesInputOrderAndFailureHolesWithoutDroppingSuccessfulResults)
    {
        const auto matrix = ComputeMatrix::create();
        const auto& data = std::get<1>(GetParam());
        std::vector<SampleDatum> inputs;
        for (const auto& samples : data.inputs) {
            inputs.emplace_back(samples);
        }

        const auto results = std::get<0>(GetParam())
            ? matrix->execute_batch_parallel<AffineOperation, Samples>(inputs, 2.0, 1.0, data.reject_negative)
            : matrix->execute_batch<AffineOperation, Samples>(inputs, 2.0, 1.0, data.reject_negative);

        ASSERT_EQ(results.size(), data.expected.size());
        for (size_t i = 0; i < results.size(); ++i) {
            SCOPED_TRACE(i);
            ASSERT_EQ(results[i].has_value(), data.expected[i].has_value());
            if (data.expected[i]) {
                EXPECT_EQ(results[i]->data, *data.expected[i]);
            }
            EXPECT_EQ(inputs[i].data, data.inputs[i]);
        }
        const auto stats = matrix->get_statistics();
        EXPECT_EQ(std::any_cast<size_t>(stats.at("total_executions")), data.inputs.size());
        EXPECT_EQ(std::any_cast<size_t>(stats.at("failed_executions")), data.reject_negative ? 1U : 0U);
    }

    class MatrixParallelNamedTest : public ::testing::TestWithParam<bool> {
    };

    INSTANTIATE_TEST_SUITE_P(NameLists, MatrixParallelNamedTest, ::testing::Bool(),
        [](const ::testing::TestParamInfo<bool>& info) { return info.param ? "MissingMiddle" : "RepeatedName"; });

    TEST_P(MatrixParallelNamedTest, ResultSlotsFollowTheRequestedNamesRatherThanCompletionOrPoolOrder)
    {
        const auto matrix = ComputeMatrix::create();
        matrix->create_operation<AffineOperation>("a", 2.0, 1.0);
        matrix->create_operation<AffineOperation>("b", -1.0, 3.0);
        matrix->create_operation<AffineOperation>("c", 0.5, 0.0);
        const std::vector<std::string> names = GetParam()
            ? std::vector<std::string> { "b", "missing", "a" }
            : std::vector<std::string> { "b", "a", "b", "c" };
        const std::vector<std::optional<Samples>> expected = GetParam()
            ? std::vector<std::optional<Samples>> { Samples { 1.0, 7.0 }, std::nullopt, Samples { 5.0, -7.0 } }
            : std::vector<std::optional<Samples>> {
                  Samples { 1.0, 7.0 }, Samples { 5.0, -7.0 }, Samples { 1.0, 7.0 }, Samples { 1.0, -2.0 } };

        const auto results = matrix->execute_parallel_named<AffineOperation, Samples>(
            names, SampleDatum(Samples { 2.0, -4.0 }));

        ASSERT_EQ(results.size(), expected.size());
        for (size_t i = 0; i < results.size(); ++i) {
            SCOPED_TRACE(i);
            ASSERT_EQ(results[i].has_value(), expected[i].has_value());
            if (expected[i]) {
                EXPECT_EQ(results[i]->data, *expected[i]);
            }
        }
    }

    struct ChainCase {
        const char* name;
        Samples input;
        Samples expected;
    };

    const ChainCase chain_cases[] {
        { "SeveralSamples", { 1.0, 3.0, 5.0 }, { 3.0, 9.0 } },
        { "MixedSigns", { -2.0, 8.0 }, { 3.0, 6.0 } },
        { "Singleton", { 4.0 }, { 4.0, 4.0 } },
    };

    using ChainParam = std::tuple<ChainPath, ChainCase>;

    class MatrixChainTest : public ::testing::TestWithParam<ChainParam> {
    };

    INSTANTIATE_TEST_SUITE_P(ExecutionAndTypeTransitions, MatrixChainTest,
        ::testing::Combine(::testing::Values(ChainPath::Direct, ChainPath::Named, ChainPath::Fluent),
            ::testing::ValuesIn(chain_cases)),
        [](const ::testing::TestParamInfo<ChainParam>& info) {
            return std::string(Reflect::enum_to_string(std::get<0>(info.param))) + "_" + std::get<1>(info.param).name;
        });

    TEST_P(MatrixChainTest, PassesTheActualTypedIntermediateToTheNextStage)
    {
        const auto matrix = ComputeMatrix::create();
        matrix->create_operation<Summarize>("summary");
        matrix->create_operation<MeanAndSum>("projection");
        const auto& data = std::get<1>(GetParam());
        const SampleDatum input(data.input);
        std::optional<SampleDatum> result;

        switch (std::get<0>(GetParam())) {
        case ChainPath::Direct:
            result = matrix->execute_chain<Summarize, MeanAndSum, Samples, Eigen::VectorXd, Samples>(input);
            break;
        case ChainPath::Named:
            result = matrix->execute_chain_named<Summarize, MeanAndSum, Samples, Eigen::VectorXd, Samples>(
                "summary", "projection", input);
            break;
        case ChainPath::Fluent:
            result = matrix->with(input)
                         .then<Summarize, Eigen::VectorXd>("summary")
                         .then<MeanAndSum, Samples>("projection")
                         .to_io();
            break;
        }

        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->data, data.expected);
        EXPECT_EQ(input.data, data.input);
    }

    class MatrixChainFailureTest : public ::testing::TestWithParam<ChainPath> {
    };

    INSTANTIATE_TEST_SUITE_P(ChainFailurePaths, MatrixChainFailureTest,
        ::testing::Values(ChainPath::Direct, ChainPath::Named, ChainPath::Fluent),
        [](const ::testing::TestParamInfo<ChainPath>& info) { return std::string(Reflect::enum_to_string(info.param)); });

    TEST_P(MatrixChainFailureTest, StopsAfterTheFirstFailureAndCanExecuteAValidChainAfterward)
    {
        const auto matrix = ComputeMatrix::create();
        matrix->create_operation<Summarize>("summary");
        matrix->create_operation<MeanAndSum>("projection");
        const SampleDatum empty(Samples {});

        switch (GetParam()) {
        case ChainPath::Direct:
            EXPECT_FALSE((matrix->execute_chain<Summarize, MeanAndSum, Samples, Eigen::VectorXd, Samples>(empty)).has_value());
            break;
        case ChainPath::Named:
            EXPECT_FALSE((matrix->execute_chain_named<Summarize, MeanAndSum, Samples, Eigen::VectorXd, Samples>(
                              "summary", "projection", empty)).has_value());
            break;
        case ChainPath::Fluent:
            EXPECT_THROW((matrix->with(empty).then<Summarize, Eigen::VectorXd>("summary")), std::runtime_error);
            break;
        }
        const auto failed_stats = matrix->get_statistics();
        EXPECT_EQ(std::any_cast<size_t>(failed_stats.at("total_executions")), 1U);
        EXPECT_EQ(std::any_cast<size_t>(failed_stats.at("failed_executions")), 1U);

        const auto recovered = matrix->execute_chain_named<Summarize, MeanAndSum, Samples, Eigen::VectorXd, Samples>(
            "summary", "projection", SampleDatum(Samples { 1.0, 3.0, 5.0 }));

        ASSERT_TRUE(recovered.has_value());
        EXPECT_EQ(recovered->data, (Samples { 3.0, 9.0 }));
    }

    class MatrixForkTest : public ::testing::TestWithParam<bool> {
    };

    INSTANTIATE_TEST_SUITE_P(ForkForms, MatrixForkTest, ::testing::Bool(),
        [](const ::testing::TestParamInfo<bool>& info) { return info.param ? "Fluent" : "Parallel"; });

    TEST_P(MatrixForkTest, BranchesReceiveTheOriginalInputRatherThanAnotherBranchesOutput)
    {
        const auto matrix = ComputeMatrix::create();
        const SampleDatum input(Samples { -2.0, 1.0, 3.0 });

        const auto results = GetParam()
            ? matrix->with(input).fork<AffineOperation, RunningOffset>()
            : matrix->execute_parallel<Samples, AffineOperation, RunningOffset>(input);

        ASSERT_TRUE(std::get<0>(results).has_value());
        ASSERT_TRUE(std::get<1>(results).has_value());
        EXPECT_EQ(std::get<0>(results)->data, (Samples { -3.0, 3.0, 7.0 }));
        EXPECT_EQ(std::get<1>(results)->data, (Samples { 0.0, 3.0, 5.0 }));
        EXPECT_EQ(input.data, (Samples { -2.0, 1.0, 3.0 }));
    }

    struct RuntimeScale {
        static constexpr auto describe_facets()
        {
            return std::tuple { FacetMember<&RuntimeScale::factor> {} };
        }

        double factor { 1.0 };
    };

    struct ObservedContext {
        static constexpr auto describe_facets()
        {
            return std::tuple { FacetMember<&ObservedContext::mode> {}, FacetMember<&ObservedContext::timeout> {} };
        }

        ExecutionMode mode;
        std::chrono::milliseconds timeout;
    };

    class ContextualOperation final : public ComputeOperation<Samples> {
    public:
        OperationType get_operation_type() const override { return OperationType::CUSTOM; }

        output_type apply_operation_internal(const input_type& input, const ExecutionContext& context) override
        {
            auto output = input;
            const auto* scale = context.facets.get<RuntimeScale>();
            for (double& sample : output.data) {
                sample *= scale ? scale->factor : 1.0;
            }
            output.facets.set(ObservedContext { context.mode, context.timeout });
            return output;
        }

    protected:
        output_type operation_function(const input_type& input) override { return input; }
    };

    using ContextParam = std::tuple<ExecutionPolicy, bool>;

    class MatrixContextTest : public ::testing::TestWithParam<ContextParam> {
    };

    INSTANTIATE_TEST_SUITE_P(PoliciesAndOverrides, MatrixContextTest,
        ::testing::Combine(::testing::Values(ExecutionPolicy::CONSERVATIVE, ExecutionPolicy::BALANCED, ExecutionPolicy::AGGRESSIVE),
            ::testing::Bool()),
        [](const ::testing::TestParamInfo<ContextParam>& info) {
            return std::string(Reflect::enum_to_string(std::get<0>(info.param)))
                + (std::get<1>(info.param) ? "_Override" : "_Default");
        });

    TEST_P(MatrixContextTest, ConfiguresTheCorrectOperationWithTypedSettingsAndSchedulingAnnotations)
    {
        const auto matrix = ComputeMatrix::create();
        matrix->set_execution_policy(std::get<0>(GetParam()));
        matrix->set_default_timeout(std::chrono::milliseconds(17));
        const bool custom_context = std::get<1>(GetParam());
        matrix->set_context_configurator([custom_context](ExecutionContext& context, const std::type_index& type) {
            context.facets.set(RuntimeScale { type == typeid(ContextualOperation) ? 3.0 : -99.0 });
            if (custom_context) {
                context.mode = ExecutionMode::CHAINED;
                context.timeout = std::chrono::milliseconds(29);
            }
        });

        const auto result = matrix->execute_async<ContextualOperation, Samples>(SampleDatum(Samples { -2.0, 3.0 })).get();

        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->data, (Samples { -6.0, 9.0 }));
        const auto* observed = result->facets.get<ObservedContext>();
        ASSERT_NE(observed, nullptr);
        const auto policy_mode = std::get<0>(GetParam()) == ExecutionPolicy::AGGRESSIVE
            ? ExecutionMode::PARALLEL : ExecutionMode::SYNC;
        EXPECT_EQ(observed->mode, custom_context ? ExecutionMode::CHAINED : policy_mode);
        EXPECT_EQ(observed->timeout, std::chrono::milliseconds(custom_context ? 29 : 17));
    }

    class MatrixConfiguratorFailureTest : public ::testing::TestWithParam<bool> {
    };

    INSTANTIATE_TEST_SUITE_P(ExecutionForms, MatrixConfiguratorFailureTest, ::testing::Bool(),
        [](const ::testing::TestParamInfo<bool>& info) { return info.param ? "Async" : "Sync"; });

    TEST_P(MatrixConfiguratorFailureTest, AConfigurationFailureDoesNotExecuteOrAdvanceTheOperation)
    {
        const auto matrix = ComputeMatrix::create();
        matrix->create_operation<RunningOffset>("state");
        matrix->set_context_configurator([](ExecutionContext&, const std::type_index&) {
            throw std::domain_error("Context configuration failed");
        });
        const SampleDatum input(Samples { 10.0 });

        const auto failed = GetParam()
            ? matrix->execute_named_async<RunningOffset, Samples>("state", input).get()
            : matrix->execute_named<RunningOffset, Samples>("state", input);
        EXPECT_FALSE(failed.has_value());
        matrix->set_context_configurator({});
        const auto recovered = matrix->execute_named<RunningOffset, Samples>("state", input);

        ASSERT_TRUE(recovered.has_value());
        EXPECT_EQ(recovered->data, (Samples { 12.0 }));
        const auto stats = matrix->get_statistics();
        EXPECT_EQ(std::any_cast<size_t>(stats.at("total_executions")), 2U);
        EXPECT_EQ(std::any_cast<size_t>(stats.at("failed_executions")), 1U);
    }

    using AccountingParam = std::tuple<bool, bool>;

    class MatrixAccountingTest : public ::testing::TestWithParam<AccountingParam> {
    };

    INSTANTIATE_TEST_SUITE_P(ExecutionAndProfiling, MatrixAccountingTest,
        ::testing::Combine(::testing::Bool(), ::testing::Bool()),
        [](const ::testing::TestParamInfo<AccountingParam>& info) {
            return std::string(std::get<0>(info.param) ? "Async_" : "Sync_")
                + (std::get<1>(info.param) ? "Profiled" : "Unprofiled");
        });

    TEST_P(MatrixAccountingTest, ReportsOnlyActualExecutionFailuresAndRecoversWithoutInflatingCounts)
    {
        const auto matrix = ComputeMatrix::create();
        matrix->set_profiling(std::get<1>(GetParam()));
        size_t errors = 0;
        std::type_index failed_type { typeid(void) };
        std::string message;
        matrix->set_error_callback([&](const std::exception& error, const std::type_index& type) {
            ++errors;
            failed_type = type;
            message = error.what();
        });
        auto execute = [&](const SampleDatum& input) {
            return std::get<0>(GetParam())
                ? matrix->execute_async<AffineOperation, Samples>(input, 2.0, 1.0, true).get()
                : matrix->execute<AffineOperation, Samples>(input, 2.0, 1.0, true);
        };

        const auto first = execute(SampleDatum(Samples { 1.0, 3.0 }));
        const auto failed = execute(SampleDatum(Samples { -1.0 }));
        const auto missing = matrix->execute_named<AffineOperation, Samples>("missing", SampleDatum(Samples { 1.0 }));
        const auto absent = matrix->execute_with(std::shared_ptr<AffineOperation> {}, SampleDatum(Samples { 1.0 }));
        const auto recovered = execute(SampleDatum(Samples { 4.0 }));

        ASSERT_TRUE(first.has_value());
        EXPECT_EQ(first->data, (Samples { 3.0, 7.0 }));
        EXPECT_FALSE(failed.has_value());
        EXPECT_FALSE(missing.has_value());
        EXPECT_FALSE(absent.has_value());
        ASSERT_TRUE(recovered.has_value());
        EXPECT_EQ(recovered->data, (Samples { 9.0 }));
        EXPECT_EQ(errors, 1U);
        EXPECT_EQ(failed_type, std::type_index(typeid(AffineOperation)));
        EXPECT_EQ(message, "Sample is outside the operation domain");
        EXPECT_EQ(matrix->get_last_error(), message);
        const auto stats = matrix->get_statistics();
        EXPECT_EQ(std::any_cast<size_t>(stats.at("total_executions")), 3U);
        EXPECT_EQ(std::any_cast<size_t>(stats.at("failed_executions")), 1U);
        EXPECT_EQ(stats.contains("average_execution_time_ms"), std::get<1>(GetParam()));
    }

    class MatrixAsyncChainTest : public ::testing::TestWithParam<bool> {
    };

    INSTANTIATE_TEST_SUITE_P(CompletionOutcomes, MatrixAsyncChainTest, ::testing::Bool(),
        [](const ::testing::TestParamInfo<bool>& info) { return info.param ? "Failure" : "Success"; });

    TEST_P(MatrixAsyncChainTest, DrainDeliversCompletedResultsOrReportsTheChainFailure)
    {
        const auto matrix = ComputeMatrix::create();
        matrix->create_operation<AffineOperation>("affine", 2.0, 1.0, GetParam());
        std::optional<SampleDatum> completed;
        const SampleDatum input(GetParam() ? Samples { -1.0 } : Samples { 1.0, 3.0 });

        matrix->with_async(input,
            [](auto chain) { return chain.template then<AffineOperation>("affine").to_io(); },
            [&completed](SampleDatum result) { completed = std::move(result); });
        matrix->drain_async();

        if (GetParam()) {
            EXPECT_FALSE(completed.has_value());
            EXPECT_FALSE(matrix->get_last_error().empty());
        } else {
            ASSERT_TRUE(completed.has_value());
            EXPECT_EQ(completed->data, (Samples { 3.0, 7.0 }));
        }
    }

}
}
