#include "gtest/gtest.h"

#include <filesystem>
#include <fstream>
#include <sstream>

#include "../../support/gpu_dispatch_test_support.hpp"

#include "MayaFlux/Yantra/Extractors/IntervalExtractor.hpp"

namespace MayaFlux::Test {
namespace {

    namespace fs = std::filesystem;

    using namespace GpuDispatch;
    using Yantra::ChannelIntervals;
    using Yantra::interval_regions;
    using Yantra::IntervalExtractorConfig;
    using Yantra::MatrixIntervalExtractor;
    using Yantra::StandardIntervalExtractor;

    using Intervals = std::vector<std::pair<size_t, size_t>>;

    const fs::path k_reference = "tests/yantra/gpu/interval_reference/interval_reference.txt";

    struct ReferenceCase {
        std::string name;
        IntervalExtractorConfig config;
        std::vector<double> scores;
        Intervals expect;
    };

    std::vector<ReferenceCase> load_cases()
    {
        std::vector<ReferenceCase> cases;
        std::ifstream file(k_reference);
        std::string line;
        while (std::getline(file, line)) {
            std::istringstream in(line);
            std::string key;
            in >> key;
            if (key == "case") {
                cases.emplace_back();
                in >> cases.back().name;
            } else if (key == "params") {
                auto& config = cases.back().config;
                int longest = 0;
                in >> config.enter_threshold >> config.min_run >> config.min_gap >> config.padding
                    >> config.gap_at_limit >> config.max_run >> longest >> config.length;
                config.use_longest_gap = longest != 0;
            } else if (key == "scores") {
                float value = 0.0F;
                while (in >> value) {
                    cases.back().scores.push_back(static_cast<double>(value));
                }
            } else if (key == "expect") {
                size_t start = 0;
                size_t end = 0;
                while (in >> start >> end) {
                    cases.back().expect.emplace_back(start, end);
                }
            }
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

    template <typename Fixture>
    class IntervalExtractorGpuTest : public Fixture { };

    using HardwareOptions = ::testing::Types<GpuDispatchFixture<true>, GpuDispatchFixture<false>>;
    TYPED_TEST_SUITE(IntervalExtractorGpuTest, HardwareOptions);

    TYPED_TEST(IntervalExtractorGpuTest, EveryReferenceCaseMatchesTheOriginalTimestamps)
    {
        const auto cases = load_cases();
        if (cases.empty()) {
            GTEST_SKIP() << k_reference.string() << " is not present";
        }
        for (const auto& c : cases) {
            EXPECT_EQ(whole(c.config, c.scores), c.expect) << c.name;
        }
    }

    TYPED_TEST(IntervalExtractorGpuTest, ASplitStreamGivesTheSameIntervalsAsOneCall)
    {
        const auto cases = load_cases();
        if (cases.empty()) {
            GTEST_SKIP() << k_reference.string() << " is not present";
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
    }

} // namespace
} // namespace MayaFlux::Test
