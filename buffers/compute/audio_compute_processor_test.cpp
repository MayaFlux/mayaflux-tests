#include "gtest/gtest.h"

#include <fstream>

#include "../../support/vulkan_test_support.hpp"

#include "MayaFlux/Buffers/AudioBuffer.hpp"
#include "MayaFlux/Buffers/AudioCompute/AudioComputeProcessor.hpp"
#include "MayaFlux/Buffers/Root/RootParallelAudioBuffer.hpp"
#include "MayaFlux/Buffers/VKBuffer.hpp"

namespace MayaFlux::Test {
namespace {

    using Buffers::AudioBuffer;
    using Buffers::AudioComputeProcessor;
    using Buffers::ProcessingToken;
    using Buffers::RootParallelAudioBuffer;
    using Buffers::VKBuffer;

    constexpr uint64_t k_sign_bit = uint64_t { 1 } << 63U;

    constexpr const char* k_sign_invert_word_pair = R"(#version 460
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

    constexpr const char* k_sign_invert_native = R"(#version 460
layout(local_size_x = 64) in;
layout(set = 0, binding = 0, std430) buffer Block { double values[]; } block;
void main() {
    uint index = gl_GlobalInvocationID.x;
    if (index >= block.values.length()) {
        return;
    }
    block.values[index] = -block.values[index];
}
)";

    class TemporaryShader {
    public:
        TemporaryShader(const void* owner, const char* source)
            : m_path(std::filesystem::temp_directory_path()
                  / ("mayaflux_audio_compute_" + std::to_string(reinterpret_cast<std::uintptr_t>(owner)) + ".comp"))
        {
            std::ofstream file(m_path);
            file << source;
        }

        TemporaryShader(const TemporaryShader&) = delete;
        TemporaryShader& operator=(const TemporaryShader&) = delete;
        TemporaryShader(TemporaryShader&&) = delete;
        TemporaryShader& operator=(TemporaryShader&&) = delete;

        ~TemporaryShader()
        {
            std::error_code error;
            std::filesystem::remove(m_path, error);
        }

        [[nodiscard]] std::string path() const { return m_path.string(); }

    private:
        std::filesystem::path m_path;
    };

    uint64_t bits_of(double value)
    {
        return std::bit_cast<uint64_t>(value);
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

    void expect_bits_equal(const std::vector<double>& actual, const std::vector<double>& expected)
    {
        ASSERT_EQ(actual.size(), expected.size());
        for (size_t i = 0; i < expected.size(); ++i) {
            EXPECT_EQ(bits_of(actual[i]), bits_of(expected[i])) << "sample " << i;
        }
    }

    bool device_supports_shader_float64()
    {
        return Portal::Graphics::get_shader_foundry()
            .get_physical_device()
            .getFeatures()
            .shaderFloat64;
    }

    class AudioComputeProcessorDeviceTest : public VulkanDeviceTest {
    protected:
        struct Attachment {
            std::shared_ptr<AudioComputeProcessor> processor;
            std::shared_ptr<AudioBuffer> child;
        };

        void SetUp() override
        {
            VulkanDeviceTest::SetUp();
            if (IsSkipped())
                return;
            Portal::Graphics::get_compute_press().shutdown();
            Portal::Graphics::get_shader_foundry().shutdown();
            auto borrowed_backend = std::shared_ptr<Core::VulkanBackend>(&backend(), [](Core::VulkanBackend*) {});
            ASSERT_TRUE(Portal::Graphics::get_shader_foundry().initialize(borrowed_backend));
            ASSERT_TRUE(Portal::Graphics::get_compute_press().initialize());
        }

        void TearDown() override
        {
            for (const auto& attachment : attachments) {
                attachment.processor->on_detach(attachment.child);
            }
            attachments.clear();
            Portal::Graphics::get_compute_press().shutdown();
            Portal::Graphics::get_shader_foundry().shutdown();
            VulkanDeviceTest::TearDown();
        }

        static std::shared_ptr<AudioBuffer> make_child(const std::vector<double>& samples)
        {
            auto child = std::make_shared<AudioBuffer>(0, static_cast<uint32_t>(samples.size()));
            child->resize(static_cast<uint32_t>(samples.size()));
            std::ranges::copy(samples, child->get_data().begin());
            return child;
        }

        std::shared_ptr<AudioComputeProcessor> attach(
            const std::string& shader_path,
            const std::shared_ptr<AudioBuffer>& child,
            bool native_f64 = false,
            uint32_t workgroup_x = 64)
        {
            auto processor = std::make_shared<AudioComputeProcessor>(shader_path, native_f64, workgroup_x);
            child->set_default_processor(processor);
            attachments.push_back({ .processor = processor, .child = child });
            return processor;
        }

        std::vector<Attachment> attachments;
    };

    TEST_F(AudioComputeProcessorDeviceTest, SignInvertFlipsEverySampleBitForBit)
    {
        TemporaryShader shader(this, k_sign_invert_word_pair);
        const std::vector<double> input {
            0.0,
            -0.0,
            1.0,
            -1.0,
            std::numeric_limits<double>::infinity(),
            -std::numeric_limits<double>::infinity(),
            std::numeric_limits<double>::denorm_min(),
            -std::numeric_limits<double>::denorm_min(),
            std::numeric_limits<double>::max(),
            std::numeric_limits<double>::lowest(),
            std::bit_cast<double>(uint64_t { 0x7FF8000000000123ULL }),
        };
        auto child = make_child(input);
        attach(shader.path(), child);

        child->process_default();

        expect_bits_equal(child->get_data(), sign_flipped(input));
    }

    TEST_F(AudioComputeProcessorDeviceTest, PartialWorkgroupStaysInsideTheBlock)
    {
        TemporaryShader shader(this, k_sign_invert_word_pair);
        std::vector<double> input(100);
        for (size_t i = 0; i < input.size(); ++i) {
            input[i] = static_cast<double>(i) * 0.5 - 20.0;
        }
        auto child = make_child(input);
        attach(shader.path(), child, false, 64);

        child->process_default();

        expect_bits_equal(child->get_data(), sign_flipped(input));
    }

    TEST_F(AudioComputeProcessorDeviceTest, TheFirstCycleAlreadyDispatches)
    {
        TemporaryShader shader(this, k_sign_invert_word_pair);
        const std::vector<double> input { 0.25, -0.75, 3.5, -4.0 };
        auto child = make_child(input);
        attach(shader.path(), child);

        child->process_default();

        EXPECT_NE(bits_of(child->get_data()[0]), bits_of(input[0]));
        expect_bits_equal(child->get_data(), sign_flipped(input));
    }

    TEST_F(AudioComputeProcessorDeviceTest, TwoCyclesRestoreTheOriginalBits)
    {
        TemporaryShader shader(this, k_sign_invert_word_pair);
        const std::vector<double> input { 0.1, -0.2, 0.3, -0.4, 0.5 };
        auto child = make_child(input);
        attach(shader.path(), child);

        child->process_default();
        child->process_default();

        expect_bits_equal(child->get_data(), input);
    }

    TEST_F(AudioComputeProcessorDeviceTest, FrameCountChangesBetweenCyclesResizeTheBlock)
    {
        TemporaryShader shader(this, k_sign_invert_word_pair);
        auto child = make_child(std::vector<double>(16, 1.0));
        attach(shader.path(), child);
        child->process_default();
        expect_bits_equal(child->get_data(), std::vector<double>(16, -1.0));

        std::vector<double> grown(40);
        for (size_t i = 0; i < grown.size(); ++i) {
            grown[i] = static_cast<double>(i) + 0.125;
        }
        child->resize(40);
        std::ranges::copy(grown, child->get_data().begin());
        child->process_default();
        expect_bits_equal(child->get_data(), sign_flipped(grown));

        std::vector<double> shrunk { 2.0, -3.0, 4.5, -5.5, 6.0, -7.0, 8.0, -9.0 };
        child->resize(8);
        std::ranges::copy(shrunk, child->get_data().begin());
        child->process_default();
        expect_bits_equal(child->get_data(), sign_flipped(shrunk));
    }

    TEST_F(AudioComputeProcessorDeviceTest, ZeroLengthChildIsNotDispatched)
    {
        TemporaryShader shader(this, k_sign_invert_word_pair);
        auto child = make_child({});
        attach(shader.path(), child);

        child->process_default();

        EXPECT_TRUE(child->get_data().empty());
    }

    TEST_F(AudioComputeProcessorDeviceTest, AZeroLengthAttachThenGrowthStillDispatches)
    {
        TemporaryShader shader(this, k_sign_invert_word_pair);
        auto child = make_child({});
        attach(shader.path(), child);

        const std::vector<double> grown { 1.5, -2.5, 3.5 };
        child->resize(static_cast<uint32_t>(grown.size()));
        std::ranges::copy(grown, child->get_data().begin());
        child->process_default();

        expect_bits_equal(child->get_data(), sign_flipped(grown));
    }

    TEST_F(AudioComputeProcessorDeviceTest, MissingShaderClearsTheChildAndNeverReplaysOldSamples)
    {
        const auto missing = (std::filesystem::temp_directory_path()
            / ("mayaflux_audio_compute_missing_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".comp"))
                                 .string();
        auto child = make_child({ 3.0, -3.0, 3.0, -3.0 });
        attach(missing, child);

        child->process_default();

        ASSERT_EQ(child->get_data().size(), 4U);
        for (size_t i = 0; i < 4; ++i) {
            EXPECT_EQ(bits_of(child->get_data()[i]), bits_of(0.0)) << "sample " << i;
        }
    }

    TEST_F(AudioComputeProcessorDeviceTest, ChildUnderAParallelRootIsMixedAfterProcessing)
    {
        TemporaryShader shader(this, k_sign_invert_word_pair);
        auto root = std::make_shared<RootParallelAudioBuffer>(0);
        root->initialize();
        root->set_mix_normalization(false);

        const std::vector<double> input { 0.5, -1.5, 2.5, -3.5, 4.5, -5.5 };
        auto child = make_child(input);
        attach(shader.path(), child);
        root->add_child_buffer(child);
        ASSERT_EQ(root->get_child_buffers().size(), 1U);

        child->process_default();
        root->process_default();
        root->handle_cycle_complete();

        const auto block = root->read_block();
        const auto expected = sign_flipped(input);
        for (size_t i = 0; i < expected.size(); ++i) {
            EXPECT_EQ(bits_of(block[i]), bits_of(expected[i])) << "sample " << i;
        }
        for (size_t i = expected.size(); i < block.size(); ++i) {
            EXPECT_EQ(block[i], 0.0) << "sample " << i;
        }
    }

    TEST_F(AudioComputeProcessorDeviceTest, ReattachAfterDetachResizesTheBlockOnProcessing)
    {
        TemporaryShader shader(this, k_sign_invert_word_pair);
        auto processor = std::make_shared<AudioComputeProcessor>(shader.path(), false, 64);

        auto first = make_child({ 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0 });
        first->set_default_processor(processor);
        first->process_default();
        processor->on_detach(first);
        first->set_default_processor(nullptr);

        const std::vector<double> input(24, 0.75);
        auto second = make_child(input);
        second->set_default_processor(processor);
        attachments.push_back({ .processor = processor, .child = second });
        second->process_default();

        expect_bits_equal(second->get_data(), sign_flipped(input));
    }

    TEST_F(AudioComputeProcessorDeviceTest, ProcessingAfterDetachDoesNotDispatch)
    {
        TemporaryShader shader(this, k_sign_invert_word_pair);
        auto processor = std::make_shared<AudioComputeProcessor>(shader.path(), false, 64);
        auto child = make_child({ 1.0, 2.0, 3.0 });
        child->set_default_processor(processor);
        processor->on_detach(child);

        processor->processing_function(child);

        for (const double sample : child->get_data()) {
            EXPECT_EQ(bits_of(sample), bits_of(0.0));
        }
    }

    TEST_F(AudioComputeProcessorDeviceTest, OnlyAudioBuffersAreCompatibleAndAVKBufferIsIgnored)
    {
        TemporaryShader shader(this, k_sign_invert_word_pair);
        AudioComputeProcessor processor(shader.path(), false, 64);
        auto child = make_child({ 1.0, 2.0 });
        auto vk = std::make_shared<VKBuffer>(64, VKBuffer::Usage::HOST_STORAGE, Kakshya::DataModality::UNKNOWN);

        EXPECT_TRUE(processor.is_compatible_with(child));
        EXPECT_FALSE(processor.is_compatible_with(vk));

        processor.on_attach(vk);
        processor.processing_function(vk);
    }

    TEST_F(AudioComputeProcessorDeviceTest, ProcessorTokenIsAudioParallel)
    {
        TemporaryShader shader(this, k_sign_invert_word_pair);
        AudioComputeProcessor processor(shader.path(), false, 64);

        EXPECT_EQ(processor.get_processing_token(), ProcessingToken::AUDIO_PARALLEL);
        EXPECT_NE(processor.get_processing_token(), ProcessingToken::GRAPHICS_BACKEND);
        EXPECT_FALSE(processor.is_native_f64());
    }

    TEST_F(AudioComputeProcessorDeviceTest, NativeF64DispatchesWhenTheDeviceSupportsIt)
    {
        if (!device_supports_shader_float64()) {
            GTEST_SKIP() << "Selected device does not report shaderFloat64";
        }
        TemporaryShader shader(this, k_sign_invert_native);
        const std::vector<double> input { 0.5, -2.25, 1.0e300, -1.0e-300, 0.0, 7.0 };
        auto child = make_child(input);
        auto processor = attach(shader.path(), child, true, 64);

        child->process_default();

        EXPECT_TRUE(processor->is_native_f64());
        expect_bits_equal(child->get_data(), sign_flipped(input));
    }

    TEST_F(AudioComputeProcessorDeviceTest, NativeF64AttachIsRejectedWithoutDeviceSupport)
    {
        if (device_supports_shader_float64()) {
            GTEST_SKIP() << "Selected device reports shaderFloat64, rejection path not reachable";
        }
        TemporaryShader shader(this, k_sign_invert_native);
        auto child = make_child({ 1.0, 2.0, 3.0 });
        auto processor = std::make_shared<AudioComputeProcessor>(shader.path(), true, 64);

        EXPECT_ANY_THROW(child->set_default_processor(processor));
        EXPECT_EQ(child->get_default_processor(), nullptr);
        for (const double sample : child->get_data()) {
            EXPECT_TRUE(sample == 1.0 || sample == 2.0 || sample == 3.0);
        }
    }

}

} // namespace MayaFlux::Test
