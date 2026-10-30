#include "gtest/gtest.h"

#include "MayaFlux/Kakshya/NDData/VertexAccess.hpp"
#include "MayaFlux/Kakshya/NDData/VertexFormats.hpp"
#include "MayaFlux/Kakshya/NDData/VertexInsertion.hpp"

namespace MayaFlux::Test {
namespace {

    using Kakshya::DataModality;
    using Kakshya::DataVariant;
    using Kakshya::LineVertex;
    using Kakshya::MeshVertex;
    using Kakshya::PointVertex;
    using Kakshya::Vertex;
    using Kakshya::VertexAccess;
    using Kakshya::VertexAccessConfig;
    using Kakshya::VertexInsertion;
    using Kakshya::VertexLayout;

    constexpr size_t k_record = 68;

    float read_float(const void* base, size_t offset)
    {
        float value {};
        std::memcpy(&value, static_cast<const std::byte*>(base) + offset, sizeof(float));
        return value;
    }

    template <typename Vec>
    Vec read_vec(const void* base, size_t offset)
    {
        Vec value {};
        std::memcpy(&value, static_cast<const std::byte*>(base) + offset, sizeof(Vec));
        return value;
    }

    std::vector<uint8_t> bytes_of(const std::vector<MeshVertex>& vertices)
    {
        std::vector<uint8_t> bytes(vertices.size() * sizeof(MeshVertex));
        std::memcpy(bytes.data(), vertices.data(), bytes.size());
        return bytes;
    }

    std::vector<MeshVertex> sample_vertices()
    {
        return {
            { .position = { 1, 2, 3 }, .color = { 0.1F, 0.2F, 0.3F, 0.4F }, .weight = 0.5F, .uv = { 0.25F, 0.75F }, .normal = { 0, 1, 0 }, .tangent = { 0, 0, 1, -1 } },
            { .position = { 4, 5, 6 }, .color = { 0.5F, 0.6F, 0.7F, 0.8F }, .weight = 0.9F, .uv = { 0.5F, 0.125F }, .normal = { 1, 0, 0 }, .tangent = { 0, 1, 0, 1 } },
        };
    }

    // ---------------------------------------------------------------------
    // VertexLayout
    // ---------------------------------------------------------------------

    struct LayoutCase {
        std::string_view name;
        VertexLayout layout;
        std::string_view scalar_name;
    };

    class VertexLayoutFactoryTest : public ::testing::TestWithParam<LayoutCase> { };

    TEST_P(VertexLayoutFactoryTest, FollowsTheCanonicalOffsetTable)
    {
        const VertexLayout& layout = GetParam().layout;

        EXPECT_EQ(layout.stride_bytes, 68U);
        ASSERT_EQ(layout.attributes.size(), 6U);

        EXPECT_EQ(layout.attributes[0].component_modality, DataModality::VERTEX_POSITIONS_3D);
        EXPECT_EQ(layout.attributes[0].offset_in_vertex, 0U);
        EXPECT_EQ(layout.attributes[1].component_modality, DataModality::VERTEX_COLORS_RGBA);
        EXPECT_EQ(layout.attributes[1].offset_in_vertex, 12U);
        EXPECT_EQ(layout.attributes[2].component_modality, DataModality::SCALAR_F32);
        EXPECT_EQ(layout.attributes[2].offset_in_vertex, 28U);
        EXPECT_EQ(layout.attributes[2].name, GetParam().scalar_name);
        EXPECT_EQ(layout.attributes[3].component_modality, DataModality::TEXTURE_COORDS_2D);
        EXPECT_EQ(layout.attributes[3].offset_in_vertex, 32U);
        EXPECT_EQ(layout.attributes[4].component_modality, DataModality::VERTEX_NORMALS_3D);
        EXPECT_EQ(layout.attributes[4].offset_in_vertex, 40U);
        EXPECT_EQ(layout.attributes[5].component_modality, DataModality::VERTEX_TANGENTS_4D);
        EXPECT_EQ(layout.attributes[5].offset_in_vertex, 52U);
    }

    TEST_P(VertexLayoutFactoryTest, StartsWithoutVertices)
    {
        EXPECT_EQ(GetParam().layout.vertex_count, 0U);
    }

    TEST_P(VertexLayoutFactoryTest, LastAttributeEndsExactlyAtTheStride)
    {
        const VertexLayout& layout = GetParam().layout;

        EXPECT_EQ(layout.attributes.back().offset_in_vertex + sizeof(glm::vec4), layout.stride_bytes);
    }

    INSTANTIATE_TEST_SUITE_P(Factories, VertexLayoutFactoryTest,
        ::testing::Values(
            LayoutCase { "Points", VertexLayout::for_points(), "size" },
            LayoutCase { "Lines", VertexLayout::for_lines(), "thickness" },
            LayoutCase { "Meshes", VertexLayout::for_meshes(), "weight" },
            LayoutCase { "Raw", VertexLayout::for_raw(), "scalar" }),
        [](const ::testing::TestParamInfo<LayoutCase>& info) {
            return std::string(info.param.name);
        });

    TEST(VertexLayoutTest, FactoriesAcceptAStrideOverride)
    {
        EXPECT_EQ(VertexLayout::for_points(96).stride_bytes, 96U);
        EXPECT_EQ(VertexLayout::for_lines(96).stride_bytes, 96U);
        EXPECT_EQ(VertexLayout::for_meshes(96).stride_bytes, 96U);
        EXPECT_EQ(VertexLayout::for_raw(96).stride_bytes, 96U);
    }

    TEST(VertexLayoutTest, TexturedQuadIsPositionThenTexcoord)
    {
        const VertexLayout layout = VertexLayout::for_textured_quad();

        EXPECT_EQ(layout.vertex_count, 4U);
        EXPECT_EQ(layout.stride_bytes, 20U);
        ASSERT_EQ(layout.attributes.size(), 2U);
        EXPECT_EQ(layout.attributes[0].component_modality, DataModality::VERTEX_POSITIONS_3D);
        EXPECT_EQ(layout.attributes[0].offset_in_vertex, 0U);
        EXPECT_EQ(layout.attributes[1].component_modality, DataModality::TEXTURE_COORDS_2D);
        EXPECT_EQ(layout.attributes[1].offset_in_vertex, 12U);
        EXPECT_EQ(layout.attributes[1].name, "texcoord");
    }

    TEST(VertexLayoutTest, TexturedQuadTakesTheVertexCount)
    {
        EXPECT_EQ(VertexLayout::for_textured_quad(6).vertex_count, 6U);
    }

    TEST(VertexLayoutTest, WordOffsetIsTheByteOffsetOverFour)
    {
        const VertexLayout layout = VertexLayout::for_meshes();

        EXPECT_EQ(layout.find_word_offset(DataModality::VERTEX_POSITIONS_3D), 0U);
        EXPECT_EQ(layout.find_word_offset(DataModality::VERTEX_COLORS_RGBA), 3U);
        EXPECT_EQ(layout.find_word_offset(DataModality::TEXTURE_COORDS_2D), 8U);
        EXPECT_EQ(layout.find_word_offset(DataModality::VERTEX_NORMALS_3D), 10U);
    }

    TEST(VertexLayoutTest, WordOffsetIsAbsentForMissingModalities)
    {
        const VertexLayout layout = VertexLayout::for_textured_quad();

        EXPECT_FALSE(layout.find_word_offset(DataModality::VERTEX_NORMALS_3D).has_value());
    }

    TEST(VertexLayoutTest, WordOffsetIsAbsentWhenTheOffsetIsNotWordAligned)
    {
        VertexLayout layout;
        layout.stride_bytes = 16;
        layout.attributes.push_back({ .component_modality = DataModality::VERTEX_POSITIONS_3D,
            .offset_in_vertex = 2,
            .name = "position" });

        EXPECT_FALSE(layout.find_word_offset(DataModality::VERTEX_POSITIONS_3D).has_value());
    }

    TEST(VertexLayoutTest, WordOffsetTakesTheFirstMatchingAttribute)
    {
        VertexLayout layout;
        layout.stride_bytes = 32;
        layout.attributes.push_back({ .component_modality = DataModality::VERTEX_COLORS_RGB, .offset_in_vertex = 8, .name = "a" });
        layout.attributes.push_back({ .component_modality = DataModality::VERTEX_COLORS_RGB, .offset_in_vertex = 16, .name = "b" });

        EXPECT_EQ(layout.find_word_offset(DataModality::VERTEX_COLORS_RGB), 2U);
    }

    TEST(VertexLayoutTest, ComputeStrideEndsAtTheLastAttribute)
    {
        VertexLayout layout;
        layout.attributes.push_back({ .component_modality = DataModality::VERTEX_POSITIONS_3D, .offset_in_vertex = 0, .name = "position" });
        layout.attributes.push_back({ .component_modality = DataModality::TEXTURE_COORDS_2D, .offset_in_vertex = 12, .name = "uv" });

        layout.compute_stride();

        EXPECT_EQ(layout.stride_bytes, 20U);
    }

    TEST(VertexLayoutTest, ComputeStrideDoesNotDependOnAttributeOrder)
    {
        VertexLayout layout;
        layout.attributes.push_back({ .component_modality = DataModality::VERTEX_COLORS_RGBA, .offset_in_vertex = 12, .name = "color" });
        layout.attributes.push_back({ .component_modality = DataModality::VERTEX_POSITIONS_3D, .offset_in_vertex = 0, .name = "position" });

        layout.compute_stride();

        EXPECT_EQ(layout.stride_bytes, 28U);
    }

    TEST(VertexLayoutTest, ComputeStrideSizesUnlistedModalitiesAsOneWord)
    {
        VertexLayout layout;
        layout.attributes.push_back({ .component_modality = DataModality::SCALAR_F32, .offset_in_vertex = 8, .name = "scalar" });

        layout.compute_stride();

        EXPECT_EQ(layout.stride_bytes, 12U);
    }

    TEST(VertexLayoutTest, ComputeStrideKeepsAnExplicitStride)
    {
        VertexLayout layout = VertexLayout::for_meshes(96);

        layout.compute_stride();

        EXPECT_EQ(layout.stride_bytes, 96U);
    }

    TEST(VertexLayoutTest, ComputeStrideNeedsAttributes)
    {
        VertexLayout layout;

        layout.compute_stride();

        EXPECT_EQ(layout.stride_bytes, 0U);
    }

    // ---------------------------------------------------------------------
    // VertexFormats
    // ---------------------------------------------------------------------

    TEST(VertexFormatsTest, VertexTypesShareTheCanonicalSize)
    {
        EXPECT_EQ(sizeof(Vertex), 68U);
        EXPECT_EQ(sizeof(PointVertex), 68U);
        EXPECT_EQ(sizeof(LineVertex), 68U);
        EXPECT_EQ(sizeof(MeshVertex), 68U);
    }

    TEST(VertexFormatsTest, LayoutIsDeducedFromTheVertexType)
    {
        EXPECT_EQ(Kakshya::vertex_layout_for<PointVertex>().attributes[2].name, "size");
        EXPECT_EQ(Kakshya::vertex_layout_for<LineVertex>().attributes[2].name, "thickness");
        EXPECT_EQ(Kakshya::vertex_layout_for<MeshVertex>().attributes[2].name, "weight");
        EXPECT_EQ(Kakshya::vertex_layout_for<Vertex>().stride_bytes, sizeof(Vertex));
    }

    TEST(VertexFormatsTest, DeducedLayoutsCoverEveryAttributeOfTheirType)
    {
        EXPECT_EQ(Kakshya::vertex_layout_for<PointVertex>().attributes.size(), 6U);
        EXPECT_EQ(Kakshya::vertex_layout_for<LineVertex>().attributes.size(), 6U);
        EXPECT_EQ(Kakshya::vertex_layout_for<MeshVertex>().attributes.size(), 6U);
        EXPECT_EQ(Kakshya::vertex_layout_for<Vertex>().attributes.size(), 6U);
    }

    TEST(VertexFormatsTest, DefaultVertexIsWhiteOpaqueZUpWithXTangent)
    {
        const Vertex v {};

        EXPECT_EQ(v.color, glm::vec4(1.0F));
        EXPECT_FLOAT_EQ(v.scalar, 1.0F);
        EXPECT_EQ(v.uv, glm::vec2(0.0F));
        EXPECT_EQ(v.normal, glm::vec3(0, 0, 1));
        EXPECT_EQ(v.tangent, glm::vec4(1, 0, 0, 1));
    }

    TEST(VertexFormatsTest, PointProjectionMapsTheScalarOntoTheSizeRange)
    {
        Vertex v {};
        v.position = { 1, 2, 3 };
        v.color = { 0.1F, 0.2F, 0.3F, 0.4F };
        v.normal = { 0, 1, 0 };
        v.tangent = { 0, 0, 1, -1 };

        v.scalar = 0.0F;
        EXPECT_FLOAT_EQ(Kakshya::to_point_vertex(v, { 4.0F, 10.0F }).size, 4.0F);
        v.scalar = 0.5F;
        const PointVertex mid = Kakshya::to_point_vertex(v, { 4.0F, 10.0F });
        EXPECT_FLOAT_EQ(mid.size, 7.0F);
        v.scalar = 1.0F;
        EXPECT_FLOAT_EQ(Kakshya::to_point_vertex(v, { 4.0F, 10.0F }).size, 10.0F);

        EXPECT_EQ(mid.position, v.position);
        EXPECT_EQ(mid.color, v.color);
        EXPECT_EQ(mid.normal, v.normal);
        EXPECT_EQ(mid.tangent, v.tangent);
    }

    TEST(VertexFormatsTest, PointProjectionHasADefaultSizeRange)
    {
        Vertex v {};
        v.scalar = 0.5F;

        EXPECT_FLOAT_EQ(Kakshya::to_point_vertex(v).size, 10.0F);
    }

    TEST(VertexFormatsTest, LineProjectionMapsTheScalarOntoTheThicknessRange)
    {
        Vertex v {};
        v.position = { 1, 2, 3 };
        v.scalar = 0.25F;

        const LineVertex line = Kakshya::to_line_vertex(v, { 2.0F, 6.0F });

        EXPECT_FLOAT_EQ(line.thickness, 3.0F);
        EXPECT_EQ(line.position, v.position);
        EXPECT_EQ(line.tangent, v.tangent);
    }

    TEST(VertexFormatsTest, LineProjectionHasADefaultThicknessRange)
    {
        Vertex v {};
        v.scalar = 0.5F;

        EXPECT_FLOAT_EQ(Kakshya::to_line_vertex(v).thickness, 1.5F);
    }

    TEST(VertexFormatsTest, MeshProjectionMapsTheScalarOntoTheWeightRange)
    {
        Vertex v {};
        v.position = { 1, 2, 3 };
        v.scalar = 0.5F;

        const MeshVertex mesh = Kakshya::to_mesh_vertex(v, { -1.0F, 1.0F });

        EXPECT_FLOAT_EQ(mesh.weight, 0.0F);
        EXPECT_EQ(mesh.position, v.position);
        EXPECT_EQ(mesh.normal, v.normal);
    }

    TEST(VertexFormatsTest, MeshProjectionDefaultsToTheUnitRange)
    {
        Vertex v {};
        v.scalar = 0.75F;

        EXPECT_FLOAT_EQ(Kakshya::to_mesh_vertex(v).weight, 0.75F);
    }

    TEST(VertexFormatsTest, BatchProjectionsKeepOrderAndLength)
    {
        std::vector<Vertex> vertices(3);
        for (size_t i = 0; i < vertices.size(); ++i) {
            vertices[i].position = { static_cast<float>(i), 0, 0 };
            vertices[i].scalar = static_cast<float>(i) / 2.0F;
        }

        const auto points = Kakshya::to_point_vertices(vertices, { 0.0F, 2.0F });
        const auto lines = Kakshya::to_line_vertices(vertices, { 0.0F, 2.0F });
        const auto meshes = Kakshya::to_mesh_vertices(vertices, { 0.0F, 2.0F });

        ASSERT_EQ(points.size(), 3U);
        ASSERT_EQ(lines.size(), 3U);
        ASSERT_EQ(meshes.size(), 3U);
        for (size_t i = 0; i < 3; ++i) {
            const float expected = static_cast<float>(i);
            EXPECT_FLOAT_EQ(points[i].position.x, expected);
            EXPECT_FLOAT_EQ(points[i].size, expected);
            EXPECT_FLOAT_EQ(lines[i].thickness, expected);
            EXPECT_FLOAT_EQ(meshes[i].weight, expected);
        }
    }

    TEST(VertexFormatsTest, BatchProjectionOfNothingIsEmpty)
    {
        EXPECT_TRUE(Kakshya::to_point_vertices({}).empty());
        EXPECT_TRUE(Kakshya::to_line_vertices({}).empty());
        EXPECT_TRUE(Kakshya::to_mesh_vertices({}).empty());
    }

    // ---------------------------------------------------------------------
    // VertexAccess
    // ---------------------------------------------------------------------

    using AccessFn = std::optional<VertexAccess> (*)(std::span<const DataVariant>, const VertexAccessConfig&);

    struct AccessCase {
        std::string_view name;
        AccessFn fn;
        std::string_view scalar_name;
        float default_scalar;
    };

    class VertexAccessTest : public ::testing::TestWithParam<AccessCase> {
    protected:
        [[nodiscard]] std::optional<VertexAccess> run(const std::vector<DataVariant>& channels, const VertexAccessConfig& config = {}) const
        {
            return GetParam().fn(channels, config);
        }
    };

    TEST_P(VertexAccessTest, NoChannelsGivesNothing)
    {
        EXPECT_FALSE(run({}).has_value());
    }

    TEST_P(VertexAccessTest, EmptyPositionChannelGivesNothing)
    {
        EXPECT_FALSE(run({ std::vector<glm::vec3> {} }).has_value());
    }

    TEST_P(VertexAccessTest, InterleavedBytesPassThroughZeroCopy)
    {
        const std::vector<DataVariant> channels { std::vector<uint8_t>(3 * k_record, 9) };

        const auto access = run(channels);

        ASSERT_TRUE(access.has_value());
        EXPECT_EQ(access->data_ptr, std::get<std::vector<uint8_t>>(channels[0]).data());
        EXPECT_EQ(access->byte_count, 3 * k_record);
        EXPECT_FALSE(access->was_converted());
        EXPECT_EQ(access->layout.vertex_count, 3U);
        EXPECT_EQ(access->layout.stride_bytes, k_record);
        EXPECT_EQ(access->layout.attributes[2].name, GetParam().scalar_name);
    }

    TEST_P(VertexAccessTest, InterleavedBytesMustBeWholeRecords)
    {
        const std::vector<DataVariant> channels { std::vector<uint8_t>(k_record + 1, 0) };

        EXPECT_FALSE(run(channels).has_value());
    }

    TEST_P(VertexAccessTest, PositionsAreAssembledIntoWholeRecords)
    {
        const std::vector<DataVariant> channels { std::vector<glm::vec3> { { 1, 2, 3 }, { 4, 5, 6 } } };

        const auto access = run(channels);

        ASSERT_TRUE(access.has_value());
        EXPECT_TRUE(access->was_converted());
        EXPECT_EQ(access->byte_count, 2 * k_record);
        EXPECT_EQ(access->layout.vertex_count, 2U);
        EXPECT_EQ(access->layout.stride_bytes, k_record);
        EXPECT_EQ(access->data_ptr, access->conversion_buffer.data());
        EXPECT_EQ(read_vec<glm::vec3>(access->data_ptr, 0), glm::vec3(1, 2, 3));
        EXPECT_EQ(read_vec<glm::vec3>(access->data_ptr, k_record), glm::vec3(4, 5, 6));
    }

    TEST_P(VertexAccessTest, AbsentChannelsTakeTheConfiguredDefaults)
    {
        const std::vector<DataVariant> channels { std::vector<glm::vec3> { { 1, 2, 3 }, { 4, 5, 6 } } };
        VertexAccessConfig config;
        config.default_color = { 0.1F, 0.2F, 0.3F, 0.4F };
        config.default_uv = { 0.5F, 0.6F };
        config.default_normal = { 0, 1, 0 };
        config.default_tangent = { 0, 0, 1 };

        const auto access = run(channels, config);

        ASSERT_TRUE(access.has_value());
        for (size_t i = 0; i < 2; ++i) {
            const size_t base = i * k_record;
            EXPECT_EQ(read_vec<glm::vec4>(access->data_ptr, base + 12), glm::vec4(0.1F, 0.2F, 0.3F, 0.4F));
            EXPECT_FLOAT_EQ(read_float(access->data_ptr, base + 28), GetParam().default_scalar);
            EXPECT_EQ(read_vec<glm::vec2>(access->data_ptr, base + 32), glm::vec2(0.5F, 0.6F));
            EXPECT_EQ(read_vec<glm::vec3>(access->data_ptr, base + 40), glm::vec3(0, 1, 0));
            EXPECT_EQ(read_vec<glm::vec3>(access->data_ptr, base + 52), glm::vec3(0, 0, 1));
        }
    }

    TEST_P(VertexAccessTest, DefaultsAreWhiteZUpAndXTangent)
    {
        const std::vector<DataVariant> channels { std::vector<glm::vec3> { { 1, 2, 3 } } };

        const auto access = run(channels);

        ASSERT_TRUE(access.has_value());
        EXPECT_EQ(read_vec<glm::vec4>(access->data_ptr, 12), glm::vec4(1.0F));
        EXPECT_EQ(read_vec<glm::vec2>(access->data_ptr, 32), glm::vec2(0.0F));
        EXPECT_EQ(read_vec<glm::vec3>(access->data_ptr, 40), glm::vec3(0, 0, 1));
        EXPECT_EQ(read_vec<glm::vec3>(access->data_ptr, 52), glm::vec3(1, 0, 0));
    }

    TEST_P(VertexAccessTest, SuppliedChannelsOverrideTheDefaults)
    {
        const std::vector<DataVariant> channels {
            std::vector<glm::vec3> { { 1, 2, 3 }, { 4, 5, 6 } },
            std::vector<glm::vec4> { { 1, 0, 0, 1 }, { 0, 1, 0, 0.5F } },
            std::vector<float> { 0.25F, 0.75F },
            std::vector<glm::vec2> { { 0, 0 }, { 1, 1 } },
            std::vector<glm::vec3> { { 0, 1, 0 }, { 0, 0, 1 } },
            std::vector<glm::vec4> { { 1, 0, 0, -1 }, { 0, 1, 0, 1 } },
        };

        const auto access = run(channels);

        ASSERT_TRUE(access.has_value());
        EXPECT_EQ(read_vec<glm::vec4>(access->data_ptr, 12), glm::vec4(1, 0, 0, 1));
        EXPECT_EQ(read_vec<glm::vec4>(access->data_ptr, k_record + 12), glm::vec4(0, 1, 0, 0.5F));
        EXPECT_FLOAT_EQ(read_float(access->data_ptr, 28), 0.25F);
        EXPECT_FLOAT_EQ(read_float(access->data_ptr, k_record + 28), 0.75F);
        EXPECT_EQ(read_vec<glm::vec2>(access->data_ptr, k_record + 32), glm::vec2(1, 1));
        EXPECT_EQ(read_vec<glm::vec3>(access->data_ptr, 40), glm::vec3(0, 1, 0));
        EXPECT_EQ(read_vec<glm::vec3>(access->data_ptr, k_record + 40), glm::vec3(0, 0, 1));
        EXPECT_EQ(read_vec<glm::vec4>(access->data_ptr, 52), glm::vec4(1, 0, 0, -1));
        EXPECT_EQ(read_vec<glm::vec4>(access->data_ptr, k_record + 52), glm::vec4(0, 1, 0, 1));
    }

    TEST_P(VertexAccessTest, ThreeComponentColorGainsAnOpaqueAlpha)
    {
        const std::vector<DataVariant> channels {
            std::vector<glm::vec3> { { 1, 2, 3 } },
            std::vector<glm::vec3> { { 0.2F, 0.4F, 0.6F } },
        };

        const auto access = run(channels);

        ASSERT_TRUE(access.has_value());
        EXPECT_EQ(read_vec<glm::vec4>(access->data_ptr, 12), glm::vec4(0.2F, 0.4F, 0.6F, 1.0F));
    }

    TEST_P(VertexAccessTest, PositionChannelDecidesTheVertexCount)
    {
        const std::vector<DataVariant> channels {
            std::vector<glm::vec3> { { 1, 2, 3 }, { 4, 5, 6 }, { 7, 8, 9 } },
            std::vector<glm::vec4> { { 1, 1, 1, 1 }, { 1, 1, 1, 1 }, { 1, 1, 1, 1 } },
        };

        const auto access = run(channels);

        ASSERT_TRUE(access.has_value());
        EXPECT_EQ(access->layout.vertex_count, 3U);
        EXPECT_EQ(access->byte_count, 3 * k_record);
    }

    TEST_P(VertexAccessTest, AssembledBytesMatchTheLayoutOffsets)
    {
        const std::vector<DataVariant> channels { std::vector<glm::vec3> { { 1, 2, 3 } } };

        const auto access = run(channels);

        ASSERT_TRUE(access.has_value());
        const VertexLayout expected = GetParam().name == "Point"   ? VertexLayout::for_points()
            : GetParam().name == "Line"                            ? VertexLayout::for_lines()
                                                                   : VertexLayout::for_meshes();
        ASSERT_EQ(access->layout.attributes.size(), expected.attributes.size());
        for (size_t i = 0; i < expected.attributes.size(); ++i) {
            EXPECT_EQ(access->layout.attributes[i].offset_in_vertex, expected.attributes[i].offset_in_vertex) << i;
            EXPECT_EQ(access->layout.attributes[i].name, expected.attributes[i].name) << i;
        }
    }

    TEST_P(VertexAccessTest, InsertedChannelsAssembleBackIntoTheOriginalBytes)
    {
        const auto original = bytes_of(sample_vertices());
        std::vector<DataVariant> channels;
        VertexInsertion insertion(channels);
        ASSERT_TRUE(insertion.insert_interleaved(original, VertexLayout::for_meshes()));

        const auto access = run(channels);

        ASSERT_TRUE(access.has_value());
        ASSERT_EQ(access->byte_count, original.size());
        EXPECT_EQ(std::memcmp(access->data_ptr, original.data(), original.size()), 0);
    }

    INSTANTIATE_TEST_SUITE_P(Primitives, VertexAccessTest,
        ::testing::Values(
            AccessCase { "Point", &Kakshya::as_point_vertex_access, "size", 3.0F },
            AccessCase { "Line", &Kakshya::as_line_vertex_access, "thickness", 1.0F },
            AccessCase { "Mesh", &Kakshya::as_mesh_vertex_access, "weight", 0.0F }),
        [](const ::testing::TestParamInfo<AccessCase>& info) {
            return std::string(info.param.name);
        });

    TEST(VertexAccessGenericTest, GenericEntryPointAssemblesPointVertices)
    {
        const std::vector<DataVariant> channels { std::vector<glm::vec3> { { 1, 2, 3 } } };

        const auto generic = Kakshya::as_vertex_access(channels);
        const auto point = Kakshya::as_point_vertex_access(channels);

        ASSERT_TRUE(generic.has_value());
        ASSERT_TRUE(point.has_value());
        EXPECT_EQ(generic->byte_count, point->byte_count);
        EXPECT_EQ(generic->layout.attributes[2].name, "size");
        EXPECT_EQ(std::memcmp(generic->data_ptr, point->data_ptr, point->byte_count), 0);
    }

    TEST(VertexAccessGenericTest, ConfigDefaultsMatchTheDocumentedValues)
    {
        const VertexAccessConfig config;

        EXPECT_EQ(config.default_color, glm::vec4(1.0F));
        EXPECT_FLOAT_EQ(config.default_size, 3.0F);
        EXPECT_FLOAT_EQ(config.default_thickness, 1.0F);
        EXPECT_FLOAT_EQ(config.default_weight, 0.0F);
        EXPECT_EQ(config.default_uv, glm::vec2(0.0F));
        EXPECT_EQ(config.default_normal, glm::vec3(0, 0, 1));
        EXPECT_EQ(config.default_tangent, glm::vec3(1, 0, 0));
    }

    TEST(VertexAccessGenericTest, DefaultAccessIsEmptyAndNotConverted)
    {
        const VertexAccess access;

        EXPECT_EQ(access.data_ptr, nullptr);
        EXPECT_EQ(access.byte_count, 0U);
        EXPECT_FALSE(access.was_converted());
    }

    // ---------------------------------------------------------------------
    // VertexInsertion
    // ---------------------------------------------------------------------

    TEST(VertexInsertionDecodeTest, CanonicalLayoutsCanBeDecoded)
    {
        EXPECT_TRUE(VertexInsertion::can_decode(VertexLayout::for_points()));
        EXPECT_TRUE(VertexInsertion::can_decode(VertexLayout::for_lines()));
        EXPECT_TRUE(VertexInsertion::can_decode(VertexLayout::for_meshes()));
        EXPECT_TRUE(VertexInsertion::can_decode(VertexLayout::for_textured_quad()));
    }

    TEST(VertexInsertionDecodeTest, ZeroStrideCannotBeDecoded)
    {
        VertexLayout layout = VertexLayout::for_meshes();
        layout.stride_bytes = 0;

        EXPECT_FALSE(VertexInsertion::can_decode(layout));
    }

    TEST(VertexInsertionDecodeTest, NoAttributesCannotBeDecoded)
    {
        VertexLayout layout;
        layout.stride_bytes = 68;

        EXPECT_FALSE(VertexInsertion::can_decode(layout));
    }

    TEST(VertexInsertionDecodeTest, UnsupportedModalitiesCannotBeDecoded)
    {
        VertexLayout layout = VertexLayout::for_meshes();
        layout.attributes.push_back({ .component_modality = DataModality::AUDIO_1D, .offset_in_vertex = 0, .name = "audio" });

        EXPECT_FALSE(VertexInsertion::can_decode(layout));
    }

    TEST(VertexInsertionDecodeTest, AttributesMustLieWithinTheStride)
    {
        VertexLayout layout = VertexLayout::for_meshes();
        layout.stride_bytes = 60;

        EXPECT_FALSE(VertexInsertion::can_decode(layout));
    }

    TEST(VertexInsertionTest, SplitsAttributesIntoTypedChannelsInLayoutOrder)
    {
        const auto vertices = sample_vertices();
        std::vector<DataVariant> channels;
        VertexInsertion insertion(channels);

        ASSERT_TRUE(insertion.insert_interleaved(bytes_of(vertices), VertexLayout::for_meshes()));

        ASSERT_EQ(channels.size(), 6U);
        const auto& positions = std::get<std::vector<glm::vec3>>(channels[0]);
        const auto& colors = std::get<std::vector<glm::vec4>>(channels[1]);
        const auto& weights = std::get<std::vector<float>>(channels[2]);
        const auto& uvs = std::get<std::vector<glm::vec2>>(channels[3]);
        const auto& normals = std::get<std::vector<glm::vec3>>(channels[4]);
        const auto& tangents = std::get<std::vector<glm::vec4>>(channels[5]);
        ASSERT_EQ(positions.size(), 2U);
        for (size_t i = 0; i < 2; ++i) {
            EXPECT_EQ(positions[i], vertices[i].position);
            EXPECT_EQ(colors[i], vertices[i].color);
            EXPECT_FLOAT_EQ(weights[i], vertices[i].weight);
            EXPECT_EQ(uvs[i], vertices[i].uv);
            EXPECT_EQ(normals[i], vertices[i].normal);
            EXPECT_EQ(tangents[i], vertices[i].tangent);
        }
    }

    TEST(VertexInsertionTest, RecordsTheLayoutAndTheDecodedCount)
    {
        std::vector<DataVariant> channels;
        VertexInsertion insertion(channels);
        VertexLayout layout = VertexLayout::for_meshes();
        layout.vertex_count = 1000;

        ASSERT_TRUE(insertion.insert_interleaved(bytes_of(sample_vertices()), layout));

        EXPECT_EQ(insertion.vertex_count(), 2U);
        EXPECT_EQ(insertion.layout().vertex_count, 2U);
        EXPECT_EQ(insertion.layout().stride_bytes, 68U);
        EXPECT_EQ(insertion.layout().attributes.size(), 6U);
    }

    TEST(VertexInsertionTest, VertexCountComesFromTheBytesNotTheLayout)
    {
        std::vector<DataVariant> channels;
        VertexInsertion insertion(channels);
        VertexLayout layout = VertexLayout::for_meshes();
        layout.vertex_count = 1000;

        ASSERT_TRUE(insertion.insert_interleaved(bytes_of(sample_vertices()), layout));

        EXPECT_EQ(std::get<std::vector<glm::vec3>>(channels[0]).size(), 2U);
    }

    TEST(VertexInsertionTest, ReplacesEarlierChannels)
    {
        std::vector<DataVariant> channels;
        VertexInsertion insertion(channels);
        ASSERT_TRUE(insertion.insert_interleaved(bytes_of(sample_vertices()), VertexLayout::for_meshes()));

        ASSERT_TRUE(insertion.insert_interleaved(bytes_of({ sample_vertices()[0] }), VertexLayout::for_meshes()));

        EXPECT_EQ(channels.size(), 6U);
        EXPECT_EQ(std::get<std::vector<glm::vec3>>(channels[0]).size(), 1U);
        EXPECT_EQ(insertion.vertex_count(), 1U);
    }

    TEST(VertexInsertionTest, RespectsAStrideLargerThanTheAttributes)
    {
        VertexLayout layout;
        layout.stride_bytes = 24;
        layout.attributes.push_back({ .component_modality = DataModality::VERTEX_POSITIONS_3D, .offset_in_vertex = 4, .name = "position" });
        layout.attributes.push_back({ .component_modality = DataModality::SCALAR_F32, .offset_in_vertex = 20, .name = "scalar" });
        std::vector<uint8_t> bytes(2 * 24, 0);
        const glm::vec3 p0(1, 2, 3);
        const glm::vec3 p1(4, 5, 6);
        const float s0 = 0.5F;
        const float s1 = 0.75F;
        std::memcpy(bytes.data() + 4, &p0, sizeof(p0));
        std::memcpy(bytes.data() + 20, &s0, sizeof(s0));
        std::memcpy(bytes.data() + 24 + 4, &p1, sizeof(p1));
        std::memcpy(bytes.data() + 24 + 20, &s1, sizeof(s1));
        std::vector<DataVariant> channels;
        VertexInsertion insertion(channels);

        ASSERT_TRUE(insertion.insert_interleaved(bytes, layout));

        EXPECT_EQ(std::get<std::vector<glm::vec3>>(channels[0]), (std::vector<glm::vec3> { p0, p1 }));
        EXPECT_EQ(std::get<std::vector<float>>(channels[1]), (std::vector<float> { s0, s1 }));
    }

    TEST(VertexInsertionTest, SplitsEveryDecodableModality)
    {
        VertexLayout layout;
        layout.stride_bytes = 12 * 4 + 8 + 16 + 4;
        uint32_t offset = 0;
        for (const auto modality : { DataModality::VERTEX_POSITIONS_3D, DataModality::VERTEX_NORMALS_3D, DataModality::VERTEX_TANGENTS_3D, DataModality::VERTEX_COLORS_RGB }) {
            layout.attributes.push_back({ .component_modality = modality, .offset_in_vertex = offset, .name = "v3" });
            offset += 12;
        }
        layout.attributes.push_back({ .component_modality = DataModality::TEXTURE_COORDS_2D, .offset_in_vertex = offset, .name = "uv" });
        offset += 8;
        layout.attributes.push_back({ .component_modality = DataModality::VERTEX_TANGENTS_4D, .offset_in_vertex = offset, .name = "t4" });
        offset += 16;
        layout.attributes.push_back({ .component_modality = DataModality::SCALAR_F32, .offset_in_vertex = offset, .name = "s" });
        const std::vector<uint8_t> bytes(layout.stride_bytes, 0);
        std::vector<DataVariant> channels;
        VertexInsertion insertion(channels);

        ASSERT_TRUE(insertion.insert_interleaved(bytes, layout));

        ASSERT_EQ(channels.size(), 7U);
        for (size_t i = 0; i < 4; ++i) {
            EXPECT_TRUE(std::holds_alternative<std::vector<glm::vec3>>(channels[i])) << i;
        }
        EXPECT_TRUE(std::holds_alternative<std::vector<glm::vec2>>(channels[4]));
        EXPECT_TRUE(std::holds_alternative<std::vector<glm::vec4>>(channels[5]));
        EXPECT_TRUE(std::holds_alternative<std::vector<float>>(channels[6]));
    }

    TEST(VertexInsertionTest, InvalidLayoutLeavesTheChannelsUntouched)
    {
        std::vector<DataVariant> channels;
        VertexInsertion insertion(channels);
        ASSERT_TRUE(insertion.insert_interleaved(bytes_of(sample_vertices()), VertexLayout::for_meshes()));
        VertexLayout bad = VertexLayout::for_meshes();
        bad.stride_bytes = 0;

        EXPECT_FALSE(insertion.insert_interleaved(bytes_of(sample_vertices()), bad));

        EXPECT_EQ(channels.size(), 6U);
        EXPECT_EQ(insertion.vertex_count(), 2U);
    }

    TEST(VertexInsertionTest, EmptyBytesAreRefused)
    {
        std::vector<DataVariant> channels;
        VertexInsertion insertion(channels);

        EXPECT_FALSE(insertion.insert_interleaved({}, VertexLayout::for_meshes()));
        EXPECT_TRUE(channels.empty());
    }

    TEST(VertexInsertionTest, PartialVerticesAreRefused)
    {
        std::vector<DataVariant> channels;
        VertexInsertion insertion(channels);
        auto bytes = bytes_of(sample_vertices());
        bytes.pop_back();

        EXPECT_FALSE(insertion.insert_interleaved(bytes, VertexLayout::for_meshes()));
        EXPECT_TRUE(channels.empty());
    }

    TEST(VertexInsertionTest, ClearEmptiesTheChannelsAndForgetsTheLayout)
    {
        std::vector<DataVariant> channels;
        VertexInsertion insertion(channels);
        ASSERT_TRUE(insertion.insert_interleaved(bytes_of(sample_vertices()), VertexLayout::for_meshes()));

        insertion.clear();

        EXPECT_TRUE(channels.empty());
        EXPECT_EQ(insertion.vertex_count(), 0U);
        EXPECT_TRUE(insertion.layout().attributes.empty());
        EXPECT_EQ(insertion.layout().stride_bytes, 0U);
    }

    TEST(VertexInsertionTest, ChannelOrderMatchesTheAccessSlotsForCanonicalLayouts)
    {
        std::vector<DataVariant> channels;
        VertexInsertion insertion(channels);
        const auto bytes = std::vector<uint8_t>(k_record, 0);

        for (const VertexLayout& layout : { VertexLayout::for_points(), VertexLayout::for_lines(), VertexLayout::for_meshes() }) {
            ASSERT_TRUE(insertion.insert_interleaved(bytes, layout));

            ASSERT_EQ(channels.size(), 6U);
            EXPECT_TRUE(std::holds_alternative<std::vector<glm::vec3>>(channels[0]));
            EXPECT_TRUE(std::holds_alternative<std::vector<glm::vec4>>(channels[1]));
            EXPECT_TRUE(std::holds_alternative<std::vector<float>>(channels[2]));
            EXPECT_TRUE(std::holds_alternative<std::vector<glm::vec2>>(channels[3]));
            EXPECT_TRUE(std::holds_alternative<std::vector<glm::vec3>>(channels[4]));
            EXPECT_TRUE(std::holds_alternative<std::vector<glm::vec4>>(channels[5]));
        }
    }

    TEST(VertexInsertionTest, RawLayoutDecodesTheSameSixSlotsAsTheOthers)
    {
        std::vector<DataVariant> channels;
        VertexInsertion insertion(channels);

        ASSERT_TRUE(insertion.insert_interleaved(std::vector<uint8_t>(k_record, 0), VertexLayout::for_raw()));

        EXPECT_EQ(channels.size(), 6U);
    }

} // namespace
} // namespace MayaFlux::Test
