#include "gtest/gtest.h"

#include <random>

#include "../../support/gpu_dispatch_test_support.hpp"

#include "MayaFlux/Kinesis/Discrete/Analysis.hpp"
#include "MayaFlux/Kinesis/Stochastic/Estimate.hpp"
#include "MayaFlux/Yantra/Executors/Audio/LaggedPair.hpp"

namespace MayaFlux::Test {
namespace {

    using namespace GpuDispatch;
    using Portal::Graphics::PairMap;
    using Yantra::lagged_pair_spec;
    using Yantra::lagged_pair_stage;
    using Yantra::WordPairExecutionContext;
    using Yantra::SegmentGeometry;

    namespace D = Kinesis::Discrete;

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

    void expect_close(double got, double want)
    {
        EXPECT_NEAR(got, want, 1.0e-11 * (1.0 + std::abs(want)));
    }

    double host_sum(std::span<const double> window, size_t lag, PairMap map, bool centred)
    {
        double mean = 0.0;
        if (centred) {
            for (const double v : window) {
                mean += v;
            }
            mean /= static_cast<double>(window.size());
        }
        double total = 0.0;
        for (size_t i = 0; i + lag < window.size(); ++i) {
            const double a = window[i] - mean;
            const double b = window[i + lag] - mean;
            switch (map) {
            case PairMap::SignChange:
                total += ((a >= 0.0) != (b >= 0.0)) ? 1.0 : 0.0;
                break;
            case PairMap::AbsDifference:
                total += std::abs(a - b);
                break;
            case PairMap::Product:
            default:
                total += a * b;
                break;
            }
        }
        return total;
    }

    std::vector<double> host_sums(std::span<const double> values, const SegmentGeometry& geometry,
        uint32_t first_lag, uint32_t lag_count, PairMap map, bool centred)
    {
        std::vector<double> out;
        for (uint32_t w = 0; w < geometry.count; ++w) {
            const std::span<const double> window = values.subspan(geometry.offset + static_cast<size_t>(w) * geometry.stride, geometry.length);
            for (uint32_t l = 0; l < lag_count; ++l) {
                out.push_back(host_sum(window, first_lag + l, map, centred));
            }
        }
        return out;
    }

    struct Pass {
        std::unique_ptr<WordPairExecutionContext> context;
        Yantra::GpuComputeConfig config;
    };

    Pass make_pass(PairMap map, bool centred)
    {
        const auto spec = lagged_pair_spec(map, centred);
        auto config = Yantra::config_from_spec(spec);
        auto context = std::make_unique<WordPairExecutionContext>(config, Yantra::bindings_from_spec(spec), false, "LaggedPair");
        return { std::move(context), std::move(config) };
    }

    std::vector<double> sums(Pass& pass, std::span<const double> values, const SegmentGeometry& geometry,
        uint32_t first_lag, uint32_t lag_count)
    {
        if (geometry.count == 0 || lag_count == 0) {
            return {};
        }
        pass.context->record_stages({ lagged_pair_stage(*pass.context, pass.config, values, geometry, first_lag, lag_count) });
        return pass.context->download_words(1, static_cast<size_t>(geometry.count) * lag_count);
    }

    template <typename Fixture>
    class LaggedPairGpuTest : public Fixture { };

    using HardwareOptions = ::testing::Types<GpuDispatchFixture<true>, GpuDispatchFixture<false>>;
    TYPED_TEST_SUITE(LaggedPairGpuTest, HardwareOptions);

    TYPED_TEST(LaggedPairGpuTest, ProductMatchesAHostSumOverOverlappingWindows)
    {
        const auto signal = random_signal(2000, 3);
        const SegmentGeometry geometry { .length = 300, .stride = 128, .offset = 0, .count = 14 };
        auto context = make_pass(PairMap::Product, false);

        const auto got = sums(context, signal, geometry, 5, 40);

        const auto want = host_sums(signal, geometry, 5, 40, PairMap::Product, false);
        ASSERT_EQ(got.size(), want.size());
        for (size_t i = 0; i < want.size(); ++i) {
            expect_close(got[i], want[i]);
        }
    }

    TYPED_TEST(LaggedPairGpuTest, CentredProductMatchesAHostSumOfMeanRemovedWindows)
    {
        auto signal = random_signal(1500, 5);
        for (auto& value : signal) {
            value += 3.0;
        }
        const SegmentGeometry geometry { .length = 256, .stride = 256, .offset = 0, .count = 5 };
        auto context = make_pass(PairMap::Product, true);

        const auto got = sums(context, signal, geometry, 0, 60);

        const auto want = host_sums(signal, geometry, 0, 60, PairMap::Product, true);
        ASSERT_EQ(got.size(), want.size());
        for (size_t i = 0; i < want.size(); ++i) {
            expect_close(got[i], want[i]);
        }
    }

    TYPED_TEST(LaggedPairGpuTest, SignChangeAtLagOneIsTheKinesisCrossingCount)
    {
        auto signal = random_signal(1200, 9);
        signal[10] = 0.0;
        signal[11] = -0.0;
        signal[12] = 0.5;
        constexpr uint32_t window = 200;
        constexpr uint32_t hop = 100;
        const size_t windows = D::num_windows(signal.size(), window, hop);
        auto context = make_pass(PairMap::SignChange, false);

        const auto got = sums(context, signal, { .length = window, .stride = hop, .offset = 0, .count = static_cast<uint32_t>(windows) }, 1, 1);

        const auto rate = D::zero_crossing_rate(signal, windows, hop, window);
        ASSERT_EQ(got.size(), windows);
        for (size_t w = 0; w < windows; ++w) {
            EXPECT_EQ(got[w], std::round(rate[w] * (window - 1)));
        }
    }

    TYPED_TEST(LaggedPairGpuTest, AbsDifferenceMatchesAHostSum)
    {
        const auto signal = random_signal(900, 13);
        const SegmentGeometry geometry { .length = 150, .stride = 150, .offset = 0, .count = 6 };
        auto context = make_pass(PairMap::AbsDifference, false);

        const auto got = sums(context, signal, geometry, 1, 30);

        const auto want = host_sums(signal, geometry, 1, 30, PairMap::AbsDifference, false);
        ASSERT_EQ(got.size(), want.size());
        for (size_t i = 0; i < want.size(); ++i) {
            expect_close(got[i], want[i]);
        }
    }

    TYPED_TEST(LaggedPairGpuTest, ALagAtOrBeyondTheWindowSumsNothing)
    {
        const auto signal = random_signal(64, 17);
        auto context = make_pass(PairMap::Product, false);

        const auto got = sums(context, signal, { .length = 8, .stride = 8, .offset = 0, .count = 2 }, 6, 5);

        ASSERT_EQ(got.size(), 10U);
        for (uint32_t w = 0; w < 2; ++w) {
            const std::span<const double> window(signal.data() + w * 8, 8);
            expect_close(got[w * 5], host_sum(window, 6, PairMap::Product, false));
            expect_close(got[w * 5 + 1], host_sum(window, 7, PairMap::Product, false));
            EXPECT_EQ(got[w * 5 + 2], 0.0);
            EXPECT_EQ(got[w * 5 + 3], 0.0);
            EXPECT_EQ(got[w * 5 + 4], 0.0);
        }
    }

    TYPED_TEST(LaggedPairGpuTest, TheGeometryOffsetAndAShortWindowAreRespected)
    {
        const auto signal = random_signal(100, 21);
        const SegmentGeometry geometry { .length = 5, .stride = 11, .offset = 7, .count = 6 };
        auto context = make_pass(PairMap::Product, true);

        const auto got = sums(context, signal, geometry, 0, 3);

        const auto want = host_sums(signal, geometry, 0, 3, PairMap::Product, true);
        ASSERT_EQ(got.size(), want.size());
        for (size_t i = 0; i < want.size(); ++i) {
            expect_close(got[i], want[i]);
        }
    }

    TYPED_TEST(LaggedPairGpuTest, NoWindowsOrNoLagsGiveNothing)
    {
        const auto signal = random_signal(64, 23);
        auto context = make_pass(PairMap::Product, false);

        EXPECT_TRUE(sums(context, signal, { .length = 8, .stride = 8, .offset = 0, .count = 0 }, 0, 4).empty());
        EXPECT_TRUE(sums(context, signal, { .length = 8, .stride = 8, .offset = 0, .count = 4 }, 0, 0).empty());
    }

    TYPED_TEST(LaggedPairGpuTest, CentredProductSelectsTheLagsOfKinesisPeriodicity)
    {
        std::vector<double> signal = random_signal(4096, 29);
        for (size_t i = 0; i < signal.size(); ++i) {
            const double period = i < 2048 ? 45.0 : 73.0;
            signal[i] = 0.3 * signal[i] + std::sin(2.0 * std::numbers::pi * static_cast<double>(i) / period);
        }
        constexpr uint32_t window = 512;
        constexpr uint32_t hop = 256;
        constexpr uint32_t first_lag = 20;
        constexpr uint32_t last_lag = 200;
        constexpr uint32_t lag_count = last_lag - first_lag + 1;
        const size_t windows = D::num_windows(signal.size(), window, hop);
        auto context = make_pass(PairMap::Product, true);

        const auto row_sums = sums(context, signal, { .length = window, .stride = hop, .offset = 0, .count = static_cast<uint32_t>(windows) }, first_lag, lag_count);

        const auto [strengths, lags] = D::periodicity(signal, windows, hop, window, first_lag, last_lag);
        ASSERT_EQ(row_sums.size(), windows * lag_count);
        for (size_t w = 0; w < windows; ++w) {
            const std::span<const double> view(signal.data() + w * hop, window);
            const double denominator = Kinesis::Stochastic::Estimate::variance(view) * static_cast<double>(window - 1);
            double best = 0.0;
            size_t best_lag = 0;
            for (uint32_t l = 0; l < lag_count; ++l) {
                const double correlation = row_sums[w * lag_count + l] / denominator;
                if (correlation > best) {
                    best = correlation;
                    best_lag = first_lag + l;
                }
            }
            EXPECT_EQ(best_lag, lags[w]);
            EXPECT_NEAR(best, strengths[w], 1.0e-9);
        }
    }

}
}
