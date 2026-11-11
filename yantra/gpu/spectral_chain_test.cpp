#include "gtest/gtest.h"

#include <random>

#include "../../support/gpu_dispatch_test_support.hpp"

#include "MayaFlux/Yantra/Analyzers/SpectralAnalyzer.hpp"
#include "MayaFlux/Yantra/Executors/Audio/WordPairMap.hpp"

namespace MayaFlux::Test {
namespace {

    using namespace GpuDispatch;
    using Portal::Graphics::WordPairOp;
    using Yantra::GpuBufferBinding;
    using Yantra::GpuDispatchRecorder;
    using Yantra::StandardSpectralAnalyzer;
    using Yantra::word_pair_map_spec;
    using Yantra::word_pair_map_stage;
    using Yantra::WordPairExecutionContext;

    constexpr auto k_word = GpuBufferBinding::ElementType::VEC2_U32;

    Yantra::SpectralAnalyzerConfig make_config()
    {
        Yantra::SpectralAnalyzerConfig config;
        config.window_size = 512;
        config.hop_size = 256;
        return config;
    }

    std::vector<double> test_signal(size_t count, uint64_t seed)
    {
        std::mt19937_64 engine(seed);
        std::uniform_real_distribution<double> noise(-0.2, 0.2);
        std::vector<double> out(count);
        for (size_t i = 0; i < count; ++i) {
            out[i] = std::sin(0.3 * static_cast<double>(i)) + 0.4 * std::sin(1.1 * static_cast<double>(i)) + noise(engine);
        }
        return out;
    }

    template <typename Fixture>
    class SpectralChainGpuTest : public Fixture { };

    using HardwareOptions = ::testing::Types<GpuDispatchFixture<true>, GpuDispatchFixture<false>>;
    TYPED_TEST_SUITE(SpectralChainGpuTest, HardwareOptions);

    TYPED_TEST(SpectralChainGpuTest, TheDescriptorsStayOnTheDeviceAndReachAFloatBufferInOneSubmission)
    {
        const auto signal = test_signal(6000, 1);
        const std::vector<Kakshya::DataVariant> input { signal };

        StandardSpectralAnalyzer reference(make_config());
        const auto want = reference.analyze_spectrum(input).channels[0];

        const auto recording = std::make_shared<GpuDispatchRecorder>(true);
        StandardSpectralAnalyzer analyzer(make_config());
        analyzer.share_recording(recording);
        const auto spec = word_pair_map_spec(WordPairOp::ToFloat32);
        auto config = Yantra::config_from_spec(spec);
        WordPairExecutionContext converter(config, Yantra::bindings_from_spec(spec), false, "ToFloat32");
        converter.set_recording(recording);

        const auto buffer = analyzer.descriptor_buffer();
        ASSERT_NE(buffer.context, nullptr);
        converter.import_shared(0, 0, *buffer.context, buffer.set, buffer.binding, k_word);

        const size_t windows = analyzer.record(signal);
        ASSERT_EQ(windows, want.band_energies.size());
        const size_t rows = buffer.values_per_window;
        ASSERT_EQ(rows, 2 + want.band_energies[0].size());
        const size_t count = windows * rows;
        converter.ensure_shared_buffer(0, 1, count, GpuBufferBinding::ElementType::FLOAT32);
        converter.record_stages({ word_pair_map_stage(converter, config, WordPairOp::ToFloat32, count) });
        const auto got = Yantra::read_float_results(converter, count);

        ASSERT_EQ(got.size(), count);
        for (size_t w = 0; w < windows; ++w) {
            for (size_t b = 0; b < want.band_energies[w].size(); ++b) {
                const auto expected = static_cast<float>(want.band_energies[w][b]);
                EXPECT_EQ(std::bit_cast<uint32_t>(got[w * rows + 2 + b]), std::bit_cast<uint32_t>(expected))
                    << "window " << w << " band " << b;
            }
        }
    }

    TYPED_TEST(SpectralChainGpuTest, ARecordOfAShortChannelHasNoWindowsAndTheRecordingCanOnlyBeSharedFirst)
    {
        const auto recording = std::make_shared<GpuDispatchRecorder>(true);
        StandardSpectralAnalyzer analyzer(make_config());
        analyzer.share_recording(recording);

        EXPECT_EQ(analyzer.record(test_signal(100, 2)), 0U);
        EXPECT_EQ(analyzer.descriptor_buffer().windows, 0U);
        EXPECT_THROW(analyzer.share_recording(recording), std::logic_error);
    }

    TYPED_TEST(SpectralChainGpuTest, SharingARecordingDoesNotChangeASynchronousAnalysis)
    {
        const auto signal = test_signal(4000, 3);
        const std::vector<Kakshya::DataVariant> input { signal };
        StandardSpectralAnalyzer alone(make_config());
        StandardSpectralAnalyzer shared(make_config());
        shared.share_recording(std::make_shared<GpuDispatchRecorder>(true));

        const auto want = alone.analyze_spectrum(input).channels[0];
        const auto got = shared.analyze_spectrum(input).channels[0];

        ASSERT_EQ(got.values.size(), want.values.size());
        for (size_t w = 0; w < want.values.size(); ++w) {
            EXPECT_EQ(got.values[w], want.values[w]) << "window " << w;
            EXPECT_EQ(got.band_energies[w], want.band_energies[w]) << "window " << w;
        }
    }

} // namespace
} // namespace MayaFlux::Test
