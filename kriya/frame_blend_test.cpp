#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kakshya/NDData/VertexFormats.hpp"
#include "MayaFlux/Kakshya/Semantic/MeshAssemblyContainer.hpp"
#include "MayaFlux/Kriya/FrameBlend.hpp"
#include "MayaFlux/Portal/Graphics/GraphicsUtils.hpp"

#include <glm/gtc/matrix_transform.hpp>

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Kakshya::MeshAssemblyLocals;
    using Kakshya::MeshData;
    using Kakshya::MeshVertex;
    using Kakshya::SpatialAttribute;
    using Kakshya::SpatialData;
    using Kakshya::SpatialScope;
    using Kakshya::VolumeData;
    using Kakshya::VolumeField;
    using Kriya::FrameContext;
    using Kriya::FrameOperation;
    using Kriya::WeightedFrame;
    using Portal::Graphics::PrimitiveTopology;

    template <typename Frame>
    std::shared_ptr<const Frame> blend_of(const FrameOperation<Frame>& operation,
        std::vector<WeightedFrame<Frame>> picks)
    {
        return operation.mix_fn()(FrameContext {}, std::span<const WeightedFrame<Frame>>(picks));
    }

    template <typename Frame>
    WeightedFrame<Frame> weighted(std::shared_ptr<const Frame> frame, double weight, size_t index = 0)
    {
        return { .frame = std::move(frame), .weight = weight, .index = index };
    }

    std::shared_ptr<const VolumeData> volume_of(std::vector<float> density, glm::uvec3 resolution = glm::uvec3(2, 1, 1))
    {
        VolumeField field;
        field.name = "density";
        field.values = std::move(density);
        VolumeData data;
        data.lattice.resolution = resolution;
        data.fields.push_back(std::move(field));
        return std::make_shared<const VolumeData>(std::move(data));
    }

    TEST(FrameBlendTest, VolumeFieldsBecomeTheWeightedMeanOfTheFrames)
    {
        const auto result = blend_of(Kriya::volume_blend(),
            { weighted(volume_of({ 0.0F, 10.0F }), 0.25), weighted(volume_of({ 10.0F, 20.0F }), 0.75) });

        ASSERT_NE(result, nullptr);
        const auto* density = result->find("density")->as_scalar();
        ASSERT_NE(density, nullptr);
        EXPECT_NEAR((*density)[0], 7.5F, 1e-5F);
        EXPECT_NEAR((*density)[1], 17.5F, 1e-5F);
    }

    TEST(FrameBlendTest, VolumeFramesOfAnotherLatticeGiveTheHeaviestFrame)
    {
        const auto heavy = volume_of({ 1.0F, 2.0F, 3.0F }, glm::uvec3(3, 1, 1));

        const auto result = blend_of(Kriya::volume_blend(),
            { weighted(volume_of({ 0.0F, 10.0F }), 0.25), weighted(heavy, 0.75) });

        EXPECT_EQ(result, heavy);
    }

    TEST(FrameBlendTest, AVectorFieldOfAVolumeIsBlendedToo)
    {
        const auto vectors = [](glm::vec3 a, glm::vec3 b) {
            VolumeField field;
            field.name = "velocity";
            field.values = std::vector<glm::vec3> { a, b };
            VolumeData data;
            data.lattice.resolution = glm::uvec3(2, 1, 1);
            data.fields.push_back(std::move(field));
            return std::make_shared<const VolumeData>(std::move(data));
        };

        const auto result = blend_of(Kriya::volume_blend(),
            { weighted(vectors(glm::vec3(0.0F), glm::vec3(2.0F)), 1.0), weighted(vectors(glm::vec3(4.0F), glm::vec3(6.0F)), 1.0) });

        ASSERT_NE(result, nullptr);
        const auto* velocity = result->find("velocity")->as_vector();
        ASSERT_NE(velocity, nullptr);
        EXPECT_EQ((*velocity)[0], glm::vec3(2.0F));
        EXPECT_EQ((*velocity)[1], glm::vec3(4.0F));
    }

    TEST(FrameBlendTest, AFieldOnlyOneFrameHasIsTakenFromThatFrame)
    {
        const auto with_extra = [] {
            auto frame = std::make_shared<VolumeData>(*volume_of({ 0.0F, 0.0F }));
            VolumeField extra;
            extra.name = "extra";
            extra.values = std::vector<float> { 5.0F, 6.0F };
            frame->fields.push_back(std::move(extra));
            return std::shared_ptr<const VolumeData>(frame);
        }();

        const auto result = blend_of(Kriya::volume_blend(),
            { weighted(with_extra, 1.0), weighted(volume_of({ 2.0F, 2.0F }), 1.0) });

        ASSERT_NE(result, nullptr);
        EXPECT_FLOAT_EQ((*result->find("extra")->as_scalar())[1], 6.0F);
        EXPECT_FLOAT_EQ((*result->find("density")->as_scalar())[0], 1.0F);
    }

    std::shared_ptr<const MeshData> triangle_at(float x, std::vector<uint32_t> indices = { 0, 1, 2 })
    {
        return std::make_shared<const MeshData>(MeshData::from_positions(
            std::vector<glm::vec3> { { x, 0.0F, 0.0F }, { x + 1.0F, 0.0F, 0.0F }, { x, 1.0F, 0.0F } }, std::move(indices))
                                                    .value());
    }

    std::vector<MeshVertex> vertices_of(const MeshData& mesh)
    {
        const auto& bytes = std::get<std::vector<uint8_t>>(mesh.vertex_variant);
        std::vector<MeshVertex> vertices(bytes.size() / sizeof(MeshVertex));
        std::memcpy(vertices.data(), bytes.data(), bytes.size());
        return vertices;
    }

    TEST(FrameBlendTest, MeshFramesWithTheSameTrianglesBlendTheirVertices)
    {
        const auto a = triangle_at(0.0F);
        const auto b = triangle_at(2.0F);

        const auto result = blend_of(Kriya::mesh_blend(), { weighted(a, 1.0), weighted(b, 1.0) });

        ASSERT_NE(result, nullptr);
        const auto vertices = vertices_of(*result);
        ASSERT_EQ(vertices.size(), 3U);
        EXPECT_NEAR(vertices[0].position.x, 1.0F, 1e-5F);
        EXPECT_NEAR(vertices[1].position.x, 2.0F, 1e-5F);
        EXPECT_NEAR(vertices[2].position.y, 1.0F, 1e-5F);
        EXPECT_NEAR(glm::length(vertices[0].normal - vertices_of(*a)[0].normal), 0.0F, 1e-5F);
    }

    TEST(FrameBlendTest, AMeshBlendFollowsTheWeights)
    {
        const auto result = blend_of(Kriya::mesh_blend(), { weighted(triangle_at(0.0F), 3.0), weighted(triangle_at(4.0F), 1.0) });

        ASSERT_NE(result, nullptr);
        EXPECT_NEAR(vertices_of(*result)[0].position.x, 1.0F, 1e-5F);
    }

    TEST(FrameBlendTest, MeshFramesWithDifferentTrianglesGiveTheHeaviestFrame)
    {
        const auto heavy = triangle_at(2.0F, { 0, 2, 1 });

        const auto result = blend_of(Kriya::mesh_blend(), { weighted(triangle_at(0.0F), 0.3), weighted(heavy, 0.7) });

        EXPECT_EQ(result, heavy);
    }

    std::shared_ptr<const SpatialData> points_of(std::vector<glm::vec3> positions, std::vector<uint64_t> ids = {})
    {
        SpatialData frame;
        frame.topology = PrimitiveTopology::POINT_LIST;
        frame.positions = std::move(positions);
        frame.ids = std::move(ids);
        return std::make_shared<const SpatialData>(std::move(frame));
    }

    TEST(FrameBlendTest, PointFramesOfTheSameCountBlendPointForPoint)
    {
        const auto result = blend_of(Kriya::spatial_blend(),
            { weighted(points_of({ glm::vec3(0.0F), glm::vec3(2.0F, 0.0F, 0.0F) }), 1.0),
                weighted(points_of({ glm::vec3(2.0F, 2.0F, 0.0F), glm::vec3(4.0F, 0.0F, 0.0F) }), 1.0) });

        ASSERT_NE(result, nullptr);
        ASSERT_EQ(result->positions.size(), 2U);
        EXPECT_EQ(result->positions[0], glm::vec3(1.0F, 1.0F, 0.0F));
        EXPECT_EQ(result->positions[1], glm::vec3(3.0F, 0.0F, 0.0F));
    }

    TEST(FrameBlendTest, PointsAreMatchedByIdWhenTheFramesKeepThemInAnotherOrder)
    {
        const auto first = [] {
            SpatialData frame;
            frame.topology = PrimitiveTopology::POINT_LIST;
            frame.positions = { glm::vec3(0.0F), glm::vec3(2.0F, 0.0F, 0.0F) };
            frame.ids = { 1, 2 };
            frame.velocities = { glm::vec3(1.0F, 0.0F, 0.0F), glm::vec3(1.0F, 0.0F, 0.0F) };
            frame.attributes.push_back({ .name = "color", .scope = SpatialScope::Varying,
                .values = Kakshya::DataVariant { std::vector<glm::vec4> { glm::vec4(1.0F, 0.0F, 0.0F, 1.0F), glm::vec4(1.0F, 0.0F, 0.0F, 1.0F) } } });
            frame.attributes.push_back({ .name = "cluster", .scope = SpatialScope::Varying,
                .values = Kakshya::DataVariant { std::vector<uint32_t> { 0, 1 } } });
            return std::make_shared<const SpatialData>(std::move(frame));
        }();
        const auto second = [] {
            SpatialData frame;
            frame.topology = PrimitiveTopology::POINT_LIST;
            frame.positions = { glm::vec3(4.0F, 0.0F, 0.0F), glm::vec3(2.0F, 0.0F, 0.0F) };
            frame.ids = { 2, 1 };
            frame.velocities = { glm::vec3(3.0F, 0.0F, 0.0F), glm::vec3(5.0F, 0.0F, 0.0F) };
            frame.attributes.push_back({ .name = "color", .scope = SpatialScope::Varying,
                .values = Kakshya::DataVariant { std::vector<glm::vec4> { glm::vec4(0.0F, 0.0F, 1.0F, 1.0F), glm::vec4(0.0F, 1.0F, 0.0F, 1.0F) } } });
            frame.attributes.push_back({ .name = "cluster", .scope = SpatialScope::Varying,
                .values = Kakshya::DataVariant { std::vector<uint32_t> { 7, 7 } } });
            return std::make_shared<const SpatialData>(std::move(frame));
        }();

        const auto result = blend_of(Kriya::spatial_blend(), { weighted(first, 1.0), weighted(second, 1.0) });

        ASSERT_NE(result, nullptr);
        EXPECT_EQ(result->ids, first->ids);
        EXPECT_EQ(result->positions[0], glm::vec3(1.0F, 0.0F, 0.0F));
        EXPECT_EQ(result->positions[1], glm::vec3(3.0F, 0.0F, 0.0F));
        EXPECT_EQ(result->velocities[0], glm::vec3(3.0F, 0.0F, 0.0F));
        EXPECT_EQ(result->velocities[1], glm::vec3(2.0F, 0.0F, 0.0F));
        const auto& color = std::get<std::vector<glm::vec4>>(result->attributes[0].values);
        EXPECT_EQ(color[0], glm::vec4(0.5F, 0.5F, 0.0F, 1.0F));
        EXPECT_EQ(color[1], glm::vec4(0.5F, 0.0F, 0.5F, 1.0F));
        EXPECT_EQ(std::get<std::vector<uint32_t>>(result->attributes[1].values), (std::vector<uint32_t> { 0, 1 }));
    }

    TEST(FrameBlendTest, PointFramesThatCannotBeMatchedGiveTheHeaviestFrame)
    {
        const auto heavy = points_of({ glm::vec3(1.0F), glm::vec3(2.0F), glm::vec3(3.0F) });

        EXPECT_EQ(blend_of(Kriya::spatial_blend(), { weighted(points_of({ glm::vec3(0.0F) }), 0.2), weighted(heavy, 0.8) }), heavy);

        const auto with_ids = points_of({ glm::vec3(0.0F), glm::vec3(1.0F) }, { 1, 2 });
        const auto other_ids = points_of({ glm::vec3(0.0F), glm::vec3(1.0F) }, { 1, 9 });
        EXPECT_EQ(blend_of(Kriya::spatial_blend(), { weighted(with_ids, 1.0), weighted(other_ids, 0.5) }), with_ids);
    }

    TEST(FrameBlendTest, PointFramesOfAnotherTopologyGiveTheHeaviestFrame)
    {
        const auto curve = [] {
            SpatialData frame;
            frame.topology = PrimitiveTopology::LINE_STRIP;
            frame.positions = { glm::vec3(0.0F), glm::vec3(1.0F) };
            frame.vertex_counts_per_curve = { 2 };
            return std::make_shared<const SpatialData>(std::move(frame));
        }();
        const auto points = points_of({ glm::vec3(0.0F), glm::vec3(1.0F) });

        EXPECT_EQ(blend_of(Kriya::spatial_blend(), { weighted(points, 0.4), weighted(curve, 0.6) }), curve);
    }

    std::shared_ptr<const MeshAssemblyLocals> placed(const glm::mat4& matrix)
    {
        MeshAssemblyLocals frame;
        frame.matrices = { matrix };
        return std::make_shared<const MeshAssemblyLocals>(std::move(frame));
    }

    std::shared_ptr<const Kakshya::MeshAssemblyContainer> one_root()
    {
        auto assembly = std::make_shared<Kakshya::MeshAssemblyContainer>();
        std::ignore = assembly->add_placement({ .name = "root" });
        return assembly;
    }

    TEST(FrameBlendTest, PlacementsBlendTranslationScaleAndRotationPerRow)
    {
        const glm::mat4 moved = glm::translate(glm::mat4(1.0F), glm::vec3(2.0F, 0.0F, 0.0F))
            * glm::rotate(glm::mat4(1.0F), glm::radians(90.0F), glm::vec3(0.0F, 0.0F, 1.0F))
            * glm::scale(glm::mat4(1.0F), glm::vec3(3.0F));

        const auto result = blend_of(Kriya::placement_blend(one_root()),
            { weighted(placed(glm::mat4(1.0F)), 1.0), weighted(placed(moved), 1.0) });

        ASSERT_NE(result, nullptr);
        ASSERT_EQ(result->matrices.size(), 1U);
        const glm::mat4& m = result->matrices[0];
        EXPECT_NEAR(m[3].x, 1.0F, 1e-5F);
        EXPECT_NEAR(glm::length(glm::vec3(m[0])), 2.0F, 1e-4F);
        const glm::vec3 axis = glm::normalize(glm::vec3(m[0]));
        EXPECT_NEAR(axis.x, std::cos(glm::radians(45.0F)), 1e-4F);
        EXPECT_NEAR(axis.y, std::sin(glm::radians(45.0F)), 1e-4F);
    }

    TEST(FrameBlendTest, TheWorldPlacementsAreResolvedAgainFromTheBlend)
    {
        const auto result = blend_of(Kriya::placement_blend(one_root()),
            { weighted(placed(glm::mat4(1.0F)), 1.0), weighted(placed(glm::translate(glm::mat4(1.0F), glm::vec3(4.0F, 0.0F, 0.0F))), 1.0) });

        ASSERT_NE(result, nullptr);
        ASSERT_EQ(result->world.size(), 1U);
        EXPECT_NEAR(result->world[0][3].x, 2.0F, 1e-5F);
    }

    TEST(FrameBlendTest, PlacementsFollowTheWeights)
    {
        const auto result = blend_of(Kriya::placement_blend(one_root()),
            { weighted(placed(glm::mat4(1.0F)), 3.0), weighted(placed(glm::translate(glm::mat4(1.0F), glm::vec3(4.0F, 0.0F, 0.0F))), 1.0) });

        ASSERT_NE(result, nullptr);
        EXPECT_NEAR(result->matrices[0][3].x, 1.0F, 1e-5F);
    }

    TEST(FrameBlendTest, AFrameOfAnotherNumberOfRowsIsLeftOut)
    {
        MeshAssemblyLocals two;
        two.matrices = { glm::mat4(1.0F), glm::mat4(1.0F) };

        const auto result = blend_of(Kriya::placement_blend(one_root()),
            { weighted(placed(glm::mat4(1.0F)), 1.0),
                weighted(placed(glm::translate(glm::mat4(1.0F), glm::vec3(2.0F, 0.0F, 0.0F))), 1.0),
                weighted(std::make_shared<const MeshAssemblyLocals>(two), 0.5) });

        ASSERT_NE(result, nullptr);
        ASSERT_EQ(result->matrices.size(), 1U);
        EXPECT_NEAR(result->matrices[0][3].x, 1.0F, 1e-5F);
    }

    TEST(FrameBlendTest, WithOnlyOneUsableFrameThePlacementIsThatFrame)
    {
        const auto only = placed(glm::translate(glm::mat4(1.0F), glm::vec3(3.0F, 0.0F, 0.0F)));
        MeshAssemblyLocals two;
        two.matrices = { glm::mat4(1.0F), glm::mat4(1.0F) };

        const auto result = blend_of(Kriya::placement_blend(one_root()),
            { weighted(only, 1.0), weighted(std::make_shared<const MeshAssemblyLocals>(two), 0.5) });

        EXPECT_EQ(result, only);
    }

    TEST(FrameBlendTest, WithNoAssemblyThePlacementsBlendAndTheWorldIsLeftEmpty)
    {
        const auto result = blend_of(Kriya::placement_blend(nullptr),
            { weighted(placed(glm::mat4(1.0F)), 1.0), weighted(placed(glm::translate(glm::mat4(1.0F), glm::vec3(2.0F, 0.0F, 0.0F))), 1.0) });

        ASSERT_NE(result, nullptr);
        EXPECT_TRUE(result->world.empty());
        EXPECT_NEAR(result->matrices[0][3].x, 1.0F, 1e-5F);
    }

    TEST(FrameBlendTest, EachBlendIsAMixOperationAReflowTakes)
    {
        EXPECT_EQ(Kriya::volume_blend().kind(), FrameOperation<VolumeData>::Kind::Mix);
        EXPECT_EQ(Kriya::mesh_blend().kind(), FrameOperation<MeshData>::Kind::Mix);
        EXPECT_EQ(Kriya::spatial_blend().kind(), FrameOperation<SpatialData>::Kind::Mix);
        EXPECT_TRUE(Kriya::placement_blend(nullptr).valid());
    }

} // namespace
} // namespace MayaFlux::Test
