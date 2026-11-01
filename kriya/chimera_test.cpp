#include "gtest/gtest.h"

#include "../kakshya/video_test_support.hpp"
#include "../support/frame_scheduler.hpp"
#include "../support/vulkan_test_support.hpp"

#include "MayaFlux/Buffers/BufferManager.hpp"
#include "MayaFlux/Buffers/BufferProcessingChain.hpp"
#include "MayaFlux/Buffers/Staging/StagingUtils.hpp"
#include "MayaFlux/Buffers/Textures/TextureArrayBuffer.hpp"
#include "MayaFlux/Buffers/VKBuffer.hpp"
#include "MayaFlux/Kakshya/Source/DynamicVideoStream.hpp"
#include "MayaFlux/Kakshya/Source/TextureCollection.hpp"
#include "MayaFlux/Kriya/BufferPipeline.hpp"
#include "MayaFlux/Kriya/Chimera.hpp"
#include "MayaFlux/Registry/BackendRegistry.hpp"
#include "MayaFlux/Portal/Graphics/TextureLoom.hpp"
#include "MayaFlux/Registry/Service/BufferService.hpp"
#include "MayaFlux/Vruta/ChronUtils.hpp"

namespace MayaFlux::Test {
namespace {

    using Buffers::ProcessingToken;
    using Buffers::TextureArrayBuffer;
    using Kakshya::DataVariant;
    using Kakshya::DynamicVideoStream;
    using Kakshya::ImageData;
    using Kriya::BufferOperation;
    using Kriya::BufferPipeline;
    using Kriya::Chimera;
    using Kriya::ChimeraBuilder;

    constexpr uint64_t k_ring_capacity = 64;
    constexpr size_t k_frame_bytes = VideoSupport::k_rgba8_frame_bytes;

    ImageData still(uint64_t seed)
    {
        const auto bytes = VideoSupport::frame_bytes(seed);
        auto image = ImageData::from_bytes(
            VideoSupport::k_width, VideoSupport::k_height, 4, Kakshya::ImageFormat::RGBA8, bytes);
        return *image;
    }

    std::vector<uint8_t> blank()
    {
        return std::vector<uint8_t>(k_frame_bytes, 0);
    }

    class ChimeraTest : public ::testing::Test {
    protected:
        void SetUp() override
        {
            frames = std::make_unique<FrameScheduler>();
            manager = std::make_shared<Buffers::BufferManager>();
            fps = static_cast<double>(Vruta::s_registered_frame_rate);
            ring_rate = fps / 2.0;
            reach = static_cast<uint64_t>(ring_rate);
        }

        void TearDown() override
        {
            for (const auto& pipeline : pipelines) {
                pipeline->end();
            }
            pipelines.clear();
            frames.reset();
        }

        [[nodiscard]] std::shared_ptr<TextureArrayBuffer> make_array(
            uint32_t layers,
            std::optional<Portal::Graphics::FitMode> fit = Portal::Graphics::FitMode::STRETCH) const
        {
            return std::make_shared<TextureArrayBuffer>(VideoSupport::rgba8_spec(layers), fit);
        }

        [[nodiscard]] std::shared_ptr<DynamicVideoStream> make_ring(uint64_t written, uint64_t capacity = k_ring_capacity) const
        {
            auto ring = std::make_shared<DynamicVideoStream>(VideoSupport::rgba8_spec(capacity, ring_rate));
            (void)VideoSupport::append_ramp(*ring, written);
            return ring;
        }

        static void grow(const std::shared_ptr<DynamicVideoStream>& ring, uint64_t count)
        {
            (void)VideoSupport::append_ramp(*ring, count, ring->get_write_head());
        }

        std::shared_ptr<BufferPipeline> make_pipeline()
        {
            auto pipeline = BufferPipeline::create(*frames->scheduler, manager);
            pipelines.push_back(pipeline);
            return pipeline;
        }

        ChimeraBuilder builder(const std::shared_ptr<TextureArrayBuffer>& array)
        {
            ChimeraBuilder result(array, make_pipeline());
            result.record(BufferOperation::dispatch_to([this](DataVariant&, uint32_t cycle) {
                cycles.push_back(cycle);
            }));
            return result;
        }

        [[nodiscard]] const std::shared_ptr<BufferPipeline>& pipeline() const { return pipelines.back(); }

        uint32_t step()
        {
            const size_t before = cycles.size();
            for (int attempt = 0; attempt < 4 && cycles.size() == before; ++attempt) {
                frames->tick();
            }
            if (cycles.size() == before) {
                ADD_FAILURE() << "the pipeline ran no cycle";
                return 0;
            }
            return cycles.back();
        }

        void run_until_cycle(uint32_t target)
        {
            for (int guard = 0; guard < 1000 && (cycles.empty() || cycles.back() < target); ++guard) {
                frames->tick();
            }
        }

        static std::vector<uint8_t> layer_bytes(const std::shared_ptr<TextureArrayBuffer>& array, uint32_t layer)
        {
            const auto& storage = array->get_pixel_data();
            const size_t begin = static_cast<size_t>(layer) * k_frame_bytes;
            if (storage.size() < begin + k_frame_bytes) {
                return blank();
            }
            return { storage.begin() + static_cast<std::ptrdiff_t>(begin),
                storage.begin() + static_cast<std::ptrdiff_t>(begin + k_frame_bytes) };
        }

        static void expect_frame(const std::shared_ptr<TextureArrayBuffer>& array, uint32_t layer, uint64_t frame)
        {
            EXPECT_EQ(layer_bytes(array, layer), VideoSupport::frame_bytes(frame)) << "layer " << layer << " frame " << frame;
        }

        static void expect_unfed(const std::shared_ptr<TextureArrayBuffer>& array, uint32_t layer)
        {
            EXPECT_EQ(layer_bytes(array, layer), blank()) << "layer " << layer;
        }

        std::unique_ptr<FrameScheduler> frames;
        std::shared_ptr<Buffers::BufferManager> manager;
        std::vector<std::shared_ptr<BufferPipeline>> pipelines;
        std::vector<uint32_t> cycles;
        double fps {};
        double ring_rate {};
        uint64_t reach {};
    };

    TEST_F(ChimeraTest, ANullBufferGivesAnEmptySet)
    {
        const Chimera chimera = ChimeraBuilder(nullptr, make_pipeline()).layer().from(still(1)).start();

        EXPECT_EQ(chimera.layer_count(), 0U);
    }

    TEST_F(ChimeraTest, NoDeclaredLayerGivesAnEmptySet)
    {
        const Chimera chimera = builder(make_array(2)).start();

        EXPECT_EQ(chimera.layer_count(), 0U);
    }

    TEST_F(ChimeraTest, MoreLayersThanTheBufferHoldsGivesAnEmptySet)
    {
        const Chimera chimera = builder(make_array(1)).layer().from(still(1)).layer().from(still(2)).start();

        EXPECT_EQ(chimera.layer_count(), 0U);
    }

    TEST_F(ChimeraTest, ANullPipelineGivesAnEmptySet)
    {
        const Chimera chimera = ChimeraBuilder(make_array(1), nullptr).layer().from(still(1)).start();

        EXPECT_EQ(chimera.layer_count(), 0U);
    }

    TEST_F(ChimeraTest, FromPipelineWithNoStreamInThePipelineGivesAnEmptySet)
    {
        const Chimera chimera = builder(make_array(1)).layer().from_pipeline().start();

        EXPECT_EQ(chimera.layer_count(), 0U);
    }

    TEST_F(ChimeraTest, LayersAreCountedInDeclarationOrder)
    {
        const Chimera chimera = builder(make_array(4)).layer().from(still(1)).layer().from(still(2)).layer().from(still(3)).start();

        EXPECT_EQ(chimera.layer_count(), 3U);
    }

    TEST_F(ChimeraTest, ControlsOnAnEmptySetDoNothing)
    {
        Chimera chimera;

        EXPECT_NO_THROW(chimera.level(0, 0.5));
        EXPECT_NO_THROW(chimera.params(0, glm::vec4(1.0F)));
        EXPECT_NO_THROW(chimera.speed(0, 2.0));
        EXPECT_NO_THROW(chimera.cut(0, 1.0));
        EXPECT_NO_THROW(chimera.set(0, still(1)));
        EXPECT_NO_THROW(chimera.stop());
    }

    TEST_F(ChimeraTest, SpeedAndCutOnALayerThatWasNotDeclaredThrow)
    {
        Chimera chimera = builder(make_array(4)).layer().from(make_ring(40)).start();

        EXPECT_THROW(chimera.speed(1, 1.0), std::out_of_range);
        EXPECT_THROW(chimera.cut(1, 1.0), std::out_of_range);
    }

    TEST_F(ChimeraTest, ALayerWithoutASourceStaysUnfed)
    {
        const auto array = make_array(2);
        const Chimera chimera = builder(array).layer().layer().from(still(4)).start();

        step();
        step();

        expect_unfed(array, 0);
        expect_frame(array, 1, 4);
    }

    TEST_F(ChimeraTest, AStillImageFillsItsLayerOnTheFirstFrameAndNoOther)
    {
        const auto array = make_array(3);
        const Chimera chimera = builder(array).layer().from(still(5)).start();

        step();

        expect_frame(array, 0, 5);
        expect_unfed(array, 1);
        expect_unfed(array, 2);
    }

    TEST_F(ChimeraTest, AStillImageIsDeliveredOnceAndNotRewritten)
    {
        const auto array = make_array(1);
        const Chimera chimera = builder(array).layer().from(still(5)).start();

        step();
        const auto other = VideoSupport::frame_bytes(9);
        ASSERT_TRUE(array->submit_layer(0, std::span<const uint8_t>(other)));
        step();
        step();

        EXPECT_EQ(layer_bytes(array, 0), other);
    }

    TEST_F(ChimeraTest, ALayerThatEntersAfterADelayIsUnfedUntilThen)
    {
        const auto array = make_array(2);
        const Chimera chimera = builder(array).layer().from(still(2)).layer().from(still(3)).enters_after(0.5).start();
        const auto entry = static_cast<uint32_t>(std::round(0.5 * fps));

        for (uint32_t i = 0; i < entry + 4; ++i) {
            const uint32_t cycle = step();
            expect_frame(array, 0, 2);
            if (cycle < entry) {
                expect_unfed(array, 1);
            } else {
                expect_frame(array, 1, 3);
            }
        }
    }

    TEST_F(ChimeraTest, ARingLayerThatEntersAfterADelayIsUnfedUntilThen)
    {
        const auto array = make_array(1);
        const auto ring = make_ring(40);
        const Chimera chimera = builder(array).layer().from(ring).lag(0.0).enters_after(0.5).start();
        const auto entry = static_cast<uint32_t>(std::round(0.5 * fps));

        for (uint32_t i = 0; i < entry + 3; ++i) {
            const uint32_t cycle = step();
            if (cycle < entry) {
                expect_unfed(array, 0);
            } else {
                expect_frame(array, 0, 38);
            }
        }
    }

    TEST_F(ChimeraTest, SettingAStillReplacesTheContentOnTheNextFrame)
    {
        const auto array = make_array(1);
        const auto ring = make_ring(40);
        Chimera chimera = builder(array).layer().from(ring).lag(0.0).start();

        step();
        expect_frame(array, 0, 38);
        chimera.set(0, still(7));
        step();
        grow(ring, 10);
        step();

        expect_frame(array, 0, 7);
    }

    TEST_F(ChimeraTest, SettingARingReplacesAStillAndFollowsTheRing)
    {
        const auto array = make_array(1);
        Chimera chimera = builder(array).layer().from(still(7)).lag(0.0).start();

        step();
        expect_frame(array, 0, 7);
        chimera.set(0, make_ring(30));
        step();

        expect_frame(array, 0, 28);
    }

    TEST_F(ChimeraTest, AFileThatCannotBeLoadedLeavesTheLayerUnfed)
    {
        const auto array = make_array(1);
        const Chimera chimera = builder(array).layer().from(std::string("/no/such/image.png")).start();

        step();
        step();

        EXPECT_EQ(chimera.layer_count(), 1U);
        expect_unfed(array, 0);
    }

    TEST_F(ChimeraTest, AFrameOfAnotherExtentIsDroppedWhenTheBufferHasNoFit)
    {
        const auto array = make_array(1, std::nullopt);
        const auto small = std::make_shared<DynamicVideoStream>(Kakshya::VideoStreamSpec {
            .width = 2, .height = 2, .format = Kakshya::ImageFormat::RGBA8, .frame_rate = ring_rate, .ring_frames = 16 });
        for (uint64_t f = 0; f < 10; ++f) {
            const auto bytes = VideoSupport::frame_bytes(f, small->get_frame_byte_size());
            ASSERT_TRUE(small->append_frame(std::span<const uint8_t>(bytes)));
        }
        const Chimera chimera = builder(array).layer().from(small).lag(0.0).start();

        step();
        step();

        expect_unfed(array, 0);
    }

    TEST_F(ChimeraTest, ALaggedLayerTrailsTheWriteHeadByTheLag)
    {
        const auto array = make_array(2);
        const auto ring = make_ring(40);
        const Chimera chimera = builder(array).layer().from(ring).lag(0.0).layer().from(ring).lag(1.0).start();

        step();
        expect_frame(array, 0, 38);
        expect_frame(array, 1, 40 - reach);
    }

    TEST_F(ChimeraTest, ALaggedLayerKeepsItsDistanceWhileTheRingGrows)
    {
        const auto array = make_array(2);
        const auto ring = make_ring(40);
        const Chimera chimera = builder(array).layer().from(ring).lag(0.0).layer().from(ring).lag(1.0).start();

        step();
        grow(ring, 5);
        step();

        expect_frame(array, 0, 43);
        expect_frame(array, 1, 45 - reach);
    }

    TEST_F(ChimeraTest, ALagShorterThanTwoFramesIsRaisedToTwoFrames)
    {
        const auto array = make_array(2);
        const auto ring = make_ring(40);
        const Chimera chimera = builder(array).layer().from(ring).lag(0.0).layer().from(ring).lag(1.0 / ring_rate).start();

        step();

        expect_frame(array, 0, 38);
        expect_frame(array, 1, 38);
    }

    TEST_F(ChimeraTest, ALayerWaitsUntilTheRingHoldsThreeFrames)
    {
        const auto array = make_array(1);
        const auto ring = make_ring(2);
        const Chimera chimera = builder(array).layer().from(ring).lag(0.0).start();

        step();
        expect_unfed(array, 0);
        grow(ring, 1);
        step();

        expect_frame(array, 0, 1);
    }

    TEST_F(ChimeraTest, ALagBeyondWhatTheRingHoldsLeavesTheLayerUnfed)
    {
        const auto array = make_array(1);
        const auto ring = make_ring(40, 16);
        const Chimera chimera = builder(array).layer().from(ring).lag(5.0).start();

        step();
        step();

        expect_unfed(array, 0);
    }

    TEST_F(ChimeraTest, ALagThatFollowsATimeMapChangesWithTimeSinceTheLayerEntered)
    {
        const auto array = make_array(1);
        const auto ring = make_ring(60);
        const double switch_at = 0.5;
        const Chimera chimera = builder(array)
                                    .layer()
                                    .from(ring)
                                    .lag(Kinesis::TimeMap { .fn = [switch_at](const double& t) {
                                        return t < switch_at ? 1.0 : 0.5;
                                    } })
                                    .start();

        for (int i = 0; i < static_cast<int>(fps * switch_at) + 6; ++i) {
            const uint32_t cycle = step();
            const bool early = static_cast<double>(cycle) / fps < switch_at;
            expect_frame(array, 0, early ? 60 - reach : 60 - reach / 2);
        }
    }

    TEST_F(ChimeraTest, SpeedHasNoEffectOnALaggedLayer)
    {
        const auto array = make_array(1);
        const auto ring = make_ring(40);
        Chimera chimera = builder(array).layer().from(ring).lag(0.0).start();

        chimera.speed(0, 0.0);
        step();
        grow(ring, 10);
        step();

        expect_frame(array, 0, 48);
    }

    TEST_F(ChimeraTest, ALayerRefreshesOnlyOnItsIntervalOfFrames)
    {
        const auto array = make_array(1);
        const auto ring = make_ring(20);
        const Chimera chimera = builder(array).layer().from(ring).lag(0.0).every_n_frames(3).start();
        std::vector<uint8_t> shown = blank();

        for (int i = 0; i < 9; ++i) {
            grow(ring, 1);
            const uint32_t cycle = step();
            if (cycle % 3 == 0) {
                shown = VideoSupport::frame_bytes(ring->get_write_head() - 2);
            }
            EXPECT_EQ(layer_bytes(array, 0), shown) << "cycle " << cycle;
        }
    }

    TEST_F(ChimeraTest, ALayerWithoutALagPlaysFromTheLatestFrameBackwardAtNegativeSpeed)
    {
        const auto array = make_array(1);
        const auto ring = make_ring(40);
        const Chimera chimera = builder(array).layer().from(ring).speed(-1.0).start();

        for (const uint64_t frame : { 38U, 37U, 37U, 36U, 36U }) {
            step();
            expect_frame(array, 0, frame);
        }
    }

    TEST_F(ChimeraTest, ADoubleSpeedBackwardLayerStepsOneRingFramePerFrame)
    {
        const auto array = make_array(1);
        const auto ring = make_ring(40);
        const Chimera chimera = builder(array).layer().from(ring).speed(-2.0).start();

        for (const uint64_t frame : { 38U, 37U, 36U, 35U }) {
            step();
            expect_frame(array, 0, frame);
        }
    }

    TEST_F(ChimeraTest, AForwardLayerNeverPassesTheLatestFrame)
    {
        const auto array = make_array(1);
        const auto ring = make_ring(40);
        const Chimera chimera = builder(array).layer().from(ring).speed(2.0).start();

        for (int i = 0; i < 4; ++i) {
            step();
            expect_frame(array, 0, 38);
        }
    }

    TEST_F(ChimeraTest, ZeroSpeedHoldsTheLayerOnItsFrameWhileTheRingGrows)
    {
        const auto array = make_array(1);
        const auto ring = make_ring(40);
        const Chimera chimera = builder(array).layer().from(ring).speed(0.0).start();

        step();
        grow(ring, 10);
        step();
        step();

        expect_frame(array, 0, 38);
    }

    TEST_F(ChimeraTest, ChangingTheSpeedWhilePlayingCarriesOnFromWhereTheLayerIs)
    {
        const auto array = make_array(1);
        const auto ring = make_ring(40);
        Chimera chimera = builder(array).layer().from(ring).speed(-1.0).start();

        step();
        step();
        step();
        expect_frame(array, 0, 37);
        chimera.speed(0, 0.0);
        step();
        step();
        expect_frame(array, 0, 37);
        chimera.speed(0, 2.0);
        step();

        expect_frame(array, 0, 38);
    }

    TEST_F(ChimeraTest, CuttingAFreeLayerJumpsToAMomentBehindTheHeadAndPlaysOnFromThere)
    {
        const auto array = make_array(1);
        const auto ring = make_ring(40);
        Chimera chimera = builder(array).layer().from(ring).speed(1.0).start();

        chimera.cut(0, 1.0);
        for (const uint64_t frame : { 10U, 11U, 11U, 12U }) {
            step();
            expect_frame(array, 0, frame);
        }
    }

    TEST_F(ChimeraTest, CuttingALaggedLayerMovesItsLagAndItKeepsFollowingTheHead)
    {
        const auto array = make_array(1);
        const auto ring = make_ring(40);
        Chimera chimera = builder(array).layer().from(ring).lag(0.0).start();

        step();
        expect_frame(array, 0, 38);
        chimera.cut(0, 1.0);
        step();
        expect_frame(array, 0, 40 - reach);
        grow(ring, 5);
        step();

        expect_frame(array, 0, 45 - reach);
    }

    TEST_F(ChimeraTest, CuttingToAMomentTheRingNoLongerHoldsKeepsTheLastFrame)
    {
        const auto array = make_array(1);
        const auto ring = make_ring(40, 16);
        Chimera chimera = builder(array).layer().from(ring).speed(0.0).start();

        step();
        expect_frame(array, 0, 38);
        chimera.cut(0, 2.0);
        step();
        step();
        expect_frame(array, 0, 38);
        chimera.cut(0, 0.1);
        step();

        expect_frame(array, 0, 40 - static_cast<uint64_t>(std::round(0.1 * ring_rate)));
    }

    TEST_F(ChimeraTest, CuttingAStillLayerChangesNothing)
    {
        const auto array = make_array(1);
        Chimera chimera = builder(array).layer().from(still(6)).start();

        step();
        chimera.cut(0, 1.0);
        step();

        expect_frame(array, 0, 6);
    }

    TEST_F(ChimeraTest, SmoothBlendsTheTwoFramesAroundAFractionalPosition)
    {
        const auto array = make_array(2);
        const auto ring = make_ring(40);
        Chimera chimera = builder(array).layer().from(ring).speed(1.0).smooth().layer().from(ring).speed(1.0).start();

        chimera.cut(0, 1.0);
        chimera.cut(1, 1.0);
        step();

        auto expected = VideoSupport::frame_bytes(10);
        for (auto& value : expected) {
            value = static_cast<uint8_t>(value + 5);
        }
        EXPECT_EQ(layer_bytes(array, 0), expected);
        expect_frame(array, 1, 10);
    }

    TEST_F(ChimeraTest, LayersOnTheSameRingKeepTheirOwnPositions)
    {
        const auto array = make_array(2);
        const auto ring = make_ring(40);
        Chimera chimera = builder(array).layer().from(ring).speed(-1.0).layer().from(ring).speed(0.0).start();

        step();
        step();
        step();

        expect_frame(array, 0, 37);
        expect_frame(array, 1, 38);
    }

    TEST_F(ChimeraTest, AnActionRunsOnEveryIntervalButNeverOnTheFirstCycle)
    {
        const auto array = make_array(1);
        std::vector<uint32_t> fired;
        const auto interval = static_cast<uint32_t>(std::round(0.5 * fps));
        const Chimera chimera = builder(array)
                                    .every(0.5, [this, &fired](Chimera&) { fired.push_back(cycles.back()); })
                                    .layer()
                                    .from(still(1))
                                    .start();

        run_until_cycle(interval * 2 + 4);

        std::vector<uint32_t> expected;
        for (uint32_t c = interval; c <= cycles.back(); c += interval) {
            expected.push_back(c);
        }
        EXPECT_EQ(fired, expected);
    }

    TEST_F(ChimeraTest, AnActionShorterThanAFrameRunsOnEveryCycleAfterTheFirst)
    {
        const auto array = make_array(1);
        std::vector<uint32_t> fired;
        const Chimera chimera = builder(array)
                                    .every(0.0, [this, &fired](Chimera&) { fired.push_back(cycles.back()); })
                                    .layer()
                                    .from(still(1))
                                    .start();

        run_until_cycle(5);

        EXPECT_EQ(fired, (std::vector<uint32_t> { 1, 2, 3, 4, 5 }));
    }

    TEST_F(ChimeraTest, AnActionCanCallTheControlsOfTheRunningSet)
    {
        const auto array = make_array(1);
        const auto ring = make_ring(60);
        const auto interval = static_cast<uint32_t>(std::round(0.5 * fps));
        const Chimera chimera = builder(array)
                                    .every(0.5, [](Chimera& running) { running.cut(0, 1.0); })
                                    .layer()
                                    .from(ring)
                                    .lag(0.0)
                                    .start();

        run_until_cycle(interval - 1);
        expect_frame(array, 0, 58);
        run_until_cycle(interval + 1);

        expect_frame(array, 0, 60 - reach);
    }

    TEST_F(ChimeraTest, StoppingHoldsEveryLayerOnItsLastFrameAndEndsThePipeline)
    {
        const auto array = make_array(1);
        const auto ring = make_ring(40);
        Chimera chimera = builder(array).layer().from(ring).lag(0.0).start();

        step();
        EXPECT_TRUE(pipeline()->is_running());
        chimera.stop();
        grow(ring, 10);
        frames->tick(3);

        EXPECT_FALSE(pipeline()->is_running());
        expect_frame(array, 0, 38);
    }

    TEST_F(ChimeraTest, TheSetStopsWhenItsLastCopyGoes)
    {
        const auto array = make_array(1);
        std::optional<Chimera> original = builder(array).layer().from(make_ring(40)).lag(0.0).start();
        std::optional<Chimera> copy = original;

        step();
        original.reset();
        EXPECT_TRUE(pipeline()->is_running());
        copy.reset();

        EXPECT_FALSE(pipeline()->is_running());
    }

    TEST_F(ChimeraTest, ACopyKeepsFeedingAfterTheOriginalIsGone)
    {
        const auto array = make_array(1);
        const auto ring = make_ring(40);
        std::optional<Chimera> original = builder(array).layer().from(ring).lag(0.0).start();
        const Chimera copy = *original;

        step();
        original.reset();
        grow(ring, 5);
        step();

        expect_frame(array, 0, 43);
    }

    TEST_F(ChimeraTest, LayerDataStaysOffUnlessAskedFor)
    {
        const auto array = make_array(1);
        const Chimera chimera = builder(array).layer().from(still(1)).start();

        step();

        EXPECT_FALSE(array->has_layer_data());
    }

    class ChimeraGpuTest : public VulkanDeviceTest {
    protected:
        void SetUp() override
        {
            VulkanDeviceTest::SetUp();
            if (IsSkipped()) {
                return;
            }
            frames = std::make_unique<FrameScheduler>();
            manager = std::make_shared<Buffers::BufferManager>();
            array = std::make_shared<TextureArrayBuffer>(VideoSupport::rgba8_spec(2), Portal::Graphics::FitMode::STRETCH);
        }

        void TearDown() override
        {
            for (const auto& pipeline : pipelines) {
                pipeline->end();
            }
            auto* service = Registry::BackendRegistry::instance().get_service<Registry::Service::BufferService>();
            for (const auto& buffer : buffers) {
                if (service && buffer && buffer->is_initialized()) {
                    service->destroy_buffer(buffer);
                }
            }
            buffers.clear();
            pipelines.clear();
            array.reset();
            frames.reset();
            VulkanDeviceTest::TearDown();
        }

        std::shared_ptr<Buffers::VKBuffer> make_buffer(size_t bytes)
        {
            auto buffer = manager->create_graphics_buffer<Buffers::VKBuffer>(
                ProcessingToken::GRAPHICS_BACKEND, bytes, Buffers::VKBuffer::Usage::HOST_STORAGE, Kakshya::DataModality::UNKNOWN);
            if (!buffer->is_initialized()) {
                auto* service = Registry::BackendRegistry::instance().get_service<Registry::Service::BufferService>();
                service->initialize_buffer(buffer);
            }
            buffer->get_processing_chain()->set_preferred_token(ProcessingToken::GRAPHICS_BACKEND);
            buffers.push_back(buffer);
            return buffer;
        }

        ChimeraBuilder builder()
        {
            auto pipeline = BufferPipeline::create(*frames->scheduler, manager);
            pipelines.push_back(pipeline);
            return { array, pipeline };
        }

        void tick(uint32_t count = 1)
        {
            for (uint32_t i = 0; i < count; ++i) {
                frames->tick();
                manager->process_token(ProcessingToken::GRAPHICS_BACKEND, 1);
            }
        }

        std::unique_ptr<FrameScheduler> frames;
        std::shared_ptr<Buffers::BufferManager> manager;
        std::shared_ptr<TextureArrayBuffer> array;
        std::vector<std::shared_ptr<Buffers::VKBuffer>> buffers;
        std::vector<std::shared_ptr<BufferPipeline>> pipelines;
    };

    TEST_F(ChimeraGpuTest, ParamsOnALayerEnableTheLayerData)
    {
        const Chimera chimera = builder().layer().from(still(1)).params(glm::vec4(0.1F, 0.2F, 0.3F, 0.4F)).start();

        EXPECT_TRUE(array->has_layer_data());
    }

    TEST_F(ChimeraGpuTest, AskingForLayerDataCreatesItWithoutParams)
    {
        const Chimera chimera = builder().layer_data().layer().from(still(1)).start();

        EXPECT_TRUE(array->has_layer_data());
    }

    TEST_F(ChimeraGpuTest, ParamsSetOnARunningSetEnableTheLayerData)
    {
        Chimera chimera = builder().layer().from(still(1)).start();

        EXPECT_FALSE(array->has_layer_data());
        chimera.params(0, glm::vec4(1.0F));

        EXPECT_TRUE(array->has_layer_data());
    }

    TEST_F(ChimeraGpuTest, ALayerFedFromThePipelineShowsTheFrameTheRecordingStored)
    {
        const auto frame = VideoSupport::frame_bytes(3);
        const auto source = make_buffer(frame.size());
        Buffers::upload_to_gpu(frame.data(), frame.size(), source);
        const auto video = std::make_shared<DynamicVideoStream>(VideoSupport::rgba8_spec(8));

        const Chimera chimera = builder()
                                    .record(BufferOperation::capture_from(source))
                                    .record(BufferOperation::route_to_container(video))
                                    .layer()
                                    .from_pipeline()
                                    .lag(0.0)
                                    .start();
        tick(60);

        ASSERT_GE(video->get_write_head(), 3U);
        const auto& storage = array->get_pixel_data();
        ASSERT_GE(storage.size(), frame.size());
        EXPECT_EQ(std::vector<uint8_t>(storage.begin(), storage.begin() + static_cast<std::ptrdiff_t>(frame.size())), frame);
    }


    class ChimeraImageTest : public ::testing::Test {
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
            frames = std::make_unique<FrameScheduler>();
            manager = std::make_shared<Buffers::BufferManager>();
            array = std::make_shared<TextureArrayBuffer>(VideoSupport::rgba8_spec(3), Portal::Graphics::FitMode::STRETCH);
        }

        void TearDown() override
        {
            for (const auto& pipeline : pipelines) {
                pipeline->end();
            }
            pipelines.clear();
            array.reset();
            frames.reset();
            loom().shutdown();
        }

        static Portal::Graphics::TextureLoom& loom() { return Portal::Graphics::get_texture_manager(); }

        ChimeraBuilder builder()
        {
            auto pipeline = BufferPipeline::create(*frames->scheduler, manager);
            pipelines.push_back(pipeline);
            return { array, pipeline };
        }

        [[nodiscard]] std::shared_ptr<Core::VKImage> make_image(uint64_t seed) const
        {
            const auto pixels = VideoSupport::frame_bytes(seed);
            return loom().create_2d(VideoSupport::k_width, VideoSupport::k_height, Portal::Graphics::ImageFormat::RGBA8, pixels.data());
        }

        [[nodiscard]] std::vector<uint8_t> layer_bytes(uint32_t layer) const
        {
            std::vector<uint8_t> all(k_frame_bytes * array->get_layer_count());
            loom().download_data(array->get_texture(), all.data(), all.size());
            const auto begin = static_cast<std::ptrdiff_t>(layer * k_frame_bytes);
            return { all.begin() + begin, all.begin() + begin + static_cast<std::ptrdiff_t>(k_frame_bytes) };
        }

        std::unique_ptr<FrameScheduler> frames;
        std::shared_ptr<Buffers::BufferManager> manager;
        std::shared_ptr<TextureArrayBuffer> array;
        std::vector<std::shared_ptr<BufferPipeline>> pipelines;
        inline static std::shared_ptr<Core::VulkanBackend> s_backend;
    };

    TEST_F(ChimeraImageTest, AGpuImageLayerIsCopiedIntoItsLayerOnTheFirstFrame)
    {
        const Chimera chimera = builder().layer().from(make_image(2)).layer().from(make_image(5)).start();

        frames->tick(2);

        EXPECT_EQ(layer_bytes(0), VideoSupport::frame_bytes(2));
        EXPECT_EQ(layer_bytes(1), VideoSupport::frame_bytes(5));
        EXPECT_EQ(layer_bytes(2), blank());
    }

    TEST_F(ChimeraImageTest, SettingAGpuImageReplacesTheLayerWithTheNewImage)
    {
        Chimera chimera = builder().layer().from(make_image(2)).start();

        frames->tick(2);
        chimera.set(0, make_image(8));
        frames->tick(2);

        EXPECT_EQ(layer_bytes(0), VideoSupport::frame_bytes(8));
    }

    TEST_F(ChimeraImageTest, ACollectionFedFromABufferGivesAChimeraLayerTheNewestImageEachCycle)
    {
        const auto image = make_image(0);
        const auto source = std::make_shared<Buffers::TextureBuffer>(VideoSupport::k_width, VideoSupport::k_height, Portal::Graphics::ImageFormat::RGBA8);
        source->set_gpu_texture(image);
        const auto collection = std::make_shared<Kakshya::TextureCollection>(
            Kakshya::TextureCollectionSpec { .width = VideoSupport::k_width, .height = VideoSupport::k_height, .ring_layers = 3 });
        const Chimera chimera = builder()
                                    .record(BufferOperation::capture_to_collection(source, collection))
                                    .layer()
                                    .from(collection)
                                    .lag(0.0)
                                    .start();

        for (uint64_t n = 1; n <= 7; ++n) {
            const auto pixels = VideoSupport::frame_bytes(n);
            loom().upload_data(image, pixels.data(), pixels.size());
            const uint64_t before = collection->get_write_head();
            for (int attempt = 0; attempt < 4 && collection->get_write_head() == before; ++attempt) {
                frames->tick();
            }

            ASSERT_GT(collection->get_write_head(), before) << n;
            EXPECT_EQ(layer_bytes(0), pixels) << n;
        }
    }

    TEST_F(ChimeraImageTest, ASourceBufferWithoutATextureAppendsNothing)
    {
        const auto source = std::make_shared<Buffers::TextureBuffer>(VideoSupport::k_width, VideoSupport::k_height, Portal::Graphics::ImageFormat::RGBA8);
        const auto collection = std::make_shared<Kakshya::TextureCollection>(
            Kakshya::TextureCollectionSpec { .width = VideoSupport::k_width, .height = VideoSupport::k_height, .ring_layers = 3 });
        const Chimera chimera = builder()
                                    .record(BufferOperation::capture_to_collection(source, collection))
                                    .layer()
                                    .from(still(1))
                                    .start();

        frames->tick(4);

        EXPECT_EQ(collection->get_write_head(), 0U);
    }

    TEST_F(ChimeraImageTest, AGpuImageLayerFollowsTheImageItsSourceIsRewrittenWith)
    {
        const auto source = make_image(2);
        const Chimera chimera = builder().layer().from(source).start();

        frames->tick(2);
        const auto next = VideoSupport::frame_bytes(6);
        loom().upload_data(source, next.data(), next.size());
        frames->tick(2);

        EXPECT_EQ(layer_bytes(0), next);
    }

} // namespace
} // namespace MayaFlux::Test
