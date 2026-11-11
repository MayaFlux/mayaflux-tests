#include "gtest/gtest.h"

#include <filesystem>
#include <fstream>

#include "../../support/gpu_dispatch_test_support.hpp"

#include "MayaFlux/IO/Tensor/SafetensorsFormat.hpp"
#include "MayaFlux/Yantra/Analyzers/PhonemeAnalyzer.hpp"
#include "MayaFlux/Yantra/Executors/Audio/HubertPhonemeModel.hpp"

namespace MayaFlux::Test {
namespace {

    namespace fs = std::filesystem;

    using namespace GpuDispatch;
    using IO::SafetensorsReader;
    using Kakshya::TensorSet;
    using Yantra::HubertPhonemeModel;
    using Yantra::PhonemeAnalyzerConfig;
    using Yantra::PhonemeMethod;
    using Yantra::StandardPhonemeAnalyzer;

    const fs::path k_hubert_weights = "data/tensors/hubert_phoneme/hubert_base_phoneme_en.safetensors";
    constexpr size_t k_classes = HubertPhonemeModel::classes;
    constexpr float k_posterior_tolerance = 2.0e-3F;

    const std::optional<TensorSet>& hubert_weights()
    {
        static const std::optional<TensorSet> weights = []() -> std::optional<TensorSet> {
            if (!fs::exists(k_hubert_weights)) {
                return std::nullopt;
            }
            SafetensorsReader reader;
            if (!reader.open(k_hubert_weights.string())) {
                return std::nullopt;
            }
            return reader.read_tensors();
        }();
        return weights;
    }

    std::vector<float> hubert_floats(const fs::path& path)
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

    std::vector<std::string> phonemes_of(const std::vector<float>& posteriors)
    {
        std::vector<std::string> out;
        uint32_t previous = HubertPhonemeModel::blank_class;
        for (size_t frame = 0; frame < posteriors.size() / k_classes; ++frame) {
            const float* row = posteriors.data() + frame * k_classes;
            const auto top = static_cast<uint32_t>(std::max_element(row, row + k_classes) - row);
            if (top != previous && top < HubertPhonemeModel::phonemes) {
                out.emplace_back(HubertPhonemeModel::label(top));
            }
            previous = top;
        }
        return out;
    }

    std::vector<Kakshya::DataVariant> hubert_input(const std::vector<float>& audio)
    {
        return { std::vector<double>(audio.begin(), audio.end()) };
    }

    template <typename Fixture>
    class HubertPhonemeGpuTest : public Fixture { };

    using HardwareOptions = ::testing::Types<GpuDispatchFixture<true>, GpuDispatchFixture<false>>;
    TYPED_TEST_SUITE(HubertPhonemeGpuTest, HardwareOptions);

    TYPED_TEST(HubertPhonemeGpuTest, PosteriorsMatchTheOriginalNetworkOnTheReferenceSignal)
    {
        const auto& weights = hubert_weights();
        const auto audio = hubert_floats("tests/models/hubert_phoneme/hubert_reference_audio.f32");
        const auto want = hubert_floats("tests/models/hubert_phoneme/hubert_reference_posteriors.f32");
        if (!weights || audio.empty() || want.empty()) {
            GTEST_SKIP() << "the HuBERT weights or the reference fixture are not present";
        }
        HubertPhonemeModel model(*weights);

        const auto got = model.process(audio);

        ASSERT_EQ(got.size(), want.size());
        ASSERT_EQ(got.size(), HubertPhonemeModel::frame_count(audio.size()) * k_classes);
        for (size_t n = 0; n < want.size(); ++n) {
            EXPECT_NEAR(got[n], want[n], k_posterior_tolerance) << "frame " << n / k_classes << " class " << n % k_classes;
        }
    }

    TYPED_TEST(HubertPhonemeGpuTest, RecordedSpeechGivesTheOriginalNetworksPosteriorsAndPhonemes)
    {
        const auto& weights = hubert_weights();
        const auto audio = hubert_floats("tests/models/hubert_phoneme/hubert_speech_audio.f32");
        const auto want = hubert_floats("tests/models/hubert_phoneme/hubert_speech_posteriors.f32");
        if (!weights || audio.empty() || want.empty()) {
            GTEST_SKIP() << "the HuBERT weights or the speech fixture are not present";
        }
        HubertPhonemeModel model(*weights);

        const auto got = model.process(audio);

        ASSERT_EQ(got.size(), want.size());
        for (size_t n = 0; n < want.size(); ++n) {
            EXPECT_NEAR(got[n], want[n], k_posterior_tolerance) << "frame " << n / k_classes << " class " << n % k_classes;
        }
        const auto phonemes = phonemes_of(got);
        EXPECT_EQ(phonemes, phonemes_of(want));
        const std::vector<std::string> opening { "G", "OW", "IH", "NG", "AH", "L", "AO", "NG" };
        ASSERT_GE(phonemes.size(), opening.size());
        EXPECT_TRUE(std::equal(opening.begin(), opening.end(), phonemes.begin()));
    }

    TYPED_TEST(HubertPhonemeGpuTest, EveryFrameIsADistributionAndTheStatesHaveOneRowPerFrame)
    {
        const auto& weights = hubert_weights();
        const auto audio = hubert_floats("tests/models/hubert_phoneme/hubert_speech_audio.f32");
        if (!weights || audio.empty()) {
            GTEST_SKIP() << "the HuBERT weights or the speech fixture are not present";
        }
        HubertPhonemeModel model(*weights);

        const auto posteriors = model.process(audio);
        const auto states = model.hidden_states();

        const size_t frames = HubertPhonemeModel::frame_count(audio.size());
        ASSERT_EQ(model.resident_frames(), frames);
        ASSERT_EQ(posteriors.size(), frames * k_classes);
        ASSERT_EQ(states.size(), frames * HubertPhonemeModel::hidden_width);
        for (size_t frame = 0; frame < frames; ++frame) {
            const auto row = std::span(posteriors).subspan(frame * k_classes, k_classes);
            EXPECT_NEAR(std::accumulate(row.begin(), row.end(), 0.0), 1.0, 1.0e-4) << "frame " << frame;
        }
        EXPECT_TRUE(std::ranges::all_of(states, [](float value) { return std::isfinite(value); }));
        EXPECT_TRUE(std::ranges::any_of(states, [](float value) { return value != 0.0F; }));
    }

    TYPED_TEST(HubertPhonemeGpuTest, ASecondUtteranceIsNotAffectedByTheFirst)
    {
        const auto& weights = hubert_weights();
        const auto speech = hubert_floats("tests/models/hubert_phoneme/hubert_speech_audio.f32");
        const auto signal = hubert_floats("tests/models/hubert_phoneme/hubert_reference_audio.f32");
        if (!weights || speech.empty() || signal.empty()) {
            GTEST_SKIP() << "the HuBERT weights or the fixtures are not present";
        }
        HubertPhonemeModel fresh(*weights);
        const auto want = fresh.process(signal);

        HubertPhonemeModel reused(*weights);
        static_cast<void>(reused.process(speech));
        const auto got = reused.process(signal);

        EXPECT_EQ(got, want);
    }

    TYPED_TEST(HubertPhonemeGpuTest, AnUtteranceShorterThanOneFrameGivesNothingAndOneTooLongIsRefused)
    {
        const auto& weights = hubert_weights();
        if (!weights) {
            GTEST_SKIP() << "data/tensors/hubert_phoneme/hubert_base_phoneme_en.safetensors is not present";
        }
        HubertPhonemeModel model(*weights);

        EXPECT_TRUE(model.process(std::vector<float>(HubertPhonemeModel::frame_span - 1, 0.1F)).empty());
        EXPECT_EQ(model.resident_frames(), 0U);
        EXPECT_THROW(static_cast<void>(model.process(std::vector<float>(HubertPhonemeModel::max_samples + 1, 0.0F))), std::invalid_argument);
    }

    TYPED_TEST(HubertPhonemeGpuTest, TheAnalyzerReportsThePhonemesTheirFramesAndTheBroadClasses)
    {
        const auto& weights = hubert_weights();
        const auto audio = hubert_floats("tests/models/hubert_phoneme/hubert_speech_audio.f32");
        const auto want = hubert_floats("tests/models/hubert_phoneme/hubert_speech_posteriors.f32");
        if (!weights || audio.empty() || want.empty()) {
            GTEST_SKIP() << "the HuBERT weights or the speech fixture are not present";
        }
        PhonemeAnalyzerConfig config;
        config.method = PhonemeMethod::BROAD_CLASSES;
        config.keep_embeddings = true;
        StandardPhonemeAnalyzer analyzer(*weights, config);

        const auto output = analyzer.apply_operation(Yantra::Datum<std::vector<Kakshya::DataVariant>> { hubert_input(audio) });
        const auto analysis = analyzer.get_phoneme_analysis();

        const size_t frames = want.size() / k_classes;
        ASSERT_EQ(analysis.frames, frames);
        ASSERT_EQ(analysis.best.size(), frames);
        ASSERT_EQ(analysis.frame_positions.size(), frames);
        EXPECT_EQ(analysis.frame_positions[1], (std::pair<size_t, size_t> { 320, 720 }));
        EXPECT_EQ(analysis.embeddings.size(), frames * HubertPhonemeModel::hidden_width);

        std::vector<std::string> names;
        for (const auto& event : analysis.events) {
            names.emplace_back(HubertPhonemeModel::label(event.phoneme));
            EXPECT_EQ(event.sample, event.frame * HubertPhonemeModel::frame_hop);
            EXPECT_EQ(analysis.best[event.frame], event.phoneme);
            EXPECT_GT(event.probability, 1.0F / k_classes);
        }
        EXPECT_EQ(names, phonemes_of(want));

        ASSERT_EQ(output.data.cols(), 3);
        ASSERT_EQ(static_cast<size_t>(output.data.rows()), frames);
        for (Eigen::Index frame = 0; frame < output.data.rows(); ++frame) {
            EXPECT_NEAR(output.data.row(frame).sum(), 1.0, 1.0e-4) << "frame " << frame;
        }
        const auto& first = analysis.events.front();
        EXPECT_FALSE(HubertPhonemeModel::is_vowel(first.phoneme));
        EXPECT_GT(output.data(static_cast<Eigen::Index>(first.frame), 1), output.data(static_cast<Eigen::Index>(first.frame), 0));

        const auto regions = Yantra::phoneme_regions(analysis, 0, 0);
        ASSERT_EQ(regions.regions.size(), analysis.events.size());
        EXPECT_EQ(regions.regions.front().label, std::optional<std::string>("G"));
        ASSERT_NE(regions.regions.front().get(Yantra::phoneme_class), nullptr);
        EXPECT_EQ(*regions.regions.front().get(Yantra::phoneme_class), first.phoneme);
        EXPECT_NE(analysis.posterior_tensor("posteriors"), nullptr);
        EXPECT_NE(analysis.embedding_tensor("states"), nullptr);
    }

    TEST(HubertPhonemeLayoutTest, FramesFollowTheConvolutionLengths)
    {
        EXPECT_EQ(HubertPhonemeModel::frame_count(399), 0U);
        EXPECT_EQ(HubertPhonemeModel::frame_count(400), 1U);
        EXPECT_EQ(HubertPhonemeModel::frame_count(719), 1U);
        EXPECT_EQ(HubertPhonemeModel::frame_count(720), 2U);
        EXPECT_EQ(HubertPhonemeModel::frame_count(32000), 99U);
        EXPECT_EQ(HubertPhonemeModel::frame_count(48000), 149U);
    }

    TEST(HubertPhonemeLayoutTest, ClassesAreNamedAndVowelsAreKnown)
    {
        EXPECT_EQ(HubertPhonemeModel::label(0), "AA");
        EXPECT_EQ(HubertPhonemeModel::label(38), "ZH");
        EXPECT_EQ(HubertPhonemeModel::label(HubertPhonemeModel::blank_class), "[PAD]");
        EXPECT_TRUE(HubertPhonemeModel::label(HubertPhonemeModel::classes).empty());
        size_t vowels = 0;
        for (uint32_t index = 0; index < HubertPhonemeModel::classes; ++index) {
            vowels += HubertPhonemeModel::is_vowel(index) ? 1 : 0;
        }
        EXPECT_EQ(vowels, 15U);
        EXPECT_TRUE(HubertPhonemeModel::is_vowel(17));
        EXPECT_FALSE(HubertPhonemeModel::is_vowel(28));
        EXPECT_FALSE(HubertPhonemeModel::is_vowel(HubertPhonemeModel::blank_class));
    }

    TEST(HubertPhonemeWeightsTest, DiagnoseNamesTheFirstProblem)
    {
        const auto& weights = hubert_weights();
        if (!weights) {
            GTEST_SKIP() << "data/tensors/hubert_phoneme/hubert_base_phoneme_en.safetensors is not present";
        }
        EXPECT_TRUE(HubertPhonemeModel::diagnose(*weights).empty());
        EXPECT_EQ(weights->tensors.size(), 212U);

        EXPECT_NE(HubertPhonemeModel::diagnose(TensorSet {}).find("missing tensor 'hubert.feature_extractor.conv_layers.0.conv.weight'"), std::string::npos);

        TensorSet incomplete = *weights;
        std::erase_if(incomplete.tensors, [](const Kakshya::TensorData& tensor) {
            return tensor.spec().name == "hubert.encoder.layers.7.attention.k_proj.bias";
        });
        EXPECT_NE(HubertPhonemeModel::diagnose(incomplete).find("hubert.encoder.layers.7.attention.k_proj.bias"), std::string::npos);
    }

    TEST(HubertPhonemeWeightsTest, TheAnalyzerRejectsWeightsThatDoNotFitAndAnotherRate)
    {
        EXPECT_THROW(StandardPhonemeAnalyzer(TensorSet {}), std::invalid_argument);

        PhonemeAnalyzerConfig config;
        EXPECT_TRUE(StandardPhonemeAnalyzer::diagnose(config).empty());
        config.sample_rate = 48000.0;
        EXPECT_NE(StandardPhonemeAnalyzer::diagnose(config).find("48000"), std::string::npos);
        const auto& weights = hubert_weights();
        if (weights) {
            EXPECT_THROW(StandardPhonemeAnalyzer(*weights, config), std::invalid_argument);
        }
    }

} // namespace
} // namespace MayaFlux::Test
