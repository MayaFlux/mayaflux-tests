#include "gtest/gtest.h"

#include <filesystem>
#include <fstream>
#include <random>

#include "../../support/gpu_dispatch_test_support.hpp"

#include "MayaFlux/Yantra/Analyzers/WindowedKernelAnalyzer.hpp"

namespace MayaFlux::Test {
namespace {

    using namespace GpuDispatch;
    using Yantra::StandardWindowedKernelAnalyzer;
    using Yantra::WindowedKernelConfig;
    using Yantra::WindowedKernelContext;

    constexpr const char* k_mean_step = R"(
    float acc = 0.0;
    for (uint k = 0u; k + 1u < window_length; ++k) {
        acc += abs(sample_at(i, k) - sample_at(i, k + 1u));
    }
    results[result_base] = acc / float(window_length - 1u);
)";

    constexpr const char* k_extremes = R"(
    float lo = sample_at(i, 0u);
    float hi = lo;
    for (uint k = 1u; k < window_length; ++k) {
        float v = sample_at(i, k);
        lo = min(lo, v);
        hi = max(hi, v);
    }
    results[result_base] = lo * parameters[0];
    results[result_base + 1u] = hi * parameters[0];
)";

    constexpr const char* k_window_sum = R"(
    float acc = 0.0;
    for (uint k = 0u; k < window_length; ++k) {
        acc += sample_at(i, k);
    }
    results[result_base] = acc;
)";

    constexpr const char* k_comp_file = R"(#version 460
layout(local_size_x = 64) in;
layout(set = 0, binding = 0, std430) buffer Samples { float samples[]; };
layout(set = 0, binding = 1, std430) buffer Parameters { float parameters[]; };
layout(set = 0, binding = 2, std430) buffer Results { float results[]; };
layout(push_constant) uniform PC {
    uint window_length;
    uint stride;
    uint offset;
    uint window_count;
    uint outputs;
} pc;
void main()
{
    uint i = gl_GlobalInvocationID.x;
    if (i >= pc.window_count) {
        return;
    }
    uint base = pc.offset + i * pc.stride;
    float acc = 0.0;
    for (uint k = 0u; k < pc.window_length; ++k) {
        float v = samples[base + k];
        acc += v * v;
    }
    results[i * pc.outputs] = acc * parameters[0];
}
)";

    WindowedKernelConfig make_config(const char* kernel, uint32_t outputs, uint32_t window, uint32_t hop)
    {
        WindowedKernelConfig config;
        config.kernel = kernel;
        config.outputs = outputs;
        config.window_size = window;
        config.hop_size = hop;
        return config;
    }

    std::vector<Kakshya::DataVariant> input_of(const std::vector<std::vector<double>>& channels)
    {
        std::vector<Kakshya::DataVariant> out;
        out.reserve(channels.size());
        for (const auto& channel : channels) {
            out.emplace_back(channel);
        }
        return out;
    }

    std::vector<double> random_signal(size_t count, uint64_t seed)
    {
        std::mt19937_64 engine(seed);
        std::uniform_real_distribution<double> distribution(-1.0, 1.0);
        std::vector<double> out(count);
        for (auto& value : out) {
            value = distribution(engine);
        }
        return out;
    }

    void expect_relative(double got, double want, double tolerance)
    {
        EXPECT_NEAR(got, want, tolerance * (1.0 + std::abs(want)));
    }

    template <typename Fixture>
    class WindowedKernelGpuTest : public Fixture { };

    using HardwareOptions = ::testing::Types<GpuDispatchFixture<true>, GpuDispatchFixture<false>>;
    TYPED_TEST_SUITE(WindowedKernelGpuTest, HardwareOptions);

    TYPED_TEST(WindowedKernelGpuTest, AKernelBodyMatchesAHostComputationPerWindow)
    {
        const auto signal = random_signal(2000, 3);
        StandardWindowedKernelAnalyzer analyzer(make_config(k_mean_step, 1, 64, 32));

        const auto analysis = analyzer.analyze_windows(input_of({ signal }));

        constexpr size_t windows = (2000 - 64) / 32 + 1;
        ASSERT_EQ(analysis.channels.size(), 1U);
        ASSERT_EQ(analysis.channels[0].values.size(), windows);
        for (size_t w = 0; w < windows; ++w) {
            double sum = 0.0;
            for (size_t k = 0; k + 1 < 64; ++k) {
                const double a = static_cast<float>(signal[w * 32 + k]);
                const double b = static_cast<float>(signal[w * 32 + k + 1]);
                sum += std::abs(a - b);
            }
            expect_relative(analysis.channels[0].values[w], sum / 63.0, 1.0e-4);
        }
    }

    TYPED_TEST(WindowedKernelGpuTest, SeveralOutputsAndAParameterReachTheKernel)
    {
        const auto signal = random_signal(1000, 5);
        auto config = make_config(k_extremes, 2, 50, 50);
        config.parameters = { 2.0F };
        StandardWindowedKernelAnalyzer analyzer(config);

        const auto channel = analyzer.analyze_windows(input_of({ signal })).channels[0];

        ASSERT_EQ(channel.outputs, 2U);
        ASSERT_EQ(channel.values.size(), 40U);
        for (size_t w = 0; w < 20; ++w) {
            float lo = static_cast<float>(signal[w * 50]);
            float hi = lo;
            for (size_t k = 1; k < 50; ++k) {
                const float v = static_cast<float>(signal[w * 50 + k]);
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
            EXPECT_EQ(channel.values[w * 2], lo * 2.0F);
            EXPECT_EQ(channel.values[w * 2 + 1], hi * 2.0F);
        }
    }

    TYPED_TEST(WindowedKernelGpuTest, AWholeCompFileRunsUnderTheSameLayout)
    {
        const auto path = std::filesystem::temp_directory_path() / "mayaflux_windowed_kernel_test.comp";
        {
            std::ofstream file(path);
            file << k_comp_file;
        }
        const auto signal = random_signal(1024, 7);
        WindowedKernelConfig config;
        config.shader_path = path.string();
        config.window_size = 128;
        config.hop_size = 64;
        config.parameters = { 0.5F };
        StandardWindowedKernelAnalyzer analyzer(config);

        const auto channel = analyzer.analyze_windows(input_of({ signal })).channels[0];
        std::filesystem::remove(path);

        constexpr size_t windows = (1024 - 128) / 64 + 1;
        ASSERT_EQ(channel.values.size(), windows);
        for (size_t w = 0; w < windows; ++w) {
            double sum = 0.0;
            for (size_t k = 0; k < 128; ++k) {
                const double v = static_cast<float>(signal[w * 64 + k]);
                sum += v * v;
            }
            expect_relative(channel.values[w], sum * 0.5, 1.0e-4);
        }
    }

    TYPED_TEST(WindowedKernelGpuTest, ManyWindowsCrossTheChunkBoundary)
    {
        std::vector<double> signal(16384 + 8);
        std::iota(signal.begin(), signal.end(), 0.0);
        StandardWindowedKernelAnalyzer analyzer(make_config(k_window_sum, 1, 4, 1));

        const auto channel = analyzer.analyze_windows(input_of({ signal })).channels[0];

        const size_t windows = signal.size() - 3;
        ASSERT_EQ(channel.values.size(), windows);
        ASSERT_EQ(channel.window_positions.size(), windows);
        for (size_t w : { size_t { 0 }, size_t { 16382 }, size_t { 16383 }, size_t { 16384 }, size_t { 16385 }, windows - 1 }) {
            const double want = 4.0 * static_cast<double>(w) + 6.0;
            EXPECT_EQ(channel.values[w], static_cast<float>(want)) << "window " << w;
            EXPECT_EQ(channel.window_positions[w].first, w);
        }
    }

    TYPED_TEST(WindowedKernelGpuTest, EveryChannelIsRunOnItsOwn)
    {
        std::vector<double> first(256, 1.0);
        std::vector<double> second(256, 3.0);
        StandardWindowedKernelAnalyzer analyzer(make_config(k_window_sum, 1, 16, 16));

        const auto analysis = analyzer.analyze_windows(input_of({ first, second }));

        ASSERT_EQ(analysis.channels.size(), 2U);
        for (const float value : analysis.channels[0].values) {
            EXPECT_EQ(value, 16.0F);
        }
        for (const float value : analysis.channels[1].values) {
            EXPECT_EQ(value, 48.0F);
        }
    }

    TYPED_TEST(WindowedKernelGpuTest, AChannelShorterThanOneWindowGivesNoWindows)
    {
        StandardWindowedKernelAnalyzer analyzer(make_config(k_window_sum, 1, 64, 32));

        const auto analysis = analyzer.analyze_windows(input_of({ random_signal(30, 9) }));

        ASSERT_EQ(analysis.channels.size(), 1U);
        EXPECT_TRUE(analysis.channels[0].values.empty());
        EXPECT_TRUE(analysis.channels[0].window_positions.empty());
    }

    TYPED_TEST(WindowedKernelGpuTest, ABodyThatDoesNotCompileHasNoShader)
    {
        const auto config = WindowedKernelContext::kernel_config("this is not glsl;");

        EXPECT_EQ(config.shader_id, Portal::Graphics::INVALID_SHADER);
    }

    TEST(WindowedKernelConfigTest, ExactlyOneOfKernelAndShaderPathIsRequired)
    {
        WindowedKernelConfig neither;
        EXPECT_THROW(StandardWindowedKernelAnalyzer { neither }, std::invalid_argument);

        WindowedKernelConfig both = make_config(k_window_sum, 1, 16, 16);
        both.shader_path = "x.comp";
        EXPECT_THROW(StandardWindowedKernelAnalyzer { both }, std::invalid_argument);
    }

    TEST(WindowedKernelConfigTest, OutputsAndWindowSettingsMustBePositive)
    {
        EXPECT_THROW(StandardWindowedKernelAnalyzer(make_config(k_window_sum, 0, 16, 16)), std::invalid_argument);
        EXPECT_THROW(StandardWindowedKernelAnalyzer(make_config(k_window_sum, 1, 0, 16)), std::invalid_argument);
        EXPECT_THROW(StandardWindowedKernelAnalyzer(make_config(k_window_sum, 1, 16, 0)), std::invalid_argument);
        EXPECT_NO_THROW(StandardWindowedKernelAnalyzer(make_config(k_window_sum, 1, 16, 16)));
    }

}
}
