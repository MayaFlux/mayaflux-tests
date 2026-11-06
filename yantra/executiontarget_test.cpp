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
        EXPECT_TRUE(contains(both.devices(), Device::CPU | Device::GPU));
        EXPECT_EQ(both.runtimes(), Runtime::ONNX);
        EXPECT_EQ(target_of_v<OnnxOp>, both);
    }

    TEST(ExecutionTargetTest, MeetsNeedsBothAxes)
    {
        EXPECT_TRUE(targets::cpu.meets(targets::cpu | targets::vulkan));
        EXPECT_FALSE(targets::cpu.meets(targets::vulkan));
        EXPECT_FALSE(targets::cpu.meets(targets::onnx_cpu));
        EXPECT_TRUE(target_of_v<OnnxOp>.meets(targets::onnx_gpu));
    }

    TEST(ExecutionTargetTest, EmptyTargetMeetsNothing)
    {
        constexpr Target none {};
        EXPECT_TRUE(none.empty());
        EXPECT_FALSE(none.meets(targets::cpu));
        EXPECT_FALSE(targets::cpu.meets(none));
        EXPECT_TRUE((Target { Device::CPU, Runtime::NONE }).empty());
        EXPECT_TRUE((Target { Device::NONE, Runtime::NATIVE }).empty());
    }

    TEST(ExecutionTargetTest, JoinIsAUnionOfPairsNotOfAxes)
    {
        constexpr Target cpu_or_vulkan = targets::cpu | targets::vulkan;
        constexpr Target gpu_native { Device::GPU, Runtime::NATIVE };

        EXPECT_TRUE(cpu_or_vulkan.has(Device::CPU, Runtime::NATIVE));
        EXPECT_TRUE(cpu_or_vulkan.has(Device::GPU, Runtime::VULKAN));
        EXPECT_FALSE(cpu_or_vulkan.has(Device::GPU, Runtime::NATIVE));
        EXPECT_FALSE(cpu_or_vulkan.has(Device::CPU, Runtime::VULKAN));
        EXPECT_FALSE(gpu_native.meets(cpu_or_vulkan));
        EXPECT_FALSE(cpu_or_vulkan.meets(gpu_native));
    }

    TEST(ExecutionTargetTest, AxisSetsGiveEveryCombination)
    {
        constexpr Target product { Device::CPU | Device::GPU, Runtime::ONNX | Runtime::NATIVE };

        EXPECT_TRUE(product.has(Device::CPU, Runtime::ONNX));
        EXPECT_TRUE(product.has(Device::CPU, Runtime::NATIVE));
        EXPECT_TRUE(product.has(Device::GPU, Runtime::ONNX));
        EXPECT_TRUE(product.has(Device::GPU, Runtime::NATIVE));
        EXPECT_FALSE(product.has(Device::GPU, Runtime::VULKAN));
        EXPECT_EQ(product.devices(), Device::CPU | Device::GPU);
        EXPECT_EQ(product.runtimes(), Runtime::ONNX | Runtime::NATIVE);
    }

    TEST(ExecutionTargetTest, CustomBitsAtTheTopOfEachAxisSurvive)
    {
        constexpr Target custom { Device::CUSTOM_1, Runtime::CUSTOM_1 };

        EXPECT_FALSE(custom.empty());
        EXPECT_TRUE(custom.has(Device::CUSTOM_1, Runtime::CUSTOM_1));
        EXPECT_EQ(custom.devices(), Device::CUSTOM_1);
        EXPECT_EQ(custom.runtimes(), Runtime::CUSTOM_1);
        EXPECT_FALSE(custom.meets(targets::cpu | targets::vulkan));
    }

} // namespace
} // namespace MayaFlux::Test
