#include "gtest/gtest.h"
#include "../../support/test_metadata.hpp"

#include "MayaFlux/Kakshya/NDData/SpatialAccess.hpp"
#include "MayaFlux/Kakshya/NDData/VertexFormats.hpp"
#include "MayaFlux/Portal/Graphics/GraphicsUtils.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Kakshya::SpatialAttribute;
    using Kakshya::SpatialData;
    using Kakshya::SpatialScope;
    using Kakshya::Vertex;
    using Kakshya::VertexAccessConfig;
    using Portal::Graphics::PrimitiveTopology;

    VertexAccessConfig distinct_defaults()
    {
        return VertexAccessConfig {
            .default_color = { 0.1F, 0.2F, 0.3F, 0.4F },
            .default_size = 7.0F,
            .default_thickness = 2.0F,
            .default_uv = { 0.25F, 0.75F },
            .default_normal = { 0.0F, 1.0F, 0.0F },
            .default_tangent = { 0.0F, 0.0F, 1.0F },
        };
    }

    SpatialData points(size_t count)
    {
        SpatialData frame;
        frame.topology = PrimitiveTopology::POINT_LIST;
        for (size_t i = 0; i < count; ++i) {
            const auto f = static_cast<float>(i);
            frame.positions.emplace_back(f, f * 0.5F, -f);
        }
        return frame;
    }

    SpatialData strips(const std::vector<int32_t>& counts)
    {
        SpatialData frame;
        frame.topology = PrimitiveTopology::LINE_STRIP;
        frame.vertex_counts_per_curve = counts;
        const auto total = std::accumulate(counts.begin(), counts.end(), 0);
        for (int i = 0; i < total; ++i) {
            frame.positions.emplace_back(static_cast<float>(i), 0.0F, 0.0F);
        }
        return frame;
    }

    template <typename T>
    SpatialAttribute attribute(std::string name, SpatialScope scope, std::vector<T> values)
    {
        return SpatialAttribute { .name = std::move(name), .scope = scope, .values = Kakshya::DataVariant { std::move(values) } };
    }

    std::vector<Vertex> records_of(const Kakshya::VertexAccess& access)
    {
        std::vector<Vertex> records(access.byte_count / sizeof(Vertex));
        std::memcpy(records.data(), access.data_ptr, access.byte_count);
        return records;
    }

    TEST(SpatialAccessTest, AFrameOfPositionsOnlyGivesWholeVerticesWithEveryOtherColumnFromTheConfig)
    {
        const auto config = distinct_defaults();
        const auto frame = points(3);

        const auto access = Kakshya::as_spatial_vertex_access(frame, config);

        ASSERT_TRUE(access.has_value());
        EXPECT_EQ(access->layout.vertex_count, 3U);
        const auto records = records_of(*access);
        ASSERT_EQ(records.size(), 3U);
        for (size_t i = 0; i < records.size(); ++i) {
            EXPECT_EQ(records[i].position, frame.positions[i]);
            EXPECT_EQ(records[i].color, config.default_color);
            EXPECT_EQ(records[i].scalar, config.default_size);
            EXPECT_EQ(records[i].uv, config.default_uv);
            EXPECT_EQ(records[i].normal, config.default_normal);
            EXPECT_EQ(records[i].tangent, glm::vec4(config.default_tangent, 1.0F));
        }
    }

    TEST(SpatialAccessTest, EveryNamedColumnLandsInItsOwnSlotOfEveryVertex)
    {
        auto frame = points(2);
        frame.attributes.push_back(attribute<glm::vec4>("color", SpatialScope::Varying, { { 1, 0, 0, 1 }, { 0, 1, 0, 1 } }));
        frame.attributes.push_back(attribute<float>("scalar", SpatialScope::Varying, { 5.0F, 6.0F }));
        frame.attributes.push_back(attribute<glm::vec2>("uv", SpatialScope::Varying, { { 0.1F, 0.2F }, { 0.3F, 0.4F } }));
        frame.attributes.push_back(attribute<glm::vec3>("normal", SpatialScope::Varying, { { 1, 0, 0 }, { 0, 0, 1 } }));
        frame.attributes.push_back(attribute<glm::vec4>("tangent", SpatialScope::Varying, { { 0, 1, 0, -1 }, { 1, 0, 0, 1 } }));

        const auto access = Kakshya::as_spatial_vertex_access(frame, distinct_defaults());

        ASSERT_TRUE(access.has_value());
        const auto records = records_of(*access);
        ASSERT_EQ(records.size(), 2U);
        EXPECT_EQ(records[0].color, glm::vec4(1, 0, 0, 1));
        EXPECT_EQ(records[1].color, glm::vec4(0, 1, 0, 1));
        EXPECT_EQ(records[0].scalar, 5.0F);
        EXPECT_EQ(records[1].scalar, 6.0F);
        EXPECT_EQ(records[0].uv, glm::vec2(0.1F, 0.2F));
        EXPECT_EQ(records[1].uv, glm::vec2(0.3F, 0.4F));
        EXPECT_EQ(records[0].normal, glm::vec3(1, 0, 0));
        EXPECT_EQ(records[1].normal, glm::vec3(0, 0, 1));
        EXPECT_EQ(records[0].tangent, glm::vec4(0, 1, 0, -1));
        EXPECT_EQ(records[1].tangent, glm::vec4(1, 0, 0, 1));
    }

    TEST(SpatialAccessTest, ThreeComponentColourAndTangentCompleteToFour)
    {
        auto frame = points(1);
        frame.attributes.push_back(attribute<glm::vec3>("color", SpatialScope::Varying, { { 0.2F, 0.4F, 0.6F } }));
        frame.attributes.push_back(attribute<glm::vec3>("tangent", SpatialScope::Varying, { { 0.0F, 1.0F, 0.0F } }));

        const auto access = Kakshya::as_spatial_vertex_access(frame);

        ASSERT_TRUE(access.has_value());
        const auto records = records_of(*access);
        EXPECT_EQ(records[0].color, glm::vec4(0.2F, 0.4F, 0.6F, 1.0F));
        EXPECT_EQ(records[0].tangent, glm::vec4(0.0F, 1.0F, 0.0F, 1.0F));
    }

    TEST(SpatialAccessTest, AConstantAttributeGivesEveryVertexItsValue)
    {
        auto frame = points(4);
        frame.attributes.push_back(attribute<glm::vec4>("color", SpatialScope::Constant, { { 0.5F, 0.5F, 0.0F, 1.0F } }));

        const auto access = Kakshya::as_spatial_vertex_access(frame);

        ASSERT_TRUE(access.has_value());
        for (const auto& record : records_of(*access)) {
            EXPECT_EQ(record.color, glm::vec4(0.5F, 0.5F, 0.0F, 1.0F));
        }
    }

    TEST(SpatialAccessTest, AUniformAttributeGivesEveryVertexOfACurveItsCurvesValue)
    {
        auto frame = strips({ 2, 3 });
        frame.attributes.push_back(attribute<float>("scalar", SpatialScope::Uniform, { 10.0F, 20.0F }));

        const auto access = Kakshya::as_spatial_vertex_access(frame);

        ASSERT_TRUE(access.has_value());
        const auto records = records_of(*access);
        ASSERT_EQ(records.size(), 5U);
        const std::vector<float> expected { 10.0F, 10.0F, 20.0F, 20.0F, 20.0F };
        for (size_t i = 0; i < records.size(); ++i) {
            EXPECT_EQ(records[i].scalar, expected[i]) << "vertex " << i;
        }
    }

    TEST(SpatialAccessTest, AUniformAttributeOfAPointFrameIsAConstant)
    {
        auto frame = points(3);
        frame.attributes.push_back(attribute<float>("scalar", SpatialScope::Uniform, { 9.0F }));

        const auto access = Kakshya::as_spatial_vertex_access(frame);

        ASSERT_TRUE(access.has_value());
        for (const auto& record : records_of(*access)) {
            EXPECT_EQ(record.scalar, 9.0F);
        }
    }

    TEST(SpatialAccessTest, AColumnTheFrameSkipsBetweenTwoItCarriesComesFromTheConfig)
    {
        const auto config = distinct_defaults();
        auto frame = points(2);
        frame.attributes.push_back(attribute<glm::vec3>("normal", SpatialScope::Varying, { { 1, 0, 0 }, { 1, 0, 0 } }));

        const auto access = Kakshya::as_spatial_vertex_access(frame, config);

        ASSERT_TRUE(access.has_value());
        for (const auto& record : records_of(*access)) {
            EXPECT_EQ(record.color, config.default_color);
            EXPECT_EQ(record.scalar, config.default_size);
            EXPECT_EQ(record.uv, config.default_uv);
            EXPECT_EQ(record.normal, glm::vec3(1, 0, 0));
            EXPECT_EQ(record.tangent, glm::vec4(config.default_tangent, 1.0F));
        }
    }

    TEST(SpatialAccessTest, ACurveFrameTakesTheThicknessDefaultWhereAPointFrameTakesTheSize)
    {
        const auto config = distinct_defaults();

        const auto curves = Kakshya::as_spatial_vertex_access(strips({ 3 }), config);
        const auto cloud = Kakshya::as_spatial_vertex_access(points(3), config);

        ASSERT_TRUE(curves.has_value());
        ASSERT_TRUE(cloud.has_value());
        EXPECT_EQ(records_of(*curves)[0].scalar, config.default_thickness);
        EXPECT_EQ(records_of(*cloud)[0].scalar, config.default_size);
        EXPECT_EQ(curves->layout.vertex_count, 3U);
    }

    TEST(SpatialAccessTest, AnAttributeThatIsNotAColumnIsLeftOut)
    {
        const auto config = distinct_defaults();
        auto frame = points(3);
        frame.attributes.push_back(attribute<float>("color", SpatialScope::Varying, { 1.0F, 1.0F, 1.0F }));
        frame.attributes.push_back(attribute<float>("scalar", SpatialScope::Varying, { 1.0F, 2.0F }));
        frame.attributes.push_back(attribute<glm::vec3>("mass", SpatialScope::Varying, { { 1, 1, 1 }, { 2, 2, 2 }, { 3, 3, 3 } }));
        frame.attributes.push_back(attribute<glm::vec2>("uv", SpatialScope::Constant, { { 1, 1 }, { 2, 2 } }));

        const auto access = Kakshya::as_spatial_vertex_access(frame, config);

        ASSERT_TRUE(access.has_value());
        for (const auto& record : records_of(*access)) {
            EXPECT_EQ(record.color, config.default_color);
            EXPECT_EQ(record.scalar, config.default_size);
            EXPECT_EQ(record.uv, config.default_uv);
        }
    }

    TEST(SpatialAccessTest, AFrameWithNoPositionsGivesNoVertices)
    {
        EXPECT_FALSE(Kakshya::as_spatial_vertex_access(SpatialData {}).has_value());
        EXPECT_FALSE(Kakshya::as_spatial_vertex_access(points(0)).has_value());
    }

    TEST(SpatialClusterIdsTest, AVaryingClusterAttributeIsReadPerVertex)
    {
        auto frame = points(3);
        frame.attributes.push_back(attribute<uint32_t>("cluster", SpatialScope::Varying, { 5U, 6U, 7U }));

        EXPECT_EQ(Kakshya::spatial_cluster_ids(frame), (std::vector<uint32_t> { 5U, 6U, 7U }));
    }

    TEST(SpatialClusterIdsTest, AUniformClusterAttributeSpreadsOverEachCurve)
    {
        auto frame = strips({ 2, 3 });
        frame.attributes.push_back(attribute<uint32_t>("cluster", SpatialScope::Uniform, { 1U, 2U }));

        EXPECT_EQ(Kakshya::spatial_cluster_ids(frame), (std::vector<uint32_t> { 1U, 1U, 2U, 2U, 2U }));
    }

    TEST(SpatialClusterIdsTest, AFrameWithoutAUsableClusterAttributeIsClusterZeroThroughout)
    {
        const std::vector<uint32_t> zeros(3, 0U);

        EXPECT_EQ(Kakshya::spatial_cluster_ids(points(3)), zeros);

        auto wrong_type = points(3);
        wrong_type.attributes.push_back(attribute<float>("cluster", SpatialScope::Varying, { 1.0F, 2.0F, 3.0F }));
        EXPECT_EQ(Kakshya::spatial_cluster_ids(wrong_type), zeros);

        auto wrong_size = points(3);
        wrong_size.attributes.push_back(attribute<uint32_t>("cluster", SpatialScope::Varying, { 1U, 2U }));
        EXPECT_EQ(Kakshya::spatial_cluster_ids(wrong_size), zeros);
    }

    TEST(SpatialClusterIdsTest, AFrameWithNoPositionsHasNoIds)
    {
        EXPECT_TRUE(Kakshya::spatial_cluster_ids(SpatialData {}).empty());
    }

} // namespace
} // namespace MayaFlux::Test
