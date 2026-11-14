#include "gtest/gtest.h"
#include "../../support/test_metadata.hpp"

#include <random>

#include "../../support/gpu_dispatch_test_support.hpp"

#include "MayaFlux/Kinesis/Discrete/Spectral.hpp"
#include "MayaFlux/Kinesis/Discrete/Taper.hpp"
#include "MayaFlux/Yantra/Analyzers/SpectralAnalyzer.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))

namespace MayaFlux::Test {
namespace {

    using namespace GpuDispatch;
    using Yantra::SpectralAnalysis;
    using Yantra::SpectralAnalyzerConfig;
    using Yantra::SpectralMethod;
    using Yantra::StandardSpectralAnalyzer;

    namespace D = Kinesis::Discrete;

    SpectralAnalyzerConfig make_config(
        uint32_t window, uint32_t hop, SpectralMethod method, std::vector<double> edges = { 0.0, 0.25, 0.5, 1.0 }, bool taper = true)
    {
        SpectralAnalyzerConfig config;
        config.window_size = window;
        config.hop_size = hop;
        config.method = method;
        config.band_edges = std::move(edges);
        config.taper = taper;
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

    std::vector<double> tone(size_t count, double bin, uint32_t window)
    {
        std::vector<double> out(count);
        for (size_t n = 0; n < count; ++n) {
            out[n] = std::cos(2.0 * std::numbers::pi * bin * static_cast<double>(n) / window);
        }
        return out;
    }

    void expect_close(double got, double want)
    {
        EXPECT_NEAR(got, want, 1.0e-11 * (1.0 + std::abs(want)));
    }

    template <typename Fixture>
    class SpectralAnalyzerGpuTest : public Fixture { };

    using HardwareOptions = ::testing::Types<GpuDispatchFixture<true>, GpuDispatchFixture<false>>;
    TYPED_TEST_SUITE(SpectralAnalyzerGpuTest, HardwareOptions);

    TYPED_TEST(SpectralAnalyzerGpuTest, TotalPowerMatchesTheKinesisProjection)
    {
        constexpr uint32_t window = 64;
        const auto signal = random_signal(1000, 3);
        StandardSpectralAnalyzer analyzer(make_config(window, 32, SpectralMethod::TOTAL_POWER));

        const auto analysis = analyzer.analyze_spectrum(input_of({ signal }));

        const std::vector<double> ones(window / 2 + 1, 1.0);
        const auto want = D::spectral_projection(signal, window, 32, ones, D::hann(window));
        ASSERT_EQ(analysis.channels.size(), 1U);
        ASSERT_EQ(analysis.channels[0].values.size(), want.size());
        for (size_t w = 0; w < want.size(); ++w) {
            expect_close(analysis.channels[0].values[w], want[w]);
        }
    }

    TYPED_TEST(SpectralAnalyzerGpuTest, BandEnergiesMatchKinesisAndPartitionTheTotal)
    {
        constexpr uint32_t window = 64;
        constexpr size_t bins = window / 2 + 1;
        const auto signal = random_signal(640, 5);
        StandardSpectralAnalyzer analyzer(make_config(window, 64, SpectralMethod::TOTAL_POWER, { 0.0, 0.25, 0.5, 1.0 }));

        const auto analysis = analyzer.analyze_spectrum(input_of({ signal }));

        std::vector<double> weights(4 * bins, 0.0);
        for (size_t k = 0; k < bins; ++k) {
            const double fraction = static_cast<double>(k) / static_cast<double>(bins - 1);
            weights[k] = 1.0;
            weights[bins + k] = fraction < 0.25 ? 1.0 : 0.0;
            weights[2 * bins + k] = (fraction >= 0.25 && fraction < 0.5) ? 1.0 : 0.0;
            weights[3 * bins + k] = fraction >= 0.5 ? 1.0 : 0.0;
        }
        const auto want = D::spectral_projection(signal, window, 64, weights, D::hann(window));

        const auto& channel = analysis.channels[0];
        ASSERT_EQ(channel.band_energies.size(), 10U);
        for (size_t w = 0; w < channel.band_energies.size(); ++w) {
            ASSERT_EQ(channel.band_energies[w].size(), 3U);
            double sum = 0.0;
            for (size_t b = 0; b < 3; ++b) {
                expect_close(channel.band_energies[w][b], want[w * 4 + 1 + b]);
                sum += channel.band_energies[w][b];
            }
            expect_close(sum, want[w * 4]);
        }
    }

    TYPED_TEST(SpectralAnalyzerGpuTest, TheCentroidSitsAtTheBinOfAPureTone)
    {
        constexpr uint32_t window = 64;
        StandardSpectralAnalyzer low(make_config(window, 64, SpectralMethod::CENTROID, { 0.0, 1.0 }, false));
        StandardSpectralAnalyzer high(make_config(window, 64, SpectralMethod::CENTROID, { 0.0, 1.0 }, false));

        const auto at_eight = low.analyze_spectrum(input_of({ tone(256, 8.0, window) }));
        const auto at_twenty = high.analyze_spectrum(input_of({ tone(256, 20.0, window) }));

        for (const double centroid : at_eight.channels[0].values) {
            EXPECT_NEAR(centroid, 8.0, 1.0e-9);
        }
        for (const double centroid : at_twenty.channels[0].values) {
            EXPECT_NEAR(centroid, 20.0, 1.0e-9);
        }
    }

    TYPED_TEST(SpectralAnalyzerGpuTest, ASilentSignalHasZeroPowerAndZeroCentroid)
    {
        StandardSpectralAnalyzer power(make_config(64, 64, SpectralMethod::TOTAL_POWER));
        StandardSpectralAnalyzer centroid(make_config(64, 64, SpectralMethod::CENTROID));
        const std::vector<double> silence(256, 0.0);

        const auto total = power.analyze_spectrum(input_of({ silence }));
        const auto bin = centroid.analyze_spectrum(input_of({ silence }));

        for (const double value : total.channels[0].values) {
            EXPECT_EQ(value, 0.0);
        }
        for (const double value : bin.channels[0].values) {
            EXPECT_EQ(value, 0.0);
        }
    }

    TYPED_TEST(SpectralAnalyzerGpuTest, FluxIsZeroForTheFirstWindowAndRisesAtAnOnset)
    {
        constexpr uint32_t window = 64;
        std::vector<double> signal(128, 0.0);
        const auto burst = tone(128, 8.0, window);
        signal.insert(signal.end(), burst.begin(), burst.end());
        StandardSpectralAnalyzer analyzer(make_config(window, 64, SpectralMethod::FLUX, { 0.0, 0.25, 0.5, 1.0 }, false));

        const auto analysis = analyzer.analyze_spectrum(input_of({ signal }));

        const auto& flux = analysis.channels[0].values;
        ASSERT_EQ(flux.size(), 4U);
        EXPECT_EQ(flux[0], 0.0);
        EXPECT_EQ(flux[1], 0.0);
        EXPECT_GT(flux[2], 1.0);
        EXPECT_NEAR(flux[3], 0.0, 1.0e-9);
    }

    TYPED_TEST(SpectralAnalyzerGpuTest, EveryChannelIsAnalysedOnItsOwn)
    {
        constexpr uint32_t window = 64;
        StandardSpectralAnalyzer analyzer(make_config(window, 64, SpectralMethod::CENTROID, { 0.0, 1.0 }, false));

        const auto analysis = analyzer.analyze_spectrum(input_of({ tone(192, 4.0, window), tone(192, 12.0, window) }));

        ASSERT_EQ(analysis.channels.size(), 2U);
        for (const double centroid : analysis.channels[0].values) {
            EXPECT_NEAR(centroid, 4.0, 1.0e-9);
        }
        for (const double centroid : analysis.channels[1].values) {
            EXPECT_NEAR(centroid, 12.0, 1.0e-9);
        }
    }

    TYPED_TEST(SpectralAnalyzerGpuTest, WindowPositionsFollowTheHop)
    {
        StandardSpectralAnalyzer analyzer(make_config(64, 48, SpectralMethod::TOTAL_POWER));

        const auto analysis = analyzer.analyze_spectrum(input_of({ random_signal(300, 7) }));

        const auto& positions = analysis.channels[0].window_positions;
        ASSERT_EQ(positions.size(), 5U);
        for (size_t w = 0; w < positions.size(); ++w) {
            EXPECT_EQ(positions[w].first, w * 48);
            EXPECT_EQ(positions[w].second, w * 48 + 64);
        }
    }

    TYPED_TEST(SpectralAnalyzerGpuTest, AChannelShorterThanOneWindowHasNoWindows)
    {
        StandardSpectralAnalyzer analyzer(make_config(64, 32, SpectralMethod::TOTAL_POWER));

        const auto analysis = analyzer.analyze_spectrum(input_of({ random_signal(40, 9), random_signal(200, 11) }));

        ASSERT_EQ(analysis.channels.size(), 2U);
        EXPECT_TRUE(analysis.channels[0].values.empty());
        EXPECT_TRUE(analysis.channels[0].band_energies.empty());
        EXPECT_EQ(analysis.channels[1].values.size(), 5U);
    }

    TYPED_TEST(SpectralAnalyzerGpuTest, ThePipelineOutputCarriesTheAnalysis)
    {
        StandardSpectralAnalyzer analyzer(make_config(64, 64, SpectralMethod::TOTAL_POWER));

        const auto output = analyzer.apply_operation(Yantra::Datum<std::vector<Kakshya::DataVariant>> { input_of({ random_signal(256, 13) }) });

        const auto* carried = output.facets.template get<SpectralAnalysis>();
        ASSERT_NE(carried, nullptr);
        EXPECT_EQ(carried->channels.size(), 1U);
        EXPECT_EQ(carried->channels[0].values.size(), 4U);
    }

    TYPED_TEST(SpectralAnalyzerGpuTest, ANewConfigRebuildsTheBandsAndTheTransform)
    {
        StandardSpectralAnalyzer analyzer(make_config(64, 64, SpectralMethod::TOTAL_POWER, { 0.0, 0.5, 1.0 }));
        const auto signal = random_signal(256, 17);

        const auto two_bands = analyzer.analyze_spectrum(input_of({ signal }));
        analyzer.set_config(make_config(128, 128, SpectralMethod::TOTAL_POWER, { 0.0, 0.25, 0.5, 0.75, 1.0 }));
        const auto four_bands = analyzer.analyze_spectrum(input_of({ signal }));

        EXPECT_EQ(two_bands.channels[0].band_energies[0].size(), 2U);
        EXPECT_EQ(two_bands.channels[0].values.size(), 4U);
        EXPECT_EQ(four_bands.channels[0].band_energies[0].size(), 4U);
        EXPECT_EQ(four_bands.channels[0].values.size(), 2U);
    }

    MAYAFLUX_TEST_WITH(TEST, SpectralAnalyzerTest, SettingsThatCannotRunAreRejected, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        EXPECT_THROW(StandardSpectralAnalyzer(make_config(100, 50, SpectralMethod::TOTAL_POWER)), std::invalid_argument);
        EXPECT_THROW(StandardSpectralAnalyzer(make_config(4096, 512, SpectralMethod::TOTAL_POWER)), std::invalid_argument);
        EXPECT_THROW(StandardSpectralAnalyzer(make_config(1, 1, SpectralMethod::TOTAL_POWER)), std::invalid_argument);
        EXPECT_THROW(StandardSpectralAnalyzer(make_config(64, 0, SpectralMethod::TOTAL_POWER)), std::invalid_argument);
        EXPECT_THROW(StandardSpectralAnalyzer(make_config(64, 32, SpectralMethod::TOTAL_POWER, { 0.5 })), std::invalid_argument);
        EXPECT_THROW(StandardSpectralAnalyzer(make_config(64, 32, SpectralMethod::TOTAL_POWER, { 0.5, 0.25 })), std::invalid_argument);
        EXPECT_THROW(StandardSpectralAnalyzer(make_config(64, 32, SpectralMethod::TOTAL_POWER, { -0.1, 1.0 })), std::invalid_argument);
        EXPECT_THROW(StandardSpectralAnalyzer(make_config(64, 32, SpectralMethod::TOTAL_POWER, { 0.0, 1.5 })), std::invalid_argument);
    }

} // namespace
} // namespace MayaFlux::Test
