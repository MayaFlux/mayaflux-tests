#include "gtest/gtest.h"
#include "../../support/test_metadata.hpp"

#include "../../support/vision_test_support.hpp"

#include "MayaFlux/Kakshya/Processors/VisionProcessor.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))

namespace MayaFlux::Test::Vision {

namespace KV = MayaFlux::Kinesis::Vision;

class VisionProcessorFormatTest : public VisionGpuFixture, public ::testing::WithParamInterface<Kakshya::ImageFormat> { };

INSTANTIATE_TEST_SUITE_P(SixteenBit, VisionProcessorFormatTest,
    ::testing::Values(Kakshya::ImageFormat::R16, Kakshya::ImageFormat::DEPTH16), sixteen_bit_name);

TEST_P(VisionProcessorFormatTest, CameraFrameIsUploadedInItsOwnFormat)
{
    if (!host_uploadable(GetParam()))
        GTEST_SKIP() << "This device has no host image copy for the format";

    constexpr uint32_t size = 32;
    const auto camera = depth_camera(size, GetParam());
    ASSERT_TRUE(camera);

    const auto sequence = KV::VisionSequence::Builder {}
                              .threshold(depth_band(900.0F, 1100.0F))
                              .connected_components({ .export_boxes = true })
                              .build();

    Kakshya::VisionProcessor processor(sequence);
    processor.on_attach(camera);
    processor.process(camera);

    const auto& boxes = processor.get_result().component_boxes;
    ASSERT_EQ(boxes.size(), 1U);

    const auto fs = static_cast<float>(size);
    EXPECT_NEAR(boxes.front().x * fs, 8.0F, 1.0F);
    EXPECT_NEAR(boxes.front().y * fs, 8.0F, 1.0F);
    EXPECT_NEAR(boxes.front().w * fs, 8.0F, 1.0F);
    EXPECT_NEAR(boxes.front().h * fs, 8.0F, 1.0F);
}

} // namespace MayaFlux::Test::Vision
