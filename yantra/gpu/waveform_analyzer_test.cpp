#include "gtest/gtest.h"

#include <random>

#include "../../support/gpu_dispatch_test_support.hpp"

#include "MayaFlux/Kinesis/Discrete/Analysis.hpp"
#include "MayaFlux/Yantra/Analyzers/WaveformAnalyzer.hpp"

namespace MayaFlux::Test {
namespace {

    using namespace GpuDispatch;
    using Yantra::StandardWaveformAnalyzer;
    using Yantra::WaveformAnalyzerConfig;
    using Yantra::WaveformMethod;

    namespace D = Kinesis::Discrete;

    constexpr double k_rate = 16000.0;

    WaveformAnalyzerConfig make_config(WaveformMethod method = WaveformMethod::LEVEL, uint32_t window = 512, uint32_t hop = 256)
    {
        WaveformAnalyzerConfig config;
        config.method = method;
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

    std::vector<double> noise(size_t count, double amplitude, uint64_t seed)
    {
        std::mt19937_64 engine(seed);
        std::uniform_real_distribution<double> distribution(-amplitude, amplitude);
        std::vector<double> out(count);
        for (auto& value : out) {
            value = distribution(engine);
        }
        return out;
    }

    std::vector<double> pure_tone(size_t count, double period)
    {
        std::vector<double> out(count);
        for (size_t i = 0; i < count; ++i) {
            out[i] = std::sin(2.0 * std::numbers::pi * static_cast<double>(i) / period);
        }
        return out;
    }

    std::vector<double> harmonic_stack(size_t count, double period, double amplitude)
    {
        std::vector<double> out(count);
        for (size_t i = 0; i < count; ++i) {
            double sum = 0.0;
            for (int h = 1; h <= 8; ++h) {
                sum += std::sin(2.0 * std::numbers::pi * h * static_cast<double>(i) / period) / h;
            }
            out[i] = amplitude * sum;
        }
        return out;
    }

    void append(std::vector<double>& to, const std::vector<double>& from)
    {
        to.insert(to.end(), from.begin(), from.end());
    }

    std::vector<double> voice_like()
    {
        std::vector<double> signal;
        append(signal, harmonic_stack(8192, 100.0, 0.8));
        append(signal, std::vector<double>(4096, 0.0));
        append(signal, noise(8192, 0.3, 7));
        append(signal, harmonic_stack(8192, 160.0, 0.4));
        return signal;
    }

    void expect_close(double got, double want)
    {
        EXPECT_NEAR(got, want, 1.0e-11 * (1.0 + std::abs(want)));
    }

    template <typename Fixture>
    class WaveformAnalyzerGpuTest : public Fixture { };

    using HardwareOptions = ::testing::Types<GpuDispatchFixture<true>, GpuDispatchFixture<false>>;
    TYPED_TEST_SUITE(WaveformAnalyzerGpuTest, HardwareOptions);

    TYPED_TEST(WaveformAnalyzerGpuTest, LevelMatchesTheKinesisRms)
    {
        const auto signal = noise(5000, 1.0, 3);
        StandardWaveformAnalyzer analyzer(make_config(WaveformMethod::LEVEL));

        const auto analysis = analyzer.analyze_waveform(input_of({ signal }));

        const size_t windows = D::num_windows(signal.size(), 512, 256);
        const auto want = D::rms(signal, windows, 256, 512);
        ASSERT_EQ(analysis.channels.size(), 1U);
        ASSERT_EQ(analysis.channels[0].values.size(), windows);
        for (size_t w = 0; w < windows; ++w) {
            expect_close(analysis.channels[0].values[w], want[w]);
            expect_close(analysis.channels[0].level[w], want[w]);
        }
    }

    TYPED_TEST(WaveformAnalyzerGpuTest, CrossingRateMatchesTheKinesisRateExactly)
    {
        const auto signal = noise(5000, 1.0, 5);
        StandardWaveformAnalyzer analyzer(make_config(WaveformMethod::CROSSING_RATE));

        const auto analysis = analyzer.analyze_waveform(input_of({ signal }));

        const size_t windows = D::num_windows(signal.size(), 512, 256);
        const auto want = D::zero_crossing_rate(signal, windows, 256, 512);
        ASSERT_EQ(analysis.channels[0].values.size(), windows);
        for (size_t w = 0; w < windows; ++w) {
            EXPECT_EQ(analysis.channels[0].values[w], want[w]);
        }
    }

    TYPED_TEST(WaveformAnalyzerGpuTest, PeriodAndPeriodicityMatchKinesisPeriodicity)
    {
        const auto signal = voice_like();
        StandardWaveformAnalyzer analyzer(make_config(WaveformMethod::PERIOD));

        const auto analysis = analyzer.analyze_waveform(input_of({ signal }));

        const size_t windows = D::num_windows(signal.size(), 512, 256);
        const auto [strengths, lags] = D::periodicity(signal, windows, 256, 512, 40, 228);
        const auto& channel = analysis.channels[0];
        ASSERT_EQ(channel.period.size(), windows);
        for (size_t w = 0; w < windows; ++w) {
            EXPECT_EQ(channel.period[w], static_cast<double>(lags[w]));
            EXPECT_EQ(channel.values[w], channel.period[w]);
            EXPECT_NEAR(channel.periodicity[w], strengths[w], 1.0e-9);
            EXPECT_NEAR(channel.frequency[w], lags[w] > 0 ? k_rate / static_cast<double>(lags[w]) : 0.0, 1.0e-9);
        }
    }

    TYPED_TEST(WaveformAnalyzerGpuTest, AHopThatDoesNotDivide512MatchesKinesisPeriodicity)
    {
        const auto signal = voice_like();
        StandardWaveformAnalyzer analyzer(make_config(WaveformMethod::PERIOD, 512, 100));

        const auto analysis = analyzer.analyze_waveform(input_of({ signal }));

        const size_t windows = D::num_windows(signal.size(), 512, 100);
        const auto [strengths, lags] = D::periodicity(signal, windows, 100, 512, 40, 228);
        const auto& channel = analysis.channels[0];
        ASSERT_EQ(channel.period.size(), windows);
        for (size_t w = 0; w < windows; ++w) {
            EXPECT_EQ(channel.period[w], static_cast<double>(lags[w]));
            EXPECT_NEAR(channel.periodicity[w], strengths[w], 1.0e-9);
            EXPECT_EQ(channel.window_positions[w].first, w * 100);
        }
    }

    TYPED_TEST(WaveformAnalyzerGpuTest, KeepingProfilesDoesNotChangeThePeriodOrTheStrength)
    {
        const auto signal = voice_like();
        auto kept = make_config(WaveformMethod::PERIODICITY);
        kept.keep_profiles = true;
        StandardWaveformAnalyzer without(make_config(WaveformMethod::PERIODICITY));
        StandardWaveformAnalyzer with(kept);

        const auto a = without.analyze_waveform(input_of({ signal })).channels[0];
        const auto b = with.analyze_waveform(input_of({ signal })).channels[0];

        ASSERT_EQ(a.period.size(), b.period.size());
        EXPECT_EQ(a.period, b.period);
        EXPECT_EQ(a.periodicity, b.periodicity);
        EXPECT_EQ(a.harmonicity, b.harmonicity);
    }

    TYPED_TEST(WaveformAnalyzerGpuTest, UnbiasedStrengthDoesNotFallWithThePeriod)
    {
        auto biased_config = make_config(WaveformMethod::PERIODICITY);
        auto unbiased_config = biased_config;
        unbiased_config.unbiased_periodicity = true;
        StandardWaveformAnalyzer biased(biased_config);
        StandardWaveformAnalyzer unbiased(unbiased_config);

        double biased_mean[2] {};
        int slot = 0;
        for (const double period : { 80.0, 160.0 }) {
            const auto signal = pure_tone(4096, period);
            const auto plain = biased.analyze_waveform(input_of({ signal })).channels[0];
            const auto corrected = unbiased.analyze_waveform(input_of({ signal })).channels[0];

            ASSERT_EQ(plain.periodicity.size(), corrected.periodicity.size());
            for (size_t w = 0; w < plain.periodicity.size(); ++w) {
                EXPECT_EQ(corrected.period[w], plain.period[w]) << period;
                EXPECT_GT(corrected.periodicity[w], plain.periodicity[w]) << period;
                EXPECT_NEAR(corrected.periodicity[w], 1.0, 0.1) << period;
                biased_mean[slot] += plain.periodicity[w] / static_cast<double>(plain.periodicity.size());
            }
            ++slot;
        }
        EXPECT_GT(biased_mean[0] - biased_mean[1], 0.1);
    }

    TYPED_TEST(WaveformAnalyzerGpuTest, UnbiasedStrengthIsTheChosenLagsStrengthOverItsShareOfPairs)
    {
        const auto signal = voice_like();
        auto unbiased_config = make_config(WaveformMethod::PERIODICITY);
        unbiased_config.unbiased_periodicity = true;
        StandardWaveformAnalyzer biased(make_config(WaveformMethod::PERIODICITY));
        StandardWaveformAnalyzer unbiased(unbiased_config);

        const auto plain = biased.analyze_waveform(input_of({ signal })).channels[0];
        const auto corrected = unbiased.analyze_waveform(input_of({ signal })).channels[0];

        ASSERT_EQ(plain.period.size(), corrected.period.size());
        EXPECT_EQ(plain.period, corrected.period);
        for (size_t w = 0; w < plain.period.size(); ++w) {
            const double lag = plain.period[w];
            const double share = lag > 0.0 ? 512.0 / (512.0 - lag) : 1.0;
            EXPECT_NEAR(corrected.periodicity[w], plain.periodicity[w] * share, 1.0e-12) << w;
        }
    }

    TYPED_TEST(WaveformAnalyzerGpuTest, KnownSegmentsReportTheirFundamentalSilenceAndNoise)
    {
        StandardWaveformAnalyzer analyzer(make_config(WaveformMethod::PERIOD));

        const auto analysis = analyzer.analyze_waveform(input_of({ voice_like() }));

        const auto& channel = analysis.channels[0];
        ASSERT_EQ(channel.period.size(), 111U);
        for (size_t w = 0; w <= 30; ++w) {
            EXPECT_EQ(channel.period[w], 100.0);
            EXPECT_NEAR(channel.frequency[w], 160.0, 1.0e-9);
            EXPECT_GT(channel.periodicity[w], 0.6);
        }
        for (size_t w = 32; w <= 46; ++w) {
            EXPECT_EQ(channel.period[w], 0.0);
            EXPECT_EQ(channel.periodicity[w], 0.0);
            EXPECT_EQ(channel.level[w], 0.0);
        }
        for (size_t w = 48; w <= 78; ++w) {
            EXPECT_LT(channel.periodicity[w], 0.3);
        }
        for (size_t w = 80; w <= 110; ++w) {
            EXPECT_EQ(channel.period[w], 160.0);
            EXPECT_NEAR(channel.frequency[w], 100.0, 1.0e-9);
            EXPECT_GT(channel.periodicity[w], 0.5);
        }
    }

    TYPED_TEST(WaveformAnalyzerGpuTest, TheMethodSelectsTheOutputDataAndEveryDescriptorIsAlwaysPresent)
    {
        const auto signal = voice_like();
        const std::vector<Kakshya::DataVariant> input = input_of({ signal });

        for (const auto method : { WaveformMethod::LEVEL, WaveformMethod::CROSSING_RATE, WaveformMethod::PERIODICITY,
                 WaveformMethod::PERIOD, WaveformMethod::FREQUENCY, WaveformMethod::PEAK, WaveformMethod::DC_OFFSET,
                 WaveformMethod::CREST_FACTOR, WaveformMethod::HARMONICITY, WaveformMethod::LEVEL_CHANGE,
                 WaveformMethod::PERIOD_CHANGE }) {
            StandardWaveformAnalyzer analyzer(make_config(method));
            const auto channel = analyzer.analyze_waveform(input).channels[0];

            const std::vector<double>* expected = nullptr;
            switch (method) {
            case WaveformMethod::CROSSING_RATE:
                expected = &channel.crossing_rate;
                break;
            case WaveformMethod::PERIODICITY:
                expected = &channel.periodicity;
                break;
            case WaveformMethod::PERIOD:
                expected = &channel.period;
                break;
            case WaveformMethod::FREQUENCY:
                expected = &channel.frequency;
                break;
            case WaveformMethod::PEAK:
                expected = &channel.peak;
                break;
            case WaveformMethod::DC_OFFSET:
                expected = &channel.dc_offset;
                break;
            case WaveformMethod::CREST_FACTOR:
                expected = &channel.crest_factor;
                break;
            case WaveformMethod::HARMONICITY:
                expected = &channel.harmonicity;
                break;
            case WaveformMethod::LEVEL_CHANGE:
                expected = &channel.level_change;
                break;
            case WaveformMethod::PERIOD_CHANGE:
                expected = &channel.period_change;
                break;
            case WaveformMethod::LEVEL:
            default:
                expected = &channel.level;
                break;
            }
            EXPECT_EQ(channel.values, *expected);
            EXPECT_EQ(channel.level.size(), channel.frequency.size());
            EXPECT_EQ(channel.crossing_rate.size(), channel.window_positions.size());
        }
    }

    TYPED_TEST(WaveformAnalyzerGpuTest, ASilentChannelHasNothingToReport)
    {
        StandardWaveformAnalyzer analyzer(make_config(WaveformMethod::PERIODICITY));

        const auto channel = analyzer.analyze_waveform(input_of({ std::vector<double>(2048, 0.0) })).channels[0];

        ASSERT_FALSE(channel.level.empty());
        for (size_t w = 0; w < channel.level.size(); ++w) {
            EXPECT_EQ(channel.level[w], 0.0);
            EXPECT_EQ(channel.periodicity[w], 0.0);
            EXPECT_EQ(channel.period[w], 0.0);
            EXPECT_EQ(channel.frequency[w], 0.0);
        }
    }

    TYPED_TEST(WaveformAnalyzerGpuTest, AChannelShorterThanOneWindowGivesNoWindows)
    {
        StandardWaveformAnalyzer analyzer(make_config());

        const auto analysis = analyzer.analyze_waveform(input_of({ noise(300, 1.0, 11) }));

        ASSERT_EQ(analysis.channels.size(), 1U);
        EXPECT_TRUE(analysis.channels[0].values.empty());
        EXPECT_TRUE(analysis.channels[0].window_positions.empty());
    }

    TYPED_TEST(WaveformAnalyzerGpuTest, EveryChannelIsAnalysedOnItsOwn)
    {
        StandardWaveformAnalyzer analyzer(make_config(WaveformMethod::PERIOD));

        const auto analysis = analyzer.analyze_waveform(
            input_of({ harmonic_stack(4096, 90.0, 0.5), harmonic_stack(4096, 140.0, 0.5) }));

        ASSERT_EQ(analysis.channels.size(), 2U);
        for (const double period : analysis.channels[0].values) {
            EXPECT_NEAR(period, 90.0, 1.0);
        }
        for (const double period : analysis.channels[1].values) {
            EXPECT_NEAR(period, 140.0, 1.0);
        }
    }

    TYPED_TEST(WaveformAnalyzerGpuTest, WindowPositionsFollowTheHop)
    {
        StandardWaveformAnalyzer analyzer(make_config(WaveformMethod::LEVEL, 512, 128));

        const auto analysis = analyzer.analyze_waveform(input_of({ noise(1500, 1.0, 13) }));

        const auto& positions = analysis.channels[0].window_positions;
        ASSERT_EQ(positions.size(), 8U);
        for (size_t w = 0; w < positions.size(); ++w) {
            EXPECT_EQ(positions[w].first, w * 128);
            EXPECT_EQ(positions[w].second, w * 128 + 512);
        }
    }

    TYPED_TEST(WaveformAnalyzerGpuTest, ALongChannelAgreesWithKinesisAcrossTheChunkBoundary)
    {
        constexpr uint32_t hop = 128;
        constexpr size_t windows = 4101;
        std::vector<double> signal = harmonic_stack((windows - 1) * hop + 512, 90.0, 0.6);
        const auto grit = noise(signal.size(), 0.2, 17);
        for (size_t i = 0; i < signal.size(); ++i) {
            signal[i] += grit[i];
        }
        StandardWaveformAnalyzer analyzer(make_config(WaveformMethod::PERIOD, 512, hop));

        const auto channel = analyzer.analyze_waveform(input_of({ signal })).channels[0];

        const auto [strengths, lags] = D::periodicity(signal, windows, hop, 512, 40, 228);
        ASSERT_EQ(channel.period.size(), windows);
        for (size_t w = 0; w < windows; ++w) {
            EXPECT_EQ(channel.period[w], static_cast<double>(lags[w])) << "window " << w;
        }
    }

    TYPED_TEST(WaveformAnalyzerGpuTest, PeakOffsetAndCrestFactorMatchKinesis)
    {
        auto signal = noise(5000, 1.0, 31);
        for (auto& value : signal) {
            value += 0.2;
        }
        StandardWaveformAnalyzer analyzer(make_config());

        const auto channel = analyzer.analyze_waveform(input_of({ signal })).channels[0];

        const size_t windows = D::num_windows(signal.size(), 512, 256);
        const auto peak = D::peak(signal, windows, 256, 512);
        const auto mean = D::mean(signal, windows, 256, 512);
        const auto rms = D::rms(signal, windows, 256, 512);
        ASSERT_EQ(channel.peak.size(), windows);
        for (size_t w = 0; w < windows; ++w) {
            EXPECT_EQ(channel.peak[w], peak[w]);
            expect_close(channel.dc_offset[w], mean[w]);
            expect_close(channel.crest_factor[w], peak[w] / rms[w]);
        }
    }

    TYPED_TEST(WaveformAnalyzerGpuTest, HarmonicityAndChangesFollowTheirDefinitions)
    {
        StandardWaveformAnalyzer analyzer(make_config());

        const auto channel = analyzer.analyze_waveform(input_of({ voice_like() })).channels[0];

        ASSERT_EQ(channel.harmonicity.size(), 111U);
        EXPECT_EQ(channel.level_change[0], 0.0);
        EXPECT_EQ(channel.period_change[0], 0.0);
        for (size_t w = 0; w < channel.harmonicity.size(); ++w) {
            const double strength = channel.periodicity[w];
            const double bounded = std::min(strength, 1.0 - 1.0e-12);
            EXPECT_DOUBLE_EQ(channel.harmonicity[w], strength > 0.0 ? bounded / (1.0 - bounded) : 0.0);
            EXPECT_TRUE(std::isfinite(channel.harmonicity[w]));
            if (w == 0) {
                continue;
            }
            EXPECT_DOUBLE_EQ(channel.level_change[w], channel.level[w] - channel.level[w - 1]);
            const bool both = channel.period[w] > 0.0 && channel.period[w - 1] > 0.0;
            const double want = both ? std::abs(channel.period[w] - channel.period[w - 1]) / channel.period[w] : 0.0;
            EXPECT_DOUBLE_EQ(channel.period_change[w], want);
        }
        EXPECT_GT(channel.harmonicity[10], channel.harmonicity[60]);
    }

    TYPED_TEST(WaveformAnalyzerGpuTest, ProfilesAreOnlyKeptOnRequest)
    {
        StandardWaveformAnalyzer analyzer(make_config());

        const auto channel = analyzer.analyze_waveform(input_of({ voice_like() })).channels[0];

        EXPECT_TRUE(channel.autocorrelation.values.empty());
        EXPECT_TRUE(channel.mean_difference.values.empty());
        EXPECT_EQ(channel.autocorrelation.to_tensor("autocorrelation"), nullptr);
    }

    TYPED_TEST(WaveformAnalyzerGpuTest, KeptProfilesMatchAHostComputation)
    {
        auto signal = voice_like();
        signal.resize(4096);
        auto config = make_config();
        config.keep_profiles = true;
        StandardWaveformAnalyzer analyzer(config);

        const auto channel = analyzer.analyze_waveform(input_of({ signal })).channels[0];

        const size_t windows = D::num_windows(signal.size(), 512, 256);
        const auto& acf = channel.autocorrelation;
        const auto& amdf = channel.mean_difference;
        ASSERT_EQ(acf.first_lag, 0U);
        ASSERT_EQ(acf.lag_count, 229U);
        ASSERT_EQ(amdf.first_lag, 40U);
        ASSERT_EQ(amdf.lag_count, 189U);
        ASSERT_EQ(acf.values.size(), windows * acf.lag_count);
        ASSERT_EQ(amdf.values.size(), windows * amdf.lag_count);

        for (size_t w = 0; w < windows; ++w) {
            const std::span<const double> view(signal.data() + w * 256, 512);
            double mean = 0.0;
            for (const double v : view) {
                mean += v;
            }
            mean /= 512.0;
            double energy = 0.0;
            for (const double v : view) {
                energy += (v - mean) * (v - mean);
            }
            for (uint32_t lag = 0; lag < acf.lag_count; ++lag) {
                double sum = 0.0;
                for (size_t i = 0; i + lag < 512; ++i) {
                    sum += (view[i] - mean) * (view[i + lag] - mean);
                }
                EXPECT_NEAR(acf.values[w * acf.lag_count + lag], sum / energy, 1.0e-9) << "window " << w << " lag " << lag;
            }
            for (uint32_t column = 0; column < amdf.lag_count; ++column) {
                const size_t lag = amdf.first_lag + column;
                double sum = 0.0;
                for (size_t i = 0; i + lag < 512; ++i) {
                    sum += std::abs(view[i] - view[i + lag]);
                }
                EXPECT_NEAR(amdf.values[w * amdf.lag_count + column], sum / static_cast<double>(512 - lag), 1.0e-11);
            }
            EXPECT_DOUBLE_EQ(acf.values[w * acf.lag_count], 1.0);
        }
    }

    TYPED_TEST(WaveformAnalyzerGpuTest, ProfilesBecomeTensorsOfWindowsByLags)
    {
        auto signal = voice_like();
        signal.resize(4096);
        auto config = make_config();
        config.keep_profiles = true;
        StandardWaveformAnalyzer analyzer(config);

        const auto channel = analyzer.analyze_waveform(input_of({ signal })).channels[0];

        const auto tensor = channel.autocorrelation.to_tensor("autocorrelation");
        ASSERT_NE(tensor, nullptr);
        const auto& spec = tensor->get_data().spec();
        ASSERT_EQ(spec.shape.size(), 2U);
        EXPECT_EQ(spec.shape[0], 15U);
        EXPECT_EQ(spec.shape[1], 229U);
        const auto values = tensor->get_data().to_vector<double>();
        ASSERT_TRUE(values.has_value());
        EXPECT_EQ(*values, channel.autocorrelation.values);
    }

    TYPED_TEST(WaveformAnalyzerGpuTest, AnalysingTwiceGivesTheSameResult)
    {
        const std::vector<Kakshya::DataVariant> input = input_of({ voice_like() });
        StandardWaveformAnalyzer analyzer(make_config(WaveformMethod::PERIODICITY));

        const auto first = analyzer.analyze_waveform(input).channels[0];
        const auto second = analyzer.analyze_waveform(input).channels[0];

        EXPECT_EQ(first.values, second.values);
        EXPECT_EQ(first.period, second.period);
        EXPECT_EQ(first.peak, second.peak);
    }

    TEST(WaveformAnalyzerConfigTest, TheDefaultsAreValid)
    {
        EXPECT_NO_THROW(StandardWaveformAnalyzer {});
    }

    TEST(WaveformAnalyzerConfigTest, AWindowShorterThanTwiceTheLongestPeriodIsRejected)
    {
        EXPECT_THROW(StandardWaveformAnalyzer(make_config(WaveformMethod::LEVEL, 256, 128)), std::invalid_argument);
    }

    TEST(WaveformAnalyzerConfigTest, AnyHopAboveZeroIsAccepted)
    {
        EXPECT_NO_THROW(StandardWaveformAnalyzer(make_config(WaveformMethod::LEVEL, 512, 100)));
        EXPECT_NO_THROW(StandardWaveformAnalyzer(make_config(WaveformMethod::LEVEL, 512, 441)));
        EXPECT_THROW(StandardWaveformAnalyzer(make_config(WaveformMethod::LEVEL, 512, 0)), std::invalid_argument);
    }

    TEST(WaveformAnalyzerConfigTest, FrequenciesMustAscendBelowNyquist)
    {
        auto config = make_config();
        config.low_frequency = 400.0;
        config.high_frequency = 70.0;
        EXPECT_THROW(StandardWaveformAnalyzer { config }, std::invalid_argument);

        config = make_config();
        config.high_frequency = 8000.0;
        EXPECT_THROW(StandardWaveformAnalyzer { config }, std::invalid_argument);

        config = make_config();
        config.sample_rate = 0.0;
        EXPECT_THROW(StandardWaveformAnalyzer { config }, std::invalid_argument);
    }

    TEST(WaveformAnalyzerConfigTest, ARangeSpanningFewerThanTwoLagsIsRejected)
    {
        auto config = make_config();
        config.low_frequency = 399.0;
        config.high_frequency = 400.0;
        EXPECT_THROW(StandardWaveformAnalyzer { config }, std::invalid_argument);
    }

}
}
