#include "gtest/gtest.h"

#include "MayaFlux/Kinesis/Tendency/DualField.hpp"
#include "MayaFlux/Kinesis/Tendency/ShaderCompat.hpp"
#include "MayaFlux/Nodes/Network/Operators/GpuFieldOperator.hpp"
#include "MayaFlux/Portal/Graphics/ShaderFoundry.hpp"

namespace MayaFlux::Test {

namespace {

    using Kinesis::FieldTarget;
    using Nodes::Network::GpuFieldOperator;
    using Nodes::Network::SpatialFieldConfig;
    using Portal::Graphics::BindingDirection;
    using Portal::Graphics::detail::emit_glsl_kernel;

    namespace fields {

        using namespace MayaFlux::ShaderCompat;

        const auto push_up = MF_FIELD(push_up, [](vec3 p) -> vec3 {
            return p * 0.0f + vec3(0.0f, 1.0f, 0.0f);
        });

        const auto tint_red = MF_FIELD(tint_red, [](vec3 p) -> vec3 {
            return p * 0.0f + vec3(1.0f, 0.0f, 0.0f);
        });

        const auto glow_level = MF_FIELD(glow_level, [](vec3 p) -> float {
            return length(p) * 0.0f + 0.5f;
        });

        const auto uv_flat = MF_FIELD(uv_flat, [](vec3 p) -> vec2 {
            return vec2(p.x * 0.0f + 0.25f, 0.75f);
        });

        const auto pulse = MF_FIELD_T(pulse, [](vec3 p, float t) -> vec3 {
            return p * sin(t);
        });

        const auto steer = MF_FIELD_P(steer, [](vec3 p, float t, vec4 a) -> vec3 {
            return p * a.x * t;
        });

        const auto clash_a = MF_FIELD(clash, [](vec3 p) -> vec3 {
            return p;
        });

        const auto clash_b = MF_FIELD(clash, [](vec3 p) -> vec3 {
            return -p;
        });

        const auto no_return_type = MF_FIELD(no_return_type, [](vec3 p) {
            return p;
        });

    } // namespace fields

    Kakshya::VertexLayout points_layout()
    {
        return Kakshya::VertexLayout::for_points();
    }

    Kakshya::VertexLayout layout_without(Kakshya::DataModality missing)
    {
        auto layout = Kakshya::VertexLayout::for_points();
        std::erase_if(layout.attributes, [missing](const Kakshya::VertexAttributeLayout& a) {
            return a.component_modality == missing;
        });
        return layout;
    }

    std::string glsl_of(const GpuFieldOperator& op)
    {
        const auto spec = op.build_spec();
        EXPECT_TRUE(spec.has_value());
        if (!spec.has_value()) {
            return {};
        }
        return emit_glsl_kernel(*spec);
    }

    size_t occurrences(const std::string& text, std::string_view needle)
    {
        size_t count = 0;
        for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) {
            ++count;
        }
        return count;
    }

    ::testing::AssertionResult has(const std::string& text, std::string_view needle)
    {
        if (text.find(needle) != std::string::npos) {
            return ::testing::AssertionSuccess();
        }
        return ::testing::AssertionFailure() << "missing: " << needle << "\n---- emitted ----\n"
                                             << text;
    }

    ::testing::AssertionResult lacks(const std::string& text, std::string_view needle)
    {
        if (text.find(needle) == std::string::npos) {
            return ::testing::AssertionSuccess();
        }
        return ::testing::AssertionFailure() << "unexpected: " << needle << "\n---- emitted ----\n"
                                             << text;
    }

    class GpuFieldOperatorTest : public ::testing::Test {
    protected:
        GpuFieldOperator op { points_layout() };
    };

    TEST_F(GpuFieldOperatorTest, StartsWithoutBindingsOrSpec)
    {
        EXPECT_EQ(op.binding_count(), 0U);
        EXPECT_FALSE(op.build_spec().has_value());
        EXPECT_FALSE(op.needs_cluster_id());
    }

    TEST_F(GpuFieldOperatorTest, ReportsStrideInWords)
    {
        EXPECT_EQ(op.query_state("stride_words"), 17.0);
    }

    TEST_F(GpuFieldOperatorTest, ReportsBindingCountThroughQueryState)
    {
        op.bind(FieldTarget::POSITION, fields::push_up);

        EXPECT_EQ(op.query_state("binding_count"), 1.0);
    }

    TEST_F(GpuFieldOperatorTest, UnknownQueryYieldsNothing)
    {
        EXPECT_FALSE(op.query_state("no_such_state").has_value());
    }

    TEST_F(GpuFieldOperatorTest, UnknownParameterAndProcessAreInert)
    {
        EXPECT_NO_THROW(op.set_parameter("no_such_parameter", 1.0));
        EXPECT_NO_THROW(op.process(0.016F));
        EXPECT_EQ(op.binding_count(), 0U);
    }

    TEST_F(GpuFieldOperatorTest, DemandsFullVertexRestoreOnlyWhileBound)
    {
        EXPECT_FALSE(op.demands_full_vertex_restore());

        op.bind(FieldTarget::POSITION, fields::push_up);
        EXPECT_TRUE(op.demands_full_vertex_restore());

        op.unbind(FieldTarget::POSITION);
        EXPECT_FALSE(op.demands_full_vertex_restore());
    }

    class GpuFieldInertLayoutTest : public ::testing::TestWithParam<uint32_t> { };

    TEST_P(GpuFieldInertLayoutTest, RejectsEveryBindAndBuildsNoSpec)
    {
        auto layout = Kakshya::VertexLayout::for_points();
        layout.stride_bytes = GetParam();
        GpuFieldOperator op(layout);
        const auto revision = op.revision();

        op.bind(FieldTarget::POSITION, fields::push_up);

        EXPECT_EQ(op.binding_count(), 0U);
        EXPECT_EQ(op.revision(), revision);
        EXPECT_FALSE(op.build_spec().has_value());
        EXPECT_EQ(op.query_state("stride_words"), 0.0);
    }

    INSTANTIATE_TEST_SUITE_P(
        GpuFieldOperators,
        GpuFieldInertLayoutTest,
        ::testing::Values(0U, 61U, 62U, 63U));

    TEST_F(GpuFieldOperatorTest, BindingAVectorFieldRecordsItAndBumpsRevision)
    {
        const auto before = op.revision();

        op.bind(FieldTarget::POSITION, fields::push_up);

        EXPECT_EQ(op.binding_count(), 1U);
        EXPECT_GT(op.revision(), before);
        EXPECT_TRUE(op.build_spec().has_value());
    }

    TEST_F(GpuFieldOperatorTest, OneFieldOnSeveralTargetsIsOneBinding)
    {
        op.bind(FieldTarget::POSITION | FieldTarget::COLOR, fields::push_up);

        EXPECT_EQ(op.binding_count(), 1U);
    }

    TEST_F(GpuFieldOperatorTest, VectorFieldCannotReachScalarOrUvTargets)
    {
        const auto before = op.revision();

        op.bind(FieldTarget::SCALAR, fields::push_up);
        op.bind(FieldTarget::UV, fields::push_up);
        op.bind(FieldTarget::POSITION | FieldTarget::SCALAR, fields::push_up);

        EXPECT_EQ(op.binding_count(), 0U);
        EXPECT_EQ(op.revision(), before);
    }

    TEST_F(GpuFieldOperatorTest, ScalarFieldBindsOnlyToScalar)
    {
        op.bind(FieldTarget::POSITION, fields::glow_level);
        op.bind(FieldTarget::COLOR | FieldTarget::SCALAR, fields::glow_level);
        EXPECT_EQ(op.binding_count(), 0U);

        op.bind(FieldTarget::SCALAR, fields::glow_level);
        EXPECT_EQ(op.binding_count(), 1U);
    }

    TEST_F(GpuFieldOperatorTest, UvFieldBindsOnlyToUv)
    {
        op.bind(FieldTarget::POSITION, fields::uv_flat);
        EXPECT_EQ(op.binding_count(), 0U);

        op.bind(FieldTarget::UV, fields::uv_flat);
        EXPECT_EQ(op.binding_count(), 1U);
    }

    TEST_F(GpuFieldOperatorTest, EmptyTargetMaskIsRejected)
    {
        op.bind(FieldTarget {}, fields::push_up);

        EXPECT_EQ(op.binding_count(), 0U);
    }

    TEST_F(GpuFieldOperatorTest, FieldWithoutShaderHalfIsRejected)
    {
        ASSERT_FALSE(fields::no_return_type.has_source());

        op.bind(FieldTarget::POSITION, fields::no_return_type);

        EXPECT_EQ(op.binding_count(), 0U);
    }

    TEST(GpuFieldLayoutTest, TargetMissingFromLayoutIsRejected)
    {
        GpuFieldOperator op(layout_without(Kakshya::DataModality::VERTEX_COLORS_RGBA));

        op.bind(FieldTarget::COLOR, fields::tint_red);
        EXPECT_EQ(op.binding_count(), 0U);

        op.bind(FieldTarget::POSITION, fields::push_up);
        EXPECT_EQ(op.binding_count(), 1U);
    }

    TEST(GpuFieldLayoutTest, LayoutWithoutPositionBuildsNoSpec)
    {
        GpuFieldOperator op(layout_without(Kakshya::DataModality::VERTEX_POSITIONS_3D));

        op.bind(FieldTarget::COLOR, fields::tint_red);

        EXPECT_EQ(op.binding_count(), 1U);
        EXPECT_FALSE(op.build_spec().has_value());
    }

    TEST_F(GpuFieldOperatorTest, DifferentBodiesUnderOneNameAreRejected)
    {
        op.bind(FieldTarget::POSITION, fields::clash_a);
        op.bind(FieldTarget::COLOR, fields::clash_b);

        EXPECT_EQ(op.binding_count(), 1U);
    }

    TEST_F(GpuFieldOperatorTest, SameFieldOnTwoTargetsEmitsItsFunctionOnce)
    {
        op.bind(FieldTarget::POSITION, fields::push_up);
        op.bind(FieldTarget::NORMAL, fields::push_up);
        ASSERT_EQ(op.binding_count(), 2U);

        const auto glsl = glsl_of(op);

        EXPECT_EQ(occurrences(glsl, "vec3 push_up("), 1U);
        EXPECT_TRUE(has(glsl, "acc_position += push_up(p);"));
        EXPECT_TRUE(has(glsl, "acc_normal += push_up(p);"));
    }

    TEST_F(GpuFieldOperatorTest, EmittedShaderDeclaresVerticesAndPushConstants)
    {
        op.bind(FieldTarget::POSITION, fields::push_up);

        const auto spec = op.build_spec();
        ASSERT_TRUE(spec.has_value());

        ASSERT_FALSE(spec->bindings.empty());
        EXPECT_EQ(spec->bindings[0].name, "vertices");
        EXPECT_EQ(spec->bindings[0].direction, BindingDirection::InOut);
        EXPECT_EQ(spec->bindings.size(), 1U);

        std::vector<std::string> names;
        for (const auto& f : spec->pc_fields) {
            names.push_back(f.name);
        }
        EXPECT_EQ(names, (std::vector<std::string> { "first_vertex", "vertex_count", "stride_words", "time", "geometry" }));

        const auto glsl = emit_glsl_kernel(*spec);
        EXPECT_TRUE(has(glsl, "buffer Block_vertices"));
        EXPECT_TRUE(has(glsl, "layout(push_constant) uniform PC"));
        EXPECT_TRUE(has(glsl, "if (i >= vertex_count) { return; }"));
        EXPECT_TRUE(has(glsl, "uint b = (first_vertex + i) * stride_words;"));
    }

    TEST_F(GpuFieldOperatorTest, PositionAccumulatesAndOtherTargetsOverwrite)
    {
        op.bind(FieldTarget::POSITION, fields::push_up);
        op.bind(FieldTarget::COLOR, fields::tint_red);

        const auto glsl = glsl_of(op);

        EXPECT_TRUE(has(glsl, "vertices[b + 0u] += acc_position.x;"));
        EXPECT_TRUE(has(glsl, "vertices[b + 1u] += acc_position.y;"));
        EXPECT_TRUE(has(glsl, "vertices[b + 2u] += acc_position.z;"));
        EXPECT_TRUE(has(glsl, "vertices[b + 3u] = acc_color.x;"));
        EXPECT_TRUE(has(glsl, "vertices[b + 5u] = acc_color.z;"));
    }

    TEST_F(GpuFieldOperatorTest, ScalarAndUvWriteAtTheirLayoutOffsets)
    {
        op.bind(FieldTarget::SCALAR, fields::glow_level);
        op.bind(FieldTarget::UV, fields::uv_flat);

        const auto glsl = glsl_of(op);

        EXPECT_TRUE(has(glsl, "vertices[b + 7u] = acc_scalar;"));
        EXPECT_TRUE(has(glsl, "vertices[b + 8u] = acc_uv.x;"));
        EXPECT_TRUE(has(glsl, "vertices[b + 9u] = acc_uv.y;"));
    }

    TEST_F(GpuFieldOperatorTest, UntouchedTargetsAreNotWritten)
    {
        op.bind(FieldTarget::COLOR, fields::tint_red);

        const auto glsl = glsl_of(op);

        EXPECT_TRUE(lacks(glsl, "acc_position"));
        EXPECT_TRUE(lacks(glsl, "acc_normal"));
        EXPECT_TRUE(lacks(glsl, "acc_uv"));
    }

    TEST_F(GpuFieldOperatorTest, PositionIsCarriedThroughTheGeometryMatrix)
    {
        op.bind(FieldTarget::POSITION, fields::push_up);

        const auto glsl = glsl_of(op);

        EXPECT_TRUE(has(glsl, "(geometry * vec4("));
        EXPECT_TRUE(has(glsl, "inverse(mat3(geometry))"));
    }

    TEST_F(GpuFieldOperatorTest, NormalsAreRenormalisedAfterTheNormalMatrix)
    {
        op.bind(FieldTarget::NORMAL, fields::push_up);

        const auto glsl = glsl_of(op);

        EXPECT_TRUE(has(glsl, "transpose(mat3(geometry))"));
        EXPECT_TRUE(has(glsl, "len_normal"));
    }

    TEST_F(GpuFieldOperatorTest, TemporalFieldIsCalledWithTime)
    {
        op.bind(FieldTarget::POSITION, fields::pulse);

        const auto glsl = glsl_of(op);

        EXPECT_TRUE(has(glsl, "acc_position += pulse(p, time);"));
        EXPECT_TRUE(lacks(glsl, "params"));
    }

    TEST_F(GpuFieldOperatorTest, ParametricFieldAddsAParamsBlock)
    {
        op.bind(FieldTarget::POSITION, fields::steer);

        const auto spec = op.build_spec();
        ASSERT_TRUE(spec.has_value());

        const auto has_params = std::ranges::any_of(spec->pc_fields,
            [](const auto& f) { return f.name == "params"; });
        EXPECT_TRUE(has_params);
        EXPECT_TRUE(has(emit_glsl_kernel(*spec), "acc_position += steer(p, time, params);"));
    }

    TEST_F(GpuFieldOperatorTest, ClusterScopedBindingDeclaresClusterBufferAndGuards)
    {
        op.bind(FieldTarget::POSITION, fields::push_up, 2U);
        op.bind(FieldTarget::COLOR, fields::tint_red);

        EXPECT_TRUE(op.needs_cluster_id());

        const auto spec = op.build_spec();
        ASSERT_TRUE(spec.has_value());
        ASSERT_EQ(spec->bindings.size(), 2U);
        EXPECT_EQ(spec->bindings[1].name, "cluster_id");
        EXPECT_EQ(spec->bindings[1].direction, BindingDirection::Input);

        const auto glsl = emit_glsl_kernel(*spec);
        EXPECT_TRUE(has(glsl, "uint my_cluster = cluster_id[first_vertex + i];"));
        EXPECT_TRUE(has(glsl, "if (my_cluster == 2u) { acc_position += push_up(p); }"));
        EXPECT_TRUE(has(glsl, "acc_color += tint_red(p);"));
    }

    TEST_F(GpuFieldOperatorTest, UnscopedBindingsNeedNoClusterBuffer)
    {
        op.bind(FieldTarget::POSITION, fields::push_up);

        EXPECT_FALSE(op.needs_cluster_id());
        EXPECT_TRUE(lacks(glsl_of(op), "cluster_id"));
    }

    TEST_F(GpuFieldOperatorTest, BuildSpecIsCachedUntilSomethingChanges)
    {
        op.bind(FieldTarget::POSITION, fields::push_up);
        const auto revision = op.revision();

        (void)op.build_spec();
        (void)op.build_spec();

        EXPECT_EQ(op.revision(), revision);
    }

    TEST_F(GpuFieldOperatorTest, UnbindingEveryTargetOfABindingRemovesIt)
    {
        op.bind(FieldTarget::POSITION, fields::push_up);
        const auto before = op.revision();

        op.unbind(FieldTarget::POSITION);

        EXPECT_EQ(op.binding_count(), 0U);
        EXPECT_GT(op.revision(), before);
        EXPECT_FALSE(op.build_spec().has_value());
    }

    TEST_F(GpuFieldOperatorTest, UnbindingOneTargetKeepsTheOthersOfASharedBinding)
    {
        op.bind(FieldTarget::POSITION | FieldTarget::COLOR, fields::push_up);

        op.unbind(FieldTarget::COLOR);

        ASSERT_EQ(op.binding_count(), 1U);
        const auto glsl = glsl_of(op);
        EXPECT_TRUE(has(glsl, "acc_position"));
        EXPECT_TRUE(lacks(glsl, "acc_color"));
    }

    TEST_F(GpuFieldOperatorTest, WorkgroupSizeReachesTheSpecAndBumpsRevisionOnlyOnChange)
    {
        op.bind(FieldTarget::POSITION, fields::push_up);
        EXPECT_EQ(op.build_spec()->workgroup_size[0], 256U);

        const auto before = op.revision();
        op.set_workgroup_size(64);
        EXPECT_EQ(op.build_spec()->workgroup_size[0], 64U);
        EXPECT_GT(op.revision(), before);

        const auto after = op.revision();
        op.set_workgroup_size(64);
        op.set_workgroup_size(0);
        EXPECT_EQ(op.revision(), after);
        EXPECT_EQ(op.build_spec()->workgroup_size[0], 64U);
    }

    TEST_F(GpuFieldOperatorTest, VertexBindingIndexReachesTheSpecAndBumpsRevisionOnlyOnChange)
    {
        op.bind(FieldTarget::POSITION, fields::push_up);

        const auto before = op.revision();
        op.set_vertex_binding(3);
        EXPECT_EQ(op.get_vertex_binding(), 3U);
        EXPECT_EQ(op.build_spec()->bindings[0].binding_index, 3U);
        EXPECT_GT(op.revision(), before);

        const auto after = op.revision();
        op.set_vertex_binding(3);
        EXPECT_EQ(op.revision(), after);
    }

    TEST(GpuFieldConfigTest, ConstructorConfigIsKept)
    {
        SpatialFieldConfig config;
        config.capture_growth = 0.75F;

        GpuFieldOperator op(points_layout(), config);

        EXPECT_EQ(op.get_field_config().capture_growth, 0.75F);
    }

    struct ConfigSetter {
        const char* name;
        void (GpuFieldOperator::*set)(float);
        std::function<float(const SpatialFieldConfig&)> read;
    };

    class GpuFieldConfigSetterTest : public ::testing::TestWithParam<ConfigSetter> { };

    TEST_P(GpuFieldConfigSetterTest, StoresTheValueAndBumpsRevision)
    {
        GpuFieldOperator op(points_layout());
        const auto before = op.revision();

        (op.*GetParam().set)(3.5F);

        EXPECT_EQ(GetParam().read(op.get_field_config()), 3.5F);
        EXPECT_GT(op.revision(), before);
    }

    TEST_P(GpuFieldConfigSetterTest, InvalidatesTheCachedSpec)
    {
        GpuFieldOperator op(points_layout());
        op.bind(FieldTarget::POSITION, fields::push_up);
        (void)op.build_spec();
        const auto before = op.revision();

        (op.*GetParam().set)(1.25F);

        EXPECT_GT(op.revision(), before);
        EXPECT_TRUE(op.build_spec().has_value());
    }

    INSTANTIATE_TEST_SUITE_P(
        GpuFieldOperators,
        GpuFieldConfigSetterTest,
        ::testing::Values(
            ConfigSetter { "DensitySaturationCount", &GpuFieldOperator::set_density_saturation_count,
                [](const SpatialFieldConfig& c) { return static_cast<float>(c.density_saturation_count); } },
            ConfigSetter { "CaptureGrowth", &GpuFieldOperator::set_capture_growth,
                [](const SpatialFieldConfig& c) { return static_cast<float>(c.capture_growth); } },
            ConfigSetter { "SwallowBaseSize", &GpuFieldOperator::set_swallow_base_size,
                [](const SpatialFieldConfig& c) { return static_cast<float>(c.swallow_base_size); } },
            ConfigSetter { "SwallowGrowthRate", &GpuFieldOperator::set_swallow_growth_rate,
                [](const SpatialFieldConfig& c) { return static_cast<float>(c.swallow_growth_rate); } },
            ConfigSetter { "SwallowMaxSize", &GpuFieldOperator::set_swallow_max_size,
                [](const SpatialFieldConfig& c) { return static_cast<float>(c.swallow_max_size); } },
            ConfigSetter { "SwallowDimFactor", &GpuFieldOperator::set_swallow_dim_factor,
                [](const SpatialFieldConfig& c) { return static_cast<float>(c.swallow_dim_factor); } },
            ConfigSetter { "SpawnDensityThreshold", &GpuFieldOperator::set_spawn_density_threshold,
                [](const SpatialFieldConfig& c) { return static_cast<float>(c.spawn_density_threshold); } }),
        [](const ::testing::TestParamInfo<ConfigSetter>& info) { return std::string(info.param.name); });

    TEST(GpuFieldConfigTest, CrossClusterToggleStoresAndBumpsRevision)
    {
        GpuFieldOperator op(points_layout());
        const auto before = op.revision();

        op.set_cross_cluster(true);

        EXPECT_TRUE(op.get_field_config().cross_cluster);
        EXPECT_GT(op.revision(), before);

        op.set_cross_cluster(false);
        EXPECT_FALSE(op.get_field_config().cross_cluster);
    }

} // namespace

} // namespace MayaFlux::Test
