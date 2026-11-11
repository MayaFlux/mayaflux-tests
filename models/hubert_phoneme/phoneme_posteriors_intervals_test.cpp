#include "gtest/gtest.h"

#include <filesystem>
#include <fstream>

#include "../../support/gpu_dispatch_test_support.hpp"

#include "MayaFlux/IO/Tensor/SafetensorsFormat.hpp"
#include "MayaFlux/Kinesis/Discrete/IntervalScan.hpp"
#include "MayaFlux/Yantra/Analyzers/PhonemeAnalyzer.hpp"
#include "MayaFlux/Yantra/Extractors/IntervalExtractor.hpp"

namespace MayaFlux::Test {
namespace {

    namespace fs = std::filesystem;

    using namespace GpuDispatch;
    using IO::SafetensorsReader;
    using Kakshya::TensorSet;
    using Yantra::HubertPhonemeModel;
    using Yantra::IntervalScanSettings;
    using Yantra::IntervalExtractorConfig;
    using Yantra::MatrixIntervalExtractor;
    using Yantra::PhonemeAnalyzerConfig;
    using Yantra::PhonemeMethod;
    using Yantra::StandardPhonemeAnalyzer;

    using Intervals = std::vector<std::pair<size_t, size_t>>;

    const fs::path k_hubert_weights = "data/tensors/hubert_phoneme/hubert_base_phoneme_en.safetensors";
    const fs::path k_speech = "tests/models/hubert_phoneme/hubert_speech_audio.f32";

    std::optional<TensorSet> read_weights(const fs::path& path)
    {
        if (!fs::exists(path)) {
            return std::nullopt;
        }
        SafetensorsReader reader;
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

    std::vector<double> column_of(const Eigen::MatrixXd& matrix, Eigen::Index column)
    {
        const auto values = matrix.col(column);
        return { values.data(), values.data() + values.size() };
    }

    Intervals on_the_host(const IntervalScanSettings& settings, std::span<const double> scores, std::span<const double> gate = {})
    {
        Kinesis::Discrete::IntervalScanner scanner(Yantra::interval_scan_config(settings, 0));
        auto out = scanner.push(scores, gate);
        const auto rest = scanner.flush();
        out.insert(out.end(), rest.begin(), rest.end());
        return out;
    }

    template <typename Fixture>
    class PhonemePosteriorsIntervalsGpuTest : public Fixture { };

    using HardwareOptions = ::testing::Types<GpuDispatchFixture<true>, GpuDispatchFixture<false>>;
    TYPED_TEST_SUITE(PhonemePosteriorsIntervalsGpuTest, HardwareOptions);

    TYPED_TEST(PhonemePosteriorsIntervalsGpuTest, EveryClassColumnOfThePosteriorsGivesItsOwnIntervals)
    {
        const auto weights = read_weights(k_hubert_weights);
        const auto speech = read_floats(k_speech);
        if (!weights || speech.empty()) {
            GTEST_SKIP() << "the HuBERT weights or the speech fixture are not present";
        }
        StandardPhonemeAnalyzer analyzer(*weights);
        const auto posteriors = analyzer.apply_operation(
            Yantra::Datum<std::vector<Kakshya::DataVariant>> { std::vector<Kakshya::DataVariant> { std::vector<double>(speech.begin(), speech.end()) } });
        const auto& matrix = posteriors.data;
        ASSERT_EQ(matrix.cols(), static_cast<Eigen::Index>(HubertPhonemeModel::classes));

        IntervalScanSettings phoneme_scan;
        phoneme_scan.enter_threshold = 0.5;
        phoneme_scan.samples_per_score = HubertPhonemeModel::frame_hop;
        phoneme_scan.min_run = 0;
        phoneme_scan.min_gap = 1;
        phoneme_scan.padding = 0;
        IntervalExtractorConfig config;
        config.flush = true;
        config.scan = phoneme_scan;
        config.columns.assign(HubertPhonemeModel::classes, phoneme_scan);
        for (size_t k = 0; k < config.columns.size(); ++k) {
            config.columns[k].enter_threshold = 0.3 + 0.01 * static_cast<double>(k);
        }

        MatrixIntervalExtractor extractor(config);
        const auto result = extractor.extract_intervals(matrix);

        ASSERT_EQ(result.channels.size(), static_cast<size_t>(HubertPhonemeModel::classes));
        for (size_t k = 0; k < result.channels.size(); ++k) {
            EXPECT_EQ(result.channels[k].intervals, on_the_host(config.columns[k], column_of(matrix, static_cast<Eigen::Index>(k))))
                << "class " << k;
        }
    }

    TYPED_TEST(PhonemePosteriorsIntervalsGpuTest, BroadClassesGetTheSameSettingsAsTheHostScanner)
    {
        const auto weights = read_weights(k_hubert_weights);
        const auto speech = read_floats(k_speech);
        if (!weights || speech.empty()) {
            GTEST_SKIP() << "the HuBERT weights or the speech fixture are not present";
        }
        PhonemeAnalyzerConfig analysis;
        analysis.method = PhonemeMethod::BROAD_CLASSES;
        StandardPhonemeAnalyzer analyzer(*weights, analysis);
        const auto broad = analyzer.apply_operation(
            Yantra::Datum<std::vector<Kakshya::DataVariant>> { std::vector<Kakshya::DataVariant> { std::vector<double>(speech.begin(), speech.end()) } });
        const auto& matrix = broad.data;
        ASSERT_EQ(matrix.cols(), 3);

        IntervalExtractorConfig config;
        config.flush = true;
        config.scan.samples_per_score = HubertPhonemeModel::frame_hop;
        config.scan.first_sample = 0;
        config.scan.min_run = 960;
        config.scan.min_gap = 320;
        config.scan.padding = 160;
        IntervalScanSettings vowels = config.scan;
        vowels.enter_threshold = 0.4;
        IntervalScanSettings consonants = config.scan;
        consonants.enter_threshold = 0.4;
        consonants.gate_channel = 0;
        config.columns = { vowels, consonants, config.scan };

        MatrixIntervalExtractor extractor(config);
        const auto result = extractor.extract_intervals(matrix);

        ASSERT_EQ(result.channels.size(), 3U);
        EXPECT_EQ(result.channels[0].intervals, on_the_host(vowels, column_of(matrix, 0)));
        EXPECT_EQ(result.channels[1].intervals, on_the_host(consonants, column_of(matrix, 1), column_of(matrix, 0)));
        EXPECT_EQ(result.channels[2].intervals, on_the_host(config.scan, column_of(matrix, 2)));
    }

}
}
