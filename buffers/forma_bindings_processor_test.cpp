#include "gtest/gtest.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../support/vulkan_test_support.hpp"

#include "MayaFlux/Buffers/BufferProcessingChain.hpp"
#include "MayaFlux/Buffers/Forma/FormaBindingsProcessor.hpp"
#include "MayaFlux/Buffers/Forma/FormaBuffer.hpp"
#include "MayaFlux/Buffers/Staging/StagingUtils.hpp"
#include "MayaFlux/Portal/Graphics/ShaderFoundry.hpp"
#include "MayaFlux/Registry/BackendRegistry.hpp"
#include "MayaFlux/Registry/Service/BufferService.hpp"

namespace MayaFlux::Test {
namespace {

class FormaBindingsProcessorDeviceTest : public VulkanDeviceTest {
protected:
    static void SetUpTestSuite()
    {
        VulkanDeviceTest::SetUpTestSuite();
        if (!vulkan_unavailable_reason().empty())
            return;

        auto borrowed = std::shared_ptr<Core::VulkanBackend>(&backend(), [](Core::VulkanBackend*) {});
        Portal::Graphics::get_shader_foundry().shutdown();
        s_foundry_ready = Portal::Graphics::get_shader_foundry().initialize(borrowed);
    }

    static void TearDownTestSuite()
    {
        if (s_foundry_ready)
            Portal::Graphics::get_shader_foundry().shutdown();
        s_foundry_ready = false;
        VulkanDeviceTest::TearDownTestSuite();
    }

    void SetUp() override
    {
        VulkanDeviceTest::SetUp();
        if (IsSkipped())
            return;
        ASSERT_TRUE(s_foundry_ready);

        buffer = std::make_shared<Buffers::FormaBuffer>(
            256, Portal::Graphics::PrimitiveTopology::POINT_LIST);
        auto* service = Registry::BackendRegistry::instance()
                            .get_service<Registry::Service::BufferService>();
        ASSERT_NE(service, nullptr);
        service->initialize_buffer(buffer);
        ASSERT_TRUE(buffer->is_initialized());
        buffer->setup_processors(Buffers::ProcessingToken::GRAPHICS_BACKEND);

        Buffers::ShaderConfig config { "point.vert.spv" };
        config.stage = Portal::Graphics::ShaderStage::VERTEX;
        bindings = std::make_shared<Buffers::FormaBindingsProcessor>(std::move(config));
        std::string rejection;
        ASSERT_TRUE(buffer->get_processing_chain()->add_processor(bindings, buffer, &rejection))
            << rejection;
    }

    void TearDown() override
    {
        if (buffer) {
            if (bindings && buffer->get_processing_chain())
                buffer->get_processing_chain()->remove_processor(bindings, buffer);
            buffer->set_default_processor(nullptr);
            if (buffer->is_initialized()) {
                auto* service = Registry::BackendRegistry::instance()
                                    .get_service<Registry::Service::BufferService>();
                if (service)
                    service->destroy_buffer(buffer);
            }
        }
        bindings.reset();
        buffer.reset();
    }

    void tick() { buffer->get_processing_chain()->process(buffer); }

    static float gpu_float(const std::shared_ptr<Buffers::VKBuffer>& source)
    {
        std::vector<uint8_t> bytes(source->get_size_bytes());
        Buffers::download_from_gpu(source, bytes.data(), bytes.size());
        float value {};
        std::memcpy(&value, bytes.data(), sizeof(value));
        return value;
    }

    std::shared_ptr<Buffers::FormaBuffer> buffer;
    std::shared_ptr<Buffers::FormaBindingsProcessor> bindings;

private:
    static inline bool s_foundry_ready {};
};

TEST_F(FormaBindingsProcessorDeviceTest, TheTargetsSharedShaderIsNeverKeptForDestruction)
{
    bindings->bind_push_constant("gain", [] { return 1.F; }, 0);
    tick();

    EXPECT_FALSE(bindings->is_shader_loaded());
}

TEST_F(FormaBindingsProcessorDeviceTest, SetupProcessorsConfiguresTheGraphicsChain)
{
    auto other = std::make_shared<Buffers::FormaBuffer>(
        256, Portal::Graphics::PrimitiveTopology::POINT_LIST);
    auto* service = Registry::BackendRegistry::instance()
                        .get_service<Registry::Service::BufferService>();
    ASSERT_NE(service, nullptr);
    service->initialize_buffer(other);
    ASSERT_TRUE(other->is_initialized());

    auto original_chain = other->get_processing_chain();
    other->setup_processors(Buffers::ProcessingToken::GRAPHICS_BACKEND);
    EXPECT_EQ(other->get_processing_chain(), original_chain);
    EXPECT_EQ(other->get_processing_chain()->get_preferred_token(),
        Buffers::ProcessingToken::GRAPHICS_BACKEND);

    other->set_default_processor(nullptr);
    service->destroy_buffer(other);
}

TEST_F(FormaBindingsProcessorDeviceTest, RawPushConstantsWriteAtTheirOffsetsAndTrackReaderChanges)
{
    float first = 1.25F;
    float second = -2.5F;
    bindings->bind_push_constant("first", [&] { return first; }, 4);
    bindings->bind_push_constant("second", [&] { return second; }, 16, sizeof(double));

    tick();

    auto& entries = buffer->get_pipeline_context().push_constant_bindings;
    ASSERT_EQ(entries.size(), 2U);
    auto at_offset = [&](uint32_t offset) {
        return std::ranges::find_if(entries, [=](const auto& entry) {
            return entry.offset == offset;
        });
    };
    auto four = at_offset(4);
    auto eight = at_offset(16);
    ASSERT_NE(four, entries.end());
    ASSERT_NE(eight, entries.end());
    ASSERT_EQ(four->data.size(), sizeof(float));
    ASSERT_EQ(eight->data.size(), sizeof(double));
    float written_first {};
    double written_second {};
    std::memcpy(&written_first, four->data.data(), sizeof(written_first));
    std::memcpy(&written_second, eight->data.data(), sizeof(written_second));
    EXPECT_FLOAT_EQ(written_first, first);
    EXPECT_DOUBLE_EQ(written_second, static_cast<double>(second));

    first = -9.75F;
    second = 3.125F;
    tick();

    ASSERT_EQ(entries.size(), 2U);
    four = at_offset(4);
    eight = at_offset(16);
    std::memcpy(&written_first, four->data.data(), sizeof(written_first));
    std::memcpy(&written_second, eight->data.data(), sizeof(written_second));
    EXPECT_FLOAT_EQ(written_first, first);
    EXPECT_DOUBLE_EQ(written_second, static_cast<double>(second));
}

template <typename T>
std::optional<T> staged_value(const Buffers::FormaBuffer& buffer, uint32_t offset)
{
    for (const auto& entry : buffer.get_pipeline_context().push_constant_bindings) {
        if (entry.offset == offset && entry.data.size() == sizeof(T)) {
            T value {};
            std::memcpy(&value, entry.data.data(), sizeof(value));
            return value;
        }
    }
    return std::nullopt;
}

TEST_F(FormaBindingsProcessorDeviceTest, ATypedBindingWritesAVec3WithEveryComponentAndTracksChanges)
{
    glm::vec3 value { 1.0F, -2.0F, 3.5F };
    bindings->bind_typed_push_constant("direction", [&] { return Kakshya::GpuValue { value }; }, 16);

    tick();
    EXPECT_EQ(staged_value<glm::vec3>(*buffer, 16), (glm::vec3 { 1.0F, -2.0F, 3.5F }));

    value = glm::vec3 { 9.0F, 8.0F, 7.0F };
    tick();
    EXPECT_EQ(staged_value<glm::vec3>(*buffer, 16), (glm::vec3 { 9.0F, 8.0F, 7.0F }));
}

TEST_F(FormaBindingsProcessorDeviceTest, ATypedBindingWritesAMatrix)
{
    glm::mat4 value(1.0F);
    value[3][1] = 4.0F;
    bindings->bind_typed_push_constant("transform", [&] { return Kakshya::GpuValue { value }; }, 0);

    tick();

    EXPECT_EQ(staged_value<glm::mat4>(*buffer, 0), value);
}

TEST_F(FormaBindingsProcessorDeviceTest, ATypedBindingKeepsIntegersExactBeyondFloatPrecision)
{
    bindings->bind_typed_push_constant("big", [] { return Kakshya::GpuValue { int32_t { 16777217 } }; }, 0);

    tick();

    EXPECT_EQ(staged_value<int32_t>(*buffer, 0), 16777217);
}

TEST_F(FormaBindingsProcessorDeviceTest, ATypedBindingResizesItsSlotWhenTheFormatChanges)
{
    bool wide = false;
    bindings->bind_typed_push_constant("shape", [&] {
        return wide ? Kakshya::GpuValue { glm::vec4 { 1.0F } } : Kakshya::GpuValue { 2.0F };
    }, 0);

    tick();
    ASSERT_EQ(staged_value<float>(*buffer, 0), 2.0F);

    wide = true;
    tick();
    EXPECT_EQ(staged_value<glm::vec4>(*buffer, 0), glm::vec4 { 1.0F });
}

TEST_F(FormaBindingsProcessorDeviceTest, ATypedBindingRejectsANullReader)
{
    bindings->bind_typed_push_constant("gain", std::function<Kakshya::GpuValue()> {}, 0);

    EXPECT_FALSE(bindings->has_binding("gain"));
}

TEST_F(FormaBindingsProcessorDeviceTest, TypedAndFloatBindingsCoexist)
{
    bindings->bind_push_constant("scalar", [] { return 1.5F; }, 0);
    bindings->bind_typed_push_constant("vector", [] { return Kakshya::GpuValue { glm::vec2 { 3.0F, 4.0F } }; }, 8);

    tick();

    EXPECT_EQ(staged_value<float>(*buffer, 0), 1.5F);
    EXPECT_EQ(staged_value<glm::vec2>(*buffer, 8), (glm::vec2 { 3.0F, 4.0F }));
}

TEST_F(FormaBindingsProcessorDeviceTest, RebindingATypedNameToAFloatReaderDropsTheTypedValue)
{
    bindings->bind_typed_push_constant("slot", [] { return Kakshya::GpuValue { glm::vec3 { 1.0F } }; }, 0);
    tick();
    ASSERT_TRUE(staged_value<glm::vec3>(*buffer, 0).has_value());

    bindings->bind_push_constant("slot", [] { return 6.5F; }, 0);
    tick();

    EXPECT_EQ(staged_value<float>(*buffer, 0), 6.5F);
    EXPECT_FALSE(staged_value<glm::vec3>(*buffer, 0).has_value());
}

TEST_F(FormaBindingsProcessorDeviceTest, UnbindingATypedBindingRemovesItsStagedEntry)
{
    bindings->bind_typed_push_constant("direction", [] { return Kakshya::GpuValue { glm::vec3 { 1.0F } }; }, 0);
    tick();
    ASSERT_TRUE(staged_value<glm::vec3>(*buffer, 0).has_value());

    EXPECT_TRUE(bindings->unbind("direction"));
    tick();

    EXPECT_FALSE(staged_value<glm::vec3>(*buffer, 0).has_value());
}

TEST_F(FormaBindingsProcessorDeviceTest, AFormatBindingConvertsTheReaderToItsFormat)
{
    float value = 7.9F;
    bindings->bind_push_constant("count", [&] { return value; }, 4, Kakshya::GpuDataFormat::UINT32);

    tick();

    EXPECT_EQ(staged_value<uint32_t>(*buffer, 4), 7U);
    value = 12.2F;
    tick();
    EXPECT_EQ(staged_value<uint32_t>(*buffer, 4), 12U);
}

TEST_F(FormaBindingsProcessorDeviceTest, AFormatBindingKeepsTheFormatsWidth)
{
    bindings->bind_push_constant("small", [] { return 3.0F; }, 0, Kakshya::GpuDataFormat::UINT8);
    bindings->bind_push_constant("mid", [] { return 4.0F; }, 4, Kakshya::GpuDataFormat::UINT16);
    bindings->bind_push_constant("wide", [] { return 2.5F; }, 8, Kakshya::GpuDataFormat::FLOAT64);

    tick();

    EXPECT_EQ(staged_value<uint8_t>(*buffer, 0), 3U);
    EXPECT_EQ(staged_value<uint16_t>(*buffer, 4), 4U);
    EXPECT_EQ(staged_value<double>(*buffer, 8), 2.5);
}

TEST_F(FormaBindingsProcessorDeviceTest, AFormatBindingSaturatesIntegerFormats)
{
    float value = -5.0F;
    bindings->bind_push_constant("clamped", [&] { return value; }, 0, Kakshya::GpuDataFormat::UINT8);

    tick();
    EXPECT_EQ(staged_value<uint8_t>(*buffer, 0), 0U);

    value = 300.0F;
    tick();
    EXPECT_EQ(staged_value<uint8_t>(*buffer, 0), 255U);
}

TEST_F(FormaBindingsProcessorDeviceTest, AFormatBindingAcceptsSignedIntegers)
{
    bindings->bind_push_constant("signed", [] { return -3.7F; }, 0, Kakshya::GpuDataFormat::INT32);

    tick();

    EXPECT_EQ(staged_value<int32_t>(*buffer, 0), -3);
}

TEST_F(FormaBindingsProcessorDeviceTest, AMultiComponentFormatCannotBeBoundToASingleReader)
{
    bindings->bind_push_constant("direction", [] { return 1.0F; }, 0, Kakshya::GpuDataFormat::VEC3_F32);

    tick();

    EXPECT_FALSE(bindings->has_binding("direction"));
    EXPECT_TRUE(buffer->get_pipeline_context().push_constant_bindings.empty());
}

TEST_F(FormaBindingsProcessorDeviceTest, AFormatBindingRejectsANullReader)
{
    bindings->bind_push_constant("gain", std::function<float()> {}, 0, Kakshya::GpuDataFormat::UINT32);

    EXPECT_FALSE(bindings->has_binding("gain"));
}

TEST_F(FormaBindingsProcessorDeviceTest, FormatAndWidthBindingsCoexistAtDifferentOffsets)
{
    bindings->bind_push_constant("by_width", [] { return 1.5F; }, 0);
    bindings->bind_push_constant("by_format", [] { return 9.0F; }, 4, Kakshya::GpuDataFormat::UINT32);

    tick();

    EXPECT_EQ(staged_value<float>(*buffer, 0), 1.5F);
    EXPECT_EQ(staged_value<uint32_t>(*buffer, 4), 9U);
}

TEST_F(FormaBindingsProcessorDeviceTest, RebindingANameSwitchesBetweenWidthAndFormat)
{
    bindings->bind_push_constant("gain", [] { return 6.0F; }, 0, Kakshya::GpuDataFormat::UINT32);
    tick();
    ASSERT_EQ(staged_value<uint32_t>(*buffer, 0), 6U);

    bindings->bind_push_constant("gain", [] { return 6.5F; }, 0);
    tick();

    EXPECT_EQ(staged_value<float>(*buffer, 0), 6.5F);
}

TEST_F(FormaBindingsProcessorDeviceTest, AMappedStateFormatBindingProjectsThenConverts)
{
    auto state = std::make_shared<Portal::Forma::MappedState<float>>();
    state->write(-4.0F);
    bindings->bind_push_constant<float>("projected", state,
        std::function<float(float)>([](float value) { return value * 3.0F; }), 0,
        Kakshya::GpuDataFormat::INT32);

    tick();
    EXPECT_EQ(staged_value<int32_t>(*buffer, 0), -12);

    state->write(2.0F);
    tick();
    EXPECT_EQ(staged_value<int32_t>(*buffer, 0), 6);
}

TEST_F(FormaBindingsProcessorDeviceTest, AMappedStateFormatBindingRejectsANullState)
{
    bindings->bind_push_constant<float>("projected", nullptr,
        std::function<float(float)>([](float value) { return value; }), 0,
        Kakshya::GpuDataFormat::UINT32);

    EXPECT_FALSE(bindings->has_binding("projected"));
}

TEST_F(FormaBindingsProcessorDeviceTest, UnbindingAFormatBindingRemovesItsStagedEntry)
{
    bindings->bind_push_constant("count", [] { return 5.0F; }, 0, Kakshya::GpuDataFormat::UINT32);
    tick();
    ASSERT_EQ(staged_value<uint32_t>(*buffer, 0), 5U);

    EXPECT_TRUE(bindings->unbind("count"));
    tick();

    EXPECT_FALSE(staged_value<uint32_t>(*buffer, 0).has_value());
}

TEST_F(FormaBindingsProcessorDeviceTest, MappedStateProjectionIsSampledOnEveryTick)
{
    auto state = std::make_shared<Portal::Forma::MappedState<float>>();
    state->write(2.0F);
    bindings->bind_push_constant<float>("projected", state,
        std::function<float(float)>([](float value) { return value * 3.0F; }), 12);

    tick();
    auto& entries = buffer->get_pipeline_context().push_constant_bindings;
    ASSERT_EQ(entries.size(), 1U);
    float written {};
    std::memcpy(&written, entries.front().data.data(), sizeof(written));
    EXPECT_FLOAT_EQ(written, 6.0F);

    state->write(-4.0F);
    tick();
    ASSERT_EQ(entries.size(), 1U);
    std::memcpy(&written, entries.front().data.data(), sizeof(written));
    EXPECT_FLOAT_EQ(written, -12.0F);
}

TEST_F(FormaBindingsProcessorDeviceTest, DescriptorValuesReachGpuAndKeepTheSameSlotAcrossTicks)
{
    float value = 0.5F;
    bindings->bind_descriptor("gain", [&] { return value; }, "gain_ubo", 3, 2);

    tick();

    auto descriptor_buffer = bindings->get_bound_buffer("gain_ubo");
    ASSERT_NE(descriptor_buffer, nullptr);
    EXPECT_FLOAT_EQ(gpu_float(descriptor_buffer), value);
    auto& entries = buffer->get_pipeline_context().descriptor_buffer_bindings;
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries.front().set, 2U);
    EXPECT_EQ(entries.front().binding, 3U);
    EXPECT_EQ(entries.front().type, vk::DescriptorType::eUniformBuffer);
    EXPECT_EQ(entries.front().buffer_info.buffer, descriptor_buffer->get_buffer());
    EXPECT_EQ(entries.front().buffer_info.range, sizeof(float));

    value = -7.25F;
    tick();

    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries.front().buffer_info.buffer, descriptor_buffer->get_buffer());
    EXPECT_FLOAT_EQ(gpu_float(descriptor_buffer), value);
}

TEST_F(FormaBindingsProcessorDeviceTest, MappedStateStorageDescriptorProjectsIntoItsOwnGpuBuffer)
{
    auto state = std::make_shared<Portal::Forma::MappedState<float>>();
    state->write(8.0F);
    bindings->bind_descriptor<float>("scaled", state,
        std::function<float(float)>([](float value) { return value / 4.0F; }),
        "scaled_ssbo", 1, 3, Portal::Graphics::DescriptorRole::STORAGE);

    tick();

    auto descriptor_buffer = bindings->get_bound_buffer("scaled_ssbo");
    ASSERT_NE(descriptor_buffer, nullptr);
    EXPECT_FLOAT_EQ(gpu_float(descriptor_buffer), 2.0F);
    auto& entries = buffer->get_pipeline_context().descriptor_buffer_bindings;
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries.front().set, 3U);
    EXPECT_EQ(entries.front().binding, 1U);
    EXPECT_EQ(entries.front().type, vk::DescriptorType::eStorageBuffer);

    state->write(-6.0F);
    tick();
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_FLOAT_EQ(gpu_float(descriptor_buffer), -1.5F);
}

TEST_F(FormaBindingsProcessorDeviceTest, UnbindingAPushConstantStopsSamplingItsReader)
{
    int calls = 0;
    bindings->bind_push_constant("live", [&] {
        ++calls;
        return 4.0F;
    }, 0);
    tick();
    ASSERT_EQ(calls, 1);
    ASSERT_EQ(buffer->get_pipeline_context().push_constant_bindings.size(), 1U);

    EXPECT_TRUE(bindings->has_binding("live"));
    EXPECT_EQ(bindings->get_binding_names(), std::vector<std::string> { "live" });
    EXPECT_TRUE(bindings->unbind("live"));
    EXPECT_FALSE(bindings->unbind("live"));
    EXPECT_FALSE(bindings->has_binding("live"));
    EXPECT_TRUE(bindings->get_binding_names().empty());

    tick();
    EXPECT_EQ(calls, 1);
    EXPECT_TRUE(buffer->get_pipeline_context().push_constant_bindings.empty());
}

TEST_F(FormaBindingsProcessorDeviceTest, BindingNamesListEveryPushConstantAndDescriptor)
{
    bindings->bind_push_constant("pc_a", [] { return 1.0F; }, 0);
    bindings->bind_push_constant("pc_b", [] { return 2.0F; }, 4);
    bindings->bind_descriptor("desc", [] { return 3.0F; }, "desc_ubo", 0, 1);

    auto names = bindings->get_binding_names();
    std::ranges::sort(names);
    EXPECT_EQ(names, (std::vector<std::string> { "desc", "pc_a", "pc_b" }));
    EXPECT_TRUE(bindings->has_binding("pc_a"));
    EXPECT_FALSE(bindings->has_binding("missing"));
    EXPECT_FALSE(bindings->unbind("missing"));
}

TEST_F(FormaBindingsProcessorDeviceTest, EveryBindingIsSampledOnEveryTick)
{
    int push_calls = 0;
    int descriptor_calls = 0;
    bindings->bind_push_constant("pc", [&] { return static_cast<float>(++push_calls); }, 0);
    bindings->bind_descriptor("desc", [&] { return static_cast<float>(++descriptor_calls); },
        "desc_ubo", 0, 1);

    tick();
    tick();
    tick();

    EXPECT_EQ(push_calls, 3);
    EXPECT_EQ(descriptor_calls, 3);
    EXPECT_FLOAT_EQ(gpu_float(bindings->get_bound_buffer("desc_ubo")), 3.0F);
    float staged {};
    std::memcpy(&staged,
        buffer->get_pipeline_context().push_constant_bindings.front().data.data(), sizeof(staged));
    EXPECT_FLOAT_EQ(staged, 3.0F);
}

TEST_F(FormaBindingsProcessorDeviceTest, NullPushReaderDoesNotReplaceAWorkingBinding)
{
    float value = 1.0F;
    bindings->bind_push_constant("gain", [&] { return value; }, 0);
    tick();

    bindings->bind_push_constant("gain", std::function<float()> {}, 0);
    value = 4.0F;
    tick();

    auto& entries = buffer->get_pipeline_context().push_constant_bindings;
    ASSERT_EQ(entries.size(), 1U);
    float staged {};
    std::memcpy(&staged, entries.front().data.data(), sizeof(staged));
    EXPECT_FLOAT_EQ(staged, value);
}

TEST_F(FormaBindingsProcessorDeviceTest, NullStateAndNullDescriptorReaderDoNotReplaceLiveBindings)
{
    float value = 5.0F;
    bindings->bind_descriptor("gain", [&] { return value; }, "gain_ubo", 0, 1);
    tick();
    auto descriptor_buffer = bindings->get_bound_buffer("gain_ubo");
    ASSERT_NE(descriptor_buffer, nullptr);

    bindings->bind_descriptor("gain", std::function<float()> {}, "gain_ubo", 0, 1);
    bindings->bind_push_constant<float>("bad_push", nullptr,
        std::function<float(float)>([](float input) { return input; }), 0);
    bindings->bind_descriptor<float>("bad_descriptor", nullptr,
        std::function<float(float)>([](float input) { return input; }),
        "bad_ubo", 2, 1);

    value = 11.0F;
    tick();

    EXPECT_TRUE(bindings->has_binding("gain"));
    EXPECT_FALSE(bindings->has_binding("bad_push"));
    EXPECT_FALSE(bindings->has_binding("bad_descriptor"));
    EXPECT_EQ(bindings->get_bound_buffer("gain_ubo"), descriptor_buffer);
    EXPECT_EQ(bindings->get_bound_buffer("bad_ubo"), nullptr);
    EXPECT_FLOAT_EQ(gpu_float(descriptor_buffer), value);
}

TEST_F(FormaBindingsProcessorDeviceTest, UnbindingADescriptorRemovesItsStagedSlotAndBuffer)
{
    float value = 2.0F;
    int calls = 0;
    bindings->bind_descriptor("gain", [&] {
        ++calls;
        return value;
    }, "gain_ubo", 0, 1);
    tick();
    auto descriptor_buffer = bindings->get_bound_buffer("gain_ubo");
    ASSERT_NE(descriptor_buffer, nullptr);
    ASSERT_EQ(calls, 1);
    ASSERT_EQ(buffer->get_pipeline_context().descriptor_buffer_bindings.size(), 1U);
    ASSERT_FLOAT_EQ(gpu_float(descriptor_buffer), 2.0F);

    ASSERT_TRUE(bindings->unbind("gain"));
    value = 9.0F;
    tick();

    EXPECT_EQ(calls, 1);
    EXPECT_FALSE(bindings->has_binding("gain"));
    EXPECT_TRUE(buffer->get_pipeline_context().descriptor_buffer_bindings.empty());
    EXPECT_EQ(bindings->get_bound_buffer("gain_ubo"), nullptr);
}

TEST_F(FormaBindingsProcessorDeviceTest, RebindingADescriptorToAnotherSlotRemovesTheOldSlot)
{
    bindings->bind_descriptor("gain", [] { return 2.0F; }, "gain_ubo", 0, 1);
    tick();
    ASSERT_EQ(buffer->get_pipeline_context().descriptor_buffer_bindings.size(), 1U);

    bindings->bind_descriptor("gain", [] { return 9.0F; }, "gain_ubo", 2, 1);
    tick();

    auto& entries = buffer->get_pipeline_context().descriptor_buffer_bindings;
    ASSERT_EQ(entries.size(), 1U);
    auto rebound = bindings->get_bound_buffer("gain_ubo");
    ASSERT_NE(rebound, nullptr);
    EXPECT_EQ(entries.front().set, 1U);
    EXPECT_EQ(entries.front().binding, 2U);
    EXPECT_EQ(entries.front().buffer_info.buffer, rebound->get_buffer());
    EXPECT_FLOAT_EQ(gpu_float(rebound), 9.0F);
}

}
}
