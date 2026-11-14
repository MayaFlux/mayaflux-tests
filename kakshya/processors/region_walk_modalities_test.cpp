#include "gtest/gtest.h"
#include "../../support/test_metadata.hpp"

#include "../video_test_support.hpp"
#include "../window_test_support.hpp"

#include "MayaFlux/Kakshya/Processors/RegionWalkProcessor.hpp"
#include "MayaFlux/Kakshya/Source/TextureContainer.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Kakshya::ImageFormat;
    using Kakshya::ProcessingState;
    using Kakshya::Region;
    using Kakshya::RegionDelivery;
    using Kakshya::RegionGroup;
    using Kakshya::RegionSelectionPattern;
    using Kakshya::RegionWalkProcessor;
    using Kakshya::TextureContainer;
    using namespace VideoSupport;
    using namespace WindowSupport;

    using Bytes = std::vector<uint8_t>;

    std::vector<uint8_t> ramp(size_t n, uint8_t seed)
    {
        std::vector<uint8_t> out(n);
        for (size_t i = 0; i < n; ++i) {
            out[i] = static_cast<uint8_t>((i * 5 + seed) & 0xFF);
        }
        return out;
    }

    Bytes crop(const Bytes& pixels, size_t width, size_t channels, size_t y0, size_t x0, size_t y1, size_t x1)
    {
        Bytes out;
        for (size_t y = y0; y <= y1; ++y) {
            for (size_t x = x0; x <= x1; ++x) {
                for (size_t c = 0; c < channels; ++c) {
                    out.push_back(pixels[(y * width + x) * channels + c]);
                }
            }
        }
        return out;
    }

    Bytes delivered(const std::shared_ptr<Kakshya::SignalSourceContainer>& container)
    {
        const auto& data = container->get_processed_data();
        return data.empty() ? Bytes {} : std::get<Bytes>(data[0]);
    }

    RegionGroup group_of(std::vector<Region> regions, RegionSelectionPattern pattern = RegionSelectionPattern::SEQUENTIAL)
    {
        RegionGroup group("walk", std::move(regions));
        group.region_selection_pattern = pattern;
        return group;
    }

    // ---- BLOCK is audio only ---------------------------------------------------------------

    TEST(RegionWalkModalityTest, BlockDeliveryIsRefusedByEveryNonAudioContainer)
    {
        const auto texture = std::make_shared<TextureContainer>(3, 3, ImageFormat::RGBA8);
        const auto video = std::make_shared<VideoProbe>(k_width, k_height, ImageFormat::RGBA8, k_fps);
        WindowRig rig;

        RegionWalkProcessor for_texture("walk", RegionDelivery::BLOCK, 4);
        RegionWalkProcessor for_video("walk", RegionDelivery::BLOCK, 4);
        RegionWalkProcessor for_window("walk", RegionDelivery::BLOCK, 4);

        EXPECT_THROW(for_texture.on_attach(texture), std::invalid_argument);
        EXPECT_THROW(for_video.on_attach(video), std::invalid_argument);
        EXPECT_THROW(for_window.on_attach(rig.container), std::invalid_argument);
    }

    // ---- textures --------------------------------------------------------------------------

    TEST(RegionWalkTextureTest, EachCropOfASingleLayerTextureIsDeliveredInTurn)
    {
        const auto texture = std::make_shared<TextureContainer>(3, 3, ImageFormat::RGBA8);
        const auto pixels = ramp(36, 2);
        texture->set_pixels(std::span<const uint8_t>(pixels), 0);
        texture->add_region_group(group_of({ Region({ 0, 0, 0 }, { 1, 1, 3 }), Region({ 1, 1, 0 }, { 2, 2, 3 }) }));
        RegionWalkProcessor processor("walk", RegionDelivery::WHOLE);
        processor.on_attach(texture);

        processor.process(texture);
        EXPECT_EQ(delivered(texture), crop(pixels, 3, 4, 0, 0, 1, 1));
        EXPECT_EQ(processor.contributions().size(), 1U);

        processor.process(texture);
        EXPECT_EQ(delivered(texture), crop(pixels, 3, 4, 1, 1, 2, 2));
        EXPECT_EQ(texture->get_processing_state(), ProcessingState::PROCESSED);
    }

    TEST(RegionWalkTextureTest, EachLayerOfAMultiLayerTextureIsDeliveredWhole)
    {
        const auto texture = std::make_shared<TextureContainer>(2, 2, ImageFormat::RGBA8, 3U);
        std::vector<Bytes> layers;
        for (uint8_t layer = 0; layer < 3; ++layer) {
            layers.push_back(ramp(16, static_cast<uint8_t>(40 * (layer + 1))));
            texture->set_pixels(std::span<const uint8_t>(layers.back()), layer);
        }
        texture->add_region_group(group_of({ Region({ 0, 0, 0, 0 }, { 0, 1, 1, 3 }),
            Region({ 1, 0, 0, 0 }, { 1, 1, 1, 3 }),
            Region({ 2, 0, 0, 0 }, { 2, 1, 1, 3 }) }));
        RegionWalkProcessor processor("walk", RegionDelivery::WHOLE);
        processor.on_attach(texture);

        for (size_t layer = 0; layer < 3; ++layer) {
            processor.process(texture);
            EXPECT_EQ(delivered(texture), layers[layer]) << "layer " << layer;
        }
    }

    // ---- video -----------------------------------------------------------------------------

    TEST(RegionWalkVideoTest, EachFrameRegionDeliversThatFrame)
    {
        const auto video = std::make_shared<VideoProbe>(k_width, k_height, ImageFormat::RGBA8, k_fps);
        video->load(frames_bytes(0, 4), 4);
        const auto frame_region = [](uint64_t frame) {
            return Region({ frame, 0, 0, 0 }, { frame, k_height - 1, k_width - 1, 3 });
        };
        video->add_region_group(group_of({ frame_region(2), frame_region(0), frame_region(3) }));
        RegionWalkProcessor processor("walk", RegionDelivery::WHOLE);
        processor.on_attach(video);

        processor.process(video);
        EXPECT_EQ(delivered(video), frame_bytes(2));
        processor.process(video);
        EXPECT_EQ(delivered(video), frame_bytes(0));
        processor.process(video);
        EXPECT_EQ(delivered(video), frame_bytes(3));
    }

    TEST(RegionWalkVideoTest, AFrameBeyondTheLoadedOnesIsWaitedOn)
    {
        const auto video = std::make_shared<VideoProbe>(k_width, k_height, ImageFormat::RGBA8, k_fps);
        video->load(frames_bytes(0, 2), 2);
        video->add_region_group(group_of({ Region({ 5, 0, 0, 0 }, { 5, k_height - 1, k_width - 1, 3 }) }));
        RegionWalkProcessor processor("walk", RegionDelivery::WHOLE);
        processor.on_attach(video);

        processor.process(video);

        EXPECT_TRUE(processor.contributions().empty());
        EXPECT_EQ(processor.current_region(), std::optional<size_t> { 0 });
    }

    // ---- window ----------------------------------------------------------------------------

    class RegionWalkWindowTest : public ::testing::Test {
    protected:
        std::vector<uint8_t> capture(uint64_t seed)
        {
            const auto frame = rig.display.present(seed);
            rig.container->process_default();
            return frame;
        }

        WindowRig rig;
    };

    TEST_F(RegionWalkWindowTest, NothingIsDeliveredUntilTheFirstCapture)
    {
        rig.container->create_default_processor();
        rig.container->add_region_group(group_of({ Region({ 1, 1 }, { 2, 2 }) }));
        RegionWalkProcessor processor("walk", RegionDelivery::WHOLE);
        processor.on_attach(rig.container);

        processor.process(rig.container);

        EXPECT_TRUE(processor.contributions().empty());
        EXPECT_EQ(rig.container->get_processing_state(), ProcessingState::IDLE);
    }

    TEST_F(RegionWalkWindowTest, ARegionDeliversItsCropOfTheLatestCapture)
    {
        rig.container->create_default_processor();
        rig.container->add_region_group(group_of({ Region({ 1, 1 }, { 2, 2 }) }, RegionSelectionPattern::ROUND_ROBIN));
        RegionWalkProcessor processor("walk", RegionDelivery::WHOLE);
        processor.on_attach(rig.container);

        for (const uint64_t seed : { 1U, 2U }) {
            const auto frame = capture(seed);
            processor.process(rig.container);
            EXPECT_EQ(delivered(rig.container),
                crop(frame, k_surface_width, k_surface_channels, 1, 1, 2, 2))
                << "capture " << seed;
        }
    }

} // namespace
} // namespace MayaFlux::Test
