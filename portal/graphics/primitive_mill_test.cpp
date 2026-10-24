#include "gtest/gtest.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../../support/vulkan_test_support.hpp"

#include "MayaFlux/Buffers/VKBuffer.hpp"
#include "MayaFlux/Portal/Graphics/LayoutTranslator.hpp"
#include "MayaFlux/Portal/Graphics/PrimitiveMill.hpp"
#include "MayaFlux/Registry/BackendRegistry.hpp"
#include "MayaFlux/Registry/Service/BufferService.hpp"

namespace MayaFlux::Test {
namespace {

    using Buffers::VKBuffer;
    using Kakshya::DataModality;
    using Kakshya::VertexAttributeLayout;
    using Kakshya::VertexLayout;
    using Portal::Graphics::DrawRun;
    using Portal::Graphics::MillSpec;
    using Portal::Graphics::MillView;
    using Portal::Graphics::PrimitiveMill;
    using Portal::Graphics::PolygonMode;
    using Portal::Graphics::PrimitiveTopology;
    using Portal::Graphics::VertexLayoutTranslator;

    using Runs = std::vector<DrawRun>;

    struct Vertex {
        float position[3];
        float color[3];
        float size;
        float uv[2];
    };

    static_assert(sizeof(Vertex) == 36);

    VertexLayout vertex_layout(uint32_t count)
    {
        VertexLayout layout;
        layout.vertex_count = count;
        layout.stride_bytes = sizeof(Vertex);
        layout.attributes = {
            VertexAttributeLayout { .component_modality = DataModality::VERTEX_POSITIONS_3D, .offset_in_vertex = 0, .name = "position" },
            VertexAttributeLayout { .component_modality = DataModality::VERTEX_COLORS_RGB, .offset_in_vertex = 12, .name = "color" },
            VertexAttributeLayout { .component_modality = DataModality::SCALAR_F32, .offset_in_vertex = 24, .name = "size" },
            VertexAttributeLayout { .component_modality = DataModality::TEXTURE_COORDS_2D, .offset_in_vertex = 28, .name = "uv" }
        };
        return layout;
    }

    Vertex vertex(float x, float y, float tag)
    {
        Vertex v {};
        v.position[0] = x;
        v.position[1] = y;
        v.position[2] = 0.0F;
        v.color[0] = tag;
        v.color[1] = tag + 0.25F;
        v.color[2] = tag + 0.5F;
        v.size = 1.0F;
        v.uv[0] = 0.125F;
        v.uv[1] = 0.875F;
        return v;
    }

    std::vector<uint8_t> bytes_of(const std::vector<Vertex>& vertices)
    {
        std::vector<uint8_t> out(vertices.size() * sizeof(Vertex));
        std::memcpy(out.data(), vertices.data(), out.size());
        return out;
    }

    Vertex at(const std::vector<uint8_t>& data, size_t index)
    {
        Vertex v {};
        std::memcpy(&v, data.data() + index * sizeof(Vertex), sizeof(Vertex));
        return v;
    }

    MillSpec plane_spec()
    {
        MillSpec spec;
        spec.ribbon = MillSpec::Ribbon::WorldPlane;
        spec.use_vertex_extent = false;
        spec.width_scale = 1.0F;
        spec.point_scale = 1.0F;
        spec.fallback_extent = 2.0F;
        return spec;
    }

    ::testing::AssertionResult position_is(const Vertex& v, float x, float y, float z = 0.0F)
    {
        if (std::abs(v.position[0] - x) < 1e-4F && std::abs(v.position[1] - y) < 1e-4F && std::abs(v.position[2] - z) < 1e-4F) {
            return ::testing::AssertionSuccess();
        }
        return ::testing::AssertionFailure() << "position (" << v.position[0] << ", " << v.position[1] << ", " << v.position[2]
                                             << ") expected (" << x << ", " << y << ", " << z << ")";
    }

    ::testing::AssertionResult uv_is(const Vertex& v, float u, float w)
    {
        if (std::abs(v.uv[0] - u) < 1e-4F && std::abs(v.uv[1] - w) < 1e-4F) {
            return ::testing::AssertionSuccess();
        }
        return ::testing::AssertionFailure() << "uv (" << v.uv[0] << ", " << v.uv[1] << ") expected (" << u << ", " << w << ")";
    }

    // ------------------------------------------------------------------
    // Closed form vertex counts
    // ------------------------------------------------------------------

    struct CountCase {
        std::string_view name;
        PrimitiveTopology topology;
        uint32_t vertices;
        uint32_t expected;
    };

    class TriangleVertexCountTest : public ::testing::TestWithParam<CountCase> { };

    TEST_P(TriangleVertexCountTest, MatchesTheClosedForm)
    {
        EXPECT_EQ(Portal::Graphics::triangle_vertex_count(GetParam().topology, GetParam().vertices), GetParam().expected);
    }

    INSTANTIATE_TEST_SUITE_P(Topologies, TriangleVertexCountTest,
        ::testing::Values(
            CountCase { "PointsBecomeQuads", PrimitiveTopology::POINT_LIST, 3, 18 },
            CountCase { "NoPoints", PrimitiveTopology::POINT_LIST, 0, 0 },
            CountCase { "LineListDropsADanglingVertex", PrimitiveTopology::LINE_LIST, 5, 12 },
            CountCase { "LineListOfOne", PrimitiveTopology::LINE_LIST, 1, 0 },
            CountCase { "LineStripOfOne", PrimitiveTopology::LINE_STRIP, 1, 0 },
            CountCase { "LineStripSegments", PrimitiveTopology::LINE_STRIP, 4, 18 },
            CountCase { "TriangleListFloorsToWholeTriangles", PrimitiveTopology::TRIANGLE_LIST, 7, 6 },
            CountCase { "TriangleListTooShort", PrimitiveTopology::TRIANGLE_LIST, 2, 0 },
            CountCase { "TriangleStripTooShort", PrimitiveTopology::TRIANGLE_STRIP, 2, 0 },
            CountCase { "TriangleStripTriangles", PrimitiveTopology::TRIANGLE_STRIP, 5, 9 },
            CountCase { "TriangleFanOfThree", PrimitiveTopology::TRIANGLE_FAN, 3, 3 },
            CountCase { "TriangleFanTriangles", PrimitiveTopology::TRIANGLE_FAN, 6, 12 }),
        [](const ::testing::TestParamInfo<CountCase>& info) { return std::string(info.param.name); });

    TEST(PrimitiveMillTest, MilledVertexCountSumsEveryRun)
    {
        const std::vector<DrawRun> runs {
            { PrimitiveTopology::POINT_LIST, 0, 2 },
            { PrimitiveTopology::LINE_STRIP, 2, 3 },
            { PrimitiveTopology::TRIANGLE_LIST, 5, 3 }
        };

        EXPECT_EQ(PrimitiveMill::milled_vertex_count(runs), 12U + 12U + 3U);
        EXPECT_EQ(PrimitiveMill::milled_vertex_count(Runs {}), 0U);
    }

    // ------------------------------------------------------------------
    // Host path
    // ------------------------------------------------------------------

    TEST(MillOnHostTest, UnmillableLayoutsProduceNothing)
    {
        const auto src = bytes_of({ vertex(0, 0, 0), vertex(1, 0, 1), vertex(0, 1, 2) });
        const std::vector<DrawRun> runs { { PrimitiveTopology::TRIANGLE_LIST, 0, 3 } };
        std::vector<uint8_t> dst { 1, 2, 3 };

        VertexLayout unaligned = vertex_layout(3);
        unaligned.stride_bytes = 34;
        VertexLayout no_position = vertex_layout(3);
        no_position.attributes.erase(no_position.attributes.begin());
        VertexLayout zero_stride = vertex_layout(3);
        zero_stride.stride_bytes = 0;

        for (const auto* layout : { &unaligned, &no_position, &zero_stride }) {
            const auto result = Portal::Graphics::mill_on_host(src, *layout, runs, plane_spec(), {}, dst);
            EXPECT_EQ(result.vertex_count, 0U);
            EXPECT_TRUE(dst.empty());
        }
    }

    TEST(MillOnHostTest, NoRunsOrNothingToMillProducesNothing)
    {
        const auto src = bytes_of({ vertex(0, 0, 0), vertex(1, 0, 1) });
        std::vector<uint8_t> dst;

        EXPECT_EQ(Portal::Graphics::mill_on_host(src, vertex_layout(2), {}, plane_spec(), {}, dst).vertex_count, 0U);
        const std::vector<DrawRun> too_short { { PrimitiveTopology::TRIANGLE_LIST, 0, 2 } };
        EXPECT_EQ(Portal::Graphics::mill_on_host(src, vertex_layout(2), too_short, plane_spec(), {}, dst).vertex_count, 0U);
    }

    TEST(MillOnHostTest, TriangleListIsCopiedBitForBitAndTheResultKeepsTheLayout)
    {
        const std::vector<Vertex> source { vertex(0, 0, 0), vertex(1, 0, 1), vertex(0, 1, 2) };
        const auto src = bytes_of(source);
        const std::vector<DrawRun> runs { { PrimitiveTopology::TRIANGLE_LIST, 0, 3 } };
        std::vector<uint8_t> dst;

        const auto result = Portal::Graphics::mill_on_host(src, vertex_layout(3), runs, plane_spec(), {}, dst);

        EXPECT_EQ(result.vertex_count, 3U);
        EXPECT_EQ(result.stride_bytes, sizeof(Vertex));
        EXPECT_EQ(result.attributes.size(), 4U);
        EXPECT_EQ(dst, src);
    }

    TEST(MillOnHostTest, TriangleStripFlipsEveryOtherTriangle)
    {
        const auto src = bytes_of({ vertex(0, 0, 0), vertex(1, 0, 1), vertex(0, 1, 2), vertex(1, 1, 3) });
        const std::vector<DrawRun> runs { { PrimitiveTopology::TRIANGLE_STRIP, 0, 4 } };
        std::vector<uint8_t> dst;

        Portal::Graphics::mill_on_host(src, vertex_layout(4), runs, plane_spec(), {}, dst);

        const std::array<float, 6> expected { 0, 1, 2, 2, 1, 3 };
        ASSERT_EQ(dst.size(), expected.size() * sizeof(Vertex));
        for (size_t i = 0; i < expected.size(); ++i) {
            EXPECT_EQ(at(dst, i).color[0], expected[i]) << "output vertex " << i;
        }
    }

    TEST(MillOnHostTest, TriangleFanSharesTheFirstVertex)
    {
        const auto src = bytes_of({ vertex(0, 0, 0), vertex(1, 0, 1), vertex(1, 1, 2), vertex(0, 1, 3) });
        const std::vector<DrawRun> runs { { PrimitiveTopology::TRIANGLE_FAN, 0, 4 } };
        std::vector<uint8_t> dst;

        Portal::Graphics::mill_on_host(src, vertex_layout(4), runs, plane_spec(), {}, dst);

        const std::array<float, 6> expected { 0, 1, 2, 0, 2, 3 };
        ASSERT_EQ(dst.size(), expected.size() * sizeof(Vertex));
        for (size_t i = 0; i < expected.size(); ++i) {
            EXPECT_EQ(at(dst, i).color[0], expected[i]) << "output vertex " << i;
        }
    }

    TEST(MillOnHostTest, APointBecomesAQuadAroundItWithTheSourceVertexCopiedToEveryCorner)
    {
        const auto src = bytes_of({ vertex(5, 5, 7) });
        const std::vector<DrawRun> runs { { PrimitiveTopology::POINT_LIST, 0, 1 } };
        std::vector<uint8_t> dst;

        Portal::Graphics::mill_on_host(src, vertex_layout(1), runs, plane_spec(), {}, dst);

        ASSERT_EQ(dst.size(), 6 * sizeof(Vertex));
        const std::array<std::pair<float, float>, 6> corners { { { 4, 4 }, { 6, 4 }, { 6, 6 }, { 4, 4 }, { 6, 6 }, { 4, 6 } } };
        for (size_t i = 0; i < corners.size(); ++i) {
            const auto v = at(dst, i);
            EXPECT_TRUE(position_is(v, corners[i].first, corners[i].second)) << "corner " << i;
            EXPECT_EQ(v.color[0], 7.0F) << "corner " << i;
            EXPECT_EQ(v.size, 1.0F) << "corner " << i;
        }
    }

    TEST(MillOnHostTest, ALineSegmentBecomesARibbonWithUvsAlongItsLength)
    {
        const auto src = bytes_of({ vertex(0, 0, 0), vertex(2, 0, 1) });
        const std::vector<DrawRun> runs { { PrimitiveTopology::LINE_LIST, 0, 2 } };
        std::vector<uint8_t> dst;

        Portal::Graphics::mill_on_host(src, vertex_layout(2), runs, plane_spec(), {}, dst);

        ASSERT_EQ(dst.size(), 6 * sizeof(Vertex));
        EXPECT_TRUE(position_is(at(dst, 0), 0, -1));
        EXPECT_TRUE(position_is(at(dst, 1), 0, 1));
        EXPECT_TRUE(position_is(at(dst, 2), 2, 1));
        EXPECT_TRUE(position_is(at(dst, 3), 0, -1));
        EXPECT_TRUE(position_is(at(dst, 4), 2, 1));
        EXPECT_TRUE(position_is(at(dst, 5), 2, -1));
        EXPECT_TRUE(uv_is(at(dst, 0), 0, 1));
        EXPECT_TRUE(uv_is(at(dst, 1), 0, 0));
        EXPECT_TRUE(uv_is(at(dst, 2), 1, 0));
        EXPECT_TRUE(uv_is(at(dst, 5), 1, 1));
        EXPECT_EQ(at(dst, 0).color[0], 0.0F);
        EXPECT_EQ(at(dst, 5).color[0], 1.0F);
    }

    TEST(MillOnHostTest, TurningOffUvSynthesisKeepsTheSourceUv)
    {
        auto spec = plane_spec();
        spec.synthesize_uv = false;
        const auto src = bytes_of({ vertex(0, 0, 0), vertex(2, 0, 1) });
        const std::vector<DrawRun> runs { { PrimitiveTopology::LINE_LIST, 0, 2 } };
        std::vector<uint8_t> dst;

        Portal::Graphics::mill_on_host(src, vertex_layout(2), runs, spec, {}, dst);

        ASSERT_EQ(dst.size(), 6 * sizeof(Vertex));
        for (size_t i = 0; i < 6; ++i) {
            EXPECT_TRUE(uv_is(at(dst, i), 0.125F, 0.875F)) << "vertex " << i;
        }
    }

    TEST(MillOnHostTest, AZeroLengthSegmentCollapsesToItsStartAndCopiesNoAttributes)
    {
        const auto src = bytes_of({ vertex(3, 3, 9), vertex(3, 3, 9) });
        const std::vector<DrawRun> runs { { PrimitiveTopology::LINE_LIST, 0, 2 } };
        std::vector<uint8_t> dst;

        Portal::Graphics::mill_on_host(src, vertex_layout(2), runs, plane_spec(), {}, dst);

        ASSERT_EQ(dst.size(), 6 * sizeof(Vertex));
        for (size_t i = 0; i < 6; ++i) {
            EXPECT_TRUE(position_is(at(dst, i), 3, 3)) << "vertex " << i;
            EXPECT_EQ(at(dst, i).color[0], 0.0F) << "vertex " << i;
        }
    }

    TEST(MillOnHostTest, RunsAreMilledInOrderFromTheirOwnOffsets)
    {
        const auto src = bytes_of({ vertex(0, 0, 0), vertex(1, 0, 1), vertex(0, 1, 2), vertex(8, 8, 3) });
        const std::vector<DrawRun> runs {
            { PrimitiveTopology::TRIANGLE_LIST, 0, 3 },
            { PrimitiveTopology::POINT_LIST, 3, 1 }
        };
        std::vector<uint8_t> dst;

        const auto result = Portal::Graphics::mill_on_host(src, vertex_layout(4), runs, plane_spec(), {}, dst);

        EXPECT_EQ(result.vertex_count, 9U);
        EXPECT_EQ(at(dst, 2).color[0], 2.0F);
        EXPECT_EQ(at(dst, 3).color[0], 3.0F);
        EXPECT_TRUE(position_is(at(dst, 3), 7, 7));
    }

    // ------------------------------------------------------------------
    // Layout translation
    // ------------------------------------------------------------------

    struct FormatCase {
        std::string_view name;
        DataModality modality;
        vk::Format format;
    };

    class ModalityFormatTest : public ::testing::TestWithParam<FormatCase> { };

    TEST_P(ModalityFormatTest, MapsToItsVertexFormat)
    {
        EXPECT_EQ(VertexLayoutTranslator::modality_to_vk_format(GetParam().modality), GetParam().format);
    }

    INSTANTIATE_TEST_SUITE_P(Modalities, ModalityFormatTest,
        ::testing::Values(
            FormatCase { "Positions", DataModality::VERTEX_POSITIONS_3D, vk::Format::eR32G32B32Sfloat },
            FormatCase { "Normals", DataModality::VERTEX_NORMALS_3D, vk::Format::eR32G32B32Sfloat },
            FormatCase { "Tangents", DataModality::VERTEX_TANGENTS_3D, vk::Format::eR32G32B32Sfloat },
            FormatCase { "ColorsRgb", DataModality::VERTEX_COLORS_RGB, vk::Format::eR32G32B32Sfloat },
            FormatCase { "ColorsRgba", DataModality::VERTEX_COLORS_RGBA, vk::Format::eR32G32B32A32Sfloat },
            FormatCase { "TextureCoords", DataModality::TEXTURE_COORDS_2D, vk::Format::eR32G32Sfloat },
            FormatCase { "Audio", DataModality::AUDIO_1D, vk::Format::eR64Sfloat },
            FormatCase { "Spectral", DataModality::SPECTRAL_2D, vk::Format::eR32G32Sfloat },
            FormatCase { "Scalar", DataModality::SCALAR_F32, vk::Format::eR32Sfloat }),
        [](const ::testing::TestParamInfo<FormatCase>& info) { return std::string(info.param.name); });

    TEST(VertexLayoutTranslatorTest, TranslateGivesOneBindingAndOneAttributePerAttributeInOrder)
    {
        const auto [bindings, attributes] = VertexLayoutTranslator::translate_layout(vertex_layout(4), 2);

        ASSERT_EQ(bindings.size(), 1U);
        EXPECT_EQ(bindings[0].binding, 2U);
        EXPECT_EQ(bindings[0].stride, sizeof(Vertex));
        EXPECT_FALSE(bindings[0].per_instance);
        EXPECT_EQ(bindings[0].input_rate, vk::VertexInputRate::eVertex);
        ASSERT_EQ(attributes.size(), 4U);
        const std::array<vk::Format, 4> formats { vk::Format::eR32G32B32Sfloat, vk::Format::eR32G32B32Sfloat, vk::Format::eR32Sfloat, vk::Format::eR32G32Sfloat };
        const std::array<uint32_t, 4> offsets { 0, 12, 24, 28 };
        for (uint32_t i = 0; i < 4; ++i) {
            EXPECT_EQ(attributes[i].location, i);
            EXPECT_EQ(attributes[i].binding, 2U);
            EXPECT_EQ(attributes[i].format, formats[i]) << "attribute " << i;
            EXPECT_EQ(attributes[i].offset, offsets[i]) << "attribute " << i;
        }
    }

    TEST(VertexLayoutTranslatorTest, EmptyAttributesOrZeroStrideTranslateToNothing)
    {
        VertexLayout empty;
        empty.stride_bytes = 12;
        VertexLayout zero_stride = vertex_layout(1);
        zero_stride.stride_bytes = 0;

        EXPECT_TRUE(VertexLayoutTranslator::translate_layout(empty).first.empty());
        EXPECT_TRUE(VertexLayoutTranslator::translate_layout(zero_stride).first.empty());
        EXPECT_TRUE(VertexLayoutTranslator::translate_layout(zero_stride).second.empty());
    }

    TEST(VertexLayoutTranslatorTest, ModalitySizesMatchTheirFormats)
    {
        EXPECT_EQ(VertexLayoutTranslator::get_modality_size_bytes(DataModality::VERTEX_POSITIONS_3D), 12U);
        EXPECT_EQ(VertexLayoutTranslator::get_modality_size_bytes(DataModality::VERTEX_COLORS_RGB), 12U);
        EXPECT_EQ(VertexLayoutTranslator::get_modality_size_bytes(DataModality::VERTEX_COLORS_RGBA), 16U);
        EXPECT_EQ(VertexLayoutTranslator::get_modality_size_bytes(DataModality::TEXTURE_COORDS_2D), 8U);
        EXPECT_EQ(VertexLayoutTranslator::get_modality_size_bytes(DataModality::SCALAR_F32), 4U);
        EXPECT_EQ(VertexLayoutTranslator::get_modality_size_bytes(DataModality::AUDIO_1D), 8U);
        EXPECT_EQ(VertexLayoutTranslator::get_modality_size_bytes(DataModality::VERTICES_3D), 60U);
    }

    TEST(VertexLayoutTranslatorTest, DescriptionsNameTheAttributeShape)
    {
        EXPECT_EQ(VertexLayoutTranslator::describe_modality(DataModality::VERTEX_POSITIONS_3D), "vec3 (positions)");
        EXPECT_EQ(VertexLayoutTranslator::describe_modality(DataModality::TEXTURE_COORDS_2D), "vec2 (UV)");
        EXPECT_EQ(VertexLayoutTranslator::describe_modality(DataModality::VERTEX_COLORS_RGBA), "vec4 (color RGBA)");
        EXPECT_EQ(VertexLayoutTranslator::describe_modality(DataModality::VERTICES_3D), "unknown");
    }


    // ------------------------------------------------------------------
    // Fill emulation and indexed spans, host path
    // ------------------------------------------------------------------

    TEST(PrimitiveMillTest, PointFillCountsSixMilledVerticesPerSourceVertex)
    {
        using Portal::Graphics::triangle_vertex_count;

        EXPECT_EQ(triangle_vertex_count(PrimitiveTopology::TRIANGLE_LIST, 7, PolygonMode::POINT), 36U);
        EXPECT_EQ(triangle_vertex_count(PrimitiveTopology::TRIANGLE_STRIP, 5, PolygonMode::POINT), 30U);
        EXPECT_EQ(triangle_vertex_count(PrimitiveTopology::TRIANGLE_FAN, 2, PolygonMode::POINT), 0U);
        EXPECT_EQ(triangle_vertex_count(PrimitiveTopology::TRIANGLE_LIST, 7, PolygonMode::LINE),
            triangle_vertex_count(PrimitiveTopology::TRIANGLE_LIST, 7));
        EXPECT_EQ(triangle_vertex_count(PrimitiveTopology::LINE_LIST, 4, PolygonMode::POINT), 24U);
    }

    TEST(PrimitiveMillTest, MilledVertexCountHonoursEachRunsFill)
    {
        const std::vector<DrawRun> runs {
            { PrimitiveTopology::TRIANGLE_LIST, 0, 3, PolygonMode::POINT },
            { PrimitiveTopology::TRIANGLE_LIST, 3, 3, PolygonMode::LINE },
            { PrimitiveTopology::TRIANGLE_LIST, 6, 3 }
        };

        EXPECT_EQ(PrimitiveMill::milled_vertex_count(runs), 18U + 3U + 3U);
    }

    TEST(MillOnHostTest, TriangleListsFetchTheirVerticesThroughTheIndices)
    {
        const auto src = bytes_of({ vertex(0, 0, 0), vertex(1, 0, 1), vertex(0, 1, 2), vertex(1, 1, 3) });
        const std::vector<uint32_t> indices { 3, 0, 2, 1, 2, 3 };
        const std::vector<DrawRun> runs { { PrimitiveTopology::TRIANGLE_LIST, 0, 6 } };
        std::vector<uint8_t> dst;

        const auto result = Portal::Graphics::mill_on_host(src, vertex_layout(4), runs, plane_spec(), {}, dst, indices);

        ASSERT_EQ(result.vertex_count, 6U);
        const std::array<float, 6> expected { 3, 0, 2, 1, 2, 3 };
        for (size_t i = 0; i < expected.size(); ++i) {
            EXPECT_EQ(at(dst, i).color[0], expected[i]) << "output vertex " << i;
        }
    }

    TEST(MillOnHostTest, StripsAndFansWalkTheIndicesNotTheVertices)
    {
        const auto src = bytes_of({ vertex(0, 0, 0), vertex(1, 0, 1), vertex(0, 1, 2), vertex(1, 1, 3) });
        const std::vector<uint32_t> indices { 3, 2, 1, 0 };
        std::vector<uint8_t> dst;

        Portal::Graphics::mill_on_host(src, vertex_layout(4),
            Runs { { PrimitiveTopology::TRIANGLE_STRIP, 0, 4 } }, plane_spec(), {}, dst, indices);
        const std::array<float, 6> strip { 3, 2, 1, 1, 2, 0 };
        ASSERT_EQ(dst.size(), strip.size() * sizeof(Vertex));
        for (size_t i = 0; i < strip.size(); ++i) {
            EXPECT_EQ(at(dst, i).color[0], strip[i]) << "strip vertex " << i;
        }

        Portal::Graphics::mill_on_host(src, vertex_layout(4),
            Runs { { PrimitiveTopology::TRIANGLE_FAN, 0, 4 } }, plane_spec(), {}, dst, indices);
        const std::array<float, 6> fan { 3, 2, 1, 3, 1, 0 };
        ASSERT_EQ(dst.size(), fan.size() * sizeof(Vertex));
        for (size_t i = 0; i < fan.size(); ++i) {
            EXPECT_EQ(at(dst, i).color[0], fan[i]) << "fan vertex " << i;
        }
    }

    TEST(MillOnHostTest, PointFillDrawsEveryVertexOfATriangleSpanAsAQuad)
    {
        const auto src = bytes_of({ vertex(0, 0, 0), vertex(10, 0, 1), vertex(0, 10, 2) });
        const std::vector<DrawRun> runs { { PrimitiveTopology::TRIANGLE_LIST, 0, 3, PolygonMode::POINT } };
        std::vector<uint8_t> dst;

        const auto result = Portal::Graphics::mill_on_host(src, vertex_layout(3), runs, plane_spec(), {}, dst);

        ASSERT_EQ(result.vertex_count, 18U);
        EXPECT_TRUE(position_is(at(dst, 0), -1, -1));
        EXPECT_TRUE(position_is(at(dst, 6), 9, -1));
        EXPECT_TRUE(position_is(at(dst, 12), -1, 9));
        EXPECT_EQ(at(dst, 6).color[0], 1.0F);
        EXPECT_EQ(at(dst, 12).color[0], 2.0F);
    }

    TEST(MillOnHostTest, PointFillOnAnIndexedSpanQuadsTheIndexedVertices)
    {
        const auto src = bytes_of({ vertex(0, 0, 0), vertex(10, 0, 1), vertex(0, 10, 2) });
        const std::vector<uint32_t> indices { 2, 2, 0 };
        const std::vector<DrawRun> runs { { PrimitiveTopology::TRIANGLE_LIST, 0, 3, PolygonMode::POINT } };
        std::vector<uint8_t> dst;

        Portal::Graphics::mill_on_host(src, vertex_layout(3), runs, plane_spec(), {}, dst, indices);

        ASSERT_EQ(dst.size(), 18 * sizeof(Vertex));
        EXPECT_EQ(at(dst, 0).color[0], 2.0F);
        EXPECT_EQ(at(dst, 6).color[0], 2.0F);
        EXPECT_EQ(at(dst, 12).color[0], 0.0F);
    }

    TEST(MillOnHostTest, LineFillKeepsTheTrianglesAndMarksEachCornerWithItsBarycentricWeights)
    {
        const std::vector<Vertex> source { vertex(0, 0, 0), vertex(10, 0, 1), vertex(0, 10, 2) };
        const std::vector<DrawRun> runs { { PrimitiveTopology::TRIANGLE_LIST, 0, 3, PolygonMode::LINE } };
        std::vector<uint8_t> dst;

        const auto result = Portal::Graphics::mill_on_host(
            bytes_of(source), vertex_layout(3), runs, plane_spec(), {}, dst);

        ASSERT_EQ(result.vertex_count, 3U);
        EXPECT_TRUE(uv_is(at(dst, 0), 21.0F, 0.0F));
        EXPECT_TRUE(uv_is(at(dst, 1), 20.0F, 1.0F));
        EXPECT_TRUE(uv_is(at(dst, 2), 20.0F, 0.0F));
        for (size_t i = 0; i < source.size(); ++i) {
            EXPECT_TRUE(position_is(at(dst, i), source[i].position[0], source[i].position[1])) << "vertex " << i;
            EXPECT_EQ(at(dst, i).color[0], source[i].color[0]) << "vertex " << i;
        }
    }

    TEST(MillOnHostTest, LineFillOnALayoutWithoutUvsLeavesTheTrianglesUntouched)
    {
        const auto src = bytes_of({ vertex(0, 0, 0), vertex(10, 0, 1), vertex(0, 10, 2) });
        auto layout = vertex_layout(3);
        layout.attributes.pop_back();
        const std::vector<DrawRun> runs { { PrimitiveTopology::TRIANGLE_LIST, 0, 3, PolygonMode::LINE } };
        std::vector<uint8_t> dst;

        Portal::Graphics::mill_on_host(src, layout, runs, plane_spec(), {}, dst);

        EXPECT_EQ(dst, src);
    }

    TEST(MillOnHostTest, FillIsIgnoredBySpansThatAreNotTriangles)
    {
        const auto src = bytes_of({ vertex(0, 0, 0), vertex(2, 0, 1) });
        std::vector<uint8_t> plain;
        std::vector<uint8_t> filled;

        Portal::Graphics::mill_on_host(src, vertex_layout(2),
            Runs { { PrimitiveTopology::LINE_LIST, 0, 2 } }, plane_spec(), {}, plain);
        Portal::Graphics::mill_on_host(src, vertex_layout(2),
            Runs { { PrimitiveTopology::LINE_LIST, 0, 2, PolygonMode::POINT } }, plane_spec(), {}, filled);

        EXPECT_EQ(filled, plain);
    }

    // ------------------------------------------------------------------
    // Device path
    // ------------------------------------------------------------------

    class PrimitiveMillDeviceTest : public ::testing::Test {
    protected:
        static void SetUpTestSuite()
        {
            if (!vulkan_unavailable_reason().empty()) {
                return;
            }
            auto backend = std::make_shared<Core::VulkanBackend>();
            if (backend->initialize(headless_graphics_config())) {
                s_backend = std::move(backend);
            }
        }

        static void TearDownTestSuite()
        {
            s_backend.reset();
        }

        void SetUp() override
        {
            if (!s_backend) {
                GTEST_SKIP() << "No usable Vulkan device: " << vulkan_unavailable_reason();
            }
            teardown_portal();
            ASSERT_TRUE(Portal::Graphics::get_shader_foundry().initialize(s_backend));
            ASSERT_TRUE(Portal::Graphics::get_compute_press().initialize());
        }

        void TearDown() override
        {
            s_attached.clear();
            teardown_portal();
        }

        static void teardown_portal()
        {
            Portal::Graphics::get_compute_press().shutdown();
            Portal::Graphics::get_shader_foundry().shutdown();
        }

        static Registry::Service::BufferService& service()
        {
            return *Registry::BackendRegistry::instance().get_service<Registry::Service::BufferService>();
        }

        static std::shared_ptr<VKBuffer> source_buffer(const std::vector<Vertex>& vertices)
        {
            const auto bytes = bytes_of(vertices);
            auto buffer = std::make_shared<VKBuffer>(bytes.size(), VKBuffer::Usage::HOST_STORAGE, DataModality::VERTEX_POSITIONS_3D);
            service().initialize_buffer(buffer);
            std::memcpy(buffer->get_mapped_ptr(), bytes.data(), bytes.size());
            buffer->set_vertex_layout(vertex_layout(static_cast<uint32_t>(vertices.size())));
            return buffer;
        }

        static std::shared_ptr<VKBuffer> indexed_source_buffer(
            const std::vector<Vertex>& vertices,
            const std::vector<uint32_t>& indices)
        {
            auto source = source_buffer(vertices);
            auto index_buffer = std::make_shared<VKBuffer>(
                indices.size() * sizeof(uint32_t), VKBuffer::Usage::HOST_STORAGE, DataModality::UNKNOWN);
            service().initialize_buffer(index_buffer);
            std::memcpy(index_buffer->get_mapped_ptr(), indices.data(), indices.size() * sizeof(uint32_t));
            source->set_index_resources(
                index_buffer->get_buffer(),
                index_buffer->get_buffer_resources().memory,
                index_buffer->get_size_bytes());
            s_attached.push_back(index_buffer);
            return source;
        }

        static std::vector<uint8_t> read_back(const std::shared_ptr<VKBuffer>& buffer, size_t bytes)
        {
            Portal::Graphics::get_shader_foundry().get_graphics_queue().waitIdle();
            auto staging = std::make_shared<VKBuffer>(bytes, VKBuffer::Usage::STAGING, DataModality::UNKNOWN);
            service().initialize_buffer(staging);
            service().copy_buffer(
                static_cast<void*>(buffer->get_buffer()),
                static_cast<void*>(staging->get_buffer()),
                bytes, 0, 0);
            service().invalidate_range(staging->get_buffer_resources().memory, 0, bytes);
            std::vector<uint8_t> out(bytes);
            std::memcpy(out.data(), staging->get_mapped_ptr(), bytes);
            return out;
        }

        static void expect_matches_host(
            const std::vector<Vertex>& source,
            const std::vector<DrawRun>& runs,
            const MillSpec& spec,
            bool compare_uv,
            const std::vector<uint32_t>& indices = {})
        {
            PrimitiveMill mill(spec);
            const auto buffer = indices.empty() ? source_buffer(source) : indexed_source_buffer(source, indices);
            const uint32_t milled = mill.mill(buffer, runs, MillView {});
            ASSERT_GT(milled, 0U);
            ASSERT_NE(mill.output(), nullptr);

            std::vector<uint8_t> expected;
            Portal::Graphics::mill_on_host(
                bytes_of(source), vertex_layout(static_cast<uint32_t>(source.size())), runs, spec, MillView {}, expected, indices);
            const auto actual = read_back(mill.output(), static_cast<size_t>(milled) * sizeof(Vertex));

            ASSERT_EQ(actual.size(), expected.size());
            for (uint32_t i = 0; i < milled; ++i) {
                const auto a = at(actual, i);
                const auto e = at(expected, i);
                EXPECT_TRUE(position_is(a, e.position[0], e.position[1], e.position[2])) << "vertex " << i;
                EXPECT_EQ(a.color[0], e.color[0]) << "color of vertex " << i;
                EXPECT_EQ(a.color[1], e.color[1]) << "color of vertex " << i;
                EXPECT_EQ(a.color[2], e.color[2]) << "color of vertex " << i;
                EXPECT_EQ(a.size, e.size) << "vertex " << i;
                if (compare_uv) {
                    EXPECT_TRUE(uv_is(a, e.uv[0], e.uv[1])) << "vertex " << i;
                }
            }
        }

    private:
        static inline std::shared_ptr<Core::VulkanBackend> s_backend;
        static inline std::vector<std::shared_ptr<VKBuffer>> s_attached;
    };

    TEST_F(PrimitiveMillDeviceTest, OutputIsNullBeforeTheFirstMill)
    {
        PrimitiveMill mill;

        EXPECT_EQ(mill.output(), nullptr);
        EXPECT_EQ(mill.milled_count(), 0U);
    }

    TEST_F(PrimitiveMillDeviceTest, TriangleSpansMatchTheHostPathBitForBit)
    {
        const std::vector<Vertex> source { vertex(0, 0, 0), vertex(1, 0, 1), vertex(0, 1, 2), vertex(1, 1, 3) };

        expect_matches_host(source, { { PrimitiveTopology::TRIANGLE_LIST, 0, 3 } }, plane_spec(), true);
        expect_matches_host(source, { { PrimitiveTopology::TRIANGLE_STRIP, 0, 4 } }, plane_spec(), true);
        expect_matches_host(source, { { PrimitiveTopology::TRIANGLE_FAN, 0, 4 } }, plane_spec(), true);
    }

    TEST_F(PrimitiveMillDeviceTest, ExplicitLayoutOverridesTheSourceBuffersLayout)
    {
        PrimitiveMill mill(plane_spec());
        const std::vector<Vertex> source { vertex(2, 3, 7), vertex(5, 3, 8), vertex(2, 6, 9) };
        const auto buffer = source_buffer(source);

        auto conflicting_layout = vertex_layout(static_cast<uint32_t>(source.size()));
        std::swap(conflicting_layout.attributes[0].offset_in_vertex,
            conflicting_layout.attributes[1].offset_in_vertex);
        buffer->set_vertex_layout(conflicting_layout);

        const auto layout = vertex_layout(static_cast<uint32_t>(source.size()));
        const uint32_t milled = mill.mill(
            buffer,
            Runs { { PrimitiveTopology::TRIANGLE_LIST, 0, 3 } },
            MillView {},
            layout);
        ASSERT_EQ(milled, 3U);

        const auto data = read_back(mill.output(), static_cast<size_t>(milled) * sizeof(Vertex));
        for (size_t i = 0; i < source.size(); ++i) {
            EXPECT_TRUE(position_is(at(data, i), source[i].position[0], source[i].position[1]));
            EXPECT_EQ(at(data, i).color[0], source[i].color[0]);
        }
    }

    TEST_F(PrimitiveMillDeviceTest, PointQuadsMatchTheHostPathInPositionAndAttributes)
    {
        expect_matches_host({ vertex(5, 5, 7), vertex(-2, 3, 8) }, { { PrimitiveTopology::POINT_LIST, 0, 2 } }, plane_spec(), false);
    }

    TEST_F(PrimitiveMillDeviceTest, PointCornerUvsLandInTheMarkerRangeAboveTen)
    {
        PrimitiveMill mill(plane_spec());
        const auto buffer = source_buffer({ vertex(5, 5, 7) });
        const uint32_t milled = mill.mill(buffer, Runs { { PrimitiveTopology::POINT_LIST, 0, 1 } }, MillView {});
        ASSERT_EQ(milled, 6U);

        const auto data = read_back(mill.output(), static_cast<size_t>(milled) * sizeof(Vertex));

        for (uint32_t i = 0; i < milled; ++i) {
            EXPECT_GE(at(data, i).uv[0], 10.0F) << "vertex " << i;
            EXPECT_GE(at(data, i).uv[1], 10.0F) << "vertex " << i;
        }
    }

    TEST_F(PrimitiveMillDeviceTest, PointCornerUvsMatchTheHostPath)
    {
        expect_matches_host({ vertex(5, 5, 7) }, { { PrimitiveTopology::POINT_LIST, 0, 1 } }, plane_spec(), true);
    }

    TEST_F(PrimitiveMillDeviceTest, AnIsolatedRibbonSegmentMatchesTheHostPath)
    {
        expect_matches_host({ vertex(0, 0, 0), vertex(2, 0, 1) }, { { PrimitiveTopology::LINE_LIST, 0, 2 } }, plane_spec(), true);
    }

    TEST_F(PrimitiveMillDeviceTest, UnmillableSourcesAndBadRunsMillNothing)
    {
        PrimitiveMill mill(plane_spec());
        auto no_layout = std::make_shared<VKBuffer>(sizeof(Vertex), VKBuffer::Usage::HOST_STORAGE, DataModality::UNKNOWN);
        service().initialize_buffer(no_layout);
        const auto buffer = source_buffer({ vertex(0, 0, 0), vertex(1, 0, 1), vertex(0, 1, 2) });

        EXPECT_EQ(mill.mill(nullptr, Runs { { PrimitiveTopology::TRIANGLE_LIST, 0, 3 } }, MillView {}), 0U);
        EXPECT_EQ(mill.mill(no_layout, Runs { { PrimitiveTopology::TRIANGLE_LIST, 0, 3 } }, MillView {}), 0U);
        EXPECT_EQ(mill.mill(buffer, {}, MillView {}), 0U);
        EXPECT_EQ(mill.mill(buffer, Runs { { PrimitiveTopology::TRIANGLE_LIST, 0, 6 } }, MillView {}), 0U);
        EXPECT_EQ(mill.mill(buffer, Runs { { PrimitiveTopology::TRIANGLE_LIST, 1, 3 } }, MillView {}), 0U);
        EXPECT_EQ(mill.mill(buffer, Runs { { PrimitiveTopology::TRIANGLE_LIST, 0, 2 } }, MillView {}), 0U);
        EXPECT_EQ(mill.milled_count(), 0U);
    }

    TEST_F(PrimitiveMillDeviceTest, ARingOfTwoRotatesTheOutputAndARingOfOneKeepsIt)
    {
        const auto buffer = source_buffer({ vertex(0, 0, 0), vertex(1, 0, 1), vertex(0, 1, 2) });
        const std::vector<DrawRun> runs { { PrimitiveTopology::TRIANGLE_LIST, 0, 3 } };

        PrimitiveMill ring_two(plane_spec(), 2);
        ASSERT_EQ(ring_two.mill(buffer, runs, MillView {}), 3U);
        const auto first = ring_two.output();
        ASSERT_EQ(ring_two.mill(buffer, runs, MillView {}), 3U);
        EXPECT_NE(ring_two.output(), first);

        PrimitiveMill ring_one(plane_spec(), 1);
        ASSERT_EQ(ring_one.mill(buffer, runs, MillView {}), 3U);
        const auto only = ring_one.output();
        ASSERT_EQ(ring_one.mill(buffer, runs, MillView {}), 3U);
        EXPECT_EQ(ring_one.output(), only);
    }

    TEST_F(PrimitiveMillDeviceTest, ALargerMillGrowsTheDestinationAndASmallerOneKeepsIt)
    {
        PrimitiveMill mill(plane_spec());
        const auto buffer = source_buffer({ vertex(0, 0, 0), vertex(1, 0, 1), vertex(0, 1, 2), vertex(1, 1, 3), vertex(2, 1, 4), vertex(2, 2, 5) });

        ASSERT_EQ(mill.mill(buffer, Runs { { PrimitiveTopology::TRIANGLE_LIST, 0, 3 } }, MillView {}), 3U);
        const auto small_size = mill.output()->get_size_bytes();
        ASSERT_EQ(mill.mill(buffer, Runs { { PrimitiveTopology::TRIANGLE_LIST, 0, 6 } }, MillView {}), 6U);
        const auto grown_size = mill.output()->get_size_bytes();
        ASSERT_EQ(mill.mill(buffer, Runs { { PrimitiveTopology::TRIANGLE_LIST, 0, 3 } }, MillView {}), 3U);

        EXPECT_GT(grown_size, small_size);
        EXPECT_EQ(mill.output()->get_size_bytes(), grown_size);
        EXPECT_EQ(mill.milled_count(), 3U);
    }

    TEST_F(PrimitiveMillDeviceTest, ReleaseDropsEverythingAndTheMillWorksAgain)
    {
        PrimitiveMill mill(plane_spec());
        const auto buffer = source_buffer({ vertex(0, 0, 0), vertex(1, 0, 1), vertex(0, 1, 2) });
        const std::vector<DrawRun> runs { { PrimitiveTopology::TRIANGLE_LIST, 0, 3 } };
        ASSERT_EQ(mill.mill(buffer, runs, MillView {}), 3U);

        mill.release();

        EXPECT_EQ(mill.output(), nullptr);
        EXPECT_EQ(mill.milled_count(), 0U);
        EXPECT_EQ(mill.mill(buffer, runs, MillView {}), 3U);
        EXPECT_NE(mill.output(), nullptr);
    }

    TEST_F(PrimitiveMillDeviceTest, ANewSpecTakesEffectOnTheNextMill)
    {
        PrimitiveMill mill(plane_spec());
        const auto buffer = source_buffer({ vertex(0, 0, 0), vertex(2, 0, 1) });
        const std::vector<DrawRun> runs { { PrimitiveTopology::LINE_LIST, 0, 2 } };
        ASSERT_EQ(mill.mill(buffer, runs, MillView {}), 6U);
        const auto narrow = at(read_back(mill.output(), 6 * sizeof(Vertex)), 1);

        auto wide_spec = plane_spec();
        wide_spec.width_scale = 3.0F;
        mill.set_spec(wide_spec);
        ASSERT_EQ(mill.mill(buffer, runs, MillView {}), 6U);
        const auto wide = at(read_back(mill.output(), 6 * sizeof(Vertex)), 1);

        EXPECT_NEAR(narrow.position[1], 1.0F, 1e-4F);
        EXPECT_NEAR(wide.position[1], 3.0F, 1e-4F);
        EXPECT_EQ(mill.spec().width_scale, 3.0F);
    }


    TEST_F(PrimitiveMillDeviceTest, IndexedTriangleSpansMatchTheHostPath)
    {
        const std::vector<Vertex> source { vertex(0, 0, 0), vertex(1, 0, 1), vertex(0, 1, 2), vertex(1, 1, 3) };

        expect_matches_host(source, { { PrimitiveTopology::TRIANGLE_LIST, 0, 6 } }, plane_spec(), true, { 3, 0, 2, 1, 2, 3 });
        expect_matches_host(source, { { PrimitiveTopology::TRIANGLE_STRIP, 0, 4 } }, plane_spec(), true, { 3, 2, 1, 0 });
        expect_matches_host(source, { { PrimitiveTopology::TRIANGLE_FAN, 0, 4 } }, plane_spec(), true, { 3, 2, 1, 0 });
    }

    TEST_F(PrimitiveMillDeviceTest, IndexedTriangleRunsAreBoundedByTheIndexCountAndOtherRunsByTheVertexCount)
    {
        PrimitiveMill mill(plane_spec());
        const auto buffer = indexed_source_buffer(
            { vertex(0, 0, 0), vertex(1, 0, 1), vertex(0, 1, 2), vertex(1, 1, 3), vertex(2, 1, 4), vertex(2, 2, 5) },
            { 0, 1, 2 });

        EXPECT_EQ(mill.mill(buffer, Runs { { PrimitiveTopology::TRIANGLE_LIST, 0, 6 } }, MillView {}), 0U);
        EXPECT_EQ(mill.mill(buffer, Runs { { PrimitiveTopology::TRIANGLE_LIST, 0, 3 } }, MillView {}), 3U);
        EXPECT_EQ(mill.mill(buffer, Runs { { PrimitiveTopology::LINE_LIST, 0, 6 } }, MillView {}), 18U);
    }

    TEST_F(PrimitiveMillDeviceTest, PointFillOfTriangleSpansMatchesTheHostPath)
    {
        const std::vector<Vertex> source { vertex(0, 0, 0), vertex(10, 0, 1), vertex(0, 10, 2) };
        const std::vector<DrawRun> runs { { PrimitiveTopology::TRIANGLE_LIST, 0, 3, PolygonMode::POINT } };

        expect_matches_host(source, runs, plane_spec(), true);
        expect_matches_host(source, runs, plane_spec(), true, { 2, 2, 0 });
    }

    TEST_F(PrimitiveMillDeviceTest, LineFillOfTriangleSpansMatchesTheHostPath)
    {
        const std::vector<Vertex> source { vertex(0, 0, 0), vertex(1, 0, 1), vertex(0, 1, 2), vertex(1, 1, 3) };

        expect_matches_host(source,
            { { PrimitiveTopology::TRIANGLE_LIST, 0, 3, PolygonMode::LINE } }, plane_spec(), true);
        expect_matches_host(source,
            { { PrimitiveTopology::TRIANGLE_STRIP, 0, 4, PolygonMode::LINE } }, plane_spec(), true);
        expect_matches_host(source,
            { { PrimitiveTopology::TRIANGLE_LIST, 0, 6, PolygonMode::LINE } }, plane_spec(), true, { 3, 0, 2, 1, 2, 3 });
    }

    TEST_F(PrimitiveMillDeviceTest, LineFillWritesTheBarycentricMarkersIntoTheUvSlot)
    {
        PrimitiveMill mill(plane_spec());
        const auto buffer = source_buffer({ vertex(0, 0, 0), vertex(10, 0, 1), vertex(0, 10, 2) });
        const uint32_t milled = mill.mill(
            buffer, Runs { { PrimitiveTopology::TRIANGLE_LIST, 0, 3, PolygonMode::LINE } }, MillView {});
        ASSERT_EQ(milled, 3U);

        const auto data = read_back(mill.output(), 3 * sizeof(Vertex));

        EXPECT_TRUE(uv_is(at(data, 0), 21.0F, 0.0F));
        EXPECT_TRUE(uv_is(at(data, 1), 20.0F, 1.0F));
        EXPECT_TRUE(uv_is(at(data, 2), 20.0F, 0.0F));
    }

    TEST_F(PrimitiveMillDeviceTest, LineFillOnALayoutWithoutUvsFallsBackToPlainTriangles)
    {
        PrimitiveMill mill(plane_spec());
        const auto buffer = source_buffer({ vertex(0, 0, 0), vertex(10, 0, 1), vertex(0, 10, 2) });
        auto layout = *buffer->get_vertex_layout();
        layout.attributes.pop_back();
        buffer->set_vertex_layout(layout);
        const uint32_t milled = mill.mill(
            buffer, Runs { { PrimitiveTopology::TRIANGLE_LIST, 0, 3, PolygonMode::LINE } }, MillView {});
        ASSERT_EQ(milled, 3U);

        const auto data = read_back(mill.output(), 3 * sizeof(Vertex));

        for (size_t i = 0; i < 3; ++i) {
            EXPECT_TRUE(uv_is(at(data, i), 0.125F, 0.875F)) << "vertex " << i;
        }
    }

} // namespace
} // namespace MayaFlux::Test
