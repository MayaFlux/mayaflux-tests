#include "gtest/gtest.h"
#include "../../support/test_metadata.hpp"

#include "MayaFlux/Kakshya/NDData/TensorDLPack.hpp"
#include "MayaFlux/Kakshya/NDData/TensorInsertion.hpp"
#include "MayaFlux/Kakshya/Region/Region.hpp"

#include "dlpack.h"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Kakshya::DataDimension;
    using Kakshya::DataVariant;
    using Kakshya::GpuDataFormat;
    using Kakshya::Region;
    using Kakshya::TensorAccess;
    using Kakshya::TensorData;
    using Kakshya::TensorDevice;
    using Kakshya::TensorDeviceType;
    using Kakshya::TensorDType;
    using Kakshya::TensorDTypeKind;
    using Kakshya::TensorImport;
    using Kakshya::TensorInsertion;
    using Kakshya::TensorQuantization;
    using Kakshya::TensorSet;
    using Kakshya::TensorSpec;

    std::optional<TensorData> make_floats(std::vector<uint64_t> shape, std::string name = "t")
    {
        uint64_t count = 1;
        for (const auto extent : shape)
            count *= extent;
        std::vector<float> values(count);
        std::iota(values.begin(), values.end(), 0.0F);
        return TensorData::from_values<float>(values, std::move(shape), std::move(name));
    }

    std::vector<float> floats_of(const TensorData& tensor)
    {
        return tensor.to_vector<float>().value_or(std::vector<float> {});
    }

    std::optional<TensorAccess> view_of(const TensorData& base, TensorSpec spec)
    {
        return Kakshya::as_tensor_access(base.storage(), spec);
    }

    template <typename T>
    void expect_variant_round_trip(std::vector<T> values)
    {
        const DataVariant variant { values };
        const auto tensor = TensorData::from_variant(variant, {}, "v");
        ASSERT_TRUE(tensor.has_value());
        EXPECT_TRUE(tensor->spec().dtype.template is<T>());
        EXPECT_EQ(tensor->spec().shape, (std::vector<uint64_t> { static_cast<uint64_t>(values.size()) }));

        const auto back = tensor->to_variant();
        ASSERT_TRUE(back.has_value());
        const auto* typed = std::get_if<std::vector<T>>(&*back);
        ASSERT_NE(typed, nullptr);
        EXPECT_EQ(*typed, values);
    }

    // ---------------------------------------------------------------------
    // TensorDType
    // ---------------------------------------------------------------------

    TEST(TensorDTypeTest, CppElementTypesMapToTheirKindAndWidth)
    {
        EXPECT_EQ(TensorDType::of<float>(), (TensorDType { TensorDTypeKind::FLOAT, 32, 1 }));
        EXPECT_EQ(TensorDType::of<double>(), (TensorDType { TensorDTypeKind::FLOAT, 64, 1 }));
        EXPECT_EQ(TensorDType::of<int64_t>(), (TensorDType { TensorDTypeKind::INT, 64, 1 }));
        EXPECT_EQ(TensorDType::of<uint8_t>(), (TensorDType { TensorDTypeKind::UINT, 8, 1 }));
        EXPECT_EQ(TensorDType::of<bool>(), (TensorDType { TensorDTypeKind::BOOL, 8, 1 }));
        EXPECT_EQ(TensorDType::of<std::complex<float>>(), (TensorDType { TensorDTypeKind::COMPLEX, 64, 1 }));
        EXPECT_TRUE(TensorDType::of<int32_t>().is<int32_t>());
        EXPECT_FALSE(TensorDType::of<int32_t>().is<uint32_t>());
    }

    TEST(TensorDTypeTest, HalfAndBrainFloatAreFirstClassTwoByteTypes)
    {
        EXPECT_EQ(TensorDType::float16().element_bytes(), 2U);
        EXPECT_EQ(TensorDType::bfloat16().element_bytes(), 2U);
        EXPECT_NE(TensorDType::float16(), TensorDType::bfloat16());
        EXPECT_NE(TensorDType::float16(), TensorDType::of<uint16_t>());
    }

    TEST(TensorDTypeTest, SubByteTypesPackIntoWholeBytes)
    {
        const TensorDType nibble { TensorDTypeKind::FLOAT4_E2M1FN, 4, 1 };
        EXPECT_TRUE(nibble.is_sub_byte());
        EXPECT_EQ(nibble.element_bytes(), 0U);
        EXPECT_EQ(nibble.storage_bytes(0), std::optional<size_t> { 0 });
        EXPECT_EQ(nibble.storage_bytes(5), std::optional<size_t> { 3 });
        EXPECT_EQ(nibble.storage_bytes(8), std::optional<size_t> { 4 });
    }

    TEST(TensorDTypeTest, StorageBytesRefusesOverflowAndZeroWidth)
    {
        EXPECT_FALSE(TensorDType::of<double>().storage_bytes(std::numeric_limits<uint64_t>::max()).has_value());
        EXPECT_FALSE((TensorDType { TensorDTypeKind::FLOAT, 0, 1 }).storage_bytes(4).has_value());
    }

    TEST(TensorDTypeTest, NamesAreShortAndLaneAware)
    {
        EXPECT_EQ(TensorDType::of<float>().name(), "f32");
        EXPECT_EQ(TensorDType::bfloat16().name(), "bf16");
        EXPECT_EQ(TensorDType::of<int64_t>().name(), "i64");
        EXPECT_EQ(TensorDType::of<uint8_t>().name(), "u8");
        EXPECT_EQ((TensorDType { TensorDTypeKind::FLOAT, 32, 4 }).name(), "f32x4");
    }

    TEST(TensorDTypeTest, GpuFormatExistsOnlyWhereTheFormatIsExact)
    {
        EXPECT_EQ(TensorDType::of<float>().gpu_format(), std::optional { GpuDataFormat::FLOAT32 });
        EXPECT_EQ((TensorDType { TensorDTypeKind::FLOAT, 32, 3 }).gpu_format(), std::optional { GpuDataFormat::VEC3_F32 });
        EXPECT_EQ(TensorDType::of<int32_t>().gpu_format(), std::optional { GpuDataFormat::INT32 });
        EXPECT_EQ(TensorDType::of<uint8_t>().gpu_format(), std::optional { GpuDataFormat::UINT8 });
        EXPECT_EQ(TensorDType::float16().gpu_format(), std::optional { GpuDataFormat::UINT16 });
        EXPECT_FALSE(TensorDType::of<int64_t>().gpu_format().has_value());
        EXPECT_FALSE(TensorDType::bfloat16().gpu_format().has_value());
    }

    // ---------------------------------------------------------------------
    // TensorSpec
    // ---------------------------------------------------------------------

    TEST(TensorSpecTest, ContiguousSpecIsValid)
    {
        const auto spec = TensorSpec::contiguous(TensorDType::of<float>(), { 2, 3 }, "a");
        EXPECT_TRUE(spec.is_valid());
        EXPECT_TRUE(spec.diagnose().empty());
        EXPECT_EQ(spec.rank(), 2U);
        EXPECT_EQ(spec.element_count(), std::optional<uint64_t> { 6 });
    }

    TEST(TensorSpecTest, ScalarAndEmptyShapesCountCorrectly)
    {
        EXPECT_EQ(TensorSpec::contiguous(TensorDType::of<float>(), {}).element_count(), std::optional<uint64_t> { 1 });
        EXPECT_EQ(TensorSpec::contiguous(TensorDType::of<float>(), { 2, 0 }).element_count(), std::optional<uint64_t> { 0 });
    }

    TEST(TensorSpecTest, ElementCountOverflowIsDetected)
    {
        auto spec = TensorSpec::contiguous(TensorDType::of<float>(), { std::numeric_limits<uint64_t>::max() });
        EXPECT_FALSE(spec.element_count().has_value());
        EXPECT_FALSE(spec.is_valid());
    }

    TEST(TensorSpecTest, DiagnoseNamesTheInconsistency)
    {
        auto strided = TensorSpec::contiguous(TensorDType::of<float>(), { 2, 3 }, "w");
        strided.strides = { 3 };
        EXPECT_NE(strided.diagnose().find("strides rank"), std::string::npos);
        EXPECT_NE(strided.diagnose().find("'w'"), std::string::npos);

        auto axes = TensorSpec::contiguous(TensorDType::of<float>(), { 2, 3 });
        axes.axes.emplace_back("only", 2);
        EXPECT_NE(axes.diagnose().find("axes rank"), std::string::npos);

        auto zero = TensorSpec::contiguous(TensorDType { TensorDTypeKind::FLOAT, 0, 1 }, { 2 });
        EXPECT_NE(zero.diagnose().find("zero width"), std::string::npos);

        auto sub_byte = TensorSpec::contiguous(TensorDType { TensorDTypeKind::FLOAT4_E2M1FN, 4, 1 }, { 2, 2 });
        sub_byte.strides = { 1, 2 };
        EXPECT_NE(sub_byte.diagnose().find("sub-byte"), std::string::npos);
    }

    TEST(TensorSpecTest, QuantizationMustMatchTheShape)
    {
        auto spec = TensorSpec::contiguous(TensorDType::of<int8_t>(), { 4, 3 });

        spec.quantization = TensorQuantization { .scale = { 0.5 }, .zero_point = {}, .axis = std::nullopt };
        EXPECT_TRUE(spec.is_valid());

        spec.quantization = TensorQuantization { .scale = { 0.5, 0.25 }, .zero_point = {}, .axis = 0 };
        EXPECT_FALSE(spec.is_valid());

        spec.quantization = TensorQuantization { .scale = { 1, 1, 1, 1 }, .zero_point = { 0, 0, 0, 0 }, .axis = 0 };
        EXPECT_TRUE(spec.is_valid());

        spec.quantization = TensorQuantization { .scale = { 1, 1, 1, 1 }, .zero_point = { 0 }, .axis = 0 };
        EXPECT_FALSE(spec.is_valid());

        spec.quantization = TensorQuantization { .scale = { 1 }, .zero_point = {}, .axis = 5 };
        EXPECT_FALSE(spec.is_valid());

        spec.quantization = TensorQuantization { .scale = {}, .zero_point = {}, .axis = std::nullopt };
        EXPECT_FALSE(spec.is_valid());
    }

    TEST(TensorSpecTest, ContiguityIgnoresUnitAxes)
    {
        auto spec = TensorSpec::contiguous(TensorDType::of<float>(), { 2, 3 });
        spec.strides = { 3, 1 };
        EXPECT_TRUE(spec.is_contiguous());

        spec.strides = { 1, 2 };
        EXPECT_FALSE(spec.is_contiguous());

        auto unit = TensorSpec::contiguous(TensorDType::of<float>(), { 1, 3 });
        unit.strides = { 99, 1 };
        EXPECT_TRUE(unit.is_contiguous());
    }

    TEST(TensorSpecTest, ResolvedStridesFollowCOrder)
    {
        const auto spec = TensorSpec::contiguous(TensorDType::of<float>(), { 2, 3, 4 });
        EXPECT_EQ(spec.resolved_strides(), (std::vector<int64_t> { 12, 4, 1 }));
        EXPECT_TRUE(TensorSpec::contiguous(TensorDType::of<float>(), {}).resolved_strides().empty());
    }

    TEST(TensorSpecTest, ByteRangeCoversContiguousOffsetAndStridedLayouts)
    {
        using Range = std::optional<std::pair<uint64_t, uint64_t>>;

        auto spec = TensorSpec::contiguous(TensorDType::of<float>(), { 2, 3 });
        EXPECT_EQ(spec.byte_range(), (Range { { 0, 24 } }));

        spec.byte_offset = 8;
        EXPECT_EQ(spec.byte_range(), (Range { { 8, 32 } }));

        auto transposed = TensorSpec::contiguous(TensorDType::of<float>(), { 3, 2 });
        transposed.strides = { 1, 3 };
        EXPECT_EQ(transposed.byte_range(), (Range { { 0, 24 } }));

        auto broadcast = TensorSpec::contiguous(TensorDType::of<float>(), { 4, 3 });
        broadcast.strides = { 0, 1 };
        EXPECT_EQ(broadcast.byte_range(), (Range { { 0, 12 } }));

        auto empty = TensorSpec::contiguous(TensorDType::of<float>(), { 0 });
        EXPECT_EQ(empty.byte_range(), (Range { { 0, 0 } }));
    }

    TEST(TensorSpecTest, NegativeStridesNeedRoomBeforeTheOffset)
    {
        using Range = std::optional<std::pair<uint64_t, uint64_t>>;

        auto reversed = TensorSpec::contiguous(TensorDType::of<float>(), { 4 });
        reversed.strides = { -1 };
        reversed.byte_offset = 12;
        EXPECT_EQ(reversed.byte_range(), (Range { { 0, 16 } }));

        reversed.byte_offset = 0;
        EXPECT_FALSE(reversed.byte_range().has_value());
        EXPECT_FALSE(reversed.is_valid());
    }

    TEST(TensorSpecTest, DimensionsKeepAxisRolesAndTakeSizesFromTheShape)
    {
        auto spec = TensorSpec::contiguous(TensorDType::of<float>(), { 2, 3 });
        const auto plain = spec.dimensions();
        ASSERT_EQ(plain.size(), 2U);
        EXPECT_EQ(plain[0].name, "axis0");
        EXPECT_EQ(plain[0].size, 2U);
        EXPECT_EQ(plain[0].stride, 3U);
        EXPECT_EQ(plain[1].stride, 1U);

        spec.axes = { DataDimension("batch", 99, 1, DataDimension::Role::CUSTOM), DataDimension::channel(99) };
        const auto named = spec.dimensions();
        EXPECT_EQ(named[0].name, "batch");
        EXPECT_EQ(named[0].size, 2U);
        EXPECT_EQ(named[1].role, DataDimension::Role::CHANNEL);
        EXPECT_EQ(named[1].size, 3U);
    }

    TEST(TensorSpecTest, WindowKeepsTheStridesAndMovesTheOffset)
    {
        const auto base = make_floats({ 4, 5 });
        ASSERT_TRUE(base.has_value());
        const std::vector<uint64_t> origin { 1, 2 };
        const std::vector<uint64_t> extent { 2, 3 };

        const auto window = base->spec().window(origin, extent);

        ASSERT_TRUE(window.has_value());
        EXPECT_EQ(window->shape, (std::vector<uint64_t> { 2, 3 }));
        EXPECT_EQ(window->strides, (std::vector<int64_t> { 5, 1 }));
        EXPECT_EQ(window->byte_offset, (1U * 5U + 2U) * sizeof(float));
        EXPECT_TRUE(window->is_valid());

        const auto view = view_of(*base, *window);
        ASSERT_TRUE(view.has_value());
        const auto copy = TensorData::copy_of(*view);
        ASSERT_TRUE(copy.has_value());
        EXPECT_EQ(floats_of(*copy), (std::vector<float> { 7, 8, 9, 12, 13, 14 }));
    }

    TEST(TensorSpecTest, WindowOfTheWholeTensorIsTheSpecItself)
    {
        const auto spec = TensorSpec::contiguous(TensorDType::of<float>(), { 4, 5 }, "whole");
        const std::vector<uint64_t> origin { 0, 0 };
        const std::vector<uint64_t> extent { 4, 5 };

        const auto window = spec.window(origin, extent);

        ASSERT_TRUE(window.has_value());
        EXPECT_EQ(window->shape, spec.shape);
        EXPECT_TRUE(window->strides.empty());
        EXPECT_EQ(window->byte_offset, 0U);
        EXPECT_EQ(window->name, "whole");
    }

    TEST(TensorSpecTest, WindowRefusesBoxesThatDoNotFit)
    {
        const auto spec = TensorSpec::contiguous(TensorDType::of<float>(), { 4, 5 });
        const auto window = [&](std::vector<uint64_t> origin, std::vector<uint64_t> extent) {
            return spec.window(origin, extent);
        };

        EXPECT_FALSE(window({ 0 }, { 2 }).has_value());
        EXPECT_FALSE(window({ 0, 0, 0 }, { 1, 1, 1 }).has_value());
        EXPECT_FALSE(window({ 0, 0 }, { 0, 2 }).has_value());
        EXPECT_FALSE(window({ 4, 0 }, { 1, 1 }).has_value());
        EXPECT_FALSE(window({ 3, 0 }, { 2, 1 }).has_value());
        EXPECT_FALSE(window({ 3, 0 }, { std::numeric_limits<uint64_t>::max(), 1 }).has_value());
        EXPECT_TRUE(window({ 3, 4 }, { 1, 1 }).has_value());
    }

    TEST(TensorSpecTest, WindowOfAReversedAxisMovesTowardsTheStart)
    {
        std::vector<float> values { 0, 1, 2, 3, 4, 5 };
        auto spec = TensorSpec::contiguous(TensorDType::of<float>(), { 6 });
        spec.strides = { -1 };
        spec.byte_offset = 5 * sizeof(float);
        const auto base = TensorData::from_bytes(spec, std::as_bytes(std::span<const float>(values)));
        ASSERT_TRUE(base.has_value());
        const std::vector<uint64_t> origin { 1 };
        const std::vector<uint64_t> extent { 3 };

        const auto window = base->spec().window(origin, extent);

        ASSERT_TRUE(window.has_value());
        EXPECT_EQ(window->byte_offset, 4U * sizeof(float));
        const auto view = view_of(*base, *window);
        ASSERT_TRUE(view.has_value());
        EXPECT_EQ(floats_of(*TensorData::copy_of(*view)), (std::vector<float> { 4, 3, 2 }));
    }

    TEST(TensorSpecTest, WindowCutsPerAxisQuantizationToTheSameRange)
    {
        auto spec = TensorSpec::contiguous(TensorDType::of<int8_t>(), { 4, 3 });
        spec.quantization = TensorQuantization { .scale = { 1, 2, 3, 4 }, .zero_point = { 0, 1, 2, 3 }, .axis = 0 };
        const std::vector<uint64_t> origin { 1, 0 };
        const std::vector<uint64_t> extent { 2, 3 };

        const auto window = spec.window(origin, extent);

        ASSERT_TRUE(window.has_value());
        ASSERT_TRUE(window->quantization.has_value());
        EXPECT_EQ(window->quantization->scale, (std::vector<double> { 2, 3 }));
        EXPECT_EQ(window->quantization->zero_point, (std::vector<int64_t> { 1, 2 }));
        EXPECT_TRUE(window->is_valid());
    }

    TEST(TensorSpecTest, WindowLeavesPerTensorQuantizationAlone)
    {
        auto spec = TensorSpec::contiguous(TensorDType::of<int8_t>(), { 4, 3 });
        spec.quantization = TensorQuantization { .scale = { 0.5 }, .zero_point = { 3 }, .axis = std::nullopt };
        const std::vector<uint64_t> origin { 2, 1 };
        const std::vector<uint64_t> extent { 1, 2 };

        const auto window = spec.window(origin, extent);

        ASSERT_TRUE(window.has_value());
        EXPECT_EQ(window->quantization->scale, (std::vector<double> { 0.5 }));
        EXPECT_EQ(window->quantization->zero_point, (std::vector<int64_t> { 3 }));
        EXPECT_TRUE(window->is_valid());
    }

    TEST(TensorSpecTest, SubByteWindowIsOnlyTheWholeTensor)
    {
        const auto spec = TensorSpec::contiguous(TensorDType { TensorDTypeKind::FLOAT4_E2M1FN, 4, 1 }, { 4, 4 });
        ASSERT_TRUE(spec.is_valid());
        const std::vector<uint64_t> origin { 0, 0 };

        EXPECT_FALSE(spec.window(origin, std::vector<uint64_t> { 2, 2 }).has_value());
        EXPECT_TRUE(spec.window(origin, std::vector<uint64_t> { 4, 4 }).has_value());
    }

    TEST(TensorSpecTest, WindowByRegionNamesLeadingAxesAndSpansTheRest)
    {
        const auto spec = TensorSpec::contiguous(TensorDType::of<float>(), { 4, 5 });

        const auto rows = spec.window(Region({ 1 }, { 2 }));
        ASSERT_TRUE(rows.has_value());
        EXPECT_EQ(rows->shape, (std::vector<uint64_t> { 2, 5 }));
        EXPECT_EQ(rows->byte_offset, 5U * sizeof(float));

        const auto box = spec.window(Region({ 1, 1 }, { 2, 3 }));
        ASSERT_TRUE(box.has_value());
        EXPECT_EQ(box->shape, (std::vector<uint64_t> { 2, 3 }));
        EXPECT_EQ(box->byte_offset, (5U + 1U) * sizeof(float));

        const std::vector<uint64_t> origin { 1, 1 };
        const std::vector<uint64_t> extent { 2, 3 };
        const auto direct = spec.window(origin, extent);
        ASSERT_TRUE(direct.has_value());
        EXPECT_EQ(box->strides, direct->strides);
        EXPECT_EQ(box->byte_offset, direct->byte_offset);
    }

    TEST(TensorSpecTest, WindowByRegionRefusesEmptyInvertedAndOversizedRegions)
    {
        const auto spec = TensorSpec::contiguous(TensorDType::of<float>(), { 4, 5 });

        EXPECT_FALSE(spec.window(Region {}).has_value());
        EXPECT_FALSE(spec.window(Region({ 3 }, { 1 })).has_value());
        EXPECT_FALSE(spec.window(Region({ 0 }, { 4 })).has_value());
        EXPECT_FALSE(spec.window(Region({ 0, 0, 0 }, { 0, 0, 0 })).has_value());
        EXPECT_FALSE(spec.window(Region({ 0, 0 }, { 1 })).has_value());
        EXPECT_TRUE(spec.window(Region(std::vector<uint64_t> { 3, 4 })).has_value());
    }

    // ---------------------------------------------------------------------
    // TensorData
    // ---------------------------------------------------------------------

    TEST(TensorDataTest, FromValuesKeepsTypeShapeAndContent)
    {
        const auto tensor = make_floats({ 2, 3 }, "weights");
        ASSERT_TRUE(tensor.has_value());
        EXPECT_TRUE(tensor->is_valid());
        EXPECT_EQ(tensor->byte_size(), 24U);
        EXPECT_EQ(tensor->spec().name, "weights");
        EXPECT_TRUE(tensor->spec().dtype.is<float>());
        EXPECT_EQ(floats_of(*tensor), (std::vector<float> { 0, 1, 2, 3, 4, 5 }));
    }

    TEST(TensorDataTest, StorageIsAlignedForExchange)
    {
        const auto tensor = make_floats({ 5 });
        ASSERT_TRUE(tensor.has_value());
        EXPECT_EQ(reinterpret_cast<uintptr_t>(tensor->storage().data()) % 256, 0U);
    }

    TEST(TensorDataTest, MissingShapeBecomesOneDimension)
    {
        std::vector<int32_t> values { 1, 2, 3 };
        const auto tensor = TensorData::from_values<int32_t>(values);
        ASSERT_TRUE(tensor.has_value());
        EXPECT_EQ(tensor->spec().shape, (std::vector<uint64_t> { 3 }));
    }

    TEST(TensorDataTest, ShapeThatDisagreesWithTheCountIsRefused)
    {
        std::vector<float> values(5, 1.0F);
        EXPECT_FALSE(TensorData::from_values<float>(values, { 2, 3 }).has_value());
    }

    TEST(TensorDataTest, FromBytesRequiresExactlyTheAddressedStorage)
    {
        const auto spec = TensorSpec::contiguous(TensorDType::of<float>(), { 2 });
        std::vector<std::byte> bytes(7);
        EXPECT_FALSE(TensorData::from_bytes(spec, bytes).has_value());
        bytes.resize(8);
        EXPECT_TRUE(TensorData::from_bytes(spec, bytes).has_value());
    }

    TEST(TensorDataTest, AllocateIsZeroFilledAndRefusesDeviceMemory)
    {
        const auto zeros = TensorData::allocate(TensorSpec::contiguous(TensorDType::of<float>(), { 4 }));
        ASSERT_TRUE(zeros.has_value());
        EXPECT_EQ(floats_of(*zeros), (std::vector<float> { 0, 0, 0, 0 }));

        auto device = TensorSpec::contiguous(TensorDType::of<float>(), { 4 });
        device.device = { TensorDeviceType::VULKAN, 0 };
        EXPECT_FALSE(TensorData::allocate(device).has_value());
    }

    TEST(TensorDataTest, CopiesAreDeep)
    {
        auto original = make_floats({ 3 });
        ASSERT_TRUE(original.has_value());
        const TensorData copy = *original;

        original->storage()[0] = std::byte { 0xFF };
        EXPECT_EQ(floats_of(copy), (std::vector<float> { 0, 1, 2 }));
        EXPECT_NE(copy.storage().data(), original->storage().data());
    }

    TEST(TensorDataTest, DefaultTensorIsInvalidAndHasNoAccess)
    {
        const TensorData nothing;
        EXPECT_FALSE(nothing.is_valid());
        EXPECT_FALSE(nothing.access().has_value());
    }

    TEST(TensorDataTest, EmptyTensorIsValid)
    {
        const auto empty = TensorData::allocate(TensorSpec::contiguous(TensorDType::of<float>(), { 0, 4 }));
        ASSERT_TRUE(empty.has_value());
        EXPECT_TRUE(empty->is_valid());
        EXPECT_EQ(empty->to_vector<float>(), std::optional<std::vector<float>> { std::vector<float> {} });
    }

    TEST(TensorDataTest, UnconventionalElementTypesNeedNoDataVariant)
    {
        std::vector<int64_t> wide { -1, 0, 9'000'000'000LL };
        const auto integers = TensorData::from_values<int64_t>(wide);
        ASSERT_TRUE(integers.has_value());
        const auto view = integers->access();
        ASSERT_TRUE(view.has_value());
        const auto span = view->as<int64_t>();
        ASSERT_EQ(span.size(), 3U);
        EXPECT_EQ(span[2], 9'000'000'000LL);
        EXPECT_TRUE(view->as<double>().empty());
        EXPECT_FALSE(integers->to_variant().has_value());

        std::vector<std::byte> half_bytes(8);
        const auto half = TensorData::from_bytes(
            TensorSpec::contiguous(TensorDType::float16(), { 4 }), half_bytes);
        ASSERT_TRUE(half.has_value());
        const auto half_view = half->access();
        ASSERT_TRUE(half_view.has_value());
        EXPECT_EQ(half_view->bytes().size(), 8U);
        EXPECT_TRUE(half_view->as<float>().empty());
        EXPECT_TRUE(half_view->as<uint16_t>().empty());
        EXPECT_EQ(half_view->gpu_view().format, std::optional { GpuDataFormat::UINT16 });
    }

    TEST(TensorDataTest, SubByteStorageRoundsUpToWholeBytes)
    {
        const auto tensor = TensorData::allocate(
            TensorSpec::contiguous(TensorDType { TensorDTypeKind::FLOAT4_E2M1FN, 4, 1 }, { 5 }));
        ASSERT_TRUE(tensor.has_value());
        EXPECT_EQ(tensor->byte_size(), 3U);
    }

    TEST(TensorDataTest, ScalarVariantsRoundTrip)
    {
        expect_variant_round_trip<double>({ 1.5, -2.0 });
        expect_variant_round_trip<float>({ 1.5F, -2.0F, 3.0F });
        expect_variant_round_trip<uint8_t>({ 1, 2, 255 });
        expect_variant_round_trip<uint16_t>({ 1, 65535 });
        expect_variant_round_trip<uint32_t>({ 7, 4'000'000'000U });
        expect_variant_round_trip<std::complex<float>>({ { 1, 2 }, { 3, -4 } });
        expect_variant_round_trip<std::complex<double>>({ { 1, 2 } });
    }

    TEST(TensorDataTest, VectorVariantsBecomeComponentAxes)
    {
        const std::vector<glm::vec3> points { { 1, 2, 3 }, { 4, 5, 6 } };
        const auto tensor = TensorData::from_variant(DataVariant { points });
        ASSERT_TRUE(tensor.has_value());
        EXPECT_EQ(tensor->spec().shape, (std::vector<uint64_t> { 2, 3 }));
        EXPECT_TRUE(tensor->spec().dtype.is<float>());
        EXPECT_EQ(tensor->to_vector<glm::vec3>(), std::optional { points });
        EXPECT_EQ(floats_of(*tensor), (std::vector<float> { 1, 2, 3, 4, 5, 6 }));
    }

    TEST(TensorDataTest, MatrixVariantsBecomeSquareAxes)
    {
        const std::vector<glm::mat4> matrices { glm::mat4(1.0F), glm::mat4(2.0F) };
        const auto tensor = TensorData::from_variant(DataVariant { matrices });
        ASSERT_TRUE(tensor.has_value());
        EXPECT_EQ(tensor->spec().shape, (std::vector<uint64_t> { 2, 4, 4 }));
        EXPECT_EQ(tensor->to_vector<glm::mat4>(), std::optional { matrices });
    }

    TEST(TensorDataTest, VariantBridgeRefusesMismatches)
    {
        const std::vector<double> wide { 1, 2, 3, 4 };
        const auto tensor = TensorData::from_variant(DataVariant { wide }, { 2, 2 });
        ASSERT_TRUE(tensor.has_value());
        EXPECT_FALSE(tensor->to_vector<float>().has_value());
        EXPECT_FALSE(tensor->to_vector<glm::vec4>().has_value());
        EXPECT_TRUE(tensor->to_vector<double>().has_value());

        EXPECT_FALSE(TensorData::from_variant(DataVariant { wide }, { 3, 2 }).has_value());
    }

    TEST(TensorDataTest, StridedTensorsDoNotBridgeToFlatVectors)
    {
        const auto base = make_floats({ 2, 3 });
        ASSERT_TRUE(base.has_value());
        auto spec = base->spec();
        spec.shape = { 3, 2 };
        spec.strides = { 1, 3 };
        const auto view = view_of(*base, spec);
        ASSERT_TRUE(view.has_value());
        EXPECT_TRUE(view->bytes().empty());
        EXPECT_TRUE(view->as<float>().empty());
        EXPECT_EQ(view->gpu_view().data, nullptr);
    }

    TEST(TensorAccessTest, ViewsBeyondTheirStorageAreRefused)
    {
        std::vector<std::byte> small(16);
        const auto spec = TensorSpec::contiguous(TensorDType::of<float>(), { 2, 3 });
        EXPECT_FALSE(Kakshya::as_tensor_access(small, spec).has_value());
    }

    TEST(TensorAccessTest, DeviceTensorsAreNotHostViewable)
    {
        std::vector<std::byte> storage(24);
        auto spec = TensorSpec::contiguous(TensorDType::of<float>(), { 6 });
        spec.device = { TensorDeviceType::CUDA, 0 };
        EXPECT_FALSE(Kakshya::as_tensor_access(storage, spec).has_value());

        spec.device = { TensorDeviceType::CUDA_HOST, 0 };
        EXPECT_TRUE(Kakshya::as_tensor_access(storage, spec).has_value());
    }

    TEST(TensorAccessTest, GpuViewReportsPointerBytesAndFormat)
    {
        const auto tensor = make_floats({ 4 });
        ASSERT_TRUE(tensor.has_value());
        const auto view = tensor->access();
        ASSERT_TRUE(view.has_value());
        const auto upload = view->gpu_view();
        EXPECT_EQ(upload.data, tensor->storage().data());
        EXPECT_EQ(upload.bytes, 16U);
        EXPECT_EQ(upload.format, std::optional { GpuDataFormat::FLOAT32 });
    }

    // ---------------------------------------------------------------------
    // copy_of over strided layouts
    // ---------------------------------------------------------------------

    struct GatherCase {
        const char* name;
        std::vector<uint64_t> shape;
        std::vector<int64_t> strides;
        uint64_t byte_offset;
        std::vector<float> expected;
    };

    class TensorGatherTest : public ::testing::TestWithParam<GatherCase> { };

    INSTANTIATE_TEST_SUITE_P(Layouts, TensorGatherTest,
        ::testing::Values(
            GatherCase { "Transposed", { 3, 2 }, { 1, 3 }, 0, { 0, 3, 1, 4, 2, 5 } },
            GatherCase { "FortranOrder", { 2, 3 }, { 1, 2 }, 0, { 0, 2, 4, 1, 3, 5 } },
            GatherCase { "Reversed", { 6 }, { -1 }, 20, { 5, 4, 3, 2, 1, 0 } },
            GatherCase { "Broadcast", { 2, 3 }, { 0, 1 }, 0, { 0, 1, 2, 0, 1, 2 } },
            GatherCase { "EveryOther", { 3 }, { 2 }, 0, { 0, 2, 4 } },
            GatherCase { "OffsetDense", { 2, 2 }, {}, 8, { 2, 3, 4, 5 } }),
        [](const ::testing::TestParamInfo<GatherCase>& info) { return std::string(info.param.name); });

    TEST_P(TensorGatherTest, CopyIsDenseCOrderOfTheLogicalElements)
    {
        const auto& param = GetParam();
        const auto base = make_floats({ 6 });
        ASSERT_TRUE(base.has_value());

        auto spec = base->spec();
        spec.shape = param.shape;
        spec.strides = param.strides;
        spec.byte_offset = param.byte_offset;

        const auto view = view_of(*base, spec);
        ASSERT_TRUE(view.has_value());

        const auto copy = TensorData::copy_of(*view);
        ASSERT_TRUE(copy.has_value());
        EXPECT_EQ(floats_of(*copy), param.expected);
        EXPECT_EQ(copy->spec().shape, param.shape);
        EXPECT_TRUE(copy->spec().strides.empty());
        EXPECT_EQ(copy->spec().byte_offset, 0U);
        EXPECT_TRUE(copy->spec().is_contiguous());
    }

    // ---------------------------------------------------------------------
    // TensorSet
    // ---------------------------------------------------------------------

    TEST(TensorSetTest, FindsTensorsByExactName)
    {
        TensorSet set;
        set.tensors.push_back(*make_floats({ 2 }, "a"));
        set.tensors.push_back(*make_floats({ 3 }, "b"));
        set.metadata["format"] = "pt";

        ASSERT_NE(set.find("b"), nullptr);
        EXPECT_EQ(set.find("b")->spec().shape, (std::vector<uint64_t> { 3 }));
        EXPECT_EQ(set.find("c"), nullptr);
        EXPECT_TRUE(set.is_consistent());
    }

    TEST(TensorSetTest, DuplicateUnnamedAndInvalidTensorsAreInconsistent)
    {
        TensorSet duplicate;
        duplicate.tensors.push_back(*make_floats({ 2 }, "a"));
        duplicate.tensors.push_back(*make_floats({ 2 }, "a"));
        EXPECT_FALSE(duplicate.is_consistent());

        TensorSet unnamed;
        unnamed.tensors.push_back(*make_floats({ 2 }, ""));
        EXPECT_FALSE(unnamed.is_consistent());

        TensorSet invalid;
        invalid.tensors.emplace_back();
        EXPECT_FALSE(invalid.is_consistent());
    }

    // ---------------------------------------------------------------------
    // TensorInsertion
    // ---------------------------------------------------------------------

    TEST(TensorInsertionTest, WriteReplacesOnlyTheRequestedElements)
    {
        auto tensor = TensorData::allocate(TensorSpec::contiguous(TensorDType::of<float>(), { 5 }));
        ASSERT_TRUE(tensor.has_value());
        TensorInsertion insert(*tensor);

        const std::vector<float> values { 7, 8 };
        EXPECT_TRUE(insert.write<float>(values, 2));
        EXPECT_EQ(floats_of(*tensor), (std::vector<float> { 0, 0, 7, 8, 0 }));
    }

    TEST(TensorInsertionTest, WriteRefusesWrongTypeAndOverrun)
    {
        auto tensor = make_floats({ 4 });
        ASSERT_TRUE(tensor.has_value());
        TensorInsertion insert(*tensor);

        const std::vector<double> wide { 1, 2 };
        EXPECT_FALSE(insert.write<double>(wide));

        const std::vector<float> three { 1, 2, 3 };
        EXPECT_FALSE(insert.write<float>(three, 2));
        EXPECT_EQ(floats_of(*tensor), (std::vector<float> { 0, 1, 2, 3 }));
    }

    TEST(TensorInsertionTest, AssignGathersAStridedSourceIntoADenseTensor)
    {
        const auto base = make_floats({ 2, 3 });
        ASSERT_TRUE(base.has_value());
        auto source_spec = base->spec();
        source_spec.shape = { 3, 2 };
        source_spec.strides = { 1, 3 };
        const auto source = view_of(*base, source_spec);
        ASSERT_TRUE(source.has_value());

        auto destination = TensorData::allocate(TensorSpec::contiguous(TensorDType::of<float>(), { 3, 2 }));
        ASSERT_TRUE(destination.has_value());
        TensorInsertion insert(*destination);

        EXPECT_TRUE(insert.assign(*source));
        EXPECT_EQ(floats_of(*destination), (std::vector<float> { 0, 3, 1, 4, 2, 5 }));
    }

    TEST(TensorInsertionTest, AssignScattersIntoAStridedDestination)
    {
        auto spec = TensorSpec::contiguous(TensorDType::of<float>(), { 2, 3 });
        spec.strides = { 1, 2 };
        auto destination = TensorData::allocate(spec);
        ASSERT_TRUE(destination.has_value());

        const auto source_tensor = make_floats({ 2, 3 });
        ASSERT_TRUE(source_tensor.has_value());
        const auto source = source_tensor->access();
        ASSERT_TRUE(source.has_value());

        TensorInsertion insert(*destination);
        EXPECT_TRUE(insert.assign(*source));

        std::vector<float> raw(6);
        std::memcpy(raw.data(), destination->storage().data(), 24);
        EXPECT_EQ(raw, (std::vector<float> { 0, 3, 1, 4, 2, 5 }));

        const auto copy = TensorData::copy_of(*destination->access());
        ASSERT_TRUE(copy.has_value());
        EXPECT_EQ(floats_of(*copy), (std::vector<float> { 0, 1, 2, 3, 4, 5 }));
    }

    TEST(TensorInsertionTest, AssignRefusesDifferentShapeOrType)
    {
        auto destination = make_floats({ 2, 3 });
        ASSERT_TRUE(destination.has_value());
        TensorInsertion insert(*destination);

        const auto other_shape = make_floats({ 3, 2 });
        EXPECT_FALSE(insert.assign(*other_shape->access()));

        std::vector<double> wide(6, 1.0);
        const auto other_type = TensorData::from_values<double>(wide, { 2, 3 });
        EXPECT_FALSE(insert.assign(*other_type->access()));
    }

    TEST(TensorInsertionTest, AssignWindowWritesOnlyTheBox)
    {
        auto destination = TensorData::allocate(TensorSpec::contiguous(TensorDType::of<float>(), { 3, 4 }));
        ASSERT_TRUE(destination.has_value());
        const auto patch = make_floats({ 2, 2 });
        ASSERT_TRUE(patch.has_value());

        TensorInsertion insert(*destination);
        EXPECT_TRUE(insert.assign_window(Region({ 1, 1 }, { 2, 2 }), *patch->access()));

        EXPECT_EQ(floats_of(*destination), (std::vector<float> { 0, 0, 0, 0, 0, 0, 1, 0, 0, 2, 3, 0 }));
    }

    TEST(TensorInsertionTest, AssignWindowScattersIntoAStridedDestination)
    {
        auto spec = TensorSpec::contiguous(TensorDType::of<float>(), { 3, 4 });
        spec.strides = { 1, 3 };
        auto destination = TensorData::allocate(spec);
        ASSERT_TRUE(destination.has_value());
        const auto patch = make_floats({ 2, 2 });
        ASSERT_TRUE(patch.has_value());

        TensorInsertion insert(*destination);
        EXPECT_TRUE(insert.assign_window(Region({ 1, 1 }, { 2, 2 }), *patch->access()));

        const auto logical = TensorData::copy_of(*destination->access());
        ASSERT_TRUE(logical.has_value());
        EXPECT_EQ(floats_of(*logical), (std::vector<float> { 0, 0, 0, 0, 0, 0, 1, 0, 0, 2, 3, 0 }));
    }

    TEST(TensorInsertionTest, AssignWindowAcceptsASourceThatOverlapsTheDestination)
    {
        auto destination = make_floats({ 4, 4 });
        ASSERT_TRUE(destination.has_value());
        const auto source_spec = destination->spec().window(Region({ 1, 1 }, { 2, 2 }));
        ASSERT_TRUE(source_spec.has_value());
        const auto source = view_of(*destination, *source_spec);
        ASSERT_TRUE(source.has_value());

        TensorInsertion insert(*destination);
        EXPECT_TRUE(insert.assign_window(Region({ 2, 2 }, { 3, 3 }), *source));

        std::vector<float> expected(16);
        std::iota(expected.begin(), expected.end(), 0.0F);
        expected[10] = 5;
        expected[11] = 6;
        expected[14] = 9;
        expected[15] = 10;
        EXPECT_EQ(floats_of(*destination), expected);
    }

    TEST(TensorInsertionTest, AssignWindowRefusesAndLeavesTheTensorAloneOtherwise)
    {
        auto destination = make_floats({ 3, 4 });
        ASSERT_TRUE(destination.has_value());
        const auto before = floats_of(*destination);
        TensorInsertion insert(*destination);

        const auto wrong_shape = make_floats({ 2, 3 });
        EXPECT_FALSE(insert.assign_window(Region({ 0, 0 }, { 1, 1 }), *wrong_shape->access()));

        std::vector<double> wide(4, 1.0);
        const auto wrong_type = TensorData::from_values<double>(wide, { 2, 2 });
        EXPECT_FALSE(insert.assign_window(Region({ 0, 0 }, { 1, 1 }), *wrong_type->access()));

        const auto patch = make_floats({ 2, 2 });
        EXPECT_FALSE(insert.assign_window(Region({ 2, 3 }, { 3, 4 }), *patch->access()));
        EXPECT_FALSE(insert.assign_window(Region {}, *patch->access()));

        EXPECT_EQ(floats_of(*destination), before);
    }

    TEST(TensorInsertionTest, AssignWindowOnASubByteTensorNeedsTheWholeTensor)
    {
        const auto dtype = TensorDType { TensorDTypeKind::FLOAT4_E2M1FN, 4, 1 };
        auto destination = TensorData::allocate(TensorSpec::contiguous(dtype, { 4, 4 }));
        ASSERT_TRUE(destination.has_value());
        const std::vector<std::byte> pattern { std::byte { 0x21 }, std::byte { 0x43 }, std::byte { 0x65 }, std::byte { 0x07 },
            std::byte { 0x18 }, std::byte { 0x29 }, std::byte { 0x3A }, std::byte { 0x4B } };
        const auto whole = TensorData::from_bytes(TensorSpec::contiguous(dtype, { 4, 4 }), pattern);
        ASSERT_TRUE(whole.has_value());
        const auto part = TensorData::allocate(TensorSpec::contiguous(dtype, { 2, 2 }));
        ASSERT_TRUE(part.has_value());

        TensorInsertion insert(*destination);
        EXPECT_FALSE(insert.assign_window(Region({ 0, 0 }, { 1, 1 }), *part->access()));
        EXPECT_TRUE(insert.assign_window(Region({ 0, 0 }, { 3, 3 }), *whole->access()));

        const auto stored = destination->storage();
        ASSERT_EQ(stored.size(), pattern.size());
        EXPECT_TRUE(std::equal(stored.begin(), stored.end(), pattern.begin()));
    }

    TEST(TensorInsertionTest, ReshapeKeepsDataAndRefusesAChangedCount)
    {
        auto tensor = make_floats({ 2, 3 });
        ASSERT_TRUE(tensor.has_value());
        TensorInsertion insert(*tensor);

        EXPECT_TRUE(insert.reshape({ 3, 2 }));
        EXPECT_EQ(tensor->spec().shape, (std::vector<uint64_t> { 3, 2 }));
        EXPECT_EQ(floats_of(*tensor), (std::vector<float> { 0, 1, 2, 3, 4, 5 }));

        EXPECT_FALSE(insert.reshape({ 4, 2 }));
        EXPECT_EQ(tensor->spec().shape, (std::vector<uint64_t> { 3, 2 }));
    }

    TEST(TensorInsertionTest, ReshapeRefusesStridedTensors)
    {
        auto spec = TensorSpec::contiguous(TensorDType::of<float>(), { 2, 3 });
        spec.strides = { 1, 2 };
        auto tensor = TensorData::allocate(spec);
        ASSERT_TRUE(tensor.has_value());
        TensorInsertion insert(*tensor);
        EXPECT_FALSE(insert.reshape({ 6 }));
    }

    TEST(TensorInsertionTest, ResizeKeepsLeadingElementsAndZeroFillsTheRest)
    {
        auto tensor = make_floats({ 2, 3 });
        ASSERT_TRUE(tensor.has_value());
        TensorInsertion insert(*tensor);

        EXPECT_TRUE(insert.resize({ 3, 3 }));
        EXPECT_EQ(tensor->spec().shape, (std::vector<uint64_t> { 3, 3 }));
        EXPECT_EQ(floats_of(*tensor), (std::vector<float> { 0, 1, 2, 3, 4, 5, 0, 0, 0 }));

        EXPECT_TRUE(insert.resize({ 2 }));
        EXPECT_EQ(floats_of(*tensor), (std::vector<float> { 0, 1 }));
    }

    TEST(TensorInsertionTest, AxesAndQuantizationAreValidatedBeforeTheyStick)
    {
        auto tensor = TensorData::allocate(TensorSpec::contiguous(TensorDType::of<int8_t>(), { 4, 3 }, "q"));
        ASSERT_TRUE(tensor.has_value());
        TensorInsertion insert(*tensor);

        EXPECT_FALSE(insert.set_axes({ DataDimension::channel(4) }));
        EXPECT_TRUE(insert.set_axes({ DataDimension("batch", 4), DataDimension::channel(3) }));
        EXPECT_EQ(tensor->spec().dimensions()[1].role, DataDimension::Role::CHANNEL);

        EXPECT_FALSE(insert.set_quantization(TensorQuantization { .scale = { 1, 2 }, .zero_point = {}, .axis = 0 }));
        EXPECT_FALSE(tensor->spec().quantization.has_value());

        EXPECT_TRUE(insert.set_quantization(TensorQuantization { .scale = { 0.1 }, .zero_point = { 3 }, .axis = std::nullopt }));
        EXPECT_TRUE(tensor->spec().quantization.has_value());
        EXPECT_TRUE(insert.set_quantization(std::nullopt));
        EXPECT_FALSE(tensor->spec().quantization.has_value());

        insert.set_name("renamed");
        EXPECT_EQ(tensor->spec().name, "renamed");
    }

    // ---------------------------------------------------------------------
    // DLPack
    // ---------------------------------------------------------------------

    int g_releases = 0;

    void count_release(DLManagedTensorVersioned*)
    {
        ++g_releases;
    }

    void count_release_legacy(DLManagedTensor*)
    {
        ++g_releases;
    }

    struct ManualTensor {
        std::vector<float> data;
        std::vector<int64_t> shape;
        std::vector<int64_t> strides;
        DLManagedTensorVersioned managed {};

        ManualTensor(std::vector<float> values, std::vector<int64_t> extents, std::vector<int64_t> steps)
            : data(std::move(values))
            , shape(std::move(extents))
            , strides(std::move(steps))
        {
            managed.version = { DLPACK_MAJOR_VERSION, DLPACK_MINOR_VERSION };
            managed.deleter = count_release;
            managed.dl_tensor.data = data.data();
            managed.dl_tensor.device = { kDLCPU, 0 };
            managed.dl_tensor.ndim = static_cast<int32_t>(shape.size());
            managed.dl_tensor.dtype = { kDLFloat, 32, 1 };
            managed.dl_tensor.shape = shape.data();
            managed.dl_tensor.strides = strides.empty() ? nullptr : strides.data();
        }
    };

    struct ManualLegacyTensor {
        std::vector<float> data;
        std::vector<int64_t> shape;
        std::vector<int64_t> strides;
        DLManagedTensor managed {};

        ManualLegacyTensor(std::vector<float> values, std::vector<int64_t> extents, std::vector<int64_t> steps)
            : data(std::move(values))
            , shape(std::move(extents))
            , strides(std::move(steps))
        {
            managed.deleter = count_release_legacy;
            managed.dl_tensor.data = data.data();
            managed.dl_tensor.device = { kDLCPU, 0 };
            managed.dl_tensor.ndim = static_cast<int32_t>(shape.size());
            managed.dl_tensor.dtype = { kDLFloat, 32, 1 };
            managed.dl_tensor.shape = shape.data();
            managed.dl_tensor.strides = strides.empty() ? nullptr : strides.data();
        }
    };

    class TensorDLPackTest : public ::testing::Test {
    protected:
        void SetUp() override { g_releases = 0; }
    };

    TEST_F(TensorDLPackTest, ExportDescribesTheTensorAndKeepsItAlive)
    {
        auto owner = std::make_shared<const TensorData>(std::move(*make_floats({ 2, 3 })));
        auto* managed = Kakshya::export_dlpack(owner);
        ASSERT_NE(managed, nullptr);

        EXPECT_EQ(owner.use_count(), 2);
        EXPECT_EQ(managed->version.major, static_cast<uint32_t>(DLPACK_MAJOR_VERSION));
        EXPECT_NE(managed->flags & DLPACK_FLAG_BITMASK_READ_ONLY, 0U);

        const auto& tensor = managed->dl_tensor;
        EXPECT_EQ(tensor.ndim, 2);
        EXPECT_EQ(tensor.shape[0], 2);
        EXPECT_EQ(tensor.shape[1], 3);
        EXPECT_EQ(tensor.strides[0], 3);
        EXPECT_EQ(tensor.strides[1], 1);
        EXPECT_EQ(tensor.dtype.code, static_cast<uint8_t>(kDLFloat));
        EXPECT_EQ(tensor.dtype.bits, 32);
        EXPECT_EQ(tensor.dtype.lanes, 1);
        EXPECT_EQ(tensor.device.device_type, kDLCPU);
        EXPECT_EQ(tensor.data, owner->storage().data());
        EXPECT_EQ(tensor.byte_offset, 0U);

        managed->deleter(managed);
        EXPECT_EQ(owner.use_count(), 1);
    }

    TEST_F(TensorDLPackTest, ExportCarriesHalfPrecisionAndOffsets)
    {
        std::vector<std::byte> bytes(16);
        auto spec = TensorSpec::contiguous(TensorDType::float16(), { 4 });
        spec.byte_offset = 8;
        bytes.resize(16);
        auto owner = std::make_shared<const TensorData>(std::move(*TensorData::from_bytes(spec, bytes)));
        auto* managed = Kakshya::export_dlpack(owner);
        ASSERT_NE(managed, nullptr);
        EXPECT_EQ(managed->dl_tensor.dtype.bits, 16);
        EXPECT_EQ(managed->dl_tensor.byte_offset, 8U);
        managed->deleter(managed);
    }

    TEST_F(TensorDLPackTest, ExportOfNothingIsNull)
    {
        EXPECT_EQ(Kakshya::export_dlpack(nullptr), nullptr);
        EXPECT_EQ(Kakshya::export_dlpack(std::make_shared<const TensorData>()), nullptr);
    }

    TEST_F(TensorDLPackTest, ExportOfAnEmptyTensorHasNoDataPointer)
    {
        auto owner = std::make_shared<const TensorData>(
            std::move(*TensorData::allocate(TensorSpec::contiguous(TensorDType::of<float>(), { 0 }))));
        auto* managed = Kakshya::export_dlpack(owner);
        ASSERT_NE(managed, nullptr);
        EXPECT_EQ(managed->dl_tensor.data, nullptr);
        managed->deleter(managed);
    }

    TEST_F(TensorDLPackTest, ImportOfAnExportIsZeroCopyAndReleasesTheOwner)
    {
        auto owner = std::make_shared<const TensorData>(std::move(*make_floats({ 2, 3 }, "x")));
        {
            auto imported = TensorImport::take(Kakshya::export_dlpack(owner));
            ASSERT_TRUE(imported.has_value());
            EXPECT_TRUE(imported->read_only());
            EXPECT_EQ(imported->spec().shape, (std::vector<uint64_t> { 2, 3 }));
            EXPECT_TRUE(imported->spec().dtype.is<float>());

            const auto view = imported->access();
            ASSERT_TRUE(view.has_value());
            EXPECT_EQ(view->element_data(), owner->storage().data());
            EXPECT_EQ(view->as<float>().size(), 6U);

            const auto copy = imported->copy();
            ASSERT_TRUE(copy.has_value());
            EXPECT_NE(copy->storage().data(), owner->storage().data());
            EXPECT_EQ(floats_of(*copy), (std::vector<float> { 0, 1, 2, 3, 4, 5 }));
            EXPECT_EQ(owner.use_count(), 2);
        }
        EXPECT_EQ(owner.use_count(), 1);
    }

    TEST_F(TensorDLPackTest, ForeignStridedTensorsCopyIntoCOrder)
    {
        ManualTensor foreign({ 0, 1, 2, 3 }, { 2, 2 }, { 1, 2 });
        {
            auto imported = TensorImport::take(&foreign.managed);
            ASSERT_TRUE(imported.has_value());
            EXPECT_FALSE(imported->read_only());
            const auto copy = imported->copy();
            ASSERT_TRUE(copy.has_value());
            EXPECT_EQ(floats_of(*copy), (std::vector<float> { 0, 2, 1, 3 }));
            EXPECT_EQ(g_releases, 0);
        }
        EXPECT_EQ(g_releases, 1);
    }

    TEST_F(TensorDLPackTest, MissingStridesMeanContiguous)
    {
        ManualTensor foreign({ 1, 2, 3, 4 }, { 2, 2 }, {});
        auto imported = TensorImport::take(&foreign.managed);
        ASSERT_TRUE(imported.has_value());
        EXPECT_EQ(floats_of(*imported->copy()), (std::vector<float> { 1, 2, 3, 4 }));
    }

    TEST_F(TensorDLPackTest, ByteOffsetLocatesElementZero)
    {
        ManualTensor foreign({ 9, 9, 5, 6 }, { 2 }, { 1 });
        foreign.managed.dl_tensor.byte_offset = 8;
        auto imported = TensorImport::take(&foreign.managed);
        ASSERT_TRUE(imported.has_value());
        EXPECT_EQ(floats_of(*imported->copy()), (std::vector<float> { 5, 6 }));
    }

    TEST_F(TensorDLPackTest, LegacyTensorsImportToo)
    {
        ManualLegacyTensor foreign({ 1, 2, 3 }, { 3 }, { 1 });
        {
            auto imported = TensorImport::take(&foreign.managed);
            ASSERT_TRUE(imported.has_value());
            EXPECT_EQ(floats_of(*imported->copy()), (std::vector<float> { 1, 2, 3 }));
        }
        EXPECT_EQ(g_releases, 1);
    }

    TEST_F(TensorDLPackTest, TheDeleterRunsOnceAcrossMoves)
    {
        ManualTensor foreign({ 1, 2 }, { 2 }, { 1 });
        {
            auto first = TensorImport::take(&foreign.managed);
            ASSERT_TRUE(first.has_value());
            TensorImport second = std::move(*first);
            TensorImport third = std::move(second);
            EXPECT_EQ(g_releases, 0);
            EXPECT_TRUE(third.access().has_value());
        }
        EXPECT_EQ(g_releases, 1);
    }

    TEST_F(TensorDLPackTest, RejectedTensorsAreStillReleased)
    {
        ManualTensor wrong_version({ 1, 2 }, { 2 }, { 1 });
        wrong_version.managed.version.major = DLPACK_MAJOR_VERSION + 1;
        EXPECT_FALSE(TensorImport::take(&wrong_version.managed).has_value());
        EXPECT_EQ(g_releases, 1);

        ManualTensor null_data({ 1, 2 }, { 2 }, { 1 });
        null_data.managed.dl_tensor.data = nullptr;
        EXPECT_FALSE(TensorImport::take(&null_data.managed).has_value());
        EXPECT_EQ(g_releases, 2);

        ManualTensor device({ 1, 2 }, { 2 }, { 1 });
        device.managed.dl_tensor.device = { kDLVulkan, 0 };
        EXPECT_FALSE(TensorImport::take(&device.managed).has_value());
        EXPECT_EQ(g_releases, 3);

        ManualTensor padded({ 1, 2 }, { 2 }, { 1 });
        padded.managed.flags = DLPACK_FLAG_BITMASK_IS_SUBBYTE_TYPE_PADDED;
        EXPECT_FALSE(TensorImport::take(&padded.managed).has_value());
        EXPECT_EQ(g_releases, 4);

        ManualTensor negative({ 1, 2 }, { -2 }, { 1 });
        EXPECT_FALSE(TensorImport::take(&negative.managed).has_value());
        EXPECT_EQ(g_releases, 5);

        ManualTensor bad_type({ 1, 2 }, { 2 }, { 1 });
        bad_type.managed.dl_tensor.dtype.code = 200;
        EXPECT_FALSE(TensorImport::take(&bad_type.managed).has_value());
        EXPECT_EQ(g_releases, 6);
    }

    TEST_F(TensorDLPackTest, NullPointersAreRefusedWithoutACallback)
    {
        EXPECT_FALSE(TensorImport::take(static_cast<DLManagedTensorVersioned*>(nullptr)).has_value());
        EXPECT_FALSE(TensorImport::take(static_cast<DLManagedTensor*>(nullptr)).has_value());
        EXPECT_EQ(g_releases, 0);
    }

} // namespace
} // namespace MayaFlux::Test
