#include "gtest/gtest.h"
#include "../../support/test_metadata.hpp"

#include <random>

#include "../../support/gpu_dispatch_test_support.hpp"

#include "MayaFlux/Yantra/Analyzers/WindowedKernelAnalyzer.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))

namespace MayaFlux::Test {
namespace {

    using namespace GpuDispatch;
    using Yantra::GpuBufferBinding;
    using Yantra::GpuDispatchRecorder;
    using Yantra::run_windowed_kernel;
    using Yantra::StandardWindowedKernelAnalyzer;
    using Yantra::windowed_kernel_config;
    using Yantra::windowed_kernel_layout;
    using Yantra::WindowedInput;
    using Yantra::WindowedKernelConfig;
    using Yantra::WindowedKernelShape;

    constexpr const char* k_lane_sum = R"(
    float acc = 0.0;
    for (uint k = lane; k < window_length; k += lanes) {
        acc += sample_at(window, k);
    }
    float total = lane_reduce_sum(acc);
    if (lane == 0u) {
        results[result_base] = total;
    }
)";

    constexpr const char* k_lane_extremes = R"(
    float best = -3.0e38;
    float worst = 3.0e38;
    for (uint k = lane; k < window_length; k += lanes) {
        best = max(best, sample_at(window, k));
        worst = min(worst, sample_at(window, k));
    }
    float top = lane_reduce_max(best);
    float bottom = lane_reduce_min(worst);
    uint index = window_length;
    for (uint k = lane; k < window_length; k += lanes) {
        if (sample_at(window, k) == top) {
            index = min(index, k);
        }
    }
    float first = lane_reduce_min(float(index));
    if (lane == 0u) {
        results[result_base] = top;
        results[result_base + 1u] = bottom;
        results[result_base + 2u] = first;
    }
)";

    constexpr const char* k_lane_first = R"(
    uint who = lane_first(sample_at(window, lane) > parameters[0]);
    if (lane == 0u) {
        results[result_base] = float(who);
    }
)";

    constexpr const char* k_lane_dot = R"(
    float acc = 0.0;
    for (uint k = lane; k < window_length; k += lanes) {
        acc += sample_at(window, k) * sample2_at(window, k);
    }
    float total = lane_reduce_sum(acc);
    if (lane == 0u) {
        results[result_base] = total;
    }
)";

    constexpr const char* k_serial_dot = R"(
    float acc = 0.0;
    for (uint k = 0u; k < window_length; ++k) {
        acc += sample_at(i, k) * sample2_at(i, k);
    }
    results[result_base] = acc;
)";

    constexpr const char* k_noop = "    results[result_base] = 0.0;\n";

    WindowedKernelConfig make_config(const char* kernel, uint32_t outputs, uint32_t window, uint32_t hop, uint32_t lanes, uint32_t inputs = 1)
    {
        WindowedKernelConfig config;
        config.kernel = kernel;
        config.outputs = outputs;
        config.window_size = window;
        config.hop_size = hop;
        config.lanes = lanes;
        config.inputs = inputs;
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

    std::vector<double> quantised_signal(size_t count, uint64_t seed)
    {
        std::mt19937_64 engine(seed);
        std::vector<double> out(count);
        for (auto& value : out) {
            value = static_cast<double>(static_cast<int>(engine() % 9) - 4) * 0.25;
        }
        return out;
    }

    std::vector<float> floats_of(const std::vector<double>& values)
    {
        return { values.begin(), values.end() };
    }

    void expect_relative(double got, double want, double tolerance)
    {
        EXPECT_NEAR(got, want, tolerance * (1.0 + std::abs(want)));
    }

    template <typename Fixture>
    class WindowedKernelLanesGpuTest : public Fixture { };

    using HardwareOptions = ::testing::Types<GpuDispatchFixture<true>, GpuDispatchFixture<false>>;
    TYPED_TEST_SUITE(WindowedKernelLanesGpuTest, HardwareOptions);

    TYPED_TEST(WindowedKernelLanesGpuTest, TheDefaultShapeIsTheOriginalLayout)
    {
        const WindowedKernelShape shape;
        const auto layout = windowed_kernel_layout();

        EXPECT_EQ(shape.workgroup(), 64U);
        EXPECT_EQ(shape.push_words(), 5U);
        EXPECT_EQ(shape.groups(65), 2U);
        ASSERT_EQ(layout.size(), 3U);
        EXPECT_EQ(layout[0].binding, 0U);
        EXPECT_EQ(layout[1].binding, 1U);
        EXPECT_EQ(layout[2].binding, 2U);
        EXPECT_EQ(layout[2].direction, GpuBufferBinding::Direction::OUTPUT);

        const WindowedKernelShape lanes { .lanes = 16, .inputs = 2 };
        EXPECT_EQ(lanes.workgroup(), 16U);
        EXPECT_EQ(lanes.push_words(), 7U);
        EXPECT_EQ(lanes.groups(65), 65U);
        EXPECT_EQ(lanes.parameters_binding(), 2U);
        EXPECT_EQ(lanes.results_binding(), 3U);
        EXPECT_EQ(windowed_kernel_layout(lanes).size(), 4U);
    }

    TYPED_TEST(WindowedKernelLanesGpuTest, ALaneSumMatchesAHostSumAtEveryLaneCount)
    {
        const auto signal = random_signal(6000, 1);
        for (const uint32_t lanes : { 2U, 16U, 64U, 128U }) {
            StandardWindowedKernelAnalyzer analyzer(make_config(k_lane_sum, 1, 1000, 300, lanes));

            const auto channel = analyzer.analyze_windows(input_of({ signal })).channels[0];

            const size_t windows = (signal.size() - 1000) / 300 + 1;
            ASSERT_EQ(channel.values.size(), windows) << "lanes " << lanes;
            for (size_t w = 0; w < windows; ++w) {
                double want = 0.0;
                for (size_t k = 0; k < 1000; ++k) {
                    want += static_cast<double>(static_cast<float>(signal[w * 300 + k]));
                }
                expect_relative(channel.values[w], want, 1.0e-4);
            }
        }
    }

    TYPED_TEST(WindowedKernelLanesGpuTest, LaneMaxMinAndTheFirstIndexOfAMaximumMatchAHostScan)
    {
        const auto signal = quantised_signal(5000, 2);
        StandardWindowedKernelAnalyzer analyzer(make_config(k_lane_extremes, 3, 257, 100, 32));

        const auto channel = analyzer.analyze_windows(input_of({ signal })).channels[0];

        const size_t windows = (signal.size() - 257) / 100 + 1;
        ASSERT_EQ(channel.values.size(), windows * 3);
        for (size_t w = 0; w < windows; ++w) {
            float top = -3.0e38F;
            float bottom = 3.0e38F;
            size_t first = 0;
            for (size_t k = 0; k < 257; ++k) {
                const auto v = static_cast<float>(signal[w * 100 + k]);
                if (v > top) {
                    top = v;
                    first = k;
                }
                bottom = std::min(bottom, v);
            }
            EXPECT_EQ(channel.values[w * 3], top) << "window " << w;
            EXPECT_EQ(channel.values[w * 3 + 1], bottom) << "window " << w;
            EXPECT_EQ(channel.values[w * 3 + 2], static_cast<float>(first)) << "window " << w;
        }
    }

    TYPED_TEST(WindowedKernelLanesGpuTest, LaneFirstReportsTheLowestLaneWhoseConditionHolds)
    {
        auto config = make_config(k_lane_first, 1, 16, 16, 16);
        config.parameters = { 0.5F };
        std::vector<double> signal(16 * 20, 0.0);
        std::mt19937_64 engine(3);
        for (size_t w = 0; w < 20; ++w) {
            if (w % 4 == 3) {
                continue;
            }
            for (int hit = 0; hit < 3; ++hit) {
                signal[w * 16 + engine() % 16] = 1.0;
            }
        }
        StandardWindowedKernelAnalyzer analyzer(config);

        const auto channel = analyzer.analyze_windows(input_of({ signal })).channels[0];

        ASSERT_EQ(channel.values.size(), 20U);
        for (size_t w = 0; w < 20; ++w) {
            size_t want = 16;
            for (size_t k = 0; k < 16; ++k) {
                if (signal[w * 16 + k] > 0.5) {
                    want = k;
                    break;
                }
            }
            EXPECT_EQ(channel.values[w], static_cast<float>(want)) << "window " << w;
        }
    }

    TYPED_TEST(WindowedKernelLanesGpuTest, ManyWindowsCrossTheChunkBoundaryWithOneWorkgroupEach)
    {
        const auto signal = random_signal(40000, 4);
        StandardWindowedKernelAnalyzer analyzer(make_config(k_lane_sum, 1, 8, 1, 4));

        const auto channel = analyzer.analyze_windows(input_of({ signal })).channels[0];

        const size_t windows = signal.size() - 8 + 1;
        ASSERT_EQ(channel.values.size(), windows);
        for (const size_t w : { size_t { 0 }, size_t { 16383 }, size_t { 16384 }, size_t { 16385 }, windows - 1 }) {
            double want = 0.0;
            for (size_t k = 0; k < 8; ++k) {
                want += static_cast<double>(static_cast<float>(signal[w + k]));
            }
            expect_relative(channel.values[w], want, 1.0e-5);
        }
    }

    TYPED_TEST(WindowedKernelLanesGpuTest, TwoChannelsAreTheInputsOfOneKernelRun)
    {
        const auto a = random_signal(3000, 5);
        const auto b = random_signal(2800, 6);
        for (const auto& [kernel, lanes] : { std::pair<const char*, uint32_t> { k_serial_dot, 1U }, std::pair<const char*, uint32_t> { k_lane_dot, 16U } }) {
            StandardWindowedKernelAnalyzer analyzer(make_config(kernel, 1, 200, 100, lanes, 2));

            const auto analysis = analyzer.analyze_windows(input_of({ a, b, random_signal(3000, 7) }));

            ASSERT_EQ(analysis.channels.size(), 1U) << "lanes " << lanes;
            const size_t windows = (b.size() - 200) / 100 + 1;
            ASSERT_EQ(analysis.channels[0].values.size(), windows) << "lanes " << lanes;
            for (size_t w = 0; w < windows; ++w) {
                double want = 0.0;
                for (size_t k = 0; k < 200; ++k) {
                    want += static_cast<double>(static_cast<float>(a[w * 100 + k])) * static_cast<double>(static_cast<float>(b[w * 100 + k]));
                }
                expect_relative(analysis.channels[0].values[w], want, 1.0e-4);
            }
        }
    }

    TYPED_TEST(WindowedKernelLanesGpuTest, EachInputHasItsOwnStrideAndOffset)
    {
        const WindowedKernelShape shape { .lanes = 16, .inputs = 2 };
        const auto config = windowed_kernel_config(k_lane_dot, shape);
        ASSERT_NE(config.shader_id, Portal::Graphics::INVALID_SHADER);
        Yantra::ShaderExecutionContext<> context(config, windowed_kernel_layout(shape), "WindowedKernel");
        const auto a = floats_of(random_signal(2000, 8));
        const auto b = floats_of(random_signal(900, 9));
        constexpr uint32_t length = 64;
        constexpr uint32_t windows = 12;
        const std::array<WindowedInput, 2> inputs { WindowedInput { .samples = a, .stride = 100, .offset = 0 },
            WindowedInput { .samples = b, .stride = 50, .offset = 10 } };

        const auto got = run_windowed_kernel(context, config, shape, inputs, length, windows, 1, {});

        ASSERT_EQ(got.size(), windows);
        for (uint32_t w = 0; w < windows; ++w) {
            double want = 0.0;
            for (uint32_t k = 0; k < length; ++k) {
                want += static_cast<double>(a[w * 100 + k]) * static_cast<double>(b[10 + w * 50 + k]);
            }
            expect_relative(got[w], want, 1.0e-4);
        }
    }

    TYPED_TEST(WindowedKernelLanesGpuTest, AnInputCanBeReadFromAnotherContextsBuffer)
    {
        const WindowedKernelShape shape { .lanes = 16, .inputs = 2 };
        const auto config = windowed_kernel_config(k_lane_dot, shape);
        ASSERT_NE(config.shader_id, Portal::Graphics::INVALID_SHADER);
        const auto owner_config = windowed_kernel_config(k_noop);
        ASSERT_NE(owner_config.shader_id, Portal::Graphics::INVALID_SHADER);

        const auto recording = std::make_shared<GpuDispatchRecorder>(true);
        Yantra::ShaderExecutionContext<> owner(owner_config, windowed_kernel_layout(), "Owner");
        Yantra::ShaderExecutionContext<> consumer(config, windowed_kernel_layout(shape), "WindowedKernel");
        owner.set_recording(recording);
        consumer.set_recording(recording);

        const auto a = floats_of(random_signal(2000, 10));
        const auto b = floats_of(random_signal(900, 11));
        owner.ensure_shared_buffer(0, 0, b.size(), GpuBufferBinding::ElementType::FLOAT32);
        owner.upload_shared_raw(0, 0, reinterpret_cast<const uint8_t*>(b.data()), b.size() * sizeof(float));
        consumer.import_shared(0, 1, owner, 0, 0, GpuBufferBinding::ElementType::FLOAT32);

        constexpr uint32_t length = 64;
        constexpr uint32_t windows = 12;
        const std::array<WindowedInput, 2> inputs { WindowedInput { .samples = a, .stride = 100, .offset = 0 },
            WindowedInput::on_device(50, 10) };

        const auto got = run_windowed_kernel(consumer, config, shape, inputs, length, windows, 1, {});

        ASSERT_EQ(got.size(), windows);
        for (uint32_t w = 0; w < windows; ++w) {
            double want = 0.0;
            for (uint32_t k = 0; k < length; ++k) {
                want += static_cast<double>(a[w * 100 + k]) * static_cast<double>(b[10 + w * 50 + k]);
            }
            expect_relative(got[w], want, 1.0e-4);
        }
    }

    TYPED_TEST(WindowedKernelLanesGpuTest, AShapeThatCannotRunIsRefusedWithTheReason)
    {
        EXPECT_THROW(StandardWindowedKernelAnalyzer(make_config(k_lane_sum, 1, 64, 32, 3)), std::invalid_argument);
        EXPECT_THROW(StandardWindowedKernelAnalyzer(make_config(k_lane_sum, 1, 64, 32, 2048)), std::invalid_argument);
        EXPECT_THROW(StandardWindowedKernelAnalyzer(make_config(k_lane_sum, 1, 64, 32, 1, 0)), std::invalid_argument);
        EXPECT_THROW(StandardWindowedKernelAnalyzer(make_config(k_lane_sum, 1, 64, 32, 1, 5)), std::invalid_argument);
        EXPECT_FALSE(Yantra::windowed_kernel_diagnose({ .lanes = 6, .inputs = 1 }).empty());
        EXPECT_TRUE(Yantra::windowed_kernel_diagnose({ .lanes = 1, .inputs = 4 }).empty());
        EXPECT_EQ(windowed_kernel_config(k_lane_sum, { .lanes = 6, .inputs = 1 }).shader_id, Portal::Graphics::INVALID_SHADER);

        const WindowedKernelShape shape { .lanes = 1, .inputs = 2 };
        const auto config = windowed_kernel_config(k_serial_dot, shape);
        Yantra::ShaderExecutionContext<> context(config, windowed_kernel_layout(shape), "WindowedKernel");
        const std::vector<float> one(100, 1.0F);
        const std::array<WindowedInput, 1> too_few { WindowedInput { .samples = one, .stride = 10, .offset = 0 } };
        EXPECT_THROW(static_cast<void>(run_windowed_kernel(context, config, shape, too_few, 10, 5, 1, {})), std::invalid_argument);
    }

    TYPED_TEST(WindowedKernelLanesGpuTest, AKernelOfTwoInputsNeedsThatManyChannels)
    {
        StandardWindowedKernelAnalyzer analyzer(make_config(k_serial_dot, 1, 64, 32, 1, 2));

        try {
            const auto analysis = analyzer.analyze_windows(input_of({ random_signal(500, 12) }));
            EXPECT_TRUE(analysis.channels.empty() || analysis.channels[0].values.empty());
        } catch (const std::exception&) {
            SUCCEED();
        }
    }

} // namespace
} // namespace MayaFlux::Test
