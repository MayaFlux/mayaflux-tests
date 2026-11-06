#include "MayaFlux/Yantra/ComputeGrammar.hpp"

#include <gtest/gtest.h>

namespace MayaFlux::Test {
namespace {

    using namespace Yantra;
    using Samples = std::vector<double>;
    using SampleDatum = Datum<Samples>;

    enum class Stage : uint8_t { Add };

    struct AddConfig : OperationConfig {
        static constexpr auto describe_facets() { return std::tuple<> {}; }
    };

    template <int Offset>
    class AddOp : public ComputeOperation<Samples> {
    public:
        using supports = Supports<AddConfig>;

        [[nodiscard]] OperationType get_operation_type() const override { return OperationType::CUSTOM; }

    protected:
        output_type operation_function(const input_type& input) override
        {
            auto output = input;
            for (double& sample : output.data) {
                sample += Offset;
            }
            return output;
        }
    };

    class CpuAdd final : public AddOp<1> { };

    /// @brief Takes over from the CPU class only for inputs of four samples or more.
    class VulkanAdd final : public AddOp<100> {
    public:
        static constexpr Target target = targets::vulkan;

        static float fitness(const input_type& input)
        {
            return input.data.size() >= 4 ? 2.0F : 0.0F;
        }
    };

    OperationRegistry registry()
    {
        OperationRegistry result;
        result.catalog<CpuAdd>(Stage::Add);
        result.catalog<VulkanAdd>(Stage::Add);
        return result;
    }

    std::shared_ptr<ComputationGrammar> grammar()
    {
        return ComputationGrammar::Builder(std::make_shared<const OperationRegistry>(registry()))
            .rule(Stage::Add)
            .build();
    }

    ExecutionContext permitting(Target allowed)
    {
        ExecutionContext context;
        context.facets.set(Permit { allowed });
        return context;
    }

    const SampleDatum small_input(Samples { 0.0, 0.0 });
    const SampleDatum large_input(Samples { 0.0, 0.0, 0.0, 0.0 });

    TEST(GrammarTargetTest, FitnessPicksTheCandidateThatSuitsTheInput)
    {
        const auto built = grammar();
        EXPECT_EQ(built->process(large_input).data.front(), 100.0);
        EXPECT_EQ(built->process(small_input).data.front(), 1.0);
    }

    TEST(GrammarTargetTest, PermitExcludesCandidatesOffTheAllowedTarget)
    {
        const auto built = grammar();
        EXPECT_EQ(built->process(large_input, permitting(targets::cpu)).data.front(), 1.0);
        EXPECT_EQ(built->process(large_input, permitting(targets::cpu | targets::vulkan)).data.front(), 100.0);
    }

    TEST(GrammarTargetTest, NoCandidateLeftIsAnErrorNotAFallback)
    {
        const auto built = grammar();
        EXPECT_THROW(built->process(small_input, permitting(targets::vulkan)), std::runtime_error);
        EXPECT_THROW(built->process(large_input, permitting(targets::onnx_cpu)), std::runtime_error);
    }

    TEST(GrammarTargetTest, PinRestrictsAStageWhenTheGrammarIsBuilt)
    {
        const auto registered = std::make_shared<const OperationRegistry>(registry());

        const auto cpu_only = ComputationGrammar::Builder(registered).rule(Stage::Add).on(targets::cpu).build();
        EXPECT_EQ(cpu_only->process(large_input).data.front(), 1.0);

        const auto vulkan_only = ComputationGrammar::Builder(registered).rule(Stage::Add).on(targets::vulkan).build();
        EXPECT_EQ(vulkan_only->process(large_input).data.front(), 100.0);
        EXPECT_THROW(vulkan_only->process(small_input), std::runtime_error);
    }

    TEST(GrammarTargetTest, PinNoCandidateMeetsFailsAtBuild)
    {
        const auto registered = std::make_shared<const OperationRegistry>(registry());
        EXPECT_THROW(ComputationGrammar::Builder(registered).rule(Stage::Add).on(targets::onnx_gpu), std::invalid_argument);
    }

    TEST(GrammarTargetTest, SuppliedInstanceMustMeetThePin)
    {
        EXPECT_THROW(ComputationGrammar::Builder()
                         .rule(Stage::Add)
                         .on(targets::cpu)
                         .uses(std::make_shared<VulkanAdd>()),
            std::invalid_argument);

        const auto built = ComputationGrammar::Builder()
                               .rule(Stage::Add)
                               .on(targets::vulkan)
                               .uses(std::make_shared<VulkanAdd>())
                               .build();
        EXPECT_EQ(built->process(large_input).data.front(), 100.0);
    }

} // namespace
} // namespace MayaFlux::Test
