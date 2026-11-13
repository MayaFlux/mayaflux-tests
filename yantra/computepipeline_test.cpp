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

    enum class PipelinePath { CreatedStages, SuppliedStages, ExplicitSignature, AssembledGrammar };
    enum class PipelineMethod { Summarize, Unknown };
    enum class EmptyInput { NoChannels, EmptyChannels, MixedChannels };
    enum class EmptyPipeline { Direct, AudioFactory, AnalysisFactory };

    const Facet<bool> reject_stage {};
    const Facet<int> source_id {};

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

    std::shared_ptr<OperationRegistry> registry()
    {
        auto result = std::make_shared<OperationRegistry>();
        result->catalog<MathematicalOperation, MathematicalTransformer<>>();
        result->catalog<TemporalOperation, TemporalTransformer<>>();
        return result;
    }

    ComputationSignature affine_signature()
    {
        return MathematicalOperation::OFFSET | MathematicalOperation::GAIN | TemporalOperation::TIME_REVERSE;
    }

    std::shared_ptr<ComputationPipeline<>> pipeline_for(PipelinePath path)
    {
        auto pipeline = std::make_shared<ComputationPipeline<>>();
        switch (path) {
        case PipelinePath::CreatedStages:
            pipeline->create_operation<MathematicalTransformer<>>("offset",
                MathematicalTransformerConfig { .operation = MathematicalOperation::OFFSET, .offset_value = 3.0 })
                .create_operation<MathematicalTransformer<>>("gain",
                    MathematicalTransformerConfig { .gain_factor = 2.0 })
                .create_operation<TemporalTransformer<>>("reverse");
            break;
        case PipelinePath::SuppliedStages:
            pipeline->add_operation(std::make_shared<MathematicalTransformer<>>(
                                        MathematicalTransformerConfig { .operation = MathematicalOperation::OFFSET, .offset_value = 3.0 }),
                        "offset")
                .add_operation(std::make_shared<MathematicalTransformer<>>(
                                   MathematicalTransformerConfig { .gain_factor = 2.0 }),
                    "gain")
                .add_operation(std::make_shared<TemporalTransformer<>>(), "reverse");
            break;
        case PipelinePath::ExplicitSignature: {
            auto grammar = std::make_shared<ComputationGrammar>();
            grammar->add_operation_rule<MathematicalTransformer<>>("offset", MathematicalOperation::OFFSET,
                MathematicalTransformerConfig { .offset_value = 3.0 });
            grammar->add_operation_rule<MathematicalTransformer<>>("gain", MathematicalOperation::GAIN,
                MathematicalTransformerConfig { .gain_factor = 2.0 });
            grammar->add_operation_rule<TemporalTransformer<>>("reverse", TemporalOperation::TIME_REVERSE);
            pipeline->set_grammar(grammar);
            break;
        }
        case PipelinePath::AssembledGrammar:
            pipeline->set_grammar(ComputationGrammar::assemble(registry(), affine_signature(),
                {
                    MathematicalTransformerConfig { .offset_value = 3.0 },
                    MathematicalTransformerConfig { .gain_factor = 2.0 },
                    TemporalTransformerConfig {},
                }));
            break;
        }
        return pipeline;
    }

    struct ChannelCase {
        const char* name;
        Channels input;
        Channels expected;
    };

    const ChannelCase channel_cases[] {
        { "Mono", { { -3.0, 1.0, 4.0, 0.5 } }, { { 7.0, 14.0, 8.0, 0.0 } } },
        { "UnequalChannels", { { -2.0, 0.0, 3.0 }, { 5.0, -1.0 } }, { { 12.0, 6.0, 2.0 }, { 4.0, 16.0 } } },
        { "SingleSamples", { { 2.0 }, { -3.0 } }, { { 10.0 }, { 0.0 } } },
        { "ManyChannels",
            { { -4.0, -3.0, -2.0 }, { -1.0, 0.0, 1.0 }, { 2.0, 3.0, 4.0 }, { 5.0, 6.0, 7.0 },
                { -8.0, -7.0, -6.0 }, { -5.0, -4.0, -3.0 }, { 8.0, 9.0, 10.0 }, { -10.0, -9.0, -8.0 } },
            { { 2.0, 0.0, -2.0 }, { 8.0, 6.0, 4.0 }, { 14.0, 12.0, 10.0 }, { 20.0, 18.0, 16.0 },
                { -6.0, -8.0, -10.0 }, { 0.0, -2.0, -4.0 }, { 26.0, 24.0, 22.0 }, { -10.0, -12.0, -14.0 } } },
    };

    using ExecutionParam = std::tuple<PipelinePath, ChannelCase>;

    class PipelineExecutionTest : public ::testing::TestWithParam<ExecutionParam> {
    protected:
        DataIO process(ComputationPipeline<>& pipeline, const DataIO& input) const
        {
            return std::get<0>(GetParam()) == PipelinePath::ExplicitSignature
                ? pipeline.process(input, affine_signature())
                : pipeline.process(input);
        }
    };

    INSTANTIATE_TEST_SUITE_P(PathsAndChannels, PipelineExecutionTest,
        ::testing::Combine(
            ::testing::Values(PipelinePath::CreatedStages, PipelinePath::SuppliedStages,
                PipelinePath::ExplicitSignature, PipelinePath::AssembledGrammar),
            ::testing::ValuesIn(channel_cases)),
        [](const ::testing::TestParamInfo<ExecutionParam>& info) {
            return std::string(Reflect::enum_to_string(std::get<0>(info.param))) + "_" + std::get<1>(info.param).name;
        });

    TEST_P(PipelineExecutionTest, ExecutesEveryStageOnceInDeclaredOrder)
    {
        const auto pipeline = pipeline_for(std::get<0>(GetParam()));
        const auto& data = std::get<1>(GetParam());

        expect_channels(process(*pipeline, to_datum(data.input)), data.expected);
    }

    TEST_P(PipelineExecutionTest, BufferedExecutionDoesNotMutateTheCallersInput)
    {
        const auto pipeline = pipeline_for(std::get<0>(GetParam()));
        const auto& data = std::get<1>(GetParam());
        const auto input = to_datum(data.input);

        expect_channels(process(*pipeline, input), data.expected);
        expect_channels(input, data.input);
    }

    TEST_P(PipelineExecutionTest, ReusesThePipelineAcrossChangingChannelCountsAndLengths)
    {
        const auto pipeline = pipeline_for(std::get<0>(GetParam()));
        const auto& data = std::get<1>(GetParam());

        expect_channels(process(*pipeline, to_datum(data.input)), data.expected);
        expect_channels(process(*pipeline, to_datum({ { 10.0, -10.0 }, { 0.0 }, { -3.0, 2.0, 7.0 } })),
            { { -14.0, 26.0 }, { 6.0 }, { 20.0, 10.0, 0.0 } });
        expect_channels(process(*pipeline, to_datum(data.input)), data.expected);
    }

    using EmptyParam = std::tuple<EmptyPipeline, EmptyInput>;

    class PipelineIdentityTest : public ::testing::TestWithParam<EmptyParam> {
    };

    INSTANTIATE_TEST_SUITE_P(FactoriesAndEmptyChannels, PipelineIdentityTest,
        ::testing::Combine(
            ::testing::Values(EmptyPipeline::Direct, EmptyPipeline::AudioFactory, EmptyPipeline::AnalysisFactory),
            ::testing::Values(EmptyInput::NoChannels, EmptyInput::EmptyChannels, EmptyInput::MixedChannels)),
        [](const ::testing::TestParamInfo<EmptyParam>& info) {
            return std::string(Reflect::enum_to_string(std::get<0>(info.param))) + "_"
                + std::string(Reflect::enum_to_string(std::get<1>(info.param)));
        });

    TEST_P(PipelineIdentityTest, AStageFreePipelinePreservesDataAndItsAttachedFacts)
    {
        std::shared_ptr<ComputationPipeline<>> pipeline;
        switch (std::get<0>(GetParam())) {
        case EmptyPipeline::Direct:
            pipeline = std::make_shared<ComputationPipeline<>>();
            break;
        case EmptyPipeline::AudioFactory:
            pipeline = PipelineFactory::create_audio_pipeline<>();
            break;
        case EmptyPipeline::AnalysisFactory:
            pipeline = PipelineFactory::create_analysis_pipeline<>();
            break;
        }
        Channels channels;
        switch (std::get<1>(GetParam())) {
        case EmptyInput::NoChannels:
            break;
        case EmptyInput::EmptyChannels:
            channels = { {}, {} };
            break;
        case EmptyInput::MixedChannels:
            channels = { {}, { -2.0, 3.0 }, {} };
            break;
        }
        auto input = to_datum(channels);
        input.facets.set(source_id, 73);
        input.metadata["source"] = std::string("authored");

        const auto result = pipeline->process(input, {});

        expect_channels(result, channels);
        const auto* carried = result.facets.get(source_id);
        ASSERT_NE(carried, nullptr);
        EXPECT_EQ(*carried, 73);
        const auto found = result.metadata.find("source");
        ASSERT_NE(found, result.metadata.end());
        EXPECT_EQ(std::any_cast<std::string>(found->second), "authored");
        EXPECT_EQ(result.dimensions.size(), input.dimensions.size());
        EXPECT_EQ(result.modality, input.modality);
    }

    std::shared_ptr<FunctionalOperation<Samples>> counter(double delta)
    {
        return std::make_shared<FunctionalOperation<Samples>>(
            [delta, calls = size_t {}](const SampleDatum& input) mutable {
                auto output = input;
                const double offset = delta * static_cast<double>(++calls);
                for (double& sample : output.data) {
                    sample += offset;
                }
                return output;
            });
    }

    class PipelineOwnershipTest : public ::testing::TestWithParam<bool> {
    };

    INSTANTIATE_TEST_SUITE_P(InstanceOwnership, PipelineOwnershipTest, ::testing::Bool(),
        [](const ::testing::TestParamInfo<bool>& info) { return info.param ? "SharedInstance" : "IndependentInstances"; });

    TEST_P(PipelineOwnershipTest, StateFollowsTheSuppliedOperationInstance)
    {
        ComputationPipeline<Samples> first;
        ComputationPipeline<Samples> second;
        auto instance = counter(2.0);
        first.add_operation(instance, "counter");
        second.add_operation(GetParam() ? instance : counter(2.0), "counter");
        instance.reset();
        const SampleDatum input(Samples { 1.0, 3.0 });

        EXPECT_EQ(first.process(input).data, (Samples { 3.0, 5.0 }));
        EXPECT_EQ(second.process(input).data, (GetParam() ? Samples { 5.0, 7.0 } : Samples { 3.0, 5.0 }));
        EXPECT_EQ(first.process(input).data, (GetParam() ? Samples { 7.0, 9.0 } : Samples { 5.0, 7.0 }));
    }

    TEST_P(PipelineOwnershipTest, ClearingOnePipelineDoesNotDestroyAnotherPipelinesOperation)
    {
        ComputationPipeline<Samples> first;
        ComputationPipeline<Samples> second;
        auto instance = counter(2.0);
        first.add_operation(instance, "counter");
        second.add_operation(GetParam() ? instance : counter(2.0), "counter");
        const SampleDatum input(Samples { 1.0 });

        EXPECT_EQ(first.process(input).data, (Samples { 3.0 }));
        first.clear_operations();
        instance.reset();

        EXPECT_EQ(first.process(input).data, input.data);
        EXPECT_EQ(second.process(input).data, (GetParam() ? Samples { 5.0 } : Samples { 3.0 }));
    }

    enum class Lookup { LiveHandle, MissingName, WrongType };

    class PipelineLookupTest : public ::testing::TestWithParam<Lookup> {
    };

    INSTANTIATE_TEST_SUITE_P(LiveLookup, PipelineLookupTest,
        ::testing::Values(Lookup::LiveHandle, Lookup::MissingName, Lookup::WrongType),
        [](const ::testing::TestParamInfo<Lookup>& info) { return std::string(Reflect::enum_to_string(info.param)); });

    TEST_P(PipelineLookupTest, OnlyAValidTypedHandleCanChangeTheLiveStage)
    {
        ComputationPipeline<> pipeline;
        pipeline.create_operation<MathematicalTransformer<>>("scale",
            MathematicalTransformerConfig { .gain_factor = 2.0 });
        const auto input = to_datum({ { -2.0, 1.0, 3.0 } });

        switch (GetParam()) {
        case Lookup::LiveHandle: {
            auto stage = pipeline.get_operation<MathematicalTransformer<>>("scale");
            ASSERT_NE(stage, nullptr);
            stage->set_config(MathematicalTransformerConfig { .gain_factor = 4.0 });
            expect_channels(pipeline.process(input), { { -8.0, 4.0, 12.0 } });
            break;
        }
        case Lookup::MissingName:
            EXPECT_EQ(pipeline.get_operation<MathematicalTransformer<>>("missing"), nullptr);
            EXPECT_FALSE(pipeline.configure_operation<MathematicalTransformer<>>("missing", [](auto operation) {
                operation->set_config(MathematicalTransformerConfig { .gain_factor = 4.0 });
            }));
            EXPECT_FALSE(pipeline.remove_operation("missing"));
            expect_channels(pipeline.process(input), { { -4.0, 2.0, 6.0 } });
            break;
        case Lookup::WrongType:
            EXPECT_EQ(pipeline.get_operation<TemporalTransformer<>>("scale"), nullptr);
            EXPECT_FALSE(pipeline.configure_operation<TemporalTransformer<>>("scale", [](auto operation) {
                operation->set_config(TemporalTransformerConfig {});
            }));
            expect_channels(pipeline.process(input), { { -4.0, 2.0, 6.0 } });
            break;
        }
    }

    enum class FailureSite { First, Middle, Last };

    class PipelineFailureTest : public ::testing::TestWithParam<FailureSite> {
    };

    INSTANTIATE_TEST_SUITE_P(FailurePositions, PipelineFailureTest,
        ::testing::Values(FailureSite::First, FailureSite::Middle, FailureSite::Last),
        [](const ::testing::TestParamInfo<FailureSite>& info) { return std::string(Reflect::enum_to_string(info.param)); });

    TEST_P(PipelineFailureTest, PropagatesTheFailureStopsDownstreamWorkAndRemainsReusable)
    {
        ComputationPipeline<Samples> pipeline;
        auto failure = std::make_shared<FunctionalOperation<Samples>>([](const SampleDatum& input) {
            const auto* reject = input.facets.get(reject_stage);
            if (reject && *reject) {
                throw std::domain_error("Rejected pipeline input");
            }
            return input;
        });
        if (GetParam() == FailureSite::First) {
            pipeline.add_operation(failure, "gate");
        }
        pipeline.add_operation(counter(2.0), "first");
        if (GetParam() == FailureSite::Middle) {
            pipeline.add_operation(failure, "gate");
        }
        pipeline.add_operation(counter(5.0), "second");
        if (GetParam() == FailureSite::Last) {
            pipeline.add_operation(failure, "gate");
        }
        SampleDatum rejected(Samples { 1.0, 3.0 });
        rejected.facets.set(reject_stage, true);
        const SampleDatum accepted(Samples { 1.0, 3.0 });

        EXPECT_THROW(pipeline.process(rejected), std::domain_error);

        const auto result = pipeline.process(accepted);
        switch (GetParam()) {
        case FailureSite::First:
            EXPECT_EQ(result.data, (Samples { 8.0, 10.0 }));
            break;
        case FailureSite::Middle:
            EXPECT_EQ(result.data, (Samples { 10.0, 12.0 }));
            break;
        case FailureSite::Last:
            EXPECT_EQ(result.data, (Samples { 15.0, 17.0 }));
            break;
        }
        EXPECT_EQ(rejected.data, (Samples { 1.0, 3.0 }));
    }

    struct Admission {
        bool enabled {};
    };

    bool admitted(const std::any&, const ExecutionContext& context)
    {
        const auto* admission = context.facets.get<Admission>();
        return admission && admission->enabled;
    }

    class PipelineSignatureTest : public ::testing::TestWithParam<bool> {
    };

    INSTANTIATE_TEST_SUITE_P(SignatureForms, PipelineSignatureTest, ::testing::Bool(),
        [](const ::testing::TestParamInfo<bool>& info) { return info.param ? "ChainedSignature" : "SingleEnum"; });

    TEST_P(PipelineSignatureTest, AnExplicitSignatureSelectsItsOwnStagesInsteadOfTheAssembledDefault)
    {
        const auto grammar = ComputationGrammar::Builder(registry())
            .rule(MathematicalOperation::GAIN, MathematicalTransformerConfig { .gain_factor = 2.0 })
            .build();
        grammar->add_operation_rule<MathematicalTransformer<>>("offset", MathematicalOperation::OFFSET,
            MathematicalTransformerConfig { .offset_value = 3.0 });
        grammar->add_operation_rule<MathematicalTransformer<>>("gain", MathematicalOperation::GAIN,
            MathematicalTransformerConfig { .gain_factor = 2.0 });
        ComputationPipeline<> pipeline(grammar);
        const auto input = to_datum({ { -2.0, 1.0, 3.0 } });

        expect_channels(pipeline.process(input), { { -4.0, 2.0, 6.0 } });
        if (GetParam()) {
            expect_channels(pipeline.process(input, MathematicalOperation::OFFSET | MathematicalOperation::GAIN),
                { { 2.0, 8.0, 12.0 } });
        } else {
            expect_channels(pipeline.process(input, MathematicalOperation::OFFSET), { { 1.0, 4.0, 6.0 } });
        }
        expect_channels(pipeline.process(input), { { -4.0, 2.0, 6.0 } });
    }

    class PipelineContextTest : public ::testing::TestWithParam<bool> {
    };

    INSTANTIATE_TEST_SUITE_P(ContextRoutes, PipelineContextTest, ::testing::Bool(),
        [](const ::testing::TestParamInfo<bool>& info) { return info.param ? "ExplicitSignature" : "AssembledSignature"; });

    TEST_P(PipelineContextTest, ForwardsTypedContextToEveryRequestedStage)
    {
        const auto grammar = ComputationGrammar::Builder(registry())
            .rule(MathematicalOperation::GAIN, MathematicalTransformerConfig { .gain_factor = 2.0 })
            .when(admitted)
            .rule(TemporalOperation::TIME_REVERSE)
            .when(admitted)
            .build();
        ComputationPipeline<> pipeline(grammar);
        const auto input = to_datum({ { -2.0, 1.0, 3.0 } });
        ExecutionContext rejected;
        ExecutionContext accepted;
        accepted.facets.set(Admission { true });

        if (GetParam()) {
            const auto signature = MathematicalOperation::GAIN | TemporalOperation::TIME_REVERSE;
            EXPECT_THROW(pipeline.process(input, signature, rejected), std::runtime_error);
            expect_channels(pipeline.process(input, signature, accepted), { { 6.0, 2.0, -4.0 } });
        } else {
            EXPECT_THROW(pipeline.process(input, rejected), std::runtime_error);
            expect_channels(pipeline.process(input, accepted), { { 6.0, 2.0, -4.0 } });
        }
    }

    TEST_P(PipelineSignatureTest, RejectsUnresolvableSignaturesInsteadOfReturningUnprocessedInput)
    {
        const auto grammar = ComputationGrammar::Builder(registry())
            .rule(MathematicalOperation::GAIN, MathematicalTransformerConfig { .gain_factor = 2.0 })
            .build();
        ComputationPipeline<> pipeline(grammar);
        const auto input = to_datum({ { 1.0 } });

        if (GetParam()) {
            EXPECT_THROW(pipeline.process(input, MathematicalOperation::GAIN | PipelineMethod::Unknown), std::runtime_error);
        } else {
            EXPECT_THROW(pipeline.process(input, PipelineMethod::Unknown), std::runtime_error);
        }
        expect_channels(pipeline.process(input), { { 2.0 } });
    }

    enum class FinalType { DeclaredOutput, IncompatibleOutput };

    class PipelineOutputTest : public ::testing::TestWithParam<FinalType> {
    };

    INSTANTIATE_TEST_SUITE_P(FinalContracts, PipelineOutputTest,
        ::testing::Values(FinalType::DeclaredOutput, FinalType::IncompatibleOutput),
        [](const ::testing::TestParamInfo<FinalType>& info) { return std::string(Reflect::enum_to_string(info.param)); });

    TEST_P(PipelineOutputTest, ExplicitSignaturesHonorThePipelinesDeclaredOutputType)
    {
        auto summary = std::make_shared<FunctionalOperation<Samples, Eigen::VectorXd>>([](const SampleDatum& input) {
            Eigen::VectorXd output(2);
            output[0] = std::accumulate(input.data.begin(), input.data.end(), 0.0);
            output[1] = static_cast<double>(input.data.size());
            return Datum<Eigen::VectorXd>(std::move(output));
        });
        const auto grammar = ComputationGrammar::Builder(std::make_shared<OperationRegistry>())
            .rule(PipelineMethod::Summarize).uses(summary)
            .build();
        const SampleDatum input(Samples { 1.0, 3.0, 5.0 });

        if (GetParam() == FinalType::DeclaredOutput) {
            ComputationPipeline<Samples, Eigen::VectorXd> pipeline(grammar);
            const auto result = pipeline.process(input, PipelineMethod::Summarize);
            ASSERT_EQ(result.data.size(), 2);
            EXPECT_DOUBLE_EQ(result.data[0], 9.0);
            EXPECT_DOUBLE_EQ(result.data[1], 3.0);
        } else {
            ComputationPipeline<Samples> pipeline(grammar);
            EXPECT_THROW(pipeline.process(input, PipelineMethod::Summarize), std::runtime_error);
        }
    }

}
}
