#include "gtest/gtest.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "../../support/vulkan_test_support.hpp"

#include "MayaFlux/Buffers/Shaders/ShaderProcessor.hpp"
#include "MayaFlux/Registry/BackendRegistry.hpp"
#include "MayaFlux/Registry/Service/BufferService.hpp"

namespace MayaFlux::Test {
namespace {

    using Buffers::ShaderBinding;
    using Buffers::ShaderConfig;
    using Buffers::ShaderProcessor;
    using Buffers::VKBuffer;
    using Portal::Graphics::ComputePipelineID;
    using Portal::Graphics::DescriptorBindingInfo;
    using Portal::Graphics::INVALID_COMPUTE_PIPELINE;
    using Portal::Graphics::INVALID_SHADER;

    class ShaderProcessorProbe final : public ShaderProcessor {
    public:
        explicit ShaderProcessorProbe(ShaderConfig config, bool dispatch = false)
            : ShaderProcessor(std::move(config))
            , m_dispatch(dispatch)
        {
        }

        ~ShaderProcessorProbe() override
        {
            resolve_pending_dispatch(true);
            if (m_pipeline != INVALID_COMPUTE_PIPELINE)
                Portal::Graphics::get_compute_press().destroy_pipeline(m_pipeline);
        }

        size_t push_constant_extent(const std::shared_ptr<VKBuffer>& buffer) const
        {
            return resolve_push_constant_size(buffer);
        }

        std::vector<uint8_t> merged_push_constants(const std::shared_ptr<VKBuffer>& buffer) const
        {
            return resolve_push_constants(buffer);
        }

        void pull_feeds(bool early = true) { pump_feeds(early); }
        bool needs_pipeline_rebuild() const { return m_needs_pipeline_rebuild; }
        bool needs_descriptor_rebuild() const { return m_needs_descriptor_rebuild; }
        void clear_rebuild_flags()
        {
            m_needs_pipeline_rebuild = false;
            m_needs_descriptor_rebuild = false;
        }
        void reserve_engine_set_zero(bool enabled) { m_engine_owns_set_zero = enabled; }
        std::optional<uint32_t> descriptor_index(uint32_t set) const { return resolve_ds_index(set); }
        void set_descriptor_count(size_t count) { m_descriptor_set_ids.resize(count); }

        auto& config_bindings() { return m_config.bindings; }

        std::set<std::pair<uint32_t, uint32_t>> created_slots;
        size_t created_push_constant_size {};
        size_t pipeline_creations {};
        size_t descriptor_creations {};
        size_t executions {};
        size_t before_executions {};
        size_t after_executions {};
        size_t shader_loads {};
        size_t dispatch_completions {};
        Portal::Graphics::ShaderID loaded_shader() const { return m_shader_id; }

    protected:
        bool tracks_pipeline_layout() const override { return true; }

        void initialize_pipeline(const std::shared_ptr<VKBuffer>& buffer) override
        {
            ++pipeline_creations;
            created_slots.clear();
            for (const auto& [name, binding] : m_config.bindings)
                created_slots.emplace(binding.set, binding.binding);
            created_push_constant_size = resolve_push_constant_size(buffer);
            if (!m_dispatch)
                return;

            if (m_pipeline != INVALID_COMPUTE_PIPELINE)
                Portal::Graphics::get_compute_press().destroy_pipeline(m_pipeline);

            std::map<uint32_t, std::vector<DescriptorBindingInfo>> grouped;
            for (const auto& [name, binding] : m_config.bindings) {
                grouped[binding.set].push_back(DescriptorBindingInfo {
                    .set = binding.set,
                    .binding = binding.binding,
                    .type = binding.type,
                    .name = name,
                    .count = binding.count });
            }
            std::vector<std::vector<DescriptorBindingInfo>> sets;
            for (auto& entry : grouped) {
                auto& bindings = entry.second;
                std::ranges::sort(bindings, {}, &DescriptorBindingInfo::binding);
                sets.push_back(std::move(bindings));
            }
            m_pipeline = Portal::Graphics::get_compute_press().create_pipeline(
                m_shader_id, sets, resolve_push_constant_size(buffer));
        }

        void initialize_descriptors(const std::shared_ptr<VKBuffer>& buffer) override
        {
            ++descriptor_creations;
            if (!m_dispatch)
                return;
            m_descriptor_set_ids = Portal::Graphics::get_compute_press().allocate_pipeline_descriptors(m_pipeline);
            update_descriptors(buffer);
        }

        void execute_shader(const std::shared_ptr<VKBuffer>& buffer) override
        {
            ++executions;
            if (!m_dispatch)
                return;

            auto& foundry = Portal::Graphics::get_shader_foundry();
            auto& press = Portal::Graphics::get_compute_press();
            const auto command = foundry.begin_commands(Portal::Graphics::ShaderFoundry::CommandBufferType::COMPUTE);
            m_last_command_buffer = command;
            m_last_processed_buffer = buffer;
            press.bind_pipeline(command, m_pipeline);
            press.bind_descriptor_sets(command, m_pipeline, m_descriptor_set_ids);
            const auto constants = resolve_push_constants(buffer);
            if (!constants.empty())
                press.push_constants(command, m_pipeline, constants.data(), constants.size());
            press.dispatch(command, 1, 1, 1);
            submit_recorded(command, buffer);
        }

        void on_shader_loaded(Portal::Graphics::ShaderID) override { ++shader_loads; }
        bool on_before_execute(Portal::Graphics::CommandBufferID, const std::shared_ptr<VKBuffer>&) override
        {
            ++before_executions;
            return true;
        }
        void on_after_execute(Portal::Graphics::CommandBufferID, const std::shared_ptr<VKBuffer>&) override
        {
            ++after_executions;
        }
        void on_dispatch_complete(const std::shared_ptr<VKBuffer>&) override { ++dispatch_completions; }

    private:
        bool m_dispatch;
        ComputePipelineID m_pipeline { INVALID_COMPUTE_PIPELINE };
    };

    ShaderConfig config_with_binding()
    {
        ShaderConfig config;
        config.bindings.emplace("data", ShaderBinding { 0, 0 });
        return config;
    }

    std::shared_ptr<VKBuffer> host_buffer(size_t bytes = 64)
    {
        return std::make_shared<VKBuffer>(bytes, VKBuffer::Usage::HOST_STORAGE,
            Kakshya::DataModality::UNKNOWN);
    }

    template <class T>
    T read_bytes(const std::vector<uint8_t>& bytes, size_t offset)
    {
        T value {};
        std::memcpy(&value, bytes.data() + offset, sizeof(T));
        return value;
    }

    TEST(ShaderProcessorTest, ConfigurationAndBindingIntrospection)
    {
        auto config = config_with_binding();
        config.shader_path = "shader.comp";
        config.bindings.emplace("other", ShaderBinding { 1, 2 });
        ShaderProcessorProbe processor(config);

        EXPECT_EQ(processor.get_shader_path(), "shader.comp");
        EXPECT_TRUE(processor.has_binding("data"));
        EXPECT_FALSE(processor.has_binding("missing"));
        auto names = processor.get_binding_names();
        std::ranges::sort(names);
        EXPECT_EQ(names, (std::vector<std::string> { "data", "other" }));
        EXPECT_FALSE(processor.are_bindings_complete());
        EXPECT_EQ(processor.get_bound_buffer_count(), 0U);
        EXPECT_TRUE(processor.is_compatible_with(host_buffer()));
        EXPECT_FALSE(processor.is_compatible_with(nullptr));
    }

    TEST(ShaderProcessorTest, BindingConfigurationAndUsageHints)
    {
        ShaderProcessorProbe processor(config_with_binding());
        processor.clear_rebuild_flags();
        processor.add_binding("input", ShaderBinding { 1, 0 });
        processor.add_binding("output", ShaderBinding { 1, 1 });

        EXPECT_TRUE(processor.needs_descriptor_rebuild());
        EXPECT_EQ(processor.get_buffer_usage_hint("input"), ShaderProcessor::BufferUsageHint::INPUT_READ);
        EXPECT_EQ(processor.get_buffer_usage_hint("output"), ShaderProcessor::BufferUsageHint::OUTPUT_WRITE);
        EXPECT_EQ(processor.get_buffer_usage_hint("data"), ShaderProcessor::BufferUsageHint::NONE);
        EXPECT_FALSE(processor.is_in_place_operation("input"));
    }

    TEST(ShaderProcessorTest, PushConstantDataAndBufferFragmentsMergeAtOffsets)
    {
        ShaderProcessorProbe processor(config_with_binding());
        auto buffer = host_buffer();
        const std::array<uint32_t, 2> base { 7, 11 };
        processor.set_push_constant_size(sizeof(base));
        processor.set_push_constant_data_raw(base.data(), sizeof(base));
        buffer->get_pipeline_context().push_constant_bindings.push_back(
            { .offset = 4, .data = { 23, 0, 0, 0 }, .name = "override" });
        buffer->get_pipeline_context().push_constant_bindings.push_back(
            { .offset = 12, .data = { 31, 0, 0, 0 }, .name = "extension" });

        EXPECT_EQ(processor.push_constant_extent(buffer), 16U);
        const auto merged = processor.merged_push_constants(buffer);
        ASSERT_EQ(merged.size(), 16U);
        EXPECT_EQ(read_bytes<uint32_t>(merged, 0), 7U);
        EXPECT_EQ(read_bytes<uint32_t>(merged, 4), 23U);
        EXPECT_EQ(read_bytes<uint32_t>(merged, 8), 0U);
        EXPECT_EQ(read_bytes<uint32_t>(merged, 12), 31U);
        EXPECT_EQ(read_bytes<uint32_t>(processor.get_push_constant_data(), 4), 11U);
    }

    TEST(ShaderProcessorTest, TypedPushConstantsSizeTheBlockAndOversizeRawWriteIsRejected)
    {
        ShaderProcessorProbe processor(config_with_binding());
        const std::array<uint32_t, 2> value { 3, 5 };
        processor.set_push_constant_data(value);

        EXPECT_EQ(processor.get_config().push_constant_size, sizeof(value));
        EXPECT_EQ(read_bytes<uint32_t>(processor.get_push_constant_data(), 4), 5U);

        const std::array<uint32_t, 3> oversized { 1, 2, 3 };
        processor.set_push_constant_data_raw(oversized.data(), sizeof(oversized));
        EXPECT_EQ(processor.get_push_constant_data().size(), sizeof(value));
        EXPECT_EQ(read_bytes<uint32_t>(processor.get_push_constant_data(), 0), 3U);
    }

    TEST(ShaderProcessorTest, NamedAndExplicitFeedsWriteTheirOwnPushConstantRanges)
    {
        auto config = config_with_binding();
        config.pc_fields = {
            { "gain", Kakshya::GpuDataFormat::FLOAT32 },
            { "offset", Kakshya::GpuDataFormat::FLOAT64 }
        };
        ShaderProcessorProbe processor(config);
        processor.feed("offset", [] { return ShaderProcessor::FeedValue { 4.5 }; });
        processor.feed("gain", [] { return ShaderProcessor::FeedValue { 2.25 }; });
        processor.feed("late", [] { return ShaderProcessor::FeedValue { 8.0 }; }, 16, sizeof(float), false);

        EXPECT_EQ(processor.get_feed_names().size(), 3U);
        processor.pull_feeds();
        EXPECT_FLOAT_EQ(read_bytes<float>(processor.get_push_constant_data(), 0), 2.25F);
        EXPECT_DOUBLE_EQ(read_bytes<double>(processor.get_push_constant_data(), 8), 4.5);
        EXPECT_EQ(processor.get_push_constant_data().size(), 16U);

        processor.pull_feeds(false);
        EXPECT_FLOAT_EQ(read_bytes<float>(processor.get_push_constant_data(), 16), 8.0F);
        processor.remove_feed("offset");
        EXPECT_FALSE(processor.has_feed("offset"));
        EXPECT_TRUE(processor.has_feed("gain"));
    }

    TEST(ShaderProcessorTest, InvalidFeedsDoNotRegisterAndMismatchedValuesKeepPriorData)
    {
        ShaderProcessorProbe processor(config_with_binding());
        processor.feed("missing", [] { return ShaderProcessor::FeedValue { 1.0 }; });
        processor.feed("null", {});
        processor.feed("bad_width", [] { return ShaderProcessor::FeedValue { 1.0 }; }, 0, 2);
        EXPECT_TRUE(processor.get_feed_names().empty());

        processor.feed("gain", [] { return ShaderProcessor::FeedValue { 3.0 }; }, 0);
        processor.pull_feeds();
        EXPECT_FLOAT_EQ(read_bytes<float>(processor.get_push_constant_data(), 0), 3.0F);

        processor.feed("gain", [] {
            return ShaderProcessor::FeedValue { Kakshya::DataVariant(std::vector<float> { 9.0F }) };
        }, 0);
        processor.pull_feeds();
        EXPECT_FLOAT_EQ(read_bytes<float>(processor.get_push_constant_data(), 0), 3.0F);
    }

    TEST(ShaderProcessorTest, TypedFeedWritesItsOwnFormatAtTheOffset)
    {
        ShaderProcessorProbe processor(config_with_binding());
        processor.feed("direction", [] {
            return ShaderProcessor::FeedValue { Kakshya::GpuValue { glm::vec3 { 1.0F, 2.0F, 3.0F } } };
        }, 4);

        processor.pull_feeds();

        EXPECT_EQ(processor.get_push_constant_data().size(), 16U);
        EXPECT_FLOAT_EQ(read_bytes<float>(processor.get_push_constant_data(), 4), 1.0F);
        EXPECT_FLOAT_EQ(read_bytes<float>(processor.get_push_constant_data(), 8), 2.0F);
        EXPECT_FLOAT_EQ(read_bytes<float>(processor.get_push_constant_data(), 12), 3.0F);
    }

    TEST(ShaderProcessorTest, NamedFeedsAfterAVec3SitAtTheirAlignedOffsets)
    {
        auto config = config_with_binding();
        config.pc_fields = {
            { "gain", Kakshya::GpuDataFormat::FLOAT32 },
            { "dir", Kakshya::GpuDataFormat::VEC3_F32 },
            { "tail", Kakshya::GpuDataFormat::FLOAT32 }
        };
        ShaderProcessorProbe processor(config);
        processor.feed("gain", [] { return ShaderProcessor::FeedValue { Kakshya::GpuValue { 1.0F } }; });
        processor.feed("dir", [] { return ShaderProcessor::FeedValue { Kakshya::GpuValue { glm::vec3 { 2.0F, 3.0F, 4.0F } } }; });
        processor.feed("tail", [] { return ShaderProcessor::FeedValue { Kakshya::GpuValue { 5.0F } }; });

        processor.pull_feeds();

        EXPECT_FLOAT_EQ(read_bytes<float>(processor.get_push_constant_data(), 0), 1.0F);
        EXPECT_FLOAT_EQ(read_bytes<float>(processor.get_push_constant_data(), 16), 2.0F);
        EXPECT_FLOAT_EQ(read_bytes<float>(processor.get_push_constant_data(), 24), 4.0F);
        EXPECT_FLOAT_EQ(read_bytes<float>(processor.get_push_constant_data(), 28), 5.0F);
    }

    TEST(ShaderProcessorTest, NamedFeedRefusesAValueOfADifferentFormatThanItsField)
    {
        auto config = config_with_binding();
        config.pc_fields = {
            { "gain", Kakshya::GpuDataFormat::FLOAT32 },
            { "next", Kakshya::GpuDataFormat::FLOAT32 }
        };
        ShaderProcessorProbe processor(config);
        processor.feed("next", [] { return ShaderProcessor::FeedValue { Kakshya::GpuValue { 7.0F } }; });
        processor.pull_feeds();

        processor.feed("gain", [] { return ShaderProcessor::FeedValue { Kakshya::GpuValue { glm::vec3 { 1.0F, 2.0F, 3.0F } } }; });
        processor.pull_feeds();

        EXPECT_EQ(processor.get_push_constant_data().size(), 8U);
        EXPECT_FLOAT_EQ(read_bytes<float>(processor.get_push_constant_data(), 4), 7.0F);
    }

    TEST(ShaderProcessorTest, NamedFeedAcceptsAValueOfItsOwnFormat)
    {
        auto config = config_with_binding();
        config.pc_fields = { { "count", Kakshya::GpuDataFormat::UINT32 } };
        ShaderProcessorProbe processor(config);
        processor.feed("count", [] { return ShaderProcessor::FeedValue { Kakshya::GpuValue { uint32_t { 12 } } }; });

        processor.pull_feeds();

        EXPECT_EQ(read_bytes<uint32_t>(processor.get_push_constant_data(), 0), 12U);
    }

    TEST(ShaderProcessorTest, ExplicitOffsetFeedsAcceptAnyFormat)
    {
        ShaderProcessorProbe processor(config_with_binding());
        processor.feed("loose", [] { return ShaderProcessor::FeedValue { Kakshya::GpuValue { glm::vec2 { 1.0F, 2.0F } } }; }, 8);

        processor.pull_feeds();

        EXPECT_FLOAT_EQ(read_bytes<float>(processor.get_push_constant_data(), 12), 2.0F);
    }

    TEST(ShaderProcessorTest, TypedFeedKeepsIntegerBitsUnconverted)
    {
        ShaderProcessorProbe processor(config_with_binding());
        processor.feed("count", [] { return ShaderProcessor::FeedValue { Kakshya::GpuValue { uint32_t { 0xDEADBEEF } } }; }, 0);

        processor.pull_feeds();

        EXPECT_EQ(read_bytes<uint32_t>(processor.get_push_constant_data(), 0), 0xDEADBEEFU);
    }

    TEST(ShaderProcessorTest, TypedFeedWritesAMatrix)
    {
        ShaderProcessorProbe processor(config_with_binding());
        glm::mat4 value(1.0F);
        value[3][0] = 5.0F;
        processor.feed("transform", [value] { return ShaderProcessor::FeedValue { Kakshya::GpuValue { value } }; }, 0);

        processor.pull_feeds();

        EXPECT_EQ(processor.get_push_constant_data().size(), 64U);
        EXPECT_FLOAT_EQ(read_bytes<float>(processor.get_push_constant_data(), 12 * sizeof(float)), 5.0F);
    }

    TEST(ShaderProcessorTest, TypedFeedFollowsTheValueEachCycle)
    {
        ShaderProcessorProbe processor(config_with_binding());
        int32_t counter = 0;
        processor.feed("tick", [&counter] { return ShaderProcessor::FeedValue { Kakshya::GpuValue { counter++ } }; }, 0);

        processor.pull_feeds();
        processor.pull_feeds();

        EXPECT_EQ(read_bytes<int32_t>(processor.get_push_constant_data(), 0), 1);
    }

    TEST(ShaderProcessorTest, TypedFeedIsRefusedByAStorageDescriptor)
    {
        ShaderProcessorProbe processor(config_with_binding());
        processor.feed("data", [] { return ShaderProcessor::FeedValue { Kakshya::GpuValue { 1.0F } }; });

        processor.pull_feeds();

        EXPECT_EQ(processor.get_bound_buffer("data"), nullptr);
    }

    TEST(ShaderProcessorTest, StorageFeedRejectsAScalarValue)
    {
        ShaderProcessorProbe processor(config_with_binding());
        processor.feed("data", [] { return ShaderProcessor::FeedValue { 3.0 }; });

        processor.pull_feeds();

        EXPECT_TRUE(processor.has_feed("data"));
        EXPECT_EQ(processor.get_bound_buffer("data"), nullptr);
    }

    TEST(ShaderProcessorTest, ConfigurationChangesMarkTheRelevantRebuilds)
    {
        ShaderProcessorProbe processor(config_with_binding());
        processor.clear_rebuild_flags();
        processor.set_push_constant_size(16);
        EXPECT_TRUE(processor.needs_pipeline_rebuild());

        processor.clear_rebuild_flags();
        processor.set_specialization_constant(4, 12);
        EXPECT_EQ(processor.get_config().specialization_constants.at(4), 12U);
        EXPECT_TRUE(processor.needs_pipeline_rebuild());

        processor.clear_rebuild_flags();
        processor.clear_specialization_constants();
        EXPECT_TRUE(processor.get_config().specialization_constants.empty());
        EXPECT_TRUE(processor.needs_pipeline_rebuild());
    }

    TEST(ShaderProcessorTest, SettingTheSamePushConstantSizeDoesNotMarkARebuild)
    {
        ShaderProcessorProbe processor(config_with_binding());
        processor.set_push_constant_size(16);
        processor.clear_rebuild_flags();

        processor.set_push_constant_size(16);

        EXPECT_FALSE(processor.needs_pipeline_rebuild());
    }

    TEST(ShaderProcessorTest, DescriptorSetIndexRespectsEngineReservation)
    {
        ShaderProcessorProbe processor(config_with_binding());
        processor.set_descriptor_count(2);
        EXPECT_EQ(processor.descriptor_index(0), 0U);
        EXPECT_EQ(processor.descriptor_index(1), 1U);
        EXPECT_FALSE(processor.descriptor_index(2).has_value());

        processor.reserve_engine_set_zero(true);
        EXPECT_FALSE(processor.descriptor_index(0).has_value());
        EXPECT_EQ(processor.descriptor_index(1), 0U);
        EXPECT_EQ(processor.descriptor_index(2), 1U);
        EXPECT_FALSE(processor.descriptor_index(3).has_value());
    }

    TEST(ShaderProcessorTest, NullBufferIsRejectedBeforeShaderLoading)
    {
        ShaderProcessorProbe processor(config_with_binding());
        processor.processing_function(nullptr);

        EXPECT_FALSE(processor.is_shader_loaded());
        EXPECT_EQ(processor.pipeline_creations, 0U);
        EXPECT_EQ(processor.executions, 0U);
    }

    constexpr const char* k_compute_shader = R"(#version 460
layout(local_size_x = 4) in;
layout(set = 0, binding = 0, std430) buffer Data { float values[]; } data;
layout(push_constant) uniform Constants { float factor; float bias; } pc;
void main() {
    uint index = gl_GlobalInvocationID.x;
    data.values[index] = data.values[index] * pc.factor + pc.bias;
}
)";

    constexpr const char* k_two_storage_shader = R"(#version 460
layout(local_size_x = 4) in;
layout(set = 0, binding = 0, std430) readonly buffer Source { float values[]; } source_data;
layout(set = 0, binding = 1, std430) writeonly buffer Target { float values[]; } target_data;
layout(push_constant) uniform Constants { float factor; float bias; } pc;
void main() {
    uint index = gl_GlobalInvocationID.x;
    target_data.values[index] = source_data.values[index] * pc.factor + pc.bias;
}
)";

    constexpr const char* k_uniform_and_storage_shader = R"(#version 460
layout(local_size_x = 4) in;
layout(set = 0, binding = 0, std140) uniform Source { float value; } source_data;
layout(set = 0, binding = 1, std430) writeonly buffer Target { float values[]; } target_data;
layout(push_constant) uniform Constants { float factor; float bias; } pc;
void main() {
    target_data.values[gl_GlobalInvocationID.x] = source_data.value * pc.factor + pc.bias;
}
)";

    struct TemporaryComputeShader {
        std::filesystem::path path;

        explicit TemporaryComputeShader(const void* owner)
            : path(std::filesystem::temp_directory_path()
                  / ("mayaflux_shader_processor_" + std::to_string(reinterpret_cast<std::uintptr_t>(owner)) + ".comp"))
        {
            std::ofstream file(path);
            file << k_compute_shader;
        }

        ~TemporaryComputeShader()
        {
            std::error_code error;
            std::filesystem::remove(path, error);
        }
    };

    class ShaderProcessorDeviceTest : public VulkanDeviceTest {
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

        std::shared_ptr<VKBuffer> tracked_buffer(size_t bytes = 64)
        {
            auto buffer = host_buffer(bytes);
            buffers.push_back(buffer);
            return buffer;
        }

        static ShaderConfig compute_config()
        {
            auto config = config_with_binding();
            config.shader_path = k_compute_shader;
            config.stage = Portal::Graphics::ShaderStage::COMPUTE;
            config.push_constant_size = 8;
            return config;
        }

        std::vector<std::shared_ptr<VKBuffer>> buffers;
    };

    TEST_F(ShaderProcessorDeviceTest, BindingAndDetachingUseNamedBuffers)
    {
        ShaderProcessorProbe processor(compute_config());
        auto first = tracked_buffer();
        auto second = tracked_buffer();
        processor.bind_buffer("data", first);

        EXPECT_TRUE(first->is_initialized());
        EXPECT_EQ(processor.get_bound_buffer("data"), first);
        EXPECT_TRUE(processor.are_bindings_complete());
        EXPECT_EQ(processor.get_bound_buffer_count(), 1U);

        processor.bind_buffer("data", second);
        EXPECT_EQ(processor.get_bound_buffer("data"), second);
        processor.on_detach(first);
        EXPECT_EQ(processor.get_bound_buffer("data"), second);
        processor.on_detach(second);
        EXPECT_EQ(processor.get_bound_buffer("data"), nullptr);
        EXPECT_FALSE(processor.are_bindings_complete());
        std::array<float, 4> missing {};
        EXPECT_FALSE(processor.download_bound("data", missing.data(), sizeof(missing)));
    }

    TEST_F(ShaderProcessorDeviceTest, InitialAttachmentAndExplicitAutoBindingUseSequentialNames)
    {
        ShaderProcessorProbe processor(ShaderConfig { k_compute_shader });
        auto first = tracked_buffer();
        auto second = tracked_buffer();
        processor.on_attach(first);
        processor.auto_bind_buffer(second);

        EXPECT_EQ(processor.get_bound_buffer("input"), first);
        EXPECT_EQ(processor.get_bound_buffer("output"), second);
        EXPECT_EQ(processor.get_config().bindings.at("input").set, 1U);
        EXPECT_EQ(processor.get_config().bindings.at("output").binding, 1U);

        processor.unbind_buffer("input");
        EXPECT_EQ(processor.get_bound_buffer("input"), nullptr);
        EXPECT_FALSE(processor.are_bindings_complete());
    }

    TEST_F(ShaderProcessorDeviceTest, ImplicitBindingNeverSharesAnOccupiedSlot)
    {
        ShaderConfig config;
        config.bindings.emplace("existing", ShaderBinding { 1, 1 });
        ShaderProcessorProbe processor(config);
        auto buffer = tracked_buffer();

        processor.bind_buffer("new", buffer);

        ASSERT_TRUE(processor.has_binding("new"));
        EXPECT_EQ(processor.get_config().bindings.at("new").set, 1U);
        EXPECT_NE(processor.get_config().bindings.at("new").binding, 1U);
        EXPECT_EQ(processor.get_config().bindings.at("existing").set, 1U);
        EXPECT_EQ(processor.get_config().bindings.at("existing").binding, 1U);
    }

    TEST_F(ShaderProcessorDeviceTest, ImplicitBindingsExtendContiguousClaimsInOrder)
    {
        ShaderConfig config;
        config.bindings.emplace("first", ShaderBinding { 1, 0 });
        config.bindings.emplace("second", ShaderBinding { 1, 1 });
        ShaderProcessorProbe processor(config);
        auto third = tracked_buffer();
        auto fourth = tracked_buffer();

        processor.bind_buffer("third", third);
        processor.bind_buffer("fourth", fourth);

        EXPECT_EQ(processor.get_config().bindings.at("third").binding, 2U);
        EXPECT_EQ(processor.get_config().bindings.at("fourth").binding, 3U);
        EXPECT_EQ(processor.get_config().bindings.at("first").binding, 0U);
        EXPECT_EQ(processor.get_config().bindings.at("second").binding, 1U);
    }

    TEST_F(ShaderProcessorDeviceTest, ImplicitBindingLeavesExplicitSetZeroClaimsAlone)
    {
        ShaderProcessorProbe processor(compute_config());
        auto buffer = tracked_buffer();

        processor.bind_buffer("extra", buffer);

        EXPECT_EQ(processor.get_config().bindings.at("data").set, 0U);
        EXPECT_EQ(processor.get_config().bindings.at("data").binding, 0U);
        EXPECT_EQ(processor.get_config().bindings.at("extra").set, 1U);
        EXPECT_EQ(processor.get_config().bindings.at("extra").binding, 0U);
    }

    TEST_F(ShaderProcessorDeviceTest, ProcessingLoadsShaderOnceAndRebuildsOnlyWhenMarked)
    {
        ShaderProcessorProbe processor(compute_config());
        auto buffer = tracked_buffer();
        processor.processing_function(buffer);
        processor.processing_function(buffer);

        EXPECT_TRUE(processor.is_shader_loaded());
        EXPECT_EQ(processor.shader_loads, 1U);
        EXPECT_EQ(processor.pipeline_creations, 1U);
        EXPECT_EQ(processor.descriptor_creations, 1U);
        EXPECT_EQ(processor.executions, 2U);
        EXPECT_EQ(processor.before_executions, 2U);
        EXPECT_EQ(processor.after_executions, 2U);

        processor.set_specialization_constant(0, 7);
        processor.processing_function(buffer);
        EXPECT_EQ(processor.pipeline_creations, 2U);
        EXPECT_EQ(processor.descriptor_creations, 2U);
    }

    TEST_F(ShaderProcessorDeviceTest, AddingABindingAtANewSlotCreatesAPipelineWithThatSlot)
    {
        ShaderProcessorProbe processor(compute_config());
        auto buffer = tracked_buffer();
        processor.processing_function(buffer);
        ASSERT_EQ(processor.pipeline_creations, 1U);
        ASSERT_FALSE(processor.created_slots.contains({ 0, 1 }));

        processor.add_binding("additional", ShaderBinding { 0, 1 });
        processor.processing_function(buffer);

        EXPECT_EQ(processor.pipeline_creations, 2U);
        EXPECT_TRUE(processor.created_slots.contains({ 0, 0 }));
        EXPECT_TRUE(processor.created_slots.contains({ 0, 1 }));
    }

    TEST_F(ShaderProcessorDeviceTest, RestatingABindingAtItsOwnSlotKeepsThePipeline)
    {
        ShaderProcessorProbe processor(compute_config());
        auto buffer = tracked_buffer();
        processor.processing_function(buffer);

        processor.add_binding("data", ShaderBinding { 0, 0 });
        processor.processing_function(buffer);

        EXPECT_EQ(processor.pipeline_creations, 1U);
    }

    TEST_F(ShaderProcessorDeviceTest, ImplicitBindingAfterProcessingCreatesAPipelineWithItsSlot)
    {
        ShaderProcessorProbe processor(compute_config());
        auto buffer = tracked_buffer();
        processor.processing_function(buffer);
        ASSERT_EQ(processor.pipeline_creations, 1U);

        processor.bind_buffer("extra", tracked_buffer());
        processor.processing_function(buffer);

        EXPECT_EQ(processor.pipeline_creations, 2U);
        EXPECT_TRUE(processor.created_slots.contains({ 1, 0 }));
    }

    TEST_F(ShaderProcessorDeviceTest, RebindingAnExistingNameKeepsThePipeline)
    {
        ShaderProcessorProbe processor(compute_config());
        auto first = tracked_buffer();
        auto second = tracked_buffer();
        processor.bind_buffer("data", first);
        processor.processing_function(first);

        processor.bind_buffer("data", second);
        processor.processing_function(first);

        EXPECT_EQ(processor.pipeline_creations, 1U);
    }

    TEST_F(ShaderProcessorDeviceTest, SubclassEditOfTheConfigBindingsCreatesAPipelineWithTheNewSlot)
    {
        ShaderProcessorProbe processor(compute_config());
        auto buffer = tracked_buffer();
        processor.processing_function(buffer);

        processor.config_bindings().emplace("direct", ShaderBinding { 0, 2 });
        processor.processing_function(buffer);

        EXPECT_EQ(processor.pipeline_creations, 2U);
        EXPECT_TRUE(processor.created_slots.contains({ 0, 2 }));
    }

    TEST_F(ShaderProcessorDeviceTest, PushConstantFeedBeyondTheCreatedRangeCreatesALargerPipelineRange)
    {
        ShaderProcessorProbe processor(compute_config());
        auto buffer = tracked_buffer();
        processor.processing_function(buffer);
        ASSERT_EQ(processor.created_push_constant_size, 8U);

        processor.feed("extra", [] { return ShaderProcessor::FeedValue { 4.0 }; }, 8, sizeof(float));
        processor.processing_function(buffer);

        EXPECT_EQ(processor.pipeline_creations, 2U);
        EXPECT_EQ(processor.created_push_constant_size, 12U);
    }

    TEST_F(ShaderProcessorDeviceTest, PushConstantFeedWithinTheCreatedRangeKeepsThePipeline)
    {
        ShaderProcessorProbe processor(compute_config());
        auto buffer = tracked_buffer();
        processor.processing_function(buffer);

        processor.feed("inside", [] { return ShaderProcessor::FeedValue { 4.0 }; }, 4, sizeof(float));
        processor.processing_function(buffer);
        processor.processing_function(buffer);

        EXPECT_EQ(processor.pipeline_creations, 1U);
        EXPECT_EQ(processor.created_push_constant_size, 8U);
    }

    TEST_F(ShaderProcessorDeviceTest, BufferPushConstantFragmentBeyondTheCreatedRangeCreatesALargerPipelineRange)
    {
        ShaderProcessorProbe processor(compute_config());
        auto buffer = tracked_buffer();
        processor.processing_function(buffer);
        ASSERT_EQ(processor.created_push_constant_size, 8U);

        buffer->get_pipeline_context().push_constant_bindings.push_back(
            { .offset = 8, .data = { 1, 0, 0, 0 }, .name = "tail" });
        processor.processing_function(buffer);

        EXPECT_EQ(processor.pipeline_creations, 2U);
        EXPECT_EQ(processor.created_push_constant_size, 12U);
    }

    TEST_F(ShaderProcessorDeviceTest, BufferPushConstantFragmentWithinTheCreatedRangeKeepsThePipeline)
    {
        ShaderProcessorProbe processor(compute_config());
        auto buffer = tracked_buffer();
        processor.processing_function(buffer);

        buffer->get_pipeline_context().push_constant_bindings.push_back(
            { .offset = 4, .data = { 1, 0, 0, 0 }, .name = "inside" });
        processor.processing_function(buffer);

        EXPECT_EQ(processor.pipeline_creations, 1U);
    }

    TEST_F(ShaderProcessorDeviceTest, BufferDescriptorAtAnUnseenSlotCreatesAPipelineWithThatSlot)
    {
        ShaderProcessorProbe processor(compute_config());
        auto buffer = tracked_buffer();
        processor.processing_function(buffer);
        ASSERT_EQ(processor.pipeline_creations, 1U);

        DescriptorBindingInfo descriptor;
        descriptor.set = 1;
        descriptor.binding = 3;
        descriptor.type = vk::DescriptorType::eStorageBuffer;
        buffer->get_pipeline_context().descriptor_buffer_bindings.push_back(descriptor);
        processor.processing_function(buffer);

        EXPECT_EQ(processor.pipeline_creations, 2U);
    }

    TEST_F(ShaderProcessorDeviceTest, HotReloadReplacesTheShaderAndRebuildsThePipeline)
    {
        TemporaryComputeShader shader(this);
        ASSERT_TRUE(std::filesystem::exists(shader.path));
        auto config = config_with_binding();
        config.shader_path = shader.path.string();
        config.stage = Portal::Graphics::ShaderStage::COMPUTE;
        ShaderProcessorProbe processor(config);
        auto buffer = tracked_buffer();
        processor.processing_function(buffer);
        ASSERT_NE(processor.loaded_shader(), INVALID_SHADER);
        const auto original_shader = processor.loaded_shader();

        ASSERT_TRUE(processor.hot_reload_shader());
        EXPECT_NE(processor.loaded_shader(), original_shader);
        EXPECT_TRUE(processor.needs_pipeline_rebuild());
        EXPECT_EQ(processor.shader_loads, 2U);

        processor.processing_function(buffer);
        EXPECT_EQ(processor.pipeline_creations, 2U);
    }

    TEST_F(ShaderProcessorDeviceTest, StorageFeedCreatesGrowsAndUnbindsItsBackingBuffer)
    {
        ShaderProcessorProbe processor(compute_config());
        std::vector<float> values { 1.0F, 2.0F, 3.0F, 4.0F };
        processor.feed("data", [&] {
            return ShaderProcessor::FeedValue { Kakshya::DataVariant(values) };
        });
        processor.pull_feeds();

        auto backing = processor.get_bound_buffer("data");
        ASSERT_NE(backing, nullptr);
        EXPECT_TRUE(backing->is_initialized());
        EXPECT_GE(backing->get_size_bytes(), values.size() * sizeof(float));
        buffers.push_back(backing);
        const auto initial_capacity = backing->get_size_bytes();

        values.resize(32, 6.0F);
        processor.pull_feeds();
        EXPECT_EQ(processor.get_bound_buffer("data"), backing);
        EXPECT_GT(backing->get_size_bytes(), initial_capacity);

        processor.remove_feed("data");
        EXPECT_FALSE(processor.has_feed("data"));
        EXPECT_EQ(processor.get_bound_buffer("data"), nullptr);
    }

    TEST_F(ShaderProcessorDeviceTest, StorageFeedAndPushConstantFeedReachOneGpuDispatch)
    {
        ShaderProcessorProbe processor(compute_config(), true);
        auto target = tracked_buffer();
        std::vector<float> values { 1.0F, 2.0F, 3.0F, 4.0F };
        processor.feed("data", [&] {
            return ShaderProcessor::FeedValue { Kakshya::DataVariant(values) };
        });
        processor.feed("factor", [] { return ShaderProcessor::FeedValue { 3.0 }; }, 0);
        processor.feed("bias", [] { return ShaderProcessor::FeedValue { 2.0 }; }, 4);

        processor.processing_function(target);

        EXPECT_TRUE(processor.is_shader_loaded());
        EXPECT_TRUE(processor.are_descriptors_ready());
        auto output = processor.get_bound_buffer("data");
        ASSERT_NE(output, nullptr);
        buffers.push_back(output);
        std::vector<uint8_t> actual(output->get_size_bytes());
        ASSERT_TRUE(processor.download_bound("data", actual.data(), actual.size()));
        EXPECT_FLOAT_EQ(read_bytes<float>(actual, 0), 5.0F);
        EXPECT_FLOAT_EQ(read_bytes<float>(actual, 4), 8.0F);
        EXPECT_FLOAT_EQ(read_bytes<float>(actual, 8), 11.0F);
        EXPECT_FLOAT_EQ(read_bytes<float>(actual, 12), 14.0F);
    }

    TEST_F(ShaderProcessorDeviceTest, BufferPushConstantFragmentOverridesProcessorValueOnGpu)
    {
        ShaderProcessorProbe processor(compute_config(), true);
        auto buffer = tracked_buffer(4 * sizeof(float));
        processor.bind_buffer("data", buffer);
        buffer->set_data({ Kakshya::DataVariant(std::vector<float> { 1.0F, 2.0F, 3.0F, 4.0F }) });
        processor.set_push_constant_data(std::array<float, 2> { 2.0F, 1.0F });
        float override_bias = 5.0F;
        std::vector<uint8_t> fragment(sizeof(float));
        std::memcpy(fragment.data(), &override_bias, sizeof(float));
        buffer->get_pipeline_context().push_constant_bindings.push_back(
            { .offset = 4, .data = fragment, .name = "bias" });

        processor.processing_function(buffer);

        std::vector<uint8_t> actual(buffer->get_size_bytes());
        ASSERT_TRUE(processor.download_bound("data", actual.data(), actual.size()));
        EXPECT_FLOAT_EQ(read_bytes<float>(actual, 0), 7.0F);
        EXPECT_FLOAT_EQ(read_bytes<float>(actual, 4), 9.0F);
        EXPECT_FLOAT_EQ(read_bytes<float>(actual, 8), 11.0F);
        EXPECT_FLOAT_EQ(read_bytes<float>(actual, 12), 13.0F);
    }

    TEST_F(ShaderProcessorDeviceTest, RebindingStorageDescriptorUpdatesTheNextDispatch)
    {
        ShaderProcessorProbe processor(compute_config(), true);
        auto first = tracked_buffer(4 * sizeof(float));
        auto second = tracked_buffer(4 * sizeof(float));
        processor.bind_buffer("data", second);
        second->set_data({ Kakshya::DataVariant(std::vector<float>(4, 10.0F)) });
        processor.bind_buffer("data", first);
        first->set_data({ Kakshya::DataVariant(std::vector<float>(4, 2.0F)) });
        processor.set_push_constant_data(std::array<float, 2> { 2.0F, 1.0F });

        processor.processing_function(first);
        processor.bind_buffer("data", second);
        processor.processing_function(first);

        std::vector<uint8_t> first_values(first->get_size_bytes());
        std::vector<uint8_t> second_values(second->get_size_bytes());
        ASSERT_TRUE(processor.download_bound("data", second_values.data(), second_values.size()));
        std::memcpy(first_values.data(), first->get_buffer_resources().mapped_ptr, first_values.size());
        EXPECT_FLOAT_EQ(read_bytes<float>(first_values, 0), 5.0F);
        EXPECT_FLOAT_EQ(read_bytes<float>(second_values, 0), 21.0F);
        EXPECT_EQ(processor.pipeline_creations, 1U);
        EXPECT_EQ(processor.descriptor_creations, 2U);
    }

    TEST_F(ShaderProcessorDeviceTest, BufferStagedDescriptorTakesPrecedenceOverNamedBinding)
    {
        ShaderProcessorProbe processor(compute_config(), true);
        auto primary = tracked_buffer(4 * sizeof(float));
        auto staged = tracked_buffer(4 * sizeof(float));
        processor.bind_buffer("data", primary);
        primary->set_data({ Kakshya::DataVariant(std::vector<float>(4, 2.0F)) });
        auto* service = Registry::BackendRegistry::instance()
                            .get_service<Registry::Service::BufferService>();
        ASSERT_NE(service, nullptr);
        service->initialize_buffer(staged);
        staged->set_data({ Kakshya::DataVariant(std::vector<float>(4, 10.0F)) });
        DescriptorBindingInfo descriptor;
        descriptor.set = 0;
        descriptor.binding = 0;
        descriptor.type = vk::DescriptorType::eStorageBuffer;
        descriptor.buffer_info = vk::DescriptorBufferInfo(staged->get_buffer(), 0, staged->get_size_bytes());
        primary->get_pipeline_context().descriptor_buffer_bindings.push_back(descriptor);
        processor.set_push_constant_data(std::array<float, 2> { 2.0F, 1.0F });

        processor.processing_function(primary);

        std::array<float, 4> primary_values {};
        std::array<float, 4> staged_values {};
        std::memcpy(primary_values.data(), primary->get_buffer_resources().mapped_ptr, sizeof(primary_values));
        std::memcpy(staged_values.data(), staged->get_buffer_resources().mapped_ptr, sizeof(staged_values));
        EXPECT_FLOAT_EQ(primary_values[0], 2.0F);
        EXPECT_FLOAT_EQ(staged_values[0], 21.0F);

        primary->get_pipeline_context().descriptor_buffer_bindings.clear();
        processor.processing_function(primary);
        std::memcpy(primary_values.data(), primary->get_buffer_resources().mapped_ptr, sizeof(primary_values));
        std::memcpy(staged_values.data(), staged->get_buffer_resources().mapped_ptr, sizeof(staged_values));
        EXPECT_FLOAT_EQ(primary_values[0], 5.0F);
        EXPECT_FLOAT_EQ(staged_values[0], 21.0F);
    }

    TEST_F(ShaderProcessorDeviceTest, TwoNamedStorageDescriptorsReachDistinctShaderBindings)
    {
        ShaderConfig config;
        config.shader_path = k_two_storage_shader;
        config.stage = Portal::Graphics::ShaderStage::COMPUTE;
        config.push_constant_size = 8;
        config.bindings.emplace("source", ShaderBinding { 0, 0 });
        config.bindings.emplace("destination", ShaderBinding { 0, 1 });
        ShaderProcessorProbe processor(config, true);
        auto source = tracked_buffer(4 * sizeof(float));
        auto destination = tracked_buffer(4 * sizeof(float));
        processor.bind_buffer("source", source);
        processor.bind_buffer("destination", destination);
        source->set_data({ Kakshya::DataVariant(std::vector<float> { 1.0F, 2.0F, 3.0F, 4.0F }) });
        processor.set_push_constant_data(std::array<float, 2> { 3.0F, 2.0F });

        processor.processing_function(source);

        std::vector<uint8_t> actual(destination->get_size_bytes());
        ASSERT_TRUE(processor.download_bound("destination", actual.data(), actual.size()));
        EXPECT_FLOAT_EQ(read_bytes<float>(actual, 0), 5.0F);
        EXPECT_FLOAT_EQ(read_bytes<float>(actual, 4), 8.0F);
        EXPECT_FLOAT_EQ(read_bytes<float>(actual, 8), 11.0F);
        EXPECT_FLOAT_EQ(read_bytes<float>(actual, 12), 14.0F);
    }

    TEST_F(ShaderProcessorDeviceTest, UniformAndStorageDescriptorsKeepTheirDeclaredTypes)
    {
        ShaderConfig config;
        config.shader_path = k_uniform_and_storage_shader;
        config.stage = Portal::Graphics::ShaderStage::COMPUTE;
        config.push_constant_size = 8;
        config.bindings.emplace("source", ShaderBinding { 0, 0, vk::DescriptorType::eUniformBuffer });
        config.bindings.emplace("destination", ShaderBinding { 0, 1 });
        ShaderProcessorProbe processor(config, true);
        auto source = std::make_shared<VKBuffer>(16, VKBuffer::Usage::UNIFORM,
            Kakshya::DataModality::UNKNOWN);
        buffers.push_back(source);
        auto destination = tracked_buffer(4 * sizeof(float));
        processor.bind_buffer("source", source);
        processor.bind_buffer("destination", destination);
        source->set_data({ Kakshya::DataVariant(std::vector<float> { 6.0F, 0.0F, 0.0F, 0.0F }) });
        processor.set_push_constant_data(std::array<float, 2> { 2.0F, 1.0F });

        processor.processing_function(source);

        std::vector<uint8_t> actual(destination->get_size_bytes());
        ASSERT_TRUE(processor.download_bound("destination", actual.data(), actual.size()));
        for (size_t i = 0; i < 4; ++i)
            EXPECT_FLOAT_EQ(read_bytes<float>(actual, i * sizeof(float)), 13.0F);
    }

    TEST_F(ShaderProcessorDeviceTest, StagedDescriptorOffsetAndRangeSelectOnlyTheTargetRegion)
    {
        ShaderProcessorProbe processor(compute_config(), true);
        constexpr size_t offset = 256;
        auto buffer = tracked_buffer(offset + 4 * sizeof(float));
        processor.bind_buffer("data", buffer);
        std::vector<float> values(68, 0.0F);
        values[64] = 1.0F;
        values[65] = 2.0F;
        values[66] = 3.0F;
        values[67] = 4.0F;
        buffer->set_data({ Kakshya::DataVariant(values) });
        DescriptorBindingInfo descriptor;
        descriptor.set = 0;
        descriptor.binding = 0;
        descriptor.type = vk::DescriptorType::eStorageBuffer;
        descriptor.buffer_info = vk::DescriptorBufferInfo(buffer->get_buffer(), offset, 4 * sizeof(float));
        buffer->get_pipeline_context().descriptor_buffer_bindings.push_back(descriptor);
        processor.set_push_constant_data(std::array<float, 2> { 2.0F, 1.0F });

        processor.processing_function(buffer);

        std::vector<uint8_t> actual(buffer->get_size_bytes());
        ASSERT_TRUE(processor.download_bound("data", actual.data(), actual.size()));
        EXPECT_FLOAT_EQ(read_bytes<float>(actual, 0), 0.0F);
        EXPECT_FLOAT_EQ(read_bytes<float>(actual, offset), 3.0F);
        EXPECT_FLOAT_EQ(read_bytes<float>(actual, offset + 4), 5.0F);
        EXPECT_FLOAT_EQ(read_bytes<float>(actual, offset + 8), 7.0F);
        EXPECT_FLOAT_EQ(read_bytes<float>(actual, offset + 12), 9.0F);
    }

    TEST_F(ShaderProcessorDeviceTest, DeferredSubmissionRetainsBufferUntilFenceResolution)
    {
        ShaderProcessorProbe processor(compute_config(), true);
        auto buffer = tracked_buffer(4 * sizeof(float));
        processor.bind_buffer("data", buffer);
        buffer->set_data({ Kakshya::DataVariant(std::vector<float>(4, 2.0F)) });
        processor.set_push_constant_data(std::array<float, 2> { 3.0F, 1.0F });
        processor.set_deferred_submission(true);

        processor.processing_function(buffer);
        EXPECT_TRUE(processor.is_dispatch_pending());
        EXPECT_EQ(processor.dispatch_completions, 0U);

        EXPECT_TRUE(processor.resolve_pending_dispatch(true));
        EXPECT_FALSE(processor.is_dispatch_pending());
        EXPECT_EQ(processor.dispatch_completions, 1U);
        EXPECT_FALSE(processor.resolve_pending_dispatch(false));
        processor.set_deferred_submission(false);

        std::vector<uint8_t> actual(buffer->get_size_bytes());
        ASSERT_TRUE(processor.download_bound("data", actual.data(), actual.size()));
        EXPECT_FLOAT_EQ(read_bytes<float>(actual, 0), 7.0F);
    }

}
}
