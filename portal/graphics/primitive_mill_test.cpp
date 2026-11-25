#include "gtest/gtest.h"
#include "../../support/test_metadata.hpp"


#include "../../support/vulkan_test_support.hpp"

#include "MayaFlux/Buffers/VKBuffer.hpp"
#include "MayaFlux/Portal/Graphics/LayoutTranslator.hpp"
#include "MayaFlux/Portal/Graphics/PrimitiveMill.hpp"
#include "MayaFlux/Registry/BackendRegistry.hpp"
#include "MayaFlux/Registry/Service/BufferService.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))

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

    MAYAFLUX_TEST_WITH(TEST_P, TriangleVertexCountTest, MatchesTheClosedForm, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
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

    MAYAFLUX_TEST_WITH(TEST, PrimitiveMillTest, MilledVertexCountSumsEveryRun, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
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
    // Layout translation
    // ------------------------------------------------------------------

    struct FormatCase {
        std::string_view name;
        DataModality modality;
        vk::Format format;
    };

    class ModalityFormatTest : public ::testing::TestWithParam<FormatCase> { };

    MAYAFLUX_TEST_WITH(TEST_P, ModalityFormatTest, MapsToItsVertexFormat, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
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

    MAYAFLUX_TEST_WITH(TEST, VertexLayoutTranslatorTest, TranslateGivesOneBindingAndOneAttributePerAttributeInOrder, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
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

    MAYAFLUX_TEST_WITH(TEST, VertexLayoutTranslatorTest, EmptyAttributesOrZeroStrideTranslateToNothing, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        VertexLayout empty;
        empty.stride_bytes = 12;
        VertexLayout zero_stride = vertex_layout(1);
        zero_stride.stride_bytes = 0;

        EXPECT_TRUE(VertexLayoutTranslator::translate_layout(empty).first.empty());
        EXPECT_TRUE(VertexLayoutTranslator::translate_layout(zero_stride).first.empty());
        EXPECT_TRUE(VertexLayoutTranslator::translate_layout(zero_stride).second.empty());
    }

    MAYAFLUX_TEST_WITH(TEST, VertexLayoutTranslatorTest, ModalitySizesMatchTheirFormats, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        EXPECT_EQ(VertexLayoutTranslator::get_modality_size_bytes(DataModality::VERTEX_POSITIONS_3D), 12U);
        EXPECT_EQ(VertexLayoutTranslator::get_modality_size_bytes(DataModality::VERTEX_COLORS_RGB), 12U);
        EXPECT_EQ(VertexLayoutTranslator::get_modality_size_bytes(DataModality::VERTEX_COLORS_RGBA), 16U);
        EXPECT_EQ(VertexLayoutTranslator::get_modality_size_bytes(DataModality::TEXTURE_COORDS_2D), 8U);
        EXPECT_EQ(VertexLayoutTranslator::get_modality_size_bytes(DataModality::SCALAR_F32), 4U);
        EXPECT_EQ(VertexLayoutTranslator::get_modality_size_bytes(DataModality::AUDIO_1D), 8U);
        EXPECT_EQ(VertexLayoutTranslator::get_modality_size_bytes(DataModality::VERTICES_3D), 68U);
    }

    MAYAFLUX_TEST_WITH(TEST, VertexLayoutTranslatorTest, DescriptionsNameTheAttributeShape, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        EXPECT_EQ(VertexLayoutTranslator::describe_modality(DataModality::VERTEX_POSITIONS_3D), "vec3 (positions)");
        EXPECT_EQ(VertexLayoutTranslator::describe_modality(DataModality::TEXTURE_COORDS_2D), "vec2 (UV)");
        EXPECT_EQ(VertexLayoutTranslator::describe_modality(DataModality::VERTEX_COLORS_RGBA), "vec4 (color RGBA)");
        EXPECT_EQ(VertexLayoutTranslator::describe_modality(DataModality::VERTICES_3D), "unknown");
    }


    // ------------------------------------------------------------------
    // Fill emulation and indexed spans
    // ------------------------------------------------------------------

    MAYAFLUX_TEST_WITH(TEST, PrimitiveMillTest, PointFillCountsSixMilledVerticesPerSourceVertex, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        using Portal::Graphics::triangle_vertex_count;

        EXPECT_EQ(triangle_vertex_count(PrimitiveTopology::TRIANGLE_LIST, 7, PolygonMode::POINT), 36U);
        EXPECT_EQ(triangle_vertex_count(PrimitiveTopology::TRIANGLE_STRIP, 5, PolygonMode::POINT), 30U);
        EXPECT_EQ(triangle_vertex_count(PrimitiveTopology::TRIANGLE_FAN, 2, PolygonMode::POINT), 0U);
        EXPECT_EQ(triangle_vertex_count(PrimitiveTopology::TRIANGLE_LIST, 7, PolygonMode::LINE),
            triangle_vertex_count(PrimitiveTopology::TRIANGLE_LIST, 7));
        EXPECT_EQ(triangle_vertex_count(PrimitiveTopology::LINE_LIST, 4, PolygonMode::POINT), 24U);
    }

    MAYAFLUX_TEST_WITH(TEST, PrimitiveMillTest, MilledVertexCountHonoursEachRunsFill, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        const std::vector<DrawRun> runs {
            { PrimitiveTopology::TRIANGLE_LIST, 0, 3, PolygonMode::POINT },
            { PrimitiveTopology::TRIANGLE_LIST, 3, 3, PolygonMode::LINE },
            { PrimitiveTopology::TRIANGLE_LIST, 6, 3 }
        };

        EXPECT_EQ(PrimitiveMill::milled_vertex_count(runs), 18U + 3U + 3U);
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

        static std::vector<uint8_t> milled(
            const std::vector<Vertex>& source,
            const std::vector<DrawRun>& runs,
            const MillSpec& spec = plane_spec(),
            const std::vector<uint32_t>& indices = {})
        {
            PrimitiveMill mill(spec);
            const auto buffer = indices.empty() ? source_buffer(source) : indexed_source_buffer(source, indices);
            const uint32_t count = mill.mill(buffer, runs, MillView {});
            if (count == 0) {
                return {};
            }
            return read_back(mill.output(), static_cast<size_t>(count) * sizeof(Vertex));
        }

        static std::shared_ptr<VKBuffer> raw_source_buffer(const std::vector<uint8_t>& bytes, const VertexLayout& layout)
        {
            auto buffer = std::make_shared<VKBuffer>(bytes.size(), VKBuffer::Usage::HOST_STORAGE, DataModality::VERTEX_POSITIONS_3D);
            service().initialize_buffer(buffer);
            std::memcpy(buffer->get_mapped_ptr(), bytes.data(), bytes.size());
            buffer->set_vertex_layout(layout);
            return buffer;
        }

        static void expect_tags(const std::vector<uint8_t>& data, const std::vector<float>& tags)
        {
            ASSERT_EQ(data.size(), tags.size() * sizeof(Vertex));
            for (size_t i = 0; i < tags.size(); ++i) {
                EXPECT_EQ(at(data, i).color[0], tags[i]) << "output vertex " << i;
            }
        }

        static void expect_wire_uvs(const std::vector<uint8_t>& data, size_t count)
        {
            const std::array<std::pair<float, float>, 3> corners { { { 21.0F, 0.0F }, { 20.0F, 1.0F }, { 20.0F, 0.0F } } };
            ASSERT_EQ(data.size(), count * sizeof(Vertex));
            for (size_t i = 0; i < count; ++i) {
                EXPECT_TRUE(uv_is(at(data, i), corners[i % 3].first, corners[i % 3].second)) << "output vertex " << i;
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

    TEST_F(PrimitiveMillDeviceTest, ATriangleListIsCopiedBitForBitAndTheOutputKeepsTheLayout)
    {
        const std::vector<Vertex> source { vertex(0, 0, 0), vertex(1, 0, 1), vertex(0, 1, 2) };
        PrimitiveMill mill(plane_spec());
        const auto buffer = source_buffer(source);
        ASSERT_EQ(mill.mill(buffer, Runs { { PrimitiveTopology::TRIANGLE_LIST, 0, 3 } }, MillView {}), 3U);

        const auto layout = mill.output()->get_vertex_layout();
        ASSERT_TRUE(layout.has_value());
        EXPECT_EQ(layout->vertex_count, 3U);
        EXPECT_EQ(layout->stride_bytes, sizeof(Vertex));
        EXPECT_EQ(layout->attributes.size(), 4U);
        EXPECT_EQ(read_back(mill.output(), 3 * sizeof(Vertex)), bytes_of(source));
    }

    TEST_F(PrimitiveMillDeviceTest, AStripFlipsEveryOtherTriangleAndAFanSharesTheFirstVertex)
    {
        const std::vector<Vertex> source { vertex(0, 0, 0), vertex(1, 0, 1), vertex(0, 1, 2), vertex(1, 1, 3) };

        expect_tags(milled(source, { { PrimitiveTopology::TRIANGLE_STRIP, 0, 4 } }), { 0, 1, 2, 2, 1, 3 });
        expect_tags(milled(source, { { PrimitiveTopology::TRIANGLE_FAN, 0, 4 } }), { 0, 1, 2, 0, 2, 3 });
    }

    TEST_F(PrimitiveMillDeviceTest, RunsAreMilledInOrderFromTheirOwnOffsets)
    {
        const auto data = milled({ vertex(0, 0, 0), vertex(1, 0, 1), vertex(0, 1, 2), vertex(8, 8, 3) },
            { { PrimitiveTopology::TRIANGLE_LIST, 0, 3 }, { PrimitiveTopology::POINT_LIST, 3, 1 } });

        ASSERT_EQ(data.size(), 9 * sizeof(Vertex));
        EXPECT_EQ(at(data, 2).color[0], 2.0F);
        EXPECT_EQ(at(data, 3).color[0], 3.0F);
        EXPECT_TRUE(position_is(at(data, 3), 7, 7));
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

    TEST_F(PrimitiveMillDeviceTest, APointBecomesAQuadAroundItWithTheSourceVertexCopiedToEveryCorner)
    {
        const auto data = milled({ vertex(5, 5, 7) }, { { PrimitiveTopology::POINT_LIST, 0, 1 } });

        ASSERT_EQ(data.size(), 6 * sizeof(Vertex));
        const std::array<std::pair<float, float>, 6> corners { { { 4, 4 }, { 6, 4 }, { 6, 6 }, { 4, 4 }, { 6, 6 }, { 4, 6 } } };
        const std::array<std::pair<float, float>, 6> uvs { { { 10, 10 }, { 11, 10 }, { 11, 11 }, { 10, 10 }, { 11, 11 }, { 10, 11 } } };
        for (size_t i = 0; i < corners.size(); ++i) {
            const auto v = at(data, i);
            EXPECT_TRUE(position_is(v, corners[i].first, corners[i].second)) << "corner " << i;
            EXPECT_TRUE(uv_is(v, uvs[i].first, uvs[i].second)) << "corner " << i;
            EXPECT_EQ(v.color[0], 7.0F) << "corner " << i;
            EXPECT_EQ(v.size, 1.0F) << "corner " << i;
        }
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

    TEST_F(PrimitiveMillDeviceTest, ALineSegmentBecomesARibbonWithUvsAlongItsLength)
    {
        const auto data = milled({ vertex(0, 0, 0), vertex(2, 0, 1) }, { { PrimitiveTopology::LINE_LIST, 0, 2 } });

        ASSERT_EQ(data.size(), 6 * sizeof(Vertex));
        EXPECT_TRUE(position_is(at(data, 0), 0, -1));
        EXPECT_TRUE(position_is(at(data, 1), 0, 1));
        EXPECT_TRUE(position_is(at(data, 2), 2, 1));
        EXPECT_TRUE(position_is(at(data, 3), 0, -1));
        EXPECT_TRUE(position_is(at(data, 4), 2, 1));
        EXPECT_TRUE(position_is(at(data, 5), 2, -1));
        EXPECT_TRUE(uv_is(at(data, 0), 0, 1));
        EXPECT_TRUE(uv_is(at(data, 1), 0, 0));
        EXPECT_TRUE(uv_is(at(data, 2), 1, 0));
        EXPECT_TRUE(uv_is(at(data, 5), 1, 1));
        EXPECT_EQ(at(data, 0).color[0], 0.0F);
        EXPECT_EQ(at(data, 5).color[0], 1.0F);
    }

    TEST_F(PrimitiveMillDeviceTest, TurningOffUvSynthesisKeepsTheSourceUv)
    {
        auto spec = plane_spec();
        spec.synthesize_uv = false;
        const auto data = milled({ vertex(0, 0, 0), vertex(2, 0, 1) }, { { PrimitiveTopology::LINE_LIST, 0, 2 } }, spec);

        ASSERT_EQ(data.size(), 6 * sizeof(Vertex));
        for (size_t i = 0; i < 6; ++i) {
            EXPECT_TRUE(uv_is(at(data, i), 0.125F, 0.875F)) << "vertex " << i;
        }
    }

    TEST_F(PrimitiveMillDeviceTest, AZeroLengthSegmentCollapsesToItsStart)
    {
        const auto data = milled({ vertex(3, 3, 9), vertex(3, 3, 9) }, { { PrimitiveTopology::LINE_LIST, 0, 2 } });

        ASSERT_EQ(data.size(), 6 * sizeof(Vertex));
        for (size_t i = 0; i < 6; ++i) {
            EXPECT_TRUE(position_is(at(data, i), 3, 3)) << "vertex " << i;
        }
    }

    TEST_F(PrimitiveMillDeviceTest, UnalignedStridelessOrPositionlessLayoutsMillNothing)
    {
        PrimitiveMill mill(plane_spec());
        const auto buffer = source_buffer({ vertex(0, 0, 0), vertex(1, 0, 1), vertex(0, 1, 2) });
        const Runs runs { { PrimitiveTopology::TRIANGLE_LIST, 0, 3 } };

        VertexLayout unaligned = vertex_layout(3);
        unaligned.stride_bytes = 34;
        VertexLayout no_position = vertex_layout(3);
        no_position.attributes.erase(no_position.attributes.begin());
        VertexLayout zero_stride = vertex_layout(3);
        zero_stride.stride_bytes = 0;

        EXPECT_EQ(mill.mill(buffer, runs, MillView {}, unaligned), 0U);
        EXPECT_EQ(mill.mill(buffer, runs, MillView {}, no_position), 0U);
        EXPECT_EQ(mill.mill(buffer, runs, MillView {}, zero_stride), 0U);
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

    TEST_F(PrimitiveMillDeviceTest, ReleaseFreesEveryBufferOfTheOutputRing)
    {
        PrimitiveMill mill(plane_spec(), 2);
        const auto buffer = source_buffer({ vertex(0, 0, 0), vertex(1, 0, 1), vertex(0, 1, 2) });
        const std::vector<DrawRun> runs { { PrimitiveTopology::TRIANGLE_LIST, 0, 3 } };
        ASSERT_EQ(mill.mill(buffer, runs, MillView {}), 3U);
        const auto first = mill.output();
        ASSERT_EQ(mill.mill(buffer, runs, MillView {}), 3U);
        const auto second = mill.output();
        ASSERT_NE(first, second);
        ASSERT_TRUE(first->is_initialized());
        ASSERT_TRUE(second->is_initialized());

        mill.release();

        EXPECT_FALSE(first->is_initialized());
        EXPECT_FALSE(second->is_initialized());
        EXPECT_TRUE(buffer->is_initialized());
    }

    TEST_F(PrimitiveMillDeviceTest, DestroyingAMillFreesItsOutput)
    {
        const auto buffer = source_buffer({ vertex(0, 0, 0), vertex(1, 0, 1), vertex(0, 1, 2) });
        const std::vector<DrawRun> runs { { PrimitiveTopology::TRIANGLE_LIST, 0, 3 } };
        std::shared_ptr<VKBuffer> output;

        {
            PrimitiveMill mill(plane_spec());
            ASSERT_EQ(mill.mill(buffer, runs, MillView {}), 3U);
            output = mill.output();
            ASSERT_TRUE(output->is_initialized());
        }

        EXPECT_FALSE(output->is_initialized());
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


    TEST_F(PrimitiveMillDeviceTest, IndexedSpansWalkTheIndicesNotTheVertices)
    {
        const std::vector<Vertex> source { vertex(0, 0, 0), vertex(1, 0, 1), vertex(0, 1, 2), vertex(1, 1, 3) };

        expect_tags(milled(source, { { PrimitiveTopology::TRIANGLE_LIST, 0, 6 } }, plane_spec(), { 3, 0, 2, 1, 2, 3 }),
            { 3, 0, 2, 1, 2, 3 });
        expect_tags(milled(source, { { PrimitiveTopology::TRIANGLE_STRIP, 0, 4 } }, plane_spec(), { 3, 2, 1, 0 }),
            { 3, 2, 1, 1, 2, 0 });
        expect_tags(milled(source, { { PrimitiveTopology::TRIANGLE_FAN, 0, 4 } }, plane_spec(), { 3, 2, 1, 0 }),
            { 3, 2, 1, 3, 1, 0 });
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

    TEST_F(PrimitiveMillDeviceTest, PointFillDrawsEveryVertexOfATriangleSpanAsAQuad)
    {
        const std::vector<Vertex> source { vertex(0, 0, 0), vertex(10, 0, 1), vertex(0, 10, 2) };
        const Runs runs { { PrimitiveTopology::TRIANGLE_LIST, 0, 3, PolygonMode::POINT } };

        const auto data = milled(source, runs);
        ASSERT_EQ(data.size(), 18 * sizeof(Vertex));
        EXPECT_TRUE(position_is(at(data, 0), -1, -1));
        EXPECT_TRUE(position_is(at(data, 6), 9, -1));
        EXPECT_TRUE(position_is(at(data, 12), -1, 9));
        EXPECT_EQ(at(data, 6).color[0], 1.0F);
        EXPECT_EQ(at(data, 12).color[0], 2.0F);

        const auto indexed = milled(source, runs, plane_spec(), { 2, 2, 0 });
        ASSERT_EQ(indexed.size(), 18 * sizeof(Vertex));
        EXPECT_EQ(at(indexed, 0).color[0], 2.0F);
        EXPECT_EQ(at(indexed, 6).color[0], 2.0F);
        EXPECT_EQ(at(indexed, 12).color[0], 0.0F);
    }

    TEST_F(PrimitiveMillDeviceTest, LineFillMarksTheCornersOfStripAndIndexedSpans)
    {
        const std::vector<Vertex> source { vertex(0, 0, 0), vertex(1, 0, 1), vertex(0, 1, 2), vertex(1, 1, 3) };

        expect_wire_uvs(milled(source, { { PrimitiveTopology::TRIANGLE_STRIP, 0, 4, PolygonMode::LINE } }), 6);
        expect_wire_uvs(milled(source, { { PrimitiveTopology::TRIANGLE_LIST, 0, 6, PolygonMode::LINE } },
                            plane_spec(), { 3, 0, 2, 1, 2, 3 }),
            6);
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
        EXPECT_TRUE(position_is(at(data, 1), 10, 0));
        EXPECT_EQ(at(data, 1).color[0], 1.0F);
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

    // ------------------------------------------------------------------
    // Line wire and point emulation, instance billboards
    // ------------------------------------------------------------------

    struct TangentVertex {
        float position[3];
        float color[3];
        float size;
        float uv[2];
        float tangent[4];
    };

    static_assert(sizeof(TangentVertex) == 52);

    constexpr std::array<float, 4> k_source_tangent { 0.5F, 0.25F, 0.125F, 1.0F };

    VertexLayout tangent_layout(uint32_t count)
    {
        auto layout = vertex_layout(count);
        layout.stride_bytes = sizeof(TangentVertex);
        layout.attributes.push_back(VertexAttributeLayout {
            .component_modality = DataModality::VERTEX_TANGENTS_4D, .offset_in_vertex = 36, .name = "tangent" });
        return layout;
    }

    std::vector<uint8_t> tangent_bytes(std::initializer_list<std::array<float, 3>> positions)
    {
        std::vector<uint8_t> out;
        for (const auto& position : positions) {
            TangentVertex v {};
            std::memcpy(v.position, position.data(), sizeof(v.position));
            v.size = 1.0F;
            v.uv[0] = 0.125F;
            v.uv[1] = 0.875F;
            std::memcpy(v.tangent, k_source_tangent.data(), sizeof(v.tangent));
            const auto* raw = reinterpret_cast<const uint8_t*>(&v);
            out.insert(out.end(), raw, raw + sizeof(v));
        }
        return out;
    }

    TangentVertex tangent_at(const std::vector<uint8_t>& data, size_t index)
    {
        TangentVertex v {};
        std::memcpy(&v, data.data() + index * sizeof(TangentVertex), sizeof(TangentVertex));
        return v;
    }

    MillSpec billboard_spec()
    {
        auto spec = plane_spec();
        spec.ribbon = MillSpec::Ribbon::WorldFacing;
        spec.billboard_instances = true;
        return spec;
    }

    ::testing::AssertionResult tangent_is(const TangentVertex& v, float x, float y, float z, float w)
    {
        if (std::abs(v.tangent[0] - x) < 1e-4F && std::abs(v.tangent[1] - y) < 1e-4F
            && std::abs(v.tangent[2] - z) < 1e-4F && std::abs(v.tangent[3] - w) < 1e-4F) {
            return ::testing::AssertionSuccess();
        }
        return ::testing::AssertionFailure() << "tangent (" << v.tangent[0] << ", " << v.tangent[1] << ", "
                                             << v.tangent[2] << ", " << v.tangent[3] << ") expected (" << x << ", "
                                             << y << ", " << z << ", " << w << ")";
    }

    TEST_F(PrimitiveMillDeviceTest, ALineSpanInLineFillKeepsItsRibbonAndCarriesTheWireMarkers)
    {
        const std::vector<Vertex> source { vertex(0, 0, 0), vertex(4, 0, 1) };

        const auto plain = milled(source, { { PrimitiveTopology::LINE_LIST, 0, 2, PolygonMode::FILL } });
        const auto wired = milled(source, { { PrimitiveTopology::LINE_LIST, 0, 2, PolygonMode::LINE } });

        expect_wire_uvs(wired, 6);
        ASSERT_EQ(plain.size(), wired.size());
        for (size_t i = 0; i < 6; ++i) {
            const auto p = at(plain, i);
            EXPECT_LT(p.uv[0], 2.0F) << "vertex " << i;
            EXPECT_TRUE(position_is(at(wired, i), p.position[0], p.position[1], p.position[2])) << "vertex " << i;
        }
    }

    TEST_F(PrimitiveMillDeviceTest, ALineSpanInPointFillBecomesOnePointQuadPerVertex)
    {
        const auto data = milled({ vertex(0, 0, 0), vertex(4, 0, 1) },
            { { PrimitiveTopology::LINE_LIST, 0, 2, PolygonMode::POINT } });

        ASSERT_EQ(data.size(), 12 * sizeof(Vertex));
        for (size_t i = 0; i < 12; ++i) {
            EXPECT_GE(at(data, i).uv[0], 10.0F) << "vertex " << i;
            EXPECT_LT(at(data, i).uv[0], 20.0F) << "vertex " << i;
        }
    }

    TEST_F(PrimitiveMillDeviceTest, BillboardPointKeepsTheCenterAndStoresTheCornerOffsetInTheTangent)
    {
        PrimitiveMill mill(billboard_spec());
        const auto buffer = raw_source_buffer(tangent_bytes({ { 3.0F, 2.0F, 0.0F } }), tangent_layout(1));
        ASSERT_EQ(mill.mill(buffer, Runs { { PrimitiveTopology::POINT_LIST, 0, 1 } }, MillView {}), 6U);
        const auto data = read_back(mill.output(), 6 * sizeof(TangentVertex));

        const std::array<std::pair<float, float>, 6> offsets { {
            { -1.0F, -1.0F }, { 1.0F, -1.0F }, { 1.0F, 1.0F },
            { -1.0F, -1.0F }, { 1.0F, 1.0F }, { -1.0F, 1.0F } } };
        for (size_t i = 0; i < 6; ++i) {
            const auto v = tangent_at(data, i);
            EXPECT_NEAR(v.position[0], 3.0F, 1e-4F) << "vertex " << i;
            EXPECT_NEAR(v.position[1], 2.0F, 1e-4F) << "vertex " << i;
            EXPECT_TRUE(tangent_is(v, offsets[i].first, offsets[i].second, 0.0F, 0.0F)) << "vertex " << i;
            EXPECT_NEAR(v.uv[0], 10.0F + (offsets[i].first + 1.0F) * 0.5F, 1e-4F) << "vertex " << i;
            EXPECT_NEAR(v.uv[1], 10.0F + (offsets[i].second + 1.0F) * 0.5F, 1e-4F) << "vertex " << i;
        }
    }

    TEST_F(PrimitiveMillDeviceTest, BillboardRibbonKeepsItsEndpointsAndStoresDirectionAndSignedHalfWidth)
    {
        PrimitiveMill mill(billboard_spec());
        const auto buffer = raw_source_buffer(
            tangent_bytes({ { 0.0F, 0.0F, 0.0F }, { 4.0F, 0.0F, 0.0F } }), tangent_layout(2));
        ASSERT_EQ(mill.mill(buffer, Runs { { PrimitiveTopology::LINE_LIST, 0, 2 } }, MillView {}), 6U);
        const auto data = read_back(mill.output(), 6 * sizeof(TangentVertex));

        const std::array<float, 6> x { 0.0F, 0.0F, 4.0F, 0.0F, 4.0F, 4.0F };
        const std::array<float, 6> half_width { -1.0F, 1.0F, 1.0F, -1.0F, 1.0F, -1.0F };
        for (size_t i = 0; i < 6; ++i) {
            const auto v = tangent_at(data, i);
            EXPECT_NEAR(v.position[0], x[i], 1e-4F) << "vertex " << i;
            EXPECT_NEAR(v.position[1], 0.0F, 1e-4F) << "vertex " << i;
            EXPECT_TRUE(tangent_is(v, 1.0F, 0.0F, 0.0F, half_width[i])) << "vertex " << i;
        }
    }

    TEST_F(PrimitiveMillDeviceTest, BillboardDegenerateRibbonSegmentClearsTheTangent)
    {
        PrimitiveMill mill(billboard_spec());
        const auto buffer = raw_source_buffer(
            tangent_bytes({ { 1.0F, 1.0F, 0.0F }, { 1.0F, 1.0F, 0.0F } }), tangent_layout(2));
        ASSERT_EQ(mill.mill(buffer, Runs { { PrimitiveTopology::LINE_LIST, 0, 2 } }, MillView {}), 6U);
        const auto data = read_back(mill.output(), 6 * sizeof(TangentVertex));

        for (size_t i = 0; i < 6; ++i) {
            EXPECT_TRUE(tangent_is(tangent_at(data, i), 0.0F, 0.0F, 0.0F, 0.0F)) << "vertex " << i;
        }
    }

    TEST_F(PrimitiveMillDeviceTest, BillboardLeavesTrianglePassthroughTangentsAlone)
    {
        PrimitiveMill mill(billboard_spec());
        const auto buffer = raw_source_buffer(
            tangent_bytes({ { 0.0F, 0.0F, 0.0F }, { 4.0F, 0.0F, 0.0F }, { 0.0F, 4.0F, 0.0F } }), tangent_layout(3));
        ASSERT_EQ(mill.mill(buffer, Runs { { PrimitiveTopology::TRIANGLE_LIST, 0, 3 } }, MillView {}), 3U);
        const auto data = read_back(mill.output(), 3 * sizeof(TangentVertex));

        for (size_t i = 0; i < 3; ++i) {
            EXPECT_TRUE(tangent_is(tangent_at(data, i), k_source_tangent[0], k_source_tangent[1],
                k_source_tangent[2], k_source_tangent[3]))
                << "vertex " << i;
        }
    }

    TEST_F(PrimitiveMillDeviceTest, WorldPlaneExpandsPointQuadsAndKeepsTheTangentWithOrWithoutTheBillboardFlag)
    {
        for (const bool flag : { false, true }) {
            auto spec = billboard_spec();
            spec.ribbon = MillSpec::Ribbon::WorldPlane;
            spec.billboard_instances = flag;
            PrimitiveMill mill(spec);
            const auto buffer = raw_source_buffer(tangent_bytes({ { 3.0F, 2.0F, 0.0F } }), tangent_layout(1));
            ASSERT_EQ(mill.mill(buffer, Runs { { PrimitiveTopology::POINT_LIST, 0, 1 } }, MillView {}), 6U);
            const auto data = read_back(mill.output(), 6 * sizeof(TangentVertex));

            EXPECT_NEAR(tangent_at(data, 0).position[0], 2.0F, 1e-4F) << "flag " << flag;
            EXPECT_NEAR(tangent_at(data, 0).position[1], 1.0F, 1e-4F) << "flag " << flag;
            EXPECT_NEAR(tangent_at(data, 2).position[0], 4.0F, 1e-4F) << "flag " << flag;
            EXPECT_NEAR(tangent_at(data, 2).position[1], 3.0F, 1e-4F) << "flag " << flag;
            EXPECT_TRUE(tangent_is(tangent_at(data, 1), k_source_tangent[0], k_source_tangent[1],
                k_source_tangent[2], k_source_tangent[3]))
                << "flag " << flag;
        }
    }

    TEST_F(PrimitiveMillDeviceTest, BillboardOnALayoutWithoutATangentFallsBackToExpandedGeometry)
    {
        const auto data = milled({ vertex(3, 2, 0) }, { { PrimitiveTopology::POINT_LIST, 0, 1 } }, billboard_spec());

        ASSERT_EQ(data.size(), 6 * sizeof(Vertex));
        EXPECT_FALSE(position_is(at(data, 0), 3.0F, 2.0F));
    }

} // namespace
} // namespace MayaFlux::Test
