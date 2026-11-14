#include "gtest/gtest.h"
#include "../../support/test_metadata.hpp"

#include "MayaFlux/Kakshya/Region/RegionSegment.hpp"
#include "MayaFlux/Kakshya/Source/TensorContainer.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Kakshya::DataModality;
    using Kakshya::MemoryLayout;
    using Kakshya::Region;
    using Kakshya::RegionGroup;
    using Kakshya::RegionSegment;
    using Kakshya::TensorContainer;
    using Kakshya::TensorData;
    using Kakshya::TensorDevice;
    using Kakshya::TensorDeviceType;
    using Kakshya::TensorDType;
    using Kakshya::TensorDTypeKind;
    using Kakshya::TensorSpec;

    using Shape = std::vector<uint64_t>;
    using Floats = std::vector<float>;

    std::optional<TensorData> ramp(Shape shape, std::string name = "ramp")
    {
        uint64_t count = 1;
        for (const auto extent : shape) {
            count *= extent;
        }
        Floats values(count);
        std::iota(values.begin(), values.end(), 0.0F);
        return TensorData::from_values<float>(values, std::move(shape), std::move(name));
    }

    TensorContainer ramp_container(Shape shape)
    {
        return TensorContainer(std::move(*ramp(std::move(shape))));
    }

    Floats floats_of(const TensorData& tensor)
    {
        return tensor.to_vector<float>().value_or(Floats {});
    }

    TensorContainer column_major_container()
    {
        Floats values(12);
        std::iota(values.begin(), values.end(), 0.0F);
        auto spec = TensorSpec::contiguous(TensorDType::of<float>(), { 3, 4 }, "fortran");
        spec.strides = { 1, 3 };
        return TensorContainer(std::move(*TensorData::from_bytes(spec, std::as_bytes(std::span<const float>(values)))));
    }

    // ---------------------------------------------------------------------
    // Construction and shape
    // ---------------------------------------------------------------------

    TEST(TensorContainerTest, HoldsTheTensorItWasGiven)
    {
        const auto container = ramp_container({ 4, 5 });

        EXPECT_TRUE(container.has_data());
        EXPECT_EQ(container.get_total_elements(), 20U);
        EXPECT_EQ(container.get_data().spec().name, "ramp");
        EXPECT_EQ(floats_of(container.get_data()).back(), 19.0F);
    }

    TEST(TensorContainerTest, ASpecAllocatesAZeroFilledTensor)
    {
        const TensorContainer container(TensorSpec::contiguous(TensorDType::of<int16_t>(), { 2, 3 }, "zeros"));

        EXPECT_EQ(container.get_total_elements(), 6U);
        const auto values = container.get_data().access()->as<int16_t>();
        ASSERT_EQ(values.size(), 6U);
        EXPECT_TRUE(std::ranges::all_of(values, [](int16_t v) { return v == 0; }));
    }

    TEST(TensorContainerTest, AnInvalidTensorOrSpecIsRefused)
    {
        EXPECT_THROW(TensorContainer { TensorData {} }, std::invalid_argument);

        auto device_spec = TensorSpec::contiguous(TensorDType::of<float>(), { 2 });
        device_spec.device = TensorDevice { TensorDeviceType::CUDA, 0 };
        EXPECT_THROW(TensorContainer { device_spec }, std::invalid_argument);

        auto inconsistent = TensorSpec::contiguous(TensorDType::of<float>(), { 2, 2 });
        inconsistent.strides = { 1 };
        EXPECT_THROW(TensorContainer { inconsistent }, std::invalid_argument);
    }

    TEST(TensorContainerTest, DimensionsFramesAndTotalsFollowTheShape)
    {
        const auto container = ramp_container({ 4, 5 });

        const auto dimensions = container.get_dimensions();
        ASSERT_EQ(dimensions.size(), 2U);
        EXPECT_EQ(dimensions[0].size, 4U);
        EXPECT_EQ(dimensions[1].size, 5U);
        EXPECT_EQ(container.get_num_frames(), 4U);
        EXPECT_EQ(container.get_frame_size(), 5U);
    }

    TEST(TensorContainerTest, AScalarTensorHasOneElementAndNoFrames)
    {
        const TensorContainer container(TensorSpec::contiguous(TensorDType::of<float>(), {}));

        EXPECT_EQ(container.get_total_elements(), 1U);
        EXPECT_EQ(container.get_num_frames(), 0U);
        EXPECT_EQ(container.get_frame_size(), 0U);
    }

    TEST(TensorContainerTest, MemoryLayoutReportsFortranStridesOnly)
    {
        auto fortran = column_major_container();
        const auto plain = ramp_container({ 3, 4 });
        const auto vector = ramp_container({ 12 });

        EXPECT_EQ(fortran.get_memory_layout(), MemoryLayout::COLUMN_MAJOR);
        EXPECT_EQ(plain.get_memory_layout(), MemoryLayout::ROW_MAJOR);
        EXPECT_EQ(vector.get_memory_layout(), MemoryLayout::ROW_MAJOR);

        fortran.set_memory_layout(MemoryLayout::ROW_MAJOR);
        EXPECT_EQ(fortran.get_memory_layout(), MemoryLayout::COLUMN_MAJOR);
    }

    TEST(TensorContainerTest, StructureIsTensorModalityAndCanBeReplaced)
    {
        auto container = ramp_container({ 2, 2 });
        EXPECT_EQ(container.get_structure().modality, DataModality::TENSOR_ND);

        auto replaced = container.get_structure();
        replaced.modality = DataModality::AUDIO_1D;
        container.set_structure(replaced);
        EXPECT_EQ(std::as_const(container).get_structure().modality, DataModality::AUDIO_1D);
    }

    // ---------------------------------------------------------------------
    // Logical indices
    // ---------------------------------------------------------------------

    TEST(TensorContainerTest, LinearIndicesAreRowMajorAndRoundTrip)
    {
        const auto container = ramp_container({ 3, 4, 2 });

        EXPECT_EQ(container.coordinates_to_linear_index({ 0, 0, 0 }), 0U);
        EXPECT_EQ(container.coordinates_to_linear_index({ 1, 2, 1 }), 1U * 8U + 2U * 2U + 1U);
        for (uint64_t index = 0; index < container.get_total_elements(); ++index) {
            const auto coordinates = container.linear_index_to_coordinates(index);
            ASSERT_EQ(coordinates.size(), 3U);
            EXPECT_EQ(container.coordinates_to_linear_index(coordinates), index);
        }
    }

    TEST(TensorContainerTest, LinearIndicesIgnoreTheStorageStrides)
    {
        const auto fortran = column_major_container();
        const auto plain = ramp_container({ 3, 4 });

        for (uint64_t index = 0; index < 12; ++index) {
            EXPECT_EQ(fortran.linear_index_to_coordinates(index), plain.linear_index_to_coordinates(index));
        }
        EXPECT_EQ(fortran.coordinates_to_linear_index({ 2, 3 }), 11U);
    }

    TEST(TensorContainerTest, BadCoordinatesGiveZeroAndBadIndicesGiveNothing)
    {
        const auto container = ramp_container({ 3, 4 });

        EXPECT_EQ(container.coordinates_to_linear_index({ 3, 0 }), 0U);
        EXPECT_EQ(container.coordinates_to_linear_index({ 0, 4 }), 0U);
        EXPECT_EQ(container.coordinates_to_linear_index({ 1 }), 0U);
        EXPECT_TRUE(container.linear_index_to_coordinates(12).empty());
    }

    // ---------------------------------------------------------------------
    // Reading regions
    // ---------------------------------------------------------------------

    TEST(TensorContainerRegionTest, ViewBorrowsTheBoxWithoutCopying)
    {
        const auto container = ramp_container({ 4, 5 });
        const auto whole = container.view();
        ASSERT_TRUE(whole.has_value());

        const auto box = container.view(Region({ 1, 2 }, { 2, 4 }));

        ASSERT_TRUE(box.has_value());
        EXPECT_EQ(box->spec().shape, (Shape { 2, 3 }));
        EXPECT_EQ(box->element_data(), whole->element_data() + (1 * 5 + 2) * sizeof(float));
        EXPECT_EQ(box->storage().data(), whole->storage().data());
    }

    TEST(TensorContainerRegionTest, RegionDataIsADenseIndependentCopy)
    {
        auto container = ramp_container({ 4, 5 });

        auto copies = container.get_region_data(Region({ 1, 2 }, { 2, 4 }));

        ASSERT_EQ(copies.size(), 1U);
        EXPECT_EQ(copies[0].spec().shape, (Shape { 2, 3 }));
        EXPECT_TRUE(copies[0].spec().is_contiguous());
        EXPECT_EQ(floats_of(copies[0]), (Floats { 7, 8, 9, 12, 13, 14 }));

        copies[0].storage()[0] = std::byte { 0x7F };
        EXPECT_EQ(floats_of(container.get_data())[7], 7.0F);
    }

    TEST(TensorContainerRegionTest, TrailingAxesNotNamedSpanFully)
    {
        const auto container = ramp_container({ 4, 5 });

        const auto copies = container.get_region_data(Region({ 1 }, { 2 }));

        ASSERT_EQ(copies.size(), 1U);
        EXPECT_EQ(copies[0].spec().shape, (Shape { 2, 5 }));
        EXPECT_EQ(floats_of(copies[0]), (Floats { 5, 6, 7, 8, 9, 10, 11, 12, 13, 14 }));
    }

    TEST(TensorContainerRegionTest, APointRegionIsOneElement)
    {
        const auto container = ramp_container({ 4, 5 });

        const auto copies = container.get_region_data(Region(std::vector<uint64_t> { 3, 4 }));

        ASSERT_EQ(copies.size(), 1U);
        EXPECT_EQ(copies[0].spec().shape, (Shape { 1, 1 }));
        EXPECT_EQ(floats_of(copies[0]), (Floats { 19 }));
    }

    TEST(TensorContainerRegionTest, InvalidRegionsSelectNothing)
    {
        const auto container = ramp_container({ 4, 5 });
        const std::vector<Region> bad {
            Region {},
            Region({ 3 }, { 1 }),
            Region({ 0 }, { 4 }),
            Region({ 0, 0, 0 }, { 0, 0, 0 }),
            Region({ 0, 0 }, { 1 }),
        };

        for (const auto& region : bad) {
            EXPECT_TRUE(container.get_region_data(region).empty());
            EXPECT_FALSE(container.view(region).has_value());
            EXPECT_FALSE(container.is_region_loaded(region));
        }
        EXPECT_TRUE(container.is_region_loaded(Region({ 0, 0 }, { 3, 4 })));
    }

    TEST(TensorContainerRegionTest, GroupDataSkipsInvalidRegionsAndKeepsOrder)
    {
        const auto container = ramp_container({ 4, 5 });
        RegionGroup group("mixed");
        group.add_region(Region({ 0, 0 }, { 0, 1 }));
        group.add_region(Region({ 9, 9 }, { 9, 9 }));
        group.add_region(Region({ 3, 3 }, { 3, 4 }));

        const auto copies = container.get_region_group_data(group);

        ASSERT_EQ(copies.size(), 2U);
        EXPECT_EQ(floats_of(copies[0]), (Floats { 0, 1 }));
        EXPECT_EQ(floats_of(copies[1]), (Floats { 18, 19 }));
    }

    TEST(TensorContainerRegionTest, RegionsOfAColumnMajorTensorReadLogically)
    {
        const auto container = column_major_container();

        const auto copies = container.get_region_data(Region({ 1, 1 }, { 2, 2 }));

        ASSERT_EQ(copies.size(), 1U);
        EXPECT_EQ(floats_of(copies[0]), (Floats { 4, 7, 5, 8 }));
    }

    TEST(TensorContainerRegionTest, RegionsOfAReversedAxisReadLogically)
    {
        Floats values { 0, 1, 2, 3, 4, 5 };
        auto spec = TensorSpec::contiguous(TensorDType::of<float>(), { 6 }, "reversed");
        spec.strides = { -1 };
        spec.byte_offset = 5 * sizeof(float);
        const TensorContainer container(std::move(*TensorData::from_bytes(spec, std::as_bytes(std::span<const float>(values)))));

        const auto copies = container.get_region_data(Region({ 1 }, { 3 }));

        ASSERT_EQ(copies.size(), 1U);
        EXPECT_EQ(floats_of(copies[0]), (Floats { 4, 3, 2 }));
    }

    TEST(TensorContainerRegionTest, ASubByteTensorOffersOnlyItsWholeExtent)
    {
        const TensorContainer container(TensorSpec::contiguous(
            TensorDType { TensorDTypeKind::FLOAT4_E2M1FN, 4, 1 }, { 4, 4 }));

        EXPECT_FALSE(container.view(Region({ 0, 0 }, { 1, 1 })).has_value());
        EXPECT_TRUE(container.view(Region({ 0, 0 }, { 3, 3 })).has_value());
    }

    struct DtypeCase {
        std::string_view name;
        TensorDType dtype;
    };

    class TensorContainerDtypeTest : public ::testing::TestWithParam<DtypeCase> { };

    INSTANTIATE_TEST_SUITE_P(Dtypes, TensorContainerDtypeTest,
        ::testing::Values(
            DtypeCase { "Float32", TensorDType::of<float>() },
            DtypeCase { "Float64", TensorDType::of<double>() },
            DtypeCase { "Float16", TensorDType::float16() },
            DtypeCase { "BFloat16", TensorDType::bfloat16() },
            DtypeCase { "Int8", TensorDType::of<int8_t>() },
            DtypeCase { "UInt16", TensorDType::of<uint16_t>() },
            DtypeCase { "Int64", TensorDType::of<int64_t>() },
            DtypeCase { "ComplexFloat", TensorDType::of<std::complex<float>>() }),
        [](const ::testing::TestParamInfo<DtypeCase>& info) { return std::string(info.param.name); });

    TEST_P(TensorContainerDtypeTest, RegionCopiesKeepTheElementBytesAndType)
    {
        const auto dtype = GetParam().dtype;
        const size_t element = dtype.element_bytes();
        std::vector<std::byte> bytes(20 * element);
        for (size_t i = 0; i < bytes.size(); ++i) {
            bytes[i] = static_cast<std::byte>((i * 7 + 3) & 0xFF);
        }
        const TensorContainer container(std::move(*TensorData::from_bytes(
            TensorSpec::contiguous(dtype, { 4, 5 }, "typed"), bytes)));

        const auto copies = container.get_region_data(Region({ 1, 2 }, { 2, 4 }));

        ASSERT_EQ(copies.size(), 1U);
        EXPECT_EQ(copies[0].spec().dtype, dtype);
        EXPECT_EQ(copies[0].spec().shape, (Shape { 2, 3 }));

        std::vector<std::byte> expected;
        for (size_t row = 1; row <= 2; ++row) {
            for (size_t column = 2; column <= 4; ++column) {
                const auto first = bytes.begin() + static_cast<std::ptrdiff_t>((row * 5 + column) * element);
                expected.insert(expected.end(), first, first + static_cast<std::ptrdiff_t>(element));
            }
        }
        const auto stored = copies[0].storage();
        ASSERT_EQ(stored.size(), expected.size());
        EXPECT_TRUE(std::equal(stored.begin(), stored.end(), expected.begin()));
    }

    // ---------------------------------------------------------------------
    // Writing regions
    // ---------------------------------------------------------------------

    TEST(TensorContainerWriteTest, SetRegionDataWritesTheBox)
    {
        TensorContainer container(TensorSpec::contiguous(TensorDType::of<float>(), { 3, 4 }));
        std::vector<TensorData> patch;
        patch.push_back(*ramp({ 2, 2 }));
        const Region box({ 1, 1 }, { 2, 2 });

        container.set_region_data(box, patch);

        EXPECT_EQ(floats_of(container.get_data()), (Floats { 0, 0, 0, 0, 0, 0, 1, 0, 0, 2, 3, 0 }));
        EXPECT_EQ(floats_of(container.get_region_data(box).front()), (Floats { 0, 1, 2, 3 }));
    }

    TEST(TensorContainerWriteTest, SetRegionDataWritesALogicalBoxOfAColumnMajorTensor)
    {
        auto container = column_major_container();
        std::vector<TensorData> patch;
        patch.push_back(*ramp({ 1, 2 }));

        container.set_region_data(Region({ 0, 0 }, { 0, 1 }), patch);

        const auto copies = container.get_region_data(Region({ 0, 0 }, { 0, 3 }));
        ASSERT_EQ(copies.size(), 1U);
        EXPECT_EQ(floats_of(copies[0]), (Floats { 0, 1, 6, 9 }));
    }

    TEST(TensorContainerWriteTest, SetRegionDataIgnoresInputsThatDoNotFit)
    {
        auto container = ramp_container({ 3, 4 });
        const auto before = floats_of(container.get_data());
        const Region box({ 0, 0 }, { 1, 1 });

        std::vector<TensorData> two;
        two.push_back(*ramp({ 2, 2 }));
        two.push_back(*ramp({ 2, 2 }));
        container.set_region_data(box, two);

        std::vector<TensorData> wrong_shape;
        wrong_shape.push_back(*ramp({ 2, 3 }));
        container.set_region_data(box, wrong_shape);

        std::vector<double> wide(4, 9.0);
        std::vector<TensorData> wrong_type;
        wrong_type.push_back(*TensorData::from_values<double>(wide, { 2, 2 }));
        container.set_region_data(box, wrong_type);

        std::vector<TensorData> fits;
        fits.push_back(*ramp({ 2, 2 }));
        container.set_region_data(Region({ 2, 3 }, { 3, 4 }), fits);
        container.set_region_data(box, {});

        EXPECT_EQ(floats_of(container.get_data()), before);
    }

    // ---------------------------------------------------------------------
    // Segments
    // ---------------------------------------------------------------------

    TEST(TensorContainerSegmentTest, ASegmentIsTheBoxInsideItsSourceRegion)
    {
        const auto container = ramp_container({ 4, 5 });
        const RegionSegment segment(Region({ 1, 0 }, { 3, 4 }), { 1, 2 }, { 2, 3 });

        const auto copies = container.get_segments_data({ segment });

        ASSERT_EQ(copies.size(), 1U);
        EXPECT_EQ(copies[0].spec().shape, (Shape { 2, 3 }));
        EXPECT_EQ(floats_of(copies[0]), (Floats { 12, 13, 14, 17, 18, 19 }));
    }

    TEST(TensorContainerSegmentTest, MalformedSegmentsAreSkipped)
    {
        const auto container = ramp_container({ 4, 5 });
        const Region source({ 0, 0 }, { 3, 4 });
        const std::vector<RegionSegment> segments {
            RegionSegment(source, { 0 }, { 1 }),
            RegionSegment(source, { 0, 0 }, { 0, 1 }),
            RegionSegment(source, { 4, 0 }, { 1, 1 }),
            RegionSegment(source, { 3, 0 }, { 2, 1 }),
            RegionSegment(Region {}, {}, {}),
            RegionSegment(source, { 1, 1 }, { 2, 2 }),
        };

        const auto copies = container.get_segments_data(segments);

        ASSERT_EQ(copies.size(), 1U);
        EXPECT_EQ(floats_of(copies[0]), (Floats { 6, 7, 11, 12 }));
    }

    TEST(TensorContainerSegmentTest, ACachedSegmentReturnsItsCachedTensor)
    {
        const auto container = ramp_container({ 4, 5 });
        RegionSegment segment(Region({ 0, 0 }, { 1, 1 }));
        std::vector<float> marker { 100, 200 };
        segment.mark_cached(Kakshya::tensor_cache(), *TensorData::from_values<float>(marker, { 1, 2 }, "cached"));

        const auto copies = container.get_segments_data({ segment });

        ASSERT_EQ(copies.size(), 1U);
        EXPECT_EQ(copies[0].spec().name, "cached");
        EXPECT_EQ(floats_of(copies[0]), (Floats { 100, 200 }));
    }

    TEST(TensorContainerSegmentTest, ASegmentCachedUnderAnotherKeyReadsTheSource)
    {
        const auto container = ramp_container({ 4, 5 });
        RegionSegment segment(Region({ 0, 0 }, { 1, 1 }));
        segment.mark_cached({ Kakshya::DataVariant { std::vector<double> { 9.0 } } });

        const auto copies = container.get_segments_data({ segment });

        ASSERT_EQ(copies.size(), 1U);
        EXPECT_EQ(floats_of(copies[0]), (Floats { 0, 1, 5, 6 }));
    }

    // ---------------------------------------------------------------------
    // Region groups, residency and clearing
    // ---------------------------------------------------------------------

    TEST(TensorContainerGroupTest, GroupsAreStoredByNameAndRemovable)
    {
        auto container = ramp_container({ 2, 2 });
        RegionGroup group("rows", { Region({ 0 }, { 0 }), Region({ 1 }, { 1 }) });

        container.add_region_group(group);

        EXPECT_EQ(container.get_region_group("rows").regions.size(), 2U);
        EXPECT_EQ(container.get_all_region_groups().size(), 1U);
        EXPECT_TRUE(container.get_region_group("absent").name.empty());

        container.remove_region_group("rows");
        EXPECT_TRUE(container.get_all_region_groups().empty());
    }

    TEST(TensorContainerGroupTest, LoadAndUnloadKeepTheTensorResident)
    {
        auto container = ramp_container({ 2, 2 });
        const Region box({ 0, 0 }, { 1, 1 });

        container.unload_region(box);
        container.load_region(box);

        EXPECT_TRUE(container.is_region_loaded(box));
        EXPECT_TRUE(container.has_data());
    }

    TEST(TensorContainerTest, RawDataPointsAtElementZero)
    {
        Floats values { 0, 1, 2, 3, 4, 5 };
        auto spec = TensorSpec::contiguous(TensorDType::of<float>(), { 6 });
        spec.strides = { -1 };
        spec.byte_offset = 5 * sizeof(float);
        const TensorContainer container(std::move(*TensorData::from_bytes(spec, std::as_bytes(std::span<const float>(values)))));

        ASSERT_NE(container.get_raw_data(), nullptr);
        EXPECT_EQ(container.get_raw_data(), container.view()->element_data());
        EXPECT_EQ(*static_cast<const float*>(container.get_raw_data()), 5.0F);
    }

    TEST(TensorContainerTest, ClearEmptiesTheTensorKeepsTypeAndNameAndDropsGroups)
    {
        auto container = ramp_container({ 4, 5 });
        container.add_region_group(RegionGroup("g", { Region({ 0 }, { 1 }) }));
        ASSERT_TRUE(container.has_data());

        container.clear();

        EXPECT_FALSE(container.has_data());
        EXPECT_EQ(container.get_total_elements(), 0U);
        EXPECT_EQ(container.get_raw_data(), nullptr);
        EXPECT_EQ(container.get_data().spec().dtype, TensorDType::of<float>());
        EXPECT_EQ(container.get_data().spec().name, "ramp");
        EXPECT_TRUE(container.get_all_region_groups().empty());
        EXPECT_TRUE(container.get_region_data(Region({ 0 }, { 0 })).empty());
    }

    TEST(TensorContainerTest, InsertionWritesThroughToTheContainer)
    {
        auto container = ramp_container({ 2, 3 });

        EXPECT_TRUE(container.insert().reshape({ 3, 2 }));

        EXPECT_EQ(container.get_num_frames(), 3U);
        EXPECT_EQ(container.get_frame_size(), 2U);
        EXPECT_EQ(container.get_dimensions()[0].size, 3U);
    }

}
}
