#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "../support/scoped_working_dir.hpp"

#include "MayaFlux/Core/GlobalStreamInfo.hpp"
#include "MayaFlux/IO/IOManager.hpp"
#include "MayaFlux/IO/Image/ImageReader.hpp"
#include "MayaFlux/IO/Image/STBImageWriter.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))

namespace MayaFlux::Test {
namespace {

    std::string absolute_path(const std::string& filename)
    {
        return (std::filesystem::current_path() / filename).string();
    }

    std::string read_bytes(const std::string& path)
    {
        std::ifstream input(path, std::ios::binary);
        return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
    }

    IO::ImageData byte_image(uint32_t channels, uint32_t width = 3, uint32_t height = 2)
    {
        const auto format = channels == 1 ? Kakshya::ImageFormat::R8
            : channels == 2 ? Kakshya::ImageFormat::RG8
                            : Kakshya::ImageFormat::RGBA8;
        auto image = Kakshya::ImageData::allocate(width, height, channels, format);
        auto& pixels = *image->as_uint8();
        for (size_t i = 0; i < pixels.size(); ++i) {
            pixels[i] = static_cast<uint8_t>((i * 37 + 19) % 256);
        }
        return std::move(*image);
    }

    std::vector<uint8_t> expected_pixels(const IO::ImageData& source)
    {
        const auto& pixels = *source.as_uint8();
        if (source.channels != 3) {
            return pixels;
        }
        std::vector<uint8_t> result;
        for (size_t i = 0; i < pixels.size(); i += 3) {
            result.insert(result.end(), pixels.begin() + static_cast<std::ptrdiff_t>(i),
                pixels.begin() + static_cast<std::ptrdiff_t>(i + 3));
            result.push_back(255);
        }
        return result;
    }

    class STBImageWriterChannelTest : public ::testing::TestWithParam<uint32_t> { };

    TEST_P(STBImageWriterChannelTest, PngRoundTripsFileAndMemory)
    {
        ScopedWorkingDir files;
        const auto source = byte_image(GetParam());
        const auto path = absolute_path("channels.png");
        IO::STBImageWriter writer;
        ASSERT_TRUE(writer.write(path, source)) << writer.get_last_error();

        const auto file = IO::ImageReader::load(path, 0);
        const auto bytes = read_bytes(path);
        ASSERT_FALSE(bytes.empty());
        const auto memory = IO::ImageReader::load_from_memory(bytes.data(), bytes.size());
        ASSERT_TRUE(file.has_value());
        ASSERT_TRUE(memory.has_value());
        EXPECT_EQ(file->width, source.width);
        EXPECT_EQ(file->height, source.height);
        EXPECT_EQ(file->channels, source.channels == 3 ? 4U : source.channels);
        EXPECT_EQ(file->format, source.channels == 1 ? Kakshya::ImageFormat::R8
                : source.channels == 2 ? Kakshya::ImageFormat::RG8
                                       : Kakshya::ImageFormat::RGBA8);
        ASSERT_NE(file->as_uint8(), nullptr);
        ASSERT_NE(memory->as_uint8(), nullptr);
        EXPECT_EQ(*file->as_uint8(), expected_pixels(source));
        EXPECT_EQ(*memory->as_uint8(), *file->as_uint8());
    }

    INSTANTIATE_TEST_SUITE_P(
        ChannelCounts, STBImageWriterChannelTest, ::testing::Values(1U, 2U, 3U, 4U));

    TEST(STBImageWriterTest, PngCompressionAndVerticalFlipRoundTrip)
    {
        ScopedWorkingDir files;
        const auto source = byte_image(4);
        IO::STBImageWriter writer;
        for (const int compression : { 0, 9, -1, 42 }) {
            IO::ImageWriteOptions options;
            options.compression = compression;
            const auto path = absolute_path("compression_" + std::to_string(compression) + ".png");
            ASSERT_TRUE(writer.write(path, source, options)) << writer.get_last_error();
            const auto loaded = IO::ImageReader::load(path);
            ASSERT_TRUE(loaded.has_value());
            ASSERT_NE(loaded->as_uint8(), nullptr);
            EXPECT_EQ(*loaded->as_uint8(), *source.as_uint8());
        }

        IO::ImageWriteOptions options;
        options.flip_vertically = true;
        const auto flipped_path = absolute_path("flipped.png");
        ASSERT_TRUE(writer.write(flipped_path, source, options)) << writer.get_last_error();
        const auto flipped = IO::ImageReader::load(flipped_path);
        ASSERT_TRUE(flipped.has_value());
        ASSERT_NE(flipped->as_uint8(), nullptr);
        const auto& pixels = *source.as_uint8();
        const size_t row_size = source.width * source.channels;
        std::vector<uint8_t> expected(pixels.begin() + static_cast<std::ptrdiff_t>(row_size), pixels.end());
        expected.insert(expected.end(), pixels.begin(), pixels.begin() + static_cast<std::ptrdiff_t>(row_size));
        EXPECT_EQ(*flipped->as_uint8(), expected);

        const auto normal_path = absolute_path("normal.png");
        ASSERT_TRUE(writer.write(normal_path, source)) << writer.get_last_error();
        const auto normal = IO::ImageReader::load(normal_path);
        ASSERT_TRUE(normal.has_value());
        ASSERT_NE(normal->as_uint8(), nullptr);
        EXPECT_EQ(*normal->as_uint8(), pixels);
    }

    TEST(STBImageWriterTest, PngCompressionLevelsAffectEncodedSize)
    {
        ScopedWorkingDir files;
        auto source = byte_image(4, 64, 64);
        std::ranges::fill(*source.as_uint8(), 42);
        IO::STBImageWriter writer;
        IO::ImageWriteOptions options;
        options.compression = 0;
        const auto uncompressed = absolute_path("uncompressed.png");
        ASSERT_TRUE(writer.write(uncompressed, source, options)) << writer.get_last_error();
        options.compression = 9;
        const auto compressed = absolute_path("compressed.png");
        ASSERT_TRUE(writer.write(compressed, source, options)) << writer.get_last_error();
        EXPECT_GT(std::filesystem::file_size(uncompressed), std::filesystem::file_size(compressed));
        for (const auto& path : { uncompressed, compressed }) {
            const auto loaded = IO::ImageReader::load(path);
            ASSERT_TRUE(loaded.has_value());
            ASSERT_NE(loaded->as_uint8(), nullptr);
            EXPECT_EQ(*loaded->as_uint8(), *source.as_uint8());
        }
    }

    TEST(STBImageWriterTest, BmpAndTgaRoundTripRgbAndRgba)
    {
        ScopedWorkingDir files;
        IO::STBImageWriter writer;
        const auto reset = byte_image(4);
        ASSERT_TRUE(writer.write(absolute_path("reset.png"), reset)) << writer.get_last_error();
        for (const auto* extension : { "bmp", "tga" }) {
            for (const uint32_t channels : { 3U, 4U }) {
                SCOPED_TRACE(std::string(extension) + " channels=" + std::to_string(channels));
                const auto source = byte_image(channels);
                const auto path = absolute_path("image_" + std::to_string(channels) + "." + extension);
                ASSERT_TRUE(writer.write(path, source)) << writer.get_last_error();
                const auto loaded = IO::ImageReader::load(path, 0);
                ASSERT_TRUE(loaded.has_value());
                ASSERT_NE(loaded->as_uint8(), nullptr);
                EXPECT_EQ(loaded->width, source.width);
                EXPECT_EQ(loaded->height, source.height);
                EXPECT_EQ(loaded->channels, 4U);
                EXPECT_EQ(*loaded->as_uint8(), expected_pixels(source));
            }
        }
    }

    TEST(STBImageWriterTest, JpegRoundTripsWithLossAndClampsQuality)
    {
        ScopedWorkingDir files;
        IO::STBImageWriter writer;
        for (const uint32_t channels : { 1U, 3U }) {
            auto source = byte_image(channels, 16, 16);
            auto& pixels = *source.as_uint8();
            for (size_t i = 0; i < pixels.size(); ++i) {
                pixels[i] = channels == 1 ? 128 : std::array<uint8_t, 3> { 160, 90, 35 }[i % 3];
            }
            for (const auto* extension : { "jpg", "jpeg" }) {
                const auto path = absolute_path("image_" + std::to_string(channels) + "." + extension);
                IO::ImageWriteOptions options;
                options.quality = 100;
                ASSERT_TRUE(writer.write(path, source, options)) << writer.get_last_error();
                const auto loaded = IO::ImageReader::load(path, 0);
                ASSERT_TRUE(loaded.has_value());
                ASSERT_NE(loaded->as_uint8(), nullptr);
                EXPECT_EQ(loaded->width, source.width);
                EXPECT_EQ(loaded->height, source.height);
                EXPECT_EQ(loaded->channels, 4U);
                auto expected = expected_pixels(source);
                if (channels == 1) {
                    expected.clear();
                    for (const auto value : pixels) {
                        expected.insert(expected.end(), { value, value, value, 255 });
                    }
                }
                ASSERT_EQ(loaded->as_uint8()->size(), expected.size());
                for (size_t i = 0; i < expected.size(); ++i) {
                    EXPECT_NEAR((*loaded->as_uint8())[i], expected[i], 4);
                }
            }
        }

        const auto source = byte_image(1, 16, 16);
        for (const auto& [requested, clamped] : { std::pair { 0, 1 }, std::pair { 101, 100 } }) {
            IO::ImageWriteOptions options;
            options.quality = requested;
            const auto requested_path = absolute_path("quality_requested.jpg");
            ASSERT_TRUE(writer.write(requested_path, source, options)) << writer.get_last_error();
            options.quality = clamped;
            const auto clamped_path = absolute_path("quality_clamped.jpg");
            ASSERT_TRUE(writer.write(clamped_path, source, options)) << writer.get_last_error();
            EXPECT_EQ(read_bytes(requested_path), read_bytes(clamped_path));
        }
    }

    TEST(STBImageWriterTest, JpegVerticalFlipReversesRows)
    {
        ScopedWorkingDir files;
        auto source = byte_image(1, 16, 16);
        auto& pixels = *source.as_uint8();
        std::fill(pixels.begin(), pixels.begin() + 128, 32);
        std::fill(pixels.begin() + 128, pixels.end(), 200);
        IO::ImageWriteOptions options;
        options.quality = 100;
        options.flip_vertically = true;
        IO::STBImageWriter writer;
        const auto path = absolute_path("flipped.jpg");
        ASSERT_TRUE(writer.write(path, source, options)) << writer.get_last_error();
        const auto loaded = IO::ImageReader::load(path, 1);
        ASSERT_TRUE(loaded.has_value());
        ASSERT_NE(loaded->as_uint8(), nullptr);
        EXPECT_EQ(loaded->channels, 1U);
        ASSERT_EQ(loaded->as_uint8()->size(), pixels.size());
        EXPECT_NEAR(loaded->as_uint8()->front(), 200, 3);
        EXPECT_NEAR(loaded->as_uint8()->back(), 32, 3);
        ASSERT_TRUE(writer.write(absolute_path("reset.jpg"), source)) << writer.get_last_error();
    }

    TEST(STBImageWriterTest, HdrFloatPixelsDecodeThroughTheEightBitReader)
    {
        ScopedWorkingDir files;
        IO::STBImageWriter writer;
        for (const uint32_t channels : { 3U, 4U }) {
            auto source = Kakshya::ImageData::allocate(1, 1, channels, Kakshya::ImageFormat::RGBA32F);
            ASSERT_TRUE(source.has_value());
            auto& pixels = *source->as_float();
            pixels[0] = 1.0F;
            pixels[1] = 0.25F;
            pixels[2] = 0.0F;
            if (channels == 4) {
                pixels[3] = 0.5F;
            }
            const auto path = absolute_path("image_" + std::to_string(channels) + ".hdr");
            ASSERT_TRUE(writer.write(path, *source)) << writer.get_last_error();
            const auto loaded = IO::ImageReader::load(path, 0);
            ASSERT_TRUE(loaded.has_value());
            ASSERT_NE(loaded->as_uint8(), nullptr);
            ASSERT_EQ(loaded->as_uint8()->size(), 4U);
            EXPECT_EQ((*loaded->as_uint8())[0], 255);
            EXPECT_NEAR((*loaded->as_uint8())[1], 136, 2);
            EXPECT_EQ((*loaded->as_uint8())[2], 0);
            EXPECT_EQ((*loaded->as_uint8())[3], 255);
        }
    }

    TEST(STBImageWriterTest, RejectsInvalidInputsAndRecoversAfterFilesystemFailure)
    {
        ScopedWorkingDir files;
        IO::STBImageWriter writer;
        const auto bytes = byte_image(4);
        auto floats = Kakshya::ImageData::allocate(1, 1, 4, Kakshya::ImageFormat::RGBA32F);
        auto uint16 = Kakshya::ImageData::allocate(1, 1, 4, Kakshya::ImageFormat::RGBA16);
        ASSERT_TRUE(floats.has_value());
        ASSERT_TRUE(uint16.has_value());
        for (const auto* extension : { "png", "jpg", "bmp", "tga" }) {
            EXPECT_FALSE(writer.write(absolute_path(std::string("invalid.") + extension), *floats));
            EXPECT_NE(writer.get_last_error().find("uint8"), std::string::npos);
            EXPECT_FALSE(writer.write(absolute_path(std::string("invalid.") + extension), *uint16));
        }
        EXPECT_FALSE(writer.write(absolute_path("invalid.hdr"), bytes));
        EXPECT_NE(writer.get_last_error().find("float"), std::string::npos);
        EXPECT_FALSE(writer.write(absolute_path("invalid.jpg"), bytes));
        EXPECT_NE(writer.get_last_error().find("channels"), std::string::npos);
        EXPECT_FALSE(writer.write(absolute_path("invalid.jpg"), byte_image(2)));
        EXPECT_NE(writer.get_last_error().find("channels"), std::string::npos);
        auto gray_float = Kakshya::ImageData::allocate(1, 1, 1, Kakshya::ImageFormat::R32F);
        ASSERT_TRUE(gray_float.has_value());
        EXPECT_FALSE(writer.write(absolute_path("invalid.hdr"), *gray_float));
        EXPECT_NE(writer.get_last_error().find("channels"), std::string::npos);
        auto gray_alpha_float = Kakshya::ImageData::allocate(1, 1, 2, Kakshya::ImageFormat::RG32F);
        ASSERT_TRUE(gray_alpha_float.has_value());
        EXPECT_FALSE(writer.write(absolute_path("invalid.hdr"), *gray_alpha_float));
        EXPECT_NE(writer.get_last_error().find("channels"), std::string::npos);

        auto inconsistent = bytes;
        inconsistent.format = Kakshya::ImageFormat::RGBA32F;
        EXPECT_FALSE(writer.write(absolute_path("invalid.png"), inconsistent));
        EXPECT_NE(writer.get_last_error().find("does not match"), std::string::npos);
        auto empty = bytes;
        empty.width = 0;
        EXPECT_FALSE(writer.write(absolute_path("invalid.png"), empty));
        EXPECT_NE(writer.get_last_error().find("zero"), std::string::npos);
        EXPECT_FALSE(writer.write(absolute_path("invalid.xyz"), bytes));
        EXPECT_NE(writer.get_last_error().find("Unsupported"), std::string::npos);
        EXPECT_FALSE(writer.write(absolute_path("missing/output.png"), bytes));
        EXPECT_FALSE(writer.get_last_error().empty());
        ASSERT_TRUE(writer.write(absolute_path("valid.png"), bytes)) << writer.get_last_error();
        EXPECT_TRUE(writer.get_last_error().empty());
    }

    TEST(STBImageWriterTest, RejectsShortBuffersAndUnrepresentableDimensions)
    {
        ScopedWorkingDir files;
        IO::STBImageWriter writer;
        auto bytes = byte_image(3);
        bytes.as_uint8()->pop_back();
        for (const auto* extension : { "png", "jpg", "bmp", "tga" }) {
            const auto path = absolute_path(std::string("short.") + extension);
            EXPECT_FALSE(writer.write(path, bytes));
            EXPECT_NE(writer.get_last_error().find("too small"), std::string::npos);
            EXPECT_FALSE(std::filesystem::exists(path));
        }

        auto floats = Kakshya::ImageData::allocate(2, 2, 3, Kakshya::ImageFormat::RGBA32F);
        ASSERT_TRUE(floats.has_value());
        floats->as_float()->pop_back();
        EXPECT_FALSE(writer.write(absolute_path("short.hdr"), *floats));
        EXPECT_NE(writer.get_last_error().find("too small"), std::string::npos);

        auto oversized = byte_image(4);
        oversized.width = std::numeric_limits<uint32_t>::max();
        EXPECT_FALSE(writer.write(absolute_path("oversized.png"), oversized));
        EXPECT_NE(writer.get_last_error().find("limits"), std::string::npos);
        auto too_many_channels = byte_image(4);
        too_many_channels.channels = 5;
        EXPECT_FALSE(writer.write(absolute_path("channels.png"), too_many_channels));
        EXPECT_NE(writer.get_last_error().find("limits"), std::string::npos);
    }

    TEST(STBImageWriterTest, BmpTgaAndHdrHonorFlipWithoutInheritingEarlierWrites)
    {
        ScopedWorkingDir files;
        auto bytes = Kakshya::ImageData::allocate(2, 2, 4, Kakshya::ImageFormat::RGBA8);
        auto floats = Kakshya::ImageData::allocate(2, 2, 4, Kakshya::ImageFormat::RGBA32F);
        ASSERT_TRUE(bytes.has_value());
        ASSERT_TRUE(floats.has_value());
        *bytes->as_uint8() = {
            0, 0, 0, 255, 0, 0, 0, 255,
            255, 255, 255, 255, 255, 255, 255, 255
        };
        *floats->as_float() = {
            0.F, 0.F, 0.F, 1.F, 0.F, 0.F, 0.F, 1.F,
            1.F, 1.F, 1.F, 1.F, 1.F, 1.F, 1.F, 1.F
        };
        const auto expected = *bytes->as_uint8();
        std::vector<uint8_t> flipped(expected.begin() + 8, expected.end());
        flipped.insert(flipped.end(), expected.begin(), expected.begin() + 8);
        IO::ImageWriteOptions flip;
        flip.flip_vertically = true;
        IO::STBImageWriter writer;
        for (const auto* extension : { "bmp", "tga", "hdr" }) {
            SCOPED_TRACE(extension);
            ASSERT_TRUE(writer.write(absolute_path("preceding.png"), *bytes, flip))
                << writer.get_last_error();
            const auto& source = std::string_view(extension) == "hdr" ? *floats : *bytes;
            const auto normal_path = absolute_path(std::string("normal.") + extension);
            ASSERT_TRUE(writer.write(normal_path, source)) << writer.get_last_error();
            const auto normal = IO::ImageReader::load(normal_path);
            ASSERT_TRUE(normal.has_value());
            ASSERT_NE(normal->as_uint8(), nullptr);
            EXPECT_EQ(*normal->as_uint8(), expected);

            const auto flipped_path = absolute_path(std::string("flipped.") + extension);
            ASSERT_TRUE(writer.write(flipped_path, source, flip)) << writer.get_last_error();
            const auto loaded = IO::ImageReader::load(flipped_path);
            ASSERT_TRUE(loaded.has_value());
            ASSERT_NE(loaded->as_uint8(), nullptr);
            EXPECT_EQ(*loaded->as_uint8(), flipped);
        }
    }

    TEST(STBImageWriterTest, RegistersFactoriesForSupportedExtensions)
    {
        IO::STBImageWriter writer;
        EXPECT_TRUE(writer.can_write("image.PNG"));
        EXPECT_FALSE(writer.can_write("image.exr"));
        const std::vector<std::string> extensions { "png", "jpg", "jpeg", "bmp", "tga", "hdr" };
        EXPECT_EQ(writer.get_supported_extensions(), extensions);
        IO::STBImageWriter::register_with_registry();
        for (const auto& extension : extensions) {
            const auto registered = IO::ImageWriterRegistry::instance().create_writer("image." + extension);
            ASSERT_NE(registered, nullptr);
            EXPECT_TRUE(registered->can_write("image." + extension));
        }
    }

    TEST(STBImageWriterTest, AsynchronousCpuSaveFlushesBeforeReading)
    {
        ScopedWorkingDir files;
        auto source = byte_image(4);
        const auto expected = *source.as_uint8();
        const auto path = absolute_path("async.png");
        Core::GlobalStreamInfo stream_info;
        IO::IOManager manager(stream_info, 60, nullptr, nullptr);
        ASSERT_TRUE(manager.save_image(source, path));
        std::ranges::fill(*source.as_uint8(), 0);
        manager.wait_for_pending_saves();
        const auto loaded = IO::ImageReader::load(path);
        ASSERT_TRUE(loaded.has_value());
        ASSERT_NE(loaded->as_uint8(), nullptr);
        EXPECT_EQ(*loaded->as_uint8(), expected);
    }

    TEST(STBImageWriterTest, ConcurrentAsynchronousWritesKeepTheirOwnOptions)
    {
        ScopedWorkingDir files;
        const auto source = byte_image(4, 32, 32);
        const auto& pixels = *source.as_uint8();
        const size_t row_size = source.width * source.channels;
        std::vector<uint8_t> flipped;
        flipped.reserve(pixels.size());
        for (size_t y = source.height; y > 0; --y) {
            const auto row = pixels.begin() + static_cast<std::ptrdiff_t>((y - 1) * row_size);
            flipped.insert(flipped.end(), row, row + static_cast<std::ptrdiff_t>(row_size));
        }

        Core::GlobalStreamInfo stream_info;
        IO::IOManager manager(stream_info, 60, nullptr, nullptr);
        std::vector<std::string> paths;
        for (int i = 0; i < 16; ++i) {
            IO::ImageWriteOptions options;
            options.flip_vertically = (i % 2) != 0;
            options.compression = (i % 2) != 0 ? 9 : 0;
            const auto extension = (i / 2) % 2 == 0 ? ".png" : ".bmp";
            paths.push_back(absolute_path("concurrent_" + std::to_string(i) + extension));
            ASSERT_TRUE(manager.save_image(source, paths.back(), options));
        }
        manager.wait_for_pending_saves();
        for (size_t i = 0; i < paths.size(); ++i) {
            SCOPED_TRACE(paths[i]);
            const auto loaded = IO::ImageReader::load(paths[i]);
            ASSERT_TRUE(loaded.has_value());
            ASSERT_NE(loaded->as_uint8(), nullptr);
            EXPECT_EQ(*loaded->as_uint8(), (i % 2) != 0 ? flipped : pixels);
        }
    }

} // namespace
} // namespace MayaFlux::Test
