#include "MayaFlux/Yantra/OperationSpec/ExecutionTarget.hpp"

#include <gtest/gtest.h>

namespace MayaFlux::Test {
namespace {

    using namespace Yantra;

    struct DefaultOp { };

    struct GpuOp {
        static constexpr Target target = targets::vulkan;
    };

    struct OnnxOp {
        static constexpr Target target = targets::onnx_cpu | targets::onnx_gpu;
    };

    TEST(ExecutionTargetTest, UndeclaredOperationRunsOnTheCpu)
    {
        static_assert(target_of_v<DefaultOp> == targets::cpu);
        SUCCEED();
    }

    TEST(ExecutionTargetTest, DeclaredTargetIsReadBack)
    {
        static_assert(target_of_v<GpuOp> == targets::vulkan);
        EXPECT_TRUE(target_of_v<GpuOp>.meets(targets::vulkan));
    }

    TEST(ExecutionTargetTest, JoinComposesEachAxis)
    {
        constexpr Target both = targets::onnx_cpu | targets::onnx_gpu;
        EXPECT_TRUE(contains(both.devices, Device::CPU | Device::GPU));
        EXPECT_EQ(both.runtimes, Runtime::ONNX);
        EXPECT_EQ(target_of_v<OnnxOp>, both);
    }

    TEST(ExecutionTargetTest, MeetsNeedsBothAxes)
    {
        EXPECT_TRUE(targets::cpu.meets(targets::cpu | targets::vulkan));
        EXPECT_FALSE(targets::cpu.meets(targets::vulkan));
        EXPECT_FALSE(targets::cpu.meets(targets::onnx_cpu));
        EXPECT_TRUE(target_of_v<OnnxOp>.meets(targets::onnx_gpu));
    }

    TEST(ExecutionTargetTest, EmptyAxisMeetsNothing)
    {
        constexpr Target none {};
        EXPECT_TRUE(none.empty());
        EXPECT_FALSE(none.meets(targets::cpu));
        EXPECT_FALSE(targets::cpu.meets(none));
    }

} // namespace
} // namespace MayaFlux::Test
