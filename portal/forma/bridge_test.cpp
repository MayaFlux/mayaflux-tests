#include "../../test_config.h"

#include "../../support/frame_scheduler.hpp"
#include "../../support/vulkan_test_support.hpp"

#include "MayaFlux/Buffers/BufferManager.hpp"
#include "MayaFlux/Buffers/BufferProcessingChain.hpp"
#include "MayaFlux/Buffers/Forma/FormaBuffer.hpp"
#include "MayaFlux/Nodes/Conduit/Constant.hpp"
#include "MayaFlux/Portal/Forma/Bridge.hpp"
#include "MayaFlux/Portal/Forma/Layer.hpp"
#include "MayaFlux/Portal/Graphics/ShaderFoundry.hpp"
#include "MayaFlux/Registry/BackendRegistry.hpp"
#include "MayaFlux/Registry/Service/BufferService.hpp"

namespace MayaFlux::Test {

namespace Forma = Portal::Forma;
using Kinesis::AABB2D;
using Portal::Graphics::PrimitiveTopology;

namespace {

    struct BridgeRig {
        FrameScheduler frames;
        std::shared_ptr<Buffers::BufferManager> buffers = std::make_shared<Buffers::BufferManager>();
        Forma::Bridge bridge { *frames.scheduler, *buffers };
        std::shared_ptr<Forma::Layer> layer = std::make_shared<Forma::Layer>();

        [[nodiscard]] Forma::ElementRef ref(uint32_t id) const { return { layer, id }; }

        template <typename T>
        std::shared_ptr<Forma::MappedState<T>> add(uint32_t id, T initial = {}, std::function<float(T)> project = {})
        {
            auto state = std::make_shared<Forma::MappedState<T>>();
            state->write(std::move(initial));
            bridge.register_element<T>(ref(id), state, std::shared_ptr<Buffers::FormaBuffer> {}, std::move(project));
            return state;
        }
    };

    Forma::GeometryFn<float> sliding_geometry(std::shared_ptr<int> calls)
    {
        return [calls = std::move(calls)](float value, std::vector<uint8_t>& bytes, Forma::Element& element) {
            ++*calls;
            bytes.assign(16, 0);
            element.bounds_hint = AABB2D { .min = { value - 0.1F, -0.1F }, .max = { value + 0.1F, 0.1F } };
        };
    }

    Forma::Mapped<float> add_slider(BridgeRig& rig, Forma::Layer& layer, std::shared_ptr<int> calls, float initial)
    {
        auto buffer = std::make_shared<Buffers::FormaBuffer>(256, PrimitiveTopology::TRIANGLE_STRIP);
        auto mapped = Forma::make_mapped<float>(initial, sliding_geometry(std::move(calls)), std::move(buffer));
        mapped.element.id = layer.add(mapped.element);
        rig.bridge.register_element(layer, mapped);
        return mapped;
    }

}

class FormaBridgeTest : public ::testing::Test {
protected:
    BridgeRig rig;
    Forma::Layer& layer { *rig.layer };
};

// =============================================================================
// Inbound
// =============================================================================

TEST_F(FormaBridgeTest, BindingACallableWritesIntoTheStateEveryFrame)
{
    auto state = rig.add<float>(1, 0.F);
    float source = 0.25F;
    rig.bridge.bind(rig.ref(1), std::function<float()>([&] { return source; }));

    rig.frames.tick();
    EXPECT_FLOAT_EQ(state->value, 0.25F);

    source = 0.5F;
    rig.frames.tick();
    EXPECT_FLOAT_EQ(state->value, 0.5F);
}

TEST_F(FormaBridgeTest, BindingAnIntegerStateCastsTheFloat)
{
    auto state = rig.add<int>(2, 0);
    rig.bridge.bind(rig.ref(2), std::function<float()>([] { return 3.7F; }));

    rig.frames.tick();

    EXPECT_EQ(state->value, 3);
}

TEST_F(FormaBridgeTest, BindingANodeReadsItsLastOutputAndAppliesTheProjection)
{
    auto plain = rig.add<float>(3, 0.F);
    auto projected = rig.add<float>(4, 0.F);
    auto node = std::make_shared<Nodes::Constant>(2.0);
    node->process_sample(0.0);

    rig.bridge.bind(rig.ref(3), node);
    rig.bridge.bind(rig.ref(4), node, [](double value) { return static_cast<float>(value * 2.0); });
    rig.frames.tick();

    EXPECT_FLOAT_EQ(plain->value, 2.0F);
    EXPECT_FLOAT_EQ(projected->value, 4.0F);
}

TEST_F(FormaBridgeTest, ABindingReplacesTheElementsPreviousInboundBinding)
{
    auto state = rig.add<float>(5, 0.F);
    int first_calls = 0;
    rig.bridge.bind(rig.ref(5), std::function<float()>([&] {
        ++first_calls;
        return 1.F;
    }));
    rig.frames.tick();
    ASSERT_EQ(first_calls, 1);

    rig.bridge.bind(rig.ref(5), std::function<float()>([] { return 2.F; }));
    rig.frames.tick(3);

    EXPECT_EQ(first_calls, 1);
    EXPECT_FLOAT_EQ(state->value, 2.F);
}

TEST_F(FormaBridgeTest, UnbindStopsTheInboundBinding)
{
    auto state = rig.add<float>(6, 0.F);
    int calls = 0;
    rig.bridge.bind(rig.ref(6), std::function<float()>([&] {
        ++calls;
        return 1.F;
    }));
    rig.frames.tick();
    ASSERT_EQ(calls, 1);

    rig.bridge.unbind(rig.ref(6));
    rig.frames.tick(3);

    EXPECT_EQ(calls, 1);
}

TEST_F(FormaBridgeTest, UnknownIdsAreIgnoredAcrossBindAndUnbind)
{
    int calls = 0;

    rig.bridge.bind(rig.ref(99), std::function<float()>([&] {
        ++calls;
        return 1.F;
    }));
    rig.bridge.unbind(rig.ref(99));
    rig.frames.tick(2);

    EXPECT_EQ(calls, 0);
}

TEST_F(FormaBridgeTest, AStateWithNoFloatConversionIgnoresInboundWrites)
{
    auto state = rig.add<std::string>(7, "keep");
    rig.bridge.bind(rig.ref(7), std::function<float()>([] { return 1.F; }));

    rig.frames.tick(2);

    EXPECT_EQ(state->value, "keep");
}

// =============================================================================
// Presentation sync
// =============================================================================

TEST_F(FormaBridgeTest, LayerSyncFollowsTheGeometryHitRegionEveryFrame)
{
    auto calls = std::make_shared<int>(0);
    auto mapped = add_slider(rig, layer, calls, 0.F);

    rig.frames.tick(2);
    EXPECT_EQ(layer.hit_test({ 0.F, 0.F }), mapped.element.id);
    EXPECT_FALSE(layer.hit_test({ 0.8F, 0.F }).has_value());

    mapped.state->write(0.8F);
    rig.frames.tick(2);

    EXPECT_EQ(layer.hit_test({ 0.8F, 0.F }), mapped.element.id);
    EXPECT_FALSE(layer.hit_test({ 0.F, 0.F }).has_value());
}

TEST_F(FormaBridgeTest, UnbindStopsThePresentationSync)
{
    auto calls = std::make_shared<int>(0);
    auto mapped = add_slider(rig, layer, calls, 0.F);
    rig.frames.tick(2);
    ASSERT_EQ(*calls, 1);

    rig.bridge.unbind(rig.ref(mapped.element.id));
    mapped.state->write(0.5F);
    rig.frames.tick(3);

    EXPECT_EQ(*calls, 1);
}

TEST_F(FormaBridgeTest, StopSyncCancelsEveryTaskOwnedByTheLayer)
{
    auto calls = std::make_shared<int>(0);
    auto mapped = add_slider(rig, layer, calls, 0.F);
    auto aggregate_state = std::make_shared<Forma::MappedState<float>>();
    int sink_calls = 0;
    rig.bridge.write(layer, { aggregate_state }, [&](std::span<const float>) { ++sink_calls; });
    rig.frames.tick(2);
    ASSERT_EQ(*calls, 1);
    ASSERT_GT(sink_calls, 0);
    const int sink_before = sink_calls;

    rig.bridge.stop_sync(layer);
    mapped.state->write(0.5F);
    rig.frames.tick(3);

    EXPECT_EQ(*calls, 1);
    EXPECT_EQ(sink_calls, sink_before);

    rig.bridge.stop_sync(layer);
}

TEST_F(FormaBridgeTest, StopSyncLeavesOtherLayersRunning)
{
    auto other = std::make_shared<Forma::Layer>();
    auto first_calls = std::make_shared<int>(0);
    auto second_calls = std::make_shared<int>(0);
    auto first = add_slider(rig, layer, first_calls, 0.F);
    auto second = add_slider(rig, *other, second_calls, 0.F);
    rig.frames.tick(2);

    rig.bridge.stop_sync(layer);
    first.state->write(0.5F);
    second.state->write(0.5F);
    rig.frames.tick(2);

    EXPECT_EQ(*first_calls, 1);
    EXPECT_EQ(*second_calls, 2);
}

TEST_F(FormaBridgeTest, StopSyncOnALayerWithNoTasksIsHarmless)
{
    Forma::Layer untouched;

    EXPECT_NO_THROW(rig.bridge.stop_sync(untouched));
}

// =============================================================================
// Outbound routes
// =============================================================================

TEST_F(FormaBridgeTest, ASinkReceivesTheScalarValueAsOneElement)
{
    auto state = rig.add<float>(10, 0.5F);
    std::vector<float> received;
    rig.bridge.write(rig.ref(10), std::function<void(std::span<const float>)>([&](std::span<const float> values) {
        received.assign(values.begin(), values.end());
    }));

    rig.frames.tick();

    EXPECT_EQ(received, std::vector<float> { 0.5F });
}

TEST_F(FormaBridgeTest, ASinkReceivesTheWholeVectorOfAVectorState)
{
    auto floats = rig.add<std::vector<float>>(11, std::vector<float> { 1.F, 2.F, 3.F });
    auto doubles = rig.add<std::vector<double>>(12, std::vector<double> { 4.0, 5.0 });
    std::vector<float> from_floats;
    std::vector<float> from_doubles;
    rig.bridge.write(rig.ref(11), std::function<void(std::span<const float>)>([&](std::span<const float> values) {
        from_floats.assign(values.begin(), values.end());
    }));
    rig.bridge.write(rig.ref(12), std::function<void(std::span<const float>)>([&](std::span<const float> values) {
        from_doubles.assign(values.begin(), values.end());
    }));

    rig.frames.tick();

    EXPECT_EQ(from_floats, (std::vector<float> { 1.F, 2.F, 3.F }));
    EXPECT_EQ(from_doubles, (std::vector<float> { 4.F, 5.F }));
}

TEST_F(FormaBridgeTest, ASinkReceivesTheComponentsOfAGlmVectorState)
{
    auto position = rig.add<glm::vec2>(30, glm::vec2(0.25F, 0.75F));
    auto colour = rig.add<glm::vec3>(31, glm::vec3(0.1F, 0.2F, 0.3F));
    std::vector<float> from_position;
    std::vector<float> from_colour;
    rig.bridge.write(rig.ref(30), std::function<void(std::span<const float>)>([&](std::span<const float> values) {
        from_position.assign(values.begin(), values.end());
    }));
    rig.bridge.write(rig.ref(31), std::function<void(std::span<const float>)>([&](std::span<const float> values) {
        from_colour.assign(values.begin(), values.end());
    }));

    rig.frames.tick();

    EXPECT_EQ(from_position, (std::vector<float> { 0.25F, 0.75F }));
    EXPECT_EQ(from_colour, (std::vector<float> { 0.1F, 0.2F, 0.3F }));
}

TEST_F(FormaBridgeTest, AConstantNodeFollowsTheStateEveryFrame)
{
    auto state = rig.add<float>(13, 0.75F);
    auto node = std::make_shared<Nodes::Constant>(0.0);
    rig.bridge.write(rig.ref(13), node);

    rig.frames.tick();
    EXPECT_DOUBLE_EQ(node->get_constant(), 0.75);

    state->write(-0.5F);
    rig.frames.tick();
    EXPECT_DOUBLE_EQ(node->get_constant(), -0.5);
}

TEST_F(FormaBridgeTest, UnbindStopsOutboundSinksAndConstants)
{
    auto state = rig.add<float>(14, 1.F);
    auto node = std::make_shared<Nodes::Constant>(0.0);
    int sink_calls = 0;
    rig.bridge.write(rig.ref(14), node);
    rig.bridge.write(rig.ref(14), std::function<void(std::span<const float>)>([&](std::span<const float>) { ++sink_calls; }));
    rig.frames.tick();
    ASSERT_EQ(sink_calls, 1);

    rig.bridge.unbind(rig.ref(14));
    state->write(9.F);
    rig.frames.tick(3);

    EXPECT_EQ(sink_calls, 1);
    EXPECT_DOUBLE_EQ(node->get_constant(), 1.0);
}

TEST_F(FormaBridgeTest, AggregateRoutesSampleStatesInTheOrderGiven)
{
    auto a = std::make_shared<Forma::MappedState<float>>();
    auto b = std::make_shared<Forma::MappedState<float>>();
    auto c = std::make_shared<Forma::MappedState<float>>();
    a->write(1.F);
    b->write(2.F);
    c->write(3.F);
    std::vector<float> received;
    rig.bridge.write(layer, { c, a, b }, [&](std::span<const float> values) {
        received.assign(values.begin(), values.end());
    });

    rig.frames.tick();

    EXPECT_EQ(received, (std::vector<float> { 3.F, 1.F, 2.F }));
}

TEST_F(FormaBridgeTest, AnAggregateRouteSurvivesUnbindingAnElement)
{
    auto state = rig.add<float>(15, 1.F);
    int sink_calls = 0;
    rig.bridge.write(layer, { state }, [&](std::span<const float>) { ++sink_calls; });
    rig.frames.tick();
    ASSERT_EQ(sink_calls, 1);

    rig.bridge.unbind(rig.ref(15));
    rig.frames.tick(2);

    EXPECT_EQ(sink_calls, 3);
}

namespace {
    struct RecordingTarget {
        std::vector<std::vector<float>> received;

        void set_data(Kakshya::DataVariant data)
        {
            if (const auto* values = std::get_if<std::vector<float>>(&data))
                received.push_back(*values);
        }
    };
}

TEST_F(FormaBridgeTest, AnAssembledAggregateDeliversTheAuthoredDataVariant)
{
    auto a = std::make_shared<Forma::MappedState<float>>();
    auto b = std::make_shared<Forma::MappedState<float>>();
    a->write(1.F);
    b->write(2.F);
    auto target = std::make_shared<RecordingTarget>();

    rig.bridge.write<float>(layer, { a, b },
        [](std::span<const float> values) {
            std::vector<float> doubled;
            for (float v : values)
                doubled.push_back(v * 2.F);
            return Kakshya::DataVariant { std::move(doubled) };
        },
        target);
    rig.frames.tick();

    ASSERT_EQ(target->received.size(), 1U);
    EXPECT_EQ(target->received.front(), (std::vector<float> { 2.F, 4.F }));

    rig.bridge.stop_sync(layer);
    rig.frames.tick(2);
    EXPECT_EQ(target->received.size(), 1U);
}

TEST_F(FormaBridgeTest, ABindingHandleForwardsEveryOperationToItsElement)
{
    auto state = rig.add<float>(16, 0.F);
    std::vector<float> received;

    auto handle = rig.bridge.at(state);
    EXPECT_EQ(handle.id(), 16U);

    handle.bind(std::function<float()>([] { return 0.5F; }))
        .write(std::function<void(std::span<const float>)>([&](std::span<const float> values) {
            received.assign(values.begin(), values.end());
        }));
    rig.frames.tick(2);

    EXPECT_FLOAT_EQ(state->value, 0.5F);
    EXPECT_EQ(received, std::vector<float> { 0.5F });

    handle.unbind();
    received.clear();
    rig.frames.tick(2);
    EXPECT_TRUE(received.empty());
}

// =============================================================================
// Shader writes (device)
// =============================================================================

class FormaBridgeDeviceTest : public VulkanDeviceTest {
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
    }

    void TearDown() override
    {
        auto* service = Registry::BackendRegistry::instance()
                            .get_service<Registry::Service::BufferService>();
        for (auto& buffer : created) {
            rig.buffers->remove_buffer(buffer, Buffers::ProcessingToken::GRAPHICS_BACKEND);
            buffer->set_default_processor(nullptr);
            if (service && buffer->is_initialized())
                service->destroy_buffer(buffer);
        }
        created.clear();
    }

    std::shared_ptr<Buffers::FormaBuffer> make_buffer()
    {
        auto buffer = std::make_shared<Buffers::FormaBuffer>(256, PrimitiveTopology::POINT_LIST);
        rig.buffers->add_buffer(buffer, Buffers::ProcessingToken::GRAPHICS_BACKEND);
        created.push_back(buffer);
        return buffer;
    }

    static void tick(const std::shared_ptr<Buffers::FormaBuffer>& buffer)
    {
        buffer->get_processing_chain()->process(buffer);
    }

    static std::optional<float> staged_push(const std::shared_ptr<Buffers::FormaBuffer>& buffer, uint32_t offset)
    {
        for (const auto& entry : buffer->get_pipeline_context().push_constant_bindings) {
            if (entry.offset == offset && entry.data.size() == sizeof(float)) {
                float value {};
                std::memcpy(&value, entry.data.data(), sizeof(value));
                return value;
            }
        }
        return std::nullopt;
    }

    template <typename T>
    static std::optional<T> staged_as(const std::shared_ptr<Buffers::FormaBuffer>& buffer, uint32_t offset)
    {
        for (const auto& entry : buffer->get_pipeline_context().push_constant_bindings) {
            if (entry.offset == offset && entry.data.size() == sizeof(T)) {
                T value {};
                std::memcpy(&value, entry.data.data(), sizeof(value));
                return value;
            }
        }
        return std::nullopt;
    }

    BridgeRig rig;
    std::vector<std::shared_ptr<Buffers::FormaBuffer>> created;

private:
    static inline bool s_foundry_ready {};
};

TEST_F(FormaBridgeDeviceTest, APushConstantWriteStagesTheElementValueEveryTick)
{
    auto buffer = make_buffer();
    auto state = rig.add<float>(20, 0.5F);
    rig.bridge.write(rig.ref(20), buffer, "point.vert.spv", 8);

    tick(buffer);
    EXPECT_EQ(staged_push(buffer, 8), 0.5F);

    state->write(0.25F);
    tick(buffer);
    EXPECT_EQ(staged_push(buffer, 8), 0.25F);
}

TEST_F(FormaBridgeDeviceTest, SeveralWritesOfOneElementShareOneBindingsProcessor)
{
    auto buffer = make_buffer();
    auto state = rig.add<float>(21, 0.75F);
    rig.bridge.write(rig.ref(21), buffer, "point.vert.spv", 8);
    rig.bridge.write(rig.ref(21), buffer, "point.vert.spv", 16);

    tick(buffer);

    EXPECT_EQ(staged_push(buffer, 8), 0.75F);
    EXPECT_EQ(staged_push(buffer, 16), 0.75F);
}

TEST_F(FormaBridgeDeviceTest, ADescriptorWriteStagesItsSlotOnTheTargetBuffer)
{
    auto buffer = make_buffer();
    auto state = rig.add<float>(22, 0.5F);
    rig.bridge.write(rig.ref(22), buffer, "point.vert.spv", "gain", 2, 1);

    tick(buffer);

    const auto& entries = buffer->get_pipeline_context().descriptor_buffer_bindings;
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries.front().set, 1U);
    EXPECT_EQ(entries.front().binding, 2U);
    EXPECT_EQ(entries.front().type, vk::DescriptorType::eUniformBuffer);
}

TEST_F(FormaBridgeDeviceTest, AShaderWriteForAnUnknownIdStagesNothing)
{
    auto buffer = make_buffer();

    rig.bridge.write(rig.ref(77), buffer, "point.vert.spv", 0);
    tick(buffer);

    EXPECT_TRUE(buffer->get_pipeline_context().push_constant_bindings.empty());
}

TEST_F(FormaBridgeDeviceTest, ASecondTargetBufferIsNeverAttached)
{
    auto first = make_buffer();
    auto second = make_buffer();
    auto state = rig.add<float>(23, 0.5F);
    rig.bridge.write(rig.ref(23), first, "point.vert.spv", 8);
    rig.bridge.write(rig.ref(23), second, "point.vert.spv", 16);

    tick(first);
    tick(second);

    EXPECT_EQ(staged_push(first, 8), 0.5F);
    EXPECT_EQ(staged_push(first, 16), 0.5F);
    EXPECT_TRUE(second->get_pipeline_context().push_constant_bindings.empty());
}

TEST_F(FormaBridgeDeviceTest, AFormatWriteStagesTheConvertedValueEveryTick)
{
    auto buffer = make_buffer();
    auto state = rig.add<float>(40, 3.9F);
    rig.bridge.write(rig.ref(40), buffer, "point.vert.spv", 8, Kakshya::GpuDataFormat::UINT32);

    tick(buffer);
    EXPECT_EQ(staged_as<uint32_t>(buffer, 8), 3U);

    state->write(5.2F);
    tick(buffer);
    EXPECT_EQ(staged_as<uint32_t>(buffer, 8), 5U);
}

TEST_F(FormaBridgeDeviceTest, AFormatWriteKeepsTheFormatsWidth)
{
    auto buffer = make_buffer();
    auto state = rig.add<float>(41, 2.5F);
    rig.bridge.write(rig.ref(41), buffer, "point.vert.spv", 8, Kakshya::GpuDataFormat::FLOAT64);

    tick(buffer);

    EXPECT_EQ(staged_as<double>(buffer, 8), 2.5);
    EXPECT_FALSE(staged_as<float>(buffer, 8).has_value());
}

TEST_F(FormaBridgeDeviceTest, AFormatWriteSaturatesIntegerFormats)
{
    auto buffer = make_buffer();
    auto state = rig.add<float>(42, -1.0F);
    rig.bridge.write(rig.ref(42), buffer, "point.vert.spv", 8, Kakshya::GpuDataFormat::UINT16);

    tick(buffer);
    EXPECT_EQ(staged_as<uint16_t>(buffer, 8), 0U);

    state->write(100000.0F);
    tick(buffer);
    EXPECT_EQ(staged_as<uint16_t>(buffer, 8), 65535U);
}

TEST_F(FormaBridgeDeviceTest, AMappedStateFormatWriteFollowsTheState)
{
    auto buffer = make_buffer();
    auto state = rig.add<float>(43, 7.0F);
    rig.bridge.write(state, buffer, "point.vert.spv", 12, Kakshya::GpuDataFormat::INT32);

    tick(buffer);

    EXPECT_EQ(staged_as<int32_t>(buffer, 12), 7);
}

TEST_F(FormaBridgeDeviceTest, ABindingHandleForwardsAFormatWrite)
{
    auto buffer = make_buffer();
    auto state = rig.add<float>(44, 9.0F);
    rig.bridge.at(rig.ref(44)).write(buffer, "point.vert.spv", 8, Kakshya::GpuDataFormat::UINT32);

    tick(buffer);

    EXPECT_EQ(staged_as<uint32_t>(buffer, 8), 9U);
}

TEST_F(FormaBridgeDeviceTest, FormatAndWidthWritesOfOneElementShareOneBindingsProcessor)
{
    auto buffer = make_buffer();
    auto state = rig.add<float>(45, 1.5F);
    rig.bridge.write(rig.ref(45), buffer, "point.vert.spv", 8);
    rig.bridge.write(rig.ref(45), buffer, "point.vert.spv", 16, Kakshya::GpuDataFormat::UINT32);

    tick(buffer);

    EXPECT_EQ(staged_as<float>(buffer, 8), 1.5F);
    EXPECT_EQ(staged_as<uint32_t>(buffer, 16), 1U);
}

TEST_F(FormaBridgeDeviceTest, AMultiComponentFormatWriteStagesNothing)
{
    auto buffer = make_buffer();
    auto state = rig.add<float>(46, 1.0F);
    rig.bridge.write(rig.ref(46), buffer, "point.vert.spv", 8, Kakshya::GpuDataFormat::VEC3_F32);

    tick(buffer);

    EXPECT_TRUE(buffer->get_pipeline_context().push_constant_bindings.empty());
}

TEST_F(FormaBridgeDeviceTest, AFormatWriteMatchingAVec3StateKeepsEveryComponent)
{
    auto buffer = make_buffer();
    auto state = rig.add<glm::vec3>(60, glm::vec3 { 1.0F, 2.0F, 3.0F });
    rig.bridge.write(state, buffer, "point.vert.spv", 16, Kakshya::GpuDataFormat::VEC3_F32);

    tick(buffer);
    EXPECT_EQ(staged_as<glm::vec3>(buffer, 16), (glm::vec3 { 1.0F, 2.0F, 3.0F }));

    state->write(glm::vec3 { -4.0F, 5.0F, 6.0F });
    tick(buffer);
    EXPECT_EQ(staged_as<glm::vec3>(buffer, 16), (glm::vec3 { -4.0F, 5.0F, 6.0F }));
}

TEST_F(FormaBridgeDeviceTest, AFormatWriteMatchingAMatrixStateKeepsTheWholeMatrix)
{
    auto buffer = make_buffer();
    glm::mat4 value(1.0F);
    value[3][0] = 7.0F;
    auto state = rig.add<glm::mat4>(61, value);
    rig.bridge.write(rig.ref(61), buffer, "point.vert.spv", 0, Kakshya::GpuDataFormat::MAT4_F32);

    tick(buffer);

    EXPECT_EQ(staged_as<glm::mat4>(buffer, 0), value);
}

TEST_F(FormaBridgeDeviceTest, AFormatWriteMatchingAnIntegerStateIsExactBeyondFloatPrecision)
{
    auto buffer = make_buffer();
    auto state = rig.add<int32_t>(62, 16777217);
    rig.bridge.write(rig.ref(62), buffer, "point.vert.spv", 8, Kakshya::GpuDataFormat::INT32);

    tick(buffer);

    EXPECT_EQ(staged_as<int32_t>(buffer, 8), 16777217);
}

TEST_F(FormaBridgeDeviceTest, AFormatWriteDifferingFromTheStatesTypeStillConvertsItsFloat)
{
    auto buffer = make_buffer();
    auto state = rig.add<int32_t>(63, 7);
    rig.bridge.write(rig.ref(63), buffer, "point.vert.spv", 8, Kakshya::GpuDataFormat::UINT32);

    tick(buffer);

    EXPECT_EQ(staged_as<uint32_t>(buffer, 8), 7U);
}

TEST_F(FormaBridgeDeviceTest, AMultiComponentFormatForAStateOfAnotherTypeStagesNothing)
{
    auto buffer = make_buffer();
    auto state = rig.add<glm::vec3>(64, glm::vec3 { 1.0F });
    rig.bridge.write(rig.ref(64), buffer, "point.vert.spv", 8, Kakshya::GpuDataFormat::VEC4_F32);

    tick(buffer);

    EXPECT_TRUE(buffer->get_pipeline_context().push_constant_bindings.empty());
}

TEST_F(FormaBridgeDeviceTest, UnbindRemovesAStagedTypedWrite)
{
    auto buffer = make_buffer();
    auto state = rig.add<glm::vec3>(65, glm::vec3 { 1.0F, 2.0F, 3.0F });
    rig.bridge.write(state, buffer, "point.vert.spv", 8, Kakshya::GpuDataFormat::VEC3_F32);
    tick(buffer);
    ASSERT_TRUE(staged_as<glm::vec3>(buffer, 8).has_value());

    rig.bridge.unbind(rig.ref(65));
    tick(buffer);

    EXPECT_FALSE(staged_as<glm::vec3>(buffer, 8).has_value());
}

TEST_F(FormaBridgeDeviceTest, AFormatWriteForAnUnknownIdStagesNothing)
{
    auto buffer = make_buffer();
    rig.bridge.write(rig.ref(78), buffer, "point.vert.spv", 0, Kakshya::GpuDataFormat::UINT32);

    tick(buffer);

    EXPECT_TRUE(buffer->get_pipeline_context().push_constant_bindings.empty());
}

TEST_F(FormaBridgeDeviceTest, UnbindRemovesAStagedFormatWrite)
{
    auto buffer = make_buffer();
    auto state = rig.add<float>(47, 4.0F);
    rig.bridge.write(rig.ref(47), buffer, "point.vert.spv", 8, Kakshya::GpuDataFormat::UINT32);
    tick(buffer);
    ASSERT_EQ(staged_as<uint32_t>(buffer, 8), 4U);

    rig.bridge.unbind(rig.ref(47));
    state->write(8.0F);
    tick(buffer);

    EXPECT_FALSE(staged_as<uint32_t>(buffer, 8).has_value());
}

TEST_F(FormaBridgeDeviceTest, UnbindRemovesTheStagedPushConstantAndStopsFollowingTheState)
{
    auto buffer = make_buffer();
    auto state = rig.add<float>(24, 0.5F);
    rig.bridge.write(rig.ref(24), buffer, "point.vert.spv", 8);
    tick(buffer);
    ASSERT_EQ(staged_push(buffer, 8), 0.5F);

    rig.bridge.unbind(rig.ref(24));
    state->write(0.9F);
    tick(buffer);

    EXPECT_FALSE(staged_push(buffer, 8).has_value());
}

}
