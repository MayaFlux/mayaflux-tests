#include "gtest/gtest.h"

#include "../support/scoped_working_dir.hpp"

#include "MayaFlux/IO/Image/EXRWriter.hpp"
#include "MayaFlux/IO/Image/ImageReader.hpp"
#include "MayaFlux/IO/IOManager.hpp"
#include "MayaFlux/Core/GlobalStreamInfo.hpp"

#include <tinyexr.h>

namespace MayaFlux::Test {
namespace {

    namespace fs = std::filesystem;

    constexpr uint32_t image_width = 3;
    constexpr uint32_t image_height = 2;

    struct HeaderSnapshot {
        int compression {};
        std::vector<int> pixel_types;
        std::vector<std::string> channel_names;
    };

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

    std::optional<HeaderSnapshot> parse_header(const std::string& path)
    {
        const auto bytes = read_bytes(path);
        if (bytes.empty()) {
            return std::nullopt;
        }

        const auto* memory = reinterpret_cast<const unsigned char*>(bytes.data());
        EXRVersion version {};
        if (ParseEXRVersionFromMemory(&version, memory, bytes.size()) != TINYEXR_SUCCESS) {
            return std::nullopt;
        }

        EXRHeader header;
        InitEXRHeader(&header);
        const char* error = nullptr;
        const int result = ParseEXRHeaderFromMemory(
            &header, &version, memory, bytes.size(), &error);
        if (error) {
            FreeEXRErrorMessage(error);
        }
        if (result != TINYEXR_SUCCESS) {
            FreeEXRHeader(&header);
            return std::nullopt;
        }

        HeaderSnapshot snapshot;
        snapshot.compression = header.compression_type;
        for (int channel = 0; channel < header.num_channels; ++channel) {
            snapshot.pixel_types.push_back(header.pixel_types[channel]);
            snapshot.channel_names.emplace_back(header.channels[channel].name);
        }
        FreeEXRHeader(&header);
        return snapshot;
    }

    std::vector<std::string> default_channel_names(uint32_t channels)
    {
        switch (channels) {
        case 1:
            return { "Y" };
        case 2:
            return { "Y", "A" };
        case 3:
            return { "B", "G", "R" };
        case 4:
            return { "A", "B", "G", "R" };
        default: {
            std::vector<std::string> names;
            for (uint32_t channel = 0; channel < channels; ++channel) {
                names.push_back("C" + std::to_string(channel));
            }
            return names;
        }
        }
    }

    struct ChannelCase {
        uint32_t channels;
    };

    class EXRWriterChannelTest : public ::testing::TestWithParam<ChannelCase> { };

    TEST_P(EXRWriterChannelTest, WritesFloatChannelsWithDefaultNamesAndReaderRoundTrips)
    {
        ScopedWorkingDir files;
        const auto source = float_image(GetParam().channels);
        const auto path = absolute_path("default_channels.exr");
        IO::EXRWriter writer;
        ASSERT_TRUE(writer.write(path, source)) << writer.get_last_error();
        EXPECT_TRUE(writer.get_last_error().empty());

        const auto header = parse_header(path);
        ASSERT_TRUE(header.has_value());
        EXPECT_EQ(header->channel_names, default_channel_names(GetParam().channels));
        EXPECT_EQ(header->pixel_types,
            std::vector<int>(GetParam().channels, TINYEXR_PIXELTYPE_FLOAT));
        EXPECT_EQ(header->compression, TINYEXR_COMPRESSIONTYPE_ZIP);

        if (GetParam().channels > 4) {
            EXPECT_FALSE(IO::ImageReader::load(path));
            return;
        }

        const auto loaded = IO::ImageReader::load(path);
        ASSERT_TRUE(loaded.has_value());
        ASSERT_NE(loaded->as_float(), nullptr);
        EXPECT_EQ(loaded->channels, GetParam().channels == 3 ? 4U : GetParam().channels);
        EXPECT_EQ(*loaded->as_float(), decoded_pixels(source));
    }

    INSTANTIATE_TEST_SUITE_P(
        ChannelCounts, EXRWriterChannelTest,
        ::testing::Values(ChannelCase { 1 }, ChannelCase { 2 }, ChannelCase { 3 },
            ChannelCase { 4 }, ChannelCase { 5 }));

    TEST(EXRWriterTest, HonorsCustomChannelNamesAndFallsBackForMismatchedNames)
    {
        ScopedWorkingDir files;
        const auto source = float_image(4);
        IO::EXRWriter writer;

        IO::ImageWriteOptions custom_options;
        custom_options.channel_names = { "position.x", "position.y", "position.z", "entity_id" };
        const auto custom_path = absolute_path("custom_names.exr");
        ASSERT_TRUE(writer.write(custom_path, source, custom_options)) << writer.get_last_error();
        const auto custom_header = parse_header(custom_path);
        ASSERT_TRUE(custom_header.has_value());
        EXPECT_EQ(custom_header->channel_names, custom_options.channel_names);

        IO::ImageWriteOptions mismatched_options;
        mismatched_options.channel_names = { "only_one" };
        const auto fallback_path = absolute_path("fallback_names.exr");
        ASSERT_TRUE(writer.write(fallback_path, source, mismatched_options)) << writer.get_last_error();
        const auto fallback_header = parse_header(fallback_path);
        ASSERT_TRUE(fallback_header.has_value());
        EXPECT_EQ(fallback_header->channel_names, default_channel_names(4));
    }

    TEST(EXRWriterTest, SupportsDocumentedCompressionModesAndFallsBackForUnknownModes)
    {
        ScopedWorkingDir files;
        const auto source = float_image(1);
        IO::EXRWriter writer;
        const std::array<int, 5> modes {
            TINYEXR_COMPRESSIONTYPE_NONE,
            TINYEXR_COMPRESSIONTYPE_RLE,
            TINYEXR_COMPRESSIONTYPE_ZIPS,
            TINYEXR_COMPRESSIONTYPE_ZIP,
            TINYEXR_COMPRESSIONTYPE_PIZ,
        };

        for (size_t i = 0; i < modes.size(); ++i) {
            IO::ImageWriteOptions options;
            options.compression = modes[i];
            const auto path = absolute_path("compression_" + std::to_string(i) + ".exr");
            ASSERT_TRUE(writer.write(path, source, options)) << writer.get_last_error();
            const auto header = parse_header(path);
            ASSERT_TRUE(header.has_value());
            EXPECT_EQ(header->compression, modes[i]);
        }

        IO::ImageWriteOptions invalid;
        invalid.compression = 9999;
        const auto path = absolute_path("compression_fallback.exr");
        ASSERT_TRUE(writer.write(path, source, invalid)) << writer.get_last_error();
        const auto header = parse_header(path);
        ASSERT_TRUE(header.has_value());
        EXPECT_EQ(header->compression, TINYEXR_COMPRESSIONTYPE_ZIP);
    }

    TEST(EXRWriterTest, FlipVerticallyReversesRowsAndRoundTrips)
    {
        ScopedWorkingDir files;
        const auto source = float_image(4);
        IO::ImageWriteOptions options;
        options.flip_vertically = true;
        const auto path = absolute_path("flipped.exr");
        IO::EXRWriter writer;
        ASSERT_TRUE(writer.write(path, source, options)) << writer.get_last_error();

        const auto loaded = IO::ImageReader::load(path);
        ASSERT_TRUE(loaded.has_value());
        ASSERT_NE(loaded->as_float(), nullptr);
        const auto& input = *source.as_float();
        const std::vector<float> expected {
            input[12], input[13], input[14], input[15],
            input[16], input[17], input[18], input[19],
            input[20], input[21], input[22], input[23],
            input[0], input[1], input[2], input[3],
            input[4], input[5], input[6], input[7],
            input[8], input[9], input[10], input[11],
        };
        EXPECT_EQ(*loaded->as_float(), expected);
    }

    TEST(EXRWriterTest, GradientRoundTripsAcrossScanlineBlocks)
    {
        ScopedWorkingDir files;
        constexpr uint32_t width = 256;
        constexpr uint32_t height = 256;
        auto source = Kakshya::ImageData::allocate(
            width, height, 4, Kakshya::ImageFormat::RGBA32F);
        ASSERT_TRUE(source.has_value());
        auto& pixels = *source->as_float();
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                const auto offset = (static_cast<size_t>(y) * width + x) * 4;
                pixels[offset] = static_cast<float>(x) / static_cast<float>(width - 1);
                pixels[offset + 1] = static_cast<float>(y) / static_cast<float>(height - 1);
                pixels[offset + 2] = 0.5F;
                pixels[offset + 3] = 1.0F;
            }
        }

        const auto path = absolute_path("gradient.exr");
        IO::EXRWriter writer;
        ASSERT_TRUE(writer.write(path, *source)) << writer.get_last_error();
        const auto loaded = IO::ImageReader::load(path);
        ASSERT_TRUE(loaded.has_value());
        EXPECT_EQ(loaded->width, width);
        EXPECT_EQ(loaded->height, height);
        EXPECT_EQ(loaded->format, Kakshya::ImageFormat::RGBA32F);
        ASSERT_NE(loaded->as_float(), nullptr);
        EXPECT_EQ(*loaded->as_float(), pixels);
    }

    TEST(EXRWriterTest, RejectsNonFloatInconsistentEmptyAndShortImages)
    {
        ScopedWorkingDir files;
        IO::EXRWriter writer;
        const auto path = absolute_path("invalid.exr");

        auto uint8 = Kakshya::ImageData::allocate(
            1, 1, 4, Kakshya::ImageFormat::RGBA8);
        ASSERT_TRUE(uint8.has_value());
        EXPECT_FALSE(writer.write(path, *uint8));
        EXPECT_NE(writer.get_last_error().find("float"), std::string::npos);

        auto uint16 = Kakshya::ImageData::allocate(
            1, 1, 4, Kakshya::ImageFormat::RGBA16);
        ASSERT_TRUE(uint16.has_value());
        EXPECT_FALSE(writer.write(path, *uint16));
        EXPECT_NE(writer.get_last_error().find("float"), std::string::npos);

        auto inconsistent = float_image(4);
        inconsistent.format = Kakshya::ImageFormat::RGBA8;
        EXPECT_FALSE(writer.write(path, inconsistent));
        EXPECT_NE(writer.get_last_error().find("does not match"), std::string::npos);

        Kakshya::ImageData empty;
        empty.channels = 4;
        empty.format = Kakshya::ImageFormat::RGBA32F;
        empty.pixels = std::vector<float>(4);
        EXPECT_FALSE(writer.write(path, empty));
        EXPECT_NE(writer.get_last_error().find("zero"), std::string::npos);

        Kakshya::ImageData short_buffer;
        short_buffer.width = 2;
        short_buffer.height = 2;
        short_buffer.channels = 2;
        short_buffer.format = Kakshya::ImageFormat::RG32F;
        short_buffer.pixels = std::vector<float>(3);
        EXPECT_FALSE(writer.write(path, short_buffer));
        EXPECT_NE(writer.get_last_error().find("too small"), std::string::npos);
    }

    TEST(EXRWriterTest, ReportsFilesystemFailuresAndClearsErrorsAfterSuccess)
    {
        ScopedWorkingDir files;
        const auto source = float_image(1);
        IO::EXRWriter writer;

        const auto invalid_path = absolute_path("missing_directory/output.exr");
        EXPECT_FALSE(writer.write(invalid_path, source));
        EXPECT_FALSE(writer.get_last_error().empty());

        const auto valid_path = absolute_path("output.exr");
        ASSERT_TRUE(writer.write(valid_path, source)) << writer.get_last_error();
        EXPECT_TRUE(writer.get_last_error().empty());
    }

    TEST(EXRWriterTest, AsynchronousCpuSaveFlushesBeforeReading)
    {
        ScopedWorkingDir files;
        auto source = float_image(4);
        const auto expected = *source.as_float();
        const auto path = absolute_path("async.exr");
        Core::GlobalStreamInfo stream_info;
        IO::IOManager manager(stream_info, 60, nullptr, nullptr);
        ASSERT_TRUE(manager.save_image(source, path));
        std::ranges::fill(*source.as_float(), 0.0F);
        manager.wait_for_pending_saves();
        const auto loaded = IO::ImageReader::load(path);
        ASSERT_TRUE(loaded.has_value());
        ASSERT_NE(loaded->as_float(), nullptr);
        EXPECT_EQ(*loaded->as_float(), expected);
    }

    TEST(EXRWriterTest, RegistersAnExtensionFactory)
    {
        IO::EXRWriter writer;
        EXPECT_TRUE(writer.can_write("image.exr"));
        EXPECT_TRUE(writer.can_write("image.EXR"));
        EXPECT_FALSE(writer.can_write("image.png"));
        EXPECT_EQ(writer.get_supported_extensions(), (std::vector<std::string> { "exr" }));

        IO::EXRWriter::register_with_registry();
        const auto registered = IO::ImageWriterRegistry::instance().create_writer("image.exr");
        ASSERT_NE(registered, nullptr);
        EXPECT_TRUE(registered->can_write("image.exr"));
    }

} // namespace
} // namespace MayaFlux::Test
