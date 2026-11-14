#include "gtest/gtest.h"
#include "../../support/test_metadata.hpp"

#include "MayaFlux/IO/Tensor/SafetensorsFormat.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::silero_weights>() }))

namespace MayaFlux::Test {
namespace {

    namespace fs = std::filesystem;

    using IO::SafetensorsReader;
    using Kakshya::TensorDType;

    const fs::path k_weights = "data/tensors/silero/silero_vad_16k.safetensors";

    struct Expected {
        const char* name;
        std::vector<uint64_t> shape;
    };

    const std::vector<Expected>& expected_tensors()
    {
        static const std::vector<Expected> tensors {
            { "stft_conv.weight", { 258, 1, 256 } },
            { "conv1.weight", { 128, 129, 3 } },
            { "conv1.bias", { 128 } },
            { "conv2.weight", { 64, 128, 3 } },
            { "conv2.bias", { 64 } },
            { "conv3.weight", { 64, 64, 3 } },
            { "conv3.bias", { 64 } },
            { "conv4.weight", { 128, 64, 3 } },
            { "conv4.bias", { 128 } },
            { "lstm_cell.weight_ih", { 512, 128 } },
            { "lstm_cell.weight_hh", { 512, 128 } },
            { "lstm_cell.bias_ih", { 512 } },
            { "lstm_cell.bias_hh", { 512 } },
            { "final_conv.weight", { 1, 128, 1 } },
            { "final_conv.bias", { 1 } },
        };
        return tensors;
    }

    class SileroWeightsTest : public ::testing::Test {
    protected:
        void SetUp() override
        {
            if (!fs::exists(k_weights)) {
                GTEST_SKIP() << k_weights.string() << " is not present";
            }
        }
    };

    TEST_F(SileroWeightsTest, TheSafetensorsReaderListsEveryTensorWithItsShapeAndType)
    {
        SafetensorsReader reader;
        ASSERT_TRUE(reader.open(k_weights.string())) << reader.get_last_error();

        const auto specs = reader.describe();

        const auto& expected = expected_tensors();
        ASSERT_EQ(specs.size(), expected.size());
        for (size_t i = 0; i < expected.size(); ++i) {
            EXPECT_EQ(specs[i].name, expected[i].name);
            EXPECT_EQ(specs[i].shape, expected[i].shape) << expected[i].name;
            EXPECT_EQ(specs[i].dtype, TensorDType::of<float>()) << expected[i].name;
        }
        EXPECT_TRUE(reader.tensor_metadata().empty());
    }

    TEST_F(SileroWeightsTest, EveryTensorReadsWithFiniteValuesAndTheParameterCountMatches)
    {
        SafetensorsReader reader;
        ASSERT_TRUE(reader.open(k_weights.string())) << reader.get_last_error();

        const auto set = reader.read_tensors();

        ASSERT_TRUE(set.has_value()) << reader.get_last_error();
        EXPECT_TRUE(set->is_consistent());
        uint64_t parameters = 0;
        for (const auto& expected : expected_tensors()) {
            const auto* tensor = set->find(expected.name);
            ASSERT_NE(tensor, nullptr) << expected.name;
            const auto values = tensor->to_vector<float>();
            ASSERT_TRUE(values.has_value()) << expected.name;
            parameters += values->size();
            for (const float value : *values) {
                ASSERT_TRUE(std::isfinite(value)) << expected.name;
            }
        }
        EXPECT_EQ(parameters, 309633U);
    }

    TEST_F(SileroWeightsTest, ASingleTensorCanBeReadByName)
    {
        SafetensorsReader reader;
        ASSERT_TRUE(reader.open(k_weights.string())) << reader.get_last_error();

        const auto bias = reader.read("final_conv.bias");

        ASSERT_TRUE(bias.has_value());
        EXPECT_EQ(bias->spec().shape, std::vector<uint64_t> { 1 });
        EXPECT_FALSE(reader.read("missing.weight").has_value());
    }

}
}
