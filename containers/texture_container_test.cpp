#include "gtest/gtest.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <typeindex>
#include <utility>
#include <variant>
#include <vector>

#include "MayaFlux/Kakshya/Region/Region.hpp"
#include "MayaFlux/Kakshya/Region/RegionSegment.hpp"
#include "MayaFlux/Kakshya/Source/TextureContainer.hpp"

namespace MayaFlux::Test {
namespace {

    using Kakshya::ImageFormat;
    using Kakshya::ProcessingState;
    using Kakshya::Region;
    using Kakshya::TextureContainer;

    Region box(std::vector<uint64_t> start, std::vector<uint64_t> end)
    {
        return Region(std::move(start), std::move(end));
    }

    std::vector<uint8_t> ramp(size_t n, uint8_t seed = 0)
    {
        std::vector<uint8_t> out(n);
        for (size_t i = 0; i < n; ++i) {
            out[i] = static_cast<uint8_t>((i * 5 + seed) & 0xFF);
        }
        return out;
    }

    // ------------------------------------------------------------------
    // Construction per format
    // ------------------------------------------------------------------

    enum class Storage : uint8_t {
        U8,
        U16,
        F32
    };

    struct FormatCase {
        std::string_view name;
        ImageFormat format;
        uint32_t channels;
        Storage storage;
    };

    class TextureContainerFormatTest : public ::testing::TestWithParam<FormatCase> { };

    TEST_P(TextureContainerFormatTest, StorageMatchesTheFormat)
    {
        const auto& c = GetParam();
        TextureContainer tc(5, 3, c.format);
        const size_t elements = size_t { 5 } * 3 * c.channels;

        EXPECT_EQ(tc.get_width(), 5U);
        EXPECT_EQ(tc.get_height(), 3U);
        EXPECT_EQ(tc.get_format(), c.format);
        EXPECT_EQ(tc.get_channel_count(), c.channels);
        EXPECT_EQ(tc.get_layer_count(), 1U);
        EXPECT_EQ(tc.row_stride() * 3, tc.byte_size());
        EXPECT_EQ(tc.pixel_bytes(0).size(), tc.byte_size());

        EXPECT_EQ(tc.as_uint8(0).size(), c.storage == Storage::U8 ? elements : 0U);
        EXPECT_EQ(tc.as_uint16(0).size(), c.storage == Storage::U16 ? elements : 0U);
        EXPECT_EQ(tc.as_float(0).size(), c.storage == Storage::F32 ? elements : 0U);

        const std::type_index expected = c.storage == Storage::U8 ? std::type_index(typeid(uint8_t))
            : c.storage == Storage::U16                           ? std::type_index(typeid(uint16_t))
                                                                  : std::type_index(typeid(float));
        EXPECT_EQ(tc.value_element_type(), expected);

        const auto bytes = tc.pixel_bytes(0);
        EXPECT_TRUE(std::all_of(bytes.begin(), bytes.end(), [](uint8_t v) { return v == 0; }));
    }

    INSTANTIATE_TEST_SUITE_P(
        Formats, TextureContainerFormatTest,
        ::testing::Values(
            FormatCase { "R8", ImageFormat::R8, 1, Storage::U8 },
            FormatCase { "RG8", ImageFormat::RG8, 2, Storage::U8 },
            FormatCase { "RGBA8", ImageFormat::RGBA8, 4, Storage::U8 },
            FormatCase { "R16", ImageFormat::R16, 1, Storage::U16 },
            FormatCase { "RGBA16", ImageFormat::RGBA16, 4, Storage::U16 },
            FormatCase { "R32F", ImageFormat::R32F, 1, Storage::F32 },
            FormatCase { "RGBA32F", ImageFormat::RGBA32F, 4, Storage::F32 }),
        [](const ::testing::TestParamInfo<FormatCase>& info) {
            return std::string(info.param.name);
        });

    TEST(TextureContainerConstructionTest, ZeroLayersIsClampedToOne)
    {
        TextureContainer tc(4, 4, ImageFormat::RGBA8, 0U);
        EXPECT_EQ(tc.get_layer_count(), 1U);
    }

    TEST(TextureContainerConstructionTest, SingleLayerDescribesHeightWidthChannels)
    {
        TextureContainer tc(6, 4, ImageFormat::RGBA8);
        const auto dims = tc.get_dimensions();

        ASSERT_EQ(dims.size(), 3U);
        EXPECT_EQ(dims[0].size, 4U);
        EXPECT_EQ(dims[1].size, 6U);
        EXPECT_EQ(dims[2].size, 4U);
        EXPECT_EQ(tc.get_total_elements(), 6U * 4U * 4U);
        EXPECT_EQ(tc.get_frame_size(), 6U * 4U);
        EXPECT_EQ(tc.get_num_frames(), 4U);
    }

    TEST(TextureContainerConstructionTest, MultipleLayersPrependALayerDimension)
    {
        TextureContainer tc(6, 4, ImageFormat::RGBA8, 3U);
        const auto dims = tc.get_dimensions();

        ASSERT_EQ(dims.size(), 4U);
        EXPECT_EQ(dims[0].size, 3U);
        EXPECT_EQ(dims[1].size, 4U);
        EXPECT_EQ(dims[2].size, 6U);
        EXPECT_EQ(dims[3].size, 4U);
        EXPECT_EQ(tc.get_layer_count(), 3U);
        EXPECT_EQ(tc.get_total_elements(), 3U * 6U * 4U * 4U);
    }

    TEST(TextureContainerConstructionTest, StartsReadyAndIdleWithData)
    {
        TextureContainer tc(2, 2, ImageFormat::RGBA8);
        EXPECT_TRUE(tc.is_ready_for_processing());
        EXPECT_EQ(tc.get_processing_state(), ProcessingState::IDLE);
        EXPECT_TRUE(tc.has_data());
        EXPECT_NE(tc.get_raw_data(), nullptr);
    }

    // ------------------------------------------------------------------
    // Pixel access
    // ------------------------------------------------------------------

    class TextureContainerPixelTest : public ::testing::Test {
    protected:
        TextureContainer tc { 2, 2, ImageFormat::RGBA8, 2U };
    };

    TEST_F(TextureContainerPixelTest, SetPixelsWritesOnlyTheAddressedLayer)
    {
        const auto first = ramp(16, 1);
        const auto second = ramp(16, 100);

        tc.set_pixels(std::span<const uint8_t>(first), 0);
        tc.set_pixels(std::span<const uint8_t>(second), 1);

        EXPECT_TRUE(std::ranges::equal(tc.as_uint8(0), first));
        EXPECT_TRUE(std::ranges::equal(tc.as_uint8(1), second));
    }

    TEST_F(TextureContainerPixelTest, SetPixelsRejectsWrongSizeWrongTypeAndBadLayer)
    {
        const auto good = ramp(16, 7);
        tc.set_pixels(std::span<const uint8_t>(good), 0);

        const auto short_data = ramp(8, 99);
        const std::vector<uint16_t> wide(16, 1);
        const std::vector<float> floats(16, 1.F);
        tc.set_pixels(std::span<const uint8_t>(short_data), 0);
        tc.set_pixels(std::span<const uint16_t>(wide), 0);
        tc.set_pixels(std::span<const float>(floats), 0);
        tc.set_pixels(std::span<const uint8_t>(good), 5);

        EXPECT_TRUE(std::ranges::equal(tc.as_uint8(0), good));
        EXPECT_TRUE(std::ranges::all_of(tc.as_uint8(1), [](uint8_t v) { return v == 0; }));
    }

    TEST_F(TextureContainerPixelTest, MutableBytesAliasTheStoredPixels)
    {
        tc.pixel_bytes(1)[3] = 0x42;
        EXPECT_EQ(tc.as_uint8(1)[3], 0x42);
        EXPECT_EQ(tc.as_uint8(0)[3], 0);
    }

    TEST_F(TextureContainerPixelTest, OutOfRangeLayersYieldEmptySpans)
    {
        EXPECT_TRUE(tc.pixel_bytes(2).empty());
        EXPECT_TRUE(std::as_const(tc).pixel_bytes(2).empty());
        EXPECT_TRUE(tc.as_uint8(2).empty());
        EXPECT_TRUE(tc.as_uint16(2).empty());
        EXPECT_TRUE(tc.as_float(2).empty());
        EXPECT_TRUE(tc.as_normalised_float(2).empty());
    }

    TEST_F(TextureContainerPixelTest, NormalisedViewScalesBytesAndTracksWrites)
    {
        std::vector<uint8_t> px(16, 0);
        px[0] = 255;
        px[1] = 51;
        tc.set_pixels(std::span<const uint8_t>(px), 0);

        auto norm = tc.as_normalised_float(0);
        ASSERT_EQ(norm.size(), 16U);
        EXPECT_FLOAT_EQ(norm[0], 1.F);
        EXPECT_FLOAT_EQ(norm[1], 0.2F);
        EXPECT_FLOAT_EQ(norm[2], 0.F);

        px[0] = 0;
        px[2] = 255;
        tc.set_pixels(std::span<const uint8_t>(px), 0);
        norm = tc.as_normalised_float(0);
        EXPECT_FLOAT_EQ(norm[0], 0.F);
        EXPECT_FLOAT_EQ(norm[2], 1.F);
    }

    TEST(TextureContainerNormalisedTest, SixteenBitScalesByFullRangeAndFloatPassesThrough)
    {
        TextureContainer u16(1, 1, ImageFormat::R16);
        const std::vector<uint16_t> wide { 65535 };
        u16.set_pixels(std::span<const uint16_t>(wide), 0);
        ASSERT_EQ(u16.as_normalised_float(0).size(), 1U);
        EXPECT_FLOAT_EQ(u16.as_normalised_float(0)[0], 1.F);

        TextureContainer f32(1, 1, ImageFormat::R32F);
        const std::vector<float> value { 0.375F };
        f32.set_pixels(std::span<const float>(value), 0);
        ASSERT_EQ(f32.as_normalised_float(0).size(), 1U);
        EXPECT_FLOAT_EQ(f32.as_normalised_float(0)[0], 0.375F);
    }

    TEST_F(TextureContainerPixelTest, ClearZeroesEveryLayerAndReturnsToIdle)
    {
        tc.set_pixels(std::span<const uint8_t>(ramp(16, 3)), 0);
        tc.set_pixels(std::span<const uint8_t>(ramp(16, 9)), 1);
        tc.update_processing_state(ProcessingState::READY);

        tc.clear();

        EXPECT_TRUE(std::ranges::all_of(tc.as_uint8(0), [](uint8_t v) { return v == 0; }));
        EXPECT_TRUE(std::ranges::all_of(tc.as_uint8(1), [](uint8_t v) { return v == 0; }));
        EXPECT_EQ(tc.get_processing_state(), ProcessingState::IDLE);
        EXPECT_EQ(tc.as_uint8(0).size(), 16U);
    }

    // ------------------------------------------------------------------
    // Coordinate access
    // ------------------------------------------------------------------

    TEST(TextureContainerValueTest, ValueAccessAddressesRowColumnChannel)
    {
        TextureContainer tc(3, 2, ImageFormat::RGBA8);
        tc.set_value_at<uint8_t>({ 1, 2, 3 }, 200);

        EXPECT_EQ(tc.get_value_at<uint8_t>({ 1, 2, 3 }), 200);
        EXPECT_EQ(tc.as_uint8(0)[(1 * 3 + 2) * 4 + 3], 200);
        EXPECT_EQ(tc.get_value_at<uint8_t>({ 0, 0, 0 }), 0);
    }

    TEST(TextureContainerValueTest, MultiLayerValueAccessLeadsWithTheLayer)
    {
        TextureContainer tc(2, 2, ImageFormat::RGBA8, 2U);
        tc.set_value_at<uint8_t>({ 1, 0, 1, 2 }, 77);

        EXPECT_EQ(tc.get_value_at<uint8_t>({ 1, 0, 1, 2 }), 77);
        EXPECT_EQ(tc.as_uint8(1)[(0 * 2 + 1) * 4 + 2], 77);
        EXPECT_EQ(tc.as_uint8(0)[(0 * 2 + 1) * 4 + 2], 0);
    }

    TEST(TextureContainerValueTest, WrongTypeOrShortCoordinatesAreIgnored)
    {
        TextureContainer tc(2, 2, ImageFormat::RGBA8);
        tc.set_value_at<float>({ 0, 0, 0 }, 1.F);
        tc.set_value_at<uint8_t>({ 0, 0 }, 9);
        tc.set_value_at<uint8_t>({ 9, 9, 9 }, 9);

        EXPECT_TRUE(std::ranges::all_of(tc.as_uint8(0), [](uint8_t v) { return v == 0; }));
        EXPECT_EQ(tc.get_value_at<float>({ 0, 0, 0 }), 0.F);
    }

    // ------------------------------------------------------------------
    // Frames (one frame per layer)
    // ------------------------------------------------------------------

    TEST(TextureContainerFrameTest, FramesAreLayersAndPadWithZeros)
    {
        TextureContainer tc(2, 2, ImageFormat::RGBA8, 2U);
        const auto first = ramp(16, 1);
        const auto second = ramp(16, 50);
        tc.set_pixels(std::span<const uint8_t>(first), 0);
        tc.set_pixels(std::span<const uint8_t>(second), 1);

        EXPECT_TRUE(std::ranges::equal(tc.get_frame_as<uint8_t>(1), second));
        EXPECT_TRUE(tc.get_frame_as<uint8_t>(2).empty());

        std::vector<uint8_t> out(40, 0xEE);
        tc.get_frames_as(out, 0, 2);
        EXPECT_TRUE(std::equal(first.begin(), first.end(), out.begin()));
        EXPECT_TRUE(std::equal(second.begin(), second.end(), out.begin() + 16));
        EXPECT_TRUE(std::all_of(out.begin() + 32, out.end(), [](uint8_t v) { return v == 0; }));
    }

    TEST(TextureContainerFrameTest, UnsupportedElementTypeThrows)
    {
        TextureContainer tc(2, 2, ImageFormat::RGBA8);
        std::vector<double> out(16);
        EXPECT_THROW(tc.get_frames<double>(std::span<double>(out), 0, 1), std::runtime_error);
    }

    // ------------------------------------------------------------------
    // Regions
    // ------------------------------------------------------------------

    TEST(TextureContainerRegionTest, GetRegionDataReturnsTheInclusiveBox)
    {
        TextureContainer tc(3, 3, ImageFormat::RGBA8);
        const auto pixels = ramp(36, 2);
        tc.set_pixels(std::span<const uint8_t>(pixels), 0);

        const auto data = tc.get_region_data(box({ 1, 1, 0 }, { 1, 2, 3 }));

        ASSERT_EQ(data.size(), 1U);
        const auto* values = std::get_if<std::vector<uint8_t>>(&data[0]);
        ASSERT_NE(values, nullptr);
        ASSERT_EQ(values->size(), 8U);
        for (size_t i = 0; i < 8; ++i) {
            EXPECT_EQ((*values)[i], pixels[(1 * 3 + 1) * 4 + i]) << i;
        }
    }

    TEST(TextureContainerRegionTest, SetRegionDataFillsTheBoxInReadingOrder)
    {
        TextureContainer tc(3, 3, ImageFormat::RGBA8);
        std::vector<uint8_t> src(16);
        for (size_t i = 0; i < src.size(); ++i) {
            src[i] = static_cast<uint8_t>(100 + i);
        }

        tc.set_region_data(box({ 1, 1 }, { 2, 2 }), { Kakshya::DataVariant(src) });

        const auto px = tc.as_uint8(0);
        for (uint32_t c = 0; c < 4; ++c) {
            EXPECT_EQ(px[(1 * 3 + 1) * 4 + c], src[0 * 4 + c]);
            EXPECT_EQ(px[(1 * 3 + 2) * 4 + c], src[1 * 4 + c]);
            EXPECT_EQ(px[(2 * 3 + 1) * 4 + c], src[2 * 4 + c]);
            EXPECT_EQ(px[(2 * 3 + 2) * 4 + c], src[3 * 4 + c]);
        }
        EXPECT_EQ(px[0], 0);
        EXPECT_EQ(px[(0 * 3 + 2) * 4], 0);
    }

    TEST(TextureContainerRegionTest, SetRegionDataIgnoresMismatchedTypeAndShortRegions)
    {
        TextureContainer tc(3, 3, ImageFormat::RGBA8);
        tc.set_region_data(box({ 0, 0 }, { 1, 1 }), { Kakshya::DataVariant(std::vector<float>(16, 1.F)) });
        tc.set_region_data(box({ 0 }, { 1 }), { Kakshya::DataVariant(std::vector<uint8_t>(16, 5)) });
        tc.set_region_data(box({ 0, 0 }, { 1, 1 }), {});

        EXPECT_TRUE(std::ranges::all_of(tc.as_uint8(0), [](uint8_t v) { return v == 0; }));
    }

    TEST(TextureContainerRegionTest, SegmentsDataReturnsEveryLayer)
    {
        TextureContainer tc(2, 2, ImageFormat::RGBA8, 3U);
        EXPECT_EQ(tc.get_segments_data({}).size(), 3U);
        EXPECT_EQ(tc.get_data().size(), 3U);
    }

    TEST(TextureContainerRegionGroupTest, GroupsAreStoredRetrievedAndRemoved)
    {
        TextureContainer tc(2, 2, ImageFormat::RGBA8);
        Kakshya::RegionGroup group;
        group.name = "marks";

        tc.add_region_group(group);
        EXPECT_EQ(tc.get_region_group("marks").name, "marks");
        EXPECT_EQ(tc.get_all_region_groups().size(), 1U);
        EXPECT_TRUE(tc.get_region_group("absent").name.empty());

        tc.remove_region_group("marks");
        EXPECT_TRUE(tc.get_all_region_groups().empty());
    }

    // ------------------------------------------------------------------
    // Signal source behaviour
    // ------------------------------------------------------------------

    TEST(TextureContainerStateTest, CallbackFiresOnStateChangesOnly)
    {
        auto tc = std::make_shared<TextureContainer>(2, 2, ImageFormat::RGBA8);
        std::vector<ProcessingState> seen;
        tc->register_state_change_callback(
            [&seen](const std::shared_ptr<Kakshya::SignalSourceContainer>&, ProcessingState s) {
                seen.push_back(s);
            });

        tc->update_processing_state(ProcessingState::READY);
        tc->update_processing_state(ProcessingState::READY);
        tc->update_processing_state(ProcessingState::IDLE);

        ASSERT_EQ(seen.size(), 2U);
        EXPECT_EQ(seen[0], ProcessingState::READY);
        EXPECT_EQ(seen[1], ProcessingState::IDLE);

        tc->unregister_state_change_callback();
        tc->update_processing_state(ProcessingState::READY);
        EXPECT_EQ(seen.size(), 2U);
        EXPECT_EQ(tc->get_processing_state(), ProcessingState::READY);
    }

    TEST(TextureContainerStateTest, ReadyFlagAndProcessingTokenToggle)
    {
        TextureContainer tc(2, 2, ImageFormat::RGBA8);
        tc.mark_ready_for_processing(false);
        EXPECT_FALSE(tc.is_ready_for_processing());
        tc.mark_ready_for_processing(true);
        EXPECT_TRUE(tc.is_ready_for_processing());

        EXPECT_TRUE(tc.try_acquire_processing_token(1));
        EXPECT_FALSE(tc.try_acquire_processing_token(2));
        EXPECT_TRUE(tc.has_processing_token(1));
        tc.reset_processing_token();
        EXPECT_TRUE(tc.try_acquire_processing_token(2));
    }

    TEST(TextureContainerStateTest, ProcessingChainIsCreatedOnDemand)
    {
        TextureContainer tc(2, 2, ImageFormat::RGBA8);
        const auto chain = tc.get_processing_chain();
        ASSERT_NE(chain, nullptr);
        EXPECT_EQ(tc.get_processing_chain(), chain);
    }

} // namespace
} // namespace MayaFlux::Test
