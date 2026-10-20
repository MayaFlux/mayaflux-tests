#include "gtest/gtest.h"

#include "MayaFlux/Nodes/Graphics/GeometryWriterNode.hpp"
#include "MayaFlux/Nodes/Graphics/TextureNode.hpp"

namespace MayaFlux::Test {

namespace {

    struct ProbeVertex {
        float x;
        float y;
        float z;

        bool operator==(const ProbeVertex&) const = default;
    };

    class ProbeGeometry : public Nodes::GpuSync::GeometryWriterNode {
    public:
        using GeometryWriterNode::GeometryWriterNode;

        void compute_frame() override { ++frames; }

        int frames { 0 };
    };

    class ProbeTexture : public Nodes::GpuSync::TextureNode {
    public:
        using TextureNode::TextureNode;

        void compute_frame() override { ++frames; }

        using TextureNode::clear;
        using TextureNode::fill;
        using TextureNode::get_pixel;
        using TextureNode::set_pixel;

        int frames { 0 };
    };

    std::vector<ProbeVertex> make_vertices(uint32_t count)
    {
        std::vector<ProbeVertex> v;
        for (uint32_t i = 0; i < count; ++i) {
            const auto f = static_cast<float>(i);
            v.push_back({ f + 1.0F, f + 2.0F, f + 3.0F });
        }
        return v;
    }

} // namespace

class GpuSyncBaseTest : public ::testing::Test {
protected:
    ProbeGeometry geometry { 0 };
    ProbeTexture texture { 4, 4 };
};

TEST_F(GpuSyncBaseTest, ProcessSampleRunsComputeFrameOnceAndReturnsZero)
{
    EXPECT_DOUBLE_EQ(geometry.process_sample(0.75), 0.0);
    EXPECT_EQ(geometry.frames, 1);

    EXPECT_DOUBLE_EQ(texture.process_sample(-3.0), 0.0);
    EXPECT_EQ(texture.frames, 1);
}

TEST_F(GpuSyncBaseTest, ProcessBatchReturnsZeroFilledVectorOfRequestedSize)
{
    auto g = geometry.process_batch(8);
    ASSERT_EQ(g.size(), 8U);
    for (double s : g) {
        EXPECT_DOUBLE_EQ(s, 0.0);
    }

    auto t = texture.process_batch(5);
    ASSERT_EQ(t.size(), 5U);
    for (double s : t) {
        EXPECT_DOUBLE_EQ(s, 0.0);
    }
}

TEST_F(GpuSyncBaseTest, TextureBatchComputesOneFramePerSample)
{
    texture.process_batch(6);
    EXPECT_EQ(texture.frames, 6);
}

class GeometryWriterBaseTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        node = std::make_shared<ProbeGeometry>(0);
        node->set_vertex_stride(sizeof(ProbeVertex));
    }

    void load(uint32_t count)
    {
        const auto v = make_vertices(count);
        node->set_vertex_data(v.data(), v.size() * sizeof(ProbeVertex));
    }

    std::shared_ptr<ProbeGeometry> node;
};

TEST_F(GeometryWriterBaseTest, DefaultCapacityReservesThatManyDefaultStrideVertices)
{
    ProbeGeometry sized { 16 };

    EXPECT_EQ(sized.get_vertex_stride(), sizeof(float) * 3);
    EXPECT_EQ(sized.get_vertex_buffer_size_bytes(), 16 * sizeof(float) * 3);
    EXPECT_EQ(sized.get_vertex_count(), 0U);
}

TEST_F(GeometryWriterBaseTest, ZeroCapacityStartsEmptyWithNoStride)
{
    ProbeGeometry empty { 0 };

    EXPECT_EQ(empty.get_vertex_stride(), 0U);
    EXPECT_EQ(empty.get_vertex_buffer_size_bytes(), 0U);
    EXPECT_EQ(empty.get_vertex_count(), 0U);
    EXPECT_FALSE(empty.has_indices());
}

TEST_F(GeometryWriterBaseTest, FreshNodeNeedsGpuUpdate)
{
    ProbeGeometry fresh { 0 };
    EXPECT_TRUE(fresh.needs_gpu_update());
    EXPECT_TRUE(fresh.needs_vertex_data_update());
}

TEST_F(GeometryWriterBaseTest, ResizeWithoutStrideDoesNothing)
{
    ProbeGeometry nostride { 0 };
    nostride.resize_vertex_buffer(10);

    EXPECT_EQ(nostride.get_vertex_count(), 0U);
    EXPECT_EQ(nostride.get_vertex_buffer_size_bytes(), 0U);
}

TEST_F(GeometryWriterBaseTest, ResizeSetsCountAndZeroedByteSize)
{
    node->resize_vertex_buffer(5);

    EXPECT_EQ(node->get_vertex_count(), 5U);
    EXPECT_EQ(node->get_vertex_buffer_size_bytes(), 5 * sizeof(ProbeVertex));
    for (uint8_t b : node->get_vertex_data()) {
        EXPECT_EQ(b, 0);
    }
}

TEST_F(GeometryWriterBaseTest, ResizeFlagsDataAndLayoutDirty)
{
    node->clear_gpu_update_flag();
    ASSERT_FALSE(node->needs_gpu_update());

    node->resize_vertex_buffer(3);

    EXPECT_TRUE(node->needs_vertex_data_update());
    EXPECT_TRUE(node->needs_layout_update());
}

TEST_F(GeometryWriterBaseTest, ResizePreservingDataKeepsExistingVerticesWhenGrowing)
{
    load(2);
    node->resize_vertex_buffer(4, true);

    EXPECT_EQ(node->get_vertex_count(), 4U);
    EXPECT_EQ(node->get_vertex_buffer_size_bytes(), 4 * sizeof(ProbeVertex));
    EXPECT_EQ(node->get_vertex_typed<ProbeVertex>(0), (ProbeVertex { 1, 2, 3 }));
    EXPECT_EQ(node->get_vertex_typed<ProbeVertex>(1), (ProbeVertex { 2, 3, 4 }));
    EXPECT_EQ(node->get_vertex_typed<ProbeVertex>(3), (ProbeVertex { 0, 0, 0 }));
}

TEST_F(GeometryWriterBaseTest, ResizePreservingDataTruncatesWhenShrinking)
{
    load(4);
    node->resize_vertex_buffer(2, true);

    EXPECT_EQ(node->get_vertex_count(), 2U);
    EXPECT_EQ(node->get_vertex_buffer_size_bytes(), 2 * sizeof(ProbeVertex));
    EXPECT_EQ(node->get_vertex_typed<ProbeVertex>(1), (ProbeVertex { 2, 3, 4 }));
}

TEST_F(GeometryWriterBaseTest, ResizeWithoutPreservingDiscardsContents)
{
    load(3);
    node->resize_vertex_buffer(3, false);

    EXPECT_EQ(node->get_vertex_typed<ProbeVertex>(0), (ProbeVertex { 0, 0, 0 }));
    EXPECT_EQ(node->get_vertex_typed<ProbeVertex>(2), (ProbeVertex { 0, 0, 0 }));
}

TEST_F(GeometryWriterBaseTest, SetVertexDataStoresBytesAndDerivesCount)
{
    const auto v = make_vertices(3);
    node->set_vertex_data(v.data(), v.size() * sizeof(ProbeVertex));

    EXPECT_EQ(node->get_vertex_count(), 3U);
    auto bytes = node->get_vertex_data();
    ASSERT_GE(bytes.size(), 3 * sizeof(ProbeVertex));
    for (uint32_t i = 0; i < 3; ++i) {
        EXPECT_EQ(node->get_vertex_typed<ProbeVertex>(i), v[i]) << "vertex " << i;
    }
}

TEST_F(GeometryWriterBaseTest, SetVertexDataFlagsDataAndLayoutDirty)
{
    node->clear_gpu_update_flag();
    load(2);

    EXPECT_TRUE(node->needs_vertex_data_update());
    EXPECT_TRUE(node->needs_layout_update());
}

TEST_F(GeometryWriterBaseTest, SetVertexDataRejectsNullAndZeroSize)
{
    load(2);
    node->clear_gpu_update_flag();

    node->set_vertex_data(nullptr, 24);
    const auto v = make_vertices(1);
    node->set_vertex_data(v.data(), 0);

    EXPECT_EQ(node->get_vertex_count(), 2U);
    EXPECT_FALSE(node->needs_gpu_update());
}

TEST_F(GeometryWriterBaseTest, SetVertexDataWithPartialTrailingVertexCountsWholeVerticesOnly)
{
    const auto v = make_vertices(3);
    node->set_vertex_data(v.data(), 2 * sizeof(ProbeVertex) + 4);

    EXPECT_EQ(node->get_vertex_count(), 2U);
}

TEST_F(GeometryWriterBaseTest, SetVertexDataWithoutStrideIsRejected)
{
    ProbeGeometry nostride { 0 };
    const auto v = make_vertices(2);
    nostride.set_vertex_data(v.data(), v.size() * sizeof(ProbeVertex));

    EXPECT_EQ(nostride.get_vertex_count(), 0U);
}

TEST_F(GeometryWriterBaseTest, SetVertexOverwritesOnlyTheTargetVertex)
{
    load(3);
    const ProbeVertex replacement { 9, 8, 7 };
    node->set_vertex(1, &replacement, sizeof(replacement));

    EXPECT_EQ(node->get_vertex_typed<ProbeVertex>(0), (ProbeVertex { 1, 2, 3 }));
    EXPECT_EQ(node->get_vertex_typed<ProbeVertex>(1), replacement);
    EXPECT_EQ(node->get_vertex_typed<ProbeVertex>(2), (ProbeVertex { 3, 4, 5 }));
}

TEST_F(GeometryWriterBaseTest, SetVertexMarksDataDirtyButNotLayout)
{
    load(2);
    node->clear_gpu_update_flag();

    const ProbeVertex replacement { 9, 8, 7 };
    node->set_vertex(0, &replacement, sizeof(replacement));

    EXPECT_TRUE(node->needs_vertex_data_update());
    EXPECT_FALSE(node->needs_layout_update());
}

TEST_F(GeometryWriterBaseTest, SetVertexOutOfRangeIsIgnored)
{
    load(2);
    node->clear_gpu_update_flag();

    const ProbeVertex replacement { 9, 8, 7 };
    node->set_vertex(2, &replacement, sizeof(replacement));
    node->set_vertex(100, &replacement, sizeof(replacement));

    EXPECT_FALSE(node->needs_gpu_update());
    EXPECT_EQ(node->get_vertex_count(), 2U);
}

TEST_F(GeometryWriterBaseTest, SetVertexLargerThanStrideDoesNotClobberNeighbour)
{
    load(2);
    const float oversize[6] = { 9, 9, 9, 7, 7, 7 };
    node->set_vertex(0, oversize, sizeof(oversize));

    EXPECT_EQ(node->get_vertex_typed<ProbeVertex>(0), (ProbeVertex { 9, 9, 9 }));
    EXPECT_EQ(node->get_vertex_typed<ProbeVertex>(1), (ProbeVertex { 2, 3, 4 }));
}

TEST_F(GeometryWriterBaseTest, SetVertexNullOrZeroSizeIsIgnored)
{
    load(2);
    node->clear_gpu_update_flag();

    node->set_vertex(0, nullptr, sizeof(ProbeVertex));
    const ProbeVertex v { 5, 5, 5 };
    node->set_vertex(0, &v, 0);

    EXPECT_FALSE(node->needs_gpu_update());
    EXPECT_EQ(node->get_vertex_typed<ProbeVertex>(0), (ProbeVertex { 1, 2, 3 }));
}

TEST_F(GeometryWriterBaseTest, GetVertexReturnsExactlyOneStrideOfBytes)
{
    load(3);

    for (uint32_t i = 0; i < 3; ++i) {
        auto bytes = node->get_vertex(i);
        ASSERT_EQ(bytes.size(), sizeof(ProbeVertex)) << "vertex " << i;
    }
}

TEST_F(GeometryWriterBaseTest, GetVertexAddressesTheRequestedVertex)
{
    const auto v = make_vertices(3);
    node->set_vertex_data(v.data(), v.size() * sizeof(ProbeVertex));

    for (uint32_t i = 0; i < 3; ++i) {
        auto bytes = node->get_vertex(i);
        ASSERT_EQ(bytes.size(), sizeof(ProbeVertex));
        EXPECT_EQ(node->get_vertex_typed<ProbeVertex>(i), v[i]) << "vertex " << i;
    }
}

TEST_F(GeometryWriterBaseTest, GetVertexOutOfRangeOrEmptyIsEmpty)
{
    EXPECT_TRUE(node->get_vertex(0).empty());

    load(2);
    EXPECT_TRUE(node->get_vertex(2).empty());
    EXPECT_TRUE(node->get_vertex(999).empty());
}

TEST_F(GeometryWriterBaseTest, TypedVertexRoundTrip)
{
    node->resize_vertex_buffer(4);
    node->set_vertex_typed(0, ProbeVertex { 1, 2, 3 });
    node->set_vertex_typed(3, ProbeVertex { 4, 5, 6 });

    EXPECT_EQ(node->get_vertex_typed<ProbeVertex>(0), (ProbeVertex { 1, 2, 3 }));
    EXPECT_EQ(node->get_vertex_typed<ProbeVertex>(3), (ProbeVertex { 4, 5, 6 }));
    EXPECT_EQ(node->get_vertex_typed<ProbeVertex>(1), (ProbeVertex { 0, 0, 0 }));
}

TEST_F(GeometryWriterBaseTest, GetVertexTypedBeyondRangeReturnsDefault)
{
    load(1);
    EXPECT_EQ(node->get_vertex_typed<ProbeVertex>(5), (ProbeVertex { 0, 0, 0 }));
}

TEST_F(GeometryWriterBaseTest, SetVerticesAdoptsStrideCountAndContents)
{
    ProbeGeometry fresh { 0 };
    const auto v = make_vertices(4);
    fresh.set_vertices<ProbeVertex>(v);

    EXPECT_EQ(fresh.get_vertex_stride(), sizeof(ProbeVertex));
    EXPECT_EQ(fresh.get_vertex_count(), 4U);
    EXPECT_EQ(fresh.get_vertex_buffer_size_bytes(), 4 * sizeof(ProbeVertex));
    for (uint32_t i = 0; i < 4; ++i) {
        EXPECT_EQ(fresh.get_vertex_typed<ProbeVertex>(i), v[i]);
    }
    EXPECT_TRUE(fresh.needs_layout_update());
    EXPECT_TRUE(fresh.needs_vertex_data_update());
}

TEST_F(GeometryWriterBaseTest, IndicesRoundTrip)
{
    EXPECT_FALSE(node->has_indices());
    EXPECT_EQ(node->get_index_count(), 0U);

    const std::vector<uint32_t> idx { 0, 2, 1, 2, 3, 1 };
    node->clear_gpu_update_flag();
    node->set_indices(idx);

    EXPECT_TRUE(node->has_indices());
    EXPECT_EQ(node->get_index_count(), 6U);
    auto out = node->get_index_data();
    ASSERT_EQ(out.size(), idx.size());
    EXPECT_TRUE(std::equal(out.begin(), out.end(), idx.begin()));
    EXPECT_TRUE(node->needs_vertex_data_update());
}

TEST_F(GeometryWriterBaseTest, SetIndicesReplacesPreviousIndices)
{
    node->set_indices(std::vector<uint32_t> { 1, 2, 3, 4 });
    node->set_indices(std::vector<uint32_t> { 7, 8 });

    EXPECT_EQ(node->get_index_count(), 2U);
}

TEST_F(GeometryWriterBaseTest, ClearZeroesVerticesAndIndicesAndKeepsCount)
{
    load(3);
    node->set_indices(std::vector<uint32_t> { 1, 2, 3 });
    node->clear_gpu_update_flag();

    node->clear();

    EXPECT_EQ(node->get_vertex_count(), 3U);
    for (uint8_t b : node->get_vertex_data()) {
        EXPECT_EQ(b, 0);
    }
    for (uint32_t i : node->get_index_data()) {
        EXPECT_EQ(i, 0U);
    }
    EXPECT_TRUE(node->needs_vertex_data_update());
}

TEST_F(GeometryWriterBaseTest, ClearAndResizeZeroesAndSetsCount)
{
    load(3);
    node->clear_and_resize(6);

    EXPECT_EQ(node->get_vertex_count(), 6U);
    EXPECT_EQ(node->get_vertex_buffer_size_bytes(), 6 * sizeof(ProbeVertex));
    for (uint8_t b : node->get_vertex_data()) {
        EXPECT_EQ(b, 0);
    }
}

TEST_F(GeometryWriterBaseTest, GpuUpdateFlagCycle)
{
    load(2);
    ASSERT_TRUE(node->needs_gpu_update());

    node->clear_gpu_update_flag();
    EXPECT_FALSE(node->needs_gpu_update());
    EXPECT_FALSE(node->needs_vertex_data_update());
    EXPECT_FALSE(node->needs_layout_update());

    node->mark_vertex_data_dirty(true);
    EXPECT_TRUE(node->needs_gpu_update());

    node->mark_vertex_data_dirty(false);
    EXPECT_FALSE(node->needs_gpu_update());
}

TEST_F(GeometryWriterBaseTest, LayoutFlagAloneKeepsGpuUpdateRequested)
{
    load(2);
    node->clear_gpu_update_flag();

    node->set_vertex_stride(sizeof(ProbeVertex));
    EXPECT_TRUE(node->needs_layout_update());
    EXPECT_TRUE(node->needs_gpu_update());
    EXPECT_FALSE(node->needs_vertex_data_update());

    node->clear_layout_update_flag();
    EXPECT_FALSE(node->needs_gpu_update());
}

TEST_F(GeometryWriterBaseTest, VertexLayoutStoredAndReturned)
{
    EXPECT_FALSE(node->get_vertex_layout().has_value());

    node->set_vertex_layout(Kakshya::VertexLayout {});
    EXPECT_TRUE(node->get_vertex_layout().has_value());
}

TEST_F(GeometryWriterBaseTest, PrimitiveTopologyRoundTrip)
{
    node->set_primitive_topology(Portal::Graphics::PrimitiveTopology::LINE_LIST);
    EXPECT_EQ(node->get_primitive_topology(), Portal::Graphics::PrimitiveTopology::LINE_LIST);

    node->set_primitive_topology(Portal::Graphics::PrimitiveTopology::TRIANGLE_LIST);
    EXPECT_EQ(node->get_primitive_topology(), Portal::Graphics::PrimitiveTopology::TRIANGLE_LIST);
}

TEST_F(GeometryWriterBaseTest, RestoreStateReturnsVerticesStrideCountAndIndices)
{
    load(3);
    node->set_indices(std::vector<uint32_t> { 0, 1, 2 });
    node->save_state();

    node->set_vertex_stride(sizeof(float));
    node->clear_and_resize(10);
    node->set_indices(std::vector<uint32_t> { 5 });

    node->restore_state();

    EXPECT_EQ(node->get_vertex_stride(), sizeof(ProbeVertex));
    EXPECT_EQ(node->get_vertex_count(), 3U);
    EXPECT_EQ(node->get_vertex_typed<ProbeVertex>(0), (ProbeVertex { 1, 2, 3 }));
    EXPECT_EQ(node->get_vertex_typed<ProbeVertex>(2), (ProbeVertex { 3, 4, 5 }));
    EXPECT_EQ(node->get_index_count(), 3U);
}

TEST_F(GeometryWriterBaseTest, RestoreStateRequestsLayoutUpdate)
{
    load(2);
    node->save_state();
    node->clear_gpu_update_flag();

    node->restore_state();

    EXPECT_TRUE(node->needs_gpu_update());
}

TEST_F(GeometryWriterBaseTest, RestoreStateRestoresSavedLayout)
{
    node->save_state();
    node->set_vertex_layout(Kakshya::VertexLayout {});
    ASSERT_TRUE(node->get_vertex_layout().has_value());

    node->restore_state();

    EXPECT_FALSE(node->get_vertex_layout().has_value());
}

TEST_F(GeometryWriterBaseTest, RestoreWithoutSaveLeavesNodeUntouched)
{
    load(2);
    node->restore_state();

    EXPECT_EQ(node->get_vertex_count(), 2U);
    EXPECT_EQ(node->get_vertex_typed<ProbeVertex>(1), (ProbeVertex { 2, 3, 4 }));
}

TEST_F(GeometryWriterBaseTest, ContextReflectsCountStrideAndBytes)
{
    load(3);
    node->update_context(0.5);

    auto& ctx = dynamic_cast<Nodes::GpuSync::GeometryContext&>(node->get_last_context());
    EXPECT_DOUBLE_EQ(ctx.value, 0.5);
    EXPECT_EQ(ctx.vertex_count, 3U);
    EXPECT_EQ(ctx.vertex_stride, sizeof(ProbeVertex));
}

class TextureBaseTest : public ::testing::Test {
protected:
    ProbeTexture tex { 4, 3 };
};

TEST_F(TextureBaseTest, ConstructionAllocatesRgbaFloatBuffer)
{
    EXPECT_EQ(tex.get_width(), 4U);
    EXPECT_EQ(tex.get_height(), 3U);
    EXPECT_EQ(tex.get_pixel_count(), 12U);
    EXPECT_EQ(tex.get_pixel_buffer().size(), 12U * 4);
    EXPECT_EQ(tex.get_buffer_size(), 12U * 4 * sizeof(float));
}

TEST_F(TextureBaseTest, ConstructionStartsZeroedAndDirty)
{
    for (float f : tex.get_pixel_buffer()) {
        EXPECT_FLOAT_EQ(f, 0.0F);
    }
    EXPECT_TRUE(tex.needs_gpu_update());
}

TEST_F(TextureBaseTest, ZeroDimensionsThrow)
{
    EXPECT_THROW(ProbeTexture(0, 4), std::invalid_argument);
    EXPECT_THROW(ProbeTexture(4, 0), std::invalid_argument);
}

TEST_F(TextureBaseTest, SetAndGetPixelRoundTrip)
{
    tex.set_pixel(2, 1, 0.1F, 0.2F, 0.3F, 0.4F);

    auto p = tex.get_pixel(2, 1);
    EXPECT_FLOAT_EQ(p[0], 0.1F);
    EXPECT_FLOAT_EQ(p[1], 0.2F);
    EXPECT_FLOAT_EQ(p[2], 0.3F);
    EXPECT_FLOAT_EQ(p[3], 0.4F);
}

TEST_F(TextureBaseTest, SetPixelDefaultsAlphaToOne)
{
    tex.set_pixel(0, 0, 0.5F, 0.5F, 0.5F);
    EXPECT_FLOAT_EQ(tex.get_pixel(0, 0)[3], 1.0F);
}

TEST_F(TextureBaseTest, PixelsAreStoredRowMajorRgba)
{
    tex.set_pixel(1, 0, 1.0F, 2.0F, 3.0F, 4.0F);
    tex.set_pixel(0, 1, 5.0F, 6.0F, 7.0F, 8.0F);

    auto buf = tex.get_pixel_buffer();
    EXPECT_FLOAT_EQ(buf[4], 1.0F);
    EXPECT_FLOAT_EQ(buf[7], 4.0F);
    EXPECT_FLOAT_EQ(buf[16], 5.0F);
    EXPECT_FLOAT_EQ(buf[19], 8.0F);
}

TEST_F(TextureBaseTest, SetPixelDoesNotTouchOtherPixels)
{
    tex.set_pixel(1, 1, 1.0F, 1.0F, 1.0F, 1.0F);

    EXPECT_FLOAT_EQ(tex.get_pixel(0, 1)[0], 0.0F);
    EXPECT_FLOAT_EQ(tex.get_pixel(2, 1)[0], 0.0F);
    EXPECT_FLOAT_EQ(tex.get_pixel(1, 0)[0], 0.0F);
    EXPECT_FLOAT_EQ(tex.get_pixel(1, 2)[0], 0.0F);
}

TEST_F(TextureBaseTest, OutOfBoundsAccessIsInertAndReadsZero)
{
    tex.clear_gpu_update_flag();

    tex.set_pixel(4, 0, 1.0F, 1.0F, 1.0F, 1.0F);
    tex.set_pixel(0, 3, 1.0F, 1.0F, 1.0F, 1.0F);

    EXPECT_FALSE(tex.needs_gpu_update());
    for (float f : tex.get_pixel_buffer()) {
        EXPECT_FLOAT_EQ(f, 0.0F);
    }

    auto p = tex.get_pixel(100, 100);
    EXPECT_FLOAT_EQ(p[0], 0.0F);
    EXPECT_FLOAT_EQ(p[3], 0.0F);
}

TEST_F(TextureBaseTest, FillWritesEveryPixel)
{
    tex.fill(0.25F, 0.5F, 0.75F, 1.0F);

    for (uint32_t y = 0; y < tex.get_height(); ++y) {
        for (uint32_t x = 0; x < tex.get_width(); ++x) {
            auto p = tex.get_pixel(x, y);
            EXPECT_FLOAT_EQ(p[0], 0.25F);
            EXPECT_FLOAT_EQ(p[1], 0.5F);
            EXPECT_FLOAT_EQ(p[2], 0.75F);
            EXPECT_FLOAT_EQ(p[3], 1.0F);
        }
    }
}

TEST_F(TextureBaseTest, ClearZeroesEverything)
{
    tex.fill(1.0F, 1.0F, 1.0F, 1.0F);
    tex.clear();

    for (float f : tex.get_pixel_buffer()) {
        EXPECT_FLOAT_EQ(f, 0.0F);
    }
}

TEST_F(TextureBaseTest, DirtyFlagFollowsWrites)
{
    tex.clear_gpu_update_flag();
    ASSERT_FALSE(tex.needs_gpu_update());

    tex.set_pixel(0, 0, 1.0F, 0.0F, 0.0F);
    EXPECT_TRUE(tex.needs_gpu_update());

    tex.clear_gpu_update_flag();
    tex.fill(0.5F, 0.5F, 0.5F);
    EXPECT_TRUE(tex.needs_gpu_update());

    tex.clear_gpu_update_flag();
    tex.clear();
    EXPECT_TRUE(tex.needs_gpu_update());
}

TEST_F(TextureBaseTest, RestoreStateReturnsSavedPixels)
{
    tex.fill(0.2F, 0.4F, 0.6F, 0.8F);
    tex.save_state();

    tex.clear();
    tex.restore_state();

    auto p = tex.get_pixel(3, 2);
    EXPECT_FLOAT_EQ(p[0], 0.2F);
    EXPECT_FLOAT_EQ(p[1], 0.4F);
    EXPECT_FLOAT_EQ(p[2], 0.6F);
    EXPECT_FLOAT_EQ(p[3], 0.8F);
}

TEST_F(TextureBaseTest, RestoreStateReturnsSavedDirtyFlag)
{
    tex.clear_gpu_update_flag();
    tex.save_state();

    tex.fill(1.0F, 1.0F, 1.0F);
    ASSERT_TRUE(tex.needs_gpu_update());

    tex.restore_state();
    EXPECT_FALSE(tex.needs_gpu_update());
}

TEST_F(TextureBaseTest, RestoreWithoutSaveLeavesPixelsUntouched)
{
    tex.fill(0.3F, 0.3F, 0.3F, 0.3F);
    tex.restore_state();

    EXPECT_FLOAT_EQ(tex.get_pixel(0, 0)[0], 0.3F);
}

TEST_F(TextureBaseTest, ContextReflectsDimensionsAndPixels)
{
    tex.fill(0.5F, 0.5F, 0.5F, 0.5F);
    tex.update_context(0.25);

    auto& ctx = dynamic_cast<Nodes::GpuSync::TextureContext&>(tex.get_last_context());
    EXPECT_DOUBLE_EQ(ctx.value, 0.25);
    EXPECT_EQ(ctx.width, 4U);
    EXPECT_EQ(ctx.height, 3U);
}

}
