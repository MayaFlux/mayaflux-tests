#include "gtest/gtest.h"
#include "../../support/test_metadata.hpp"

#include "MayaFlux/Buffers/AudioBuffer.hpp"
#include "MayaFlux/Buffers/Node/PolynomialProcessor.hpp"
#include "MayaFlux/Nodes/Generators/Polynomial.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

using Buffers::AudioBuffer;
using Buffers::PolynomialProcessor;
using Nodes::Generator::Polynomial;
using Nodes::Generator::PolynomialMode;

namespace {

    constexpr unsigned int BUFFER_SIZE = 512;

    double quadratic(double x)
    {
        return 2.0 * x * x + 3.0 * x + 1.0;
    }

    std::shared_ptr<Polynomial> make_quadratic()
    {
        return std::make_shared<Polynomial>(quadratic);
    }

    std::shared_ptr<AudioBuffer> make_block(size_t samples, double fill = 0.0, uint32_t channel = 0)
    {
        auto buffer = std::make_shared<AudioBuffer>(channel, BUFFER_SIZE);
        buffer->resize(static_cast<uint32_t>(samples));
        std::ranges::fill(buffer->get_data(), fill);
        return buffer;
    }

    Polynomial::CoefBufferFunction weights()
    {
        return [](std::span<double> window, std::span<double> coefs) -> double {
            return coefs[0] * window[0] + coefs[1] * window[1];
        };
    }

}

enum class ProcessorSource : uint8_t {
    InternalFunction,
    InternalCoefficients,
    External
};

class ProcessorSourceTest : public ::testing::TestWithParam<ProcessorSource> { };

TEST_P(ProcessorSourceTest, AppliesTheQuadraticToEverySample)
{
    std::shared_ptr<PolynomialProcessor> processor;
    switch (GetParam()) {
    case ProcessorSource::InternalFunction:
        processor = std::make_shared<PolynomialProcessor>(
            PolynomialProcessor::ProcessMode::SAMPLE_BY_SAMPLE, 64, quadratic);
        break;
    case ProcessorSource::InternalCoefficients:
        processor = std::make_shared<PolynomialProcessor>(
            PolynomialProcessor::ProcessMode::SAMPLE_BY_SAMPLE, 64, std::vector<double> { 2.0, 3.0, 1.0 });
        break;
    case ProcessorSource::External:
        processor = std::make_shared<PolynomialProcessor>(
            make_quadratic(), PolynomialProcessor::ProcessMode::SAMPLE_BY_SAMPLE);
        break;
    }

    EXPECT_EQ(processor->is_using_internal(), GetParam() != ProcessorSource::External);
    ASSERT_NE(processor->get_polynomial(), nullptr);

    auto buffer = make_block(BUFFER_SIZE);
    for (size_t i = 0; i < buffer->get_data().size(); ++i) {
        buffer->get_data().at(i) = static_cast<double>(i) / static_cast<double>(buffer->get_data().size());
    }
    const std::vector<double> original = buffer->get_data();

    processor->process(buffer);

    for (size_t i = 0; i < original.size(); ++i) {
        EXPECT_NEAR(buffer->get_data().at(i), quadratic(original.at(i)), 1e-12) << "i=" << i;
    }
}

INSTANTIATE_TEST_SUITE_P(
    Sources,
    ProcessorSourceTest,
    ::testing::Values(
        ProcessorSource::InternalFunction,
        ProcessorSource::InternalCoefficients,
        ProcessorSource::External),
    [](const ::testing::TestParamInfo<ProcessorSource>& info) {
        switch (info.param) {
        case ProcessorSource::InternalFunction:
            return std::string("InternalFunction");
        case ProcessorSource::InternalCoefficients:
            return std::string("InternalCoefficients");
        default:
            return std::string("External");
        }
    });

TEST(PolynomialProcessorTest, ExternalNodeKeepsItsGraphStateFlag)
{
    auto external = make_quadratic();
    auto processor = std::make_shared<PolynomialProcessor>(
        external, PolynomialProcessor::ProcessMode::SAMPLE_BY_SAMPLE);

    auto buffer = make_block(1, 0.5);

    const double from_graph = external->process_sample(0.5);
    Nodes::atomic_add_flag(external->m_state, Nodes::NodeState::PROCESSED);

    processor->process(buffer);

    EXPECT_DOUBLE_EQ(buffer->get_data().front(), from_graph);
    EXPECT_DOUBLE_EQ(from_graph, quadratic(0.5));
    EXPECT_TRUE(external->m_state.load() & Nodes::NodeState::PROCESSED);
}

TEST(PolynomialProcessorTest, BatchModeResetsHistoryEachBuffer)
{
    auto processor = std::make_shared<PolynomialProcessor>(
        PolynomialProcessor::ProcessMode::BATCH,
        64,
        [](std::span<double> w) -> double {
            return w[0] + 0.5 * w[1] + 0.2 * w[2];
        },
        PolynomialMode::RECURSIVE,
        3);

    auto buffer = make_block(5, 1.0);

    processor->process(buffer);
    EXPECT_NEAR(buffer->get_data().at(0), 1.0, 1e-12);
    EXPECT_NEAR(buffer->get_data().at(1), 1.5, 1e-12);
    EXPECT_NEAR(buffer->get_data().at(2), 1.95, 1e-12);

    std::ranges::fill(buffer->get_data(), 1.0);
    processor->process(buffer);
    EXPECT_NEAR(buffer->get_data().at(0), 1.0, 1e-12);
}

TEST(PolynomialProcessorTest, WindowedModeRestartsEveryWindow)
{
    auto processor = std::make_shared<PolynomialProcessor>(
        PolynomialProcessor::ProcessMode::WINDOWED,
        5,
        [](std::span<double> w) -> double { return w[0] + 0.5 * w[1]; },
        PolynomialMode::RECURSIVE,
        2);

    auto buffer = make_block(10, 1.0);

    processor->process(buffer);

    EXPECT_DOUBLE_EQ(buffer->get_data().at(0), 1.0);
    EXPECT_DOUBLE_EQ(buffer->get_data().at(1), 1.5);
    EXPECT_DOUBLE_EQ(buffer->get_data().at(5), 1.0);
    EXPECT_DOUBLE_EQ(buffer->get_data().at(6), 1.5);
}

TEST(PolynomialProcessorTest, SampleBySampleKeepsHistoryAcrossBuffers)
{
    auto processor = std::make_shared<PolynomialProcessor>(
        PolynomialProcessor::ProcessMode::SAMPLE_BY_SAMPLE,
        64,
        [](std::span<double> w) -> double { return w[0] + 0.5 * w[1]; },
        PolynomialMode::RECURSIVE,
        2);

    auto first = make_block(3, 1.0);
    processor->process(first);
    EXPECT_DOUBLE_EQ(first->get_data().at(2), 1.75);

    auto second = make_block(3, 1.0);
    processor->process(second);
    EXPECT_DOUBLE_EQ(second->get_data().at(0), 1.0 + 0.5 * 1.75);
}

TEST(PolynomialProcessorTest, BufferContextFeedforwardSeesTheBufferBehindIt)
{
    auto processor = std::make_shared<PolynomialProcessor>(
        PolynomialProcessor::ProcessMode::BUFFER_CONTEXT,
        64,
        [](std::span<double> w) -> double {
            double sum = 0.0;
            const size_t taps = std::min<size_t>(w.size(), 3UL);
            for (size_t i = 0; i < taps; ++i) {
                sum += w[i];
            }
            return sum / static_cast<double>(taps);
        },
        PolynomialMode::FEEDFORWARD,
        3);

    auto buffer = make_block(5);
    const std::vector<double> input { 1.0, 2.0, 3.0, 4.0, 5.0 };
    buffer->get_data() = input;

    processor->process(buffer);

    EXPECT_DOUBLE_EQ(buffer->get_data().at(0), 1.0);
    EXPECT_DOUBLE_EQ(buffer->get_data().at(1), 1.5);
    EXPECT_NEAR(buffer->get_data().at(2), (1.0 + 1.5 + 3.0) / 3.0, 1e-10);
    EXPECT_NEAR(buffer->get_data().at(3), (1.5 + (1.0 + 1.5 + 3.0) / 3.0 + 4.0) / 3.0, 1e-10);
}

TEST(PolynomialProcessorTest, BufferContextRecursiveFiltersInPlace)
{
    auto processor = std::make_shared<PolynomialProcessor>(
        PolynomialProcessor::ProcessMode::BUFFER_CONTEXT,
        64,
        [](std::span<double> w) -> double { return w[0] + 0.5 * w[1]; },
        PolynomialMode::RECURSIVE,
        2);

    auto buffer = make_block(4, 1.0);

    processor->process(buffer);

    EXPECT_DOUBLE_EQ(buffer->get_data().at(0), 1.0);
    EXPECT_DOUBLE_EQ(buffer->get_data().at(1), 1.5);
    EXPECT_DOUBLE_EQ(buffer->get_data().at(2), 1.75);
    EXPECT_DOUBLE_EQ(buffer->get_data().at(3), 1.875);
}

TEST(PolynomialProcessorTest, UpdatePolynomialNodeTakesEffectOnTheNextBuffer)
{
    auto processor = std::make_shared<PolynomialProcessor>(
        std::make_shared<Polynomial>([](double x) { return x * x; }),
        PolynomialProcessor::ProcessMode::SAMPLE_BY_SAMPLE);

    auto buffer = make_block(1, 2.0);
    processor->process(buffer);
    EXPECT_DOUBLE_EQ(buffer->get_data().front(), 4.0);

    processor->update_polynomial_node(std::make_shared<Polynomial>([](double x) { return x * x * x; }));

    buffer->get_data().front() = 2.0;
    processor->process(buffer);
    EXPECT_DOUBLE_EQ(buffer->get_data().front(), 8.0);
}

TEST(PolynomialProcessorTest, ForceUseInternalSwapsToAnOwnedNode)
{
    auto external = std::make_shared<Polynomial>([](double x) { return x * x; });
    auto processor = std::make_shared<PolynomialProcessor>(
        external, PolynomialProcessor::ProcessMode::SAMPLE_BY_SAMPLE);

    EXPECT_FALSE(processor->is_using_internal());
    EXPECT_EQ(processor->get_polynomial(), external);

    auto buffer = make_block(1, 2.0);
    processor->process(buffer);
    EXPECT_DOUBLE_EQ(buffer->get_data().front(), 4.0);

    processor->force_use_internal([](double x) { return x * x * x; });
    EXPECT_FALSE(processor->is_using_internal());

    buffer->get_data().front() = 2.0;
    processor->process(buffer);

    EXPECT_TRUE(processor->is_using_internal());
    EXPECT_NE(processor->get_polynomial(), external);
    EXPECT_DOUBLE_EQ(buffer->get_data().front(), 8.0);
}

TEST(PolynomialProcessorTest, CoefNodeAsExternalWeightsOverABuffer)
{
    auto node = std::make_shared<Polynomial>(
        weights(), PolynomialMode::FEEDFORWARD, 2, std::vector<double> { 0.7, 0.3 });
    auto processor = std::make_shared<PolynomialProcessor>(
        node, PolynomialProcessor::ProcessMode::SAMPLE_BY_SAMPLE);

    auto buffer = make_block(4);
    for (size_t i = 0; i < 4; ++i) {
        buffer->get_data().at(i) = static_cast<double>(i + 1);
    }

    processor->process(buffer);

    EXPECT_DOUBLE_EQ(buffer->get_data().at(0), 0.7);
    EXPECT_DOUBLE_EQ(buffer->get_data().at(1), 1.7);
    EXPECT_DOUBLE_EQ(buffer->get_data().at(2), 2.7);
    EXPECT_DOUBLE_EQ(buffer->get_data().at(3), 3.7);
}

TEST(PolynomialProcessorTest, CoefNodeRetunedBetweenBuffers)
{
    auto node = std::make_shared<Polynomial>(
        [](std::span<double> w, std::span<double> c) -> double { return c[0] * w[0]; },
        PolynomialMode::FEEDFORWARD, 1, std::vector<double> { 2.0 });
    auto processor = std::make_shared<PolynomialProcessor>(
        node, PolynomialProcessor::ProcessMode::SAMPLE_BY_SAMPLE);

    auto buffer = make_block(2, 1.0);
    processor->process(buffer);
    EXPECT_DOUBLE_EQ(buffer->get_data().at(1), 2.0);

    node->set_coefficients({ 4.0 });
    std::ranges::fill(buffer->get_data(), 1.0);
    processor->process(buffer);
    EXPECT_DOUBLE_EQ(buffer->get_data().at(1), 4.0);
}

TEST(PolynomialProcessorTest, ClonePerChannelKeepsHistorySeparate)
{
    auto node = std::make_shared<Polynomial>(
        weights(), PolynomialMode::FEEDFORWARD, 2, std::vector<double> { 0.5, 0.5 });
    auto first_processor = std::make_shared<PolynomialProcessor>(
        node, PolynomialProcessor::ProcessMode::SAMPLE_BY_SAMPLE);
    const auto second_processor = first_processor->clone();

    EXPECT_NE(second_processor->get_polynomial(), first_processor->get_polynomial());
    EXPECT_TRUE(second_processor->is_using_internal());

    auto first = make_block(4, 2.0, 0);
    auto second = make_block(4, 4.0, 1);

    first_processor->process(first);
    second_processor->process(second);

    EXPECT_DOUBLE_EQ(first->get_data().at(0), 1.0);
    EXPECT_DOUBLE_EQ(first->get_data().at(3), 2.0);
    EXPECT_DOUBLE_EQ(second->get_data().at(0), 2.0);
    EXPECT_DOUBLE_EQ(second->get_data().at(3), 4.0);
}

}
