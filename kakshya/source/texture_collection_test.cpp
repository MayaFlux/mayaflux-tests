#include "gtest/gtest.h"

#include <vector>

#include "../../support/vulkan_test_support.hpp"

#include "MayaFlux/Core/Backends/Graphics/Vulkan/VKImage.hpp"
#include "MayaFlux/Kakshya/Source/TextureCollection.hpp"

namespace MayaFlux::Test {
namespace {

    using Kakshya::ImageFormat;
    using Kakshya::TextureCollection;
    using Kakshya::TextureCollectionSpec;
    using Portal::Graphics::SubmitMode;
    using Portal::Graphics::TextureLoom;

    TextureLoom& loom()
    {
        return TextureLoom::instance();
    }

    std::vector<uint8_t> ramp(size_t n, uint8_t seed = 0)
    {
        std::vector<uint8_t> out(n);
        for (size_t i = 0; i < n; ++i) {
            out[i] = static_cast<uint8_t>((i * 5 + seed) & 0xFF);
        }
        return out;
    }

    std::vector<uint8_t> uniform(size_t n, uint8_t value)
    {
        return std::vector<uint8_t>(n, value);
    }

    std::shared_ptr<Core::VKImage> make_image(uint32_t width, uint32_t height, const std::vector<uint8_t>& pixels)
    {
        return loom().create_2d(width, height, ImageFormat::RGBA8, pixels.data());
    }

    TEST(TextureCollectionConstructionTest, StartsEmptyAndKeepsItsSpec)
    {
        const TextureCollectionSpec spec { .width = 4, .height = 2, .max_bytes = 1024 };
        TextureCollection collection(spec);

        EXPECT_EQ(collection.get_layer_count(), 0U);
        EXPECT_EQ(collection.get_write_head(), 0U);
        EXPECT_FALSE(collection.is_full());
        EXPECT_EQ(collection.get_spec().width, 4U);
        EXPECT_EQ(collection.get_spec().height, 2U);
        EXPECT_EQ(collection.get_spec().max_bytes, 1024U);
    }

    TEST(TextureCollectionConstructionTest, ZeroByteCapNeverReportsFull)
    {
        TextureCollection collection(TextureCollectionSpec { .width = 64, .height = 64, .max_bytes = 0 });
        EXPECT_FALSE(collection.is_full());
    }

    class TextureCollectionGpuTest : public ::testing::Test {
    protected:
        static void SetUpTestSuite()
        {
            if (!vulkan_unavailable_reason().empty())
                return;
            s_backend = std::make_shared<Core::VulkanBackend>();
            if (!s_backend->initialize(headless_graphics_config()))
                s_backend.reset();
        }

        static void TearDownTestSuite()
        {
            s_backend.reset();
        }

        void SetUp() override
        {
            if (!s_backend)
                GTEST_SKIP() << "No usable Vulkan device: " << vulkan_unavailable_reason();
            loom().shutdown();
            ASSERT_TRUE(loom().initialize(s_backend));
        }

        void TearDown() override
        {
            loom().shutdown();
        }

        static std::vector<uint8_t> read_layer(const TextureCollection& collection, uint32_t layer, size_t byte_count)
        {
            std::vector<uint8_t> bytes(byte_count);
            loom().download_data(collection.to_image(layer), bytes.data(), bytes.size());
            return bytes;
        }

        static inline std::shared_ptr<Core::VulkanBackend> s_backend;
    };

    TEST_F(TextureCollectionGpuTest, AppendStoresEachImageAsTheNextLayer)
    {
        TextureCollection collection(TextureCollectionSpec { .width = 2, .height = 2 });
        const auto first = ramp(16, 3);
        const auto second = ramp(16, 91);

        ASSERT_TRUE(collection.append(make_image(2, 2, first)));
        EXPECT_EQ(collection.get_layer_count(), 1U);
        EXPECT_EQ(collection.get_write_head(), 1U);
        EXPECT_EQ(read_layer(collection, 0, first.size()), first);

        ASSERT_TRUE(collection.append(make_image(2, 2, second)));
        EXPECT_EQ(collection.get_layer_count(), 2U);
        EXPECT_EQ(collection.get_write_head(), 2U);
        EXPECT_EQ(read_layer(collection, 0, first.size()), first);
        EXPECT_EQ(read_layer(collection, 1, second.size()), second);
    }

    TEST_F(TextureCollectionGpuTest, AppendRejectsANullImage)
    {
        TextureCollection collection(TextureCollectionSpec { .width = 2, .height = 2 });

        EXPECT_FALSE(collection.append(nullptr));
        EXPECT_EQ(collection.get_layer_count(), 0U);
        EXPECT_EQ(collection.get_write_head(), 0U);
    }

    TEST_F(TextureCollectionGpuTest, SmallerImageIsCenteredOnAZeroedBackground)
    {
        TextureCollection collection(TextureCollectionSpec { .width = 4, .height = 4 });
        const auto source = ramp(2U * 4U * 4U, 7);

        ASSERT_TRUE(collection.append(make_image(2, 4, source)));

        std::vector<uint8_t> expected(4U * 4U * 4U, 0);
        for (uint32_t y = 0; y < 4U; ++y) {
            for (uint32_t x = 1; x <= 2; ++x) {
                for (uint32_t c = 0; c < 4; ++c) {
                    const size_t src_index = ((static_cast<size_t>(y) * 2U) + (x - 1U)) * 4U + c;
                    const size_t dst_index = ((static_cast<size_t>(y) * 4U) + x) * 4U + c;
                    expected[dst_index] = source[src_index];
                }
            }
        }
        EXPECT_EQ(read_layer(collection, 0, expected.size()), expected);
    }

    TEST_F(TextureCollectionGpuTest, LargerUniformImageFillsTheLayer)
    {
        TextureCollection collection(TextureCollectionSpec { .width = 4, .height = 4 });

        ASSERT_TRUE(collection.append(make_image(8, 8, uniform(8U * 8U * 4U, 200))));
        EXPECT_EQ(read_layer(collection, 0, 4U * 4U * 4U), uniform(4U * 4U * 4U, 200));
    }

    TEST_F(TextureCollectionGpuTest, LinearCollectionStopsAtTheByteCap)
    {
        TextureCollection collection(TextureCollectionSpec { .width = 2, .height = 2, .max_bytes = 32 });
        ASSERT_TRUE(collection.append(make_image(2, 2, ramp(16, 1))));
        EXPECT_FALSE(collection.is_full());
        ASSERT_TRUE(collection.append(make_image(2, 2, ramp(16, 2))));
        EXPECT_TRUE(collection.is_full());

        EXPECT_FALSE(collection.append(make_image(2, 2, ramp(16, 3))));
        EXPECT_EQ(collection.get_layer_count(), 2U);
        EXPECT_EQ(collection.get_write_head(), 2U);
    }

    TEST_F(TextureCollectionGpuTest, CircularCollectionOverwritesTheOldestAndIgnoresTheCap)
    {
        TextureCollection collection(TextureCollectionSpec {
            .width = 2, .height = 2, .max_bytes = 16, .ring_layers = 2 });
        const auto a = ramp(16, 10);
        const auto b = ramp(16, 20);
        const auto c = ramp(16, 30);

        ASSERT_TRUE(collection.append(make_image(2, 2, a)));
        ASSERT_TRUE(collection.append(make_image(2, 2, b)));
        EXPECT_FALSE(collection.is_full());
        ASSERT_TRUE(collection.append(make_image(2, 2, c)));

        EXPECT_EQ(collection.get_layer_count(), 2U);
        EXPECT_EQ(collection.get_write_head(), 3U);
        EXPECT_EQ(read_layer(collection, 0, c.size()), c);
        EXPECT_EQ(read_layer(collection, 1, b.size()), b);
    }

    TEST_F(TextureCollectionGpuTest, AsyncAppendIsReadableAfterwards)
    {
        TextureCollection collection(TextureCollectionSpec { .width = 2, .height = 2 });
        const auto pixels = ramp(16, 44);

        ASSERT_TRUE(collection.append(make_image(2, 2, pixels), SubmitMode::ASYNC));
        EXPECT_EQ(read_layer(collection, 0, pixels.size()), pixels);
    }

} // namespace
} // namespace MayaFlux::Test
