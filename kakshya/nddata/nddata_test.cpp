#include "gtest/gtest.h"

#include <string_view>

#include "MayaFlux/Kakshya/NDData/NDData.hpp"

namespace MayaFlux::Test {
namespace {

    using Kakshya::DataDimension;
    using Kakshya::DataModality;
    using Kakshya::DataVariant;
    using Kakshya::FrameView;
    using Kakshya::MemoryLayout;
    using Kakshya::OrganizationStrategy;
    using Role = Kakshya::DataDimension::Role;

    TEST(NDDataDimensionTest, TimeIsAUnitStrideTimeAxis)
    {
        const auto dim = DataDimension::time(480);

        EXPECT_EQ(dim.name, "time");
        EXPECT_EQ(dim.size, 480U);
        EXPECT_EQ(dim.stride, 1U);
        EXPECT_EQ(dim.role, Role::TIME);
        EXPECT_FALSE(dim.grouping.has_value());
        EXPECT_FALSE(dim.value_range.has_value());
    }

    TEST(NDDataDimensionTest, TimeAcceptsAName)
    {
        EXPECT_EQ(DataDimension::time(8, "frames").name, "frames");
    }

    TEST(NDDataDimensionTest, ChannelCarriesItsStride)
    {
        const auto dim = DataDimension::channel(2, 480);

        EXPECT_EQ(dim.name, "channel");
        EXPECT_EQ(dim.size, 2U);
        EXPECT_EQ(dim.stride, 480U);
        EXPECT_EQ(dim.role, Role::CHANNEL);
    }

    TEST(NDDataDimensionTest, DepthDefaultsToOneComponent)
    {
        const auto dim = DataDimension::depth();

        EXPECT_EQ(dim.name, "depth");
        EXPECT_EQ(dim.size, 1U);
        EXPECT_EQ(dim.stride, 1U);
        EXPECT_EQ(dim.role, Role::DEPTH);
    }

    TEST(NDDataDimensionTest, FrequencyIsAFrequencyAxis)
    {
        const auto dim = DataDimension::frequency(513);

        EXPECT_EQ(dim.name, "frequency");
        EXPECT_EQ(dim.size, 513U);
        EXPECT_EQ(dim.role, Role::FREQUENCY);
    }

    TEST(NDDataDimensionTest, MipLevelsAreAnIndexedAxis)
    {
        const auto dim = DataDimension::mipmap_levels(6);

        EXPECT_EQ(dim.name, "mip_levels");
        EXPECT_EQ(dim.size, 6U);
        EXPECT_EQ(dim.role, Role::MIP_LEVEL);
    }

    TEST(NDDataDimensionTest, SpatialAxisSelectsTheRole)
    {
        EXPECT_EQ(DataDimension::spatial(4, 'x').role, Role::SPATIAL_X);
        EXPECT_EQ(DataDimension::spatial(4, 'y').role, Role::SPATIAL_Y);
        EXPECT_EQ(DataDimension::spatial(4, 'z').role, Role::SPATIAL_Z);
    }

    TEST(NDDataDimensionTest, UnknownSpatialAxisFallsBackToZ)
    {
        EXPECT_EQ(DataDimension::spatial(4, 'w').role, Role::SPATIAL_Z);
    }

    TEST(NDDataDimensionTest, SpatialKeepsItsSizeStrideAndName)
    {
        const auto dim = DataDimension::spatial(16, 'y', 32, "rows");

        EXPECT_EQ(dim.name, "rows");
        EXPECT_EQ(dim.size, 16U);
        EXPECT_EQ(dim.stride, 32U);
    }

    TEST(NDDataDimensionTest, SpatialWithAnEmptyNameIsNamedAfterItsAxis)
    {
        EXPECT_EQ(DataDimension::spatial(4, 'y', 1, "").name, "spatial_y");
    }

    TEST(NDDataDimensionTest, Spatial1dIsAnXAxisOfUnitStride)
    {
        const auto dim = DataDimension::spatial_1d(64);

        EXPECT_EQ(dim.size, 64U);
        EXPECT_EQ(dim.stride, 1U);
        EXPECT_EQ(dim.role, Role::SPATIAL_X);
    }

    TEST(NDDataDimensionTest, Spatial2dRowStrideIsTheWidthInElements)
    {
        const auto dim = DataDimension::spatial_2d(64, 32);

        EXPECT_EQ(dim.size, 32U);
        EXPECT_EQ(dim.stride, 64U);
        EXPECT_EQ(dim.role, Role::SPATIAL_Y);
    }

    TEST(NDDataDimensionTest, Spatial3dSliceStrideIsWidthTimesHeightInElements)
    {
        const auto dim = DataDimension::spatial_3d(64, 32, 8);

        EXPECT_EQ(dim.size, 8U);
        EXPECT_EQ(dim.stride, 64U * 32U);
        EXPECT_EQ(dim.role, Role::SPATIAL_Z);
    }

    TEST(NDDataDimensionTest, Spatial2dStrideAgreesWithTheRowMajorStrideOfTheSameShape)
    {
        const auto dim = DataDimension::spatial_2d(64, 32);
        const auto strides = DataDimension::calculate_strides({ 32, 64 }, MemoryLayout::ROW_MAJOR);

        EXPECT_EQ(dim.stride, strides[0]);
    }

    TEST(NDDataDimensionTest, Spatial3dStrideAgreesWithTheRowMajorStrideOfTheSameShape)
    {
        const auto dim = DataDimension::spatial_3d(64, 32, 8);
        const auto strides = DataDimension::calculate_strides({ 8, 32, 64 }, MemoryLayout::ROW_MAJOR);

        EXPECT_EQ(dim.stride, strides[0]);
    }

    TEST(NDDataDimensionTest, GroupedRecordsTheComponentsPerElement)
    {
        const auto dim = DataDimension::grouped("points", 10, 3, Role::POSITION);

        EXPECT_EQ(dim.name, "points");
        EXPECT_EQ(dim.size, 10U);
        EXPECT_EQ(dim.role, Role::POSITION);
        ASSERT_TRUE(dim.grouping.has_value());
        EXPECT_EQ(dim.grouping->count, 3U);
        EXPECT_EQ(dim.grouping->offset, 0U);
    }

    struct VertexDimensionCase {
        std::string_view name;
        DataDimension dimension;
        std::string_view expected_name;
        Role role;
        uint8_t components;
    };

    class NDDataVertexDimensionTest : public ::testing::TestWithParam<VertexDimensionCase> { };

    TEST_P(NDDataVertexDimensionTest, IsGroupedWithTheExpectedRoleAndComponentCount)
    {
        const auto& dim = GetParam().dimension;

        EXPECT_EQ(dim.name, GetParam().expected_name);
        EXPECT_EQ(dim.size, 7U);
        EXPECT_EQ(dim.role, GetParam().role);
        ASSERT_TRUE(dim.grouping.has_value());
        EXPECT_EQ(dim.grouping->count, GetParam().components);
    }

    INSTANTIATE_TEST_SUITE_P(Vertices, NDDataVertexDimensionTest,
        ::testing::Values(
            VertexDimensionCase { "positions", DataDimension::vertex_positions(7), "positions", Role::POSITION, 3 },
            VertexDimensionCase { "normals", DataDimension::vertex_normals(7), "normals", Role::NORMAL, 3 },
            VertexDimensionCase { "uvs", DataDimension::texture_coords(7), "uvs", Role::UV, 2 },
            VertexDimensionCase { "rgb", DataDimension::vertex_colors(7), "colors", Role::COLOR, 3 },
            VertexDimensionCase { "rgba", DataDimension::vertex_colors(7, true), "colors", Role::COLOR, 4 }),
        [](const ::testing::TestParamInfo<VertexDimensionCase>& info) {
            return std::string(info.param.name);
        });

    TEST(NDDataDimensionTest, WithRangeStoresTheSpanAndInvalidMarker)
    {
        auto dim = DataDimension::depth();
        dim.with_range(0.0, 4000.0, 0.0);

        ASSERT_TRUE(dim.value_range.has_value());
        EXPECT_EQ(dim.value_range->min, 0.0);
        EXPECT_EQ(dim.value_range->max, 4000.0);
        ASSERT_TRUE(dim.value_range->invalid.has_value());
        EXPECT_EQ(*dim.value_range->invalid, 0.0);
    }

    TEST(NDDataDimensionTest, WithRangeWithoutAnInvalidMarkerLeavesItAbsent)
    {
        auto dim = DataDimension::depth();
        dim.with_range(-1.0, 1.0);

        ASSERT_TRUE(dim.value_range.has_value());
        EXPECT_FALSE(dim.value_range->invalid.has_value());
    }

    TEST(NDDataDimensionTest, WithRangeChains)
    {
        auto dim = DataDimension::depth();

        EXPECT_EQ(&dim.with_range(0.0, 1.0), &dim);
    }

    TEST(NDDataDimensionTest, WithRangeRejectsAnEmptyOrInvertedSpan)
    {
        auto equal = DataDimension::depth();
        equal.with_range(1.0, 1.0);
        EXPECT_FALSE(equal.value_range.has_value());

        auto inverted = DataDimension::depth();
        inverted.with_range(2.0, 1.0);
        EXPECT_FALSE(inverted.value_range.has_value());
    }

    TEST(NDDataDimensionTest, RejectedRangeKeepsAPreviouslyAppliedOne)
    {
        auto dim = DataDimension::depth();
        dim.with_range(0.0, 10.0);
        dim.with_range(5.0, 5.0);

        ASSERT_TRUE(dim.value_range.has_value());
        EXPECT_EQ(dim.value_range->max, 10.0);
    }

    TEST(NDDataStrideTest, RowMajorStridesShrinkTowardTheLastAxis)
    {
        const auto strides = DataDimension::calculate_strides({ 2, 3, 4 }, MemoryLayout::ROW_MAJOR);

        EXPECT_EQ(strides, (std::vector<uint64_t> { 12, 4, 1 }));
    }

    TEST(NDDataStrideTest, ColumnMajorStridesGrowTowardTheLastAxis)
    {
        const auto strides = DataDimension::calculate_strides({ 2, 3, 4 }, MemoryLayout::COLUMN_MAJOR);

        EXPECT_EQ(strides, (std::vector<uint64_t> { 1, 2, 6 }));
    }

    TEST(NDDataStrideTest, ASingleAxisHasUnitStrideInEitherLayout)
    {
        EXPECT_EQ(DataDimension::calculate_strides({ 5 }, MemoryLayout::ROW_MAJOR), (std::vector<uint64_t> { 1 }));
        EXPECT_EQ(DataDimension::calculate_strides({ 5 }, MemoryLayout::COLUMN_MAJOR), (std::vector<uint64_t> { 1 }));
    }

    TEST(NDDataStrideTest, AnEmptyShapeHasNoStrides)
    {
        EXPECT_TRUE(DataDimension::calculate_strides({}, MemoryLayout::ROW_MAJOR).empty());
        EXPECT_TRUE(DataDimension::calculate_strides({}, MemoryLayout::COLUMN_MAJOR).empty());
    }

    TEST(NDDataStrideTest, OuterStrideTimesOuterExtentIsTheElementCount)
    {
        const std::vector<uint64_t> shape { 3, 5, 7 };
        const auto strides = DataDimension::calculate_strides(shape, MemoryLayout::ROW_MAJOR);

        EXPECT_EQ(strides.front() * shape.front(), 3U * 5U * 7U);
    }

    TEST(NDDataCreateDimensionsTest, MultichannelAudioIsTimeThenChannel)
    {
        const auto dims = DataDimension::create_dimensions(DataModality::AUDIO_MULTICHANNEL, { 100, 2 });

        ASSERT_EQ(dims.size(), 2U);
        EXPECT_EQ(dims[0].role, Role::TIME);
        EXPECT_EQ(dims[0].size, 100U);
        EXPECT_EQ(dims[1].role, Role::CHANNEL);
        EXPECT_EQ(dims[1].size, 2U);
        EXPECT_EQ(dims[1].stride, 1U);
    }

    TEST(NDDataCreateDimensionsTest, ImageIsRowsThenColumnsWithRowMajorStrides)
    {
        const auto dims = DataDimension::create_dimensions(DataModality::IMAGE_2D, { 4, 8 });

        ASSERT_EQ(dims.size(), 2U);
        EXPECT_EQ(dims[0].role, Role::SPATIAL_Y);
        EXPECT_EQ(dims[0].size, 4U);
        EXPECT_EQ(dims[0].stride, 8U);
        EXPECT_EQ(dims[1].role, Role::SPATIAL_X);
        EXPECT_EQ(dims[1].size, 8U);
        EXPECT_EQ(dims[1].stride, 1U);
    }

    TEST(NDDataCreateDimensionsTest, ColorImageEndsInAChannelAxis)
    {
        const auto dims = DataDimension::create_dimensions(DataModality::IMAGE_COLOR, { 4, 8, 3 });

        ASSERT_EQ(dims.size(), 3U);
        EXPECT_EQ(dims[2].role, Role::CHANNEL);
        EXPECT_EQ(dims[2].size, 3U);
    }

    TEST(NDDataCreateDimensionsTest, DepthMapEndsInADepthAxis)
    {
        const auto dims = DataDimension::create_dimensions(DataModality::DEPTH_MAP, { 4, 8, 2 });

        ASSERT_EQ(dims.size(), 3U);
        EXPECT_EQ(dims[2].role, Role::DEPTH);
        EXPECT_EQ(dims[2].size, 2U);
    }

    TEST(NDDataCreateDimensionsTest, SpectralDataIsTimeWindowsThenFrequency)
    {
        const auto dims = DataDimension::create_dimensions(DataModality::SPECTRAL_2D, { 10, 513 });

        ASSERT_EQ(dims.size(), 2U);
        EXPECT_EQ(dims[0].name, "time_windows");
        EXPECT_EQ(dims[0].role, Role::TIME);
        EXPECT_EQ(dims[1].role, Role::FREQUENCY);
        EXPECT_EQ(dims[1].stride, 1U);
    }

    TEST(NDDataCreateDimensionsTest, VolumeIsXYZ)
    {
        const auto dims = DataDimension::create_dimensions(DataModality::VOLUMETRIC_3D, { 2, 3, 4 });

        ASSERT_EQ(dims.size(), 3U);
        EXPECT_EQ(dims[0].role, Role::SPATIAL_X);
        EXPECT_EQ(dims[1].role, Role::SPATIAL_Y);
        EXPECT_EQ(dims[2].role, Role::SPATIAL_Z);
    }

    TEST(NDDataCreateDimensionsTest, ColorVideoIsFramesRowsColumnsChannels)
    {
        const auto dims = DataDimension::create_dimensions(DataModality::VIDEO_COLOR, { 5, 4, 8, 3 });

        ASSERT_EQ(dims.size(), 4U);
        EXPECT_EQ(dims[0].name, "frames");
        EXPECT_EQ(dims[0].role, Role::TIME);
        EXPECT_EQ(dims[1].role, Role::SPATIAL_Y);
        EXPECT_EQ(dims[2].role, Role::SPATIAL_X);
        EXPECT_EQ(dims[3].role, Role::CHANNEL);
    }

    TEST(NDDataCreateDimensionsTest, MonoAudioIsASingleTimeAxis)
    {
        const auto dims = DataDimension::create_dimensions(DataModality::AUDIO_1D, { 256 });

        ASSERT_EQ(dims.size(), 1U);
        EXPECT_EQ(dims[0].name, "time");
        EXPECT_EQ(dims[0].role, Role::TIME);
        EXPECT_EQ(dims[0].size, 256U);
    }

    TEST(NDDataCreateDimensionsTest, ColorImageArrayIsIndexRowsColumnsChannels)
    {
        const auto dims = DataDimension::create_dimensions(DataModality::IMAGE_COLOR_ARRAY, { 6, 4, 8, 3 });

        ASSERT_EQ(dims.size(), 4U);
        EXPECT_EQ(dims[0].name, "index");
        EXPECT_EQ(dims[0].role, Role::TIME);
        EXPECT_EQ(dims[0].size, 6U);
        EXPECT_EQ(dims[1].role, Role::SPATIAL_Y);
        EXPECT_EQ(dims[2].role, Role::SPATIAL_X);
        EXPECT_EQ(dims[3].role, Role::CHANNEL);
        EXPECT_EQ(dims[3].size, 3U);
    }

    TEST(NDDataCreateDimensionsTest, GrayscaleVideoIsFramesRowsColumns)
    {
        const auto dims = DataDimension::create_dimensions(DataModality::VIDEO_GRAYSCALE, { 5, 4, 8 });

        ASSERT_EQ(dims.size(), 3U);
        EXPECT_EQ(dims[0].name, "frames");
        EXPECT_EQ(dims[0].role, Role::TIME);
        EXPECT_EQ(dims[1].role, Role::SPATIAL_Y);
        EXPECT_EQ(dims[1].stride, 8U);
        EXPECT_EQ(dims[2].role, Role::SPATIAL_X);
        EXPECT_EQ(dims[2].stride, 1U);
    }

    TEST(NDDataCreateDimensionsTest, DepthVideoIsFramesRowsColumnsDepth)
    {
        const auto dims = DataDimension::create_dimensions(DataModality::VIDEO_DEPTH, { 5, 4, 8, 2 });

        ASSERT_EQ(dims.size(), 4U);
        EXPECT_EQ(dims[0].name, "frames");
        EXPECT_EQ(dims[0].role, Role::TIME);
        EXPECT_EQ(dims[3].role, Role::DEPTH);
        EXPECT_EQ(dims[3].size, 2U);
        EXPECT_EQ(dims[3].stride, 1U);
    }

    TEST(NDDataCreateDimensionsTest, StridesFollowTheRequestedLayout)
    {
        const auto row = DataDimension::create_dimensions(DataModality::IMAGE_COLOR, { 4, 8, 3 }, MemoryLayout::ROW_MAJOR);
        const auto column = DataDimension::create_dimensions(DataModality::IMAGE_COLOR, { 4, 8, 3 }, MemoryLayout::COLUMN_MAJOR);

        EXPECT_EQ(row[0].stride, 24U);
        EXPECT_EQ(row[1].stride, 3U);
        EXPECT_EQ(row[2].stride, 1U);
        EXPECT_EQ(column[0].stride, 1U);
        EXPECT_EQ(column[1].stride, 4U);
        EXPECT_EQ(column[2].stride, 32U);
    }

    TEST(NDDataCreateDimensionsTest, VolumeStridesAgreeWithCalculateStrides)
    {
        const std::vector<uint64_t> shape { 2, 3, 4 };
        const auto dims = DataDimension::create_dimensions(DataModality::VOLUMETRIC_3D, shape);
        const auto strides = DataDimension::calculate_strides(shape, MemoryLayout::ROW_MAJOR);

        ASSERT_EQ(dims.size(), strides.size());
        for (size_t i = 0; i < dims.size(); ++i) {
            EXPECT_EQ(dims[i].stride, strides[i]) << i;
            EXPECT_EQ(dims[i].size, shape[i]) << i;
        }
    }

    struct RankCase {
        std::string_view name;
        DataModality modality;
        std::vector<uint64_t> wrong_shape;
    };

    class NDDataRankMismatchTest : public ::testing::TestWithParam<RankCase> { };

    TEST_P(NDDataRankMismatchTest, ThrowsInvalidArgument)
    {
        EXPECT_THROW(
            DataDimension::create_dimensions(GetParam().modality, GetParam().wrong_shape),
            std::invalid_argument);
    }

    INSTANTIATE_TEST_SUITE_P(Modalities, NDDataRankMismatchTest,
        ::testing::Values(
            RankCase { "AUDIO_1D", DataModality::AUDIO_1D, { 4, 2 } },
            RankCase { "AUDIO_MULTICHANNEL", DataModality::AUDIO_MULTICHANNEL, { 4 } },
            RankCase { "IMAGE_2D", DataModality::IMAGE_2D, { 4 } },
            RankCase { "IMAGE_COLOR", DataModality::IMAGE_COLOR, { 4, 4 } },
            RankCase { "IMAGE_COLOR_ARRAY", DataModality::IMAGE_COLOR_ARRAY, { 4, 4, 4 } },
            RankCase { "DEPTH_MAP", DataModality::DEPTH_MAP, { 4, 4 } },
            RankCase { "SPECTRAL_2D", DataModality::SPECTRAL_2D, { 4 } },
            RankCase { "VOLUMETRIC_3D", DataModality::VOLUMETRIC_3D, { 4, 4 } },
            RankCase { "VIDEO_GRAYSCALE", DataModality::VIDEO_GRAYSCALE, { 4, 4 } },
            RankCase { "VIDEO_COLOR", DataModality::VIDEO_COLOR, { 4, 4, 4 } },
            RankCase { "VIDEO_DEPTH", DataModality::VIDEO_DEPTH, { 4, 4, 4 } }),
        [](const ::testing::TestParamInfo<RankCase>& info) {
            return std::string(info.param.name);
        });

    TEST(NDDataCreateDimensionsTest, ModalitiesWithoutADimensionLayoutThrow)
    {
        EXPECT_THROW(DataDimension::create_dimensions(DataModality::TENSOR_ND, { 2, 2 }), std::invalid_argument);
        EXPECT_THROW(DataDimension::create_dimensions(DataModality::UNKNOWN, { 2 }), std::invalid_argument);
    }

    TEST(NDDataCreateModuleTest, MultichannelAudioIsPlanarByDefault)
    {
        const auto [variants, dims] = DataDimension::create_for_modality<float>(
            DataModality::AUDIO_MULTICHANNEL, { 10, 3 }, 0.25F);

        ASSERT_EQ(variants.size(), 3U);
        EXPECT_EQ(dims.size(), 2U);
        for (const auto& variant : variants) {
            const auto* channel = std::get_if<std::vector<float>>(&variant);
            ASSERT_NE(channel, nullptr);
            ASSERT_EQ(channel->size(), 10U);
            EXPECT_TRUE(std::ranges::all_of(*channel, [](float v) { return v == 0.25F; }));
        }
    }

    TEST(NDDataCreateModuleTest, InterleavedOrganizationUsesOneVariantForEverything)
    {
        const auto [variants, dims] = DataDimension::create_for_modality<float>(
            DataModality::AUDIO_MULTICHANNEL, { 10, 3 }, 0.0F,
            MemoryLayout::ROW_MAJOR, OrganizationStrategy::INTERLEAVED);

        ASSERT_EQ(variants.size(), 1U);
        EXPECT_EQ(std::get<std::vector<float>>(variants[0]).size(), 30U);
    }

    TEST(NDDataCreateModuleTest, ColorImageHasOnePlanePerChannel)
    {
        const auto variants = DataDimension::create_for_modality<uint8_t>(
            DataModality::IMAGE_COLOR, { 2, 3, 4 }).first;

        ASSERT_EQ(variants.size(), 4U);
        for (const auto& variant : variants) {
            EXPECT_EQ(std::get<std::vector<uint8_t>>(variant).size(), 6U);
        }
    }

    TEST(NDDataCreateModuleTest, ColorVideoHasOnePlanePerFrameAndChannel)
    {
        const auto variants = DataDimension::create_for_modality<uint8_t>(
            DataModality::VIDEO_COLOR, { 2, 2, 2, 3 }).first;

        ASSERT_EQ(variants.size(), 6U);
        for (const auto& variant : variants) {
            EXPECT_EQ(std::get<std::vector<uint8_t>>(variant).size(), 4U);
        }
    }

    TEST(NDDataCreateModuleTest, DepthVideoHasOnePlanePerComponentSpanningAllFrames)
    {
        const auto variants = DataDimension::create_for_modality<float>(
            DataModality::VIDEO_DEPTH, { 3, 2, 2, 2 }).first;

        ASSERT_EQ(variants.size(), 2U);
        for (const auto& variant : variants) {
            EXPECT_EQ(std::get<std::vector<float>>(variant).size(), 12U);
        }
    }

    TEST(NDDataCreateModuleTest, DepthMapHasOnePlanePerComponent)
    {
        const auto variants = DataDimension::create_for_modality<float>(
            DataModality::DEPTH_MAP, { 3, 4, 2 }, 1.5F).first;

        ASSERT_EQ(variants.size(), 2U);
        for (const auto& variant : variants) {
            const auto& plane = std::get<std::vector<float>>(variant);
            ASSERT_EQ(plane.size(), 12U);
            EXPECT_TRUE(std::ranges::all_of(plane, [](float v) { return v == 1.5F; }));
        }
    }

    TEST(NDDataCreateModuleTest, GrayscaleVideoHasOnePlanePerFrame)
    {
        const auto variants = DataDimension::create_for_modality<uint8_t>(
            DataModality::VIDEO_GRAYSCALE, { 4, 2, 3 }).first;

        ASSERT_EQ(variants.size(), 4U);
        for (const auto& variant : variants) {
            EXPECT_EQ(std::get<std::vector<uint8_t>>(variant).size(), 6U);
        }
    }

    TEST(NDDataCreateModuleTest, InterleavedColorVideoIsOneVariantOfEveryElement)
    {
        const auto variants = DataDimension::create_for_modality<uint8_t>(
            DataModality::VIDEO_COLOR, { 2, 3, 4, 3 }, uint8_t { 7 },
            MemoryLayout::ROW_MAJOR, OrganizationStrategy::INTERLEAVED).first;

        ASSERT_EQ(variants.size(), 1U);
        const auto& all = std::get<std::vector<uint8_t>>(variants[0]);
        EXPECT_EQ(all.size(), 72U);
        EXPECT_TRUE(std::ranges::all_of(all, [](uint8_t v) { return v == 7; }));
    }

    TEST(NDDataCreateModuleTest, StorageTypeFollowsTheTemplateArgument)
    {
        const auto complex_module = DataDimension::create_spectral_2d<std::complex<float>>(2, 4);
        const auto vec_module = DataDimension::create_for_modality<glm::vec3>(
            DataModality::VOLUMETRIC_3D, { 2, 2, 2 }, glm::vec3(1.0F, 2.0F, 3.0F));

        ASSERT_EQ(complex_module.first.size(), 1U);
        EXPECT_EQ(std::get<std::vector<std::complex<float>>>(complex_module.first[0]).size(), 8U);
        ASSERT_EQ(vec_module.first.size(), 1U);
        EXPECT_EQ(std::get<std::vector<glm::vec3>>(vec_module.first[0]).front(), glm::vec3(1.0F, 2.0F, 3.0F));
    }

    TEST(NDDataCreateModuleTest, VolumeIsOneVariantOfTheProductOfItsExtents)
    {
        const auto variants = DataDimension::create_for_modality<float>(
            DataModality::VOLUMETRIC_3D, { 2, 3, 4 }).first;

        ASSERT_EQ(variants.size(), 1U);
        EXPECT_EQ(std::get<std::vector<float>>(variants[0]).size(), 24U);
    }

    TEST(NDDataCreateModuleTest, AudioHelperFillsTheDefaultValue)
    {
        const auto [variants, dims] = DataDimension::create_audio_1d<double>(16, 0.5);

        ASSERT_EQ(variants.size(), 1U);
        ASSERT_EQ(dims.size(), 1U);
        EXPECT_EQ(dims[0].role, Role::TIME);
        const auto& samples = std::get<std::vector<double>>(variants[0]);
        ASSERT_EQ(samples.size(), 16U);
        EXPECT_TRUE(std::ranges::all_of(samples, [](double v) { return v == 0.5; }));
    }

    TEST(NDDataCreateModuleTest, MultichannelAudioHelperMatchesTheGenericFactory)
    {
        const auto helper = DataDimension::create_audio_multichannel<float>(8, 2);
        const auto generic = DataDimension::create_for_modality<float>(DataModality::AUDIO_MULTICHANNEL, { 8, 2 });

        EXPECT_EQ(helper.first.size(), generic.first.size());
        EXPECT_EQ(helper.second.size(), generic.second.size());
    }

    TEST(NDDataCreateModuleTest, ImageHelperIsHeightTimesWidth)
    {
        const auto [variants, dims] = DataDimension::create_image_2d<uint8_t>(3, 5);

        ASSERT_EQ(variants.size(), 1U);
        EXPECT_EQ(std::get<std::vector<uint8_t>>(variants[0]).size(), 15U);
        EXPECT_EQ(dims.size(), 2U);
    }

    TEST(NDDataCreateModuleTest, SpectralHelperIsWindowsTimesBins)
    {
        const auto [variants, dims] = DataDimension::create_spectral_2d<float>(4, 9);

        ASSERT_EQ(variants.size(), 1U);
        EXPECT_EQ(std::get<std::vector<float>>(variants[0]).size(), 36U);
        EXPECT_EQ(dims[1].role, Role::FREQUENCY);
    }

    TEST(NDDataCreateModuleTest, ModalitiesWithoutADimensionLayoutRefuseToCreateAModule)
    {
        EXPECT_THROW(
            DataDimension::create_for_modality<float>(DataModality::TENSOR_ND, { 2, 2 }),
            std::invalid_argument);
    }

    TEST(NDDataModalityTest, StructuredModalitiesAreTheVectorAndMatrixOnes)
    {
        for (const DataModality modality : {
                 DataModality::VERTEX_POSITIONS_3D,
                 DataModality::VERTEX_NORMALS_3D,
                 DataModality::VERTEX_TANGENTS_3D,
                 DataModality::VERTEX_COLORS_RGB,
                 DataModality::VERTEX_COLORS_RGBA,
                 DataModality::TEXTURE_COORDS_2D,
                 DataModality::TRANSFORMATION_MATRIX }) {
            EXPECT_TRUE(Kakshya::is_structured_modality(modality)) << static_cast<int>(modality);
        }
    }

    TEST(NDDataModalityTest, UnstructuredModalitiesAreNotStructured)
    {
        for (const DataModality modality : {
                 DataModality::AUDIO_1D,
                 DataModality::IMAGE_COLOR,
                 DataModality::VERTICES_3D,
                 DataModality::SCALAR_F32,
                 DataModality::UNKNOWN }) {
            EXPECT_FALSE(Kakshya::is_structured_modality(modality)) << static_cast<int>(modality);
        }
    }

    TEST(NDDataModalityTest, ModalityNamesAreTheEnumeratorNames)
    {
        EXPECT_EQ(Kakshya::modality_to_string(DataModality::AUDIO_1D), "AUDIO_1D");
        EXPECT_EQ(Kakshya::modality_to_string(DataModality::IMAGE_COLOR_ARRAY), "IMAGE_COLOR_ARRAY");
        EXPECT_EQ(Kakshya::modality_to_string(DataModality::VERTEX_POSITIONS_3D), "VERTEX_POSITIONS_3D");
        EXPECT_EQ(Kakshya::modality_to_string(DataModality::UNKNOWN), "UNKNOWN");
    }

    TEST(NDDataModalityTest, EveryModalityHasADistinctName)
    {
        std::set<std::string_view> names;
        for (uint8_t i = 0; i <= static_cast<uint8_t>(DataModality::UNKNOWN); ++i) {
            const auto name = Kakshya::modality_to_string(static_cast<DataModality>(i));

            EXPECT_FALSE(name.empty()) << static_cast<int>(i);
            EXPECT_TRUE(names.insert(name).second) << name;
        }
    }

    TEST(NDDataVariantTest, VariantElementsAreTheStorageTypes)
    {
        EXPECT_TRUE(Kakshya::DataVariantElement<double>);
        EXPECT_TRUE(Kakshya::DataVariantElement<float>);
        EXPECT_TRUE(Kakshya::DataVariantElement<uint8_t>);
        EXPECT_TRUE(Kakshya::DataVariantElement<uint16_t>);
        EXPECT_TRUE(Kakshya::DataVariantElement<uint32_t>);
        EXPECT_TRUE(Kakshya::DataVariantElement<std::complex<float>>);
        EXPECT_TRUE(Kakshya::DataVariantElement<std::complex<double>>);
        EXPECT_TRUE(Kakshya::DataVariantElement<glm::vec3>);
        EXPECT_TRUE(Kakshya::DataVariantElement<glm::mat4>);
    }

    TEST(NDDataVariantTest, TypesOutsideTheVariantAreNotVariantElements)
    {
        EXPECT_FALSE(Kakshya::DataVariantElement<int32_t>);
        EXPECT_FALSE(Kakshya::DataVariantElement<int64_t>);
        EXPECT_FALSE(Kakshya::DataVariantElement<glm::dvec3>);
        EXPECT_FALSE(Kakshya::DataVariantElement<std::string>);
    }

    TEST(NDDataVariantTest, VariantElementConceptIgnoresCvRefQualifiers)
    {
        EXPECT_TRUE(Kakshya::DataVariantElement<const float&>);
        EXPECT_TRUE(Kakshya::DataVariantElement<const glm::vec2&>);
    }

    TEST(NDDataFrameViewTest, DefaultViewIsEmpty)
    {
        const FrameView view {};

        EXPECT_TRUE(view.empty());
        EXPECT_EQ(view.size(), 0U);
    }

    TEST(NDDataFrameViewTest, ViewReportsTheSizeOfItsSpan)
    {
        const std::vector<float> samples(12, 1.0F);
        const FrameView view { Kakshya::DataSpanVariant { std::span<const float>(samples) } };

        EXPECT_FALSE(view.empty());
        EXPECT_EQ(view.size(), 12U);
    }

    TEST(NDDataFrameViewTest, ViewReportsTheNativeElementType)
    {
        const std::vector<uint8_t> pixels(4, 0);
        const FrameView view { Kakshya::DataSpanVariant { std::span<const uint8_t>(pixels) } };

        EXPECT_EQ(view.element_type(), std::type_index(typeid(uint8_t)));
    }

    TEST(NDDataFrameViewTest, MatchingTypedAccessAliasesTheStorage)
    {
        const std::vector<float> samples { 1.0F, 2.0F, 3.0F };
        const FrameView view { Kakshya::DataSpanVariant { std::span<const float>(samples) } };

        const auto typed = view.as<float>();

        EXPECT_EQ(typed.data(), samples.data());
        EXPECT_EQ(typed.size(), samples.size());
    }

    TEST(NDDataFrameViewTest, MismatchedTypedAccessIsEmpty)
    {
        const std::vector<float> samples { 1.0F, 2.0F, 3.0F };
        const FrameView view { Kakshya::DataSpanVariant { std::span<const float>(samples) } };

        EXPECT_TRUE(view.as<double>().empty());
        EXPECT_TRUE(view.as<uint8_t>().empty());
        EXPECT_TRUE(view.as<glm::vec3>().empty());
    }

    TEST(NDDataFrameViewTest, VectorSpansAreViewedInElements)
    {
        const std::vector<glm::vec3> points(5, glm::vec3 { 1.0F });
        const FrameView view { Kakshya::DataSpanVariant { std::span<const glm::vec3>(points) } };

        EXPECT_EQ(view.size(), 5U);
        EXPECT_EQ(view.element_type(), std::type_index(typeid(glm::vec3)));
        EXPECT_EQ(view.as<glm::vec3>().size(), 5U);
    }

    TEST(NDDataFrameViewTest, RawExposesTheUnderlyingSpanVariant)
    {
        const std::vector<double> samples { 0.5 };
        const FrameView view { Kakshya::DataSpanVariant { std::span<const double>(samples) } };

        EXPECT_TRUE(std::holds_alternative<std::span<const double>>(view.raw()));
    }

} // namespace
} // namespace MayaFlux::Test
