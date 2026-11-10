#include "gtest/gtest.h"

#include "../../support/gpu_dispatch_test_support.hpp"
#include "../../support/interval_reference_support.hpp"

#include "MayaFlux/Kinesis/Discrete/IntervalScan.hpp"
#include "MayaFlux/Yantra/Extractors/IntervalExtractor.hpp"

namespace MayaFlux::Test {
namespace {

    namespace Ref = IntervalReference;

    using namespace GpuDispatch;
    using Yantra::ChannelIntervals;
    using Yantra::interval_regions;
    using Yantra::IntervalExtractorConfig;
    using Yantra::MatrixIntervalExtractor;
    using Yantra::StandardIntervalExtractor;

    using Intervals = Ref::Intervals;

    struct ReferenceCase {
        std::string name;
        IntervalExtractorConfig config;
        std::vector<double> scores;
        Intervals expect;
    };

    std::vector<ReferenceCase> load_cases()
    {
        std::vector<ReferenceCase> cases;
        for (auto& c : Ref::load()) {
            IntervalExtractorConfig config;
            config.enter_threshold = c.enter;
            config.min_run = c.min_run;
            config.min_gap = c.min_gap;
            config.padding = c.padding;
            config.gap_at_limit = c.gap_at_limit;
            config.max_run = c.max_run;
            config.use_longest_gap = c.longest;
            config.length = c.length;
            cases.push_back({ .name = std::move(c.name), .config = config, .scores = std::move(c.scores), .expect = std::move(c.expect) });
        }
        return cases;
    }

    Eigen::MatrixXd matrix_of(const std::vector<std::vector<double>>& channels)
    {
        Eigen::MatrixXd out(static_cast<Eigen::Index>(channels.front().size()), static_cast<Eigen::Index>(channels.size()));
        for (size_t c = 0; c < channels.size(); ++c) {
            EXPECT_EQ(channels[c].size(), channels.front().size()) << "matrix columns must have equal length";
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

    IntervalExtractorConfig small_config()
    {
        IntervalExtractorConfig config;
        config.samples_per_score = 10;
        config.min_run = 15;
        config.min_gap = 20;
        config.padding = 5;
        config.gap_at_limit = 0;
        return config;
    }

    Intervals extract(MatrixIntervalExtractor& extractor, const std::vector<double>& scores)
    {
        return extractor.extract_intervals(matrix_of({ scores })).channels[0].intervals;
    }

    Intervals whole(IntervalExtractorConfig config, const std::vector<double>& scores)
    {
        config.flush = true;
        MatrixIntervalExtractor extractor(config);
        return extract(extractor, scores);
    }

    Intervals gated(IntervalExtractorConfig config, const std::vector<double>& gate, const std::vector<double>& scores)
    {
        config.gate_channel = 0;
        config.flush = true;
        MatrixIntervalExtractor extractor(config);
        return extractor.extract_intervals(matrix_of({ gate, scores })).channels[1].intervals;
    }

    Kinesis::Discrete::IntervalScanConfig scan_config_of(const IntervalExtractorConfig& config)
    {
        return {
            .enter = config.enter_threshold,
            .leave = std::max(config.enter_threshold - config.leave_margin, config.leave_floor),
            .gate_threshold = config.gate_threshold,
            .hop = config.samples_per_score,
            .first_sample = config.first_sample,
            .min_run = config.min_run,
            .min_gap = config.min_gap,
            .padding = config.padding,
            .gap_at_limit = config.gap_at_limit,
            .max_run = config.max_run,
            .use_longest_gap = config.use_longest_gap,
            .length = config.length,
            .emit_on_close = config.emit_on_close,
        };
    }

    Intervals on_the_host(const IntervalExtractorConfig& config, const std::vector<double>& scores, const std::vector<double>& gate)
    {
        Kinesis::Discrete::IntervalScanner scanner(scan_config_of(config));
        auto out = scanner.push(scores, gate);
        const auto rest = scanner.flush();
        out.insert(out.end(), rest.begin(), rest.end());
        return out;
    }

    template <typename Fixture>
    class IntervalExtractorGpuTest : public Fixture { };

    using HardwareOptions = ::testing::Types<GpuDispatchFixture<true>, GpuDispatchFixture<false>>;
    TYPED_TEST_SUITE(IntervalExtractorGpuTest, HardwareOptions);

    TYPED_TEST(IntervalExtractorGpuTest, EveryReferenceCaseMatchesTheOriginalTimestamps)
    {
        const auto cases = load_cases();
        if (cases.empty()) {
            GTEST_SKIP() << Ref::fixture_path().string() << " is not present";
        }
        for (const auto& c : cases) {
            EXPECT_EQ(whole(c.config, c.scores), c.expect) << c.name;
        }
    }

    TYPED_TEST(IntervalExtractorGpuTest, ASplitStreamGivesTheSameIntervalsAsOneCall)
    {
        const auto cases = load_cases();
        if (cases.empty()) {
            GTEST_SKIP() << Ref::fixture_path().string() << " is not present";
        }
        constexpr std::array<size_t, 5> sizes { 1, 7, 64, 2, 333 };
        for (const auto& c : cases) {
            auto config = c.config;
            config.flush = false;
            MatrixIntervalExtractor extractor(config);

            Intervals joined;
            size_t at = 0;
            size_t step = 0;
            while (at < c.scores.size()) {
                const size_t count = std::min(sizes[step++ % sizes.size()], c.scores.size() - at);
                const bool last = at + count == c.scores.size();
                if (last) {
                    auto closing = config;
                    closing.flush = true;
                    extractor.set_config(closing);
                }
                const std::vector<double> part(c.scores.begin() + static_cast<std::ptrdiff_t>(at),
                    c.scores.begin() + static_cast<std::ptrdiff_t>(at + count));
                const auto got = extract(extractor, part);
                joined.insert(joined.end(), got.begin(), got.end());
                at += count;
            }
            EXPECT_EQ(joined, c.expect) << c.name;
        }
    }

    TYPED_TEST(IntervalExtractorGpuTest, ARunIsPaddedAndReleasedByTheFlush)
    {
        const std::vector<double> scores { 0, 0, 1, 1, 1, 0, 0, 0, 0, 0 };
        auto config = small_config();
        config.length = 100;

        EXPECT_EQ(whole(config, scores), (Intervals { { 15, 55 } }));
    }

    TYPED_TEST(IntervalExtractorGpuTest, TheNewestIntervalWaitsForTheNextCall)
    {
        const std::vector<double> scores { 0, 0, 1, 1, 1, 0, 0, 0, 0, 0 };
        auto config = small_config();
        config.length = 120;
        MatrixIntervalExtractor extractor(config);

        EXPECT_TRUE(extract(extractor, scores).empty());

        config.flush = true;
        extractor.set_config(config);
        EXPECT_EQ(extract(extractor, { 0, 0 }), (Intervals { { 15, 55 } }));
    }

    TYPED_TEST(IntervalExtractorGpuTest, AGapUnderTwiceThePaddingIsSplitBetweenNeighbours)
    {
        const std::vector<double> scores { 0, 0, 1, 1, 1, 0, 0, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 };
        auto config = small_config();
        config.padding = 30;
        config.length = 200;

        EXPECT_EQ(whole(config, scores), (Intervals { { 0, 75 }, { 75, 160 } }));
    }

    TYPED_TEST(IntervalExtractorGpuTest, AWideGapKeepsTheFullPaddingOnBothSides)
    {
        const std::vector<double> scores { 0, 0, 1, 1, 1, 0, 0, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 };
        auto config = small_config();
        config.padding = 10;
        config.length = 200;

        EXPECT_EQ(whole(config, scores), (Intervals { { 10, 60 }, { 90, 140 } }));
    }

    TYPED_TEST(IntervalExtractorGpuTest, ARunShorterThanTheMinimumIsDropped)
    {
        const std::vector<double> scores { 0, 1, 0, 0, 0, 0, 0 };
        auto config = small_config();
        config.length = 70;

        EXPECT_TRUE(whole(config, scores).empty());
    }

    TYPED_TEST(IntervalExtractorGpuTest, AGapShorterThanTheMinimumDoesNotEndTheRun)
    {
        const std::vector<double> scores { 0, 1, 1, 0, 1, 1, 0, 0, 0, 0, 0 };
        auto config = small_config();
        config.padding = 0;
        config.length = 110;

        EXPECT_EQ(whole(config, scores), (Intervals { { 10, 60 } }));
    }

    TYPED_TEST(IntervalExtractorGpuTest, ScoresBetweenTheThresholdsHoldTheState)
    {
        const std::vector<double> scores { 0, 1, 1, 0.4, 0.4, 0.4, 0.4, 0.4, 0.4, 1, 1, 0, 0, 0, 0 };
        auto config = small_config();
        config.padding = 0;
        config.length = 150;

        EXPECT_EQ(whole(config, scores), (Intervals { { 10, 110 } }));
    }

    TYPED_TEST(IntervalExtractorGpuTest, AFlushEndsTheStreamAndTheNextCallStartsANewOne)
    {
        const std::vector<double> scores { 0, 0, 1, 1, 1, 0, 0, 0, 0, 0 };
        auto config = small_config();
        config.length = 100;
        config.flush = true;
        MatrixIntervalExtractor extractor(config);

        const auto first = extract(extractor, scores);
        const auto second = extract(extractor, scores);

        EXPECT_EQ(first, (Intervals { { 15, 55 } }));
        EXPECT_EQ(second, first);
    }

    TYPED_TEST(IntervalExtractorGpuTest, ResetForgetsAnOpenRun)
    {
        auto config = small_config();
        config.length = 100;
        MatrixIntervalExtractor extractor(config);
        EXPECT_TRUE(extract(extractor, { 0, 0, 1, 1, 1 }).empty());

        extractor.reset();
        config.flush = true;
        extractor.set_config(config);

        EXPECT_EQ(extract(extractor, { 0, 0, 1, 1, 1, 0, 0, 0, 0, 0 }), (Intervals { { 15, 55 } }));
    }

    TYPED_TEST(IntervalExtractorGpuTest, EveryChannelKeepsItsOwnState)
    {
        const std::vector<double> first { 0, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 };
        const std::vector<double> second { 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0 };
        auto config = small_config();
        config.length = 120;
        config.flush = true;

        const auto alone_first = whole(config, first);
        const auto alone_second = whole(config, second);

        MatrixIntervalExtractor extractor(config);
        const auto both = extractor.extract_intervals(matrix_of({ first, second }));

        ASSERT_EQ(both.channels.size(), 2U);
        EXPECT_EQ(both.channels[0].intervals, alone_first);
        EXPECT_EQ(both.channels[1].intervals, alone_second);
        EXPECT_FALSE(alone_second.empty());
    }

    TYPED_TEST(IntervalExtractorGpuTest, TheOutputDataHoldsStartAndEndOfEachInterval)
    {
        auto config = small_config();
        config.length = 100;
        config.flush = true;
        MatrixIntervalExtractor extractor(config);

        const auto result = extractor.apply_operation(
            Yantra::Datum<Eigen::MatrixXd> { matrix_of({ { 0, 0, 1, 1, 1, 0, 0, 0, 0, 0 } }) });

        ASSERT_EQ(result.data.size(), 1U);
        EXPECT_EQ(result.data[0], (std::vector<double> { 15.0, 55.0 }));
    }

    TYPED_TEST(IntervalExtractorGpuTest, VariantInputGivesTheSameIntervalsAsMatrixInput)
    {
        const std::vector<double> scores { 0, 0, 1, 1, 1, 0, 0, 0, 0, 0 };
        auto config = small_config();
        config.length = 100;
        config.flush = true;
        StandardIntervalExtractor extractor(config);

        const auto got = extractor.extract_intervals(input_of({ scores })).channels[0].intervals;

        EXPECT_EQ(got, whole(config, scores));
    }

    TYPED_TEST(IntervalExtractorGpuTest, TheFirstSampleMovesEveryPositionAndTheClamp)
    {
        auto config = small_config();
        config.first_sample = 5;
        config.length = 120;

        EXPECT_EQ(whole(config, { 0, 0, 1, 1, 1, 0, 0, 0, 0, 0 }), (Intervals { { 20, 60 } }));
    }

    TYPED_TEST(IntervalExtractorGpuTest, TheFirstSampleCountsInTheEndOfTheScoresWhenNoLengthIsSet)
    {
        auto config = small_config();
        config.first_sample = 5;
        config.padding = 100;

        EXPECT_EQ(whole(config, { 0, 0, 1, 1, 1, 1, 1, 1 }), (Intervals { { 0, 85 } }));
    }

    TYPED_TEST(IntervalExtractorGpuTest, AGateBelowItsThresholdEndsARunAtOnce)
    {
        auto config = small_config();
        config.padding = 0;
        config.length = 120;
        const std::vector<double> scores { 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0 };
        const std::vector<double> gate { 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1 };

        EXPECT_EQ(whole(config, scores), (Intervals { { 10, 90 } }));
        EXPECT_EQ(gated(config, gate, scores), (Intervals { { 10, 50 }, { 60, 90 } }));
    }

    TYPED_TEST(IntervalExtractorGpuTest, AGapIsBridgedOnlyWhileTheGateStaysHigh)
    {
        auto config = small_config();
        config.padding = 0;
        config.length = 140;
        const std::vector<double> scores { 0, 1, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0 };
        const std::vector<double> open(14, 1.0);
        const std::vector<double> broken { 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1 };

        EXPECT_EQ(gated(config, open, scores), (Intervals { { 10, 100 } }));
        EXPECT_EQ(gated(config, broken, scores), (Intervals { { 10, 40 }, { 60, 100 } }));
    }

    TYPED_TEST(IntervalExtractorGpuTest, ALowGateIsNeverEngaged)
    {
        auto config = small_config();
        config.padding = 0;
        config.length = 100;
        const std::vector<double> scores { 1, 1, 1, 1, 1, 1, 0, 0, 0, 0 };
        const std::vector<double> gate { 0, 0, 1, 1, 1, 1, 1, 1, 1, 1 };

        EXPECT_EQ(gated(config, gate, scores), (Intervals { { 20, 60 } }));
    }

    TYPED_TEST(IntervalExtractorGpuTest, ARunEndedByTheGateIsStillPaddedAgainstItsNeighbour)
    {
        auto config = small_config();
        config.length = 120;
        const std::vector<double> scores { 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0 };
        const std::vector<double> gate { 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1 };

        EXPECT_EQ(gated(config, gate, scores), (Intervals { { 5, 55 }, { 55, 95 } }));
    }

    TYPED_TEST(IntervalExtractorGpuTest, ARunShorterThanTheMinimumIsDroppedWhenTheGateEndsIt)
    {
        auto config = small_config();
        config.length = 100;
        const std::vector<double> scores { 0, 1, 1, 1, 1, 1, 0, 0, 0, 0 };
        const std::vector<double> gate { 1, 1, 0, 1, 1, 1, 1, 1, 1, 1 };

        EXPECT_EQ(gated(config, gate, scores), (Intervals { { 25, 65 } }));
    }

    TYPED_TEST(IntervalExtractorGpuTest, TheGateColumnItselfIsScannedAsItIs)
    {
        auto config = small_config();
        config.padding = 0;
        config.length = 120;
        const std::vector<double> voice { 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1 };
        const std::vector<double> vowel { 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0 };

        auto with_gate = config;
        with_gate.gate_channel = 0;
        with_gate.flush = true;
        MatrixIntervalExtractor extractor(with_gate);
        const auto both = extractor.extract_intervals(matrix_of({ voice, vowel }));

        ASSERT_EQ(both.channels.size(), 2U);
        EXPECT_EQ(both.channels[0].intervals, whole(config, voice));
        EXPECT_EQ(both.channels[1].intervals, (Intervals { { 10, 50 }, { 60, 90 } }));
    }

    TYPED_TEST(IntervalExtractorGpuTest, AGatedSplitStreamGivesTheSameIntervalsAsOneCall)
    {
        std::vector<double> scores;
        std::vector<double> gate;
        Ref::random_gated(5, 600, scores, gate);
        auto config = small_config();
        config.gate_channel = 0;
        config.first_sample = 3;
        config.length = 10000;

        const auto expected = gated(config, gate, scores);
        EXPECT_FALSE(expected.empty());

        config.flush = false;
        MatrixIntervalExtractor extractor(config);
        Intervals joined;
        for (size_t at = 0; at < scores.size(); at += 41) {
            const size_t count = std::min<size_t>(41, scores.size() - at);
            if (at + count == scores.size()) {
                auto closing = config;
                closing.flush = true;
                extractor.set_config(closing);
            }
            const std::vector<double> gate_part(gate.begin() + static_cast<std::ptrdiff_t>(at), gate.begin() + static_cast<std::ptrdiff_t>(at + count));
            const std::vector<double> score_part(scores.begin() + static_cast<std::ptrdiff_t>(at), scores.begin() + static_cast<std::ptrdiff_t>(at + count));
            const auto got = extractor.extract_intervals(matrix_of({ gate_part, score_part })).channels[1].intervals;
            joined.insert(joined.end(), got.begin(), got.end());
        }

        EXPECT_EQ(joined, expected);
    }

    TYPED_TEST(IntervalExtractorGpuTest, TheHostScannerGivesTheSameIntervalsOnRandomGatedSequences)
    {
        for (const uint32_t seed : { 1U, 2U, 3U, 4U }) {
            std::vector<double> scores;
            std::vector<double> gate;
            Ref::random_gated(seed, 800, scores, gate);

            auto config = small_config();
            config.gate_channel = 0;
            config.first_sample = seed * 7;
            config.max_run = seed % 2 == 0 ? 0U : 200U;
            config.use_longest_gap = seed < 3;
            config.emit_on_close = seed % 2 == 1;
            config.length = 20000;

            const auto device = gated(config, gate, scores);
            EXPECT_FALSE(device.empty()) << seed;
            EXPECT_EQ(device, on_the_host(config, scores, gate)) << seed;
        }
    }

    TYPED_TEST(IntervalExtractorGpuTest, TheHostScannerGivesTheSameIntervalsOnEveryReferenceCase)
    {
        const auto cases = load_cases();
        if (cases.empty()) {
            GTEST_SKIP() << Ref::fixture_path().string() << " is not present";
        }
        for (const auto& c : cases) {
            EXPECT_EQ(whole(c.config, c.scores), on_the_host(c.config, c.scores, {})) << c.name;
        }
    }

    TYPED_TEST(IntervalExtractorGpuTest, ARunInProgressAndAPendingIntervalAreReportedOnEveryCall)
    {
        auto config = small_config();
        config.length = 200;
        MatrixIntervalExtractor extractor(config);

        const auto first = extractor.extract_intervals(matrix_of({ { 0, 0, 1, 1, 1 } })).channels[0];
        EXPECT_TRUE(first.intervals.empty());
        EXPECT_EQ(first.open_start, std::optional<size_t> { 20 });
        EXPECT_FALSE(first.pending.has_value());

        const auto second = extractor.extract_intervals(matrix_of({ { 1, 1 } })).channels[0];
        EXPECT_EQ(second.open_start, std::optional<size_t> { 20 });

        const auto third = extractor.extract_intervals(matrix_of({ { 0, 0, 0 } })).channels[0];
        EXPECT_TRUE(third.intervals.empty());
        EXPECT_FALSE(third.open_start.has_value());
        EXPECT_EQ(third.pending, (std::optional<std::pair<size_t, size_t>> { { 15, 70 } }));

        config.flush = true;
        extractor.set_config(config);
        const auto last = extractor.extract_intervals(matrix_of({ { 0 } })).channels[0];
        EXPECT_EQ(last.intervals, (Intervals { { 15, 75 } }));
        EXPECT_FALSE(last.open_start.has_value());
        EXPECT_FALSE(last.pending.has_value());
    }

    TYPED_TEST(IntervalExtractorGpuTest, EmitOnCloseReleasesAnIntervalAtOnceWithFullPaddingOnBothSides)
    {
        auto config = small_config();
        config.padding = 30;
        config.length = 200;
        config.emit_on_close = true;
        MatrixIntervalExtractor extractor(config);

        const auto result = extractor.extract_intervals(
            matrix_of({ { 0, 0, 1, 1, 1, 0, 0, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 } })).channels[0];

        EXPECT_EQ(result.intervals, (Intervals { { 0, 80 }, { 70, 160 } }));
        EXPECT_FALSE(result.pending.has_value());
    }

    TYPED_TEST(IntervalExtractorGpuTest, TheReportsMatchTheHostScannerAfterEveryCall)
    {
        std::vector<double> scores;
        std::vector<double> gate;
        Ref::random_gated(9, 500, scores, gate);
        auto config = small_config();
        config.gate_channel = 0;
        config.length = 10000;

        MatrixIntervalExtractor extractor(config);
        Kinesis::Discrete::IntervalScanner scanner(scan_config_of(config));
        for (size_t at = 0; at < scores.size(); at += 29) {
            const size_t count = std::min<size_t>(29, scores.size() - at);
            const std::vector<double> gate_part(gate.begin() + static_cast<std::ptrdiff_t>(at), gate.begin() + static_cast<std::ptrdiff_t>(at + count));
            const std::vector<double> score_part(scores.begin() + static_cast<std::ptrdiff_t>(at), scores.begin() + static_cast<std::ptrdiff_t>(at + count));

            const auto device = extractor.extract_intervals(matrix_of({ gate_part, score_part })).channels[1];
            const auto host = scanner.push(score_part, gate_part);

            EXPECT_EQ(device.intervals, host) << at;
            EXPECT_EQ(device.open_start, scanner.open_start()) << at;
            EXPECT_EQ(device.pending, scanner.pending()) << at;
        }
    }

    TYPED_TEST(IntervalExtractorGpuTest, AStageRecordedInACallersOwnContextGivesTheExtractorsIntervals)
    {
        using namespace Yantra;
        using Portal::Graphics::BufferUsageHint;

        std::vector<double> scores;
        std::vector<double> gate;
        Ref::random_gated(21, 400, scores, gate);
        auto config = small_config();
        config.gate_channel = 0;
        config.length = 10000;
        const auto expected = gated(config, gate, scores);
        ASSERT_FALSE(expected.empty());

        auto recording = std::make_shared<GpuDispatchRecorder>(true);
        ShaderExecutionContext<> context(GpuComputeConfig { .shader_path = interval_scan_shader, .workgroup_size = { 1, 1, 1 }, .push_constant_size = sizeof(IntervalScanPC) });
        context.set_recording(recording);
        ASSERT_TRUE(context.ensure_gpu_ready());

        constexpr auto f32 = GpuBufferBinding::ElementType::FLOAT32;
        constexpr auto u32 = GpuBufferBinding::ElementType::UINT32;
        constexpr size_t scores_binding = 0;
        constexpr size_t gate_binding = 1;
        constexpr size_t state_binding = 2;
        constexpr size_t runs_binding = 3;
        constexpr size_t report_binding = 4;
        const auto n = static_cast<uint32_t>(scores.size());

        context.ensure_shared_buffer(0, scores_binding, n, f32, BufferUsageHint::COMPUTE);
        context.ensure_shared_buffer(0, gate_binding, n, f32, BufferUsageHint::COMPUTE);
        context.ensure_shared_buffer(0, state_binding, interval_scan_state_words, u32, BufferUsageHint::COMPUTE);
        context.ensure_shared_buffer(0, runs_binding, interval_scan_runs_words(n), u32);
        context.ensure_shared_buffer(0, report_binding, interval_scan_report_words, u32);

        const std::vector<float> score_floats(scores.begin(), scores.end());
        const std::vector<float> gate_floats(gate.begin(), gate.end());
        const std::vector<uint32_t> state_zeros(interval_scan_state_words, 0U);
        const uint32_t no_runs = 0;
        context.upload_shared_raw(0, scores_binding, reinterpret_cast<const uint8_t*>(score_floats.data()), score_floats.size() * sizeof(float));
        context.upload_shared_raw(0, gate_binding, reinterpret_cast<const uint8_t*>(gate_floats.data()), gate_floats.size() * sizeof(float));
        context.upload_shared_raw(0, state_binding, reinterpret_cast<const uint8_t*>(state_zeros.data()), state_zeros.size() * sizeof(uint32_t));
        context.upload_shared_raw(0, report_binding, reinterpret_cast<const uint8_t*>(&no_runs), sizeof(no_runs));

        context.record_stages({ interval_scan_stage(scan_config_of(config),
            context.shared_buffer_address(0, scores_binding), context.shared_buffer_address(0, gate_binding),
            context.shared_buffer_address(0, state_binding), context.shared_buffer_address(0, runs_binding),
            context.shared_buffer_address(0, report_binding), 0, n, true) });

        std::array<uint32_t, interval_scan_report_words> words {};
        context.download_shared(0, report_binding, words.data(), sizeof(words));
        const auto report = decode_interval_scan_report(words);
        std::vector<uint32_t> runs(static_cast<size_t>(report.emitted) * 2);
        context.download_shared(0, runs_binding, runs.data(), runs.size() * sizeof(uint32_t));

        Intervals got;
        for (uint32_t i = 0; i < report.emitted; ++i) {
            got.emplace_back(runs[2 * i], runs[2 * i + 1]);
        }
        EXPECT_EQ(got, expected);
        EXPECT_FALSE(report.open_start.has_value());
        EXPECT_FALSE(report.pending.has_value());
    }

    TYPED_TEST(IntervalExtractorGpuTest, ASubmissionCollectedLaterGivesTheSynchronousResult)
    {
        std::vector<double> scores;
        std::vector<double> gate;
        Ref::random_gated(31, 500, scores, gate);
        auto config = small_config();
        config.gate_channel = 0;
        config.length = 10000;

        MatrixIntervalExtractor synchronous(config);
        const auto expected = synchronous.extract_intervals(matrix_of({ gate, scores }));

        MatrixIntervalExtractor deferred(config);
        ASSERT_TRUE(deferred.submit(matrix_of({ gate, scores })));
        EXPECT_TRUE(deferred.pending());
        for (size_t spins = 0; !deferred.ready() && spins < 10000000; ++spins) { }
        const auto got = deferred.collect();

        ASSERT_TRUE(got.has_value());
        EXPECT_FALSE(deferred.pending());
        ASSERT_EQ(got->channels.size(), 2U);
        for (size_t channel = 0; channel < 2; ++channel) {
            EXPECT_EQ(got->channels[channel].intervals, expected.channels[channel].intervals) << channel;
            EXPECT_EQ(got->channels[channel].open_start, expected.channels[channel].open_start) << channel;
            EXPECT_EQ(got->channels[channel].pending, expected.channels[channel].pending) << channel;
        }
    }

    TYPED_TEST(IntervalExtractorGpuTest, AnOutstandingSubmissionRefusesAnotherAndASynchronousCall)
    {
        auto config = small_config();
        config.length = 100;
        MatrixIntervalExtractor extractor(config);

        EXPECT_TRUE(extractor.ready());
        EXPECT_FALSE(extractor.pending());
        EXPECT_FALSE(extractor.collect().has_value());

        ASSERT_TRUE(extractor.submit(matrix_of({ { 0, 0, 1, 1, 1 } })));
        EXPECT_FALSE(extractor.submit(matrix_of({ { 0, 0, 1, 1, 1 } })));
        EXPECT_THROW(std::ignore = extractor.extract_intervals(matrix_of({ { 0, 0, 1, 1, 1 } })), std::runtime_error);

        for (size_t spins = 0; !extractor.ready() && spins < 10000000; ++spins) { }
        EXPECT_TRUE(extractor.collect().has_value());
        EXPECT_FALSE(extractor.collect().has_value());
        EXPECT_NO_THROW(std::ignore = extractor.extract_intervals(matrix_of({ { 0, 0 } })));
    }

    TYPED_TEST(IntervalExtractorGpuTest, TheStreamContinuesAcrossSubmissions)
    {
        auto config = small_config();
        config.length = 100;
        MatrixIntervalExtractor extractor(config);

        ASSERT_TRUE(extractor.submit(matrix_of({ { 0, 0, 1, 1, 1 } })));
        for (size_t spins = 0; !extractor.ready() && spins < 10000000; ++spins) { }
        const auto first = extractor.collect();
        ASSERT_TRUE(first.has_value());
        EXPECT_TRUE(first->channels[0].intervals.empty());
        EXPECT_EQ(first->channels[0].open_start, std::optional<size_t> { 20 });

        config.flush = true;
        extractor.set_config(config);
        ASSERT_TRUE(extractor.submit(matrix_of({ { 0, 0, 0, 0, 0 } })));
        for (size_t spins = 0; !extractor.ready() && spins < 10000000; ++spins) { }
        const auto second = extractor.collect();

        ASSERT_TRUE(second.has_value());
        EXPECT_EQ(second->channels[0].intervals, (Intervals { { 15, 55 } }));
    }

    TYPED_TEST(IntervalExtractorGpuTest, ResetDiscardsAnOutstandingSubmission)
    {
        auto config = small_config();
        config.length = 100;
        MatrixIntervalExtractor extractor(config);
        ASSERT_TRUE(extractor.submit(matrix_of({ { 0, 0, 1, 1, 1 } })));

        extractor.reset();

        EXPECT_FALSE(extractor.pending());
        EXPECT_TRUE(extractor.ready());
        EXPECT_FALSE(extractor.collect().has_value());
        EXPECT_TRUE(extractor.buffers().empty());
    }

    TYPED_TEST(IntervalExtractorGpuTest, TheBuffersHoldTheLatestResultsOnTheDevice)
    {
        auto config = small_config();
        config.length = 100;
        config.flush = true;
        MatrixIntervalExtractor extractor(config);
        EXPECT_TRUE(extractor.buffers().empty());

        const auto result = extractor.extract_intervals(
            matrix_of({ { 0, 0, 1, 1, 1, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 1, 1, 1, 1, 0, 0 } }));
        const auto views = extractor.buffers();

        ASSERT_EQ(views.size(), 2U);
        for (size_t channel = 0; channel < 2; ++channel) {
            const auto& view = views[channel];
            ASSERT_NE(view.runs_address, 0U);
            ASSERT_NE(view.report_address, 0U);
            ASSERT_TRUE(view.runs && view.runs->mapped_ptr);
            ASSERT_TRUE(view.report && view.report->mapped_ptr);
            EXPECT_EQ(view.capacity, 14U);

            std::array<uint32_t, Yantra::interval_scan_report_words> words {};
            std::memcpy(words.data(), view.report->mapped_ptr, sizeof(words));
            const auto report = Yantra::decode_interval_scan_report(words);
            const auto& expected = result.channels[channel].intervals;
            ASSERT_EQ(report.emitted, expected.size()) << channel;

            const auto* runs = static_cast<const uint32_t*>(view.runs->mapped_ptr);
            for (size_t i = 0; i < expected.size(); ++i) {
                EXPECT_EQ(runs[2 * i], expected[i].first) << channel;
                EXPECT_EQ(runs[2 * i + 1], expected[i].second) << channel;
            }
        }
        EXPECT_NE(views[0].runs_address, views[1].runs_address);
    }

    TYPED_TEST(IntervalExtractorGpuTest, AGateChannelThatIsNotAColumnIsRefused)
    {
        auto config = small_config();
        config.gate_channel = 2;
        config.flush = true;
        MatrixIntervalExtractor extractor(config);

        EXPECT_THROW(std::ignore = extractor.extract_intervals(matrix_of({ { 0, 1 }, { 0, 1 } })), std::runtime_error);
    }

    TEST(IntervalExtractorTest, RegionsSpanTheIntervalsInclusivelyAndSkipEmptyOnes)
    {
        const ChannelIntervals intervals { .intervals = { { 15, 55 }, { 60, 60 }, { 70, 71 } } };

        const auto group = interval_regions(intervals, 0, 1, "speech");

        ASSERT_EQ(group.regions.size(), 2U);
        EXPECT_EQ(group.name, "speech");
        EXPECT_EQ(group.regions[0].start_coordinates, (std::vector<uint64_t> { 15, 0 }));
        EXPECT_EQ(group.regions[0].end_coordinates, (std::vector<uint64_t> { 54, 1 }));
        EXPECT_EQ(group.regions[1].end_coordinates, (std::vector<uint64_t> { 70, 1 }));
    }

    TEST(IntervalExtractorTest, InvalidSettingsAreRefused)
    {
        IntervalExtractorConfig no_hop;
        no_hop.samples_per_score = 0;
        EXPECT_THROW(MatrixIntervalExtractor { no_hop }, std::invalid_argument);

        IntervalExtractorConfig tight_cap;
        tight_cap.max_run = tight_cap.samples_per_score + 2 * tight_cap.padding;
        EXPECT_THROW(MatrixIntervalExtractor { tight_cap }, std::invalid_argument);

        IntervalExtractorConfig not_finite;
        not_finite.enter_threshold = std::numeric_limits<double>::quiet_NaN();
        EXPECT_THROW(MatrixIntervalExtractor { not_finite }, std::invalid_argument);

        IntervalExtractorConfig capped;
        capped.max_run = capped.samples_per_score + 2 * capped.padding + 1;
        EXPECT_NO_THROW(MatrixIntervalExtractor { capped });

        IntervalExtractorConfig bad_gate;
        bad_gate.gate_threshold = std::numeric_limits<double>::quiet_NaN();
        EXPECT_THROW(MatrixIntervalExtractor { bad_gate }, std::invalid_argument);
    }

} // namespace
} // namespace MayaFlux::Test
