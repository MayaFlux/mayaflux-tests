#include "gtest/gtest.h"
#include "../../support/test_metadata.hpp"

#include <filesystem>
#include <fstream>
#include <random>

#include "../../support/gpu_dispatch_test_support.hpp"

#include "MayaFlux/IO/Tensor/SafetensorsFormat.hpp"
#include "MayaFlux/Kinesis/Discrete/Extract.hpp"
#include "MayaFlux/Yantra/Extractors/SpeechSegmenter.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))

namespace MayaFlux::Test {
namespace {

    namespace fs = std::filesystem;
    namespace D = Kinesis::Discrete;

    using namespace GpuDispatch;
    using Yantra::measure_voicing;
    using Yantra::RegionSpeechSegmenter;
    using Yantra::SpeechSegmentation;
    using Yantra::SpeechSegmenterConfig;
    using Yantra::StandardWaveformAnalyzer;
    using Yantra::voiced_intervals;
    using Yantra::voicing_analyzer_config;

    using Intervals = std::vector<std::pair<size_t, size_t>>;

    constexpr double k_rate = 16000.0;
    const char* const k_vad_weights = "data/tensors/silero/silero_vad_16k.safetensors";
    const char* const k_speech = "tests/models/hubert_phoneme/hubert_speech_audio.f32";

    std::vector<double> tone(size_t count, double period)
    {
        std::vector<double> out(count);
        for (size_t i = 0; i < count; ++i) {
            out[i] = std::sin(2.0 * std::numbers::pi * static_cast<double>(i) / period);
        }
        return out;
    }

    std::vector<double> noise(size_t count, uint64_t seed)
    {
        std::mt19937_64 engine(seed);
        std::uniform_real_distribution<double> distribution(-1.0, 1.0);
        std::vector<double> out(count);
        for (auto& value : out) {
            value = distribution(engine);
        }
        return out;
    }

    std::optional<Kakshya::TensorSet> read_weights(const fs::path& path)
    {
        if (!fs::exists(path)) {
            return std::nullopt;
        }
        IO::SafetensorsReader reader;
        if (!reader.open(path.string())) {
            return std::nullopt;
        }
        return reader.read_tensors();
    }

    std::vector<float> read_floats(const fs::path& path)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) {
            return {};
        }
        std::vector<float> out(static_cast<size_t>(file.tellg()) / sizeof(float));
        file.seekg(0);
        file.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size() * sizeof(float)));
        return out;
    }

    MAYAFLUX_TEST_WITH(TEST, IntervalStatisticsTest, ShareAboveCountsValuesAtOrAboveTheThreshold, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        const std::vector<double> values { 0.1, 0.5, 0.9, 0.4, 0.6, 0.2 };
        const std::vector<size_t> positions { 0, 10, 20, 30, 40, 50 };
        const Intervals intervals { { 0, 30 }, { 30, 60 }, { 100, 200 } };

        const auto share = D::interval_share_above(values, positions, intervals, 0.5);
        ASSERT_EQ(share.size(), 3U);
        EXPECT_DOUBLE_EQ(share[0], 2.0 / 3.0);
        EXPECT_DOUBLE_EQ(share[1], 1.0 / 3.0);
        EXPECT_DOUBLE_EQ(share[2], 0.0);
    }

    MAYAFLUX_TEST_WITH(TEST, IntervalStatisticsTest, MedianTakesTheMiddleValueOrTheMeanOfTheMiddlePair, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        const std::vector<double> values { 5.0, 1.0, 3.0, 8.0, 2.0, 4.0 };
        const std::vector<size_t> positions { 0, 10, 20, 30, 40, 50 };
        const Intervals intervals { { 0, 30 }, { 0, 60 }, { 60, 90 } };

        const auto median = D::interval_median(values, positions, intervals);
        ASSERT_EQ(median.size(), 3U);
        EXPECT_DOUBLE_EQ(median[0], 3.0);
        EXPECT_DOUBLE_EQ(median[1], 3.5);
        EXPECT_DOUBLE_EQ(median[2], 0.0);
    }

    MAYAFLUX_TEST_WITH(TEST, IntervalStatisticsTest, IntervalsAreHalfOpen, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        const std::vector<double> values { 1.0, 2.0, 3.0 };
        const std::vector<size_t> positions { 10, 20, 30 };
        const auto median = D::interval_median(values, positions, { { 10, 30 } });
        ASSERT_EQ(median.size(), 1U);
        EXPECT_DOUBLE_EQ(median[0], 1.5);
    }

    MAYAFLUX_TEST_WITH(TEST, IntervalStatisticsTest, IntersectionKeepsOnlyWhatBothSetsCover, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        EXPECT_EQ(D::intersect_intervals({ { 0, 10 }, { 20, 30 } }, { { 5, 25 } }), (Intervals { { 5, 10 }, { 20, 25 } }));
        EXPECT_EQ(D::intersect_intervals({ { 20, 30 }, { 0, 10 }, { 8, 12 } }, { { 0, 100 } }), (Intervals { { 0, 12 }, { 20, 30 } }));
        EXPECT_EQ(D::intersect_intervals({ { 0, 10 } }, { { 10, 20 } }), Intervals {});
        EXPECT_EQ(D::intersect_intervals({}, { { 0, 10 } }), Intervals {});
        EXPECT_EQ(D::intersect_intervals({ { 0, 10 } }, {}), Intervals {});
    }

    template <typename Fixture>
    class SpeechSegmenterGpuTest : public Fixture { };

    using HardwareOptions = ::testing::Types<GpuDispatchFixture<true>, GpuDispatchFixture<false>>;
    TYPED_TEST_SUITE(SpeechSegmenterGpuTest, HardwareOptions);

    TYPED_TEST(SpeechSegmenterGpuTest, ASteadyToneIsFullyVoicedAtItsFrequency)
    {
        StandardWaveformAnalyzer analyzer(voicing_analyzer_config(k_rate));
        const auto samples = tone(16000, 80.0);

        const auto voicing = measure_voicing(analyzer, samples, { { 0, 16000 } }, 0.5);
        ASSERT_EQ(voicing.voiced_fraction.size(), 1U);
        EXPECT_DOUBLE_EQ(voicing.voiced_fraction[0], 1.0);
        EXPECT_NEAR(voicing.f0[0], 200.0, 2.0);
    }

    TYPED_TEST(SpeechSegmenterGpuTest, WhiteNoiseIsNotVoiced)
    {
        StandardWaveformAnalyzer analyzer(voicing_analyzer_config(k_rate));
        const auto samples = noise(16000, 7);

        const auto voicing = measure_voicing(analyzer, samples, { { 0, 16000 } }, 0.5);
        ASSERT_EQ(voicing.voiced_fraction.size(), 1U);
        EXPECT_LT(voicing.voiced_fraction[0], 0.05);
    }

    TYPED_TEST(SpeechSegmenterGpuTest, SilenceBetweenTonesGivesNoVoicingAndNoPitch)
    {
        StandardWaveformAnalyzer analyzer(voicing_analyzer_config(k_rate));
        auto samples = tone(24000, 80.0);
        std::fill(samples.begin() + 8000, samples.begin() + 16000, 0.0);
        const Intervals intervals { { 0, 7680 }, { 8192, 15616 }, { 16512, 24000 } };

        const auto voicing = measure_voicing(analyzer, samples, intervals, 0.5);
        ASSERT_EQ(voicing.voiced_fraction.size(), 3U);
        EXPECT_DOUBLE_EQ(voicing.voiced_fraction[0], 1.0);
        EXPECT_NEAR(voicing.f0[0], 200.0, 2.0);
        EXPECT_DOUBLE_EQ(voicing.voiced_fraction[1], 0.0);
        EXPECT_DOUBLE_EQ(voicing.f0[1], 0.0);
        EXPECT_DOUBLE_EQ(voicing.voiced_fraction[2], 1.0);
        EXPECT_NEAR(voicing.f0[2], 200.0, 2.0);
    }

    TYPED_TEST(SpeechSegmenterGpuTest, VoicedIntervalsFollowTheToneAndSkipTheSilence)
    {
        constexpr uint32_t hop = 64;
        StandardWaveformAnalyzer analyzer(voicing_analyzer_config(k_rate, hop));
        auto samples = tone(24000, 80.0);
        std::fill(samples.begin() + 8000, samples.begin() + 16000, 0.0);

        const auto voiced = voiced_intervals(analyzer, samples, 0.5, hop);
        ASSERT_EQ(voiced.size(), 2U);
        EXPECT_LE(voiced[0].first, 300U);
        EXPECT_GE(voiced[0].second, 7500U);
        EXPECT_LE(voiced[0].second, 8400U);
        EXPECT_GE(voiced[1].first, 15600U);
        EXPECT_LE(voiced[1].first, 16500U);
        EXPECT_GE(voiced[1].second, 23500U);
        EXPECT_LE(voiced[1].second, samples.size());
    }

    TYPED_TEST(SpeechSegmenterGpuTest, NoiseAndShortInputsHaveNoVoicedIntervals)
    {
        StandardWaveformAnalyzer analyzer(voicing_analyzer_config(k_rate, 64));
        EXPECT_TRUE(voiced_intervals(analyzer, noise(16000, 7), 0.5, 64).empty());
        EXPECT_TRUE(voiced_intervals(analyzer, tone(100, 80.0), 0.5, 64).empty());
    }

    TYPED_TEST(SpeechSegmenterGpuTest, AnInputShorterThanOneWindowGivesZeros)
    {
        StandardWaveformAnalyzer analyzer(voicing_analyzer_config(k_rate));
        const auto voicing = measure_voicing(analyzer, tone(100, 80.0), { { 0, 100 } }, 0.5);
        ASSERT_EQ(voicing.voiced_fraction.size(), 1U);
        EXPECT_DOUBLE_EQ(voicing.voiced_fraction[0], 0.0);
        EXPECT_DOUBLE_EQ(voicing.f0[0], 0.0);
    }

    MAYAFLUX_TEST_WITH(TYPED_TEST, SpeechSegmenterGpuTest, SpeechPitchLiesInsideTheSearchedRange, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::repo_assets, ::MayaFlux::Test::Meta::Capability::silero_weights, ::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        const auto weights = read_weights(k_vad_weights);
        const auto speech = read_floats(k_speech);
        if (!weights || speech.empty()) {
            GTEST_SKIP() << "the Silero weights or the speech fixture are not present";
        }
        RegionSpeechSegmenter segmenter(*weights);

        SpeechSegmenterConfig config;
        config.speech.measure = true;
        const std::vector<double> samples(speech.begin(), speech.end());
        const SpeechSegmentation record = segmenter.segment(samples, config);

        ASSERT_FALSE(record.speech.empty());
        ASSERT_EQ(record.voiced_fraction.size(), record.speech.size());
        ASSERT_EQ(record.f0.size(), record.speech.size());
        for (size_t k = 0; k < record.speech.size(); ++k) {
            EXPECT_GE(record.voiced_fraction[k], 0.0);
            EXPECT_LE(record.voiced_fraction[k], 1.0);
            if (record.voiced_fraction[k] > 0.0) {
                EXPECT_GE(record.f0[k], 70.0);
                EXPECT_LE(record.f0[k], 400.0);
            } else {
                EXPECT_DOUBLE_EQ(record.f0[k], 0.0);
            }
        }
    }

    MAYAFLUX_TEST_WITH(TYPED_TEST, SpeechSegmenterGpuTest, MeasuresAreLeftEmptyUnlessAsked, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::repo_assets, ::MayaFlux::Test::Meta::Capability::silero_weights, ::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        const auto weights = read_weights(k_vad_weights);
        const auto speech = read_floats(k_speech);
        if (!weights || speech.empty()) {
            GTEST_SKIP() << "the Silero weights or the speech fixture are not present";
        }
        RegionSpeechSegmenter segmenter(*weights);

        const std::vector<double> samples(speech.begin(), speech.end());
        const SpeechSegmentation record = segmenter.segment(samples, SpeechSegmenterConfig {});
        EXPECT_FALSE(record.speech.empty());
        EXPECT_TRUE(record.voiced_fraction.empty());
        EXPECT_TRUE(record.f0.empty());
    }

    TYPED_TEST(SpeechSegmenterGpuTest, RefusesAnotherRateAndMissingWeights)
    {
        SpeechSegmenterConfig config;
        config.sample_rate = 8000.0;
        EXPECT_FALSE(RegionSpeechSegmenter::diagnose(config).empty());
        EXPECT_THROW(RegionSpeechSegmenter(Kakshya::TensorSet {}), std::invalid_argument);

        config = {};
        config.speech.voicing_threshold = 0.0;
        EXPECT_FALSE(RegionSpeechSegmenter::diagnose(config).empty());
    }

}
}
