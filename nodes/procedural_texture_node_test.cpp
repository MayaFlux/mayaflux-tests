#include "gtest/gtest.h"

#include "MayaFlux/Nodes/Graphics/ProceduralTextureNode.hpp"

namespace MayaFlux::Test {

namespace {

    using Nodes::GpuSync::ProceduralTextureNode;

    class ProceduralTextureNodeTest : public ::testing::Test {
    protected:
        static glm::vec4 pixel_at(const ProceduralTextureNode& node, uint32_t x, uint32_t y)
        {
            const auto buf = node.get_pixel_buffer();
            const size_t idx = (static_cast<size_t>(y) * node.get_width() + x) * 4;
            return { buf[idx], buf[idx + 1], buf[idx + 2], buf[idx + 3] };
        }

        static ProceduralTextureNode::PixelGenerator gradient()
        {
            return [](uint32_t x, uint32_t y, uint32_t, uint32_t) {
                return glm::vec4(
                    static_cast<float>(x),
                    static_cast<float>(y),
                    static_cast<float>(x + y),
                    1.0F);
            };
        }
    };

    TEST_F(ProceduralTextureNodeTest, ConstructionKeepsDimensions)
    {
        ProceduralTextureNode node(5, 3);

        EXPECT_EQ(node.get_width(), 5U);
        EXPECT_EQ(node.get_height(), 3U);
        EXPECT_EQ(node.get_pixel_buffer().size(), 5U * 3U * 4U);
    }

    TEST_F(ProceduralTextureNodeTest, ZeroDimensionsThrow)
    {
        EXPECT_THROW(ProceduralTextureNode(0, 4), std::invalid_argument);
        EXPECT_THROW(ProceduralTextureNode(4, 0, gradient()), std::invalid_argument);
    }

    TEST_F(ProceduralTextureNodeTest, DefaultGeneratorProducesOpaqueBlack)
    {
        ProceduralTextureNode node(3, 2);
        node.compute_frame();

        for (uint32_t y = 0; y < 2; ++y) {
            for (uint32_t x = 0; x < 3; ++x) {
                EXPECT_EQ(pixel_at(node, x, y), glm::vec4(0.0F, 0.0F, 0.0F, 1.0F));
            }
        }
    }

    TEST_F(ProceduralTextureNodeTest, NullGeneratorAtConstructionFallsBackToOpaqueBlack)
    {
        ProceduralTextureNode node(2, 2, ProceduralTextureNode::PixelGenerator {});
        node.compute_frame();

        EXPECT_EQ(pixel_at(node, 1, 1), glm::vec4(0.0F, 0.0F, 0.0F, 1.0F));
    }

    TEST_F(ProceduralTextureNodeTest, GeneratorIsCalledOncePerPixelWithCoordinatesAndSize)
    {
        std::vector<std::array<uint32_t, 4>> calls;
        ProceduralTextureNode node(4, 3,
            [&calls](uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
                calls.push_back({ x, y, w, h });
                return glm::vec4(0.0F);
            });

        node.compute_frame();

        ASSERT_EQ(calls.size(), 12U);
        std::set<std::pair<uint32_t, uint32_t>> seen;
        for (const auto& c : calls) {
            EXPECT_LT(c[0], 4U);
            EXPECT_LT(c[1], 3U);
            EXPECT_EQ(c[2], 4U);
            EXPECT_EQ(c[3], 3U);
            seen.insert({ c[0], c[1] });
        }
        EXPECT_EQ(seen.size(), 12U);
    }

    TEST_F(ProceduralTextureNodeTest, GeneratorColourLandsAtItsOwnPixel)
    {
        ProceduralTextureNode node(4, 3, gradient());
        node.compute_frame();

        for (uint32_t y = 0; y < 3; ++y) {
            for (uint32_t x = 0; x < 4; ++x) {
                EXPECT_EQ(pixel_at(node, x, y),
                    glm::vec4(static_cast<float>(x), static_cast<float>(y),
                        static_cast<float>(x + y), 1.0F))
                    << "pixel " << x << "," << y;
            }
        }
    }

    TEST_F(ProceduralTextureNodeTest, EveryChannelIsWrittenIncludingAlpha)
    {
        ProceduralTextureNode node(2, 1,
            [](uint32_t, uint32_t, uint32_t, uint32_t) {
                return glm::vec4(0.1F, 0.2F, 0.3F, 0.4F);
            });
        node.compute_frame();

        EXPECT_EQ(pixel_at(node, 0, 0), glm::vec4(0.1F, 0.2F, 0.3F, 0.4F));
        EXPECT_EQ(pixel_at(node, 1, 0), glm::vec4(0.1F, 0.2F, 0.3F, 0.4F));
    }

    TEST_F(ProceduralTextureNodeTest, ComputeFrameMarksTextureForGpuUpload)
    {
        ProceduralTextureNode node(2, 2, gradient());
        node.clear_gpu_update_flag();
        ASSERT_FALSE(node.needs_gpu_update());

        node.compute_frame();

        EXPECT_TRUE(node.needs_gpu_update());
    }

    TEST_F(ProceduralTextureNodeTest, EachFrameRegeneratesFromTheGenerator)
    {
        int frame = 0;
        ProceduralTextureNode node(2, 2,
            [&frame](uint32_t, uint32_t, uint32_t, uint32_t) {
                return glm::vec4(static_cast<float>(frame), 0.0F, 0.0F, 1.0F);
            });

        frame = 1;
        node.compute_frame();
        EXPECT_FLOAT_EQ(pixel_at(node, 1, 1).r, 1.0F);

        frame = 2;
        node.compute_frame();
        EXPECT_FLOAT_EQ(pixel_at(node, 1, 1).r, 2.0F);
    }

    TEST_F(ProceduralTextureNodeTest, SetGeneratorReplacesTheGeneratorForLaterFrames)
    {
        ProceduralTextureNode node(2, 2, gradient());
        node.compute_frame();

        node.set_generator([](uint32_t, uint32_t, uint32_t, uint32_t) {
            return glm::vec4(0.5F, 0.5F, 0.5F, 0.5F);
        });
        node.compute_frame();

        EXPECT_EQ(pixel_at(node, 1, 1), glm::vec4(0.5F, 0.5F, 0.5F, 0.5F));
    }

    TEST_F(ProceduralTextureNodeTest, SetGeneratorMarksDirtyWithoutRegenerating)
    {
        ProceduralTextureNode node(2, 2, gradient());
        node.compute_frame();
        node.clear_gpu_update_flag();

        node.set_generator([](uint32_t, uint32_t, uint32_t, uint32_t) {
            return glm::vec4(0.5F);
        });

        EXPECT_TRUE(node.needs_gpu_update());
        EXPECT_EQ(pixel_at(node, 1, 1), glm::vec4(1.0F, 1.0F, 2.0F, 1.0F));
    }

    TEST_F(ProceduralTextureNodeTest, SetNullGeneratorKeepsThePreviousOne)
    {
        ProceduralTextureNode node(2, 2, gradient());
        node.clear_gpu_update_flag();

        node.set_generator(ProceduralTextureNode::PixelGenerator {});
        node.compute_frame();

        EXPECT_EQ(pixel_at(node, 1, 1), glm::vec4(1.0F, 1.0F, 2.0F, 1.0F));
    }

    TEST_F(ProceduralTextureNodeTest, SetNullGeneratorDoesNotRequestUpload)
    {
        ProceduralTextureNode node(2, 2, gradient());
        node.clear_gpu_update_flag();

        node.set_generator(ProceduralTextureNode::PixelGenerator {});

        EXPECT_FALSE(node.needs_gpu_update());
    }

    TEST_F(ProceduralTextureNodeTest, ProcessSampleGeneratesAFrameAndReturnsZero)
    {
        ProceduralTextureNode node(2, 2, gradient());

        EXPECT_DOUBLE_EQ(node.process_sample(0.0), 0.0);
        EXPECT_EQ(pixel_at(node, 1, 1), glm::vec4(1.0F, 1.0F, 2.0F, 1.0F));
    }

    TEST_F(ProceduralTextureNodeTest, ProcessBatchGeneratesOneFramePerSample)
    {
        int calls = 0;
        ProceduralTextureNode node(2, 2,
            [&calls](uint32_t, uint32_t, uint32_t, uint32_t) {
                ++calls;
                return glm::vec4(0.0F);
            });

        auto out = node.process_batch(3);

        EXPECT_EQ(out.size(), 3U);
        EXPECT_EQ(calls, 3 * 4);
    }

    TEST_F(ProceduralTextureNodeTest, SaveAndRestoreReturnsGeneratedPixels)
    {
        ProceduralTextureNode node(2, 2, gradient());
        node.compute_frame();
        node.save_state();

        node.set_generator([](uint32_t, uint32_t, uint32_t, uint32_t) {
            return glm::vec4(0.0F);
        });
        node.compute_frame();
        ASSERT_EQ(pixel_at(node, 1, 1), glm::vec4(0.0F));

        node.restore_state();

        EXPECT_EQ(pixel_at(node, 1, 1), glm::vec4(1.0F, 1.0F, 2.0F, 1.0F));
    }

} // namespace

}
