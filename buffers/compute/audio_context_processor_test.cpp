#include "gtest/gtest.h"
#include "../../support/test_metadata.hpp"

#include "../../support/gpu_dispatch_test_support.hpp"

#include "MayaFlux/Buffers/AudioBuffer.hpp"
#include "MayaFlux/Buffers/AudioCompute/AudioContextProcessor.hpp"
#include "MayaFlux/Yantra/Executors/WordPairExecutionContext.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))

namespace MayaFlux::Test {
namespace {

    using namespace GpuDispatch;
    using Buffers::AudioBuffer;
    using Buffers::AudioContextProcessor;
    using Buffers::ProcessingToken;
    using Portal::Graphics::GpuBufferBinding;
    using Yantra::WordPairExecutionContext;

    constexpr uint64_t k_sign_bit = uint64_t { 1 } << 63U;

    constexpr std::string_view k_invert_source = R"(#version 460
layout(local_size_x = 64) in;
layout(set = 0, binding = 0, std430) buffer Block { uvec2 words[]; } block;
void main() {
    uint index = gl_GlobalInvocationID.x;
    if (index >= block.words.length()) {
        return;
    }
    block.words[index].y ^= 0x80000000u;
}
)";

    std::shared_ptr<WordPairExecutionContext> invert_context()
    {
        const auto shader = load_compute(k_invert_source);
        EXPECT_NE(shader, Portal::Graphics::INVALID_SHADER);

        auto config = compute_config(shader, 0);
        config.workgroup_size = { 64, 1, 1 };

        GpuBufferBinding block;
        block.set = 0;
        block.binding = 0;
        block.direction = GpuBufferBinding::Direction::INPUT_OUTPUT;
        block.element_type = GpuBufferBinding::ElementType::VEC2_U32;

        return std::make_shared<WordPairExecutionContext>(config, std::vector<GpuBufferBinding> { block }, false);
    }

    std::shared_ptr<WordPairExecutionContext> unrunnable_context()
    {
        GpuBufferBinding block;
        block.set = 0;
        block.binding = 0;
        block.direction = GpuBufferBinding::Direction::INPUT_OUTPUT;
        block.element_type = GpuBufferBinding::ElementType::VEC2_U32;

        return std::make_shared<WordPairExecutionContext>(
            Yantra::GpuComputeConfig {}, std::vector<GpuBufferBinding> { block }, false);
    }

    double with_sign_flipped(double value)
    {
        return std::bit_cast<double>(std::bit_cast<uint64_t>(value) ^ k_sign_bit);
    }

    std::vector<double> sign_flipped(const std::vector<double>& input)
    {
        std::vector<double> expected(input.size());
        std::ranges::transform(input, expected.begin(), with_sign_flipped);
        return expected;
    }

    std::vector<double> ramp(size_t count, double first)
    {
        std::vector<double> out(count);
        for (size_t i = 0; i < count; ++i) {
            out[i] = first + static_cast<double>(i) * 0.25;
        }
        return out;
    }

    void expect_bits_equal(const std::vector<double>& actual, const std::vector<double>& expected)
    {
        ASSERT_EQ(actual.size(), expected.size());
        for (size_t i = 0; i < expected.size(); ++i) {
            EXPECT_EQ(std::bit_cast<uint64_t>(actual[i]), std::bit_cast<uint64_t>(expected[i])) << "sample " << i;
        }
    }

    void expect_silent(const std::vector<double>& samples)
    {
        for (size_t i = 0; i < samples.size(); ++i) {
            EXPECT_EQ(std::bit_cast<uint64_t>(samples[i]), uint64_t { 0 }) << "sample " << i;
        }
    }

    template <typename Fixture>
    class AudioContextProcessorGpuTest : public Fixture {
    protected:
        static std::shared_ptr<AudioBuffer> make_child(size_t frames)
        {
            auto child = std::make_shared<AudioBuffer>(0, static_cast<uint32_t>(frames));
            child->resize(static_cast<uint32_t>(frames));
            return child;
        }

        static void run(const std::shared_ptr<AudioBuffer>& child, const std::vector<double>& input)
        {
            std::ranges::copy(input, child->get_data().begin());
            child->process_default();
            foundry().wait_submitted();
        }
    };

    using HardwareOptions = ::testing::Types<GpuDispatchFixture<true>, GpuDispatchFixture<false>>;
    TYPED_TEST_SUITE(AudioContextProcessorGpuTest, HardwareOptions);

    MAYAFLUX_TEST_WITH(TEST, AudioContextProcessorTest, ANullFactoryIsRejected, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        EXPECT_THROW(AudioContextProcessor(AudioContextProcessor::ContextFactory {}), std::invalid_argument);
    }

    TYPED_TEST(AudioContextProcessorGpuTest, DepthIsClampedToOneAndOneContextIsBuiltPerSlot)
    {
        int built = 0;
        const auto factory = [&built] {
            ++built;
            return unrunnable_context();
        };

        const AudioContextProcessor deep(factory, 3);
        EXPECT_EQ(deep.depth(), 3U);
        EXPECT_EQ(built, 3);

        built = 0;
        const AudioContextProcessor shallow(factory, 0);
        EXPECT_EQ(shallow.depth(), 1U);
        EXPECT_EQ(built, 1);
    }

    TYPED_TEST(AudioContextProcessorGpuTest, TheProcessorBelongsToTheParallelTokenAndAcceptsOnlyAudioBuffers)
    {
        const AudioContextProcessor processor(&invert_context, 1);

        EXPECT_EQ(processor.get_processing_token(), ProcessingToken::AUDIO_PARALLEL);
        EXPECT_TRUE(processor.is_compatible_with(this->make_child(8)));
        EXPECT_FALSE(processor.is_compatible_with(nullptr));
    }

    TYPED_TEST(AudioContextProcessorGpuTest, OutputLagsInputByDepthCalls)
    {
        auto child = this->make_child(100);
        auto processor = std::make_shared<AudioContextProcessor>(&invert_context, 2);
        child->set_default_processor(processor);

        const auto first = ramp(100, 1.0);
        const auto second = ramp(100, -40.0);
        const auto third = ramp(100, 7.5);
        const auto fourth = ramp(100, -0.125);

        this->run(child, first);
        expect_silent(child->get_data());
        this->run(child, second);
        expect_silent(child->get_data());
        this->run(child, third);
        expect_bits_equal(child->get_data(), sign_flipped(first));
        this->run(child, fourth);
        expect_bits_equal(child->get_data(), sign_flipped(second));

        EXPECT_EQ(processor->missed(), uint64_t { 0 });
        processor->on_detach(child);
    }

    TYPED_TEST(AudioContextProcessorGpuTest, DepthOneDeliversOnTheNextCall)
    {
        auto child = this->make_child(64);
        auto processor = std::make_shared<AudioContextProcessor>(&invert_context, 1);
        child->set_default_processor(processor);

        const auto first = ramp(64, 2.0);
        const auto second = ramp(64, -9.0);

        this->run(child, first);
        expect_silent(child->get_data());
        this->run(child, second);
        expect_bits_equal(child->get_data(), sign_flipped(first));

        processor->on_detach(child);
    }

    TYPED_TEST(AudioContextProcessorGpuTest, TheFirstDepthCallsAreSilentWithoutBeingCountedAsMissed)
    {
        auto child = this->make_child(32);
        auto processor = std::make_shared<AudioContextProcessor>(&invert_context, 3);
        child->set_default_processor(processor);

        this->run(child, ramp(32, 1.0));
        this->run(child, ramp(32, 2.0));
        this->run(child, ramp(32, 3.0));

        expect_silent(child->get_data());
        EXPECT_EQ(processor->missed(), uint64_t { 0 });

        processor->on_detach(child);
    }

    TYPED_TEST(AudioContextProcessorGpuTest, ABlockLongerThanOneWorkgroupIsProcessedWhole)
    {
        auto child = this->make_child(200);
        auto processor = std::make_shared<AudioContextProcessor>(&invert_context, 1);
        child->set_default_processor(processor);

        const auto first = ramp(200, 0.5);

        this->run(child, first);
        this->run(child, ramp(200, 1.5));

        expect_bits_equal(child->get_data(), sign_flipped(first));
        processor->on_detach(child);
    }

    TYPED_TEST(AudioContextProcessorGpuTest, AStageFunctionReceivesEachCallsSequenceAndReplacesDefaultStaging)
    {
        std::vector<uint64_t> seen;
        auto stage = [&seen](WordPairExecutionContext& context, std::span<const double> samples, uint64_t sequence) {
            seen.push_back(sequence);
            context.stage_words(0, samples);
            context.set_output_size(0, samples.size() * sizeof(uint64_t));
        };

        auto child = this->make_child(48);
        auto processor = std::make_shared<AudioContextProcessor>(&invert_context, 1, stage);
        child->set_default_processor(processor);

        const auto first = ramp(48, 4.0);

        this->run(child, first);
        this->run(child, ramp(48, 5.0));
        this->run(child, ramp(48, 6.0));

        EXPECT_EQ(seen, (std::vector<uint64_t> { 0, 1, 2 }));
        processor->on_detach(child);
    }

    TYPED_TEST(AudioContextProcessorGpuTest, ADispatchThatCannotRunLeavesTheChildSilentAndIsCounted)
    {
        auto child = this->make_child(16);
        auto processor = std::make_shared<AudioContextProcessor>(&unrunnable_context, 1);
        child->set_default_processor(processor);

        this->run(child, ramp(16, 1.0));
        expect_silent(child->get_data());
        this->run(child, ramp(16, 2.0));
        expect_silent(child->get_data());

        EXPECT_EQ(processor->missed(), uint64_t { 1 });
        processor->on_detach(child);
    }

    TYPED_TEST(AudioContextProcessorGpuTest, DetachingAfterASubmitWaitsForTheJobAndIsClean)
    {
        auto child = this->make_child(256);
        auto processor = std::make_shared<AudioContextProcessor>(&invert_context, 2);
        child->set_default_processor(processor);

        std::ranges::copy(ramp(256, 1.0), child->get_data().begin());
        child->process_default();

        EXPECT_NO_THROW(processor->on_detach(child));
        foundry().wait_submitted();
    }

    TYPED_TEST(AudioContextProcessorGpuTest, AnEmptyChildIsLeftAlone)
    {
        auto child = std::make_shared<AudioBuffer>(0, 0);
        auto processor = std::make_shared<AudioContextProcessor>(&invert_context, 1);
        child->set_default_processor(processor);

        EXPECT_NO_THROW(child->process_default());
        EXPECT_EQ(processor->missed(), uint64_t { 0 });
        processor->on_detach(child);
    }

} // namespace
} // namespace MayaFlux::Test
