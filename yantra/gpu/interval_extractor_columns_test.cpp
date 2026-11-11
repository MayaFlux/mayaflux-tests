#include "gtest/gtest.h"

#include <cmath>
#include <tuple>

#include "../../support/gpu_dispatch_test_support.hpp"

#include "MayaFlux/Kinesis/Discrete/IntervalScan.hpp"
#include "MayaFlux/Yantra/Extractors/IntervalExtractor.hpp"

namespace MayaFlux::Test {
namespace {

    using namespace GpuDispatch;
    using Yantra::IntervalExtractorConfig;
    using Yantra::IntervalScanSettings;
    using Yantra::MatrixIntervalExtractor;
    using Yantra::StandardIntervalExtractor;

    using Intervals = std::vector<std::pair<size_t, size_t>>;

    std::vector<double> wave(size_t n, double phase, double rate)
    {
        std::vector<double> out(n);
        for (size_t i = 0; i < n; ++i) {
            out[i] = 0.5 + 0.5 * std::sin(rate * static_cast<double>(i) + phase);
        }
        return out;
    }

    Eigen::MatrixXd matrix_of(const std::vector<std::vector<double>>& channels)
    {
        Eigen::MatrixXd out(static_cast<Eigen::Index>(channels.front().size()), static_cast<Eigen::Index>(channels.size()));
        for (size_t c = 0; c < channels.size(); ++c) {
            for (size_t r = 0; r < channels.front().size(); ++r) {
                out(static_cast<Eigen::Index>(r), static_cast<Eigen::Index>(c)) = channels[c][r];
            }
        }
        return out;
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

    Intervals on_the_host(const IntervalScanSettings& settings, uint32_t length, std::span<const double> scores, std::span<const double> gate)
    {
        Kinesis::Discrete::IntervalScanner scanner(Yantra::interval_scan_config(settings, length));
        auto out = scanner.push(scores, gate);
        const auto rest = scanner.flush();
        out.insert(out.end(), rest.begin(), rest.end());
        return out;
    }

    IntervalExtractorConfig base_config()
    {
        IntervalExtractorConfig config;
        config.flush = true;
        config.scan.samples_per_score = 10;
        config.scan.min_run = 15;
        config.scan.min_gap = 20;
        config.scan.padding = 5;
        config.scan.gap_at_limit = 0;
        return config;
    }

    template <typename Fixture>
    class IntervalColumnsGpuTest : public Fixture { };

    using HardwareOptions = ::testing::Types<GpuDispatchFixture<true>, GpuDispatchFixture<false>>;
    TYPED_TEST_SUITE(IntervalColumnsGpuTest, HardwareOptions);

    TYPED_TEST(IntervalColumnsGpuTest, EachColumnFollowsItsOwnSettings)
    {
        const size_t n = 400;
        const auto a = wave(n, 0.0, 0.07);
        const auto b = wave(n, 1.3, 0.05);
        const auto c = wave(n, 2.6, 0.11);

        auto config = base_config();
        IntervalScanSettings spiky = config.scan;
        spiky.enter_threshold = 0.2;
        spiky.min_run = 0;
        spiky.min_gap = 0;
        spiky.padding = 0;
        IntervalScanSettings slow = config.scan;
        slow.samples_per_score = 20;
        slow.first_sample = 3;
        config.columns = { spiky, slow };

        MatrixIntervalExtractor extractor(config);
        const auto result = extractor.extract_intervals(matrix_of({ a, b, c }));

        ASSERT_EQ(result.channels.size(), 3U);
        EXPECT_EQ(result.channels[0].intervals, on_the_host(spiky, 0, a, {}));
        EXPECT_EQ(result.channels[1].intervals, on_the_host(slow, 0, b, {}));
        EXPECT_EQ(result.channels[2].intervals, on_the_host(config.scan, 0, c, {}));
    }

    TYPED_TEST(IntervalColumnsGpuTest, FortyThreeColumnsWithOwnThresholdsMatchTheHost)
    {
        const size_t n = 300;
        std::vector<std::vector<double>> columns;
        for (size_t k = 0; k < 43; ++k) {
            columns.push_back(wave(n, 0.37 * static_cast<double>(k), 0.03 + 0.002 * static_cast<double>(k)));
        }

        auto config = base_config();
        config.columns.reserve(columns.size());
        for (size_t k = 0; k < columns.size(); ++k) {
            IntervalScanSettings settings = config.scan;
            settings.enter_threshold = 0.2 + 0.01 * static_cast<double>(k);
            settings.min_run = 0;
            settings.min_gap = 1;
            config.columns.push_back(settings);
        }

        MatrixIntervalExtractor extractor(config);
        const auto result = extractor.extract_intervals(matrix_of(columns));

        ASSERT_EQ(result.channels.size(), columns.size());
        for (size_t k = 0; k < columns.size(); ++k) {
            EXPECT_EQ(result.channels[k].intervals, on_the_host(config.columns[k], 0, columns[k], {})) << "column " << k;
        }
    }

    TYPED_TEST(IntervalColumnsGpuTest, EachColumnIsGatedByItsOwnGateColumn)
    {
        const size_t n = 400;
        const auto a = wave(n, 0.0, 0.07);
        const auto b = wave(n, 1.3, 0.05);
        const auto c = wave(n, 2.6, 0.11);

        auto config = base_config();
        IntervalScanSettings gated_by_a = config.scan;
        gated_by_a.gate_channel = 0;
        IntervalScanSettings gated_by_b = config.scan;
        gated_by_b.gate_channel = 1;
        gated_by_b.gate_threshold = 0.3;
        config.columns = { config.scan, gated_by_a, gated_by_b };

        MatrixIntervalExtractor extractor(config);
        const auto result = extractor.extract_intervals(matrix_of({ a, b, c }));

        ASSERT_EQ(result.channels.size(), 3U);
        EXPECT_EQ(result.channels[0].intervals, on_the_host(config.scan, 0, a, {}));
        EXPECT_EQ(result.channels[1].intervals, on_the_host(gated_by_a, 0, b, a));
        EXPECT_EQ(result.channels[2].intervals, on_the_host(gated_by_b, 0, c, b));
    }

    TYPED_TEST(IntervalColumnsGpuTest, AColumnWhoseCapCannotHoldOneHopIsRefused)
    {
        auto config = base_config();
        IntervalScanSettings capped = config.scan;
        capped.max_run = capped.samples_per_score + 2 * capped.padding;
        config.columns = { capped };

        EXPECT_THROW((void)MatrixIntervalExtractor(config), std::invalid_argument);
    }

    TYPED_TEST(IntervalColumnsGpuTest, AGateOfAnotherLengthIsRefused)
    {
        const auto a = wave(200, 0.0, 0.07);
        const auto shorter = wave(150, 1.0, 0.05);

        auto config = base_config();
        IntervalScanSettings gated = config.scan;
        gated.gate_channel = 0;
        config.columns = { config.scan, gated };

        StandardIntervalExtractor extractor(config);

        EXPECT_THROW(std::ignore = extractor.extract_intervals(input_of({ a, shorter })), std::runtime_error);
    }

    TYPED_TEST(IntervalColumnsGpuTest, AResidentGateIsTheBoundScoresOfItsColumn)
    {
        using namespace Yantra;
        using Portal::Graphics::BufferUsageHint;
        constexpr auto f32 = GpuBufferBinding::ElementType::FLOAT32;
        const size_t n = 400;
        const auto gate = wave(n, 0.0, 0.07);
        const auto scored = wave(n, 1.3, 0.05);

        auto config = base_config();
        IntervalScanSettings gated = config.scan;
        gated.gate_channel = 0;
        gated.gate_threshold = 0.4;
        config.columns = { config.scan, gated };

        auto recording = std::make_shared<GpuDispatchRecorder>(true);
        ShaderExecutionContext<> source(GpuComputeConfig { .shader_path = interval_scan_shader, .workgroup_size = { 1, 1, 1 }, .push_constant_size = sizeof(IntervalScanPC) });
        source.set_recording(recording);
        ASSERT_TRUE(source.ensure_gpu_ready());
        source.ensure_shared_buffer(0, 0, n, f32, BufferUsageHint::COMPUTE);
        source.ensure_shared_buffer(0, 1, n, f32, BufferUsageHint::COMPUTE);
        const std::vector<float> gate_floats(gate.begin(), gate.end());
        const std::vector<float> scored_floats(scored.begin(), scored.end());
        source.upload_shared_raw(0, 0, reinterpret_cast<const uint8_t*>(gate_floats.data()), gate_floats.size() * sizeof(float));
        source.upload_shared_raw(0, 1, reinterpret_cast<const uint8_t*>(scored_floats.data()), scored_floats.size() * sizeof(float));

        MatrixIntervalExtractor extractor(config);
        extractor.share_recording(recording);
        extractor.bind_scores(0, source, 0, 0);
        extractor.bind_scores(1, source, 0, 1);
        const auto n32 = static_cast<uint32_t>(n);
        const std::array<uint32_t, 2> counts { n32, n32 };
        const auto result = extractor.extract_resident(counts);

        ASSERT_EQ(result.channels.size(), 2U);
        EXPECT_EQ(result.channels[0].intervals, on_the_host(config.scan, 0, gate, {}));
        EXPECT_EQ(result.channels[1].intervals, on_the_host(gated, 0, scored, gate));
    }

    TYPED_TEST(IntervalColumnsGpuTest, EachColumnSplitIntoTwoCallsGivesTheSameAsOneCall)
    {
        const size_t n = 400;
        const size_t cut = 230;
        const auto a = wave(n, 0.0, 0.07);
        const auto b = wave(n, 1.3, 0.05);

        auto config = base_config();
        IntervalScanSettings slow = config.scan;
        slow.samples_per_score = 20;
        slow.first_sample = 3;
        config.columns = { config.scan, slow };

        const auto whole_a = on_the_host(config.scan, 0, a, {});
        const auto whole_b = on_the_host(slow, 0, b, {});

        MatrixIntervalExtractor extractor(config);
        const auto head = [&](const std::vector<double>& column) {
            return std::vector<double>(column.begin(), column.begin() + static_cast<std::ptrdiff_t>(cut));
        };
        const auto tail = [&](const std::vector<double>& column) {
            return std::vector<double>(column.begin() + static_cast<std::ptrdiff_t>(cut), column.end());
        };

        config.flush = false;
        extractor.set_config(config);
        const auto first = extractor.extract_intervals(matrix_of({ head(a), head(b) }));

        config.flush = true;
        extractor.set_config(config);
        const auto second = extractor.extract_intervals(matrix_of({ tail(a), tail(b) }));

        ASSERT_EQ(first.channels.size(), 2U);
        ASSERT_EQ(second.channels.size(), 2U);
        Intervals got_a = first.channels[0].intervals;
        got_a.insert(got_a.end(), second.channels[0].intervals.begin(), second.channels[0].intervals.end());
        Intervals got_b = first.channels[1].intervals;
        got_b.insert(got_b.end(), second.channels[1].intervals.begin(), second.channels[1].intervals.end());
        EXPECT_EQ(got_a, whole_a);
        EXPECT_EQ(got_b, whole_b);
    }

    TYPED_TEST(IntervalColumnsGpuTest, SettingsChangedWithSetConfigAreCheckedOnTheNextCall)
    {
        const auto a = wave(200, 0.0, 0.07);

        auto config = base_config();
        MatrixIntervalExtractor extractor(config);
        IntervalExtractorConfig bad = config;
        bad.columns = { config.scan };
        bad.columns[0].max_run = bad.columns[0].samples_per_score + 2 * bad.columns[0].padding;
        extractor.set_config(bad);

        EXPECT_THROW(std::ignore = extractor.extract_intervals(matrix_of({ a })), std::runtime_error);
    }

}
}
