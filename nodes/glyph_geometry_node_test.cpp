#include "gtest/gtest.h"

#include <cstdint>
#include <string>
#include <vector>

#include "../support/text_test_support.hpp"

#include "MayaFlux/Nodes/Graphics/GlyphGeometryNode.hpp"
#include "MayaFlux/Portal/Text/GlyphOutline.hpp"
#include "MayaFlux/Portal/Text/TypeFaceFoundry.hpp"

namespace MayaFlux::Test {
namespace {

    using Kakshya::LineVertex;
    using Nodes::GpuSync::GlyphGeometryNode;
    using Portal::Text::decompose_glyph;
    using Portal::Text::GlyphOutline;
    using Portal::Text::TypeFaceFoundry;

    constexpr uint32_t k_pixel_size = 48;

    size_t segment_pairs(const std::vector<GlyphOutline>& outlines)
    {
        size_t pairs = 0;
        for (const auto& ol : outlines) {
            uint32_t start = 0;
            for (const uint32_t end : ol.contour_ends) {
                if (end - start >= 2) {
                    pairs += end - start - 1;
                }
                start = end;
            }
        }
        return pairs;
    }

    std::vector<LineVertex> uploaded(const GlyphGeometryNode& node)
    {
        std::vector<LineVertex> out;
        for (uint32_t i = 0; i < node.get_vertex_count(); ++i) {
            out.push_back(node.get_vertex_typed<LineVertex>(i));
        }
        return out;
    }

    class GlyphGeometryNodeTest : public SystemFontTest {
    protected:
        [[nodiscard]] GlyphOutline outline(uint32_t cp)
        {
            return decompose_glyph(face, cp, k_pixel_size);
        }
    };

    // ------------------------------------------------------------------
    // Construction
    // ------------------------------------------------------------------

    TEST_F(GlyphGeometryNodeTest, EmptyTextIsAValidEmptyLineNode)
    {
        const GlyphGeometryNode node(face, k_pixel_size, "");

        EXPECT_EQ(node.get_vertex_count(), 0U);
        EXPECT_TRUE(node.outlines().empty());
        EXPECT_TRUE(node.pen_offsets().empty());
        EXPECT_EQ(node.get_vertex_stride(), sizeof(LineVertex));
        EXPECT_EQ(node.get_primitive_topology(), Portal::Graphics::PrimitiveTopology::LINE_LIST);
        ASSERT_TRUE(node.get_vertex_layout().has_value());
        EXPECT_EQ(node.get_vertex_layout()->vertex_count, 0U);
    }

    TEST_F(GlyphGeometryNodeTest, ASingleGlyphBecomesOneLinePairPerOutlineSegment)
    {
        const GlyphGeometryNode node(face, k_pixel_size, "O");

        ASSERT_EQ(node.outlines().size(), 1U);
        const size_t pairs = segment_pairs(node.outlines());
        ASSERT_GT(pairs, 0U);
        EXPECT_EQ(node.get_vertex_count(), pairs * 2);
        ASSERT_TRUE(node.get_vertex_layout().has_value());
        EXPECT_EQ(node.get_vertex_layout()->vertex_count, node.get_vertex_count());
        EXPECT_TRUE(node.needs_gpu_update());
    }

    TEST_F(GlyphGeometryNodeTest, SegmentsJoinConsecutiveOutlinePointsAtThePenOffset)
    {
        const GlyphGeometryNode node(face, k_pixel_size, "I", 10.F, 20.F);
        const auto verts = uploaded(node);
        const GlyphOutline& ol = node.outlines().front();

        size_t v = 0;
        uint32_t start = 0;
        for (const uint32_t end : ol.contour_ends) {
            for (uint32_t i = start; i + 1 < end; ++i) {
                ASSERT_LT(v + 1, verts.size());
                EXPECT_EQ(glm::vec2(verts[v].position), ol.points[i] + glm::vec2(10.F, 20.F));
                EXPECT_EQ(glm::vec2(verts[v + 1].position), ol.points[i + 1] + glm::vec2(10.F, 20.F));
                EXPECT_FLOAT_EQ(verts[v].position.z, 0.F);
                v += 2;
            }
            start = end;
        }
        EXPECT_EQ(v, verts.size());
    }

    TEST_F(GlyphGeometryNodeTest, ContoursAreClosedSoEveryEdgeOfTheGlyphIsDrawn)
    {
        for (const char c : { 'I', 'O', 'B' }) {
            const GlyphGeometryNode node(face, k_pixel_size, std::string(1, c));
            const GlyphOutline& ol = node.outlines().front();
            uint32_t start = 0;
            for (const uint32_t end : ol.contour_ends) {
                EXPECT_EQ(ol.points[start], ol.points[end - 1]) << c;
                start = end;
            }
        }
    }

    TEST_F(GlyphGeometryNodeTest, DefaultColourAndThicknessAreWhiteAndOne)
    {
        const GlyphGeometryNode node(face, k_pixel_size, "I");
        const auto verts = uploaded(node);
        ASSERT_FALSE(verts.empty());
        for (const auto& v : verts) {
            EXPECT_EQ(v.color, glm::vec3(1.F));
            EXPECT_FLOAT_EQ(v.thickness, 1.F);
        }
    }

    TEST_F(GlyphGeometryNodeTest, SmallerToleranceNeverProducesFewerSegments)
    {
        const GlyphGeometryNode coarse(face, 128, "O", 0.F, 0.F, 4.F);
        const GlyphGeometryNode fine(face, 128, "O", 0.F, 0.F, 0.05F);
        EXPECT_LT(coarse.get_vertex_count(), fine.get_vertex_count());
    }

    // ------------------------------------------------------------------
    // Text layout
    // ------------------------------------------------------------------

    TEST_F(GlyphGeometryNodeTest, GlyphsAdvanceByTheirOutlineAdvance)
    {
        const GlyphGeometryNode node(face, k_pixel_size, "ab", 3.F, 7.F);

        ASSERT_EQ(node.outlines().size(), 2U);
        ASSERT_EQ(node.pen_offsets().size(), 2U);
        EXPECT_EQ(node.outlines()[0].codepoint, static_cast<uint32_t>('a'));
        EXPECT_EQ(node.outlines()[1].codepoint, static_cast<uint32_t>('b'));
        EXPECT_EQ(node.pen_offsets()[0], glm::vec2(3.F, 7.F));
        EXPECT_EQ(node.pen_offsets()[1], glm::vec2(3.F + static_cast<float>(outline('a').advance_x), 7.F));
    }

    TEST_F(GlyphGeometryNodeTest, WhitespaceKeepsItsAdvanceButAddsNoGeometry)
    {
        const GlyphGeometryNode node(face, k_pixel_size, " a");

        ASSERT_EQ(node.outlines().size(), 2U);
        EXPECT_TRUE(node.outlines()[0].points.empty());
        EXPECT_EQ(node.pen_offsets()[1].x, static_cast<float>(outline(' ').advance_x));
        EXPECT_EQ(node.get_vertex_count(), segment_pairs(node.outlines()) * 2);
    }

    TEST_F(GlyphGeometryNodeTest, NewlineReturnsToThePenColumnAndDropsByThePixelSize)
    {
        const GlyphGeometryNode node(face, k_pixel_size, "a\nb", 5.F, 9.F);

        ASSERT_EQ(node.outlines().size(), 2U);
        EXPECT_EQ(node.pen_offsets()[1], glm::vec2(5.F, 9.F + static_cast<float>(k_pixel_size)));
    }

    TEST_F(GlyphGeometryNodeTest, CarriageReturnAndInvalidBytesAreSkipped)
    {
        const GlyphGeometryNode node(face, k_pixel_size, std::string("a\r\xFF") + "b");

        ASSERT_EQ(node.outlines().size(), 2U);
        EXPECT_EQ(node.outlines()[0].codepoint, static_cast<uint32_t>('a'));
        EXPECT_EQ(node.outlines()[1].codepoint, static_cast<uint32_t>('b'));
        EXPECT_EQ(node.pen_offsets()[1].x, static_cast<float>(outline('a').advance_x));
    }

    // ------------------------------------------------------------------
    // Mutation
    // ------------------------------------------------------------------

    TEST_F(GlyphGeometryNodeTest, SetTextRebuildsTheOutlinesAndVertices)
    {
        GlyphGeometryNode node(face, k_pixel_size, "I");
        const uint32_t before = node.get_vertex_count();

        node.set_text("O");

        ASSERT_EQ(node.outlines().size(), 1U);
        EXPECT_EQ(node.outlines()[0].codepoint, static_cast<uint32_t>('O'));
        EXPECT_NE(node.get_vertex_count(), before);
        EXPECT_EQ(node.get_vertex_count(), segment_pairs(node.outlines()) * 2);
    }

    TEST_F(GlyphGeometryNodeTest, SetTextToEmptyClearsEverything)
    {
        GlyphGeometryNode node(face, k_pixel_size, "abc");
        node.set_text("");

        EXPECT_TRUE(node.outlines().empty());
        EXPECT_TRUE(node.pen_offsets().empty());
        EXPECT_EQ(node.get_vertex_count(), 0U);
    }

    TEST_F(GlyphGeometryNodeTest, SetColourAndThicknessApplyOnTheNextFrame)
    {
        GlyphGeometryNode node(face, k_pixel_size, "I");
        node.set_color({ 0.25F, 0.5F, 0.75F });
        node.set_thickness(3.5F);
        node.compute_frame();

        const auto verts = uploaded(node);
        ASSERT_FALSE(verts.empty());
        for (const auto& v : verts) {
            EXPECT_EQ(v.color, glm::vec3(0.25F, 0.5F, 0.75F));
            EXPECT_FLOAT_EQ(v.thickness, 3.5F);
        }
    }

    TEST_F(GlyphGeometryNodeTest, SetPenOffsetsMovesTheGlyphsAndIgnoresAWrongCount)
    {
        GlyphGeometryNode node(face, k_pixel_size, "I");
        const auto before = uploaded(node);
        ASSERT_FALSE(before.empty());

        node.set_pen_offsets({ { 1.F, 2.F }, { 3.F, 4.F } });
        node.compute_frame();
        EXPECT_EQ(uploaded(node).front().position, before.front().position);

        node.set_pen_offsets({ { 100.F, -50.F } });
        node.compute_frame();
        const auto after = uploaded(node);
        ASSERT_EQ(after.size(), before.size());
        EXPECT_EQ(glm::vec2(after.front().position), glm::vec2(before.front().position) + glm::vec2(100.F, -50.F));
    }

    TEST_F(GlyphGeometryNodeTest, SetPenMovesTheGeometryOnTheNextFrame)
    {
        GlyphGeometryNode node(face, k_pixel_size, "I");
        const auto before = uploaded(node);
        ASSERT_FALSE(before.empty());

        node.set_pen(40.F, 25.F);
        node.compute_frame();

        const auto after = uploaded(node);
        ASSERT_EQ(after.size(), before.size());
        EXPECT_EQ(glm::vec2(after.front().position), glm::vec2(before.front().position) + glm::vec2(40.F, 25.F));
    }

    TEST_F(GlyphGeometryNodeTest, ComputeFrameOnACleanNodeLeavesNothingToUpload)
    {
        GlyphGeometryNode node(face, k_pixel_size, "I");
        node.clear_gpu_update_flag();
        ASSERT_FALSE(node.needs_gpu_update());

        node.compute_frame();

        EXPECT_FALSE(node.needs_gpu_update());
    }

    TEST_F(GlyphGeometryNodeTest, ChangingAPropertyMarksTheNodeForUpload)
    {
        GlyphGeometryNode node(face, k_pixel_size, "I");
        node.clear_gpu_update_flag();

        node.set_thickness(2.F);
        node.compute_frame();

        EXPECT_TRUE(node.needs_gpu_update());
    }

    // ------------------------------------------------------------------
    // Default font
    // ------------------------------------------------------------------

    TEST_F(GlyphGeometryNodeTest, DefaultFontNodeMatchesTheExplicitFaceAtTheAtlasSize)
    {
        auto& foundry = TypeFaceFoundry::instance();
        if (foundry.get_default_glyph_atlas()) {
            GTEST_SKIP() << "default font already set earlier in this process";
        }
        ASSERT_TRUE(foundry.set_default_font(font_path, k_pixel_size, 512));

        const GlyphGeometryNode via_default("a\nb");
        const GlyphGeometryNode via_face(*foundry.get_default_face(), k_pixel_size, "a\nb");

        EXPECT_EQ(via_default.get_vertex_count(), via_face.get_vertex_count());
        EXPECT_EQ(via_default.pen_offsets(), via_face.pen_offsets());
    }

    TEST_F(GlyphGeometryNodeTest, DefaultFontNodeWithoutADefaultHasOutlinesButNoGeometry)
    {
        if (TypeFaceFoundry::instance().get_default_glyph_atlas()) {
            GTEST_SKIP() << "default font already set earlier in this process";
        }

        const GlyphGeometryNode node("ab");

        EXPECT_EQ(node.outlines().size(), 2U);
        EXPECT_EQ(node.get_vertex_count(), 0U);
    }

} // namespace
} // namespace MayaFlux::Test
