#include "gtest/gtest.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../support/vulkan_test_support.hpp"

#include "MayaFlux/Buffers/AudioBuffer.hpp"
#include "MayaFlux/Buffers/Shaders/DescriptorBindingsProcessor.hpp"
#include "MayaFlux/Buffers/Staging/StagingUtils.hpp"
#include "MayaFlux/Nodes/Conduit/Constant.hpp"
#include "MayaFlux/Nodes/Generators/Polynomial.hpp"
#include "MayaFlux/Nodes/Generators/Sine.hpp"
#include "MayaFlux/Nodes/Network/NodeNetwork.hpp"
#include "MayaFlux/Nodes/Network/Operators/PhysicsOperator.hpp"
#include "MayaFlux/Nodes/Network/ParticleNetwork.hpp"
#include "MayaFlux/Portal/Graphics/ShaderFoundry.hpp"
#include "MayaFlux/Registry/BackendRegistry.hpp"
#include "MayaFlux/Registry/Service/BufferService.hpp"

namespace MayaFlux::Test {
namespace {

    using Buffers::DescriptorBindingsProcessor;
    using Buffers::ShaderBinding;
    using Buffers::ShaderConfig;
    using Buffers::VKBuffer;
    using Portal::Graphics::DescriptorRole;
    using BindingMode = DescriptorBindingsProcessor::ProcessingMode;

    constexpr const char* k_descriptor_shader = R"(#version 460
layout(local_size_x = 1) in;
layout(set = 0, binding = 0, std140) uniform Params { float gain; } params;
layout(set = 0, binding = 1, std430) buffer Samples { float values[]; } samples;
void main() {
    samples.values[0] = params.gain;
}
)";

    class DescriptorBindingsShaderFile {
    public:
        explicit DescriptorBindingsShaderFile(const void* owner)
            : m_path(std::filesystem::temp_directory_path()
                  / ("mayaflux_descriptor_bindings_" + std::to_string(reinterpret_cast<std::uintptr_t>(owner)) + ".comp"))
        {
            std::ofstream file(m_path);
            file << k_descriptor_shader;
        }

        DescriptorBindingsShaderFile(const DescriptorBindingsShaderFile&) = delete;
        DescriptorBindingsShaderFile& operator=(const DescriptorBindingsShaderFile&) = delete;
        DescriptorBindingsShaderFile(DescriptorBindingsShaderFile&&) = delete;
        DescriptorBindingsShaderFile& operator=(DescriptorBindingsShaderFile&&) = delete;

        ~DescriptorBindingsShaderFile()
        {
            std::error_code error;
            std::filesystem::remove(m_path, error);
        }

        [[nodiscard]] std::string path() const { return m_path.string(); }

    private:
        std::filesystem::path m_path;
    };

    class DescriptorBindingsProcessorDeviceTest : public VulkanDeviceTest {
    protected:
        void SetUp() override
        {
            VulkanDeviceTest::SetUp();
            if (IsSkipped())
                return;

            Portal::Graphics::get_shader_foundry().shutdown();
            auto borrowed = std::shared_ptr<Core::VulkanBackend>(&backend(), [](Core::VulkanBackend*) {});
            ASSERT_TRUE(Portal::Graphics::get_shader_foundry().initialize(borrowed));

            shader = std::make_unique<DescriptorBindingsShaderFile>(this);

            ShaderConfig config { shader->path() };
            config.stage = Portal::Graphics::ShaderStage::COMPUTE;
            config.bindings["params"] = ShaderBinding(0, 0, DescriptorRole::UNIFORM);
            config.bindings["samples"] = ShaderBinding(0, 1, DescriptorRole::STORAGE);
            processor = std::make_shared<DescriptorBindingsProcessor>(std::move(config));

            target = make_buffer(sizeof(float) * 4, VKBuffer::Usage::HOST_STORAGE);
        }

        void TearDown() override
        {
            auto* service = Registry::BackendRegistry::instance()
                                .get_service<Registry::Service::BufferService>();
            if (processor && service) {
                for (const char* name : { "params", "samples" }) {
                    if (auto bound = processor->get_bound_buffer(name); bound && bound->is_initialized())
                        service->destroy_buffer(bound);
                }
            }
            processor.reset();

            for (const auto& buffer : buffers) {
                if (buffer && buffer->is_initialized() && service)
                    service->destroy_buffer(buffer);
            }
            buffers.clear();
            target.reset();
            shader.reset();

            Portal::Graphics::get_shader_foundry().shutdown();
            VulkanDeviceTest::TearDown();
        }

        std::shared_ptr<VKBuffer> make_buffer(size_t size, VKBuffer::Usage usage)
        {
            auto buffer = std::make_shared<VKBuffer>(size, usage, Kakshya::DataModality::UNKNOWN);
            auto* service = Registry::BackendRegistry::instance()
                                .get_service<Registry::Service::BufferService>();
            service->initialize_buffer(buffer);
            buffers.push_back(buffer);
            return buffer;
        }

        void tick() { processor->processing_function(target); }

        auto& staged() { return target->get_pipeline_context().descriptor_buffer_bindings; }

        static std::vector<float> gpu_floats(const std::shared_ptr<VKBuffer>& source, size_t count)
        {
            std::vector<uint8_t> bytes(source->get_size_bytes());
            Buffers::download_from_gpu(source, bytes.data(), bytes.size());
            std::vector<float> values(count);
            std::memcpy(values.data(), bytes.data(), count * sizeof(float));
            return values;
        }

        std::unique_ptr<DescriptorBindingsShaderFile> shader;
        std::shared_ptr<DescriptorBindingsProcessor> processor;
        std::shared_ptr<VKBuffer> target;
        std::vector<std::shared_ptr<VKBuffer>> buffers;
    };

    // ------------------------------------------------------------------
    // Scalar nodes
    // ------------------------------------------------------------------

    TEST_F(DescriptorBindingsProcessorDeviceTest, AScalarNodeValueReachesItsDescriptorBufferAndIsStaged)
    {
        auto node = std::make_shared<Nodes::Constant>(2.5);
        processor->bind_scalar_node("gain", node, "params", 0);

        tick();

        auto gpu = processor->get_bound_buffer("params");
        ASSERT_NE(gpu, nullptr);
        EXPECT_EQ(gpu_floats(gpu, 1), std::vector<float> { 2.5F });
        ASSERT_EQ(staged().size(), 1U);
        EXPECT_EQ(staged().front().set, 0U);
        EXPECT_EQ(staged().front().binding, 0U);
        EXPECT_EQ(staged().front().type, vk::DescriptorType::eUniformBuffer);
        EXPECT_EQ(staged().front().buffer_info.buffer, gpu->get_buffer());
        EXPECT_EQ(staged().front().buffer_info.range, sizeof(float));
    }

    TEST_F(DescriptorBindingsProcessorDeviceTest, TheRoleDecidesTheStagedDescriptorType)
    {
        auto node = std::make_shared<Nodes::Constant>(1.0);
        processor->bind_scalar_node("stored", node, "samples", 0, DescriptorRole::STORAGE);

        tick();

        ASSERT_EQ(staged().size(), 1U);
        EXPECT_EQ(staged().front().binding, 1U);
        EXPECT_EQ(staged().front().type, vk::DescriptorType::eStorageBuffer);
    }

    TEST_F(DescriptorBindingsProcessorDeviceTest, ASlotIsRewrittenEveryTickNotDuplicated)
    {
        auto node = std::make_shared<Nodes::Constant>(1.0);
        processor->bind_scalar_node("gain", node, "params", 0);

        tick();
        node->set_constant(-3.5);
        tick();
        tick();

        ASSERT_EQ(staged().size(), 1U);
        EXPECT_EQ(gpu_floats(processor->get_bound_buffer("params"), 1), std::vector<float> { -3.5F });
    }

    TEST_F(DescriptorBindingsProcessorDeviceTest, SeveralBindingsStageOneSlotEach)
    {
        auto gain = std::make_shared<Nodes::Constant>(0.5);
        auto level = std::make_shared<Nodes::Constant>(4.0);
        processor->bind_scalar_node("gain", gain, "params", 0, DescriptorRole::UNIFORM);
        processor->bind_scalar_node("level", level, "samples", 0, DescriptorRole::STORAGE);

        tick();

        ASSERT_EQ(staged().size(), 2U);
        const auto slot = [&](uint32_t binding) {
            return std::ranges::find_if(staged(), [=](const auto& entry) { return entry.binding == binding; });
        };
        ASSERT_NE(slot(0), staged().end());
        ASSERT_NE(slot(1), staged().end());
        EXPECT_EQ(slot(0)->type, vk::DescriptorType::eUniformBuffer);
        EXPECT_EQ(slot(1)->type, vk::DescriptorType::eStorageBuffer);
        EXPECT_EQ(gpu_floats(processor->get_bound_buffer("params"), 1), std::vector<float> { 0.5F });
        EXPECT_EQ(gpu_floats(processor->get_bound_buffer("samples"), 1), std::vector<float> { 4.0F });
    }

    TEST_F(DescriptorBindingsProcessorDeviceTest, ExternalModeReadsTheNodesLastOutputInsteadOfProcessingIt)
    {
        auto node = std::make_shared<Nodes::Generator::Sine>(440.0F, 1.0);
        for (int i = 0; i < 10; ++i)
            node->process_sample();
        const double last = node->get_last_output();
        processor->bind_scalar_node("gain", node, "params", 0,
            DescriptorRole::UNIFORM, BindingMode::EXTERNAL);

        tick();
        tick();
        EXPECT_EQ(gpu_floats(processor->get_bound_buffer("params"), 1),
            std::vector<float> { static_cast<float>(last) });
        EXPECT_DOUBLE_EQ(node->get_last_output(), last);

        processor->set_processing_mode("gain", BindingMode::INTERNAL);
        tick();
        EXPECT_NE(node->get_last_output(), last);
        EXPECT_EQ(gpu_floats(processor->get_bound_buffer("params"), 1),
            std::vector<float> { static_cast<float>(node->get_last_output()) });
    }

    // ------------------------------------------------------------------
    // Vector, matrix and structured nodes
    // ------------------------------------------------------------------

    TEST_F(DescriptorBindingsProcessorDeviceTest, ANodeWithoutGpuDataStillStagesItsSlotAndUploadsNothing)
    {
        auto node = std::make_shared<Nodes::Constant>(1.0);
        processor->bind_vector_node("spectrum", node, "samples", 0);

        tick();

        ASSERT_TRUE(processor->has_binding("spectrum"));
        ASSERT_EQ(staged().size(), 1U);
        EXPECT_EQ(staged().front().binding, 1U);
        EXPECT_EQ(staged().front().type, vk::DescriptorType::eStorageBuffer);
        EXPECT_EQ(staged().front().buffer_info.range, static_cast<size_t>(4096) * sizeof(float));
    }

    TEST_F(DescriptorBindingsProcessorDeviceTest, MatrixAndStructuredBindingsStartWithTheirOwnCapacities)
    {
        auto matrix = std::make_shared<Nodes::Constant>(1.0);
        auto structured = std::make_shared<Nodes::Constant>(1.0);
        processor->bind_matrix_node("grid", matrix, "params", 0, DescriptorRole::STORAGE);
        processor->bind_structured_node("records", structured, "samples", 0);

        tick();

        ASSERT_EQ(staged().size(), 2U);
        const auto slot = [&](uint32_t binding) {
            return std::ranges::find_if(staged(), [=](const auto& entry) { return entry.binding == binding; });
        };
        ASSERT_NE(slot(0), staged().end());
        ASSERT_NE(slot(1), staged().end());
        EXPECT_EQ(slot(0)->buffer_info.range, static_cast<size_t>(1024) * 1024 * sizeof(float));
        EXPECT_EQ(slot(1)->buffer_info.range, static_cast<size_t>(1024) * 64);
    }

    // ------------------------------------------------------------------
    // Buffer sources
    // ------------------------------------------------------------------

    TEST_F(DescriptorBindingsProcessorDeviceTest, AnAudioBufferIsUploadedAsFloats)
    {
        auto audio = std::make_shared<Buffers::AudioBuffer>(0);
        audio->resize(4);
        audio->get_data() = { 0.5, -1.0, 2.0, 0.25 };
        processor->bind_audio_buffer("audio", audio, "samples", 0);

        tick();

        auto gpu = processor->get_bound_buffer("samples");
        ASSERT_NE(gpu, nullptr);
        EXPECT_EQ(gpu_floats(gpu, 4), (std::vector<float> { 0.5F, -1.0F, 2.0F, 0.25F }));
        ASSERT_EQ(staged().size(), 1U);
        EXPECT_EQ(staged().front().type, vk::DescriptorType::eStorageBuffer);
        EXPECT_EQ(staged().front().buffer_info.range, 4 * sizeof(float));
    }

    TEST_F(DescriptorBindingsProcessorDeviceTest, AGrowingAudioBufferGrowsItsDescriptorBufferWithHeadroom)
    {
        auto audio = std::make_shared<Buffers::AudioBuffer>(0);
        audio->resize(4);
        audio->get_data() = { 1.0, 2.0, 3.0, 4.0 };
        processor->bind_audio_buffer("audio", audio, "samples", 0);
        tick();

        std::vector<double> larger(64);
        for (size_t i = 0; i < larger.size(); ++i)
            larger[i] = static_cast<double>(i);
        audio->get_data() = larger;
        tick();

        auto gpu = processor->get_bound_buffer("samples");
        ASSERT_NE(gpu, nullptr);
        EXPECT_GE(gpu->get_size_bytes(), larger.size() * sizeof(float));
        const auto uploaded = gpu_floats(gpu, 64);
        for (size_t i = 0; i < larger.size(); ++i)
            ASSERT_FLOAT_EQ(uploaded[i], static_cast<float>(i)) << "sample " << i;
        ASSERT_EQ(staged().size(), 1U);
        EXPECT_GE(staged().front().buffer_info.range, larger.size() * sizeof(float));
    }

    TEST_F(DescriptorBindingsProcessorDeviceTest, AHostVisibleBufferIsCopiedEveryTick)
    {
        auto host = make_buffer(4 * sizeof(float), VKBuffer::Usage::HOST_STORAGE);
        host->set_data({ Kakshya::DataVariant(std::vector<float> { 1.0F, 2.0F, 3.0F, 4.0F }) });
        processor->bind_host_vk_buffer("host", host, "samples", 0);

        tick();
        auto gpu = processor->get_bound_buffer("samples");
        ASSERT_NE(gpu, nullptr);
        EXPECT_EQ(gpu_floats(gpu, 4), (std::vector<float> { 1.0F, 2.0F, 3.0F, 4.0F }));

        host->set_data({ Kakshya::DataVariant(std::vector<float> { 5.0F, 6.0F, 7.0F, 8.0F }) });
        tick();
        EXPECT_EQ(gpu_floats(gpu, 4), (std::vector<float> { 5.0F, 6.0F, 7.0F, 8.0F }));
    }

    TEST_F(DescriptorBindingsProcessorDeviceTest, ADeviceLocalBufferIsRefused)
    {
        auto device_local = make_buffer(64, VKBuffer::Usage::COMPUTE);
        ASSERT_FALSE(device_local->is_host_visible());

        processor->bind_host_vk_buffer("device", device_local, "samples", 0);

        EXPECT_FALSE(processor->has_binding("device"));
        EXPECT_EQ(processor->get_bound_buffer("samples"), nullptr);
    }

    // ------------------------------------------------------------------
    // Rejected bindings
    // ------------------------------------------------------------------

    TEST_F(DescriptorBindingsProcessorDeviceTest, NullSourcesBindNothing)
    {
        processor->bind_scalar_node("a", nullptr, "params", 0);
        processor->bind_vector_node("b", nullptr, "samples", 0);
        processor->bind_matrix_node("c", nullptr, "samples", 0);
        processor->bind_structured_node("d", nullptr, "samples", 0);
        processor->bind_audio_buffer("e", nullptr, "samples", 0);
        processor->bind_host_vk_buffer("f", nullptr, "samples", 0);
        processor->bind_network("g", nullptr, "samples", 0);

        EXPECT_TRUE(processor->get_binding_names().empty());
        EXPECT_EQ(processor->get_bound_buffer("samples"), nullptr);
    }

    TEST_F(DescriptorBindingsProcessorDeviceTest, ADescriptorNameMissingFromTheShaderConfigBindsNothing)
    {
        auto node = std::make_shared<Nodes::Constant>(1.0);
        auto audio = std::make_shared<Buffers::AudioBuffer>(0, 4);
        auto host = make_buffer(16, VKBuffer::Usage::HOST_STORAGE);

        processor->bind_scalar_node("a", node, "missing", 0);
        processor->bind_vector_node("b", node, "missing", 0);
        processor->bind_matrix_node("c", node, "missing", 0);
        processor->bind_structured_node("d", node, "missing", 0);
        processor->bind_audio_buffer("e", audio, "missing", 0);
        processor->bind_host_vk_buffer("f", host, "missing", 0);

        EXPECT_TRUE(processor->get_binding_names().empty());
        EXPECT_EQ(processor->get_bound_buffer("missing"), nullptr);
        tick();
        EXPECT_TRUE(staged().empty());
    }

    // ------------------------------------------------------------------
    // Bookkeeping
    // ------------------------------------------------------------------

    TEST_F(DescriptorBindingsProcessorDeviceTest, BindingNamesAndMembershipFollowBindAndUnbind)
    {
        auto node = std::make_shared<Nodes::Constant>(1.0);
        processor->bind_scalar_node("gain", node, "params", 0);
        processor->bind_scalar_node("level", node, "samples", 0, DescriptorRole::STORAGE);

        auto names = processor->get_binding_names();
        std::ranges::sort(names);
        EXPECT_EQ(names, (std::vector<std::string> { "gain", "level" }));
        EXPECT_TRUE(processor->has_binding("gain"));
        EXPECT_FALSE(processor->has_binding("missing"));

        processor->unbind("gain");

        EXPECT_FALSE(processor->has_binding("gain"));
        EXPECT_EQ(processor->get_binding_names(), std::vector<std::string> { "level" });
        EXPECT_NO_THROW(processor->unbind("gain"));
        EXPECT_NO_THROW(processor->unbind("missing"));
    }

    TEST_F(DescriptorBindingsProcessorDeviceTest, UnbindingReleasesTheDescriptorBufferAndStopsSamplingTheNode)
    {
        auto node = std::make_shared<Nodes::Constant>(2.0);
        processor->bind_scalar_node("gain", node, "params", 0);
        tick();
        ASSERT_NE(processor->get_bound_buffer("params"), nullptr);

        processor->unbind("gain");

        EXPECT_EQ(processor->get_bound_buffer("params"), nullptr);
    }

    TEST_F(DescriptorBindingsProcessorDeviceTest, UnbindingRemovesTheSlotItStagedOnTheTarget)
    {
        auto node = std::make_shared<Nodes::Constant>(2.0);
        processor->bind_scalar_node("gain", node, "params", 0);
        tick();
        ASSERT_EQ(staged().size(), 1U);

        processor->unbind("gain");
        tick();

        EXPECT_TRUE(staged().empty());
    }

    TEST_F(DescriptorBindingsProcessorDeviceTest, UnbindingOneSlotLeavesTheOthersStaged)
    {
        auto gain = std::make_shared<Nodes::Constant>(1.0);
        auto level = std::make_shared<Nodes::Constant>(2.0);
        processor->bind_scalar_node("gain", gain, "params", 0);
        processor->bind_scalar_node("level", level, "samples", 0, DescriptorRole::STORAGE);
        tick();
        ASSERT_EQ(staged().size(), 2U);

        processor->unbind("gain");
        tick();

        ASSERT_EQ(staged().size(), 1U);
        EXPECT_EQ(staged().front().binding, 1U);
        EXPECT_EQ(gpu_floats(processor->get_bound_buffer("samples"), 1), std::vector<float> { 2.0F });
    }

    TEST_F(DescriptorBindingsProcessorDeviceTest, ARebindAfterUnbindStagesTheNewBufferInTheSameSlot)
    {
        auto first = std::make_shared<Nodes::Constant>(2.0);
        processor->bind_scalar_node("gain", first, "params", 0);
        tick();
        const auto old_handle = staged().front().buffer_info.buffer;

        processor->unbind("gain");
        auto second = std::make_shared<Nodes::Constant>(7.0);
        processor->bind_scalar_node("gain", second, "params", 0);
        tick();

        auto gpu = processor->get_bound_buffer("params");
        ASSERT_NE(gpu, nullptr);
        ASSERT_EQ(staged().size(), 1U);
        EXPECT_EQ(staged().front().buffer_info.buffer, gpu->get_buffer());
        EXPECT_NE(staged().front().buffer_info.buffer, old_handle);
        EXPECT_EQ(gpu_floats(gpu, 1), std::vector<float> { 7.0F });
    }

    TEST_F(DescriptorBindingsProcessorDeviceTest, ARebindAfterTheUnbindHasBeenAppliedStagesTheSlotAgain)
    {
        auto node = std::make_shared<Nodes::Constant>(3.0);
        processor->bind_scalar_node("gain", node, "params", 0);
        tick();
        processor->unbind("gain");
        tick();
        ASSERT_TRUE(staged().empty());

        processor->bind_scalar_node("gain", node, "params", 0);
        tick();

        ASSERT_EQ(staged().size(), 1U);
        EXPECT_EQ(gpu_floats(processor->get_bound_buffer("params"), 1), std::vector<float> { 3.0F });
    }

    TEST_F(DescriptorBindingsProcessorDeviceTest, ProcessingModesDefaultToInternalAndCanBeSetPerBindingOrForAll)
    {
        auto node = std::make_shared<Nodes::Constant>(1.0);
        processor->bind_scalar_node("gain", node, "params", 0);
        processor->bind_scalar_node("level", node, "samples", 0, DescriptorRole::STORAGE);

        EXPECT_EQ(processor->get_processing_mode("gain"), BindingMode::INTERNAL);
        EXPECT_EQ(processor->get_processing_mode("level"), BindingMode::INTERNAL);

        processor->set_processing_mode("gain", BindingMode::EXTERNAL);
        EXPECT_EQ(processor->get_processing_mode("gain"), BindingMode::EXTERNAL);
        EXPECT_EQ(processor->get_processing_mode("level"), BindingMode::INTERNAL);

        processor->set_processing_mode(BindingMode::EXTERNAL);
        EXPECT_EQ(processor->get_processing_mode("gain"), BindingMode::EXTERNAL);
        EXPECT_EQ(processor->get_processing_mode("level"), BindingMode::EXTERNAL);

        EXPECT_EQ(processor->get_processing_mode("missing"), BindingMode::INTERNAL);
        EXPECT_NO_THROW(processor->set_processing_mode("missing", BindingMode::EXTERNAL));
        EXPECT_FALSE(processor->has_binding("missing"));
    }

    TEST_F(DescriptorBindingsProcessorDeviceTest, ABindingModeIsKeptWhenTheBindingIsGiven)
    {
        auto node = std::make_shared<Nodes::Constant>(1.0);
        processor->bind_scalar_node("gain", node, "params", 0,
            DescriptorRole::UNIFORM, BindingMode::EXTERNAL);

        EXPECT_EQ(processor->get_processing_mode("gain"), BindingMode::EXTERNAL);
    }

    // ------------------------------------------------------------------
    // Nodes carrying GPU data
    // ------------------------------------------------------------------

    std::shared_ptr<Nodes::Generator::Polynomial> gpu_window_node(size_t window)
    {
        auto node = std::make_shared<Nodes::Generator::Polynomial>(
            [](std::span<double> view) { return view[0]; },
            Nodes::Generator::PolynomialMode::FEEDFORWARD, window);
        node->set_gpu_compatible(true);
        return node;
    }

    TEST_F(DescriptorBindingsProcessorDeviceTest, AVectorNodeUploadsItsGpuWindowAndTracksNewSamples)
    {
        auto node = gpu_window_node(3);
        for (const double sample : { 0.1, 0.2, 0.3, 0.4 })
            node->process_sample(sample);
        processor->bind_vector_node("window", node, "samples", 0,
            DescriptorRole::STORAGE, BindingMode::EXTERNAL);

        tick();
        auto gpu = processor->get_bound_buffer("samples");
        ASSERT_NE(gpu, nullptr);
        EXPECT_EQ(gpu_floats(gpu, 3), (std::vector<float> { 0.4F, 0.3F, 0.2F }));
        ASSERT_EQ(staged().size(), 1U);
        EXPECT_EQ(staged().front().type, vk::DescriptorType::eStorageBuffer);
        EXPECT_EQ(staged().front().buffer_info.range, static_cast<size_t>(4096) * sizeof(float));

        node->process_sample(0.5);
        tick();
        EXPECT_EQ(gpu_floats(gpu, 3), (std::vector<float> { 0.5F, 0.4F, 0.3F }));
    }

    TEST_F(DescriptorBindingsProcessorDeviceTest, AVectorLargerThanItsBufferGrowsTheBufferAndTheStagedRange)
    {
        constexpr size_t k_window = 5000;
        auto node = gpu_window_node(k_window);
        for (size_t i = 0; i < k_window; ++i)
            node->process_sample(static_cast<double>(i));
        processor->bind_vector_node("window", node, "samples", 0,
            DescriptorRole::STORAGE, BindingMode::EXTERNAL);

        tick();

        auto gpu = processor->get_bound_buffer("samples");
        ASSERT_NE(gpu, nullptr);
        EXPECT_GE(gpu->get_size_bytes(), k_window * sizeof(float));
        ASSERT_EQ(staged().size(), 1U);
        EXPECT_GE(staged().front().buffer_info.range, k_window * sizeof(float));
        const auto uploaded = gpu_floats(gpu, k_window);
        EXPECT_FLOAT_EQ(uploaded.front(), static_cast<float>(k_window - 1));
        EXPECT_FLOAT_EQ(uploaded.back(), 0.0F);
    }

    // ------------------------------------------------------------------
    // Networks
    // ------------------------------------------------------------------

    class DescriptorBindingsAudioNetwork final : public Nodes::Network::NodeNetwork {
    public:
        explicit DescriptorBindingsAudioNetwork(Nodes::Network::OutputMode mode = Nodes::Network::OutputMode::AUDIO_SINK)
        {
            set_output_mode(mode);
        }

        void process_batch(unsigned int /*num_samples*/) override { }
        [[nodiscard]] size_t get_node_count() const override { return 0; }
        void initialize() override { }
        void reset() override { }

        void fill(std::vector<double> samples) { m_last_audio_buffer = std::move(samples); }
    };

    TEST_F(DescriptorBindingsProcessorDeviceTest, AnAudioNetworkIsUploadedAsFloatsAndSizedFromItsBuffer)
    {
        auto network = std::make_shared<DescriptorBindingsAudioNetwork>();
        network->fill({ 0.5, -1.0, 2.0 });
        processor->bind_network("audio", network, "samples", 0);

        tick();

        auto gpu = processor->get_bound_buffer("samples");
        ASSERT_NE(gpu, nullptr);
        EXPECT_EQ(gpu_floats(gpu, 3), (std::vector<float> { 0.5F, -1.0F, 2.0F }));
        ASSERT_EQ(staged().size(), 1U);
        EXPECT_EQ(staged().front().type, vk::DescriptorType::eStorageBuffer);
        EXPECT_EQ(staged().front().buffer_info.range, 3 * sizeof(float));

        network->fill({ 4.0, 5.0, 6.0 });
        tick();
        EXPECT_EQ(gpu_floats(gpu, 3), (std::vector<float> { 4.0F, 5.0F, 6.0F }));
    }

    TEST_F(DescriptorBindingsProcessorDeviceTest, AnAudioNetworkWithoutABufferGetsTheDefaultCapacity)
    {
        auto network = std::make_shared<DescriptorBindingsAudioNetwork>(Nodes::Network::OutputMode::AUDIO_COMPUTE);
        processor->bind_network("audio", network, "samples", 0);

        tick();

        ASSERT_TRUE(processor->has_binding("audio"));
        ASSERT_EQ(staged().size(), 1U);
        EXPECT_EQ(staged().front().buffer_info.range, static_cast<size_t>(4096) * sizeof(float));
    }

    TEST_F(DescriptorBindingsProcessorDeviceTest, ANetworkWithNeitherAudioNorGeometryBindsNothing)
    {
        auto network = std::make_shared<DescriptorBindingsAudioNetwork>(Nodes::Network::OutputMode::NONE);

        processor->bind_network("silent", network, "samples", 0);

        EXPECT_FALSE(processor->has_binding("silent"));
        EXPECT_EQ(processor->get_bound_buffer("samples"), nullptr);
    }

    TEST_F(DescriptorBindingsProcessorDeviceTest, AGraphicsNetworkUploadsItsOperatorVertexBytes)
    {
        auto particles = std::make_shared<Nodes::Network::ParticleNetwork>(
            8, glm::vec3(-1.0F), glm::vec3(1.0F), Kinesis::SpatialDistribution::RANDOM_VOLUME);
        particles->create_operator<Nodes::Network::PhysicsOperator>();
        particles->process_batch(1);
        auto* graphics = dynamic_cast<Nodes::Network::GraphicsOperator*>(particles->get_operator());
        ASSERT_NE(graphics, nullptr);
        const auto expected = graphics->get_vertex_data();
        ASSERT_FALSE(expected.empty());

        processor->bind_network("cloud", particles, "samples", 0);
        tick();

        auto gpu = processor->get_bound_buffer("samples");
        ASSERT_NE(gpu, nullptr);
        std::vector<uint8_t> bytes(gpu->get_size_bytes());
        Buffers::download_from_gpu(gpu, bytes.data(), bytes.size());
        ASSERT_GE(bytes.size(), expected.size());
        EXPECT_TRUE(std::equal(expected.begin(), expected.end(), bytes.begin()));
        ASSERT_EQ(staged().size(), 1U);
        EXPECT_EQ(staged().front().type, vk::DescriptorType::eStorageBuffer);
        EXPECT_EQ(staged().front().buffer_info.range, static_cast<size_t>(1024) * 64);
    }

}
}
