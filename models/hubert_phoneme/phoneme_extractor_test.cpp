#include "gtest/gtest.h"

#include <filesystem>
#include <fstream>

#include "../../support/gpu_dispatch_test_support.hpp"

#include "MayaFlux/IO/Tensor/SafetensorsFormat.hpp"
#include "MayaFlux/Yantra/Extractors/PhonemeExtractor.hpp"

namespace MayaFlux::Test {
namespace {

    namespace fs = std::filesystem;

    using namespace GpuDispatch;
    using IO::SafetensorsReader;
    using Kakshya::TensorSet;
    using Yantra::AudioPhonemeExtractor;
    using Yantra::HubertPhonemeModel;
    using Yantra::PhonemeExtraction;
    using Yantra::PhonemeExtractorConfig;
    using Yantra::PhonemeSelection;
    using Yantra::RegionPhonemeExtractor;
    using Yantra::StandardPhonemeExtractor;

    const fs::path k_vad_weights = "data/tensors/silero/silero_vad_16k.safetensors";
    const fs::path k_hubert_weights = "data/tensors/hubert_phoneme/hubert_base_phoneme_en.safetensors";
    const fs::path k_speech = "tests/models/hubert_phoneme/hubert_speech_audio.f32";
    constexpr size_t k_silence = 16000;

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

    std::vector<float> speech_then_silence_then_speech(const std::vector<float>& speech)
    {
        std::vector<float> out(speech);
        out.insert(out.end(), k_silence, 0.0F);
        out.insert(out.end(), speech.begin(), speech.end());
        return out;
    }

    std::vector<Kakshya::DataVariant> as_input(const std::vector<float>& audio)
    {
        return { std::vector<double>(audio.begin(), audio.end()) };
    }

    std::vector<std::string> names_in(const PhonemeExtraction& record, uint32_t segment)
    {
        std::vector<std::string> names;
        for (size_t k = 0; k < record.events.size(); ++k) {
            if (record.segments[k] == segment) {
                names.emplace_back(HubertPhonemeModel::label(record.events[k].phoneme));
            }
        }
        return names;
    }

    template <typename Fixture>
    class PhonemeExtractorGpuTest : public Fixture { };

    using HardwareOptions = ::testing::Types<GpuDispatchFixture<true>, GpuDispatchFixture<false>>;
    TYPED_TEST_SUITE(PhonemeExtractorGpuTest, HardwareOptions);

    TYPED_TEST(PhonemeExtractorGpuTest, SpeechIsFoundAndTheSilenceBetweenIsLeftOut)
    {
        const auto vad = read_weights(k_vad_weights);
        const auto hubert = read_weights(k_hubert_weights);
        const auto speech = read_floats(k_speech);
        if (!vad || !hubert || speech.empty()) {
            GTEST_SKIP() << "the Silero or HuBERT weights or the speech fixture are not present";
        }
        StandardPhonemeExtractor extractor(*vad, *hubert);

        const auto output = extractor.apply_operation(Yantra::Datum<std::vector<Kakshya::DataVariant>> { as_input(speech_then_silence_then_speech(speech)) });
        const auto* record = output.facets.template get<PhonemeExtraction>();
        ASSERT_NE(record, nullptr);

        ASSERT_EQ(record->speech.size(), 2U);
        ASSERT_EQ(record->events.size(), record->segments.size());
        for (size_t k = 0; k < record->events.size(); ++k) {
            const auto& event = record->events[k];
            const auto& interval = record->speech[record->segments[k]];
            EXPECT_LT(event.start, event.end) << "event " << k;
            EXPECT_GE(event.start, interval.first) << "event " << k;
            EXPECT_LE(event.end, interval.second) << "event " << k;
        }
    }

    TYPED_TEST(PhonemeExtractorGpuTest, EachSpeechIntervalOpensWithTheSameWords)
    {
        const auto vad = read_weights(k_vad_weights);
        const auto hubert = read_weights(k_hubert_weights);
        const auto speech = read_floats(k_speech);
        if (!vad || !hubert || speech.empty()) {
            GTEST_SKIP() << "the Silero or HuBERT weights or the speech fixture are not present";
        }
        StandardPhonemeExtractor extractor(*vad, *hubert);

        const auto output = extractor.apply_operation(Yantra::Datum<std::vector<Kakshya::DataVariant>> { as_input(speech_then_silence_then_speech(speech)) });
        const auto* record = output.facets.template get<PhonemeExtraction>();
        ASSERT_NE(record, nullptr);

        const std::vector<std::string> opening { "G", "OW", "IH", "NG", "AH", "L", "AO", "NG" };
        for (uint32_t segment = 0; segment < 2; ++segment) {
            const auto names = names_in(*record, segment);
            ASSERT_GE(names.size(), opening.size()) << "segment " << segment;
            EXPECT_TRUE(std::equal(opening.begin(), opening.end(), names.begin())) << "segment " << segment;
        }
    }

    TYPED_TEST(PhonemeExtractorGpuTest, RangesAreContiguousWithinASpeechInterval)
    {
        const auto vad = read_weights(k_vad_weights);
        const auto hubert = read_weights(k_hubert_weights);
        const auto speech = read_floats(k_speech);
        if (!vad || !hubert || speech.empty()) {
            GTEST_SKIP() << "the Silero or HuBERT weights or the speech fixture are not present";
        }
        StandardPhonemeExtractor extractor(*vad, *hubert);

        const auto output = extractor.apply_operation(Yantra::Datum<std::vector<Kakshya::DataVariant>> { as_input(speech_then_silence_then_speech(speech)) });
        const auto* record = output.facets.template get<PhonemeExtraction>();
        ASSERT_NE(record, nullptr);

        for (size_t k = 1; k < record->events.size(); ++k) {
            if (record->segments[k] == record->segments[k - 1]) {
                EXPECT_EQ(record->events[k].start, record->events[k - 1].end) << "event " << k;
            }
        }
    }

    TYPED_TEST(PhonemeExtractorGpuTest, VowelSelectionKeepsOnlyVowelRecords)
    {
        const auto vad = read_weights(k_vad_weights);
        const auto hubert = read_weights(k_hubert_weights);
        const auto speech = read_floats(k_speech);
        if (!vad || !hubert || speech.empty()) {
            GTEST_SKIP() << "the Silero or HuBERT weights or the speech fixture are not present";
        }
        PhonemeExtractorConfig config;
        config.method = PhonemeSelection::VOWELS;
        StandardPhonemeExtractor extractor(*vad, *hubert, config);

        const auto output = extractor.apply_operation(Yantra::Datum<std::vector<Kakshya::DataVariant>> { as_input(speech_then_silence_then_speech(speech)) });
        const auto* record = output.facets.template get<PhonemeExtraction>();
        ASSERT_NE(record, nullptr);

        size_t vowels = 0;
        for (const auto& event : record->events) {
            vowels += HubertPhonemeModel::is_vowel(event.phoneme) ? 1 : 0;
        }
        ASSERT_GT(vowels, 0U);
        ASSERT_EQ(output.data.size(), vowels);
        for (size_t row = 0; row < output.data.size(); ++row) {
            const auto record_row = output.data.at(row);
            ASSERT_TRUE(record_row.has_value());
            EXPECT_EQ(record_row->template get<uint8_t>("vowel"), std::optional<uint8_t>(1)) << "row " << row;
        }
    }

    TYPED_TEST(PhonemeExtractorGpuTest, AudioOutputHoldsTheSelectedRanges)
    {
        const auto vad = read_weights(k_vad_weights);
        const auto hubert = read_weights(k_hubert_weights);
        const auto speech = read_floats(k_speech);
        if (!vad || !hubert || speech.empty()) {
            GTEST_SKIP() << "the Silero or HuBERT weights or the speech fixture are not present";
        }
        AudioPhonemeExtractor extractor(*vad, *hubert);

        const auto output = extractor.apply_operation(Yantra::Datum<std::vector<Kakshya::DataVariant>> { as_input(speech_then_silence_then_speech(speech)) });
        const auto* record = output.facets.template get<PhonemeExtraction>();
        ASSERT_NE(record, nullptr);

        size_t expected = 0;
        for (const auto& event : record->events) {
            expected += event.end - event.start;
        }
        ASSERT_EQ(output.data.size(), 1U);
        const auto* audio = std::get_if<std::vector<double>>(&output.data[0]);
        ASSERT_NE(audio, nullptr);
        EXPECT_EQ(audio->size(), expected);
    }

    TYPED_TEST(PhonemeExtractorGpuTest, RegionOutputHasOneRegionPerPhoneme)
    {
        const auto vad = read_weights(k_vad_weights);
        const auto hubert = read_weights(k_hubert_weights);
        const auto speech = read_floats(k_speech);
        if (!vad || !hubert || speech.empty()) {
            GTEST_SKIP() << "the Silero or HuBERT weights or the speech fixture are not present";
        }
        RegionPhonemeExtractor extractor(*vad, *hubert);

        const auto output = extractor.apply_operation(Yantra::Datum<std::vector<Kakshya::DataVariant>> { as_input(speech_then_silence_then_speech(speech)) });
        const auto* record = output.facets.template get<PhonemeExtraction>();
        ASSERT_NE(record, nullptr);

        ASSERT_EQ(output.data.regions.size(), record->events.size());
        EXPECT_EQ(output.data.regions.front().label, std::optional<std::string>("G"));
    }

    TYPED_TEST(PhonemeExtractorGpuTest, RefusesAnotherRateAndMissingWeights)
    {
        const auto vad = read_weights(k_vad_weights);
        const auto hubert = read_weights(k_hubert_weights);
        if (!vad || !hubert) {
            GTEST_SKIP() << "the Silero or HuBERT weights are not present";
        }
        PhonemeExtractorConfig config;
        config.sample_rate = 8000.0;
        EXPECT_THROW(StandardPhonemeExtractor(*vad, *hubert, config), std::invalid_argument);
        EXPECT_THROW(StandardPhonemeExtractor(TensorSet {}, *hubert), std::invalid_argument);
        EXPECT_THROW(StandardPhonemeExtractor(*vad, TensorSet {}), std::invalid_argument);
    }

    TYPED_TEST(PhonemeExtractorGpuTest, SilenceGivesAnEmptyButValidResult)
    {
        const auto vad = read_weights(k_vad_weights);
        const auto hubert = read_weights(k_hubert_weights);
        if (!vad || !hubert) {
            GTEST_SKIP() << "the Silero or HuBERT weights are not present";
        }
        StandardPhonemeExtractor extractor(*vad, *hubert);
        const std::vector<float> silence(2 * k_silence, 0.0F);

        const auto output = extractor.apply_operation(Yantra::Datum<std::vector<Kakshya::DataVariant>> { as_input(silence) });
        const auto* record = output.facets.template get<PhonemeExtraction>();
        ASSERT_NE(record, nullptr);

        EXPECT_TRUE(record->speech.empty());
        EXPECT_TRUE(record->events.empty());
        EXPECT_TRUE(record->segments.empty());
        EXPECT_EQ(output.data.size(), 0U);
    }

    TYPED_TEST(PhonemeExtractorGpuTest, MergeAdjacentJoinsTouchingRanges)
    {
        const auto vad = read_weights(k_vad_weights);
        const auto hubert = read_weights(k_hubert_weights);
        const auto speech = read_floats(k_speech);
        if (!vad || !hubert || speech.empty()) {
            GTEST_SKIP() << "the Silero or HuBERT weights or the speech fixture are not present";
        }
        PhonemeExtractorConfig config;
        config.merge_adjacent = true;
        AudioPhonemeExtractor extractor(*vad, *hubert, config);

        const auto output = extractor.apply_operation(Yantra::Datum<std::vector<Kakshya::DataVariant>> { as_input(speech_then_silence_then_speech(speech)) });
        const auto* record = output.facets.template get<PhonemeExtraction>();
        ASSERT_NE(record, nullptr);

        std::vector<std::pair<size_t, size_t>> ranges;
        for (const auto& event : record->events) {
            ranges.emplace_back(event.start, event.end);
        }
        const auto merged = Kinesis::Discrete::merge_intervals(ranges);
        size_t expected = 0;
        for (const auto& [start, end] : merged) {
            expected += end - start;
        }
        ASSERT_LT(merged.size(), ranges.size());
        ASSERT_EQ(output.data.size(), 1U);
        const auto* audio = std::get_if<std::vector<double>>(&output.data[0]);
        ASSERT_NE(audio, nullptr);
        EXPECT_EQ(audio->size(), expected);
    }

}
}
