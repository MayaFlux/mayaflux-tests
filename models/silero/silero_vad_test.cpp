#include "gtest/gtest.h"

#include <filesystem>
#include <random>

#include "../../support/gpu_dispatch_test_support.hpp"

#include "MayaFlux/IO/Tensor/SafetensorsFormat.hpp"
#include "MayaFlux/Yantra/Analyzers/VadAnalyzer.hpp"
#include "MayaFlux/Yantra/Executors/Audio/SileroVadModel.hpp"

namespace MayaFlux::Test {
namespace {

    namespace fs = std::filesystem;

    using namespace GpuDispatch;
    using IO::SafetensorsReader;
    using Kakshya::TensorData;
    using Kakshya::TensorSet;
    using Yantra::VadAnalyzerConfig;
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

    std::vector<double> doubles_of(const TensorSet& set, const char* name)
    {
        const auto values = set.find(name)->to_vector<float>().value_or(std::vector<float> {});
        return { values.begin(), values.end() };
    }

    std::vector<double> conv1d(const std::vector<double>& x, size_t c_in, size_t t_in, const std::vector<double>& w,
        const std::vector<double>& bias, size_t c_out, size_t k, size_t stride, size_t pad, bool relu)
    {
        const size_t t_out = (t_in + 2 * pad - k) / stride + 1;
        std::vector<double> out(c_out * t_out, 0.0);
        for (size_t co = 0; co < c_out; ++co) {
            for (size_t to = 0; to < t_out; ++to) {
                double acc = bias.empty() ? 0.0 : bias[co];
                for (size_t ci = 0; ci < c_in; ++ci) {
                    for (size_t tap = 0; tap < k; ++tap) {
                        const long ti = static_cast<long>(to * stride + tap) - static_cast<long>(pad);
                        if (ti < 0 || ti >= static_cast<long>(t_in)) {
                            continue;
                        }
                        acc += w[(co * c_in + ci) * k + tap] * x[ci * t_in + static_cast<size_t>(ti)];
                    }
                }
                out[co * t_out + to] = relu ? std::max(acc, 0.0) : acc;
            }
        }
        return out;
    }

    double sigmoid(double v)
    {
        return 1.0 / (1.0 + std::exp(-v));
    }

    class HostSilero {
    public:
        explicit HostSilero(const TensorSet& set)
            : m_stft(doubles_of(set, "stft_conv.weight"))
            , m_c1w(doubles_of(set, "conv1.weight"))
            , m_c1b(doubles_of(set, "conv1.bias"))
            , m_c2w(doubles_of(set, "conv2.weight"))
            , m_c2b(doubles_of(set, "conv2.bias"))
            , m_c3w(doubles_of(set, "conv3.weight"))
            , m_c3b(doubles_of(set, "conv3.bias"))
            , m_c4w(doubles_of(set, "conv4.weight"))
            , m_c4b(doubles_of(set, "conv4.bias"))
            , m_wih(doubles_of(set, "lstm_cell.weight_ih"))
            , m_whh(doubles_of(set, "lstm_cell.weight_hh"))
            , m_bih(doubles_of(set, "lstm_cell.bias_ih"))
            , m_bhh(doubles_of(set, "lstm_cell.bias_hh"))
            , m_fw(doubles_of(set, "final_conv.weight"))
            , m_fb(doubles_of(set, "final_conv.bias")[0])
            , m_tail(64, 0.0)
            , m_h(128, 0.0)
            , m_c(128, 0.0)
        {
        }

        double step(std::span<const double> chunk)
        {
            std::vector<double> sequence(m_tail);
            sequence.insert(sequence.end(), chunk.begin(), chunk.end());
            std::vector<double> padded(sequence);
            for (size_t j = 0; j < 64; ++j) {
                padded.push_back(sequence[574 - j]);
            }

            const auto transform = conv1d(padded, 1, 640, m_stft, {}, 258, 256, 128, 0, false);
            std::vector<double> magnitude(129 * 4);
            for (size_t c = 0; c < 129; ++c) {
                for (size_t t = 0; t < 4; ++t) {
                    const double re = transform[c * 4 + t];
                    const double im = transform[(129 + c) * 4 + t];
                    magnitude[c * 4 + t] = std::sqrt(re * re + im * im);
                }
            }
            const auto e1 = conv1d(magnitude, 129, 4, m_c1w, m_c1b, 128, 3, 1, 1, true);
            const auto e2 = conv1d(e1, 128, 4, m_c2w, m_c2b, 64, 3, 2, 1, true);
            const auto e3 = conv1d(e2, 64, 2, m_c3w, m_c3b, 64, 3, 2, 1, true);
            const auto x = conv1d(e3, 64, 1, m_c4w, m_c4b, 128, 3, 1, 1, true);

            std::vector<double> gates(512);
            for (size_t r = 0; r < 512; ++r) {
                double acc = m_bih[r] + m_bhh[r];
                for (size_t j = 0; j < 128; ++j) {
                    acc += m_wih[r * 128 + j] * x[j] + m_whh[r * 128 + j] * m_h[j];
                }
                gates[r] = acc;
            }
            for (size_t u = 0; u < 128; ++u) {
                const double i = sigmoid(gates[u]);
                const double f = sigmoid(gates[128 + u]);
                const double g = std::tanh(gates[256 + u]);
                const double o = sigmoid(gates[384 + u]);
                m_c[u] = f * m_c[u] + i * g;
                m_h[u] = o * std::tanh(m_c[u]);
            }

            double logit = m_fb;
            for (size_t u = 0; u < 128; ++u) {
                logit += m_fw[u] * std::max(m_h[u], 0.0);
            }
            m_tail.assign(sequence.end() - 64, sequence.end());
            return sigmoid(logit);
        }

    private:
        std::vector<double> m_stft, m_c1w, m_c1b, m_c2w, m_c2b, m_c3w, m_c3b, m_c4w, m_c4b;
        std::vector<double> m_wih, m_whh, m_bih, m_bhh, m_fw;
        double m_fb;
        std::vector<double> m_tail, m_h, m_c;
    };

    std::vector<float> test_audio(size_t chunks)
    {
        std::mt19937_64 engine(11);
        std::uniform_real_distribution<double> noise(-1.0, 1.0);
        std::vector<float> out(chunks * 512);
        for (size_t i = 0; i < out.size(); ++i) {
            const size_t chunk = i / 512;
            double value = 0.0;
            if (chunk % 4 == 0) {
                value = 0.2 * noise(engine);
            } else if (chunk % 4 == 1) {
                for (int h = 1; h <= 8; ++h) {
                    value += std::sin(2.0 * std::numbers::pi * h * static_cast<double>(i) / 100.0) / h;
                }
                value *= 0.4;
            } else if (chunk % 4 == 2) {
                value = 0.0;
            } else {
                value = 0.6 * noise(engine);
            }
            out[i] = static_cast<float>(value);
        }
        return out;
    }

    template <typename Fixture>
    class SileroVadGpuTest : public Fixture { };

    using HardwareOptions = ::testing::Types<GpuDispatchFixture<true>, GpuDispatchFixture<false>>;
    TYPED_TEST_SUITE(SileroVadGpuTest, HardwareOptions);

    TYPED_TEST(SileroVadGpuTest, ProbabilitiesMatchAnIndependentHostForwardPass)
    {
        const auto weights = load_weights();
        if (!weights) {
            GTEST_SKIP() << "data/tensors/silero/silero_vad_16k.safetensors is not present";
        }
        const auto audio = test_audio(48);
        SileroVadModel model(*weights);

        const auto got = model.process(audio);

        HostSilero host(*weights);
        ASSERT_EQ(got.size(), 48U);
        for (size_t n = 0; n < got.size(); ++n) {
            std::vector<double> chunk(audio.begin() + static_cast<long>(n * 512), audio.begin() + static_cast<long>((n + 1) * 512));
            EXPECT_NEAR(got[n], host.step(chunk), 1.0e-3) << "chunk " << n;
        }
    }

    TYPED_TEST(SileroVadGpuTest, ASplitStreamGivesTheSameProbabilitiesAsOneCall)
    {
        const auto weights = load_weights();
        if (!weights) {
            GTEST_SKIP() << "data/tensors/silero/silero_vad_16k.safetensors is not present";
        }
        const auto audio = test_audio(20);
        SileroVadModel whole(*weights);
        SileroVadModel split(*weights);

        const auto want = whole.process(audio);
        auto got = split.process(std::span<const float>(audio).first(700));
        const auto rest = split.process(std::span<const float>(audio).subspan(700));
        got.insert(got.end(), rest.begin(), rest.end());

        ASSERT_EQ(got.size(), want.size());
        for (size_t n = 0; n < want.size(); ++n) {
            EXPECT_NEAR(got[n], want[n], 1.0e-5) << "chunk " << n;
        }
    }

    TYPED_TEST(SileroVadGpuTest, ACompletedChunkLeavesThePartialOneWaitingUntilFlushed)
    {
        const auto weights = load_weights();
        if (!weights) {
            GTEST_SKIP() << "data/tensors/silero/silero_vad_16k.safetensors is not present";
        }
        auto audio = test_audio(2);
        SileroVadModel partial(*weights);
        SileroVadModel padded(*weights);

        const auto first = partial.process(std::span<const float>(audio).first(1000));
        EXPECT_EQ(first.size(), 1U);
        EXPECT_EQ(partial.pending(), 488U);
        const auto flushed = partial.flush();
        EXPECT_EQ(partial.pending(), 0U);
        EXPECT_TRUE(partial.flush().empty());

        std::fill(audio.begin() + 1000, audio.end(), 0.0F);
        const auto want = padded.process(audio);
        ASSERT_EQ(flushed.size(), 1U);
        ASSERT_EQ(want.size(), 2U);
        EXPECT_NEAR(first[0], want[0], 1.0e-5);
        EXPECT_NEAR(flushed[0], want[1], 1.0e-5);
    }

    TYPED_TEST(SileroVadGpuTest, ResetRestoresTheInitialState)
    {
        const auto weights = load_weights();
        if (!weights) {
            GTEST_SKIP() << "data/tensors/silero/silero_vad_16k.safetensors is not present";
        }
        const auto audio = test_audio(12);
        SileroVadModel model(*weights);

        const auto first = model.process(audio);
        model.reset();
        const auto second = model.process(audio);

        ASSERT_EQ(first.size(), second.size());
        for (size_t n = 0; n < first.size(); ++n) {
            EXPECT_EQ(first[n], second[n]);
        }
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

    TYPED_TEST(SileroVadGpuTest, ProbabilitiesMatchTheOriginalNetworkOnTheReferenceSignal)
    {
        const auto weights = load_weights();
        const auto audio = read_floats("tests/models/silero/silero_reference_audio.f32");
        const auto want = read_floats("tests/models/silero/silero_reference_probabilities.f32");
        if (!weights || audio.empty() || want.empty()) {
            GTEST_SKIP() << "the Silero weights or the reference fixture are not present";
        }
        ASSERT_EQ(audio.size(), want.size() * 512);
        SileroVadModel model(*weights);

        const auto got = model.process(audio);

        ASSERT_EQ(got.size(), want.size());
        for (size_t n = 0; n < want.size(); ++n) {
            EXPECT_NEAR(got[n], want[n], 2.0e-3) << "chunk " << n;
        }
    }

    TYPED_TEST(SileroVadGpuTest, SilenceIsNotSpeech)
    {
        const auto weights = load_weights();
        if (!weights) {
            GTEST_SKIP() << "data/tensors/silero/silero_vad_16k.safetensors is not present";
        }
        SileroVadModel model(*weights);

        const auto probabilities = model.process(std::vector<float>(512 * 16, 0.0F));

        ASSERT_EQ(probabilities.size(), 16U);
        for (const float p : probabilities) {
            EXPECT_GE(p, 0.0F);
            EXPECT_LT(p, 0.5F);
        }
    }

    TYPED_TEST(SileroVadGpuTest, TheAnalyzerContinuesAStreamAndFlushesTheTail)
    {
        const auto weights = load_weights();
        if (!weights) {
            GTEST_SKIP() << "data/tensors/silero/silero_vad_16k.safetensors is not present";
        }
        const auto audio = test_audio(4);
        VadAnalyzerConfig config;
        config.flush_tail = true;
        StandardVadAnalyzer analyzer(*weights, config);
        std::vector<double> clip(audio.begin(), audio.begin() + 1000);
        std::vector<double> more(audio.begin() + 1000, audio.end());

        const auto first = analyzer.analyze_speech(std::vector<Kakshya::DataVariant> { clip });
        const auto second = analyzer.analyze_speech(std::vector<Kakshya::DataVariant> { more });

        ASSERT_EQ(first.probabilities.size(), 2U);
        EXPECT_EQ(first.chunk_positions[0], (std::pair<size_t, size_t> { 0, 512 }));
        EXPECT_EQ(first.chunk_positions[1], (std::pair<size_t, size_t> { 512, 1024 }));
        ASSERT_FALSE(second.probabilities.empty());
        EXPECT_EQ(second.chunk_positions[0].first, 1024U);

        analyzer.reset();
        const auto again = analyzer.analyze_speech(std::vector<Kakshya::DataVariant> { clip });
        ASSERT_EQ(again.probabilities.size(), 2U);
        EXPECT_EQ(again.chunk_positions[0].first, 0U);
        EXPECT_EQ(again.probabilities, first.probabilities);
    }

    TEST(SileroVadWeightsTest, DiagnoseNamesTheFirstProblem)
    {
        const TensorSet empty;
        EXPECT_EQ(SileroVadModel::diagnose(empty), "missing tensor 'stft_conv.weight'");

        auto weights = load_weights();
        if (!weights) {
            GTEST_SKIP() << "data/tensors/silero/silero_vad_16k.safetensors is not present";
        }
        EXPECT_EQ(SileroVadModel::diagnose(*weights), "");

        auto wrong = TensorData::from_values<float>(std::vector<float>(64, 0.0F), { 64 }, "conv1.bias");
        ASSERT_TRUE(wrong.has_value());
        TensorSet altered;
        for (const auto& tensor : weights->tensors) {
            if (tensor.spec().name == "conv1.bias") {
                altered.tensors.push_back(std::move(*wrong));
            } else {
                altered.tensors.push_back(tensor);
            }
        }
        EXPECT_EQ(SileroVadModel::diagnose(altered), "tensor 'conv1.bias' has shape [64] but [128] is required");
    }

    TEST(SileroVadWeightsTest, TheAnalyzerRejectsWeightsThatDoNotFit)
    {
        EXPECT_THROW(StandardVadAnalyzer { TensorSet {} }, std::invalid_argument);
    }

}
}
