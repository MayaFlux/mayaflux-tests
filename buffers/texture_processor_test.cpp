#include "gtest/gtest.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

#include "../support/vulkan_test_support.hpp"

#include "MayaFlux/Buffers/Textures/TextureBuffer.hpp"
#include "MayaFlux/Buffers/Textures/TextureProcessor.hpp"
#include "MayaFlux/Core/Backends/Graphics/Vulkan/VKImage.hpp"
#include "MayaFlux/Portal/Graphics/TextureLoom.hpp"
#include "MayaFlux/Registry/BackendRegistry.hpp"
#include "MayaFlux/Registry/Service/BufferService.hpp"

namespace MayaFlux::Test {
namespace {

    using Buffers::TextureBuffer;
    using Portal::Graphics::ImageFormat;

    class ProbeTextureProcessor : public Buffers::TextureProcessor {
    public:
        using TextureProcessor::on_attach;
        using TextureProcessor::on_detach;
        using TextureProcessor::processing_function;
    };

    std::vector<uint8_t> pattern(size_t bytes, uint8_t seed)
    {
        std::vector<uint8_t> out(bytes);
        for (size_t i = 0; i < bytes; ++i) {
            out[i] = static_cast<uint8_t>((i * 37 + seed) & 0xFF);
        }
        return out;
    }

    class TextureProcessorDeviceTest : public ::testing::Test {
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
            loom().shutdown();
            ASSERT_TRUE(loom().initialize(s_backend));
            processor = std::make_shared<ProbeTextureProcessor>();
        }

        void TearDown() override
        {
            if (s_backend && buffer && buffer->is_initialized()) {
                s_backend->get_resource_manager().cleanup_buffer(buffer);
            }
            processor.reset();
            buffer.reset();
            if (s_backend) {
                loom().shutdown();
            }
        }

        static Portal::Graphics::TextureLoom& loom() { return Portal::Graphics::get_texture_manager(); }

        void make_buffer(uint32_t w, uint32_t h, const std::vector<uint8_t>& pixels = {})
        {
            buffer = std::make_shared<TextureBuffer>(w, h, ImageFormat::RGBA8,
                pixels.empty() ? nullptr : pixels.data());
        }

        void attach() { processor->on_attach(buffer); }
        void process() { processor->processing_function(buffer); }

        [[nodiscard]] std::vector<uint8_t> read_back() const
        {
            const auto data = loom().download_image(buffer->get_texture());
            std::vector<uint8_t> out;
            if (data) {
                out.resize(data->byte_size());
                std::memcpy(out.data(), data->data(), out.size());
            }
            return out;
        }

        std::shared_ptr<TextureBuffer> buffer;
        std::shared_ptr<ProbeTextureProcessor> processor;

    private:
        static inline std::shared_ptr<Core::VulkanBackend> s_backend;
    };

    // ------------------------------------------------------------------
    // Attach
    // ------------------------------------------------------------------

    TEST_F(TextureProcessorDeviceTest, AttachInitializesTheBufferAndUploadsInitialPixels)
    {
        const auto pixels = pattern(size_t { 4 } * 4 * 4, 11);
        make_buffer(4, 4, pixels);

        attach();

        EXPECT_TRUE(buffer->is_initialized());
        ASSERT_TRUE(buffer->has_texture());
        EXPECT_EQ(buffer->get_texture()->get_width(), 4U);
        EXPECT_EQ(buffer->get_texture()->get_height(), 4U);
        EXPECT_EQ(read_back(), pixels);
    }

    TEST_F(TextureProcessorDeviceTest, AttachWithoutPixelsStillCreatesTheTexture)
    {
        make_buffer(8, 2);

        attach();

        EXPECT_TRUE(buffer->is_initialized());
        ASSERT_TRUE(buffer->has_texture());
        EXPECT_EQ(buffer->get_texture()->get_width(), 8U);
        EXPECT_EQ(buffer->get_texture()->get_height(), 2U);
        EXPECT_FALSE(buffer->is_texture_dirty());
    }

    TEST_F(TextureProcessorDeviceTest, AttachKeepsATextureThatWasAlreadySupplied)
    {
        make_buffer(4, 4);
        auto image = loom().create_2d(4, 4, ImageFormat::RGBA8, nullptr, 1);
        ASSERT_NE(image, nullptr);
        buffer->set_gpu_texture(image);

        attach();

        EXPECT_EQ(buffer->get_texture(), image);
    }

    TEST_F(TextureProcessorDeviceTest, AttachToAPlainBufferIsIgnored)
    {
        auto plain = std::make_shared<Buffers::VKBuffer>(
            256, Buffers::VKBuffer::Usage::VERTEX, Kakshya::DataModality::IMAGE_COLOR);

        EXPECT_NO_THROW(processor->on_attach(plain));
        EXPECT_NO_THROW(processor->processing_function(plain));
    }

    // ------------------------------------------------------------------
    // Per-frame updates
    // ------------------------------------------------------------------

    TEST_F(TextureProcessorDeviceTest, ProcessingUploadsPixelsChangedAfterAttach)
    {
        make_buffer(4, 4, pattern(64, 1));
        attach();

        const auto updated = pattern(64, 200);
        buffer->set_pixel_data(updated.data(), updated.size());
        ASSERT_TRUE(buffer->is_texture_dirty());

        process();

        EXPECT_FALSE(buffer->is_texture_dirty());
        EXPECT_EQ(read_back(), updated);
    }

    TEST_F(TextureProcessorDeviceTest, ProcessingACleanBufferLeavesTheTextureUntouched)
    {
        const auto pixels = pattern(64, 5);
        make_buffer(4, 4, pixels);
        attach();

        process();
        process();

        EXPECT_FALSE(buffer->is_texture_dirty());
        EXPECT_EQ(read_back(), pixels);
    }

    TEST_F(TextureProcessorDeviceTest, ProcessingAfterAResizeReallocatesTheTexture)
    {
        make_buffer(4, 4, pattern(64, 3));
        attach();
        const auto before = buffer->get_texture();

        buffer->resize_texture(8, 2);
        const auto resized = pattern(size_t { 8 } * 2 * 4, 90);
        buffer->set_pixel_data(resized.data(), resized.size());
        process();

        ASSERT_TRUE(buffer->has_texture());
        EXPECT_NE(buffer->get_texture(), before);
        EXPECT_EQ(buffer->get_texture()->get_width(), 8U);
        EXPECT_EQ(buffer->get_texture()->get_height(), 2U);
        EXPECT_EQ(read_back(), resized);
    }

    TEST_F(TextureProcessorDeviceTest, GeometryChangesDoNotDisturbTheUploadedPixels)
    {
        const auto pixels = pattern(64, 17);
        make_buffer(4, 4, pixels);
        attach();

        buffer->set_position(0.5F, -0.5F);
        buffer->set_scale(0.25F, 0.75F);
        buffer->set_rotation(0.5F);
        process();

        EXPECT_EQ(read_back(), pixels);
    }

    // ------------------------------------------------------------------
    // Streaming mode
    // ------------------------------------------------------------------

    TEST_F(TextureProcessorDeviceTest, StreamingModeIsOffByDefaultAndToggles)
    {
        EXPECT_FALSE(processor->is_streaming_mode());
        processor->set_streaming_mode(true);
        EXPECT_TRUE(processor->is_streaming_mode());
    }

    TEST_F(TextureProcessorDeviceTest, StreamingUploadsMatchDirectUploadsAcrossRepeatedFrames)
    {
        make_buffer(4, 4, pattern(64, 1));
        processor->set_streaming_mode(true);
        attach();

        for (const uint8_t seed : { uint8_t { 40 }, uint8_t { 80 }, uint8_t { 120 } }) {
            const auto frame = pattern(64, seed);
            buffer->set_pixel_data(frame.data(), frame.size());
            process();
            EXPECT_FALSE(buffer->is_texture_dirty()) << static_cast<int>(seed);
            EXPECT_EQ(read_back(), frame) << static_cast<int>(seed);
        }
    }

    TEST_F(TextureProcessorDeviceTest, StreamingSurvivesAResizeOnceTheStagingIsInvalidated)
    {
        make_buffer(4, 4, pattern(64, 1));
        processor->set_streaming_mode(true);
        attach();

        const auto first = pattern(64, 60);
        buffer->set_pixel_data(first.data(), first.size());
        process();

        buffer->resize_texture(8, 8);
        processor->invalidate_staging();
        const auto resized = pattern(size_t { 8 } * 8 * 4, 150);
        buffer->set_pixel_data(resized.data(), resized.size());
        process();

        EXPECT_EQ(buffer->get_texture()->get_width(), 8U);
        EXPECT_EQ(read_back(), resized);
    }

    // ------------------------------------------------------------------
    // Detach
    // ------------------------------------------------------------------

    TEST_F(TextureProcessorDeviceTest, ProcessingAfterDetachDoesNothing)
    {
        const auto pixels = pattern(64, 9);
        make_buffer(4, 4, pixels);
        attach();

        processor->on_detach(buffer);

        const auto updated = pattern(64, 99);
        buffer->set_pixel_data(updated.data(), updated.size());
        process();

        EXPECT_TRUE(buffer->is_texture_dirty());
        EXPECT_EQ(read_back(), pixels);
    }

    // ------------------------------------------------------------------
    // No backend
    // ------------------------------------------------------------------

    TEST(TextureProcessorNoBackendTest, AttachWithoutABufferServiceThrows)
    {
        if (Registry::BackendRegistry::instance().get_service<Registry::Service::BufferService>()) {
            GTEST_SKIP() << "a graphics backend is registered in this process";
        }

        auto buffer = std::make_shared<TextureBuffer>(4, 4, ImageFormat::RGBA8);
        EXPECT_THROW(
            {
                ProbeTextureProcessor processor;
                processor.on_attach(buffer);
            },
            std::runtime_error);
    }

} // namespace
} // namespace MayaFlux::Test
