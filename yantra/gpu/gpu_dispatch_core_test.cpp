#include "gtest/gtest.h"

#include "../../support/gpu_dispatch_test_support.hpp"

#include "MayaFlux/Core/Backends/Graphics/Vulkan/VKImage.hpp"
#include "MayaFlux/Portal/Graphics/SamplerForge.hpp"
#include "MayaFlux/Portal/Graphics/TextureLoom.hpp"
#include "MayaFlux/Yantra/Executors/GpuDispatchCore.hpp"

namespace MayaFlux::Test {
namespace {

    using namespace GpuDispatch;
    using Kakshya::DataDimension;
    using Portal::Graphics::BufferUsageHint;
    using Portal::Graphics::GpuBufferBinding;
    using Portal::Graphics::INVALID_FENCE;
    using Yantra::ChainedIndirectParams;
    using Yantra::ChainedParams;
    using Yantra::DataStructureInfo;
    using Yantra::DependencyStage;
    using Yantra::ExecutionContext;
    using Yantra::GpuChannelResult;
    using Yantra::GpuComputeConfig;
    using Yantra::GpuDispatchCore;
    using Yantra::GpuDispatchRecorder;
    using Kakshya::GpuDataFormat;
    using Yantra::IndirectGroupsSource;

    constexpr size_t k_words = 16;
    constexpr size_t k_bytes = k_words * sizeof(uint32_t);
    constexpr uint32_t k_image_width = 16;
    constexpr size_t k_row_bytes = static_cast<size_t>(k_image_width) * 4;

    constexpr std::string_view k_count_factor_source = R"(#version 450
layout(local_size_x = 16) in;
layout(set = 0, binding = 0, std430) readonly buffer In { uint in_values[]; };
layout(set = 0, binding = 1, std430) buffer Out { uint out_values[]; };
layout(push_constant) uniform PC { uint count; uint factor; } pc;
void main()
{
    const uint x = gl_GlobalInvocationID.x;
    if (x < pc.count) {
        out_values[x] = in_values[x] * pc.factor;
    }
}
)";

    constexpr std::string_view k_float_scale_source = R"(#version 450
layout(local_size_x = 16) in;
layout(set = 0, binding = 0, std430) readonly buffer In { float in_values[]; };
layout(set = 0, binding = 1, std430) buffer Out { float out_values[]; };
layout(push_constant) uniform PC { uint count; float factor; } pc;
void main()
{
    const uint x = gl_GlobalInvocationID.x;
    if (x < pc.count) {
        out_values[x] = in_values[x] * pc.factor;
    }
}
)";

    constexpr std::string_view k_counts_writer_source = R"(#version 450
layout(local_size_x = 1) in;
layout(set = 0, binding = 3, std430) buffer Counts { uint counts[]; };
void main()
{
    counts[0] = 2;
    counts[1] = 1;
    counts[2] = 1;
}
)";

    constexpr std::string_view k_paint_source = R"(#version 450
layout(local_size_x = 16) in;
layout(set = 0, binding = 0, rgba8) uniform writeonly image2D out_image;
void main()
{
    imageStore(out_image, ivec2(gl_GlobalInvocationID.x, 0), vec4(1.0));
}
)";

    constexpr std::string_view k_declared_unused_image_source = R"(#version 450
layout(local_size_x = 16) in;
layout(set = 0, binding = 0, std430) readonly buffer In { uint in_values[]; };
layout(set = 0, binding = 1, std430) buffer Out { uint out_values[]; };
layout(set = 0, binding = 2, rgba8) uniform writeonly image2D unused_image;
layout(push_constant) uniform PC { uint count; uint factor; } pc;
void main()
{
    const uint x = gl_GlobalInvocationID.x;
    if (x < pc.count) {
        out_values[x] = in_values[x] * pc.factor;
    }
}
)";

    struct FloatPush {
        uint32_t count;
        float factor;
    };

    /**
     * @brief Dispatch core with a declared binding list, exposing the protected entry points and helpers.
     */
    class DispatchCoreProbe : public GpuDispatchCore {
    public:
        DispatchCoreProbe(GpuComputeConfig config, std::vector<GpuBufferBinding> bindings, bool isolated = true)
            : GpuDispatchCore(std::move(config), isolated)
            , m_bindings(std::move(bindings))
        {
        }

        GpuChannelResult run()
        {
            return dispatch_core({}, DataStructureInfo {});
        }

        GpuChannelResult run_with(const std::vector<std::vector<double>>& channels, const DataStructureInfo& info)
        {
            return dispatch_core(channels, info);
        }

        GpuChannelResult run_native(const std::vector<float>& values)
        {
            return run_native_with_channels(values, {}, DataStructureInfo {});
        }

        GpuChannelResult run_native_with_channels(const std::vector<float>& values,
            const std::vector<std::vector<double>>& channels, const DataStructureInfo& info)
        {
            stage_native_bytes(GpuDataFormat::FLOAT32, values.data(), values.size() * sizeof(float));
            return dispatch_core(channels, info);
        }

        GpuChannelResult run_native_raw(GpuDataFormat element, const void* data, size_t byte_size)
        {
            stage_native_bytes(element, data, byte_size);
            return dispatch_core({}, DataStructureInfo {});
        }

        GpuChannelResult run_channels(const std::vector<std::vector<double>>& channels, const DataStructureInfo& info)
        {
            return dispatch_core(channels, info);
        }

        void stage_passthrough_words(size_t index, const std::vector<uint32_t>& words)
        {
            stage_passthrough(index, words.data(), words.size() * sizeof(uint32_t));
        }

        GpuChannelResult run_chained(const ExecutionContext& ctx)
        {
            return dispatch_core_chained({}, DataStructureInfo {}, ctx);
        }

        GpuChannelResult run_chained_indirect(const ExecutionContext& ctx)
        {
            return dispatch_core_chained_indirect({}, DataStructureInfo {}, ctx);
        }

        Portal::Graphics::FenceID run_async()
        {
            return dispatch_core_async({}, DataStructureInfo {});
        }

        std::array<uint32_t, 3> groups_for(size_t total, const DataStructureInfo& info) const
        {
            return calculate_dispatch_size(total, info);
        }

        GpuComputeConfig config() const
        {
            return gpu_config();
        }

    protected:
        std::vector<GpuBufferBinding> declare_buffer_bindings() const override
        {
            return m_bindings;
        }

    private:
        std::vector<GpuBufferBinding> m_bindings;
    };

    template <typename Fixture>
    class GpuDispatchCoreTest : public Fixture {
    };

    using HardwareOptions = ::testing::Types<GpuDispatchFixture<true>, GpuDispatchFixture<false>>;
    TYPED_TEST_SUITE(GpuDispatchCoreTest, HardwareOptions);

    template <bool Dedicated>
    class GpuDispatchCoreImageFixture : public GpuDispatchFixture<Dedicated> {
    protected:
        void SetUp() override
        {
            GpuDispatchFixture<Dedicated>::SetUp();
            ASSERT_TRUE(Portal::Graphics::get_texture_manager().initialize(this->backend()));
            ASSERT_TRUE(Portal::Graphics::SamplerForge::instance().initialize(this->backend()));
        }

        void TearDown() override
        {
            if (this->backend()) {
                Portal::Graphics::SamplerForge::instance().shutdown();
                Portal::Graphics::get_texture_manager().shutdown();
            }
            GpuDispatchFixture<Dedicated>::TearDown();
        }
    };

    template <typename Fixture>
    class GpuDispatchCoreImageTest : public Fixture {
    };

    using ImageHardwareOptions = ::testing::Types<GpuDispatchCoreImageFixture<true>, GpuDispatchCoreImageFixture<false>>;
    TYPED_TEST_SUITE(GpuDispatchCoreImageTest, ImageHardwareOptions);

    constexpr std::string_view k_double_scale_source = R"(#version 450
layout(local_size_x = 16) in;
layout(set = 0, binding = 0, std430) readonly buffer In { double in_values[]; };
layout(set = 0, binding = 1, std430) buffer Out { double out_values[]; };
layout(push_constant) uniform PC { uint count; double factor; } pc;
void main()
{
    const uint x = gl_GlobalInvocationID.x;
    if (x < pc.count) {
        out_values[x] = in_values[x] * pc.factor;
    }
}
)";

    struct DoublePush {
        uint32_t count;
        uint32_t pad;
        double factor;
    };

    bool device_supports_shader_float64()
    {
        return foundry().get_physical_device().getFeatures().shaderFloat64;
    }

    GpuBufferBinding make_binding(uint32_t index, GpuBufferBinding::Direction direction,
        BufferUsageHint usage = BufferUsageHint::COMPUTE_STORAGE,
        GpuBufferBinding::ElementType element_type = GpuBufferBinding::ElementType::UINT32)
    {
        GpuBufferBinding out;
        out.set = 0;
        out.binding = index;
        out.direction = direction;
        out.element_type = element_type;
        out.usage_hint = usage;
        return out;
    }

    GpuBufferBinding make_float_binding(uint32_t index, GpuBufferBinding::Direction direction)
    {
        return make_binding(index, direction, BufferUsageHint::COMPUTE_STORAGE, GpuBufferBinding::ElementType::FLOAT32);
    }

    GpuBufferBinding make_image_binding(uint32_t index)
    {
        GpuBufferBinding out;
        out.set = 0;
        out.binding = index;
        out.direction = GpuBufferBinding::Direction::OUTPUT;
        out.element_type = GpuBufferBinding::ElementType::IMAGE_STORAGE;
        return out;
    }

    /** Input in unit binding 0, output in unit binding 1. */
    std::vector<GpuBufferBinding> unit_bindings()
    {
        return {
            make_binding(0, GpuBufferBinding::Direction::INPUT),
            make_binding(1, GpuBufferBinding::Direction::OUTPUT),
        };
    }

    /** unit_bindings plus a GPU-resident indirect buffer at binding 3. */
    std::vector<GpuBufferBinding> indirect_bindings()
    {
        auto out = unit_bindings();
        out.push_back(make_binding(3, GpuBufferBinding::Direction::INPUT, BufferUsageHint::INDIRECT));
        return out;
    }

    /** Output and indirect buffer held in shared buffers, so they persist across stages. */
    std::vector<GpuBufferBinding> shared_bindings()
    {
        return {
            make_binding(0, GpuBufferBinding::Direction::INPUT),
            make_binding(1, GpuBufferBinding::Direction::INPUT_OUTPUT),
            make_binding(3, GpuBufferBinding::Direction::INPUT, BufferUsageHint::INDIRECT),
        };
    }

    /** Float input and output, plus a uint input that only supplies the workgroup count. */
    std::vector<GpuBufferBinding> float_bindings()
    {
        return {
            make_float_binding(0, GpuBufferBinding::Direction::INPUT),
            make_float_binding(1, GpuBufferBinding::Direction::OUTPUT),
            make_binding(2, GpuBufferBinding::Direction::INPUT),
        };
    }

    /** Float input and output only: native input is the sole input and sets the dispatch size. */
    std::vector<GpuBufferBinding> native_float_bindings()
    {
        return {
            make_float_binding(0, GpuBufferBinding::Direction::INPUT),
            make_float_binding(1, GpuBufferBinding::Direction::OUTPUT),
        };
    }

    /** Double input and output only: native float64 input is the sole input and sets the dispatch size. */
    std::vector<GpuBufferBinding> native_float64_bindings()
    {
        return {
            make_binding(0, GpuBufferBinding::Direction::INPUT, BufferUsageHint::COMPUTE_STORAGE, GpuBufferBinding::ElementType::FLOAT64),
            make_binding(1, GpuBufferBinding::Direction::OUTPUT, BufferUsageHint::COMPUTE_STORAGE, GpuBufferBinding::ElementType::FLOAT64),
        };
    }

    /** Uint input and output only: native uint32 input is the sole input and sets the dispatch size. */
    std::vector<GpuBufferBinding> native_uint_bindings()
    {
        return {
            make_binding(0, GpuBufferBinding::Direction::INPUT),
            make_binding(1, GpuBufferBinding::Direction::OUTPUT),
        };
    }

    /** Passthrough input at 0, uint output at 1, uint input that only supplies the workgroup count at 2. */
    std::vector<GpuBufferBinding> passthrough_bindings()
    {
        return {
            make_binding(0, GpuBufferBinding::Direction::INPUT, BufferUsageHint::COMPUTE_STORAGE,
                GpuBufferBinding::ElementType::PASSTHROUGH),
            make_binding(1, GpuBufferBinding::Direction::OUTPUT),
            make_binding(2, GpuBufferBinding::Direction::INPUT),
        };
    }

    GpuComputeConfig count_factor_config()
    {
        GpuComputeConfig config;
        config.shader_id = load_compute(k_count_factor_source);
        config.workgroup_size = { 16, 1, 1 };
        config.push_constant_size = 2 * sizeof(uint32_t);
        return config;
    }

    GpuComputeConfig float_scale_config()
    {
        GpuComputeConfig config;
        config.shader_id = load_compute(k_float_scale_source);
        config.workgroup_size = { 16, 1, 1 };
        config.push_constant_size = sizeof(FloatPush);
        return config;
    }

    GpuComputeConfig double_scale_config()
    {
        GpuComputeConfig config;
        config.shader_id = load_compute(k_double_scale_source);
        config.workgroup_size = { 16, 1, 1 };
        config.push_constant_size = sizeof(DoublePush);
        return config;
    }

    GpuComputeConfig counts_writer_config()
    {
        GpuComputeConfig config;
        config.shader_id = load_compute(k_counts_writer_source);
        config.workgroup_size = { 1, 1, 1 };
        return config;
    }

    GpuComputeConfig paint_config()
    {
        GpuComputeConfig config;
        config.shader_id = load_compute(k_paint_source);
        config.workgroup_size = { 16, 1, 1 };
        return config;
    }

    GpuComputeConfig declared_unused_config()
    {
        GpuComputeConfig config;
        config.shader_id = load_compute(k_declared_unused_image_source);
        config.workgroup_size = { 16, 1, 1 };
        config.push_constant_size = 2 * sizeof(uint32_t);
        return config;
    }

    DependencyStage input_stage(GpuComputeConfig config, std::vector<uint32_t> input, uint32_t count, uint32_t factor)
    {
        DependencyStage stage;
        stage.config = std::move(config);
        stage.stage_fn = [input = std::move(input), count, factor](GpuDispatchCore& core) {
            core.set_binding_data<uint32_t>(0, input);
            core.set_push_constants(std::array<uint32_t, 2> { count, factor });
        };
        return stage;
    }

    std::vector<uint32_t> as_words(const std::vector<float>& values)
    {
        std::vector<uint32_t> out(values.size());
        std::memcpy(out.data(), values.data(), values.size() * sizeof(float));
        return out;
    }

    std::vector<uint8_t> as_bytes(const std::vector<uint32_t>& words)
    {
        std::vector<uint8_t> out(words.size() * sizeof(uint32_t));
        std::memcpy(out.data(), words.data(), out.size());
        return out;
    }

    std::vector<uint32_t> read_binding(DispatchCoreProbe& core, size_t index, size_t count)
    {
        std::vector<uint32_t> out(count);
        core.download_binding(index, out.data(), count * sizeof(uint32_t));
        return out;
    }

    std::vector<uint32_t> read_shared(DispatchCoreProbe& core, uint32_t set, size_t binding, size_t count)
    {
        std::vector<uint32_t> out(count);
        core.download_shared(set, binding, out.data(), count * sizeof(uint32_t));
        return out;
    }

    void seed_shared(DispatchCoreProbe& core, uint32_t set, size_t binding, const std::vector<uint32_t>& words)
    {
        core.upload_shared_raw(set, binding, reinterpret_cast<const uint8_t*>(words.data()),
            words.size() * sizeof(uint32_t));
    }

    std::vector<float> float_ramp(size_t count, float first = 1.0F)
    {
        std::vector<float> out(count);
        for (size_t i = 0; i < count; ++i) {
            out[i] = first + static_cast<float>(i);
        }
        return out;
    }

    std::vector<float> float_scaled(std::vector<float> values, float factor)
    {
        for (auto& v : values) {
            v *= factor;
        }
        return values;
    }

    std::vector<std::vector<double>> as_channels(const std::vector<float>& values, size_t split)
    {
        std::vector<double> first;
        std::vector<double> second;
        for (size_t i = 0; i < values.size(); ++i) {
            (i < split ? first : second).emplace_back(static_cast<double>(values[i]));
        }
        return { first, second };
    }

    TYPED_TEST(GpuDispatchCoreTest, RunDispatchesTheShaderOverStagedInput)
    {
        DispatchCoreProbe core(count_factor_config(), unit_bindings());
        ASSERT_TRUE(core.ensure_gpu_ready());
        core.set_binding_data<uint32_t>(0, ascending(k_words));
        core.set_output_size(1, k_bytes);
        core.set_push_constants(std::array<uint32_t, 2> { static_cast<uint32_t>(k_words), 3 });

        const auto result = core.run();

        EXPECT_EQ(as_words(result.primary), scaled(ascending(k_words), 3));
    }

    TYPED_TEST(GpuDispatchCoreTest, ChannelsAreFlattenedAcrossTheirWholeLengthAndDispatched)
    {
        DispatchCoreProbe core(float_scale_config(), float_bindings());
        ASSERT_TRUE(core.ensure_gpu_ready());
        core.set_push_constants(FloatPush { .count = static_cast<uint32_t>(k_words), .factor = 2.5F });

        const auto values = float_ramp(k_words);
        const auto result = core.run_with(as_channels(values, 8), DataStructureInfo {});

        EXPECT_EQ(result.primary, float_scaled(values, 2.5F));
    }

    TYPED_TEST(GpuDispatchCoreTest, NativeBytesStagedBeforeDispatchReachTheGpu)
    {
        DispatchCoreProbe core(float_scale_config(), native_float_bindings());
        ASSERT_TRUE(core.ensure_gpu_ready());
        core.set_output_size(1, k_bytes);
        core.set_push_constants(FloatPush { .count = static_cast<uint32_t>(k_words), .factor = 2.0F });

        const auto values = float_ramp(k_words);
        const auto result = core.run_native(values);

        EXPECT_EQ(result.primary, float_scaled(values, 2.0F));
    }

    TYPED_TEST(GpuDispatchCoreTest, NativeBytesTakePrecedenceOverStagedChannels)
    {
        DispatchCoreProbe core(float_scale_config(), native_float_bindings());
        ASSERT_TRUE(core.ensure_gpu_ready());
        core.set_output_size(1, k_bytes);
        core.set_push_constants(FloatPush { .count = static_cast<uint32_t>(k_words), .factor = 2.0F });

        const auto native = float_ramp(k_words);
        const std::vector<std::vector<double>> channels { std::vector<double>(k_words, 100.0) };
        const auto result = core.run_native_with_channels(native, channels, DataStructureInfo {});

        EXPECT_EQ(result.primary, float_scaled(native, 2.0F));
    }

    TYPED_TEST(GpuDispatchCoreTest, NativeUint32InputIsCopiedRawIntoAUint32Binding)
    {
        DispatchCoreProbe core(count_factor_config(), native_uint_bindings());
        ASSERT_TRUE(core.ensure_gpu_ready());
        core.set_output_size(1, k_bytes);
        core.set_push_constants(std::array<uint32_t, 2> { static_cast<uint32_t>(k_words), 3 });

        const auto words = ascending(k_words);
        const auto result = core.run_native_raw(GpuDataFormat::UINT32, words.data(), words.size() * sizeof(uint32_t));

        EXPECT_EQ(as_words(result.primary), scaled(words, 3));
    }

    TYPED_TEST(GpuDispatchCoreTest, NativeInputIsRefusedByABindingOfAnotherType)
    {
        DispatchCoreProbe core(float_scale_config(), native_float_bindings());
        ASSERT_TRUE(core.ensure_gpu_ready());
        core.set_output_size(1, k_bytes);
        core.set_push_constants(FloatPush { .count = static_cast<uint32_t>(k_words), .factor = 2.0F });

        const auto words = ascending(k_words);
        EXPECT_TRUE(core.run_native_raw(GpuDataFormat::UINT32, words.data(), words.size() * sizeof(uint32_t)).primary.empty());

        const std::vector<uint8_t> pixels(k_words, 7);
        EXPECT_TRUE(core.run_native_raw(GpuDataFormat::UINT8, pixels.data(), pixels.size()).primary.empty());

        const auto native = float_ramp(k_words);
        EXPECT_EQ(core.run_native(native).primary, float_scaled(native, 2.0F));
    }

    TYPED_TEST(GpuDispatchCoreTest, NativeFloat64InputIsCopiedRawIntoAFloat64Binding)
    {
        if (!device_supports_shader_float64()) {
            GTEST_SKIP() << "Selected device does not report shaderFloat64";
        }
        DispatchCoreProbe core(double_scale_config(), native_float64_bindings());
        ASSERT_TRUE(core.ensure_gpu_ready());
        core.set_output_size(1, k_words * sizeof(double));
        core.set_push_constants(DoublePush { .count = static_cast<uint32_t>(k_words), .pad = 0, .factor = 2.5 });

        std::vector<double> values(k_words);
        for (size_t i = 0; i < k_words; ++i) {
            values[i] = static_cast<double>(i) + 0.5;
        }
        const auto result = core.run_native_raw(GpuDataFormat::FLOAT64, values.data(), values.size() * sizeof(double));

        std::vector<double> out(result.primary.size() * sizeof(float) / sizeof(double));
        ASSERT_EQ(out.size(), k_words);
        std::memcpy(out.data(), result.primary.data(), out.size() * sizeof(double));

        std::vector<double> expected(values);
        for (auto& v : expected) {
            v *= 2.5;
        }
        EXPECT_EQ(out, expected);
    }

    TYPED_TEST(GpuDispatchCoreTest, NativeFloat64InputIsRefusedByAFloat32Binding)
    {
        DispatchCoreProbe core(float_scale_config(), native_float_bindings());
        ASSERT_TRUE(core.ensure_gpu_ready());
        core.set_output_size(1, k_bytes);
        core.set_push_constants(FloatPush { .count = static_cast<uint32_t>(k_words), .factor = 2.0F });

        const std::vector<double> doubles(k_words, 1.0);
        EXPECT_TRUE(core.run_native_raw(GpuDataFormat::FLOAT64, doubles.data(), doubles.size() * sizeof(double)).primary.empty());

        const auto native = float_ramp(k_words);
        EXPECT_EQ(core.run_native(native).primary, float_scaled(native, 2.0F));
    }

    TYPED_TEST(GpuDispatchCoreTest, NativeFloat32InputIsRefusedByAFloat64Binding)
    {
        if (!device_supports_shader_float64()) {
            GTEST_SKIP() << "Selected device does not report shaderFloat64";
        }
        DispatchCoreProbe core(double_scale_config(), native_float64_bindings());
        ASSERT_TRUE(core.ensure_gpu_ready());
        core.set_output_size(1, k_words * sizeof(double));
        core.set_push_constants(DoublePush { .count = static_cast<uint32_t>(k_words), .pad = 0, .factor = 2.0 });

        const auto native = float_ramp(k_words);
        EXPECT_TRUE(core.run_native(native).primary.empty());
    }

    TYPED_TEST(GpuDispatchCoreTest, NativeInputIsConsumedByItsOwnDispatch)
    {
        DispatchCoreProbe core(float_scale_config(), native_float_bindings());
        ASSERT_TRUE(core.ensure_gpu_ready());
        core.set_output_size(1, k_bytes);
        core.set_push_constants(FloatPush { .count = static_cast<uint32_t>(k_words), .factor = 2.0F });

        const auto native = float_ramp(k_words);
        EXPECT_EQ(core.run_native(native).primary, float_scaled(native, 2.0F));

        const auto channel_values = float_ramp(k_words, 100.0F);
        const auto second = core.run_channels(as_channels(channel_values, 8), DataStructureInfo {});

        EXPECT_EQ(second.primary, float_scaled(channel_values, 2.0F));
    }

    TYPED_TEST(GpuDispatchCoreTest, PassthroughBytesAreUploadedRaw)
    {
        DispatchCoreProbe core(count_factor_config(), passthrough_bindings());
        ASSERT_TRUE(core.ensure_gpu_ready());
        core.stage_passthrough_words(0, ascending(k_words));
        core.set_binding_data<uint32_t>(2, ascending(k_words));
        core.set_output_size(1, k_bytes);
        core.set_push_constants(std::array<uint32_t, 2> { static_cast<uint32_t>(k_words), 3 });

        const auto result = core.run();

        EXPECT_EQ(as_words(result.primary), scaled(ascending(k_words), 3));
    }

    TYPED_TEST(GpuDispatchCoreTest, RecordStagesGivesEachStageItsOwnInput)
    {
        for (const bool recorded : { false, true }) {
            SCOPED_TRACE(recorded ? "recorded" : "immediate");
            auto recording = std::make_shared<GpuDispatchRecorder>(true);
            DispatchCoreProbe core(count_factor_config(), shared_bindings());
            core.set_recording(recorded ? recording : nullptr);
            ASSERT_TRUE(core.ensure_gpu_ready());
            core.ensure_shared_buffer(0, 1, k_words, GpuBufferBinding::ElementType::UINT32);
            core.fill_shared(0, 1, 0);

            const auto config = count_factor_config();
            core.record_stages({
                input_stage(config, ascending(k_words), 8, 2),
                input_stage(config, ascending(k_words, 100), 4, 3),
            });
            if (recorded) {
                recording->wait();
            }

            std::vector<uint32_t> expected(k_words, 0);
            for (uint32_t i = 0; i < 4; ++i) {
                expected[i] = 3 * (100 + i);
            }
            for (uint32_t i = 4; i < 8; ++i) {
                expected[i] = 2 * (1 + i);
            }
            EXPECT_EQ(read_shared(core, 0, 1, k_words), expected);
        }
    }

    TYPED_TEST(GpuDispatchCoreTest, RecordStagesRestoresTheContextsOwnState)
    {
        DispatchCoreProbe core(count_factor_config(), indirect_bindings());
        ASSERT_TRUE(core.ensure_gpu_ready());
        core.ensure_shared_buffer(0, 3, 3, GpuBufferBinding::ElementType::UINT32, BufferUsageHint::INDIRECT);
        core.set_binding_data<uint32_t>(0, ascending(k_words));
        core.set_output_size(1, k_bytes);
        core.set_push_constants(std::array<uint32_t, 2> { static_cast<uint32_t>(k_words), 3 });
        const std::string key_before = core.dispatch_key();
        const auto shader_before = core.config().shader_id;

        DependencyStage writer;
        writer.config = counts_writer_config();
        writer.config.push_constant_size = 2 * sizeof(uint32_t);
        writer.explicit_groups = std::array<uint32_t, 3> { 1, 1, 1 };
        writer.stage_fn = [](GpuDispatchCore& stage_core) {
            stage_core.set_binding_data<uint32_t>(0, filled(k_words, 99));
            stage_core.set_push_constants(std::array<uint32_t, 2> { 1, 7 });
        };
        core.record_stages({ writer });

        EXPECT_EQ(core.dispatch_key(), key_before);
        EXPECT_EQ(core.config().shader_id, shader_before);
        EXPECT_EQ(as_words(core.run().primary), scaled(ascending(k_words), 3));
    }

    TYPED_TEST(GpuDispatchCoreTest, ExplicitGroupsAreUsedInsteadOfTheDataSize)
    {
        DispatchCoreProbe core(count_factor_config(), unit_bindings());
        ASSERT_TRUE(core.ensure_gpu_ready());
        core.set_output_size(1, 2 * k_bytes);

        DependencyStage stage = input_stage(count_factor_config(), ascending(2 * k_words), 2 * k_words, 3);
        stage.explicit_groups = std::array<uint32_t, 3> { 1, 1, 1 };
        core.record_stages({ stage });

        std::vector<uint32_t> expected(2 * k_words, 0);
        for (size_t i = 0; i < k_words; ++i) {
            expected[i] = 3 * static_cast<uint32_t>(i + 1);
        }
        EXPECT_EQ(read_binding(core, 1, 2 * k_words), expected);
    }

    TYPED_TEST(GpuDispatchCoreTest, RecordStagesReadsIndirectCountsWrittenByAnEarlierStage)
    {
        for (const bool recorded : { false, true }) {
            SCOPED_TRACE(recorded ? "recorded" : "immediate");
            auto recording = std::make_shared<GpuDispatchRecorder>(true);
            DispatchCoreProbe core(count_factor_config(), shared_bindings());
            core.set_recording(recorded ? recording : nullptr);
            ASSERT_TRUE(core.ensure_gpu_ready());
            core.ensure_shared_buffer(0, 1, 2 * k_words, GpuBufferBinding::ElementType::UINT32);
            core.ensure_shared_buffer(0, 3, 3, GpuBufferBinding::ElementType::UINT32, BufferUsageHint::INDIRECT);
            core.fill_shared(0, 1, 0);

            DependencyStage writer;
            writer.config = counts_writer_config();
            writer.explicit_groups = std::array<uint32_t, 3> { 1, 1, 1 };
            writer.stage_fn = [](GpuDispatchCore&) { };

            DependencyStage consumer = input_stage(count_factor_config(), ascending(2 * k_words), 2 * k_words, 3);
            consumer.indirect_groups = IndirectGroupsSource { .set = 0, .binding = 3, .offset_bytes = 0 };

            core.record_stages({ writer, consumer });
            if (recorded) {
                recording->wait();
            }

            EXPECT_EQ(read_shared(core, 0, 1, 2 * k_words), scaled(ascending(2 * k_words), 3));
        }
    }

    TYPED_TEST(GpuDispatchCoreTest, GroupsFollowTheSpatialDimensionsOfTheWorkgroup)
    {
        GpuComputeConfig config;
        config.workgroup_size = { 16, 8, 1 };
        DispatchCoreProbe core(config, unit_bindings());

        DataStructureInfo planar;
        planar.dimensions = {
            DataDimension("x", 20, 1, DataDimension::Role::SPATIAL_X),
            DataDimension("y", 10, 1, DataDimension::Role::SPATIAL_Y),
        };
        EXPECT_EQ(core.groups_for(200, planar), (std::array<uint32_t, 3> { 2, 2, 1 }));

        DataStructureInfo volume = planar;
        volume.dimensions.push_back(DataDimension("z", 5, 1, DataDimension::Role::SPATIAL_Z));
        EXPECT_EQ(core.groups_for(1000, volume), (std::array<uint32_t, 3> { 2, 2, 5 }));
    }

    TYPED_TEST(GpuDispatchCoreTest, WithoutSpatialDimensionsTheGroupCountIsOneDimensional)
    {
        GpuComputeConfig config;
        config.workgroup_size = { 16, 1, 1 };
        DispatchCoreProbe core(config, unit_bindings());

        EXPECT_EQ(core.groups_for(100, DataStructureInfo {}), (std::array<uint32_t, 3> { 7, 1, 1 }));
    }

    TYPED_TEST(GpuDispatchCoreTest, ReadbackReturnsSizedAuxOutputsAlongsideThePrimary)
    {
        const std::vector<GpuBufferBinding> bindings {
            make_binding(0, GpuBufferBinding::Direction::INPUT),
            make_binding(1, GpuBufferBinding::Direction::OUTPUT),
            make_binding(2, GpuBufferBinding::Direction::INPUT_OUTPUT),
        };
        DispatchCoreProbe core(count_factor_config(), bindings);
        ASSERT_TRUE(core.ensure_gpu_ready());
        core.set_binding_data<uint32_t>(0, ascending(k_words));
        core.set_binding_data<uint32_t>(2, ascending(k_words, 1000));
        core.set_output_size(1, k_bytes);
        core.set_output_size(2, k_bytes);
        core.set_push_constants(std::array<uint32_t, 2> { static_cast<uint32_t>(k_words), 3 });

        const auto result = core.run();

        EXPECT_EQ(as_words(result.primary), scaled(ascending(k_words), 3));
        ASSERT_TRUE(result.aux.contains(2));
        EXPECT_EQ(result.aux.at(2), as_bytes(ascending(k_words, 1000)));
    }

    TYPED_TEST(GpuDispatchCoreTest, PrimaryReadsTheFirstInoutBindingWhenThereIsNoOutput)
    {
        const std::vector<GpuBufferBinding> bindings {
            make_binding(0, GpuBufferBinding::Direction::INPUT),
            make_binding(1, GpuBufferBinding::Direction::INPUT_OUTPUT),
        };
        DispatchCoreProbe core(count_factor_config(), bindings);
        ASSERT_TRUE(core.ensure_gpu_ready());
        core.set_binding_data<uint32_t>(0, ascending(k_words));
        core.set_binding_data<uint32_t>(1, filled(k_words, 0));
        core.set_push_constants(std::array<uint32_t, 2> { static_cast<uint32_t>(k_words), 3 });

        const auto result = core.run();

        EXPECT_EQ(as_words(result.primary), scaled(ascending(k_words), 3));
    }

    TYPED_TEST(GpuDispatchCoreTest, ChainedPassesReturnNothingWhileRecording)
    {
        for (const bool recorded : { false, true }) {
            SCOPED_TRACE(recorded ? "recorded" : "immediate");
            auto recording = std::make_shared<GpuDispatchRecorder>(true);
            DispatchCoreProbe core(count_factor_config(), unit_bindings());
            core.set_recording(recorded ? recording : nullptr);
            ASSERT_TRUE(core.ensure_gpu_ready());
            core.set_binding_data<uint32_t>(0, ascending(k_words));
            core.set_output_size(1, k_bytes);

            ExecutionContext ctx;
            ctx.parameters = ChainedParams {
                .pass_count = 2,
                .pc_updater = [](uint32_t pass, void* out) {
                    const std::array<uint32_t, 2> push { static_cast<uint32_t>(k_words), pass + 2 };
                    std::memcpy(out, push.data(), sizeof(push));
                },
                .passes_per_batch = std::nullopt,
            };

            const auto result = core.run_chained(ctx);
            if (recorded) {
                EXPECT_TRUE(result.primary.empty());
                EXPECT_TRUE(recording->is_open());
                recording->wait();
            } else {
                EXPECT_EQ(as_words(result.primary), scaled(ascending(k_words), 3));
            }

            EXPECT_EQ(read_binding(core, 1, k_words), scaled(ascending(k_words), 3));
        }
    }

    TYPED_TEST(GpuDispatchCoreTest, ChainedIndirectPassesRunOverTheSeededGroups)
    {
        for (const bool recorded : { false, true }) {
            SCOPED_TRACE(recorded ? "recorded" : "immediate");
            auto recording = std::make_shared<GpuDispatchRecorder>(true);
            DispatchCoreProbe core(count_factor_config(), indirect_bindings());
            core.set_recording(recorded ? recording : nullptr);
            ASSERT_TRUE(core.ensure_gpu_ready());
            core.ensure_shared_buffer(0, 3, 3, GpuBufferBinding::ElementType::UINT32, BufferUsageHint::INDIRECT);
            seed_shared(core, 0, 3, { 1, 1, 1 });
            core.set_binding_data<uint32_t>(0, ascending(2 * k_words));
            core.set_output_size(1, 2 * k_bytes);

            ExecutionContext ctx;
            ctx.parameters = ChainedIndirectParams {
                .pass_count = 2,
                .pc_updater = [](uint32_t pass, uint32_t, void* out) {
                    const std::array<uint32_t, 2> push { static_cast<uint32_t>(k_words), pass + 2 };
                    std::memcpy(out, push.data(), sizeof(push));
                },
                .passes_per_batch = std::nullopt,
            };

            const auto result = core.run_chained_indirect(ctx);

            std::vector<uint32_t> expected(2 * k_words, 0);
            for (size_t i = 0; i < k_words; ++i) {
                expected[i] = 3 * static_cast<uint32_t>(i + 1);
            }
            if (recorded) {
                EXPECT_TRUE(result.primary.empty());
                EXPECT_TRUE(recording->is_open());
                recording->wait();
            } else {
                EXPECT_EQ(as_words(result.primary), expected);
            }

            EXPECT_EQ(read_binding(core, 1, 2 * k_words), expected);
        }
    }

    TYPED_TEST(GpuDispatchCoreTest, ChainedIndirectUploadsInputsToAFreshCore)
    {
        DispatchCoreProbe core(count_factor_config(), indirect_bindings());
        core.ensure_shared_buffer(0, 3, 3, GpuBufferBinding::ElementType::UINT32, BufferUsageHint::INDIRECT);
        seed_shared(core, 0, 3, { 1, 1, 1 });
        core.set_binding_data<uint32_t>(0, ascending(k_words));
        core.set_output_size(1, k_bytes);

        ExecutionContext ctx;
        ctx.parameters = ChainedIndirectParams {
            .pass_count = 1,
            .pc_updater = [](uint32_t, uint32_t, void* out) {
                const std::array<uint32_t, 2> push { static_cast<uint32_t>(k_words), 2 };
                std::memcpy(out, push.data(), sizeof(push));
            },
            .passes_per_batch = std::nullopt,
        };

        const auto result = core.run_chained_indirect(ctx);

        EXPECT_EQ(as_words(result.primary), scaled(ascending(k_words), 2));
    }

    TYPED_TEST(GpuDispatchCoreTest, AsyncDispatchReturnsAFenceOnlyWithoutARecording)
    {
        for (const bool recorded : { false, true }) {
            SCOPED_TRACE(recorded ? "recorded" : "immediate");
            auto recording = std::make_shared<GpuDispatchRecorder>(true);
            DispatchCoreProbe core(count_factor_config(), unit_bindings());
            core.set_recording(recorded ? recording : nullptr);
            ASSERT_TRUE(core.ensure_gpu_ready());
            core.set_binding_data<uint32_t>(0, ascending(k_words));
            core.set_output_size(1, k_bytes);
            core.set_push_constants(std::array<uint32_t, 2> { static_cast<uint32_t>(k_words), 3 });

            const auto fence = core.run_async();
            if (recorded) {
                EXPECT_EQ(fence, INVALID_FENCE);
                recording->wait();
            } else {
                ASSERT_NE(fence, INVALID_FENCE);
                foundry().wait_for_fence(fence);
                EXPECT_TRUE(foundry().is_fence_signaled(fence));
                foundry().release_fence(fence);
            }

            EXPECT_EQ(read_binding(core, 1, k_words), scaled(ascending(k_words), 3));
        }
    }

    TYPED_TEST(GpuDispatchCoreTest, DispatchOnAContextThatWasNeverMadeReadyThrows)
    {
        DispatchCoreProbe core(count_factor_config(), unit_bindings());

        EXPECT_FALSE(core.is_gpu_ready());
        EXPECT_THROW(core.run(), std::runtime_error);
    }

    TYPED_TEST(GpuDispatchCoreTest, DeclaredImageThatTheKernelNeverReadsIsNotBound)
    {
        const std::vector<GpuBufferBinding> bindings {
            make_binding(0, GpuBufferBinding::Direction::INPUT),
            make_binding(1, GpuBufferBinding::Direction::OUTPUT),
            GpuBufferBinding { .set = 0,
                .binding = 2,
                .direction = GpuBufferBinding::Direction::INPUT,
                .element_type = GpuBufferBinding::ElementType::IMAGE_STORAGE },
        };
        DispatchCoreProbe core(declared_unused_config(), bindings, false);
        ASSERT_TRUE(core.ensure_gpu_ready());
        core.set_binding_data<uint32_t>(0, ascending(k_words));
        core.set_output_size(1, k_bytes);
        core.set_push_constants(std::array<uint32_t, 2> { static_cast<uint32_t>(k_words), 3 });

        GpuChannelResult result;
        EXPECT_NO_THROW(result = core.run());

        EXPECT_EQ(as_words(result.primary), scaled(ascending(k_words), 3));
    }

    TYPED_TEST(GpuDispatchCoreImageTest, ImageOutputIsStagedBoundAndWrittenByTheKernel)
    {
        const std::vector<GpuBufferBinding> bindings {
            make_image_binding(0),
            make_binding(1, GpuBufferBinding::Direction::INPUT),
        };
        DispatchCoreProbe core(paint_config(), bindings, false);
        ASSERT_TRUE(core.ensure_gpu_ready());
        auto image = Portal::Graphics::get_texture_manager().create_storage_image(k_image_width, 1);
        ASSERT_NE(image, nullptr);
        core.stage_image_at(0, image, GpuBufferBinding::ElementType::IMAGE_STORAGE);
        core.set_binding_data<uint32_t>(1, ascending(k_image_width));

        const auto result = core.run();

        EXPECT_TRUE(result.primary.empty());
        EXPECT_EQ(core.get_output_image(0), image);
        EXPECT_EQ(image->get_current_layout(), vk::ImageLayout::eGeneral);
        std::vector<uint8_t> pixels(k_row_bytes, 0);
        Portal::Graphics::get_texture_manager().download_data(image, pixels.data(), pixels.size());
        EXPECT_EQ(pixels, std::vector<uint8_t>(k_row_bytes, 255));
    }

    TYPED_TEST(GpuDispatchCoreImageTest, ImageBindingWithoutAStagedImageIsSkipped)
    {
        const std::vector<GpuBufferBinding> bindings {
            make_image_binding(0),
            make_binding(1, GpuBufferBinding::Direction::INPUT),
        };
        DispatchCoreProbe core(paint_config(), bindings, false);
        ASSERT_TRUE(core.ensure_gpu_ready());
        core.set_binding_data<uint32_t>(1, ascending(k_image_width));

        GpuChannelResult result;
        EXPECT_NO_THROW(result = core.run());

        EXPECT_TRUE(result.primary.empty());
        EXPECT_EQ(core.get_output_image(0), nullptr);
    }

} // namespace
} // namespace MayaFlux::Test
