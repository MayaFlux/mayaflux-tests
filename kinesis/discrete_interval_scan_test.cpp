#include "gtest/gtest.h"

#include "../support/interval_reference_support.hpp"

#include "MayaFlux/Kinesis/Discrete/IntervalScan.hpp"

namespace MayaFlux::Test {
namespace {

    namespace D = Kinesis::Discrete;
    namespace Ref = IntervalReference;

    using Intervals = Ref::Intervals;

    struct ScannerReferenceCase {
        std::string name;
        D::IntervalScanConfig config;
        std::vector<double> scores;
        Intervals expect;
    };

    std::vector<ScannerReferenceCase> load_scanner_cases()
    {
        std::vector<ScannerReferenceCase> cases;
        for (auto& c : Ref::load()) {
            D::IntervalScanConfig config;
            config.enter = c.enter;
            config.leave = std::max(c.enter - 0.15, 0.01);
            config.hop = 512;
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

    D::IntervalScanConfig small_scan()
    {
        D::IntervalScanConfig config;
        config.hop = 10;
        config.min_run = 15;
        config.min_gap = 20;
        config.padding = 5;
        config.gap_at_limit = 0;
        return config;
    }

    Intervals scan(const D::IntervalScanConfig& config, const std::vector<double>& scores, const std::vector<double>& gate = {})
    {
        D::IntervalScanner scanner(config);
        auto out = scanner.push(scores, gate);
        const auto rest = scanner.flush();
        out.insert(out.end(), rest.begin(), rest.end());
        return out;
    }

    TEST(IntervalScannerTest, EveryReferenceCaseMatchesTheOriginalTimestamps)
    {
        const auto cases = load_scanner_cases();
        if (cases.empty()) {
            GTEST_SKIP() << Ref::fixture_path().string() << " is not present";
        }
        for (const auto& c : cases) {
            EXPECT_EQ(scan(c.config, c.scores), c.expect) << c.name;
        }
    }

    TEST(IntervalScannerTest, ASplitStreamGivesTheSameIntervalsAsOneCall)
    {
        const auto cases = load_scanner_cases();
        if (cases.empty()) {
            GTEST_SKIP() << Ref::fixture_path().string() << " is not present";
        }
        constexpr std::array<size_t, 5> sizes { 1, 7, 64, 2, 333 };
        for (const auto& c : cases) {
            D::IntervalScanner scanner(c.config);
            Intervals joined;
            size_t at = 0;
            size_t step = 0;
            while (at < c.scores.size()) {
                const size_t count = std::min(sizes[step++ % sizes.size()], c.scores.size() - at);
                const auto got = scanner.push(std::span<const double>(c.scores).subspan(at, count));
                joined.insert(joined.end(), got.begin(), got.end());
                at += count;
            }
            const auto rest = scanner.flush();
            joined.insert(joined.end(), rest.begin(), rest.end());
            EXPECT_EQ(joined, c.expect) << c.name;
        }
    }

    TEST(IntervalScannerTest, ARunIsPaddedAndReleasedByTheFlush)
    {
        auto config = small_scan();
        config.length = 100;

        EXPECT_EQ(scan(config, { 0, 0, 1, 1, 1, 0, 0, 0, 0, 0 }), (Intervals { { 15, 55 } }));
    }

    TEST(IntervalScannerTest, TheNewestIntervalWaitsForTheFlush)
    {
        auto config = small_scan();
        config.length = 120;
        D::IntervalScanner scanner(config);

        EXPECT_TRUE(scanner.push(std::vector<double> { 0, 0, 1, 1, 1, 0, 0, 0, 0, 0 }).empty());
        EXPECT_EQ(scanner.flush(), (Intervals { { 15, 55 } }));
    }

    TEST(IntervalScannerTest, AFlushEndsTheStreamAndTheNextPushStartsANewOne)
    {
        auto config = small_scan();
        config.length = 100;
        const std::vector<double> scores { 0, 0, 1, 1, 1, 0, 0, 0, 0, 0 };

        D::IntervalScanner scanner(config);
        EXPECT_TRUE(scanner.push(scores).empty());
        const auto first = scanner.flush();
        EXPECT_TRUE(scanner.push(scores).empty());
        const auto second = scanner.flush();

        EXPECT_EQ(first, (Intervals { { 15, 55 } }));
        EXPECT_EQ(second, first);
    }

    TEST(IntervalScannerTest, ResetForgetsAnOpenRun)
    {
        auto config = small_scan();
        config.length = 100;
        D::IntervalScanner scanner(config);
        EXPECT_TRUE(scanner.push(std::vector<double> { 0, 0, 1, 1, 1 }).empty());

        scanner.reset();

        EXPECT_TRUE(scanner.flush().empty());
        EXPECT_TRUE(scanner.push(std::vector<double> { 0, 0, 1, 1, 1, 0, 0, 0, 0, 0 }).empty());
        EXPECT_EQ(scanner.flush(), (Intervals { { 15, 55 } }));
    }

    TEST(IntervalScannerTest, TheFirstSampleMovesEveryPositionAndTheClamp)
    {
        auto config = small_scan();
        config.first_sample = 5;
        config.length = 120;

        EXPECT_EQ(scan(config, { 0, 0, 1, 1, 1, 0, 0, 0, 0, 0 }), (Intervals { { 20, 60 } }));
    }

    TEST(IntervalScannerTest, TheFirstSampleCountsInTheEndOfTheScoresWhenNoLengthIsSet)
    {
        auto config = small_scan();
        config.first_sample = 5;
        config.padding = 100;

        EXPECT_EQ(scan(config, { 0, 0, 1, 1, 1, 1, 1, 1 }), (Intervals { { 0, 85 } }));
    }

    TEST(IntervalScannerTest, AGateBelowItsThresholdEndsARunAtOnce)
    {
        auto config = small_scan();
        config.padding = 0;
        config.length = 120;
        const std::vector<double> scores { 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0 };
        const std::vector<double> gate { 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1 };

        EXPECT_EQ(scan(config, scores), (Intervals { { 10, 90 } }));
        EXPECT_EQ(scan(config, scores, gate), (Intervals { { 10, 50 }, { 60, 90 } }));
    }

    TEST(IntervalScannerTest, AGapIsBridgedOnlyWhileTheGateStaysHigh)
    {
        auto config = small_scan();
        config.padding = 0;
        config.length = 140;
        const std::vector<double> scores { 0, 1, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0 };
        const std::vector<double> open { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
        const std::vector<double> broken { 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1 };

        EXPECT_EQ(scan(config, scores, open), (Intervals { { 10, 100 } }));
        EXPECT_EQ(scan(config, scores, broken), (Intervals { { 10, 40 }, { 60, 100 } }));
    }

    TEST(IntervalScannerTest, ALowGateIsNeverEngaged)
    {
        auto config = small_scan();
        config.padding = 0;
        config.length = 100;
        const std::vector<double> scores { 1, 1, 1, 1, 1, 1, 0, 0, 0, 0 };
        const std::vector<double> gate { 0, 0, 1, 1, 1, 1, 1, 1, 1, 1 };

        EXPECT_EQ(scan(config, scores), (Intervals { { 0, 60 } }));
        EXPECT_EQ(scan(config, scores, gate), (Intervals { { 20, 60 } }));
    }

    TEST(IntervalScannerTest, ARunEndedByTheGateIsStillPaddedAgainstItsNeighbour)
    {
        auto config = small_scan();
        config.length = 120;
        const std::vector<double> scores { 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0 };
        const std::vector<double> gate { 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1 };

        EXPECT_EQ(scan(config, scores, gate), (Intervals { { 5, 55 }, { 55, 95 } }));
    }

    TEST(IntervalScannerTest, ARunShorterThanTheMinimumIsDroppedWhenTheGateEndsIt)
    {
        auto config = small_scan();
        config.length = 100;
        const std::vector<double> scores { 0, 1, 1, 1, 1, 1, 0, 0, 0, 0 };
        const std::vector<double> gate { 1, 1, 0, 1, 1, 1, 1, 1, 1, 1 };

        EXPECT_EQ(scan(config, scores, gate), (Intervals { { 25, 65 } }));
    }

    TEST(IntervalScannerTest, ASplitGatedStreamGivesTheSameIntervalsAsOneCall)
    {
        std::vector<double> scores;
        std::vector<double> gate;
        Ref::random_gated(11, 900, scores, gate);

        auto config = small_scan();
        config.length = 10000;
        const auto expected = scan(config, scores, gate);
        EXPECT_FALSE(expected.empty());

        D::IntervalScanner scanner(config);
        Intervals joined;
        for (size_t at = 0; at < scores.size(); at += 37) {
            const size_t count = std::min<size_t>(37, scores.size() - at);
            const auto got = scanner.push(std::span<const double>(scores).subspan(at, count),
                std::span<const double>(gate).subspan(at, count));
            joined.insert(joined.end(), got.begin(), got.end());
        }
        const auto rest = scanner.flush();
        joined.insert(joined.end(), rest.begin(), rest.end());

        EXPECT_EQ(joined, expected);
    }

    TEST(IntervalScannerTest, ARunInProgressIsReportedBeforeItCloses)
    {
        auto config = small_scan();
        config.length = 200;
        D::IntervalScanner scanner(config);

        EXPECT_FALSE(scanner.open_start().has_value());
        EXPECT_TRUE(scanner.push(std::vector<double> { 0, 0, 1, 1, 1 }).empty());
        EXPECT_EQ(scanner.open_start(), std::optional<size_t> { 20 });
        EXPECT_FALSE(scanner.pending().has_value());

        EXPECT_TRUE(scanner.push(std::vector<double> { 1, 1 }).empty());
        EXPECT_EQ(scanner.open_start(), std::optional<size_t> { 20 });

        EXPECT_TRUE(scanner.push(std::vector<double> { 0, 0, 0 }).empty());
        EXPECT_FALSE(scanner.open_start().has_value());
        EXPECT_EQ(scanner.pending(), (std::optional<std::pair<size_t, size_t>> { { 15, 70 } }));

        EXPECT_EQ(scanner.flush(), (Intervals { { 15, 75 } }));
        EXPECT_FALSE(scanner.pending().has_value());
    }

    TEST(IntervalScannerTest, ARunStaysOpenThroughAGapShorterThanTheMinimum)
    {
        auto config = small_scan();
        config.length = 200;
        D::IntervalScanner scanner(config);

        EXPECT_TRUE(scanner.push(std::vector<double> { 0, 1, 1, 0 }).empty());

        EXPECT_EQ(scanner.open_start(), std::optional<size_t> { 10 });
    }

    TEST(IntervalScannerTest, EmitOnCloseReleasesAnIntervalAtOnceWithFullPaddingOnBothSides)
    {
        auto config = small_scan();
        config.padding = 30;
        config.length = 200;
        config.emit_on_close = true;
        D::IntervalScanner scanner(config);

        const auto got = scanner.push(std::vector<double> { 0, 0, 1, 1, 1, 0, 0, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 });

        EXPECT_EQ(got, (Intervals { { 0, 80 }, { 70, 160 } }));
        EXPECT_FALSE(scanner.pending().has_value());
        EXPECT_TRUE(scanner.flush().empty());
    }

    TEST(IntervalScannerTest, EmitOnCloseWaitsForTheTrailingGapOnly)
    {
        auto config = small_scan();
        config.length = 200;
        config.emit_on_close = true;
        D::IntervalScanner scanner(config);

        EXPECT_TRUE(scanner.push(std::vector<double> { 0, 0, 1, 1, 1, 0, 0 }).empty());
        EXPECT_EQ(scanner.push(std::vector<double> { 0 }), (Intervals { { 15, 55 } }));
    }

    TEST(IntervalScannerTest, EmitOnCloseClampsThePaddedEndToTheScoresSeenWhenNoLengthIsSet)
    {
        auto config = small_scan();
        config.padding = 100;
        config.emit_on_close = true;
        D::IntervalScanner scanner(config);

        EXPECT_EQ(scanner.push(std::vector<double> { 0, 0, 1, 1, 1, 0, 0, 0 }), (Intervals { { 0, 80 } }));
    }

    TEST(IntervalScannerTest, InvalidSettingsAreRefused)
    {
        D::IntervalScanConfig no_hop;
        no_hop.hop = 0;
        EXPECT_THROW(D::IntervalScanner { no_hop }, std::invalid_argument);

        D::IntervalScanConfig tight_cap;
        tight_cap.max_run = tight_cap.hop + 2 * tight_cap.padding;
        EXPECT_THROW(D::IntervalScanner { tight_cap }, std::invalid_argument);

        D::IntervalScanConfig not_finite;
        not_finite.gate_threshold = std::numeric_limits<double>::infinity();
        EXPECT_THROW(D::IntervalScanner { not_finite }, std::invalid_argument);

        D::IntervalScanConfig capped;
        capped.max_run = capped.hop + 2 * capped.padding + 1;
        EXPECT_NO_THROW(D::IntervalScanner { capped });
    }

    TEST(IntervalScannerTest, AGateOfAnotherLengthIsRefused)
    {
        D::IntervalScanner scanner(small_scan());

        EXPECT_THROW(std::ignore = scanner.push(std::vector<double> { 0, 1, 1 }, std::vector<double> { 1, 1 }), std::invalid_argument);
    }

} // namespace
} // namespace MayaFlux::Test
