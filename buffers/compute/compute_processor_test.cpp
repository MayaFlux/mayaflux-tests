#include "gtest/gtest.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "../../support/vulkan_test_support.hpp"

#include "MayaFlux/Buffers/Shaders/ComputeProcessor.hpp"
#include "MayaFlux/Registry/BackendRegistry.hpp"
#include "MayaFlux/Registry/Service/BufferService.hpp"

namespace MayaFlux::Test {
namespace {

    using Buffers::ComputeProcessor;
    using Buffers::ShaderBinding;
    using Buffers::ShaderProcessor;
    using Buffers::VKBuffer;
    using Portal::Graphics::ComputePipelineID;

    constexpr const char* k_scale_shader = R"(#version 460
layout(local_size_x = 4) in;
layout(set = 0, binding = 0, std430) buffer Data { float values[]; } data;
layout(push_constant) uniform Constants { float factor; float bias; } pc;
void main() {
    uint index = gl_GlobalInvocationID.x;
    data.values[index] = data.values[index] * pc.factor + pc.bias;
}
)";

    constexpr const char* k_scale_shader_with_unused_tail = R"(#version 460
layout(local_size_x = 4) in;
layout(set = 0, binding = 0, std430) buffer Data { float values[]; } data;
layout(push_constant) uniform Constants { float factor; float bias; float tail; } pc;
void main() {
    uint index = gl_GlobalInvocationID.x;
    data.values[index] = data.values[index] * pc.factor + pc.bias;
}
)";

    class TemporaryShader {
    public:
        TemporaryShader(const void* owner, const char* source)
            : m_path(std::filesystem::temp_directory_path()
                  / ("mayaflux_compute_processor_" + std::to_string(reinterpret_cast<std::uintptr_t>(owner)) + ".comp"))
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

    class ComputeProcessorProbe final : public ComputeProcessor {
    public:
        using ComputeProcessor::ComputeProcessor;

        std::vector<ComputePipelineID> pipeline_ids;

    protected:
        void on_pipeline_created(ComputePipelineID id) override
        {
            pipeline_ids.push_back(id);
            ComputeProcessor::on_pipeline_created(id);
        }
    };

    template <class T>
    T read_bytes(const std::vector<uint8_t>& bytes, size_t offset)
    {
        T value {};
        std::memcpy(&value, bytes.data() + offset, sizeof(T));
        return value;
    }

    class ComputeProcessorDeviceTest : public VulkanDeviceTest {
    protected:
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
            for (const auto& buffer : buffers) {
                if (buffer && buffer->is_initialized()) {
                    auto* service = Registry::BackendRegistry::instance()
                                        .get_service<Registry::Service::BufferService>();
                    if (service)
                        service->destroy_buffer(buffer);
                }
            }
            buffers.clear();
            Portal::Graphics::get_compute_press().shutdown();
            Portal::Graphics::get_shader_foundry().shutdown();
            VulkanDeviceTest::TearDown();
        }

        std::shared_ptr<VKBuffer> tracked_buffer(const std::vector<float>& values)
        {
            auto buffer = std::make_shared<VKBuffer>(values.size() * sizeof(float),
                VKBuffer::Usage::HOST_STORAGE, Kakshya::DataModality::UNKNOWN);
            buffers.push_back(buffer);
            auto* service = Registry::BackendRegistry::instance()
                                .get_service<Registry::Service::BufferService>();
            service->initialize_buffer(buffer);
            buffer->set_data({ Kakshya::DataVariant(values) });
            return buffer;
        }

        static std::unique_ptr<ComputeProcessorProbe> scale_processor(const TemporaryShader& shader)
        {
            auto processor = std::make_unique<ComputeProcessorProbe>(shader.path(), 4);
            processor->add_binding("data", ShaderBinding { 0, 0 });
            processor->set_push_constant_size(8);
            return processor;
        }

        static std::vector<float> contents(const ShaderProcessor& processor, const std::string& name, size_t count)
        {
            std::vector<uint8_t> bytes(count * sizeof(float));
            EXPECT_TRUE(processor.download_bound(name, bytes.data(), bytes.size()));
            std::vector<float> values(count);
            for (size_t i = 0; i < count; ++i)
                values[i] = read_bytes<float>(bytes, i * sizeof(float));
            return values;
        }

        static bool pipeline_alive(ComputePipelineID id)
        {
            return !Portal::Graphics::get_compute_press().allocate_pipeline_descriptors(id).empty();
        }

        std::vector<std::shared_ptr<VKBuffer>> buffers;
    };

    TEST_F(ComputeProcessorDeviceTest, ProcessorConstantsReachTheDispatch)
    {
        TemporaryShader shader(this, k_scale_shader);
        auto processor = scale_processor(shader);
        auto buffer = tracked_buffer({ 1.0F, 2.0F, 3.0F, 4.0F });
        processor->bind_buffer("data", buffer);
        processor->set_push_constant_data(std::array<float, 2> { 3.0F, 2.0F });

        processor->processing_function(buffer);

        EXPECT_EQ(contents(*processor, "data", 4), (std::vector<float> { 5.0F, 8.0F, 11.0F, 14.0F }));
        EXPECT_EQ(processor->pipeline_ids.size(), 1U);
    }

    TEST_F(ComputeProcessorDeviceTest, BufferPushConstantFragmentMergesOverTheProcessorConstants)
    {
        TemporaryShader shader(this, k_scale_shader);
        auto processor = scale_processor(shader);
        auto buffer = tracked_buffer({ 1.0F, 2.0F, 3.0F, 4.0F });
        processor->bind_buffer("data", buffer);
        processor->set_push_constant_data(std::array<float, 2> { 2.0F, 1.0F });
        const float bias = 5.0F;
        std::vector<uint8_t> fragment(sizeof(float));
        std::memcpy(fragment.data(), &bias, sizeof(float));
        buffer->get_pipeline_context().push_constant_bindings.push_back(
            { .offset = 4, .data = fragment, .name = "bias" });

        processor->processing_function(buffer);

        EXPECT_EQ(contents(*processor, "data", 4), (std::vector<float> { 7.0F, 9.0F, 11.0F, 13.0F }));
    }

    TEST_F(ComputeProcessorDeviceTest, LateFeedIsPumpedIntoTheDispatchThatRecordsIt)
    {
        TemporaryShader shader(this, k_scale_shader);
        auto processor = scale_processor(shader);
        auto buffer = tracked_buffer({ 1.0F, 2.0F, 3.0F, 4.0F });
        processor->bind_buffer("data", buffer);
        processor->feed("factor", [] { return ShaderProcessor::FeedValue { 3.0 }; }, 0, sizeof(float));
        processor->feed("bias", [] { return ShaderProcessor::FeedValue { 2.0 }; }, 4, sizeof(float), false);

        processor->processing_function(buffer);

        EXPECT_EQ(contents(*processor, "data", 4), (std::vector<float> { 5.0F, 8.0F, 11.0F, 14.0F }));
    }

    TEST_F(ComputeProcessorDeviceTest, LateFeedBeyondTheCreatedRangeGetsAPipelineThatCoversItBeforeItIsPushed)
    {
        TemporaryShader shader(this, k_scale_shader_with_unused_tail);
        auto processor = scale_processor(shader);
        auto buffer = tracked_buffer({ 1.0F, 2.0F, 3.0F, 4.0F });
        processor->bind_buffer("data", buffer);
        processor->set_push_constant_data(std::array<float, 2> { 2.0F, 0.0F });
        processor->processing_function(buffer);
        ASSERT_EQ(processor->pipeline_ids.size(), 1U);

        processor->feed("tail", [] { return ShaderProcessor::FeedValue { 9.0 }; }, 8, sizeof(float), false);
        processor->processing_function(buffer);

        EXPECT_EQ(processor->pipeline_ids.size(), 2U);
        EXPECT_EQ(contents(*processor, "data", 4), (std::vector<float> { 4.0F, 8.0F, 12.0F, 16.0F }));
    }

    TEST_F(ComputeProcessorDeviceTest, RebindingAnExistingNameKeepsOnePipelineAndRedirectsTheDispatch)
    {
        TemporaryShader shader(this, k_scale_shader);
        auto processor = scale_processor(shader);
        auto first = tracked_buffer({ 1.0F, 1.0F, 1.0F, 1.0F });
        auto second = tracked_buffer({ 2.0F, 2.0F, 2.0F, 2.0F });
        processor->bind_buffer("data", first);
        processor->set_push_constant_data(std::array<float, 2> { 2.0F, 1.0F });
        processor->processing_function(first);

        processor->bind_buffer("data", second);
        processor->processing_function(first);

        EXPECT_EQ(processor->pipeline_ids.size(), 1U);
        EXPECT_EQ(contents(*processor, "data", 4), (std::vector<float> { 5.0F, 5.0F, 5.0F, 5.0F }));
    }

    TEST_F(ComputeProcessorDeviceTest, ReplacingThePipelineRetiresTheOldOne)
    {
        TemporaryShader shader(this, k_scale_shader);
        auto processor = scale_processor(shader);
        auto buffer = tracked_buffer({ 1.0F, 2.0F, 3.0F, 4.0F });
        processor->bind_buffer("data", buffer);
        processor->set_push_constant_data(std::array<float, 2> { 1.0F, 0.0F });
        processor->processing_function(buffer);
        ASSERT_EQ(processor->pipeline_ids.size(), 1U);
        const auto original = processor->pipeline_ids.front();

        processor->set_push_constant_size(16);
        processor->processing_function(buffer);

        ASSERT_EQ(processor->pipeline_ids.size(), 2U);
        EXPECT_NE(processor->pipeline_ids.back(), original);
        EXPECT_FALSE(pipeline_alive(original));
        EXPECT_TRUE(pipeline_alive(processor->pipeline_ids.back()));
        EXPECT_EQ(contents(*processor, "data", 4), (std::vector<float> { 1.0F, 2.0F, 3.0F, 4.0F }));
    }

    TEST_F(ComputeProcessorDeviceTest, DestroyingTheProcessorReleasesItsPipeline)
    {
        TemporaryShader shader(this, k_scale_shader);
        auto processor = scale_processor(shader);
        auto buffer = tracked_buffer({ 1.0F, 2.0F, 3.0F, 4.0F });
        processor->bind_buffer("data", buffer);
        processor->set_push_constant_data(std::array<float, 2> { 1.0F, 0.0F });
        processor->processing_function(buffer);
        ASSERT_EQ(processor->pipeline_ids.size(), 1U);
        const auto id = processor->pipeline_ids.front();
        ASSERT_TRUE(pipeline_alive(id));

        processor.reset();

        EXPECT_FALSE(pipeline_alive(id));
    }

}
}
