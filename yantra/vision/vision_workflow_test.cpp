#include "gtest/gtest.h"

#include "../../support/vision_test_support.hpp"
#include "MayaFlux/Kakshya/Source/DynamicVideoStream.hpp"
#include "MayaFlux/Yantra/Workflows/Vision/VisionWorkflow.hpp"

namespace MayaFlux::Test::Vision {

namespace KV = MayaFlux::Kinesis::Vision;
using Yantra::Vision::VisionMatrix;
using Yantra::Vision::VisionWorkflow;
using Yantra::VisionExtractMode;
using Yantra::VisionExtractorConfig;

namespace {

using Fixture = VisionGpuFixture;

constexpr uint32_t k_size = 128;
constexpr uint32_t k_count = 4;
constexpr float k_motion_min_confidence = 0.01F;

/**
 * @brief Pixels of one layer with any of R, G or B above zero. Edge output
 *        keeps the source colour, and the motion mask draws green, so a
 *        single channel would miss one of them.
 */
uint32_t lit_pixels(const Kakshya::TextureContainer& c, uint32_t layer)
{
    const auto bytes = c.pixel_bytes(layer);
    uint32_t lit = 0;
    for (size_t i = 0; i + 3 < bytes.size(); i += 4) {
        if (bytes[i] > 0 || bytes[i + 1] > 0 || bytes[i + 2] > 0)
            ++lit;
    }
    return lit;
}

uint64_t lit_total(const Kakshya::TextureContainer& c)
{
    uint64_t total = 0;
    for (uint32_t i = 0; i < c.get_layer_count(); ++i)
        total += lit_pixels(c, i);
    return total;
}

bool all_layers_lit(const Kakshya::TextureContainer& c)
{
    if (c.get_layer_count() == 0)
        return false;
    for (uint32_t i = 0; i < c.get_layer_count(); ++i) {
        if (lit_pixels(c, i) == 0)
            return false;
    }
    return true;
}

/** @brief @p count wave frames, each shifted @p step px further in x than the last. */
std::vector<ImageHandle> wave_frames(uint32_t count, uint32_t size, float step = 2.0F)
{
    std::vector<ImageHandle> frames;
    frames.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
        frames.push_back(wave_frame(size, static_cast<float>(i) * step, 0.0F));
    return frames;
}

/**
 * @brief Binary pseudo-random texture, sampled at whole-pixel offsets so a
 *        shift of dx moves it exactly. Its steep gradients give the dense
 *        flow solver a structure tensor large enough for the mask's
 *        min_confidence gate; the smooth wave frames gave confidences far
 *        below it.
 */
ImageHandle noise_frame(uint32_t size, int32_t dx)
{
    return pattern_frame(size, [dx](uint32_t x, uint32_t y) {
        const int32_t xs = static_cast<int32_t>(x) - dx;
        const auto ys = static_cast<int32_t>(y);
        uint32_t h = static_cast<uint32_t>(xs) * 73856093U ^ static_cast<uint32_t>(ys) * 19349663U;
        h ^= h >> 13;
        h *= 0x5bd1e995U;
        h ^= h >> 15;
        const uint32_t parity = static_cast<uint32_t>((xs + ys) & 1);
        const float v = (parity ^ (h & 1U)) != 0U ? 1.0F : 0.0F;
        return glm::vec4 { v, v, v, 1.0F };
    });
}

/** @brief @p count copies of the noise texture, so no motion between them. */
std::vector<ImageHandle> still_frames(uint32_t count, uint32_t size)
{
    std::vector<ImageHandle> frames;
    frames.reserve(count);
    const auto frame = noise_frame(size, 0);
    for (uint32_t i = 0; i < count; ++i)
        frames.push_back(frame);
    return frames;
}

/** @brief @p count noise frames, each shifted @p step whole pixels further in x. */
std::vector<ImageHandle> moving_noise_frames(uint32_t count, uint32_t size, int32_t step)
{
    std::vector<ImageHandle> frames;
    frames.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
        frames.push_back(noise_frame(size, static_cast<int32_t>(i) * step));
    return frames;
}

std::shared_ptr<Kakshya::TextureContainer> stack_of(const std::vector<ImageHandle>& frames, uint32_t size)
{
    auto stacked = std::make_shared<Kakshya::TextureContainer>(size, size, Portal::Graphics::ImageFormat::RGBA32F, static_cast<uint32_t>(frames.size()));
    for (uint32_t i = 0; i < frames.size(); ++i)
        stacked->from_image(frames[i], i);
    return stacked;
}

KV::VisionQuery edge_query()
{
    return { .intents = KV::VisionIntent::DetectEdges };
}

KV::VisionQuery motion_query()
{
    return { .intents = KV::VisionIntent::EstimateMotion };
}

/** @brief Lit pixels of a standalone image, read back as RGBA8. */
uint32_t lit_in_image(const ImageHandle& image)
{
    Kakshya::TextureContainer readback(image, Portal::Graphics::ImageFormat::RGBA8);
    return lit_pixels(readback, 0);
}

/**
 * @brief Lit pixels of each motion-mask image, recorded frame by frame on one
 *        matrix. The motion mask's min_confidence is set to
 *        k_motion_min_confidence: the dense solver reports confidences around
 *        0.08 on these frames, below the 0.1 default gate, so the default
 *        drops every pixel regardless of the flow. Frames with no flow yet
 *        record nothing, so the result is one entry per frame after the first.
 */
std::vector<uint32_t> motion_lit_counts(const std::vector<ImageHandle>& frames)
{
    const auto matrix = VisionMatrix::create(motion_query(), VisionExtractMode::MotionMask);
    edit_config<VisionExtractorConfig>(*matrix->extractor(), [](auto& c) {
        c.min_confidence = k_motion_min_confidence;
    });

    std::vector<uint32_t> counts;
    for (const auto& frame : frames) {
        matrix->extract(matrix->analyze(frame), frame);
        if (matrix->extraction())
            counts.push_back(lit_in_image(matrix->extraction()));
    }
    return counts;
}

} // namespace

class VisionWorkflowTest : public Fixture { };

// ---------------------------------------------------------------------------
// Image list
// ---------------------------------------------------------------------------

TEST_F(VisionWorkflowTest, ImageListBecomesOneLitLayerPerFrame)
{
    const auto frames = wave_frames(k_count, k_size);
    for (const auto& frame : frames)
        ASSERT_TRUE(frame);

    const auto collection = VisionWorkflow::process_to_container(frames, edge_query(), VisionExtractMode::Edges);
    ASSERT_TRUE(collection);
    EXPECT_EQ(collection->get_layer_count(), k_count);
    EXPECT_EQ(collection->get_width(), k_size);
    EXPECT_EQ(collection->get_height(), k_size);
    EXPECT_EQ(collection->get_write_head(), k_count);
    EXPECT_TRUE(all_layers_lit(*collection));
}

TEST_F(VisionWorkflowTest, NoFramesGiveNoCollection)
{
    EXPECT_FALSE(VisionWorkflow::process_to_container(std::vector<ImageHandle> {}, edge_query(), VisionExtractMode::Edges));
}

TEST_F(VisionWorkflowTest, AllNullFramesGiveNoCollection)
{
    const auto frames = std::vector<ImageHandle> { nullptr, nullptr };
    EXPECT_FALSE(VisionWorkflow::process_to_container(frames, edge_query(), VisionExtractMode::Edges));
}

TEST_F(VisionWorkflowTest, NullFramesAddNoLayer)
{
    const auto frames = std::vector<ImageHandle> {
        wave_frame(k_size, 0.0F, 0.0F),
        nullptr,
        wave_frame(k_size, 2.0F, 0.0F),
    };

    const auto collection = VisionWorkflow::process_to_container(frames, edge_query(), VisionExtractMode::Edges);
    ASSERT_TRUE(collection);
    EXPECT_EQ(collection->get_layer_count(), 2U);
    EXPECT_TRUE(all_layers_lit(*collection));
}

TEST_F(VisionWorkflowTest, SizeComesFromTheFirstNonNullFrameWhenSpecIsZero)
{
    const auto frames = std::vector<ImageHandle> { nullptr, wave_frame(k_size, 0.0F, 0.0F) };

    const auto collection = VisionWorkflow::process_to_container(frames, edge_query(), VisionExtractMode::Edges);
    ASSERT_TRUE(collection);
    EXPECT_EQ(collection->get_spec().width, k_size);
    EXPECT_EQ(collection->get_spec().height, k_size);
    EXPECT_EQ(collection->get_layer_count(), 1U);
}

TEST_F(VisionWorkflowTest, ExplicitSpecSizeScalesEachFrameIntoTheLayers)
{
    constexpr uint32_t small = 64;
    const auto frames = wave_frames(k_count, k_size);
    for (const auto& frame : frames)
        ASSERT_TRUE(frame);

    Kakshya::TextureCollectionSpec spec;
    spec.width = small;
    spec.height = small;

    const auto collection = VisionWorkflow::process_to_container(frames, edge_query(), VisionExtractMode::Edges, spec);
    ASSERT_TRUE(collection);
    EXPECT_EQ(collection->get_width(), small);
    EXPECT_EQ(collection->get_height(), small);
    EXPECT_EQ(collection->get_layer_count(), k_count);
    EXPECT_TRUE(all_layers_lit(*collection));
}

TEST_F(VisionWorkflowTest, MixedFrameSizesAreFittedIntoTheFirstFramesSize)
{
    const auto frames = std::vector<ImageHandle> {
        wave_frame(k_size, 0.0F, 0.0F),
        wave_frame(k_size / 2, 0.0F, 0.0F),
    };
    ASSERT_TRUE(frames[0] && frames[1]);

    const auto collection = VisionWorkflow::process_to_container(frames, edge_query(), VisionExtractMode::Edges);
    ASSERT_TRUE(collection);
    EXPECT_EQ(collection->get_width(), k_size);
    EXPECT_EQ(collection->get_layer_count(), 2U);
    EXPECT_TRUE(all_layers_lit(*collection));
}

TEST_F(VisionWorkflowTest, ByteCapDoesNotStopRecording)
{
    const auto frames = wave_frames(k_count, k_size);
    for (const auto& frame : frames)
        ASSERT_TRUE(frame);

    Kakshya::TextureCollectionSpec spec;
    spec.max_bytes = 1;

    const auto collection = VisionWorkflow::process_to_container(frames, edge_query(), VisionExtractMode::Edges, spec);
    ASSERT_TRUE(collection);
    EXPECT_EQ(collection->get_layer_count(), k_count);
    EXPECT_EQ(collection->get_write_head(), k_count);
}

TEST_F(VisionWorkflowTest, RingDoesNotLimitRecording)
{
    const auto frames = wave_frames(k_count, k_size);
    for (const auto& frame : frames)
        ASSERT_TRUE(frame);

    Kakshya::TextureCollectionSpec spec;
    spec.ring_layers = 2;

    const auto collection = VisionWorkflow::process_to_container(frames, edge_query(), VisionExtractMode::Edges, spec);
    ASSERT_TRUE(collection);
    EXPECT_EQ(collection->get_layer_count(), k_count);
    EXPECT_EQ(collection->get_write_head(), k_count);
}

// ---------------------------------------------------------------------------
// Texture container
// ---------------------------------------------------------------------------

TEST_F(VisionWorkflowTest, ContainerLayersAreEachRecorded)
{
    const auto frames = wave_frames(k_count, k_size);
    for (const auto& frame : frames)
        ASSERT_TRUE(frame);

    const auto collection = VisionWorkflow::process_to_container(stack_of(frames, k_size), edge_query(), VisionExtractMode::Edges);
    ASSERT_TRUE(collection);
    EXPECT_EQ(collection->get_layer_count(), k_count);
    EXPECT_TRUE(all_layers_lit(*collection));
}

TEST_F(VisionWorkflowTest, ContainerSizeIsTakenWhenSpecIsZero)
{
    const auto frames = wave_frames(k_count, k_size);
    for (const auto& frame : frames)
        ASSERT_TRUE(frame);

    const auto collection = VisionWorkflow::process_to_container(stack_of(frames, k_size), edge_query(), VisionExtractMode::Edges);
    ASSERT_TRUE(collection);
    EXPECT_EQ(collection->get_spec().width, k_size);
    EXPECT_EQ(collection->get_spec().height, k_size);
}

TEST_F(VisionWorkflowTest, NullContainerGivesNoCollection)
{
    const std::shared_ptr<Kakshya::TextureContainer> none;
    EXPECT_FALSE(VisionWorkflow::process_to_container(none, edge_query(), VisionExtractMode::Edges));
}

TEST_F(VisionWorkflowTest, CollectionIsAcceptedAsItsOwnInput)
{
    const auto frames = wave_frames(k_count, k_size);
    for (const auto& frame : frames)
        ASSERT_TRUE(frame);

    const auto first = VisionWorkflow::process_to_container(frames, edge_query(), VisionExtractMode::Edges);
    ASSERT_TRUE(first);

    const auto second = VisionWorkflow::process_to_container(first, edge_query(), VisionExtractMode::Edges);
    ASSERT_TRUE(second);
    EXPECT_EQ(second->get_layer_count(), k_count);
    EXPECT_TRUE(all_layers_lit(*second));
}

// ---------------------------------------------------------------------------
// Extraction and query flow
// ---------------------------------------------------------------------------

TEST_F(VisionWorkflowTest, ExtractionWithoutAnImageAddsNoLayer)
{
    const auto frames = wave_frames(k_count, k_size);
    for (const auto& frame : frames)
        ASSERT_TRUE(frame);

    const auto collection = VisionWorkflow::process_to_container(frames, edge_query(), VisionExtractMode::Sample);
    ASSERT_TRUE(collection);
    EXPECT_EQ(collection->get_layer_count(), 0U);
}

TEST_F(VisionWorkflowTest, EdgesWithoutTheDetectEdgesIntentAddsNoLayer)
{
    const auto frames = wave_frames(k_count, k_size);
    for (const auto& frame : frames)
        ASSERT_TRUE(frame);

    const auto collection = VisionWorkflow::process_to_container(frames, motion_query(), VisionExtractMode::Edges);
    ASSERT_TRUE(collection);
    EXPECT_EQ(collection->get_layer_count(), 0U);
}

TEST_F(VisionWorkflowTest, QueryWithoutIntentsRecordsNothing)
{
    const auto frames = wave_frames(k_count, k_size);
    for (const auto& frame : frames)
        ASSERT_TRUE(frame);

    const auto collection = VisionWorkflow::process_to_container(frames, KV::VisionQuery {}, VisionExtractMode::Edges);
    ASSERT_TRUE(collection);
    EXPECT_EQ(collection->get_layer_count(), 0U);
}

TEST_F(VisionWorkflowTest, MotionMaskLitsOnlyWhereTheFramesMove)
{
    // The first frame of a run has no flow, so it records nothing.
    constexpr size_t expected_layers = k_count - 1;
    constexpr int32_t step = 6;

    const auto still = motion_lit_counts(still_frames(k_count, k_size));
    const auto moving = motion_lit_counts(moving_noise_frames(k_count, k_size, step));

    ASSERT_EQ(still.size(), expected_layers);
    ASSERT_EQ(moving.size(), expected_layers);

    const auto total = [](const std::vector<uint32_t>& counts) {
        uint64_t sum = 0;
        for (const auto c : counts)
            sum += c;
        return sum;
    };

    EXPECT_EQ(total(still), 0U);
    EXPECT_GT(total(moving), 0U);
    EXPECT_GT(moving.back(), 0U);
}

TEST_F(VisionWorkflowTest, LiveMethodsWithoutAMatrixGiveNull)
{
    VisionWorkflow workflow(nullptr, nullptr, nullptr, nullptr);

    EXPECT_FALSE(workflow.process_to_buffer(std::shared_ptr<VisionMatrix> {}, nullptr, Portal::Graphics::RenderConfig {}));
    EXPECT_FALSE(workflow.process_to_live_container(std::shared_ptr<VisionMatrix> {}, nullptr));
}

TEST_F(VisionWorkflowTest, LiveMethodsWithoutASourceGiveNull)
{
    VisionWorkflow workflow(nullptr, nullptr, nullptr, nullptr);
    const auto matrix = VisionMatrix::create(edge_query(), VisionExtractMode::Edges);

    EXPECT_FALSE(workflow.process_to_buffer(matrix, nullptr, Portal::Graphics::RenderConfig {}));
    EXPECT_FALSE(workflow.process_to_live_container(matrix, nullptr));
}

TEST_F(VisionWorkflowTest, LiveMethodsWithoutEngineManagersGiveNull)
{
    VisionWorkflow workflow(nullptr, nullptr, nullptr, nullptr);
    const auto matrix = VisionMatrix::create(edge_query(), VisionExtractMode::Edges);
    const auto source = std::make_shared<Kakshya::DynamicVideoStream>(Kakshya::VideoStreamSpec {
        .width = k_size,
        .height = k_size,
        .format = Kakshya::ImageFormat::RGBA8,
        .frame_rate = 30.0,
        .ring_frames = 0,
    });

    EXPECT_FALSE(workflow.process_to_buffer(matrix, source, Portal::Graphics::RenderConfig {}));
    EXPECT_FALSE(workflow.process_to_live_container(matrix, source));
}

TEST_F(VisionWorkflowTest, StopAllWithNoRunsIsRepeatable)
{
    VisionWorkflow workflow(nullptr, nullptr, nullptr, nullptr);
    workflow.stop_all();
    workflow.stop_all();
    SUCCEED();
}

} // namespace MayaFlux::Test::Vision
