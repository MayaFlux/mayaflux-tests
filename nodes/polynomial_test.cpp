#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Nodes/Generators/Polynomial.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

using Nodes::Generator::Polynomial;
using Nodes::Generator::PolynomialMode;

namespace {

    double quadratic(double x)
    {
        return 2.0 * x * x + 3.0 * x + 1.0;
    }

    std::shared_ptr<Polynomial> make_quadratic()
    {
        return std::make_shared<Polynomial>(quadratic);
    }

    Polynomial::CoefBufferFunction counter()
    {
        return [](std::span<double>, std::span<double> coefs) -> double {
            coefs[0] += 1.0;
            return coefs[0];
        };
    }

}

struct EvaluationPoint {
    double x;
    double expected;
};

class DirectEvaluationTest : public ::testing::TestWithParam<EvaluationPoint> { };

TEST_P(DirectEvaluationTest, FunctionNodeEvaluatesTheFunction)
{
    const auto [x, expected] = GetParam();

    EXPECT_DOUBLE_EQ(make_quadratic()->process_sample(x), expected);
}

TEST_P(DirectEvaluationTest, CoefficientNodeEvaluatesTheSameCurve)
{
    const auto [x, expected] = GetParam();
    Polynomial node(std::vector<double> { 2.0, 3.0, 1.0 });

    EXPECT_DOUBLE_EQ(node.process_sample(x), expected);
}

INSTANTIATE_TEST_SUITE_P(
    Quadratic,
    DirectEvaluationTest,
    ::testing::Values(
        EvaluationPoint { 2.0, 15.0 },
        EvaluationPoint { -1.0, 0.0 },
        EvaluationPoint { 0.0, 1.0 },
        EvaluationPoint { 0.5, 3.0 },
        EvaluationPoint { -2.5, 6.0 }));

TEST(PolynomialDirectTest, HasNoHistoryWindow)
{
    const auto node = make_quadratic();

    EXPECT_EQ(node->get_mode(), PolynomialMode::DIRECT);
    EXPECT_EQ(node->get_buffer_size(), 0U);
}

TEST(PolynomialDirectTest, BatchEvaluatesAtZeroInput)
{
    const auto batch = make_quadratic()->process_batch(10);

    ASSERT_EQ(batch.size(), 10U);
    for (const double sample : batch) {
        EXPECT_DOUBLE_EQ(sample, 1.0);
    }
}

TEST(PolynomialDirectTest, BatchRunsAStatefulFunctionInOrder)
{
    auto calls = std::make_shared<int>(0);
    Polynomial node([calls](double) { return static_cast<double>((*calls)++); });

    const auto batch = node.process_batch(10);

    for (size_t i = 0; i < batch.size(); ++i) {
        EXPECT_DOUBLE_EQ(batch.at(i), static_cast<double>(i));
    }
}

TEST(PolynomialDirectTest, TickCallbackFiresForEverySample)
{
    const auto node = make_quadratic();
    int calls = 0;
    double last = 0.0;

    node->on_tick([&calls, &last](const Nodes::NodeContext& ctx) {
        ++calls;
        last = ctx.value;
    });

    const double result = node->process_sample(2.0);
    EXPECT_EQ(calls, 1);
    EXPECT_DOUBLE_EQ(last, result);

    node->process_batch(5);
    EXPECT_EQ(calls, 6);
}

TEST(PolynomialDirectTest, CoefficientNodeFollowsSetCoefficients)
{
    Polynomial node(std::vector<double> { 2.0, 3.0, 1.0 });

    EXPECT_DOUBLE_EQ(node.process_sample(2.0), 15.0);

    node.set_coefficients({ 1.0, 0.0, 0.0 });
    EXPECT_DOUBLE_EQ(node.process_sample(2.0), 4.0);

    node.set_coefficients({ 0.0, 0.0, 9.0 });
    EXPECT_DOUBLE_EQ(node.process_sample(2.0), 9.0);
}

TEST(PolynomialDirectTest, FunctionNodeIgnoresSetCoefficients)
{
    Polynomial node([](double x) { return x * x; });

    node.set_coefficients({ 5.0 });

    EXPECT_DOUBLE_EQ(node.process_sample(3.0), 9.0);
}

TEST(PolynomialDirectTest, SetDirectFunctionOverridesACoefficientNode)
{
    Polynomial node(std::vector<double> { 2.0, 3.0, 1.0 });

    node.set_direct_function([](double) { return 7.0; });
    EXPECT_DOUBLE_EQ(node.process_sample(2.0), 7.0);

    node.set_coefficients({ 1.0, 0.0, 0.0 });
    EXPECT_DOUBLE_EQ(node.process_sample(2.0), 7.0);
}

struct DifferenceCase {
    const char* name;
    PolynomialMode mode;
    size_t window;
    Polynomial::BufferFunction plain;
    Polynomial::CoefBufferFunction with_coefs;
    std::vector<double> coefs;
    std::vector<double> inputs;
    std::vector<double> expected;
};

std::vector<DifferenceCase> difference_cases()
{
    return {
        {
            .name = "FeedforwardTwoTap",
            .mode = PolynomialMode::FEEDFORWARD,
            .window = 2,
            .plain = [](std::span<double> w) -> double { return 0.7 * w[0] + 0.3 * w[1]; },
            .with_coefs = [](std::span<double> w, std::span<double> c) -> double { return c[0] * w[0] + c[1] * w[1]; },
            .coefs = { 0.7, 0.3 },
            .inputs = { 1.0, 2.0, 3.0 },
            .expected = { 0.7, 1.7, 2.7 },
        },
        {
            .name = "FeedforwardDifference",
            .mode = PolynomialMode::FEEDFORWARD,
            .window = 2,
            .plain = [](std::span<double> w) -> double { return w[0] - w[1]; },
            .with_coefs = [](std::span<double> w, std::span<double> c) -> double { return c[0] * w[0] + c[1] * w[1]; },
            .coefs = { 1.0, -1.0 },
            .inputs = { 3.0, 5.0, 4.0 },
            .expected = { 3.0, 2.0, -1.0 },
        },
        {
            .name = "RecursiveTwoPole",
            .mode = PolynomialMode::RECURSIVE,
            .window = 3,
            .plain = [](std::span<double> w) -> double { return w[0] + 0.5 * w[1] + 0.2 * w[2]; },
            .with_coefs = [](std::span<double> w, std::span<double> c) -> double { return w[0] + c[0] * w[1] + c[1] * w[2]; },
            .coefs = { 0.5, 0.2 },
            .inputs = { 1.0, 1.0, 1.0 },
            .expected = { 1.0, 1.5, 1.95 },
        },
        {
            .name = "RecursiveOnePoleImpulse",
            .mode = PolynomialMode::RECURSIVE,
            .window = 2,
            .plain = [](std::span<double> w) -> double { return w[0] + 0.5 * w[1]; },
            .with_coefs = [](std::span<double> w, std::span<double> c) -> double { return w[0] + c[0] * w[1]; },
            .coefs = { 0.5 },
            .inputs = { 1.0, 0.0, 0.0, 0.0 },
            .expected = { 1.0, 0.5, 0.25, 0.125 },
        },
    };
}

class DifferenceEquationTest : public ::testing::TestWithParam<DifferenceCase> { };

TEST_P(DifferenceEquationTest, PlainFunctionFollowsTheEquation)
{
    const auto& c = GetParam();
    auto node = std::make_shared<Polynomial>(c.plain, c.mode, c.window);

    EXPECT_EQ(node->get_mode(), c.mode);
    EXPECT_EQ(node->get_buffer_size(), c.window);
    for (size_t n = 0; n < c.inputs.size(); ++n) {
        EXPECT_NEAR(node->process_sample(c.inputs.at(n)), c.expected.at(n), 1e-12) << "n=" << n;
    }
}

TEST_P(DifferenceEquationTest, CoefFunctionGivesTheSameSequence)
{
    const auto& c = GetParam();
    auto node = std::make_shared<Polynomial>(c.with_coefs, c.mode, c.window, c.coefs);

    for (size_t n = 0; n < c.inputs.size(); ++n) {
        EXPECT_NEAR(node->process_sample(c.inputs.at(n)), c.expected.at(n), 1e-12) << "n=" << n;
    }
}

TEST_P(DifferenceEquationTest, ResetReturnsToTheStartingSequence)
{
    const auto& c = GetParam();
    auto node = std::make_shared<Polynomial>(c.with_coefs, c.mode, c.window, c.coefs);

    for (const double input : c.inputs) {
        node->process_sample(input);
    }
    node->reset();

    for (size_t n = 0; n < c.inputs.size(); ++n) {
        EXPECT_NEAR(node->process_sample(c.inputs.at(n)), c.expected.at(n), 1e-12) << "n=" << n;
    }
}

INSTANTIATE_TEST_SUITE_P(
    Equations,
    DifferenceEquationTest,
    ::testing::ValuesIn(difference_cases()),
    [](const ::testing::TestParamInfo<DifferenceCase>& info) { return std::string(info.param.name); });

TEST(PolynomialBufferTest, InitialConditionsSeedTheHistoryNewestFirst)
{
    Polynomial node(
        [](std::span<double> w) -> double { return w[0] + 0.5 * w[1] + 0.2 * w[2]; },
        PolynomialMode::RECURSIVE, 3);

    node.set_initial_conditions({ 4.0, 0.0, 0.0 });

    EXPECT_DOUBLE_EQ(node.process_sample(0.0), 2.0);
}

TEST(PolynomialCoefArrayTest, DefaultsToEmpty)
{
    Polynomial node(
        [](std::span<double>, std::span<double> coefs) -> double { return static_cast<double>(coefs.size()); },
        PolynomialMode::FEEDFORWARD, 1);

    EXPECT_TRUE(node.get_coefficients().empty());
    EXPECT_DOUBLE_EQ(node.process_sample(1.0), 0.0);
}

TEST(PolynomialCoefArrayTest, ReplacedLive)
{
    Polynomial node(
        [](std::span<double> w, std::span<double> c) -> double { return c[0] * w[0]; },
        PolynomialMode::FEEDFORWARD, 1, std::vector<double> { 2.0 });

    EXPECT_DOUBLE_EQ(node.process_sample(1.0), 2.0);

    node.set_coefficients({ 5.0 });

    EXPECT_DOUBLE_EQ(node.process_sample(1.0), 5.0);
}

TEST(PolynomialCoefArrayTest, WritableFromTheFunctionAndKeptAcrossReset)
{
    Polynomial node(counter(), PolynomialMode::FEEDFORWARD, 1, std::vector<double> { 0.0 });

    EXPECT_DOUBLE_EQ(node.process_sample(0.0), 1.0);
    EXPECT_DOUBLE_EQ(node.process_sample(0.0), 2.0);

    node.reset();

    EXPECT_DOUBLE_EQ(node.get_coefficients().front(), 2.0);
    EXPECT_DOUBLE_EQ(node.process_sample(0.0), 3.0);
}

TEST(PolynomialCoefArrayTest, RestoredWithSavedState)
{
    Polynomial node(counter(), PolynomialMode::FEEDFORWARD, 1, std::vector<double> { 0.0 });

    node.process_sample(0.0);
    node.save_state();
    node.process_sample(0.0);
    node.process_sample(0.0);
    EXPECT_DOUBLE_EQ(node.get_coefficients().front(), 3.0);

    node.restore_state();

    EXPECT_DOUBLE_EQ(node.get_coefficients().front(), 1.0);
    EXPECT_DOUBLE_EQ(node.process_sample(0.0), 2.0);
}

TEST(PolynomialCoefArrayTest, NotSavedForANodeWithoutACoefFunction)
{
    Polynomial node(std::vector<double> { 2.0, 3.0, 1.0 });

    node.save_state();
    node.set_coefficients({ 7.0 });
    node.restore_state();

    EXPECT_EQ(node.get_coefficients().size(), 1U);
    EXPECT_DOUBLE_EQ(node.get_coefficients().front(), 7.0);
}

TEST(PolynomialCoefArrayTest, SetBufferFunctionOverloadsReplaceEachOther)
{
    Polynomial node(
        [](std::span<double> w) -> double { return w[0]; },
        PolynomialMode::FEEDFORWARD, 1);

    EXPECT_DOUBLE_EQ(node.process_sample(2.0), 2.0);

    node.set_coefficients({ 10.0 });
    node.set_buffer_function(
        [](std::span<double> w, std::span<double> c) -> double { return c[0] * w[0]; },
        PolynomialMode::FEEDFORWARD, 1);
    EXPECT_DOUBLE_EQ(node.process_sample(2.0), 20.0);

    node.set_buffer_function(
        [](std::span<double> w) -> double { return -w[0]; },
        PolynomialMode::FEEDFORWARD, 1);
    EXPECT_DOUBLE_EQ(node.process_sample(2.0), -2.0);
}

TEST(PolynomialCoefArrayTest, SettingACoefFunctionResizesTheWindow)
{
    Polynomial node(
        [](std::span<double> w) -> double { return w[0]; },
        PolynomialMode::FEEDFORWARD, 1);

    node.set_buffer_function(
        [](std::span<double> w, std::span<double>) -> double { return static_cast<double>(w.size()); },
        PolynomialMode::FEEDFORWARD, 4);

    EXPECT_EQ(node.get_buffer_size(), 4U);
    EXPECT_DOUBLE_EQ(node.process_sample(1.0), 4.0);
}

TEST(PolynomialCoefArrayTest, SwitchingAwayStopsSavingTheArray)
{
    Polynomial node(
        [](std::span<double>, std::span<double> c) -> double { return c[0]; },
        PolynomialMode::FEEDFORWARD, 1, std::vector<double> { 5.0 });

    node.set_buffer_function(
        [](std::span<double> w) -> double { return w[0]; },
        PolynomialMode::FEEDFORWARD, 1);

    node.save_state();
    node.set_coefficients({ 9.0 });
    node.restore_state();

    EXPECT_DOUBLE_EQ(node.get_coefficients().front(), 9.0);
    EXPECT_DOUBLE_EQ(node.clone()->process_sample(4.0), 4.0);
}

TEST(PolynomialCloneTest, CoefFunctionCloneKeepsItsOwnArrayAndHistory)
{
    Polynomial node(
        [](std::span<double> w, std::span<double> c) -> double {
            c[0] += 1.0;
            return c[0] + w[1];
        },
        PolynomialMode::FEEDFORWARD, 2, std::vector<double> { 0.0 });

    node.process_sample(10.0);
    node.process_sample(20.0);

    const auto copy = node.clone();

    EXPECT_DOUBLE_EQ(copy->get_coefficients().front(), 2.0);
    EXPECT_DOUBLE_EQ(copy->process_sample(5.0), 3.0);
    EXPECT_DOUBLE_EQ(node.process_sample(5.0), 3.0 + 20.0);

    copy->process_sample(5.0);
    EXPECT_DOUBLE_EQ(copy->get_coefficients().front(), 4.0);
    EXPECT_DOUBLE_EQ(node.get_coefficients().front(), 3.0);
}

TEST(PolynomialCloneTest, KeepsEachKindOfNode)
{
    Polynomial from_coefficients(std::vector<double> { 2.0, 3.0, 1.0 });
    const auto coefficient_copy = from_coefficients.clone();
    coefficient_copy->set_coefficients({ 1.0, 0.0, 0.0 });

    EXPECT_DOUBLE_EQ(from_coefficients.process_sample(2.0), 15.0);
    EXPECT_DOUBLE_EQ(coefficient_copy->process_sample(2.0), 4.0);

    Polynomial from_function([](double x) { return x * x; });
    EXPECT_DOUBLE_EQ(from_function.clone()->process_sample(3.0), 9.0);

    Polynomial from_buffer(
        [](std::span<double> w) -> double { return -w[0]; },
        PolynomialMode::FEEDFORWARD, 1);
    const auto buffer_copy = from_buffer.clone();
    EXPECT_EQ(buffer_copy->get_mode(), PolynomialMode::FEEDFORWARD);
    EXPECT_DOUBLE_EQ(buffer_copy->process_sample(2.0), -2.0);
}

namespace {

    using Nodes::Generator::PolynomialContext;
    using Nodes::Generator::PolynomialContextGpu;

    Polynomial make_window_node(size_t window)
    {
        return Polynomial([](std::span<double> view) { return view[0]; },
            PolynomialMode::FEEDFORWARD, window);
    }

class PolynomialContextTest : public ::testing::TestWithParam<bool> { };

TEST_P(PolynomialContextTest, TheHookReceivesTheContextTypeTheFlagSelects)
{
    auto node = make_window_node(3);
    node.set_gpu_compatible(GetParam());
    const Nodes::NodeContext* seen = nullptr;
    node.on_tick([&seen](const Nodes::NodeContext& ctx) { seen = &ctx; });

    node.process_sample(1.0);

    ASSERT_NE(seen, nullptr);
    EXPECT_EQ(seen, &node.get_last_context());
    EXPECT_NE(dynamic_cast<const PolynomialContext*>(seen), nullptr);
    EXPECT_EQ(dynamic_cast<const PolynomialContextGpu*>(seen) != nullptr, GetParam());
    EXPECT_EQ(dynamic_cast<const Nodes::GpuVectorData*>(seen) != nullptr, GetParam());
}

TEST_P(PolynomialContextTest, TheContextReportsTheNodeState)
{
    auto node = make_window_node(3);
    node.set_gpu_compatible(GetParam());
    double value = 0.0;
    PolynomialMode mode = PolynomialMode::DIRECT;
    size_t buffer_size = 0;
    node.on_tick([&](const Nodes::NodeContext& ctx) {
        const auto* poly = dynamic_cast<const PolynomialContext*>(&ctx);
        ASSERT_NE(poly, nullptr);
        value = poly->value;
        mode = poly->get_mode();
        buffer_size = poly->get_buffer_size();
    });

    const double result = node.process_sample(0.75);

    EXPECT_DOUBLE_EQ(value, result);
    EXPECT_EQ(mode, PolynomialMode::FEEDFORWARD);
    EXPECT_EQ(buffer_size, 3U);
}

TEST_P(PolynomialContextTest, TheContextCarriesTheCoefficients)
{
    Polynomial node(std::vector<double> { 2.0, 3.0, 1.0 });
    node.set_gpu_compatible(GetParam());
    std::vector<double> coefficients;
    PolynomialMode mode = PolynomialMode::FEEDFORWARD;
    node.on_tick([&](const Nodes::NodeContext& ctx) {
        const auto* poly = dynamic_cast<const PolynomialContext*>(&ctx);
        ASSERT_NE(poly, nullptr);
        coefficients = poly->get_coefficients();
        mode = poly->get_mode();
    });

    node.process_sample(1.0);

    EXPECT_EQ(coefficients, (std::vector<double> { 2.0, 3.0, 1.0 }));
    EXPECT_EQ(mode, PolynomialMode::DIRECT);
}

TEST_P(PolynomialContextTest, TheContextWindowIsTheHistoryNewestFirst)
{
    auto node = make_window_node(3);
    node.set_gpu_compatible(GetParam());
    std::vector<double> input;
    std::vector<double> output;
    node.on_tick([&](const Nodes::NodeContext& ctx) {
        const auto* poly = dynamic_cast<const PolynomialContext*>(&ctx);
        ASSERT_NE(poly, nullptr);
        const auto in = poly->get_input_buffer();
        const auto out = poly->get_output_buffer();
        input.assign(in.begin(), in.end());
        output.assign(out.begin(), out.end());
    });

    for (const double sample : { 1.0, 2.0, 3.0, 4.0 }) {
        node.process_sample(sample);
    }

    ASSERT_EQ(input.size(), 3U);
    EXPECT_EQ(input, (std::vector<double> { 4.0, 3.0, 2.0 }));
    EXPECT_EQ(output, input);
}

TEST_P(PolynomialContextTest, TheFlagDoesNotChangeTheSamples)
{
    auto flagged = make_window_node(3);
    auto reference = make_window_node(3);
    flagged.set_gpu_compatible(GetParam());

    for (const double sample : { 0.1, -0.4, 0.9, 0.25, -0.7 }) {
        EXPECT_DOUBLE_EQ(flagged.process_sample(sample), reference.process_sample(sample));
    }
}

INSTANTIATE_TEST_SUITE_P(
    GpuFlag,
    PolynomialContextTest,
    ::testing::Bool(),
    [](const ::testing::TestParamInfo<bool>& info) { return info.param ? "GpuCompatible" : "SingleValue"; });

TEST(PolynomialGpuContextTest, VectorDataIsTheWindowCastToFloat)
{
    auto node = make_window_node(3);
    node.set_gpu_compatible(true);
    std::vector<float> gpu;
    std::vector<double> window;
    bool has_data = false;
    node.on_tick([&](const Nodes::NodeContext& ctx) {
        const auto* poly = dynamic_cast<const PolynomialContextGpu*>(&ctx);
        ASSERT_NE(poly, nullptr);
        const auto data = poly->gpu_data();
        gpu.assign(data.begin(), data.end());
        const auto view = poly->get_input_buffer();
        window.assign(view.begin(), view.end());
        has_data = poly->has_gpu_data();
        EXPECT_EQ(poly->gpu_data_size(), data.size());
    });

    for (const double sample : { 0.1, 0.2, 0.3, 0.4 }) {
        node.process_sample(sample);
    }

    EXPECT_TRUE(has_data);
    ASSERT_EQ(gpu.size(), window.size());
    ASSERT_EQ(gpu.size(), 3U);
    for (size_t i = 0; i < gpu.size(); ++i) {
        EXPECT_EQ(gpu[i], static_cast<float>(window[i])) << "element " << i;
    }
    EXPECT_EQ(gpu[0], 0.4F);
    EXPECT_EQ(gpu[2], 0.2F);
}

TEST(PolynomialGpuContextTest, VectorDataSlidesWithTheHistory)
{
    auto node = make_window_node(2);
    node.set_gpu_compatible(true);
    std::vector<std::pair<float, float>> windows;
    node.on_tick([&](const Nodes::NodeContext& ctx) {
        const auto* poly = dynamic_cast<const PolynomialContextGpu*>(&ctx);
        ASSERT_NE(poly, nullptr);
        const auto data = poly->gpu_data();
        ASSERT_EQ(data.size(), 2U);
        windows.emplace_back(data[0], data[1]);
    });

    for (const double sample : { 1.0, 2.0, 3.0, 4.0 }) {
        node.process_sample(sample);
    }

    ASSERT_EQ(windows.size(), 4U);
    EXPECT_EQ(windows[1], (std::pair<float, float> { 2.0F, 1.0F }));
    EXPECT_EQ(windows[2], (std::pair<float, float> { 3.0F, 2.0F }));
    EXPECT_EQ(windows[3], (std::pair<float, float> { 4.0F, 3.0F }));
}

TEST(PolynomialGpuContextTest, ADirectNodeHasNoWindowToUpload)
{
    auto node = make_quadratic();
    node->set_gpu_compatible(true);
    bool has_data = true;
    bool is_gpu = false;
    node->on_tick([&](const Nodes::NodeContext& ctx) {
        const auto* poly = dynamic_cast<const PolynomialContextGpu*>(&ctx);
        is_gpu = poly != nullptr;
        has_data = poly != nullptr && poly->has_gpu_data();
    });

    node->process_sample(2.0);

    EXPECT_TRUE(is_gpu);
    EXPECT_FALSE(has_data);
}

TEST(PolynomialGpuContextTest, TheFlagSwitchesTheContextObjectAndTheVectorCapability)
{
    auto node = make_window_node(3);
    const Nodes::NodeContext* single = &node.get_last_context();

    EXPECT_FALSE(node.has_capability(Nodes::NodeCapability::VECTOR));
    EXPECT_TRUE(node.has_capability(Nodes::NodeCapability::SCALAR));

    node.set_gpu_compatible(true);
    const Nodes::NodeContext* gpu = &node.get_last_context();

    EXPECT_NE(gpu, single);
    EXPECT_TRUE(node.has_capability(Nodes::NodeCapability::VECTOR));
    EXPECT_TRUE(node.has_capability(Nodes::NodeCapability::SCALAR));

    node.set_gpu_compatible(false);

    EXPECT_EQ(&node.get_last_context(), single);
    EXPECT_FALSE(node.has_capability(Nodes::NodeCapability::VECTOR));
    EXPECT_TRUE(node.has_capability(Nodes::NodeCapability::SCALAR));
}

}

}
