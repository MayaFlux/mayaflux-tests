#include "gtest/gtest.h"

#include "../../support/gpu_dispatch_test_support.hpp"

#include "MayaFlux/Core/Backends/Graphics/Vulkan/VKImage.hpp"
#include "MayaFlux/Kakshya/Source/TextureContainer.hpp"
#include "MayaFlux/Portal/Graphics/SamplerForge.hpp"
#include "MayaFlux/Portal/Graphics/TextureLoom.hpp"
#include "MayaFlux/Yantra/Executors/ShaderExecutionContext.hpp"
#include "MayaFlux/Yantra/Executors/TextureExecutionContext.hpp"

namespace MayaFlux::Test {
namespace {

    using namespace GpuDispatch;
    using Portal::Graphics::FenceID;
    using Portal::Graphics::GpuBufferBinding;
    using Portal::Graphics::ImageFormat;
    using Portal::Graphics::INVALID_FENCE;
    using Yantra::ChainedIndirectParams;
    using Yantra::Datum;
    using Yantra::ExecutionMode;
    using Yantra::ExecutionContext;
    using Yantra::GpuComputeConfig;
    using Yantra::GpuDispatchRecorder;
    using Yantra::ShaderExecutionContext;
    using Yantra::TextureExecutionContext;

    constexpr size_t k_count = 16;
    constexpr uint32_t k_factor = 3;
    constexpr uint32_t k_w = 16;
    constexpr uint32_t k_h = 8;
    constexpr uint32_t k_channels = 4;

    constexpr std::string_view k_scale_words_source = R"(#version 450
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

    constexpr std::string_view k_add_words_source = R"(#version 450
layout(local_size_x = 16) in;
layout(set = 0, binding = 0, std430) buffer Data { uint values[]; };
layout(push_constant) uniform PC { uint count; uint add; } pc;
void main()
{
    const uint x = gl_GlobalInvocationID.x;
    if (x < pc.count) {
        values[x] += pc.add;
    }
}
)";

    struct ScalePush {
        uint32_t count;
        uint32_t factor;
    };

    struct AddPush {
        uint32_t count;
        uint32_t add;
    };

    struct ImagePush {
        uint32_t width;
        uint32_t height;
        float scale;
    };

    struct CountPush {
        uint32_t count;
    };

    struct LayerPush {
        uint32_t width;
        uint32_t height;
        float scale;
        uint32_t layer;
    };

    struct FloatPush {
        uint32_t count;
        float factor;
    };

    constexpr std::string_view k_scale_layer_source = R"(#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(set = 0, binding = 0, rgba32f) uniform writeonly image2D out_image;
layout(set = 0, binding = 1) uniform sampler2DArray in_image;
layout(push_constant) uniform PC { uint width; uint height; float scale; uint layer; } pc;
void main()
{
    const ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (p.x >= int(pc.width) || p.y >= int(pc.height)) {
        return;
    }
    imageStore(out_image, p, texelFetch(in_image, ivec3(p, int(pc.layer)), 0) * pc.scale);
}
)";

    constexpr std::string_view k_copy_rgba8_source = R"(#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(set = 0, binding = 0, rgba8) uniform writeonly image2D out_image;
layout(set = 0, binding = 1) uniform sampler2D in_image;
layout(push_constant) uniform PC { uint width; uint height; float scale; } pc;
void main()
{
    const ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (p.x >= int(pc.width) || p.y >= int(pc.height)) {
        return;
    }
    imageStore(out_image, p, texelFetch(in_image, p, 0));
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

    constexpr std::string_view k_scale_image_source = R"(#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(set = 0, binding = 0, rgba32f) uniform writeonly image2D out_image;
layout(set = 0, binding = 1) uniform sampler2D in_image;
layout(push_constant) uniform PC { uint width; uint height; float scale; } pc;
void main()
{
    const ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (p.x >= int(pc.width) || p.y >= int(pc.height)) {
        return;
    }
    imageStore(out_image, p, texelFetch(in_image, p, 0) * pc.scale);
}
)";

    constexpr std::string_view k_decimate_image_source = R"(#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(set = 0, binding = 0, rgba32f) uniform writeonly image2D out_image;
layout(set = 0, binding = 1) uniform sampler2D in_image;
layout(push_constant) uniform PC { uint width; uint height; float scale; } pc;
void main()
{
    const ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (p.x >= int(pc.width) || p.y >= int(pc.height)) {
        return;
    }
    imageStore(out_image, p, texelFetch(in_image, p * 2, 0));
}
)";

    constexpr std::string_view k_row_reduce_source = R"(#version 450
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
layout(set = 0, binding = 1) uniform sampler2D in_image;
layout(set = 0, binding = 6, std430) buffer Out { float out_values[]; };
layout(push_constant) uniform PC { uint count; } pc;
void main()
{
    const uint x = gl_GlobalInvocationID.x;
    if (x >= pc.count) {
        return;
    }
    out_values[x] = texelFetch(in_image, ivec2(int(x), 0), 0).r;
}
)";

    std::vector<uint32_t> words_from(const std::vector<uint8_t>& raw)
    {
        std::vector<uint32_t> out(raw.size() / sizeof(uint32_t));
        std::memcpy(out.data(), raw.data(), out.size() * sizeof(uint32_t));
        return out;
    }

    std::vector<float> floats_from(const std::vector<uint8_t>& raw)
    {
        std::vector<float> out(raw.size() / sizeof(float));
        std::memcpy(out.data(), raw.data(), out.size() * sizeof(float));
        return out;
    }

    std::vector<float> ramp_pixels(uint32_t w, uint32_t h)
    {
        std::vector<float> px(static_cast<size_t>(w) * h * k_channels);
        for (size_t i = 0; i < px.size(); ++i) {
            px[i] = static_cast<float>(i) * 0.5F;
        }
        return px;
    }

    std::vector<float> red_values(const std::vector<float>& px, uint32_t count)
    {
        std::vector<float> out(count);
        for (size_t x = 0; x < count; ++x) {
            out[x] = px[x * k_channels];
        }
        return out;
    }

    std::vector<float> scaled_floats(std::vector<float> values, float factor)
    {
        for (auto& v : values) {
            v *= factor;
        }
        return values;
    }

    GpuComputeConfig shader_config(Portal::Graphics::ShaderID shader, size_t push_bytes, std::array<uint32_t, 3> workgroup)
    {
        GpuComputeConfig config = compute_config(shader, push_bytes);
        config.workgroup_size = workgroup;
        return config;
    }

    std::shared_ptr<Core::VKImage> float_image(uint32_t w, uint32_t h, const std::vector<float>& px)
    {
        return Portal::Graphics::get_texture_manager().create_2d(w, h, ImageFormat::RGBA32F, px.data());
    }

    std::vector<float> read_floats(const std::shared_ptr<Core::VKImage>& image, size_t count)
    {
        std::vector<float> out(count);
        Portal::Graphics::get_texture_manager().download_data(image, out.data(), count * sizeof(float));
        return out;
    }

    void wait_and_release(FenceID fence)
    {
        ASSERT_NE(fence, INVALID_FENCE);
        foundry().wait_for_fence(fence);
        foundry().release_fence(fence);
    }

    GpuBufferBinding binding_of(uint32_t binding, GpuBufferBinding::Direction direction,
        GpuBufferBinding::ElementType type,
        Portal::Graphics::BufferUsageHint hint = Portal::Graphics::BufferUsageHint::COMPUTE_STORAGE)
    {
        GpuBufferBinding out;
        out.set = 0;
        out.binding = binding;
        out.direction = direction;
        out.element_type = type;
        out.usage_hint = hint;
        return out;
    }

    std::shared_ptr<Core::VKImage> byte_image(uint32_t w, uint32_t h, const std::vector<uint8_t>& bytes)
    {
        return Portal::Graphics::get_texture_manager().create_2d(w, h, ImageFormat::RGBA8, bytes.data());
    }

    std::vector<uint8_t> read_bytes(const std::shared_ptr<Core::VKImage>& image, size_t count)
    {
        std::vector<uint8_t> out(count);
        Portal::Graphics::get_texture_manager().download_data(image, out.data(), count);
        return out;
    }

    std::vector<float> decimated_values(const std::vector<float>& px, uint32_t out_w, uint32_t out_h)
    {
        std::vector<float> expected;
        expected.reserve(size_t { out_w } * out_h * k_channels);
        for (size_t y = 0; y < out_h; ++y) {
            for (size_t x = 0; x < out_w; ++x) {
                for (size_t c = 0; c < k_channels; ++c) {
                    expected.push_back(px[((2 * y) * k_w + 2 * x) * k_channels + c]);
                }
            }
        }
        return expected;
    }

    std::vector<float> container_floats(const Kakshya::TextureContainer& container, uint32_t layer)
    {
        const auto px = container.as_float(layer);
        return { px.begin(), px.end() };
    }

    template <typename Fixture>
    class ShaderExecutionContextGpuTest : public Fixture { };

    using HardwareOptions = ::testing::Types<GpuDispatchFixture<true>, GpuDispatchFixture<false>>;
    TYPED_TEST_SUITE(ShaderExecutionContextGpuTest, HardwareOptions);

    TYPED_TEST(ShaderExecutionContextGpuTest, FluentBindingsDispatchAndReadBackThroughReadOutput)
    {
        const auto shader = load_compute(k_scale_words_source);
        ASSERT_NE(shader, Portal::Graphics::INVALID_SHADER);

        const auto input = ascending(k_count);
        auto executor = std::make_shared<ShaderExecutionContext<>>(
            shader_config(shader, sizeof(ScalePush), { 16, 1, 1 }));
        executor->input(0, input, GpuBufferBinding::ElementType::UINT32)
            .output(1, k_count * sizeof(uint32_t), GpuBufferBinding::ElementType::UINT32)
            .push(ScalePush { .count = static_cast<uint32_t>(k_count), .factor = k_factor });

        const auto result = executor->execute(Datum<std::vector<Kakshya::DataVariant>> {}, ExecutionContext {});

        EXPECT_EQ(ShaderExecutionContext<>::read_output<uint32_t>(result, 1), scaled(input, k_factor));
    }

    TYPED_TEST(ShaderExecutionContextGpuTest, InferredBindingIndicesFollowDeclarationOrder)
    {
        const auto shader = load_compute(k_scale_words_source);
        ASSERT_NE(shader, Portal::Graphics::INVALID_SHADER);

        const auto input = ascending(k_count);
        auto executor = std::make_shared<ShaderExecutionContext<>>(
            shader_config(shader, sizeof(ScalePush), { 16, 1, 1 }));
        executor->input(input, GpuBufferBinding::ElementType::UINT32)
            .output(k_count * sizeof(uint32_t), GpuBufferBinding::ElementType::UINT32)
            .push(ScalePush { .count = static_cast<uint32_t>(k_count), .factor = k_factor });

        const auto result = executor->execute(Datum<std::vector<Kakshya::DataVariant>> {}, ExecutionContext {});

        EXPECT_EQ(ShaderExecutionContext<>::read_output<uint32_t>(result, 1), scaled(input, k_factor));
    }

    TYPED_TEST(ShaderExecutionContextGpuTest, SkipReadbackRemovesTheOutputFromTheResult)
    {
        const auto shader = load_compute(k_scale_words_source);
        ASSERT_NE(shader, Portal::Graphics::INVALID_SHADER);

        auto executor = std::make_shared<ShaderExecutionContext<>>(
            shader_config(shader, sizeof(ScalePush), { 16, 1, 1 }));
        executor->input(0, ascending(k_count), GpuBufferBinding::ElementType::UINT32)
            .output(1, k_count * sizeof(uint32_t), GpuBufferBinding::ElementType::UINT32)
            .push(ScalePush { .count = static_cast<uint32_t>(k_count), .factor = k_factor });
        executor->set_skip_readback(1, true);

        const auto result = executor->execute(Datum<std::vector<Kakshya::DataVariant>> {}, ExecutionContext {});

        EXPECT_THROW(ShaderExecutionContext<>::read_output<uint32_t>(result, 1), std::runtime_error);
    }

    TYPED_TEST(ShaderExecutionContextGpuTest, MultipassUpdaterSeesEveryPassIndexAndEveryPassApplies)
    {
        const auto shader = load_compute(k_add_words_source);
        ASSERT_NE(shader, Portal::Graphics::INVALID_SHADER);

        const auto input = ascending(k_count);
        auto executor = std::make_shared<ShaderExecutionContext<>>(
            shader_config(shader, sizeof(AddPush), { 16, 1, 1 }));
        executor->in_out(0, input, GpuBufferBinding::ElementType::UINT32);
        executor->set_output_size(0, k_count * sizeof(uint32_t));
        executor->set_multipass(3, [](uint32_t pass, void* raw) {
            auto* push = static_cast<AddPush*>(raw);
            push->count = static_cast<uint32_t>(k_count);
            push->add = pass + 1;
        });

        const auto result = executor->execute(Datum<std::vector<Kakshya::DataVariant>> {}, ExecutionContext {});

        EXPECT_EQ(ShaderExecutionContext<>::read_output<uint32_t>(result, 0), summed(input, filled(k_count, 6)));
    }

    TYPED_TEST(ShaderExecutionContextGpuTest, DispatchAsyncThenCollectResultMatchesExecute)
    {
        const auto shader = load_compute(k_scale_words_source);
        ASSERT_NE(shader, Portal::Graphics::INVALID_SHADER);

        const auto input = ascending(k_count);
        auto executor = std::make_shared<ShaderExecutionContext<>>(
            shader_config(shader, sizeof(ScalePush), { 16, 1, 1 }));
        executor->input(0, input, GpuBufferBinding::ElementType::UINT32)
            .output(1, k_count * sizeof(uint32_t), GpuBufferBinding::ElementType::UINT32)
            .push(ScalePush { .count = static_cast<uint32_t>(k_count), .factor = k_factor });

        const auto fence = executor->dispatch_async(Datum<std::vector<Kakshya::DataVariant>> {});
        wait_and_release(fence);
        const auto collected = executor->collect_result();

        const auto it = collected.aux.find(1);
        ASSERT_NE(it, collected.aux.end());
        EXPECT_EQ(words_from(it->second), scaled(input, k_factor));
    }

    TYPED_TEST(ShaderExecutionContextGpuTest, RecordedDispatchIsReadBackOnCollect)
    {
        const auto shader = load_compute(k_scale_words_source);
        ASSERT_NE(shader, Portal::Graphics::INVALID_SHADER);

        const auto input = ascending(k_count);
        auto recording = std::make_shared<GpuDispatchRecorder>(true);
        auto executor = std::make_shared<ShaderExecutionContext<>>(
            shader_config(shader, sizeof(ScalePush), { 16, 1, 1 }));
        executor->set_recording(recording);
        executor->input(0, input, GpuBufferBinding::ElementType::UINT32)
            .output(1, k_count * sizeof(uint32_t), GpuBufferBinding::ElementType::UINT32)
            .push(ScalePush { .count = static_cast<uint32_t>(k_count), .factor = k_factor });

        const auto fence = executor->dispatch_async(Datum<std::vector<Kakshya::DataVariant>> {});
        EXPECT_EQ(fence, INVALID_FENCE);
        const auto collected = executor->collect_result();

        const auto it = collected.aux.find(1);
        ASSERT_NE(it, collected.aux.end());
        EXPECT_EQ(words_from(it->second), scaled(input, k_factor));
    }

    TYPED_TEST(ShaderExecutionContextGpuTest, RebindingInputBetweenExecutesSeesTheNewInput)
    {
        const auto shader = load_compute(k_scale_words_source);
        ASSERT_NE(shader, Portal::Graphics::INVALID_SHADER);

        const auto first = ascending(k_count);
        const auto second = filled(k_count, 7);
        auto executor = std::make_shared<ShaderExecutionContext<>>(
            shader_config(shader, sizeof(ScalePush), { 16, 1, 1 }));
        executor->input(0, first, GpuBufferBinding::ElementType::UINT32)
            .output(1, k_count * sizeof(uint32_t), GpuBufferBinding::ElementType::UINT32)
            .push(ScalePush { .count = static_cast<uint32_t>(k_count), .factor = k_factor });

        const auto one = executor->execute(Datum<std::vector<Kakshya::DataVariant>> {}, ExecutionContext {});
        EXPECT_EQ(ShaderExecutionContext<>::read_output<uint32_t>(one, 1), scaled(first, k_factor));

        executor->set_binding_data(0, second);
        const auto two = executor->execute(Datum<std::vector<Kakshya::DataVariant>> {}, ExecutionContext {});
        EXPECT_EQ(ShaderExecutionContext<>::read_output<uint32_t>(two, 1), scaled(second, k_factor));
    }

    TYPED_TEST(ShaderExecutionContextGpuTest, NativeUint32DatumReachesTheUint32Binding)
    {
        const auto shader = load_compute(k_scale_words_source);
        ASSERT_NE(shader, Portal::Graphics::INVALID_SHADER);

        const auto words = ascending(k_count);
        Datum<std::vector<Kakshya::DataVariant>> input {};
        input.data.emplace_back(words);

        auto executor = std::make_shared<ShaderExecutionContext<>>(
            shader_config(shader, sizeof(ScalePush), { 16, 1, 1 }));
        executor->in_out(0, GpuBufferBinding::ElementType::UINT32)
            .output(1, k_count * sizeof(uint32_t), GpuBufferBinding::ElementType::UINT32)
            .push(ScalePush { .count = static_cast<uint32_t>(k_count), .factor = k_factor });

        const auto result = executor->execute(input, ExecutionContext {});

        EXPECT_EQ(ShaderExecutionContext<>::read_output<uint32_t>(result, 1), scaled(words, k_factor));
    }

    TYPED_TEST(ShaderExecutionContextGpuTest, NativeFloatIntoUint32OnlyBindingIsSkippedWithoutThrowing)
    {
        const auto shader = load_compute(k_scale_words_source);
        ASSERT_NE(shader, Portal::Graphics::INVALID_SHADER);

        Datum<std::vector<Kakshya::DataVariant>> input {};
        input.data.emplace_back(std::vector<float>(k_count, 1.0F));

        auto executor = std::make_shared<ShaderExecutionContext<>>(
            shader_config(shader, sizeof(ScalePush), { 16, 1, 1 }));
        executor->in_out(0, GpuBufferBinding::ElementType::UINT32)
            .output(1, k_count * sizeof(uint32_t), GpuBufferBinding::ElementType::UINT32)
            .push(ScalePush { .count = static_cast<uint32_t>(k_count), .factor = k_factor });

        Datum<std::vector<Kakshya::DataVariant>> result {};
        EXPECT_NO_THROW(result = executor->execute(input, ExecutionContext {}));
        EXPECT_FALSE(result.metadata.contains("gpu_output_1"));
    }


    template <bool Dedicated>
    class GpuImageContextFixture : public GpuDispatchFixture<Dedicated> {
    protected:
        void SetUp() override
        {
            GpuDispatchFixture<Dedicated>::SetUp();
            if (this->IsSkipped() || this->HasFatalFailure()) {
                return;
            }
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
    class TextureExecutionContextGpuTest : public Fixture { };

    using ImageHardwareOptions = ::testing::Types<GpuImageContextFixture<true>, GpuImageContextFixture<false>>;
    TYPED_TEST_SUITE(TextureExecutionContextGpuTest, ImageHardwareOptions);

    TYPED_TEST(TextureExecutionContextGpuTest, ImageModeScalesInputIntoTheOutputImage)
    {
        const auto shader = load_compute(k_scale_image_source);
        ASSERT_NE(shader, Portal::Graphics::INVALID_SHADER);

        const auto px = ramp_pixels(k_w, k_h);
        const auto input = float_image(k_w, k_h, px);
        ASSERT_NE(input, nullptr);

        auto ctx = std::make_shared<TextureExecutionContext>(
            shader_config(shader, sizeof(ImagePush), { 8, 8, 1 }),
            ImageFormat::RGBA32F,
            TextureExecutionContext::OutputMode::IMAGE);
        ctx->stage_image(input);
        ctx->set_push_constants(ImagePush { .width = k_w, .height = k_h, .scale = 2.0F });
        ctx->prepare_output_image(k_w, k_h);
        ctx->set_output_dimensions(k_w, k_h);
        const auto fence = ctx->dispatch_async({});
        ctx->clear_output_dimensions();
        wait_and_release(fence);

        const auto output = ctx->get_output_image(0);
        ASSERT_NE(output, nullptr);
        EXPECT_EQ(output->get_width(), k_w);
        EXPECT_EQ(output->get_height(), k_h);
        EXPECT_EQ(read_floats(output, px.size()), scaled_floats(px, 2.0F));
    }

    TYPED_TEST(TextureExecutionContextGpuTest, OutputDimensionsSetTheSizeAndTheDispatchGrid)
    {
        const auto shader = load_compute(k_decimate_image_source);
        ASSERT_NE(shader, Portal::Graphics::INVALID_SHADER);

        const auto px = ramp_pixels(k_w, k_h);
        const auto input = float_image(k_w, k_h, px);
        ASSERT_NE(input, nullptr);

        const uint32_t out_w = k_w / 2;
        const uint32_t out_h = k_h / 2;
        auto ctx = std::make_shared<TextureExecutionContext>(
            shader_config(shader, sizeof(ImagePush), { 8, 8, 1 }),
            ImageFormat::RGBA32F,
            TextureExecutionContext::OutputMode::IMAGE);
        ctx->stage_image(input);
        ctx->set_push_constants(ImagePush { .width = out_w, .height = out_h, .scale = 1.0F });
        ctx->prepare_output_image(out_w, out_h);
        ctx->set_output_dimensions(out_w, out_h);
        const auto fence = ctx->dispatch_async({});
        ctx->clear_output_dimensions();
        wait_and_release(fence);

        const auto output = ctx->get_output_image(0);
        ASSERT_NE(output, nullptr);
        EXPECT_EQ(output->get_width(), out_w);
        EXPECT_EQ(output->get_height(), out_h);

        std::vector<float> expected;
        expected.reserve(size_t { out_w } * out_h * k_channels);
        for (size_t y = 0; y < out_h; ++y) {
            for (size_t x = 0; x < out_w; ++x) {
                for (size_t c = 0; c < k_channels; ++c) {
                    expected.push_back(px[((2 * y) * k_w + 2 * x) * k_channels + c]);
                }
            }
        }
        EXPECT_EQ(read_floats(output, expected.size()), expected);
    }

    TYPED_TEST(TextureExecutionContextGpuTest, StagedOutputBecomesNextInputWithoutAliasing)
    {
        const auto shader = load_compute(k_scale_image_source);
        ASSERT_NE(shader, Portal::Graphics::INVALID_SHADER);

        const auto px = ramp_pixels(k_w, k_h);
        const auto input = float_image(k_w, k_h, px);
        ASSERT_NE(input, nullptr);

        auto ctx = std::make_shared<TextureExecutionContext>(
            shader_config(shader, sizeof(ImagePush), { 8, 8, 1 }),
            ImageFormat::RGBA32F,
            TextureExecutionContext::OutputMode::IMAGE);
        ctx->set_avoid_output_alias(true);

        ctx->stage_image(input);
        ctx->set_push_constants(ImagePush { .width = k_w, .height = k_h, .scale = 2.0F });
        ctx->prepare_output_image(k_w, k_h);
        ctx->set_output_dimensions(k_w, k_h);
        wait_and_release(ctx->dispatch_async({}));
        ctx->clear_output_dimensions();
        const auto first = ctx->get_output_image(0);
        ASSERT_NE(first, nullptr);

        ctx->stage_image(first);
        ctx->set_push_constants(ImagePush { .width = k_w, .height = k_h, .scale = 3.0F });
        ctx->prepare_output_image(k_w, k_h);
        ctx->set_output_dimensions(k_w, k_h);
        wait_and_release(ctx->dispatch_async({}));
        ctx->clear_output_dimensions();
        const auto second = ctx->get_output_image(0);
        ASSERT_NE(second, nullptr);

        EXPECT_NE(second, first);
        EXPECT_NE(second, input);
        EXPECT_EQ(read_floats(second, px.size()), scaled_floats(px, 6.0F));
    }

    TYPED_TEST(TextureExecutionContextGpuTest, OutputImageIsReusedForAnUnchangedSize)
    {
        const auto shader = load_compute(k_scale_image_source);
        ASSERT_NE(shader, Portal::Graphics::INVALID_SHADER);

        auto ctx = std::make_shared<TextureExecutionContext>(
            shader_config(shader, sizeof(ImagePush), { 8, 8, 1 }),
            ImageFormat::RGBA32F,
            TextureExecutionContext::OutputMode::IMAGE);

        ctx->prepare_output_image(k_w, k_h);
        const auto first = ctx->get_output_image(0);
        ASSERT_NE(first, nullptr);

        ctx->prepare_output_image(k_w, k_h);
        EXPECT_EQ(ctx->get_output_image(0), first);

        ctx->prepare_output_image(k_w / 2, k_h / 2);
        const auto smaller = ctx->get_output_image(0);
        ASSERT_NE(smaller, nullptr);
        EXPECT_NE(smaller, first);
        EXPECT_EQ(smaller->get_width(), k_w / 2);
        EXPECT_EQ(smaller->get_height(), k_h / 2);
    }

    TYPED_TEST(TextureExecutionContextGpuTest, ExecuteLeavesTheOutputImageShaderReadable)
    {
        const auto shader = load_compute(k_scale_image_source);
        ASSERT_NE(shader, Portal::Graphics::INVALID_SHADER);

        const auto px = ramp_pixels(k_w, k_h);
        const auto input = float_image(k_w, k_h, px);
        ASSERT_NE(input, nullptr);

        auto ctx = std::make_shared<TextureExecutionContext>(
            shader_config(shader, sizeof(ImagePush), { 8, 8, 1 }),
            ImageFormat::RGBA32F,
            TextureExecutionContext::OutputMode::IMAGE);
        ctx->stage_image(input);
        ctx->set_push_constants(ImagePush { .width = k_w, .height = k_h, .scale = 2.0F });
        ctx->prepare_output_image(k_w, k_h);
        ctx->set_output_dimensions(k_w, k_h);

        const auto result = ctx->execute(Datum<std::vector<Kakshya::DataVariant>> {}, ExecutionContext {});
        EXPECT_FALSE(result.has_container());

        const auto output = ctx->get_output_image(0);
        ASSERT_NE(output, nullptr);
        EXPECT_EQ(output->get_current_layout(), vk::ImageLayout::eShaderReadOnlyOptimal);
        EXPECT_EQ(read_floats(output, px.size()), scaled_floats(px, 2.0F));
    }

    TYPED_TEST(TextureExecutionContextGpuTest, ContainerInputSizesAndStagesTheOutput)
    {
        const auto shader = load_compute(k_scale_image_source);
        ASSERT_NE(shader, Portal::Graphics::INVALID_SHADER);

        const uint32_t w = 4;
        const uint32_t h = 4;
        const auto px = ramp_pixels(w, h);
        auto container = std::make_shared<Kakshya::TextureContainer>(w, h, ImageFormat::RGBA32F);
        container->set_pixels(std::span<const float>(px), 0);

        Datum<std::vector<Kakshya::DataVariant>> input {};
        input.set_container(container);

        auto ctx = std::make_shared<TextureExecutionContext>(
            shader_config(shader, sizeof(ImagePush), { 8, 8, 1 }),
            ImageFormat::RGBA32F,
            TextureExecutionContext::OutputMode::IMAGE);
        ctx->set_push_constants(ImagePush { .width = w, .height = h, .scale = 2.0F });

        EXPECT_NO_THROW(static_cast<void>(ctx->execute(input, ExecutionContext {})));

        const auto output = ctx->get_output_image(0);
        ASSERT_NE(output, nullptr);
        EXPECT_EQ(output->get_width(), w);
        EXPECT_EQ(output->get_height(), h);
        EXPECT_EQ(read_floats(output, px.size()), scaled_floats(px, 2.0F));
    }

    TYPED_TEST(TextureExecutionContextGpuTest, UnstagedImageSkipsTheDispatchWithoutThrowing)
    {
        const auto shader = load_compute(k_scale_image_source);
        ASSERT_NE(shader, Portal::Graphics::INVALID_SHADER);

        const auto px = ramp_pixels(k_w, k_h);
        const auto input = float_image(k_w, k_h, px);
        ASSERT_NE(input, nullptr);

        auto ctx = std::make_shared<TextureExecutionContext>(
            shader_config(shader, sizeof(ImagePush), { 8, 8, 1 }),
            ImageFormat::RGBA32F,
            TextureExecutionContext::OutputMode::IMAGE);
        ctx->set_push_constants(ImagePush { .width = k_w, .height = k_h, .scale = 2.0F });
        ctx->prepare_output_image(k_w, k_h);
        ctx->set_output_dimensions(k_w, k_h);

        FenceID skipped = INVALID_FENCE;
        EXPECT_NO_THROW(skipped = ctx->dispatch_async({}));
        EXPECT_EQ(skipped, INVALID_FENCE);
        ctx->clear_output_dimensions();

        ctx->stage_image(input);
        ctx->prepare_output_image(k_w, k_h);
        ctx->set_output_dimensions(k_w, k_h);
        wait_and_release(ctx->dispatch_async({}));
        ctx->clear_output_dimensions();

        const auto output = ctx->get_output_image(0);
        ASSERT_NE(output, nullptr);
        EXPECT_EQ(read_floats(output, px.size()), scaled_floats(px, 2.0F));
    }

    TYPED_TEST(TextureExecutionContextGpuTest, RecordedChainStaysOrderedUntilTheWait)
    {
        const auto shader = load_compute(k_scale_image_source);
        ASSERT_NE(shader, Portal::Graphics::INVALID_SHADER);

        const auto px = ramp_pixels(k_w, k_h);
        const auto input = float_image(k_w, k_h, px);
        ASSERT_NE(input, nullptr);

        auto recording = std::make_shared<GpuDispatchRecorder>(false);
        auto ctx = std::make_shared<TextureExecutionContext>(
            shader_config(shader, sizeof(ImagePush), { 8, 8, 1 }),
            ImageFormat::RGBA32F,
            TextureExecutionContext::OutputMode::IMAGE);
        ctx->set_recording(recording);
        ctx->set_avoid_output_alias(true);

        ctx->stage_image(input);
        ctx->set_push_constants(ImagePush { .width = k_w, .height = k_h, .scale = 2.0F });
        ctx->prepare_output_image(k_w, k_h);
        ctx->set_output_dimensions(k_w, k_h);
        EXPECT_EQ(ctx->dispatch_async({}), INVALID_FENCE);
        ctx->clear_output_dimensions();
        const auto first = ctx->get_output_image(0);
        ASSERT_NE(first, nullptr);

        ctx->stage_image(first);
        ctx->set_push_constants(ImagePush { .width = k_w, .height = k_h, .scale = 3.0F });
        ctx->prepare_output_image(k_w, k_h);
        ctx->set_output_dimensions(k_w, k_h);
        EXPECT_EQ(ctx->dispatch_async({}), INVALID_FENCE);
        ctx->clear_output_dimensions();
        const auto second = ctx->get_output_image(0);
        ASSERT_NE(second, nullptr);

        recording->wait();
        EXPECT_EQ(read_floats(second, px.size()), scaled_floats(px, 6.0F));
    }

    TYPED_TEST(TextureExecutionContextGpuTest, ReductionReadsTheBufferThroughRecordedCollect)
    {
        const auto shader = load_compute(k_row_reduce_source);
        ASSERT_NE(shader, Portal::Graphics::INVALID_SHADER);

        const auto px = ramp_pixels(k_w, k_h);
        const auto input = float_image(k_w, k_h, px);
        ASSERT_NE(input, nullptr);

        constexpr uint32_t count = 4;
        auto recording = std::make_shared<GpuDispatchRecorder>(false);
        auto ctx = std::make_shared<TextureExecutionContext>(
            shader_config(shader, sizeof(CountPush), { 64, 1, 1 }),
            ImageFormat::RGBA32F,
            TextureExecutionContext::OutputMode::SCALAR,
            1,
            std::vector<GpuBufferBinding> {
                { .set = 0, .binding = 6, .direction = GpuBufferBinding::Direction::OUTPUT, .element_type = GpuBufferBinding::ElementType::FLOAT32 } });
        ctx->set_recording(recording);

        ctx->stage_image(input);
        ctx->set_push_constants(CountPush { .count = count });
        ctx->set_output_size(6, count * sizeof(float));
        ctx->set_output_dimensions(count, 1);
        EXPECT_EQ(ctx->dispatch_async({}), INVALID_FENCE);
        ctx->clear_output_dimensions();

        const auto collected = ctx->collect_result();
        const auto it = collected.aux.find(6);
        ASSERT_NE(it, collected.aux.end());
        EXPECT_EQ(floats_from(it->second), red_values(px, count));
    }

    TYPED_TEST(TextureExecutionContextGpuTest, FenceWaitThenCollectResultMatchesTheDirectPattern)
    {
        const auto shader = load_compute(k_row_reduce_source);
        ASSERT_NE(shader, Portal::Graphics::INVALID_SHADER);

        const auto px = ramp_pixels(k_w, k_h);
        const auto input = float_image(k_w, k_h, px);
        ASSERT_NE(input, nullptr);

        constexpr uint32_t count = 4;
        auto ctx = std::make_shared<TextureExecutionContext>(
            shader_config(shader, sizeof(CountPush), { 64, 1, 1 }),
            ImageFormat::RGBA32F,
            TextureExecutionContext::OutputMode::SCALAR,
            1,
            std::vector<GpuBufferBinding> {
                { .set = 0, .binding = 6, .direction = GpuBufferBinding::Direction::OUTPUT, .element_type = GpuBufferBinding::ElementType::FLOAT32 } });

        ctx->stage_image(input);
        ctx->set_push_constants(CountPush { .count = count });
        ctx->set_output_size(6, count * sizeof(float));
        ctx->set_output_dimensions(count, 1);
        const auto fence = ctx->dispatch_async({});
        ctx->clear_output_dimensions();
        wait_and_release(fence);

        const auto collected = ctx->collect_result();
        const auto it = collected.aux.find(6);
        ASSERT_NE(it, collected.aux.end());
        EXPECT_EQ(floats_from(it->second), red_values(px, count));
    }

    TYPED_TEST(TextureExecutionContextGpuTest, UnusedDeclaredImageBindingDoesNotBlockTheDispatch)
    {
        const auto shader = load_compute(k_row_reduce_source);
        ASSERT_NE(shader, Portal::Graphics::INVALID_SHADER);

        const auto px = ramp_pixels(k_w, k_h);
        const auto input = float_image(k_w, k_h, px);
        ASSERT_NE(input, nullptr);

        constexpr uint32_t count = 4;
        auto recording = std::make_shared<GpuDispatchRecorder>(false);
        auto ctx = std::make_shared<TextureExecutionContext>(
            shader_config(shader, sizeof(CountPush), { 64, 1, 1 }),
            ImageFormat::RGBA32F,
            TextureExecutionContext::OutputMode::SCALAR,
            1,
            std::vector<GpuBufferBinding> {
                { .set = 0, .binding = 2, .direction = GpuBufferBinding::Direction::INPUT, .element_type = GpuBufferBinding::ElementType::IMAGE_SAMPLED },
                { .set = 0, .binding = 6, .direction = GpuBufferBinding::Direction::OUTPUT, .element_type = GpuBufferBinding::ElementType::FLOAT32 } });
        ctx->set_recording(recording);

        ctx->stage_image(input);
        ctx->set_push_constants(CountPush { .count = count });
        ctx->set_output_size(6, count * sizeof(float));
        ctx->set_output_dimensions(count, 1);
        EXPECT_EQ(ctx->dispatch_async({}), INVALID_FENCE);
        ctx->clear_output_dimensions();

        const auto collected = ctx->collect_result();
        const auto it = collected.aux.find(6);
        ASSERT_NE(it, collected.aux.end());
        EXPECT_EQ(floats_from(it->second), red_values(px, count));
    }

    TYPED_TEST(TextureExecutionContextGpuTest, ContainerOutputModeReturnsTheOutputAsAContainer)
    {
        const auto shader = load_compute(k_scale_image_source);
        ASSERT_NE(shader, Portal::Graphics::INVALID_SHADER);

        const auto px = ramp_pixels(k_w, k_h);
        auto source = std::make_shared<Kakshya::TextureContainer>(k_w, k_h, ImageFormat::RGBA32F);
        source->set_pixels(std::span<const float>(px), 0);

        Datum<std::vector<Kakshya::DataVariant>> input {};
        input.set_container(source);

        auto ctx = std::make_shared<TextureExecutionContext>(
            shader_config(shader, sizeof(ImagePush), { 8, 8, 1 }),
            ImageFormat::RGBA32F,
            TextureExecutionContext::OutputMode::CONTAINER);
        ctx->set_push_constants(ImagePush { .width = k_w, .height = k_h, .scale = 2.0F });

        const auto result = ctx->execute(input, ExecutionContext {});

        ASSERT_TRUE(result.has_container());
        const auto out = std::dynamic_pointer_cast<Kakshya::TextureContainer>(result.get_container());
        ASSERT_NE(out, nullptr);
        EXPECT_EQ(out->get_width(), k_w);
        EXPECT_EQ(out->get_height(), k_h);
        EXPECT_EQ(container_floats(*out, 0), scaled_floats(px, 2.0F));
    }

    TYPED_TEST(TextureExecutionContextGpuTest, ContainerOutputUsesTheDimensionOverride)
    {
        const auto shader = load_compute(k_decimate_image_source);
        ASSERT_NE(shader, Portal::Graphics::INVALID_SHADER);

        const auto px = ramp_pixels(k_w, k_h);
        auto source = std::make_shared<Kakshya::TextureContainer>(k_w, k_h, ImageFormat::RGBA32F);
        source->set_pixels(std::span<const float>(px), 0);

        const uint32_t out_w = k_w / 2;
        const uint32_t out_h = k_h / 2;
        Datum<std::vector<Kakshya::DataVariant>> input {};
        input.set_container(source);

        auto ctx = std::make_shared<TextureExecutionContext>(
            shader_config(shader, sizeof(ImagePush), { 8, 8, 1 }),
            ImageFormat::RGBA32F,
            TextureExecutionContext::OutputMode::CONTAINER);
        ctx->set_push_constants(ImagePush { .width = out_w, .height = out_h, .scale = 1.0F });
        ctx->set_output_dimensions(out_w, out_h);

        const auto result = ctx->execute(input, ExecutionContext {});
        ctx->clear_output_dimensions();

        ASSERT_TRUE(result.has_container());
        const auto out = std::dynamic_pointer_cast<Kakshya::TextureContainer>(result.get_container());
        ASSERT_NE(out, nullptr);
        EXPECT_EQ(out->get_width(), out_w);
        EXPECT_EQ(out->get_height(), out_h);
        EXPECT_EQ(container_floats(*out, 0), decimated_values(px, out_w, out_h));
    }

    TYPED_TEST(TextureExecutionContextGpuTest, CollectContainerResultReadsTheOutputAfterAFenceWait)
    {
        const auto shader = load_compute(k_scale_image_source);
        ASSERT_NE(shader, Portal::Graphics::INVALID_SHADER);

        const auto px = ramp_pixels(k_w, k_h);
        const auto input = float_image(k_w, k_h, px);
        ASSERT_NE(input, nullptr);

        auto ctx = std::make_shared<TextureExecutionContext>(
            shader_config(shader, sizeof(ImagePush), { 8, 8, 1 }),
            ImageFormat::RGBA32F,
            TextureExecutionContext::OutputMode::CONTAINER);
        ctx->stage_image(input);
        ctx->set_push_constants(ImagePush { .width = k_w, .height = k_h, .scale = 2.0F });
        ctx->prepare_output_image(k_w, k_h);
        ctx->set_output_dimensions(k_w, k_h);
        const auto fence = ctx->dispatch_async({});
        ctx->clear_output_dimensions();
        wait_and_release(fence);

        const auto result = ctx->collect_container_result();

        ASSERT_TRUE(result.has_container());
        const auto out = std::dynamic_pointer_cast<Kakshya::TextureContainer>(result.get_container());
        ASSERT_NE(out, nullptr);
        EXPECT_EQ(out->get_width(), k_w);
        EXPECT_EQ(out->get_height(), k_h);
        EXPECT_EQ(container_floats(*out, 0), scaled_floats(px, 2.0F));
    }

    TYPED_TEST(TextureExecutionContextGpuTest, StageContainerStagesASingleLayerContainer)
    {
        const auto shader = load_compute(k_scale_image_source);
        ASSERT_NE(shader, Portal::Graphics::INVALID_SHADER);

        const auto px = ramp_pixels(k_w, k_h);
        Kakshya::TextureContainer source(k_w, k_h, ImageFormat::RGBA32F);
        source.set_pixels(std::span<const float>(px), 0);

        auto ctx = std::make_shared<TextureExecutionContext>(
            shader_config(shader, sizeof(ImagePush), { 8, 8, 1 }),
            ImageFormat::RGBA32F,
            TextureExecutionContext::OutputMode::IMAGE);
        ctx->stage_container(source, 0);
        ctx->set_push_constants(ImagePush { .width = k_w, .height = k_h, .scale = 2.0F });
        ctx->prepare_output_image(k_w, k_h);
        ctx->set_output_dimensions(k_w, k_h);
        wait_and_release(ctx->dispatch_async({}));
        ctx->clear_output_dimensions();

        const auto output = ctx->get_output_image(0);
        ASSERT_NE(output, nullptr);
        EXPECT_EQ(read_floats(output, px.size()), scaled_floats(px, 2.0F));
    }

    TYPED_TEST(TextureExecutionContextGpuTest, MultiLayerContainerStagesEveryLayerAsAnArray)
    {
        const auto shader = load_compute(k_scale_layer_source);
        ASSERT_NE(shader, Portal::Graphics::INVALID_SHADER);

        const auto layer0 = ramp_pixels(k_w, k_h);
        auto layer1 = ramp_pixels(k_w, k_h);
        for (auto& v : layer1) {
            v = v * 3.0F + 1.0F;
        }

        auto source = std::make_shared<Kakshya::TextureContainer>(k_w, k_h, ImageFormat::RGBA32F, 2U);
        source->set_pixels(std::span<const float>(layer0), 0);
        source->set_pixels(std::span<const float>(layer1), 1);

        Datum<std::vector<Kakshya::DataVariant>> input {};
        input.set_container(source);

        auto ctx = std::make_shared<TextureExecutionContext>(
            shader_config(shader, sizeof(LayerPush), { 8, 8, 1 }),
            ImageFormat::RGBA32F,
            TextureExecutionContext::OutputMode::IMAGE);
        ctx->set_push_constants(LayerPush { .width = k_w, .height = k_h, .scale = 2.0F, .layer = 1U });

        EXPECT_NO_THROW(static_cast<void>(ctx->execute(input, ExecutionContext {})));

        const auto output = ctx->get_output_image(0);
        ASSERT_NE(output, nullptr);
        EXPECT_EQ(read_floats(output, layer1.size()), scaled_floats(layer1, 2.0F));
    }

    TYPED_TEST(TextureExecutionContextGpuTest, ExplicitBindingsConstructorRunsAnImageDispatch)
    {
        const auto shader = load_compute(k_copy_rgba8_source);
        ASSERT_NE(shader, Portal::Graphics::INVALID_SHADER);

        std::vector<uint8_t> bytes(size_t { k_w } * k_h * k_channels);
        for (size_t i = 0; i < bytes.size(); ++i) {
            bytes[i] = static_cast<uint8_t>((i * 5) & 0xFF);
        }
        const auto input = byte_image(k_w, k_h, bytes);
        ASSERT_NE(input, nullptr);

        auto ctx = std::make_shared<TextureExecutionContext>(
            shader_config(shader, sizeof(ImagePush), { 8, 8, 1 }),
            std::vector<GpuBufferBinding> {
                binding_of(1, GpuBufferBinding::Direction::INPUT, GpuBufferBinding::ElementType::IMAGE_SAMPLED),
                binding_of(0, GpuBufferBinding::Direction::OUTPUT, GpuBufferBinding::ElementType::IMAGE_STORAGE) },
            TextureExecutionContext::OutputMode::IMAGE);
        ctx->stage_image(input);
        ctx->set_push_constants(ImagePush { .width = k_w, .height = k_h, .scale = 1.0F });
        ctx->prepare_output_image(k_w, k_h);
        ctx->set_output_dimensions(k_w, k_h);
        wait_and_release(ctx->dispatch_async({}));
        ctx->clear_output_dimensions();

        const auto output = ctx->get_output_image(0);
        ASSERT_NE(output, nullptr);
        EXPECT_EQ(read_bytes(output, bytes.size()), bytes);
    }

    TYPED_TEST(ShaderExecutionContextGpuTest, ChainedIndirectAccumulatesOverEveryPass)
    {
        const auto shader = load_compute(k_add_words_source);
        ASSERT_NE(shader, Portal::Graphics::INVALID_SHADER);

        const auto input = ascending(k_count);
        auto executor = std::make_shared<ShaderExecutionContext<>>(
            shader_config(shader, sizeof(AddPush), { 16, 1, 1 }),
            std::vector<GpuBufferBinding> {
                binding_of(0, GpuBufferBinding::Direction::INPUT_OUTPUT, GpuBufferBinding::ElementType::UINT32),
                binding_of(3, GpuBufferBinding::Direction::INPUT, GpuBufferBinding::ElementType::UINT32,
                    Portal::Graphics::BufferUsageHint::INDIRECT) });
        executor->ensure_shared_buffer(0, 3, 3, GpuBufferBinding::ElementType::UINT32,
            Portal::Graphics::BufferUsageHint::INDIRECT);
        executor->set_binding_data(0, input);
        executor->set_output_size(0, k_count * sizeof(uint32_t));

        ExecutionContext ctx;
        ctx.mode = ExecutionMode::CHAINED_INDIRECT;
        ctx.parameters = ChainedIndirectParams {
            .pass_count = 3,
            .pc_updater = [](uint32_t pass, uint32_t, void* raw) {
                auto* push = static_cast<AddPush*>(raw);
                push->count = static_cast<uint32_t>(k_count);
                push->add = pass + 1;
            },
            .passes_per_batch = std::nullopt,
        };

        const auto result = executor->execute(Datum<std::vector<Kakshya::DataVariant>> {}, ctx);

        EXPECT_EQ(ShaderExecutionContext<>::read_output<uint32_t>(result, 0), summed(input, filled(k_count, 6)));
    }

    TYPED_TEST(ShaderExecutionContextGpuTest, DoubleInputComesBackAsDoubleThroughStructureInfo)
    {
        const auto shader = load_compute(k_float_scale_source);
        ASSERT_NE(shader, Portal::Graphics::INVALID_SHADER);

        std::vector<double> values(k_count);
        for (size_t i = 0; i < k_count; ++i) {
            values[i] = static_cast<double>(i + 1);
        }
        Datum<std::vector<Kakshya::DataVariant>> input {};
        input.data.emplace_back(values);

        auto executor = std::make_shared<ShaderExecutionContext<>>(
            shader_config(shader, sizeof(FloatPush), { 16, 1, 1 }),
            std::vector<GpuBufferBinding> {
                binding_of(0, GpuBufferBinding::Direction::INPUT, GpuBufferBinding::ElementType::FLOAT32),
                binding_of(1, GpuBufferBinding::Direction::OUTPUT, GpuBufferBinding::ElementType::FLOAT32) });
        executor->set_output_size(1, k_count * sizeof(float));
        executor->push(FloatPush { .count = static_cast<uint32_t>(k_count), .factor = 2.0F });

        const auto result = executor->execute(input, ExecutionContext {});

        ASSERT_EQ(result.data.size(), 1U);
        const auto* out = std::get_if<std::vector<double>>(&result.data[0]);
        ASSERT_NE(out, nullptr);
        std::vector<double> expected(values);
        for (auto& v : expected) {
            v *= 2.0;
        }
        EXPECT_EQ(*out, expected);
    }

} // namespace
} // namespace MayaFlux::Test
