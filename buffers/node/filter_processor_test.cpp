#include "gtest/gtest.h"
#include "../../support/test_metadata.hpp"

#include "MayaFlux/Buffers/AudioBuffer.hpp"
#include "MayaFlux/Buffers/Node/FilterProcessor.hpp"
#include "MayaFlux/Nodes/Filters/FIR.hpp"
#include "MayaFlux/Nodes/Filters/IIR.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

using Buffers::AudioBuffer;
using Buffers::FilterProcessor;
using Nodes::NodeState;
using Nodes::Filters::Filter;
using Nodes::Filters::FIR;
using Nodes::Filters::IIR;

namespace {

    constexpr uint32_t k_block = 16;

    std::shared_ptr<AudioBuffer> make_block(const std::vector<double>& values)
    {
        auto buffer = std::make_shared<AudioBuffer>(0, 512);
        buffer->resize(static_cast<uint32_t>(values.size()));
        buffer->get_data() = values;
        return buffer;
    }

    std::vector<double> ramp(size_t length)
    {
        std::vector<double> x(length);
        for (size_t n = 0; n < length; ++n) {
            x.at(n) = static_cast<double>(n + 1);
        }
        return x;
    }

    std::vector<double> ones(size_t length)
    {
        return std::vector<double>(length, 1.0);
    }

    std::vector<double> convolve(const std::vector<double>& taps, const std::vector<double>& x)
    {
        std::vector<double> y(x.size(), 0.0);
        for (size_t n = 0; n < x.size(); ++n) {
            for (size_t k = 0; k < taps.size() && k <= n; ++k) {
                y.at(n) += taps.at(k) * x.at(n - k);
            }
        }
        return y;
    }

    std::shared_ptr<Filter> leaky_integrator()
    {
        return std::make_shared<IIR>(std::vector<double> { 1.0, -0.5 }, std::vector<double> { 1.0 });
    }

}

TEST(FilterProcessorTest, FIRArgumentsBuildAnOwnedFIR)
{
    FilterProcessor processor(std::vector<double> { 0.5, 0.5 });

    EXPECT_TRUE(processor.is_using_internal());
    ASSERT_NE(processor.get_filter(), nullptr);
    EXPECT_NE(std::dynamic_pointer_cast<FIR>(processor.get_filter()), nullptr);
}

TEST(FilterProcessorTest, IIRArgumentsBuildAnOwnedIIR)
{
    FilterProcessor processor(std::vector<double> { 1.0, -0.5 }, std::vector<double> { 1.0 });

    EXPECT_TRUE(processor.is_using_internal());
    ASSERT_NE(processor.get_filter(), nullptr);
    EXPECT_NE(std::dynamic_pointer_cast<IIR>(processor.get_filter()), nullptr);
}

TEST(FilterProcessorTest, ExistingFilterIsNotOwned)
{
    std::shared_ptr<Filter> filter = leaky_integrator();
    FilterProcessor processor(filter);

    EXPECT_FALSE(processor.is_using_internal());
    EXPECT_EQ(processor.get_filter(), filter);
}

TEST(FilterProcessorTest, WithoutAFilterTheBufferIsUntouched)
{
    FilterProcessor processor;
    auto buffer = make_block(ramp(k_block));

    processor.process(buffer);

    EXPECT_EQ(buffer->get_data(), ramp(k_block));
}

TEST(FilterProcessorTest, EmptyBufferIsLeftAlone)
{
    FilterProcessor processor(std::vector<double> { 0.5, 0.5 });
    auto buffer = make_block({});

    processor.process(buffer);

    EXPECT_TRUE(buffer->get_data().empty());
}

TEST(FilterProcessorTest, FIRMatchesTheConvolutionOfTheInput)
{
    const std::vector<double> taps { 0.5, 0.3, 0.2 };
    const std::vector<double> input = ramp(k_block);
    const std::vector<double> expected = convolve(taps, input);

    FilterProcessor processor(taps);
    auto buffer = make_block(input);

    processor.process(buffer);

    for (size_t n = 0; n < input.size(); ++n) {
        EXPECT_NEAR(buffer->get_data().at(n), expected.at(n), 1e-12) << "n=" << n;
    }
}

TEST(FilterProcessorTest, IIRFollowsTheDifferenceEquation)
{
    FilterProcessor processor(leaky_integrator());
    auto buffer = make_block(ones(k_block));

    processor.process(buffer);

    double previous = 0.0;
    for (size_t n = 0; n < k_block; ++n) {
        const double expected = 1.0 + 0.5 * previous;
        EXPECT_NEAR(buffer->get_data().at(n), expected, 1e-12) << "n=" << n;
        previous = expected;
    }
}

TEST(FilterProcessorTest, InactiveFilterCarriesItsStateIntoTheNextBuffer)
{
    FilterProcessor processor(leaky_integrator());
    auto first = make_block(ones(k_block));
    auto second = make_block(ones(k_block));

    processor.process(first);
    processor.process(second);

    EXPECT_GT(second->get_data().front(), first->get_data().front());
}

TEST(FilterProcessorTest, ActiveFilterIsSnapshottedAndRewoundBetweenBuffers)
{
    auto filter = leaky_integrator();
    filter->m_state.store(NodeState::ACTIVE);
    FilterProcessor processor(filter);
    auto first = make_block(ones(k_block));
    auto second = make_block(ones(k_block));

    processor.process(first);
    processor.process(second);

    EXPECT_EQ(first->get_data(), second->get_data());
}

TEST(FilterProcessorTest, AttachingResetsTheFilterHistory)
{
    FilterProcessor processor(leaky_integrator());
    auto first = make_block(ones(k_block));
    auto second = make_block(ones(k_block));
    auto third = make_block(ones(k_block));

    processor.process(first);
    processor.process(second);
    processor.on_attach(third);
    processor.process(third);

    EXPECT_EQ(third->get_data(), first->get_data());
}

TEST(FilterProcessorTest, ProcessingLeavesNoInputContextBehind)
{
    FilterProcessor processor(std::vector<double> { 0.5, 0.5 });
    auto buffer = make_block(ramp(k_block));

    processor.process(buffer);

    EXPECT_FALSE(processor.get_filter()->using_external_input_context());
}

TEST(FilterProcessorTest, UpdatedFilterTakesEffectOnTheNextBuffer)
{
    FilterProcessor processor(std::vector<double> { 1.0 });
    auto replacement = std::make_shared<FIR>(std::vector<double> { 2.0 });
    auto buffer = make_block(ramp(k_block));

    processor.update_filter_node(replacement);
    EXPECT_NE(processor.get_filter(), replacement);

    processor.process(buffer);

    EXPECT_EQ(processor.get_filter(), replacement);
    for (size_t n = 0; n < k_block; ++n) {
        EXPECT_DOUBLE_EQ(buffer->get_data().at(n), 2.0 * static_cast<double>(n + 1)) << "n=" << n;
    }
}

}
