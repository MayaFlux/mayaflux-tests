#include "gtest/gtest.h"

#include "../../support/gpu_dispatch_test_support.hpp"

#include "MayaFlux/Yantra/Executors/ShaderExecutionContext.hpp"
#include "MayaFlux/Yantra/Executors/TensorKernels/Stages.hpp"
#include "MayaFlux/Yantra/Executors/TensorKernels/Weights.hpp"

namespace MayaFlux::Test {
namespace {

    using namespace GpuDispatch;
    using Kakshya::TensorData;
    using Kakshya::TensorSet;
    using Yantra::TensorKernels::check_tensors;
    using Yantra::TensorKernels::pack_weights;
    using Yantra::TensorKernels::TensorRequirement;

    const std::vector<TensorRequirement>& required()
    {
        static const std::vector<TensorRequirement> list {
            { .name = "w", .shape = { 1, 1, 1 } },
            { .name = "b", .shape = { 1 } },
        };
        return list;
    }

    TensorData floats(std::string name, std::vector<uint64_t> shape, float value)
    {
        const size_t count = Yantra::TensorKernels::detail::element_count(shape);
        return TensorData::from_values<float>(std::vector<float>(count, value), std::move(shape), std::move(name)).value();
    }

    TensorSet bias_then_weight()
    {
        TensorSet set;
        set.tensors.push_back(floats("b", { 1 }, 0.5F));
        set.tensors.push_back(floats("w", { 1, 1, 1 }, 2.0F));
        return set;
    }

    TEST(TensorKernelsWeightsTest, CheckTensorsNamesTheFirstProblemInTheOrderRequired)
    {
        TensorSet wrong;
        wrong.tensors.push_back(floats("w", { 2 }, 1.0F));

        EXPECT_EQ(check_tensors(TensorSet {}, required()), "missing tensor 'w'");
        EXPECT_EQ(check_tensors(wrong, required()), "tensor 'w' has shape [2] but [1, 1, 1] is required");
        EXPECT_EQ(check_tensors(bias_then_weight(), required()), "");
    }

    TEST(TensorKernelsWeightsTest, PackedTensorsFollowTheOrderRequiredNotTheOrderOfTheSet)
    {
        const auto packed = pack_weights(bias_then_weight(), required());

        ASSERT_TRUE(packed.has_value());
        EXPECT_EQ(packed->values, (std::vector<float> { 2.0F, 0.5F }));
        EXPECT_EQ(packed->offsets, (std::vector<size_t> { 0, 1 }));
        EXPECT_EQ(packed->address(1000, 0), 1000U);
        EXPECT_EQ(packed->address(1000, 1), 1000U + sizeof(float));
    }

    TEST(TensorKernelsWeightsTest, OtherElementTypesAreConvertedToFloat)
    {
        TensorSet set;
        set.tensors.push_back(TensorData::from_values<int32_t>(std::vector<int32_t> { 3 }, { 1, 1, 1 }, "w").value());
        set.tensors.push_back(floats("b", { 1 }, 0.5F));

        const auto packed = pack_weights(set, required());

        ASSERT_TRUE(packed.has_value());
        EXPECT_EQ(packed->values, (std::vector<float> { 3.0F, 0.5F }));
    }

    TEST(TensorKernelsWeightsTest, AnElementTypeThatCannotBecomeFloatIsNamedWithItsReason)
    {
        TensorSet set;
        set.tensors.push_back(TensorData::from_values<std::complex<float>>(
            std::vector<std::complex<float>> { { 1.0F, 2.0F } }, { 1, 1, 1 }, "w").value());
        set.tensors.push_back(floats("b", { 1 }, 0.5F));

        const auto packed = pack_weights(set, required());

        ASSERT_FALSE(packed.has_value());
        EXPECT_TRUE(packed.error().starts_with("tensor 'w' cannot become float32: ")) << packed.error();
    }

    TEST(TensorKernelsWeightsTest, PackingRefusesWhatCheckingRefuses)
    {
        const auto packed = pack_weights(TensorSet {}, required());

        ASSERT_FALSE(packed.has_value());
        EXPECT_EQ(packed.error(), "missing tensor 'w'");
    }

    template <typename Fixture>
    class TensorKernelsWeightsGpuTest : public Fixture { };

    using HardwareOptions = ::testing::Types<GpuDispatchFixture<true>, GpuDispatchFixture<false>>;
    TYPED_TEST_SUITE(TensorKernelsWeightsGpuTest, HardwareOptions);

    TYPED_TEST(TensorKernelsWeightsGpuTest, TheAddressesOfAnUploadedPackFeedAKernelStage)
    {
        using namespace Yantra;
        namespace TK = Yantra::TensorKernels;

        const auto packed = pack_weights(bias_then_weight(), required());
        ASSERT_TRUE(packed.has_value());

        auto recording = std::make_shared<GpuDispatchRecorder>(true);
        ShaderExecutionContext<> context(GpuComputeConfig {
            .shader_path = TK::conv1d_shader, .workgroup_size = { TK::element_workgroup, 1, 1 }, .push_constant_size = sizeof(TK::Conv1dPC) });
        context.set_recording(recording);
        ASSERT_TRUE(context.ensure_gpu_ready());

        constexpr size_t weights_binding = 0;
        constexpr size_t input_binding = 1;
        constexpr size_t output_binding = 2;
        constexpr auto f32 = GpuBufferBinding::ElementType::FLOAT32;
        TK::upload_packed_weights(context, 0, weights_binding, *packed);
        context.ensure_shared_buffer(0, input_binding, 3, f32);
        context.ensure_shared_buffer(0, output_binding, 3, f32);
        const std::array<float, 3> input { 1.0F, 2.0F, 3.0F };
        context.upload_shared_raw(0, input_binding, reinterpret_cast<const uint8_t*>(input.data()), sizeof(input));

        const uint64_t weights = context.shared_buffer_address(0, weights_binding);
        context.record_stages({ TK::conv1d_stage(TK::Conv1d { .c_in = 1, .c_out = 1, .kernel = 1 },
            context.shared_buffer_address(0, input_binding), packed->address(weights, 0), packed->address(weights, 1),
            context.shared_buffer_address(0, output_binding), 1, 3) });

        std::array<float, 3> output {};
        context.download_shared(0, output_binding, output.data(), sizeof(output));

        EXPECT_NEAR(output[0], 2.5F, 1e-6F);
        EXPECT_NEAR(output[1], 4.5F, 1e-6F);
        EXPECT_NEAR(output[2], 6.5F, 1e-6F);
    }

} // namespace
} // namespace MayaFlux::Test
