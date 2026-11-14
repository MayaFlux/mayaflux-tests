#include "gtest/gtest.h"
#include "../../support/test_metadata.hpp"

#include "../../support/vision_test_support.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))

namespace MayaFlux::Test::Vision {

using Kinesis::Vision::BoundingBox;
using Kinesis::Vision::CannyParams;
using Kinesis::Vision::VisionOp;
using Kinesis::Vision::VisionSequence;
using Yantra::VisionGpuExecutor;

class VisionGpuAliasTest : public VisionGpuFixture { };

TEST_F(VisionGpuAliasTest, SlotsHoldTheLastWriterWhenOpsShareOneImage)
{
    constexpr uint32_t size = 64;
    const auto frame = wave_frame(size, 0.0F, 0.0F);
    ASSERT_NE(frame, nullptr);

    const auto sequence = VisionSequence::Builder {}
                              .rgba_to_gray()
                              .gaussian_blur({ .sigma = 1.5F })
                              .sobel()
                              .threshold({ .value = 0.2F })
                              .build();
    VisionGpuExecutor executor;
    const auto result = executor.run(sequence, frame, size, size);

    ASSERT_NE(result.gray, nullptr);
    ASSERT_NE(result.images.gaussian_blur, nullptr);
    ASSERT_NE(result.images.sobel, nullptr);
    ASSERT_NE(result.images.threshold, nullptr);
    EXPECT_EQ(result.gray, result.images.sobel);
    EXPECT_EQ(result.images.gaussian_blur, result.images.threshold);

    const auto gray_sequence = VisionSequence::Builder {}.rgba_to_gray().build();
    VisionGpuExecutor fresh;
    const auto reference = fresh.run(gray_sequence, frame, size, size);
    ASSERT_NE(reference.gray, nullptr);
    EXPECT_GT(max_abs_diff(read_floats(result.gray), read_floats(reference.gray)), k_alias_tolerance);
}

TEST_F(VisionGpuAliasTest, DownsampledRunKeepsFullSizeGrayApartFromTheHalfSizeOutputs)
{
    constexpr uint32_t size = 64;
    const auto frame = wave_frame(size, 0.0F, 0.0F);
    ASSERT_NE(frame, nullptr);

    const VisionSequence downsample_sequence {
        .steps = {
            { .op = VisionOp::RgbaToGray },
            { .op = VisionOp::Downsample2x },
            { .op = VisionOp::Sobel },
        },
    };
    VisionGpuExecutor executor;
    const auto downsampled = executor.run(downsample_sequence, frame, size, size);

    ASSERT_NE(downsampled.images.sobel, nullptr);
    EXPECT_EQ(downsampled.images.sobel->get_width(), size / 2);
    ASSERT_NE(downsampled.gray, nullptr);
    ASSERT_NE(downsampled.images.downsample_2x, nullptr);
    EXPECT_NE(downsampled.gray, downsampled.images.downsample_2x);
    EXPECT_NE(downsampled.gray, downsampled.images.sobel);

    const auto gray_sequence = VisionSequence::Builder {}.rgba_to_gray().build();
    VisionGpuExecutor fresh;
    const auto reference = fresh.run(gray_sequence, frame, size, size);
    ASSERT_NE(reference.gray, nullptr);
    EXPECT_LE(max_abs_diff(read_floats(downsampled.gray), read_floats(reference.gray)), k_alias_tolerance);
}

TEST_F(VisionGpuAliasTest, ComponentColoursAndContourImageAreSeparateFromThePixelSlots)
{
    constexpr uint32_t size = 64;
    const auto blobs = solid_region_frame(size, { .x = 0.25F, .y = 0.25F, .w = 0.3F, .h = 0.3F }, glm::vec4 { 1.0F });
    ASSERT_NE(blobs, nullptr);

    const auto sequence = VisionSequence::Builder {}
                              .rgba_to_gray()
                              .threshold_otsu()
                              .connected_components({ .with_colors = true })
                              .find_contours({ .as_image = true })
                              .build();
    VisionGpuExecutor executor;
    const auto elements = executor.run(sequence, blobs, size, size);

    const auto& colors = elements.images.component_colors;
    const auto& contours = elements.images.contour_image;
    ASSERT_NE(colors, nullptr);
    ASSERT_NE(contours, nullptr);
    EXPECT_NE(colors, elements.gray);
    EXPECT_NE(contours, elements.gray);
    EXPECT_NE(colors, elements.images.threshold_otsu);
    EXPECT_NE(contours, elements.images.threshold_otsu);
    EXPECT_EQ(colors, contours);
}

TEST_F(VisionGpuAliasTest, OpenCloseAndCannyEachPopulateTheirOwnSlot)
{
    constexpr uint32_t size = 64;
    const auto frame = solid_region_frame(size, { .x = 0.25F, .y = 0.25F, .w = 0.4F, .h = 0.3F }, glm::vec4 { 1.0F });
    ASSERT_NE(frame, nullptr);

    const auto open_sequence = VisionSequence::Builder {}.rgba_to_gray().open({ .radius = 1 }).build();
    const auto close_sequence = VisionSequence::Builder {}.rgba_to_gray().close({ .radius = 1 }).build();
    const auto canny_sequence = VisionSequence::Builder {}
                                    .rgba_to_gray()
                                    .canny({ .sigma = 1.4F, .low_threshold = 0.1F, .high_threshold = 0.3F })
                                    .build();

    VisionGpuExecutor open_executor;
    const auto opened = open_executor.run(open_sequence, frame, size, size);
    EXPECT_NE(opened.images.open, nullptr);

    VisionGpuExecutor close_executor;
    const auto closed = close_executor.run(close_sequence, frame, size, size);
    EXPECT_NE(closed.images.close, nullptr);

    VisionGpuExecutor canny_executor;
    const auto edged = canny_executor.run(canny_sequence, frame, size, size);
    EXPECT_NE(edged.images.canny, nullptr);
}

TEST_F(VisionGpuAliasTest, RepeatedCannyInOneSequenceMatchesASingleCannyRunOnItsOutput)
{
    constexpr uint32_t size = 64;
    const CannyParams canny { .sigma = 1.4F, .low_threshold = 0.1F, .high_threshold = 0.3F };
    const auto frame = solid_region_frame(size, { .x = 0.25F, .y = 0.25F, .w = 0.4F, .h = 0.3F }, glm::vec4 { 1.0F });
    ASSERT_NE(frame, nullptr);

    VisionGpuExecutor once_executor;
    const auto once = once_executor.run(VisionSequence::Builder {}.rgba_to_gray().canny(canny).build(), frame, size, size);
    ASSERT_NE(once.images.canny, nullptr);
    const auto first_pixels = read_floats(once.images.canny);

    VisionGpuExecutor twice_executor;
    const auto twice = twice_executor.run(
        VisionSequence::Builder {}.rgba_to_gray().canny(canny).canny(canny).build(), frame, size, size);
    ASSERT_NE(twice.images.canny, nullptr);

    const auto first_upload = rgba_image(size, size, first_pixels);
    ASSERT_NE(first_upload, nullptr);
    VisionGpuExecutor reference_executor;
    const auto reference = reference_executor.run(VisionSequence::Builder {}.canny(canny).build(), first_upload, size, size);
    ASSERT_NE(reference.images.canny, nullptr);

    EXPECT_LE(max_abs_diff(read_floats(twice.images.canny), read_floats(reference.images.canny)), k_alias_tolerance);
}

TEST_F(VisionGpuAliasTest, SnapshotsCaptureTheirStepAndTheirImagesAreReusedNextRun)
{
    constexpr uint32_t size = 64;
    const auto frame = wave_frame(size, 0.0F, 0.0F);
    ASSERT_NE(frame, nullptr);

    const auto sequence = VisionSequence::Builder {}
                              .rgba_to_gray()
                              .snapshot()
                              .gaussian_blur({ .sigma = 1.5F })
                              .snapshot()
                              .sobel()
                              .threshold({ .value = 0.2F })
                              .build();
    VisionGpuExecutor executor;
    const auto result = executor.run(sequence, frame, size, size);
    ASSERT_EQ(result.snapshots.size(), 2U);
    ASSERT_NE(result.snapshots[0].image, nullptr);
    ASSERT_NE(result.snapshots[1].image, nullptr);

    const auto gray_snap = result.snapshots[0].image;
    const auto blur_snap = result.snapshots[1].image;
    EXPECT_NE(gray_snap, blur_snap);
    for (const auto& [name, handle] : named_slots(result)) {
        if (handle) {
            EXPECT_NE(handle, gray_snap) << name;
            EXPECT_NE(handle, blur_snap) << name;
        }
    }

    VisionGpuExecutor gray_executor;
    const auto gray_ref = gray_executor.run(VisionSequence::Builder {}.rgba_to_gray().build(), frame, size, size);
    ASSERT_NE(gray_ref.gray, nullptr);
    EXPECT_LE(max_abs_diff(read_floats(gray_snap), read_floats(gray_ref.gray)), k_alias_tolerance);

    VisionGpuExecutor blur_executor;
    const auto blur_ref = blur_executor.run(
        VisionSequence::Builder {}.rgba_to_gray().gaussian_blur({ .sigma = 1.5F }).build(), frame, size, size);
    ASSERT_NE(blur_ref.images.gaussian_blur, nullptr);
    EXPECT_LE(max_abs_diff(read_floats(blur_snap), read_floats(blur_ref.images.gaussian_blur)), k_alias_tolerance);

    const auto again = executor.run(sequence, frame, size, size);
    ASSERT_EQ(again.snapshots.size(), 2U);
    EXPECT_EQ(again.snapshots[0].image, gray_snap);
    EXPECT_EQ(again.snapshots[1].image, blur_snap);
}

TEST_F(VisionGpuAliasTest, ASecondRunOverwritesTheFirstRunsSlotImage)
{
    constexpr uint32_t size = 64;
    const auto frame = wave_frame(size, 0.0F, 0.0F);
    ASSERT_NE(frame, nullptr);

    const auto sobel_sequence = VisionSequence::Builder {}.rgba_to_gray().sobel().build();
    const auto blur_sequence = VisionSequence::Builder {}.rgba_to_gray().gaussian_blur({ .sigma = 1.5F }).build();
    VisionGpuExecutor executor;
    const auto first = executor.run(sobel_sequence, frame, size, size);
    ASSERT_NE(first.images.sobel, nullptr);

    const auto before = read_floats(first.images.sobel);
    const auto second = executor.run(blur_sequence, frame, size, size);
    ASSERT_NE(second.images.gaussian_blur, nullptr);
    const auto after = read_floats(first.images.sobel);
    EXPECT_GT(max_abs_diff(before, after), k_alias_tolerance);
}

/**
 * A threshold, a confine, then the same threshold again. The second threshold
 * must produce the same pixels as a run that never saw the first threshold.
 */
TEST_F(VisionGpuAliasTest, ThresholdAfterConfineRunsItsOwnShader)
{
    constexpr uint32_t size = 64;
    const BoundingBox region { .x = 0.25F, .y = 0.25F, .w = 0.5F, .h = 0.5F };
    const auto frame = wave_frame(size, 0.0F, 0.0F);
    ASSERT_NE(frame, nullptr);

    const auto with_prior_threshold = VisionSequence::Builder {}
                                          .rgba_to_gray()
                                          .threshold({ .value = 0.5F })
                                          .confine({ .bounds = region })
                                          .threshold({ .value = 0.5F })
                                          .build();
    const auto without_prior_threshold = VisionSequence::Builder {}
                                             .rgba_to_gray()
                                             .confine({ .bounds = region })
                                             .threshold({ .value = 0.5F })
                                             .build();

    VisionGpuExecutor executor;
    const auto actual = executor.run(with_prior_threshold, frame, size, size);
    ASSERT_NE(actual.images.threshold, nullptr);

    VisionGpuExecutor reference_executor;
    const auto expected = reference_executor.run(without_prior_threshold, frame, size, size);
    ASSERT_NE(expected.images.threshold, nullptr);

    ASSERT_EQ(actual.images.threshold->get_width(), expected.images.threshold->get_width());
    ASSERT_EQ(actual.images.threshold->get_height(), expected.images.threshold->get_height());
    EXPECT_LE(max_abs_diff(read_floats(actual.images.threshold), read_floats(expected.images.threshold)),
        k_alias_tolerance);
}

/**
 * A gaussian blur followed by sobel. Sobel declares no push constant range,
 * so the blur's push bytes must not reach its dispatch. A validation message
 * during the run means they did.
 */
TEST_F(VisionGpuAliasTest, PushDataDoesNotCarryIntoAShaderWithNoPushRange)
{
    constexpr uint32_t size = 64;
    const auto frame = wave_frame(size, 0.0F, 0.0F);
    ASSERT_NE(frame, nullptr);

    const auto sequence = VisionSequence::Builder {}
                              .rgba_to_gray()
                              .gaussian_blur({ .sigma = 1.5F })
                              .sobel()
                              .build();

    LogCapture::clear();
    VisionGpuExecutor executor;
    const auto result = executor.run(sequence, frame, size, size);
    ASSERT_NE(result.images.sobel, nullptr);

    for (const auto& message : LogCapture::messages()) {
        EXPECT_EQ(message.find("Validation layer"), std::string::npos) << message;
    }
}

} // namespace MayaFlux::Test::Vision
