#include "gtest/gtest.h"
#include "../../support/test_metadata.hpp"

#include <filesystem>
#include <fstream>

#include "../../support/gpu_dispatch_test_support.hpp"

#include "MayaFlux/IO/Tensor/SafetensorsFormat.hpp"
#include "MayaFlux/Yantra/Analyzers/VadAnalyzer.hpp"
#include "MayaFlux/Yantra/Executors/Audio/SileroVadModel.hpp"
#include "MayaFlux/Yantra/Extractors/IntervalExtractor.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::silero_weights, ::MayaFlux::Test::Meta::Capability::vulkan>() }))

namespace MayaFlux::Test {
namespace {

    namespace fs = std::filesystem;

    using namespace GpuDispatch;
    using IO::SafetensorsReader;
    using Kakshya::TensorSet;
    using Yantra::GpuDispatchRecorder;
    using Yantra::IntervalExtractorConfig;
    using Yantra::MatrixIntervalExtractor;
    using Yantra::SileroVadModel;
    using Yantra::StandardVadAnalyzer;

    const fs::path k_weights = "data/tensors/silero/silero_vad_16k.safetensors";

    std::optional<TensorSet> load_weights()
    {
        if (!fs::exists(k_weights)) {
            return std::nullopt;
        }
        SafetensorsReader reader;
        if (!reader.open(k_weights.string())) {
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

    IntervalExtractorConfig speech_config(size_t samples)
    {
        IntervalExtractorConfig config;
        config.scan.samples_per_score = SileroVadModel::chunk_samples;
        config.flush = true;
        config.length = static_cast<uint32_t>(samples);
        return config;
    }

    template <typename Fixture>
    class SileroChainGpuTest : public Fixture { };

    using HardwareOptions = ::testing::Types<GpuDispatchFixture<true>, GpuDispatchFixture<false>>;
    TYPED_TEST_SUITE(SileroChainGpuTest, HardwareOptions);

    MAYAFLUX_TEST_WITH(TYPED_TEST, SileroChainGpuTest, TheNetworkAndTheScanShareOneSubmissionAndGiveTheOfflineIntervals, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::repo_assets, ::MayaFlux::Test::Meta::Capability::silero_weights, ::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        const auto weights = load_weights();
        const auto audio = read_floats("tests/models/silero/silero_reference_audio.f32");
        if (!weights || audio.empty()) {
            GTEST_SKIP() << "the Silero weights or the reference fixture are not present";
        }
        const auto config = speech_config(audio.size());

        SileroVadModel offline(*weights);
        const auto probabilities = offline.process(audio);
        Eigen::MatrixXd column(static_cast<Eigen::Index>(probabilities.size()), 1);
        for (size_t n = 0; n < probabilities.size(); ++n) {
            column(static_cast<Eigen::Index>(n), 0) = probabilities[n];
        }
        MatrixIntervalExtractor reference(config);
        const auto want = reference.extract_intervals(column).channels[0].intervals;

        const auto recording = std::make_shared<GpuDispatchRecorder>(true);
        SileroVadModel model(*weights, recording);
        MatrixIntervalExtractor chained(config);
        chained.share_recording(recording);
        const auto buffer = model.probability_buffer();
        chained.bind_scores(0, *buffer.context, buffer.set, buffer.binding);

        const size_t recorded = model.record(audio);
        ASSERT_EQ(recorded, probabilities.size());
        ASSERT_EQ(model.resident_count(), recorded);
        const std::array<uint32_t, 1> counts { static_cast<uint32_t>(recorded) };
        const auto got = chained.extract_resident(counts).channels[0].intervals;

        EXPECT_EQ(got, want);
    }

    MAYAFLUX_TEST_WITH(TYPED_TEST, SileroChainGpuTest, ASubmittedChainCompletesWithoutBlockingTheCaller, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::repo_assets, ::MayaFlux::Test::Meta::Capability::silero_weights, ::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        const auto weights = load_weights();
        const auto audio = read_floats("tests/models/silero/silero_reference_audio.f32");
        if (!weights || audio.empty()) {
            GTEST_SKIP() << "the Silero weights or the reference fixture are not present";
        }
        const auto config = speech_config(audio.size());

        SileroVadModel offline(*weights);
        const auto probabilities = offline.process(audio);
        Eigen::MatrixXd column(static_cast<Eigen::Index>(probabilities.size()), 1);
        for (size_t n = 0; n < probabilities.size(); ++n) {
            column(static_cast<Eigen::Index>(n), 0) = probabilities[n];
        }
        MatrixIntervalExtractor reference(config);
        const auto want = reference.extract_intervals(column).channels[0].intervals;

        const auto recording = std::make_shared<GpuDispatchRecorder>(true);
        SileroVadModel model(*weights, recording);
        MatrixIntervalExtractor chained(config);
        chained.share_recording(recording);
        const auto buffer = model.probability_buffer();
        chained.bind_scores(0, *buffer.context, buffer.set, buffer.binding);

        const size_t recorded = model.record(audio);
        const std::array<uint32_t, 1> counts { static_cast<uint32_t>(recorded) };
        ASSERT_TRUE(chained.submit_resident(counts));
        EXPECT_TRUE(chained.pending());
        EXPECT_FALSE(chained.submit_resident(counts));

        std::optional<Yantra::IntervalExtraction> record;
        for (int attempt = 0; attempt < 5000 && !record; ++attempt) {
            record = chained.collect();
            if (!record) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        ASSERT_TRUE(record.has_value());
        EXPECT_EQ(record->channels[0].intervals, want);
        EXPECT_FALSE(chained.pending());
    }

    TYPED_TEST(SileroChainGpuTest, ABindingNeedsTheSameRecordingAndEachChannelNeedsItsBinding)
    {
        const auto weights = load_weights();
        if (!weights) {
            GTEST_SKIP() << "data/tensors/silero/silero_vad_16k.safetensors is not present";
        }
        const auto config = speech_config(SileroVadModel::chunk_samples);
        const std::array<uint32_t, 1> counts { 1U };

        SileroVadModel alone(*weights);
        const auto elsewhere = alone.probability_buffer();
        MatrixIntervalExtractor on_its_own(config);
        EXPECT_THROW(on_its_own.bind_scores(0, *elsewhere.context, elsewhere.set, elsewhere.binding), std::invalid_argument);

        const auto recording = std::make_shared<GpuDispatchRecorder>(true);
        SileroVadModel model(*weights, recording);
        const auto buffer = model.probability_buffer();
        MatrixIntervalExtractor unbound(config);
        unbound.share_recording(recording);
        EXPECT_THROW(static_cast<void>(unbound.extract_resident(counts)), std::invalid_argument);

        MatrixIntervalExtractor started(config);
        started.share_recording(recording);
        started.bind_scores(0, *buffer.context, buffer.set, buffer.binding);
        EXPECT_THROW(started.share_recording(recording), std::logic_error);
    }

    MAYAFLUX_TEST_WITH(TYPED_TEST, SileroChainGpuTest, AVadAnalyzerSharesItsRecordingAndTheScanReadsItsProbabilitiesInPlace, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::repo_assets, ::MayaFlux::Test::Meta::Capability::silero_weights, ::MayaFlux::Test::Meta::Capability::vulkan>() }))
    {
        const auto weights = load_weights();
        const auto audio = read_floats("tests/models/silero/silero_reference_audio.f32");
        if (!weights || audio.empty()) {
            GTEST_SKIP() << "the Silero weights or the reference fixture are not present";
        }
        const auto config = speech_config(audio.size());

        StandardVadAnalyzer offline(*weights);
        const std::vector<Kakshya::DataVariant> input { std::vector<double>(audio.begin(), audio.end()) };
        const auto probabilities = offline.analyze_speech(input).probabilities;
        Eigen::MatrixXd column(static_cast<Eigen::Index>(probabilities.size()), 1);
        for (size_t n = 0; n < probabilities.size(); ++n) {
            column(static_cast<Eigen::Index>(n), 0) = probabilities[n];
        }
        MatrixIntervalExtractor reference(config);
        const auto want = reference.extract_intervals(column).channels[0].intervals;

        const auto recording = std::make_shared<GpuDispatchRecorder>(true);
        StandardVadAnalyzer analyzer(*weights);
        analyzer.share_recording(recording);
        MatrixIntervalExtractor chained(config);
        chained.share_recording(recording);
        const auto buffer = analyzer.probability_buffer();
        chained.bind_scores(0, *buffer.context, buffer.set, buffer.binding);

        const size_t recorded = analyzer.record(audio);
        ASSERT_EQ(recorded, probabilities.size());
        ASSERT_EQ(analyzer.resident_count(), recorded);
        const std::array<uint32_t, 1> counts { static_cast<uint32_t>(recorded) };
        const auto got = chained.extract_resident(counts).channels[0].intervals;

        EXPECT_EQ(got, want);
        EXPECT_THROW(analyzer.share_recording(recording), std::logic_error);
    }

} // namespace
} // namespace MayaFlux::Test
