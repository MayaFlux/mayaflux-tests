#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "../support/scoped_working_dir.hpp"
#include "../support/vulkan_test_support.hpp"

#include "MayaFlux/IO/Image/EXRWriter.hpp"
#include "MayaFlux/IO/Image/ImageExport.hpp"
#include "MayaFlux/IO/Image/ImageReader.hpp"
#include "MayaFlux/IO/IOManager.hpp"
#include "MayaFlux/Core/GlobalStreamInfo.hpp"

#include <tinyexr.h>

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))

namespace MayaFlux::Test {
namespace {

    namespace fs = std::filesystem;

    constexpr uint32_t image_width = 3;
    constexpr uint32_t image_height = 2;

    IO::ImageData float_image(uint32_t channels)
    {
        const auto format = channels == 1
            ? Kakshya::ImageFormat::R32F
            : channels == 2 ? Kakshya::ImageFormat::RG32F
                            : Kakshya::ImageFormat::RGBA32F;
        auto image = Kakshya::ImageData::allocate(
            image_width, image_height, channels, format);
        auto& pixels = *image->as_float();
        for (size_t i = 0; i < pixels.size(); ++i) {
            pixels[i] = static_cast<float>(static_cast<int>(i) - 5) * 0.375F;
        }
        return std::move(*image);
    }

    std::vector<float> decoded_pixels(const IO::ImageData& source)
    {
        const auto& pixels = *source.as_float();
        if (source.channels != 3) {
            return pixels;
        }

        std::vector<float> decoded;
        decoded.reserve(static_cast<size_t>(source.width) * source.height * 4);
        for (size_t pixel = 0; pixel < static_cast<size_t>(source.width) * source.height; ++pixel) {
            decoded.insert(decoded.end(), pixels.begin() + static_cast<std::ptrdiff_t>(pixel * 3),
                pixels.begin() + static_cast<std::ptrdiff_t>(pixel * 3 + 3));
            decoded.push_back(1.0F);
        }
        return decoded;
    }

    std::string absolute_path(const std::string& filename)
    {
        return (fs::current_path() / filename).string();
    }

    std::string read_bytes(const std::string& path)
    {
        std::ifstream input(path, std::ios::binary);
        return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
    }

    std::vector<unsigned char> make_uint_exr()
    {
        uint32_t sample = 17;
        unsigned char* planes[] { reinterpret_cast<unsigned char*>(&sample) };
        EXRImage image;
        InitEXRImage(&image);
        image.num_channels = 1;
        image.width = 1;
        image.height = 1;
        image.images = planes;

        EXRHeader header;
        InitEXRHeader(&header);
        header.num_channels = 1;
        header.compression_type = TINYEXR_COMPRESSIONTYPE_NONE;
        EXRChannelInfo channels[1] {};
        std::strcpy(channels[0].name, "Y");
        int pixel_types[] { TINYEXR_PIXELTYPE_UINT };
        int requested_pixel_types[] { TINYEXR_PIXELTYPE_UINT };
        header.channels = channels;
        header.pixel_types = pixel_types;
        header.requested_pixel_types = requested_pixel_types;

        unsigned char* encoded = nullptr;
        const char* error = nullptr;
        const auto size = SaveEXRImageToMemory(&image, &header, &encoded, &error);
        if (error) {
            FreeEXRErrorMessage(error);
        }
        if (size == 0 || !encoded) {
            std::free(encoded);
            return {};
        }

        std::vector<unsigned char> bytes(encoded, encoded + size);
        std::free(encoded);
        return bytes;
    }

    std::vector<unsigned char> make_half_exr()
    {
        const float samples[] { -2.0F, 0.5F, 3.25F };
        unsigned char* encoded = nullptr;
        const char* error = nullptr;
        const int size = SaveEXRToMemory(samples, 3, 1, 1, 1, &encoded, &error);
        if (error) {
            FreeEXRErrorMessage(error);
        }
        if (size <= 0 || !encoded) {
            std::free(encoded);
            return {};
        }

        std::vector<unsigned char> bytes(encoded, encoded + size);
        std::free(encoded);
        return bytes;
    }

    class EXRReaderChannelTest : public ::testing::TestWithParam<uint32_t> { };

    TEST_P(EXRReaderChannelTest, FileAndMemoryLoadsPreserveFloatPixels)
    {
        ScopedWorkingDir files;
        const auto source = float_image(GetParam());
        const auto path = absolute_path("channel_data.exr");
        IO::EXRWriter writer;
        ASSERT_TRUE(writer.write(path, source)) << writer.get_last_error();

        const auto from_file = IO::ImageReader::load(path, 1);
        ASSERT_TRUE(from_file.has_value());
        EXPECT_EQ(from_file->width, image_width);
        EXPECT_EQ(from_file->height, image_height);
        EXPECT_EQ(from_file->channels, GetParam() == 3 ? 4U : GetParam());
        EXPECT_EQ(from_file->format, GetParam() == 1 ? Kakshya::ImageFormat::R32F
                : GetParam() == 2 ? Kakshya::ImageFormat::RG32F
                                  : Kakshya::ImageFormat::RGBA32F);
        ASSERT_NE(from_file->as_float(), nullptr);
        EXPECT_EQ(*from_file->as_float(), decoded_pixels(source));

        const auto bytes = read_bytes(path);
        ASSERT_FALSE(bytes.empty());
        const auto from_memory = IO::ImageReader::load_from_memory(bytes.data(), bytes.size());
        ASSERT_TRUE(from_memory.has_value());
        ASSERT_NE(from_memory->as_float(), nullptr);
        EXPECT_EQ(*from_memory->as_float(), *from_file->as_float());
    }

    INSTANTIATE_TEST_SUITE_P(
        SupportedChannelCounts, EXRReaderChannelTest,
        ::testing::Values(1U, 2U, 3U, 4U));

    TEST(EXRReaderTest, CropsFloatPixelsAndRejectsOutOfBoundsRegions)
    {
        ScopedWorkingDir files;
        const auto source = float_image(4);
        const auto path = absolute_path("crop.exr");
        IO::EXRWriter writer;
        ASSERT_TRUE(writer.write(path, source)) << writer.get_last_error();

        const auto crop = IO::ImageReader::load(
            path, 4, Kakshya::Region::image_rect(1, 0, 2, 1));
        ASSERT_TRUE(crop.has_value());
        EXPECT_EQ(crop->width, 2U);
        EXPECT_EQ(crop->height, 2U);
        ASSERT_NE(crop->as_float(), nullptr);
        const auto& pixels = *source.as_float();
        EXPECT_EQ(*crop->as_float(), (std::vector<float> {
            pixels[4], pixels[5], pixels[6], pixels[7],
            pixels[8], pixels[9], pixels[10], pixels[11],
            pixels[16], pixels[17], pixels[18], pixels[19],
            pixels[20], pixels[21], pixels[22], pixels[23] }));

        EXPECT_FALSE(IO::ImageReader::load(
            path, 4, Kakshya::Region::image_rect(0, 0, image_width, 1)));
        EXPECT_FALSE(IO::ImageReader::load(
            path, 4, Kakshya::Region::image_rect(2, 1, 1, 1)));
    }

    TEST(EXRReaderTest, ReaderLifecycleReturnsFloatDataAndDimensions)
    {
        ScopedWorkingDir files;
        const auto source = float_image(2);
        const auto path = absolute_path("lifecycle.exr");
        IO::EXRWriter writer;
        ASSERT_TRUE(writer.write(path, source)) << writer.get_last_error();

        IO::ImageReader reader(4);
        EXPECT_TRUE(reader.can_read(path));
        ASSERT_TRUE(reader.open(path));
        EXPECT_TRUE(reader.is_open());
        EXPECT_EQ(reader.get_dimension_sizes(), (std::vector<uint64_t> { image_width, image_height }));
        const auto values = reader.read_all();
        ASSERT_EQ(values.size(), 1U);
        const auto* pixels = std::get_if<std::vector<float>>(&values.front());
        ASSERT_NE(pixels, nullptr);
        EXPECT_EQ(*pixels, *source.as_float());

        IO::FileRegion region;
        region.start_coordinates = { 1, 0 };
        region.end_coordinates = { 3, 2 };
        const auto region_values = reader.read_region(region);
        ASSERT_EQ(region_values.size(), 1U);
        const auto* region_bytes = std::get_if<std::vector<uint8_t>>(&region_values.front());
        ASSERT_NE(region_bytes, nullptr);
        const auto& source_pixels = *source.as_float();
        const std::array<float, 8> expected_region {
            source_pixels[2], source_pixels[3], source_pixels[4], source_pixels[5],
            source_pixels[8], source_pixels[9], source_pixels[10], source_pixels[11],
        };
        std::vector<uint8_t> expected_bytes(sizeof(expected_region));
        std::memcpy(expected_bytes.data(), expected_region.data(), expected_bytes.size());
        EXPECT_EQ(*region_bytes, expected_bytes);

        reader.close();
        EXPECT_FALSE(reader.is_open());
        EXPECT_TRUE(reader.get_dimension_sizes().empty());
        EXPECT_FALSE(reader.open(absolute_path("missing.exr")));
        EXPECT_FALSE(reader.get_last_error().empty());
    }

    TEST(EXRReaderTest, ConvertsHalfPixelsAndRejectsUnsignedAndUnsupportedChannelCounts)
    {
        const auto half = make_half_exr();
        ASSERT_FALSE(half.empty());
        const auto half_image = IO::ImageReader::load_from_memory(half.data(), half.size());
        ASSERT_TRUE(half_image.has_value());
        ASSERT_NE(half_image->as_float(), nullptr);
        EXPECT_EQ(*half_image->as_float(), (std::vector<float> { -2.0F, 0.5F, 3.25F }));

        const auto uint_image = make_uint_exr();
        ASSERT_FALSE(uint_image.empty());
        EXPECT_FALSE(IO::ImageReader::load_from_memory(uint_image.data(), uint_image.size()));

        ScopedWorkingDir files;
        const auto five_channels = float_image(5);
        const auto path = absolute_path("five_channels.exr");
        IO::EXRWriter writer;
        ASSERT_TRUE(writer.write(path, five_channels)) << writer.get_last_error();
        EXPECT_FALSE(IO::ImageReader::load(path));
    }

    TEST(EXRReaderTest, RejectsInvalidMemoryAndUnsupportedVersionFlags)
    {
        EXPECT_FALSE(IO::ImageReader::load_from_memory(nullptr, 0));
        const unsigned char invalid[] { 0, 1, 2, 3, 4, 5, 6, 7 };
        EXPECT_FALSE(IO::ImageReader::load_from_memory(invalid, sizeof(invalid)));

        ScopedWorkingDir files;
        const std::string invalid_image(
            reinterpret_cast<const char*>(invalid), sizeof(invalid));
        files.write("invalid.png", invalid_image);
        EXPECT_FALSE(IO::ImageReader::load(absolute_path("invalid.png")));

        const auto source = float_image(1);
        const auto path = absolute_path("flags.exr");
        IO::EXRWriter writer;
        ASSERT_TRUE(writer.write(path, source)) << writer.get_last_error();
        const auto original = read_bytes(path);
        ASSERT_GT(original.size(), 8U);

        const auto truncated = original.substr(0, 8);
        EXPECT_FALSE(IO::ImageReader::load_from_memory(truncated.data(), truncated.size()));

        for (const unsigned char flag : { 0x08, 0x10 }) {
            auto flagged = original;
            flagged[5] = static_cast<char>(static_cast<unsigned char>(flagged[5]) | flag);
            EXPECT_FALSE(IO::ImageReader::load_from_memory(flagged.data(), flagged.size()));
        }
    }

    class EXRReaderGpuTest : public ::testing::Test {
    protected:
        static void SetUpTestSuite()
        {
            if (!vulkan_unavailable_reason().empty()) {
                return;
            }
            s_backend = std::make_shared<Core::VulkanBackend>();
            if (!s_backend->initialize(headless_graphics_config())) {
                s_backend.reset();
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
            auto& loom = Portal::Graphics::TextureLoom::instance();
            loom.shutdown();
            ASSERT_TRUE(loom.initialize(s_backend));
        }

        void TearDown() override
        {
            Portal::Graphics::TextureLoom::instance().shutdown();
        }

    private:
        static inline std::shared_ptr<Core::VulkanBackend> s_backend;
    };

    MAYAFLUX_TEST_WITH(TEST_F, EXRReaderGpuTest, UploadsFloatPixelsToARealTextureAndReadsThemBack, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan, ::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        ScopedWorkingDir files;
        const auto source = float_image(4);
        const auto path = absolute_path("gpu_pixels.exr");
        IO::EXRWriter writer;
        ASSERT_TRUE(writer.write(path, source)) << writer.get_last_error();

        const auto texture = IO::ImageReader::load_texture(path);
        ASSERT_NE(texture, nullptr);
        const auto readback = Portal::Graphics::TextureLoom::instance().download_image(texture);
        ASSERT_TRUE(readback.has_value());
        EXPECT_EQ(readback->width, source.width);
        EXPECT_EQ(readback->height, source.height);
        ASSERT_NE(readback->as_float(), nullptr);
        EXPECT_EQ(*readback->as_float(), *source.as_float());
    }

    MAYAFLUX_TEST_WITH(TEST_F, EXRReaderGpuTest, SavesRealTextureSynchronouslyAndAsynchronously, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan, ::MayaFlux::Test::Meta::Capability::writable_temp>() }))
    {
        ScopedWorkingDir files;
        const auto source = float_image(4);
        const auto input = absolute_path("input.exr");
        IO::EXRWriter writer;
        ASSERT_TRUE(writer.write(input, source)) << writer.get_last_error();
        const auto texture = IO::ImageReader::load_texture(input);
        ASSERT_NE(texture, nullptr);

        Core::GlobalStreamInfo stream_info;
        IO::IOManager manager(stream_info, 60, nullptr, nullptr);
        const auto sync_path = absolute_path("sync.exr");
        const auto async_path = absolute_path("async.exr");
        ASSERT_TRUE(IO::save_image(texture, sync_path));
        ASSERT_TRUE(manager.save_image(texture, async_path));
        manager.wait_for_pending_saves();

        for (const auto& path : { sync_path, async_path }) {
            const auto loaded = IO::ImageReader::load(path);
            ASSERT_TRUE(loaded.has_value()) << path;
            ASSERT_NE(loaded->as_float(), nullptr);
            EXPECT_EQ(*loaded->as_float(), *source.as_float());
        }
    }

} // namespace
} // namespace MayaFlux::Test
