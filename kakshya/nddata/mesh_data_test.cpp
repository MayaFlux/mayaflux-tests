#include "gtest/gtest.h"

#include "MayaFlux/Kakshya/NDData/MeshData.hpp"
#include "MayaFlux/Kakshya/NDData/MeshInsertion.hpp"

namespace MayaFlux::Test {
namespace {

    using Kakshya::DataModality;
    using Kakshya::DataVariant;
    using Kakshya::MeshAccess;
    using Kakshya::MeshData;
    using Kakshya::MeshInsertion;
    using Kakshya::MeshSubrange;
    using Kakshya::Region;
    using Kakshya::RegionGroup;
    using Kakshya::VertexLayout;

    constexpr size_t k_stride = 68;

    std::vector<uint8_t> make_vertices(size_t count, uint32_t stride = k_stride)
    {
        std::vector<uint8_t> bytes(count * stride);
        for (size_t i = 0; i < bytes.size(); ++i) {
            bytes[i] = static_cast<uint8_t>(i % 251);
        }
        return bytes;
    }

    std::string text_attribute(const Region& region, const std::string& key)
    {
        return std::any_cast<std::string>(region.attributes.at(key));
    }

    uint64_t number_attribute(const Region& region, const std::string& key)
    {
        return std::any_cast<uint64_t>(region.attributes.at(key));
    }

    VertexLayout position_only_layout(uint32_t stride)
    {
        VertexLayout layout;
        layout.stride_bytes = stride;
        layout.attributes.push_back({ .component_modality = DataModality::VERTEX_POSITIONS_3D,
            .offset_in_vertex = 0,
            .name = "position" });
        return layout;
    }

    MeshData filled_mesh(size_t vertices = 4, std::vector<uint32_t> indices = { 0, 1, 2, 2, 1, 3 })
    {
        MeshData data = MeshData::empty();
        data.vertex_variant = make_vertices(vertices);
        data.index_variant = std::move(indices);
        return data;
    }

    TEST(MeshSubrangeTest, RegionSpansTheIndexRange)
    {
        MeshSubrange sub;
        sub.index_start = 12;
        sub.index_count = 30;

        const Region region = sub.to_region();

        EXPECT_EQ(region.start_coordinates, (std::vector<uint64_t> { 12 }));
        EXPECT_EQ(region.end_coordinates, (std::vector<uint64_t> { 42 }));
    }

    TEST(MeshSubrangeTest, RegionCarriesTheSubrangeAttributes)
    {
        MeshSubrange sub;
        sub.index_start = 6;
        sub.index_count = 9;
        sub.vertex_offset = 100;
        sub.name = "hull";
        sub.material_name = "steel";
        sub.diffuse_path = "steel.png";
        sub.diffuse_embedded = true;

        const Region region = sub.to_region();

        EXPECT_EQ(text_attribute(region, "name"), "hull");
        EXPECT_EQ(text_attribute(region, "material_name"), "steel");
        EXPECT_EQ(text_attribute(region, "diffuse_path"), "steel.png");
        EXPECT_EQ(number_attribute(region, "vertex_offset"), 100U);
        EXPECT_EQ(number_attribute(region, "index_count"), 9U);
        EXPECT_TRUE(std::any_cast<bool>(region.attributes.at("diffuse_embedded")));
    }

    TEST(MeshSubrangeTest, DefaultSubrangeIsAnEmptyUnnamedRegionAtZero)
    {
        const Region region = MeshSubrange {}.to_region();

        EXPECT_EQ(region.start_coordinates, (std::vector<uint64_t> { 0 }));
        EXPECT_EQ(region.end_coordinates, (std::vector<uint64_t> { 0 }));
        EXPECT_TRUE(text_attribute(region, "name").empty());
        EXPECT_FALSE(std::any_cast<bool>(region.attributes.at("diffuse_embedded")));
    }

    TEST(MeshAccessQueryTest, DefaultAccessIsInvalidAndEmpty)
    {
        const MeshAccess access;

        EXPECT_FALSE(access.is_valid());
        EXPECT_FALSE(access.has_submeshes());
        EXPECT_EQ(access.vertex_count(), 0U);
        EXPECT_EQ(access.face_count(), 0U);
    }

    TEST(MeshAccessQueryTest, VertexCountIsBytesOverStride)
    {
        MeshAccess access;
        access.vertex_bytes = 3 * k_stride;
        access.layout.stride_bytes = k_stride;

        EXPECT_EQ(access.vertex_count(), 3U);
    }

    TEST(MeshAccessQueryTest, ZeroStrideGivesNoVertices)
    {
        MeshAccess access;
        access.vertex_bytes = 340;

        EXPECT_EQ(access.vertex_count(), 0U);
    }

    TEST(MeshAccessQueryTest, FaceCountIsIndicesOverThree)
    {
        MeshAccess access;
        access.index_count = 9;

        EXPECT_EQ(access.face_count(), 3U);
    }

    TEST(MeshAccessQueryTest, EmptyRegionGroupIsNotSubmeshes)
    {
        MeshAccess access;
        access.submeshes = RegionGroup("submeshes");

        EXPECT_FALSE(access.has_submeshes());

        access.submeshes->add_region(MeshSubrange {}.to_region());

        EXPECT_TRUE(access.has_submeshes());
    }

    TEST(MeshAccessQueryTest, ValidityNeedsEveryPart)
    {
        const std::vector<uint8_t> vertices = make_vertices(1);
        const std::vector<uint32_t> indices { 0, 0, 0 };

        MeshAccess access;
        access.vertex_ptr = vertices.data();
        access.vertex_bytes = vertices.size();
        access.index_ptr = indices.data();
        access.index_count = 3;
        access.layout.stride_bytes = k_stride;
        EXPECT_TRUE(access.is_valid());

        MeshAccess no_stride = access;
        no_stride.layout.stride_bytes = 0;
        EXPECT_FALSE(no_stride.is_valid());

        MeshAccess no_indices = access;
        no_indices.index_count = 0;
        EXPECT_FALSE(no_indices.is_valid());

        MeshAccess no_vertices = access;
        no_vertices.vertex_bytes = 0;
        EXPECT_FALSE(no_vertices.is_valid());
    }

    TEST(MeshAccessQueryTest, SpansCoverTheBorrowedStorage)
    {
        const std::vector<uint8_t> vertices = make_vertices(2);
        const std::vector<uint32_t> indices { 0, 1, 0 };

        MeshAccess access;
        access.vertex_ptr = vertices.data();
        access.vertex_bytes = vertices.size();
        access.index_ptr = indices.data();
        access.index_count = 3;

        EXPECT_EQ(access.indices().data(), indices.data());
        EXPECT_EQ(access.indices().size(), 3U);
        EXPECT_EQ(static_cast<const void*>(access.vertices().data()), static_cast<const void*>(vertices.data()));
        EXPECT_EQ(access.vertices().size(), vertices.size());
    }

    TEST(AsMeshAccessTest, ViewsTheVariantsWithoutCopying)
    {
        const DataVariant vertices = make_vertices(4);
        const DataVariant indices = std::vector<uint32_t> { 0, 1, 2, 2, 1, 3 };

        const auto access = Kakshya::as_mesh_access(vertices, indices, VertexLayout::for_meshes(k_stride));

        ASSERT_TRUE(access.has_value());
        EXPECT_EQ(access->vertex_ptr, std::get<std::vector<uint8_t>>(vertices).data());
        EXPECT_EQ(access->index_ptr, std::get<std::vector<uint32_t>>(indices).data());
        EXPECT_EQ(access->vertex_bytes, 4U * k_stride);
        EXPECT_EQ(access->index_count, 6U);
        EXPECT_EQ(access->vertex_count(), 4U);
        EXPECT_EQ(access->face_count(), 2U);
        EXPECT_TRUE(access->is_valid());
    }

    TEST(AsMeshAccessTest, LayoutVertexCountIsDerivedFromTheBytes)
    {
        const DataVariant vertices = make_vertices(5);
        const DataVariant indices = std::vector<uint32_t> { 0, 1, 2 };
        VertexLayout layout = VertexLayout::for_meshes(k_stride);
        layout.vertex_count = 999;

        const auto access = Kakshya::as_mesh_access(vertices, indices, layout);

        ASSERT_TRUE(access.has_value());
        EXPECT_EQ(access->layout.vertex_count, 5U);
        EXPECT_EQ(access->layout.stride_bytes, k_stride);
        EXPECT_EQ(access->layout.attributes.size(), layout.attributes.size());
    }

    TEST(AsMeshAccessTest, SubmeshesAreForwarded)
    {
        const DataVariant vertices = make_vertices(3);
        const DataVariant indices = std::vector<uint32_t> { 0, 1, 2 };
        RegionGroup group("submeshes");
        group.add_region(MeshSubrange { .index_start = 0, .index_count = 3, .name = "only" }.to_region());

        const auto access = Kakshya::as_mesh_access(vertices, indices, VertexLayout::for_meshes(k_stride), group);

        ASSERT_TRUE(access.has_value());
        EXPECT_TRUE(access->has_submeshes());
        EXPECT_EQ(access->submeshes->name, "submeshes");
        EXPECT_EQ(text_attribute(access->submeshes->regions[0], "name"), "only");
    }

    TEST(AsMeshAccessTest, WithoutSubmeshesTheyAreAbsent)
    {
        const DataVariant vertices = make_vertices(3);
        const DataVariant indices = std::vector<uint32_t> { 0, 1, 2 };

        const auto access = Kakshya::as_mesh_access(vertices, indices, VertexLayout::for_meshes(k_stride));

        ASSERT_TRUE(access.has_value());
        EXPECT_FALSE(access->submeshes.has_value());
    }

    TEST(AsMeshAccessTest, VertexVariantMustHoldBytes)
    {
        const DataVariant indices = std::vector<uint32_t> { 0, 1, 2 };

        EXPECT_FALSE(Kakshya::as_mesh_access(DataVariant { std::vector<float>(17, 0.0F) }, indices, VertexLayout::for_meshes(k_stride)));
    }

    TEST(AsMeshAccessTest, VertexBytesMustNotBeEmpty)
    {
        const DataVariant indices = std::vector<uint32_t> { 0, 1, 2 };

        EXPECT_FALSE(Kakshya::as_mesh_access(DataVariant { std::vector<uint8_t> {} }, indices, VertexLayout::for_meshes(k_stride)));
    }

    TEST(AsMeshAccessTest, IndexVariantMustHoldUnsigned32)
    {
        const DataVariant vertices = make_vertices(3);

        EXPECT_FALSE(Kakshya::as_mesh_access(vertices, DataVariant { std::vector<uint16_t> { 0, 1, 2 } }, VertexLayout::for_meshes(k_stride)));
    }

    TEST(AsMeshAccessTest, IndicesMustNotBeEmpty)
    {
        const DataVariant vertices = make_vertices(3);

        EXPECT_FALSE(Kakshya::as_mesh_access(vertices, DataVariant { std::vector<uint32_t> {} }, VertexLayout::for_meshes(k_stride)));
    }

    TEST(AsMeshAccessTest, LayoutNeedsAStride)
    {
        const DataVariant vertices = make_vertices(3);
        const DataVariant indices = std::vector<uint32_t> { 0, 1, 2 };
        VertexLayout layout = VertexLayout::for_meshes(k_stride);
        layout.stride_bytes = 0;

        EXPECT_FALSE(Kakshya::as_mesh_access(vertices, indices, layout));
    }

    TEST(AsMeshAccessTest, LayoutNeedsAttributes)
    {
        const DataVariant vertices = make_vertices(3);
        const DataVariant indices = std::vector<uint32_t> { 0, 1, 2 };
        VertexLayout layout;
        layout.stride_bytes = k_stride;

        EXPECT_FALSE(Kakshya::as_mesh_access(vertices, indices, layout));
    }

    TEST(AsMeshAccessTest, VertexBytesMustBeWholeVertices)
    {
        const DataVariant vertices = std::vector<uint8_t>(k_stride * 2 + 1, 0);
        const DataVariant indices = std::vector<uint32_t> { 0, 1, 2 };

        EXPECT_FALSE(Kakshya::as_mesh_access(vertices, indices, VertexLayout::for_meshes(k_stride)));
    }

    TEST(MeshDataTest, EmptyHoldsTypedEmptyStorageAndTheMeshLayout)
    {
        const MeshData data = MeshData::empty();

        EXPECT_TRUE(std::holds_alternative<std::vector<uint8_t>>(data.vertex_variant));
        EXPECT_TRUE(std::holds_alternative<std::vector<uint32_t>>(data.index_variant));
        EXPECT_EQ(data.layout.stride_bytes, k_stride);
        EXPECT_FALSE(data.layout.attributes.empty());
        EXPECT_FALSE(data.submeshes.has_value());
    }

    TEST(MeshDataTest, EmptyIsNotValidAndHasNothingToCount)
    {
        const MeshData data = MeshData::empty();

        EXPECT_FALSE(data.is_valid());
        EXPECT_EQ(data.vertex_count(), 0U);
        EXPECT_EQ(data.face_count(), 0U);
        EXPECT_FALSE(data.access().has_value());
    }

    TEST(MeshDataTest, FilledDataIsValidAndCounted)
    {
        const MeshData data = filled_mesh(4, { 0, 1, 2, 2, 1, 3 });

        EXPECT_TRUE(data.is_valid());
        EXPECT_EQ(data.vertex_count(), 4U);
        EXPECT_EQ(data.face_count(), 2U);
    }

    TEST(MeshDataTest, NeedsBothStreams)
    {
        MeshData no_indices = filled_mesh();
        no_indices.index_variant = std::vector<uint32_t> {};
        MeshData no_vertices = filled_mesh();
        no_vertices.vertex_variant = std::vector<uint8_t> {};

        EXPECT_FALSE(no_indices.is_valid());
        EXPECT_FALSE(no_vertices.is_valid());
    }

    TEST(MeshDataTest, NeedsAStride)
    {
        MeshData data = filled_mesh();
        data.layout.stride_bytes = 0;

        EXPECT_FALSE(data.is_valid());
        EXPECT_EQ(data.vertex_count(), 0U);
    }

    TEST(MeshDataTest, WrongStorageTypesCountAsNothing)
    {
        MeshData data = filled_mesh();
        data.vertex_variant = std::vector<float>(68, 0.0F);
        data.index_variant = std::vector<uint16_t> { 0, 1, 2 };

        EXPECT_FALSE(data.is_valid());
        EXPECT_EQ(data.vertex_count(), 0U);
        EXPECT_EQ(data.face_count(), 0U);
    }

    TEST(MeshDataTest, TrailingIndicesDoNotMakeAFace)
    {
        const MeshData data = filled_mesh(4, { 0, 1, 2, 3 });

        EXPECT_EQ(data.face_count(), 1U);
    }

    TEST(MeshDataTest, AccessBorrowsTheOwnedStorage)
    {
        const MeshData data = filled_mesh();

        const auto access = data.access();

        ASSERT_TRUE(access.has_value());
        EXPECT_EQ(access->vertex_ptr, std::get<std::vector<uint8_t>>(data.vertex_variant).data());
        EXPECT_EQ(access->index_ptr, std::get<std::vector<uint32_t>>(data.index_variant).data());
        EXPECT_EQ(access->vertex_count(), data.vertex_count());
        EXPECT_EQ(access->face_count(), data.face_count());
    }

    TEST(MeshDataTest, AccessCarriesTheSubmeshes)
    {
        MeshData data = filled_mesh();
        data.submeshes = RegionGroup("submeshes");
        data.submeshes->add_region(MeshSubrange { .index_start = 0, .index_count = 6, .name = "all" }.to_region());

        const auto access = data.access();

        ASSERT_TRUE(access.has_value());
        EXPECT_TRUE(access->has_submeshes());
    }

    TEST(MeshDataTest, AccessRejectsPartialVertices)
    {
        MeshData data = filled_mesh();
        std::get<std::vector<uint8_t>>(data.vertex_variant).push_back(0);

        EXPECT_FALSE(data.access().has_value());
    }

    struct InsertionTarget {
        DataVariant vertices;
        DataVariant indices;
        MeshInsertion insertion { vertices, indices };
    };

    TEST(MeshInsertionFlatTest, StoresBytesAndIndices)
    {
        InsertionTarget t;
        const auto bytes = make_vertices(4);
        const std::vector<uint32_t> indices { 0, 1, 2, 2, 1, 3 };

        t.insertion.insert_flat(bytes, indices, VertexLayout::for_meshes(k_stride));

        EXPECT_EQ(std::get<std::vector<uint8_t>>(t.vertices), bytes);
        EXPECT_EQ(std::get<std::vector<uint32_t>>(t.indices), indices);
    }

    TEST(MeshInsertionFlatTest, BuildDescribesTheInsertedMesh)
    {
        InsertionTarget t;
        const auto bytes = make_vertices(4);
        const std::vector<uint32_t> indices { 0, 1, 2, 2, 1, 3 };

        t.insertion.insert_flat(bytes, indices, VertexLayout::for_meshes(k_stride));
        const auto access = t.insertion.build();

        ASSERT_TRUE(access.has_value());
        EXPECT_EQ(access->vertex_count(), 4U);
        EXPECT_EQ(access->face_count(), 2U);
        EXPECT_EQ(access->layout.vertex_count, 4U);
        EXPECT_FALSE(access->submeshes.has_value());
        EXPECT_EQ(access->vertex_ptr, std::get<std::vector<uint8_t>>(t.vertices).data());
    }

    TEST(MeshInsertionFlatTest, InsertionCopiesTheSource)
    {
        InsertionTarget t;
        auto bytes = make_vertices(3);
        std::vector<uint32_t> indices { 0, 1, 2 };

        t.insertion.insert_flat(bytes, indices, VertexLayout::for_meshes(k_stride));
        bytes[0] = 200;
        indices[0] = 2;

        EXPECT_NE(std::get<std::vector<uint8_t>>(t.vertices)[0], 200);
        EXPECT_EQ(std::get<std::vector<uint32_t>>(t.indices)[0], 0U);
    }

    TEST(MeshInsertionFlatTest, ReplacesEarlierContent)
    {
        InsertionTarget t;
        const std::vector<uint32_t> first { 0, 1, 2 };
        const std::vector<uint32_t> second { 0, 1, 2, 0, 2, 3 };

        t.insertion.insert_flat(make_vertices(3), first, VertexLayout::for_meshes(k_stride));
        t.insertion.insert_flat(make_vertices(4), second, VertexLayout::for_meshes(k_stride));
        const auto access = t.insertion.build();

        ASSERT_TRUE(access.has_value());
        EXPECT_EQ(access->vertex_count(), 4U);
        EXPECT_EQ(access->index_count, 6U);
    }

    TEST(MeshInsertionFlatTest, FlatInsertDropsEarlierSubmeshes)
    {
        InsertionTarget t;
        const std::vector<uint32_t> tri { 0, 1, 2 };
        t.insertion.insert_submesh(make_vertices(3), tri, "a");

        t.insertion.insert_flat(make_vertices(3), tri, VertexLayout::for_meshes(k_stride));

        const auto access = t.insertion.build();
        ASSERT_TRUE(access.has_value());
        EXPECT_FALSE(access->submeshes.has_value());
    }

    TEST(MeshInsertionFlatTest, ZeroStrideIsRefusedAndNothingIsWritten)
    {
        InsertionTarget t;
        VertexLayout layout = VertexLayout::for_meshes(k_stride);
        layout.stride_bytes = 0;

        t.insertion.insert_flat(make_vertices(3), std::vector<uint32_t> { 0, 1, 2 }, layout);

        EXPECT_FALSE(t.insertion.build().has_value());
        EXPECT_FALSE(std::holds_alternative<std::vector<uint8_t>>(t.vertices));
    }

    TEST(MeshInsertionFlatTest, PartialVertexBytesAreRefused)
    {
        InsertionTarget t;
        const std::vector<uint8_t> bytes(k_stride + 3, 0);

        t.insertion.insert_flat(bytes, std::vector<uint32_t> { 0, 1, 2 }, VertexLayout::for_meshes(k_stride));

        EXPECT_FALSE(t.insertion.build().has_value());
    }

    TEST(MeshInsertionFlatTest, IndicesMustMakeWholeTriangles)
    {
        InsertionTarget t;

        t.insertion.insert_flat(make_vertices(3), std::vector<uint32_t> { 0, 1, 2, 0 }, VertexLayout::for_meshes(k_stride));

        EXPECT_FALSE(t.insertion.build().has_value());
    }

    TEST(MeshInsertionFlatTest, RefusedInsertKeepsThePreviousMesh)
    {
        InsertionTarget t;
        t.insertion.insert_flat(make_vertices(3), std::vector<uint32_t> { 0, 1, 2 }, VertexLayout::for_meshes(k_stride));

        t.insertion.insert_flat(make_vertices(5), std::vector<uint32_t> { 0, 1 }, VertexLayout::for_meshes(k_stride));

        const auto access = t.insertion.build();
        ASSERT_TRUE(access.has_value());
        EXPECT_EQ(access->vertex_count(), 3U);
    }

    TEST(MeshInsertionFlatTest, EmptyMeshBuildsNothing)
    {
        InsertionTarget t;

        t.insertion.insert_flat({}, {}, VertexLayout::for_meshes(k_stride));

        EXPECT_FALSE(t.insertion.build().has_value());
    }

    TEST(MeshInsertionBuildTest, NothingInsertedBuildsNothing)
    {
        InsertionTarget t;

        EXPECT_FALSE(t.insertion.build().has_value());
    }

    TEST(MeshInsertionSubmeshTest, SingleSubmeshKeepsLocalIndices)
    {
        InsertionTarget t;

        t.insertion.insert_submesh(make_vertices(3), std::vector<uint32_t> { 0, 1, 2 }, "tri", "matte");

        EXPECT_EQ(std::get<std::vector<uint32_t>>(t.indices), (std::vector<uint32_t> { 0, 1, 2 }));
        EXPECT_EQ(std::get<std::vector<uint8_t>>(t.vertices).size(), 3U * k_stride);
    }

    TEST(MeshInsertionSubmeshTest, LaterSubmeshIndicesAreOffsetByTheVerticesBefore)
    {
        InsertionTarget t;

        t.insertion.insert_submesh(make_vertices(3), std::vector<uint32_t> { 0, 1, 2 });
        t.insertion.insert_submesh(make_vertices(4), std::vector<uint32_t> { 0, 1, 2, 2, 1, 3 });
        t.insertion.insert_submesh(make_vertices(3), std::vector<uint32_t> { 2, 1, 0 });

        EXPECT_EQ(std::get<std::vector<uint32_t>>(t.indices),
            (std::vector<uint32_t> { 0, 1, 2, 3, 4, 5, 5, 4, 6, 9, 8, 7 }));
        EXPECT_EQ(std::get<std::vector<uint8_t>>(t.vertices).size(), 10U * k_stride);
    }

    TEST(MeshInsertionSubmeshTest, VertexBytesAreConcatenatedInOrder)
    {
        InsertionTarget t;
        const auto first = make_vertices(2);
        const std::vector<uint8_t> second(2 * k_stride, 7);

        t.insertion.insert_submesh(first, std::vector<uint32_t> { 0, 1, 0 });
        t.insertion.insert_submesh(second, std::vector<uint32_t> { 0, 1, 0 });

        const auto& all = std::get<std::vector<uint8_t>>(t.vertices);
        ASSERT_EQ(all.size(), first.size() + second.size());
        EXPECT_TRUE(std::equal(first.begin(), first.end(), all.begin()));
        EXPECT_TRUE(std::equal(second.begin(), second.end(), all.begin() + static_cast<std::ptrdiff_t>(first.size())));
    }

    TEST(MeshInsertionSubmeshTest, EachSubmeshBecomesARegionWithItsNames)
    {
        InsertionTarget t;

        t.insertion.insert_submesh(make_vertices(3), std::vector<uint32_t> { 0, 1, 2 }, "hull", "steel");
        t.insertion.insert_submesh(make_vertices(4), std::vector<uint32_t> { 0, 1, 2, 2, 1, 3 }, "deck", "wood");
        const auto access = t.insertion.build();

        ASSERT_TRUE(access.has_value());
        ASSERT_TRUE(access->has_submeshes());
        EXPECT_EQ(access->submeshes->name, "submeshes");
        ASSERT_EQ(access->submeshes->regions.size(), 2U);

        const Region& hull = access->submeshes->regions[0];
        EXPECT_EQ(text_attribute(hull, "name"), "hull");
        EXPECT_EQ(text_attribute(hull, "material_name"), "steel");
        EXPECT_EQ(hull.start_coordinates, (std::vector<uint64_t> { 0 }));
        EXPECT_EQ(hull.end_coordinates, (std::vector<uint64_t> { 3 }));
        EXPECT_EQ(number_attribute(hull, "vertex_offset"), 0U);

        const Region& deck = access->submeshes->regions[1];
        EXPECT_EQ(text_attribute(deck, "name"), "deck");
        EXPECT_EQ(text_attribute(deck, "material_name"), "wood");
        EXPECT_EQ(deck.start_coordinates, (std::vector<uint64_t> { 3 }));
        EXPECT_EQ(deck.end_coordinates, (std::vector<uint64_t> { 9 }));
        EXPECT_EQ(number_attribute(deck, "vertex_offset"), 3U);
        EXPECT_EQ(number_attribute(deck, "index_count"), 6U);
    }

    TEST(MeshInsertionSubmeshTest, BuildTotalsAcrossSubmeshes)
    {
        InsertionTarget t;

        t.insertion.insert_submesh(make_vertices(3), std::vector<uint32_t> { 0, 1, 2 });
        t.insertion.insert_submesh(make_vertices(4), std::vector<uint32_t> { 0, 1, 2, 2, 1, 3 });
        const auto access = t.insertion.build();

        ASSERT_TRUE(access.has_value());
        EXPECT_EQ(access->vertex_count(), 7U);
        EXPECT_EQ(access->layout.vertex_count, 7U);
        EXPECT_EQ(access->face_count(), 3U);
    }

    TEST(MeshInsertionSubmeshTest, NamesAreOptional)
    {
        InsertionTarget t;

        t.insertion.insert_submesh(make_vertices(3), std::vector<uint32_t> { 0, 1, 2 });
        const auto access = t.insertion.build();

        ASSERT_TRUE(access.has_value());
        EXPECT_TRUE(text_attribute(access->submeshes->regions[0], "name").empty());
        EXPECT_TRUE(text_attribute(access->submeshes->regions[0], "material_name").empty());
    }

    TEST(MeshInsertionSubmeshTest, StrideMismatchSkipsTheBatch)
    {
        InsertionTarget t;
        t.insertion.insert_submesh(make_vertices(3), std::vector<uint32_t> { 0, 1, 2 }, "a");

        t.insertion.insert_submesh(make_vertices(3, 32), std::vector<uint32_t> { 0, 1, 2 }, "b", {}, position_only_layout(32));

        const auto access = t.insertion.build();
        ASSERT_TRUE(access.has_value());
        EXPECT_EQ(access->vertex_count(), 3U);
        EXPECT_EQ(access->index_count, 3U);
        EXPECT_EQ(access->submeshes->regions.size(), 1U);
    }

    TEST(MeshInsertionSubmeshTest, AttributeCountMismatchSkipsTheBatch)
    {
        InsertionTarget t;
        t.insertion.insert_submesh(make_vertices(3), std::vector<uint32_t> { 0, 1, 2 }, "a");

        t.insertion.insert_submesh(make_vertices(3), std::vector<uint32_t> { 0, 1, 2 }, "b", {}, position_only_layout(k_stride));

        const auto access = t.insertion.build();
        ASSERT_TRUE(access.has_value());
        EXPECT_EQ(access->submeshes->regions.size(), 1U);
        EXPECT_EQ(access->vertex_count(), 3U);
    }

    TEST(MeshInsertionSubmeshTest, FirstSubmeshEstablishesTheLayout)
    {
        InsertionTarget t;

        t.insertion.insert_submesh(make_vertices(2, 12), std::vector<uint32_t> { 0, 1, 0 }, "p", {}, position_only_layout(12));
        t.insertion.insert_submesh(make_vertices(2, 12), std::vector<uint32_t> { 0, 1, 0 }, "q", {}, position_only_layout(12));
        const auto access = t.insertion.build();

        ASSERT_TRUE(access.has_value());
        EXPECT_EQ(access->layout.stride_bytes, 12U);
        EXPECT_EQ(access->layout.attributes.size(), 1U);
        EXPECT_EQ(access->vertex_count(), 4U);
    }

    TEST(MeshInsertionSubmeshTest, InvalidBatchesAreRefused)
    {
        InsertionTarget t;
        VertexLayout no_stride = VertexLayout::for_meshes(k_stride);
        no_stride.stride_bytes = 0;

        t.insertion.insert_submesh(make_vertices(3), std::vector<uint32_t> { 0, 1, 2 }, "a", {}, no_stride);
        t.insertion.insert_submesh(std::vector<uint8_t>(k_stride + 1, 0), std::vector<uint32_t> { 0, 1, 2 }, "b");
        t.insertion.insert_submesh(make_vertices(3), std::vector<uint32_t> { 0, 1 }, "c");

        EXPECT_FALSE(t.insertion.build().has_value());
        EXPECT_FALSE(std::holds_alternative<std::vector<uint32_t>>(t.indices));
    }

    TEST(MeshInsertionSubmeshTest, RefusedBatchDoesNotShiftLaterOffsets)
    {
        InsertionTarget t;
        t.insertion.insert_submesh(make_vertices(3), std::vector<uint32_t> { 0, 1, 2 }, "a");
        t.insertion.insert_submesh(make_vertices(5), std::vector<uint32_t> { 0, 1 }, "bad");

        t.insertion.insert_submesh(make_vertices(3), std::vector<uint32_t> { 0, 1, 2 }, "c");

        EXPECT_EQ(std::get<std::vector<uint32_t>>(t.indices), (std::vector<uint32_t> { 0, 1, 2, 3, 4, 5 }));
    }

    TEST(MeshInsertionSubmeshTest, WrongTypedVariantsAreReplacedWithTheCanonicalStorage)
    {
        InsertionTarget t;
        t.vertices = std::vector<float>(4, 1.0F);
        t.indices = std::vector<uint16_t> { 1, 2 };

        t.insertion.insert_submesh(make_vertices(3), std::vector<uint32_t> { 0, 1, 2 });

        EXPECT_TRUE(std::holds_alternative<std::vector<uint8_t>>(t.vertices));
        EXPECT_TRUE(std::holds_alternative<std::vector<uint32_t>>(t.indices));
        EXPECT_TRUE(t.insertion.build().has_value());
    }

    TEST(MeshInsertionClearTest, ResetsStorageLayoutAndSubmeshes)
    {
        InsertionTarget t;
        t.insertion.insert_submesh(make_vertices(3), std::vector<uint32_t> { 0, 1, 2 }, "a");

        t.insertion.clear();

        EXPECT_TRUE(std::get<std::vector<uint8_t>>(t.vertices).empty());
        EXPECT_TRUE(std::get<std::vector<uint32_t>>(t.indices).empty());
        EXPECT_FALSE(t.insertion.build().has_value());
    }

    TEST(MeshInsertionClearTest, AllowsADifferentLayoutAfterwards)
    {
        InsertionTarget t;
        t.insertion.insert_submesh(make_vertices(3), std::vector<uint32_t> { 0, 1, 2 });
        t.insertion.clear();

        t.insertion.insert_submesh(make_vertices(2, 12), std::vector<uint32_t> { 0, 1, 0 }, "p", {}, position_only_layout(12));
        const auto access = t.insertion.build();

        ASSERT_TRUE(access.has_value());
        EXPECT_EQ(access->layout.stride_bytes, 12U);
        EXPECT_EQ(access->vertex_count(), 2U);
        EXPECT_EQ(access->submeshes->regions.size(), 1U);
        EXPECT_EQ(access->submeshes->regions[0].start_coordinates, (std::vector<uint64_t> { 0 }));
    }

    TEST(MeshInsertionRoundTripTest, BuiltAccessMatchesMeshDataAccess)
    {
        InsertionTarget t;
        const std::vector<uint32_t> indices { 0, 1, 2, 2, 1, 3 };
        t.insertion.insert_flat(make_vertices(4), indices, VertexLayout::for_meshes(k_stride));

        MeshData data = MeshData::empty();
        data.vertex_variant = t.vertices;
        data.index_variant = t.indices;

        const auto built = t.insertion.build();
        const auto viewed = data.access();

        ASSERT_TRUE(built.has_value());
        ASSERT_TRUE(viewed.has_value());
        EXPECT_EQ(built->vertex_count(), viewed->vertex_count());
        EXPECT_EQ(built->face_count(), viewed->face_count());
        EXPECT_TRUE(std::ranges::equal(built->vertices(), viewed->vertices()));
        EXPECT_TRUE(std::ranges::equal(built->indices(), viewed->indices()));
    }

} // namespace
} // namespace MayaFlux::Test
