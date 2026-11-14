#include "gtest/gtest.h"
#include "../../support/test_metadata.hpp"


#include "MayaFlux/Portal/Graphics/ShaderFoundry.hpp"
#include "MayaFlux/Portal/Graphics/ShaderSpec.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Kakshya::DataModality;
    using Kakshya::GpuDataFormat;
    using Portal::Graphics::BindingDirection;
    using Portal::Graphics::KernelOp;
    using Portal::Graphics::KernelSource;
    using Portal::Graphics::KernelTemplate;
    using Portal::Graphics::ShaderSpec;
    using Portal::Graphics::push_constant_layout;

    // ------------------------------------------------------------------
    // ShaderSpec::Assemble
    // ------------------------------------------------------------------

    TEST(ShaderSpecAssembleTest, DefaultsDescribeAnElementwiseScaleOverBlocksOf256)
    {
        const ShaderSpec spec = ShaderSpec::Assemble {}.build();

        EXPECT_EQ(spec.tmpl, KernelTemplate::Elementwise);
        EXPECT_EQ(spec.op, KernelOp::Scale);
        EXPECT_TRUE(spec.bindings.empty());
        EXPECT_TRUE(spec.pc_fields.empty());
        EXPECT_TRUE(spec.functions.empty());
        EXPECT_FALSE(spec.kernel.has_value());
        EXPECT_EQ(spec.push_constant_bytes, 0U);
        EXPECT_EQ(spec.workgroup_size, (std::array<uint32_t, 3> { 256, 1, 1 }));
    }

    TEST(ShaderSpecAssembleTest, TemplateOpAndWorkgroupAreCarriedIntoTheSpec)
    {
        const ShaderSpec spec = ShaderSpec::Assemble {}
                                    .tmpl(KernelTemplate::Reduction)
                                    .op(KernelOp::Sum)
                                    .workgroup(64, 2, 4)
                                    .build();

        EXPECT_EQ(spec.tmpl, KernelTemplate::Reduction);
        EXPECT_EQ(spec.op, KernelOp::Sum);
        EXPECT_EQ(spec.workgroup_size, (std::array<uint32_t, 3> { 64, 2, 4 }));
    }

    TEST(ShaderSpecAssembleTest, WorkgroupYAndZDefaultToOne)
    {
        const ShaderSpec spec = ShaderSpec::Assemble {}.workgroup(128).build();

        EXPECT_EQ(spec.workgroup_size, (std::array<uint32_t, 3> { 128, 1, 1 }));
    }

    TEST(ShaderSpecAssembleTest, BindingsGetSequentialIndicesInDeclarationOrder)
    {
        const ShaderSpec spec = ShaderSpec::Assemble {}
                                    .ssbo("a", BindingDirection::Input, GpuDataFormat::FLOAT32)
                                    .storage_image("img")
                                    .texture("tex")
                                    .ssbo("b", BindingDirection::Output, GpuDataFormat::UINT32)
                                    .build();

        ASSERT_EQ(spec.bindings.size(), 4U);
        for (uint32_t i = 0; i < spec.bindings.size(); ++i) {
            EXPECT_EQ(spec.bindings[i].binding_index, i) << spec.bindings[i].name;
        }
        EXPECT_EQ(spec.bindings[0].name, "a");
        EXPECT_EQ(spec.bindings[1].name, "img");
        EXPECT_EQ(spec.bindings[2].name, "tex");
        EXPECT_EQ(spec.bindings[3].name, "b");
    }

    TEST(ShaderSpecAssembleTest, StartBindingAndStartSetOffsetLaterSsbos)
    {
        const ShaderSpec spec = ShaderSpec::Assemble {}
                                    .start_set(2)
                                    .start_binding(5)
                                    .ssbo("a", BindingDirection::InOut, GpuDataFormat::FLOAT32)
                                    .ssbo("b", BindingDirection::InOut, GpuDataFormat::FLOAT32)
                                    .build();

        ASSERT_EQ(spec.bindings.size(), 2U);
        EXPECT_EQ(spec.bindings[0].set, 2U);
        EXPECT_EQ(spec.bindings[0].binding_index, 5U);
        EXPECT_EQ(spec.bindings[1].set, 2U);
        EXPECT_EQ(spec.bindings[1].binding_index, 6U);
    }

    TEST(ShaderSpecAssembleTest, SsboRecordsDirectionFormatAndModality)
    {
        const ShaderSpec spec = ShaderSpec::Assemble {}
                                    .ssbo("pos", BindingDirection::Output, GpuDataFormat::VEC4_F32, DataModality::VERTEX_POSITIONS_3D)
                                    .build();

        ASSERT_EQ(spec.bindings.size(), 1U);
        EXPECT_EQ(spec.bindings[0].direction, BindingDirection::Output);
        EXPECT_EQ(spec.bindings[0].format, GpuDataFormat::VEC4_F32);
        EXPECT_EQ(spec.bindings[0].modality, DataModality::VERTEX_POSITIONS_3D);
    }

    TEST(ShaderSpecAssembleTest, SsboModalityDefaultsToScalarFloat)
    {
        const ShaderSpec spec = ShaderSpec::Assemble {}
                                    .ssbo("sig", BindingDirection::InOut, GpuDataFormat::FLOAT32)
                                    .build();

        EXPECT_EQ(spec.bindings.at(0).modality, DataModality::SCALAR_F32);
    }

    TEST(ShaderSpecAssembleTest, TextureIsAnInputSampledImageOfFourFloatChannels)
    {
        const ShaderSpec spec = ShaderSpec::Assemble {}.texture("src").build();

        ASSERT_EQ(spec.bindings.size(), 1U);
        EXPECT_EQ(spec.bindings[0].modality, DataModality::TEXTURE_2D);
        EXPECT_EQ(spec.bindings[0].direction, BindingDirection::Input);
        EXPECT_EQ(spec.bindings[0].format, GpuDataFormat::VEC4_F32);
    }

    TEST(ShaderSpecAssembleTest, StorageImageDefaultsToOutputAndHonoursAnExplicitDirection)
    {
        const ShaderSpec spec = ShaderSpec::Assemble {}
                                    .storage_image("dst")
                                    .storage_image("src", BindingDirection::Input)
                                    .build();

        ASSERT_EQ(spec.bindings.size(), 2U);
        EXPECT_EQ(spec.bindings[0].modality, DataModality::IMAGE_2D);
        EXPECT_EQ(spec.bindings[0].direction, BindingDirection::Output);
        EXPECT_EQ(spec.bindings[1].modality, DataModality::IMAGE_2D);
        EXPECT_EQ(spec.bindings[1].direction, BindingDirection::Input);
        EXPECT_EQ(spec.bindings[1].format, GpuDataFormat::VEC4_F32);
    }

    TEST(ShaderSpecGeneratorTest, ASpecOfEmittedFormatsIsSupported)
    {
        ShaderSpec spec;
        spec.bindings.push_back({ .name = "in_values", .direction = BindingDirection::Input, .format = GpuDataFormat::VEC2_U32 });
        spec.bindings.push_back({ .name = "out_values", .direction = BindingDirection::Output, .format = GpuDataFormat::FLOAT32 });
        Portal::Graphics::PushConstantField field;
        field.name = "factor";
        field.format = GpuDataFormat::FLOAT32;
        spec.pc_fields.push_back(field);

        EXPECT_TRUE(Portal::Graphics::detail::double_field_reason(spec).empty());
    }

    TEST(ShaderSpecGeneratorTest, AFloat64BindingIsNamed)
    {
        ShaderSpec spec;
        spec.bindings.push_back({ .name = "samples", .direction = BindingDirection::InOut, .format = GpuDataFormat::FLOAT64 });

        const auto reason = Portal::Graphics::detail::double_field_reason(spec);

        EXPECT_NE(reason.find("samples"), std::string::npos);
        EXPECT_NE(reason.find("64-bit float"), std::string::npos);
    }

    TEST(ShaderSpecGeneratorTest, AVec2F64BindingIsNamedToo)
    {
        ShaderSpec spec;
        spec.bindings.push_back({ .name = "pairs", .direction = BindingDirection::Input, .format = GpuDataFormat::VEC2_F64 });

        EXPECT_FALSE(Portal::Graphics::detail::double_field_reason(spec).empty());
    }

    TEST(ShaderSpecGeneratorTest, AFloat64PushConstantFieldIsNamed)
    {
        ShaderSpec spec;
        Portal::Graphics::PushConstantField field;
        field.name = "rate";
        field.format = GpuDataFormat::FLOAT64;
        spec.pc_fields.push_back(field);

        const auto reason = Portal::Graphics::detail::double_field_reason(spec);

        EXPECT_NE(reason.find("rate"), std::string::npos);
        EXPECT_NE(reason.find("64-bit float"), std::string::npos);
    }

    TEST(ShaderSpecGeneratorTest, AUserKernelDeclaresDoubleFieldsAsDoubleTypes)
    {
        const auto spec = ShaderSpec::Assemble {}
                              .ssbo("sig", BindingDirection::InOut, GpuDataFormat::FLOAT64)
                              .ssbo("pairs", BindingDirection::Input, GpuDataFormat::VEC2_F64)
                              .ssbo("triples", BindingDirection::Input, GpuDataFormat::VEC3_F64)
                              .ssbo("quads", BindingDirection::Input, GpuDataFormat::VEC4_F64)
                              .pc("rate", GpuDataFormat::FLOAT64)
                              .kernel(MF_KERNEL([](double* sig, const double* pairs, const double* triples, const double* quads, double rate, uint32_t i) {
                                  sig[i] = sig[i] * rate + pairs[i] + triples[i] + quads[i];
                              }))
                              .build();

        const std::string glsl = Portal::Graphics::detail::emit_glsl_kernel(spec);

        EXPECT_NE(glsl.find("double sig[];"), std::string::npos);
        EXPECT_NE(glsl.find("dvec2 pairs[];"), std::string::npos);
        EXPECT_NE(glsl.find("dvec3 triples[];"), std::string::npos);
        EXPECT_NE(glsl.find("dvec4 quads[];"), std::string::npos);
        EXPECT_NE(glsl.find("double rate;"), std::string::npos);
    }

    TEST(ShaderSpecGeneratorTest, Vec3AndVec4F64BindingsAreNamed)
    {
        ShaderSpec spec;
        spec.bindings.push_back({ .name = "triples", .direction = BindingDirection::Input, .format = GpuDataFormat::VEC3_F64 });
        spec.bindings.push_back({ .name = "quads", .direction = BindingDirection::Input, .format = GpuDataFormat::VEC4_F64 });

        EXPECT_NE(Portal::Graphics::detail::double_field_reason(spec).find("triples"), std::string::npos);

        spec.bindings.erase(spec.bindings.begin());
        EXPECT_NE(Portal::Graphics::detail::double_field_reason(spec).find("quads"), std::string::npos);
    }

    TEST(ShaderSpecAssembleTest, PushConstantFieldsKeepDeclarationOrderAndDefaultToFloat)
    {
        const ShaderSpec spec = ShaderSpec::Assemble {}
                                    .pc("gain")
                                    .pc("count", GpuDataFormat::UINT32)
                                    .pc("offset")
                                    .build();

        ASSERT_EQ(spec.pc_fields.size(), 3U);
        EXPECT_EQ(spec.pc_fields[0].name, "gain");
        EXPECT_EQ(spec.pc_fields[0].format, GpuDataFormat::FLOAT32);
        EXPECT_EQ(spec.pc_fields[1].name, "count");
        EXPECT_EQ(spec.pc_fields[1].format, GpuDataFormat::UINT32);
        EXPECT_EQ(spec.pc_fields[2].name, "offset");
    }

    TEST(ShaderSpecAssembleTest, PushConstantBytesSumTheFieldFormats)
    {
        const ShaderSpec spec = ShaderSpec::Assemble {}
                                    .pc("gain")
                                    .pc("count", GpuDataFormat::UINT32)
                                    .pc("tint", GpuDataFormat::VEC4_F32)
                                    .build();

        EXPECT_EQ(spec.push_constant_bytes, 32U);
    }

    TEST(ShaderSpecAssembleTest, PushConstantBytesFollowStd430AlignmentWithMixedFormats)
    {
        const ShaderSpec spec = ShaderSpec::Assemble {}
                                    .pc("gain")
                                    .pc("dir", GpuDataFormat::VEC3_F32)
                                    .pc("tail")
                                    .pc("wide", GpuDataFormat::FLOAT64)
                                    .build();

        EXPECT_EQ(push_constant_layout(spec.pc_fields).offsets, (std::vector<uint32_t> { 0, 16, 28, 32 }));
        EXPECT_EQ(spec.push_constant_bytes, 48U);
    }

    TEST(ShaderSpecAssembleTest, FloatOnlyPushConstantsStayPacked)
    {
        const ShaderSpec spec = ShaderSpec::Assemble {}
                                    .pc("a")
                                    .pc("b")
                                    .pc("c", GpuDataFormat::UINT32)
                                    .build();

        EXPECT_EQ(push_constant_layout(spec.pc_fields).offsets, (std::vector<uint32_t> { 0, 4, 8 }));
        EXPECT_EQ(spec.push_constant_bytes, 12U);
    }

    TEST(ShaderSpecAssembleTest, FunctionsKeepDeclarationOrderAndAllFourParts)
    {
        const ShaderSpec spec = ShaderSpec::Assemble {}
                                    .function("float", "first", "float x", "    return x;")
                                    .function("vec3", "second", "vec3 p, float k", "    return p * k;")
                                    .build();

        ASSERT_EQ(spec.functions.size(), 2U);
        EXPECT_EQ(spec.functions[0].return_type, "float");
        EXPECT_EQ(spec.functions[0].name, "first");
        EXPECT_EQ(spec.functions[0].params, "float x");
        EXPECT_EQ(spec.functions[0].body, "    return x;");
        EXPECT_EQ(spec.functions[1].name, "second");
        EXPECT_EQ(spec.functions[1].params, "vec3 p, float k");
    }

    TEST(ShaderSpecAssembleTest, KernelWithoutParamNamesTakesBindingsThenPushFieldsThenIndex)
    {
        KernelSource source;
        source.body = " sig[i] *= gain; ";

        const ShaderSpec spec = ShaderSpec::Assemble {}
                                    .ssbo("sig", BindingDirection::InOut, GpuDataFormat::FLOAT32)
                                    .ssbo("aux", BindingDirection::Input, GpuDataFormat::FLOAT32)
                                    .pc("gain")
                                    .pc("bias")
                                    .kernel(source)
                                    .build();

        ASSERT_TRUE(spec.kernel.has_value());
        EXPECT_EQ(spec.kernel->param_names,
            (std::vector<std::string> { "sig", "aux", "gain", "bias", "i" }));
        EXPECT_EQ(spec.kernel->body, " sig[i] *= gain; ");
    }

    TEST(ShaderSpecAssembleTest, KernelWithParamNamesKeepsThemUntouched)
    {
        const ShaderSpec spec = ShaderSpec::Assemble {}
                                    .ssbo("sig", BindingDirection::InOut, GpuDataFormat::FLOAT32)
                                    .pc("gain")
                                    .kernel(MF_KERNEL([](float* data, float amount) { data[0] *= amount; }))
                                    .build();

        ASSERT_TRUE(spec.kernel.has_value());
        EXPECT_EQ(spec.kernel->param_names, (std::vector<std::string> { "data", "amount" }));
    }

    // ------------------------------------------------------------------
    // KernelSource::parse and MF_KERNEL
    // ------------------------------------------------------------------

    TEST(KernelSourceTest, ParameterNamesDropTypesAndPointerStars)
    {
        const KernelSource ks = KernelSource::parse(
            "[](float* sig, const float* ref, float gain, uint32_t i) { sig[i] = gain; }");

        EXPECT_EQ(ks.param_names, (std::vector<std::string> { "sig", "ref", "gain", "i" }));
    }

    TEST(KernelSourceTest, BodyIsTheTextBetweenTheOutermostBracesVerbatim)
    {
        const KernelSource ks = KernelSource::parse(
            "[](float* sig, uint32_t i) {\n    sig[i] = 1.0f;\n}");

        EXPECT_EQ(ks.body, "\n    sig[i] = 1.0f;\n");
    }

    TEST(KernelSourceTest, NestedBracesStayInsideTheBody)
    {
        const KernelSource ks = KernelSource::parse(
            "[](float* sig, uint32_t i) { if (sig[i] > 0.0f) { sig[i] = 0.0f; } sig[i] += 1.0f; }");

        EXPECT_EQ(ks.body, " if (sig[i] > 0.0f) { sig[i] = 0.0f; } sig[i] += 1.0f; ");
    }

    TEST(KernelSourceTest, CommasInsideTemplateArgumentsDoNotSplitAParameter)
    {
        const KernelSource ks = KernelSource::parse(
            "[](std::array<float, 2> pair, float gain) { }");

        EXPECT_EQ(ks.param_names, (std::vector<std::string> { "pair", "gain" }));
    }

    TEST(KernelSourceTest, CaptureListDoesNotDisturbParameterExtraction)
    {
        const KernelSource ks = KernelSource::parse("[&](float* sig, uint32_t i) { sig[i] = 0.0f; }");

        EXPECT_EQ(ks.param_names, (std::vector<std::string> { "sig", "i" }));
        EXPECT_EQ(ks.body, " sig[i] = 0.0f; ");
    }

    TEST(KernelSourceTest, EmptyParameterListAndBodyAreAccepted)
    {
        const KernelSource ks = KernelSource::parse("[]() { }");

        EXPECT_TRUE(ks.param_names.empty());
        EXPECT_EQ(ks.body, " ");
    }

    TEST(KernelSourceTest, RawKeepsTheStringifiedLambdaUnchanged)
    {
        const std::string text = "[](float* sig, uint32_t i) { sig[i] = 0.0f; }";

        EXPECT_EQ(KernelSource::parse(text).raw, text);
    }

    TEST(KernelSourceTest, TextWithoutAParameterListYieldsNoParametersAndNoBody)
    {
        const KernelSource ks = KernelSource::parse("not a lambda");

        EXPECT_TRUE(ks.param_names.empty());
        EXPECT_TRUE(ks.body.empty());
        EXPECT_EQ(ks.raw, "not a lambda");
    }

    TEST(KernelSourceTest, ParameterListWithoutABodyYieldsNamesOnly)
    {
        const KernelSource ks = KernelSource::parse("[](float* sig, uint32_t i)");

        EXPECT_EQ(ks.param_names, (std::vector<std::string> { "sig", "i" }));
        EXPECT_TRUE(ks.body.empty());
    }

    TEST(KernelSourceTest, MfKernelParsesTheLambdaWrittenAtTheCallSite)
    {
        const KernelSource ks = MF_KERNEL([](float* sig, float gain, uint32_t i) {
            sig[i] = sig[i] * gain;
        });

        EXPECT_EQ(ks.param_names, (std::vector<std::string> { "sig", "gain", "i" }));
        EXPECT_NE(ks.body.find("sig[i] = sig[i] * gain;"), std::string::npos);
    }

} // namespace
} // namespace MayaFlux::Test
