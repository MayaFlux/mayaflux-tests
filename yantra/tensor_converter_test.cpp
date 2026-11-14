#include "MayaFlux/Yantra/ComputeMatrix.hpp"
#include "MayaFlux/Yantra/Transformers/TensorConverter.hpp"

#include <gtest/gtest.h>
#include "../support/test_metadata.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using namespace Yantra;
    using Kakshya::TensorContainer;
    using Kakshya::TensorData;
    using Kakshya::TensorDType;
    using Kakshya::TensorQuantization;
    using TensorPtr = std::shared_ptr<TensorContainer>;

    template <typename T>
    TensorPtr container_of(const std::vector<T>& values, std::vector<uint64_t> shape = {}, std::string name = "weights")
    {
        auto tensor = TensorData::from_values<T>(values, std::move(shape), std::move(name));
        return std::make_shared<TensorContainer>(std::move(*tensor));
    }

    template <typename T>
    std::vector<T> values_of(const TensorContainer& container)
    {
        const auto view = container.view();
        if (!view)
            return {};
        const auto span = view->template as<T>();
        return { span.begin(), span.end() };
    }

    std::shared_ptr<TensorConverter> converter(TensorDType target,
        std::optional<TensorQuantization> quantization = std::nullopt)
    {
        TensorConversionConfig config;
        config.target = target;
        config.quantization = std::move(quantization);
        auto operation = std::make_shared<TensorConverter>();
        operation->set_config(config);
        return operation;
    }

    std::string message_of(const std::function<void()>& action)
    {
        try {
            action();
        } catch (const std::exception& e) {
            return e.what();
        }
        return {};
    }

    TEST(TensorConverterTest, TheDefaultTargetIsFloat32)
    {
        const auto source = container_of<int16_t>({ 1, -2, 3, -4 }, { 2, 2 });
        TensorConverter operation;

        const auto result = operation.apply_operation(TensorContainerIO { source });

        ASSERT_TRUE(result.data != nullptr);
        EXPECT_EQ(result.data->get_data().spec().dtype, TensorDType::of<float>());
        EXPECT_EQ(result.data->get_data().spec().shape, (std::vector<uint64_t> { 2, 2 }));
        EXPECT_EQ(values_of<float>(*result.data), (std::vector<float> { 1, -2, 3, -4 }));
    }

    TEST(TensorConverterTest, TheInputContainerIsLeftAlone)
    {
        const auto source = container_of<int16_t>({ 1, 2, 3 });
        const auto operation = converter(TensorDType::float16());

        const auto result = operation->apply_operation(TensorContainerIO { source });

        EXPECT_NE(result.data, source);
        EXPECT_EQ(source->get_data().spec().dtype, TensorDType::of<int16_t>());
        EXPECT_EQ(values_of<int16_t>(*source), (std::vector<int16_t> { 1, 2, 3 }));
    }

    TEST(TensorConverterTest, AConfiguredQuantizationIsStoredWithTheResult)
    {
        const auto source = container_of<float>({ 0.0F, 0.25F, 0.375F, 100.0F });
        const TensorQuantization request { .scale = { 0.25 }, .zero_point = { 10 }, .axis = std::nullopt };
        const auto operation = converter(TensorDType::of<int8_t>(), request);

        const auto result = operation->apply_operation(TensorContainerIO { source });

        EXPECT_EQ(values_of<int8_t>(*result.data), (std::vector<int8_t> { 10, 11, 12, 127 }));
        const auto& spec = result.data->get_data().spec();
        ASSERT_TRUE(spec.quantization.has_value());
        EXPECT_EQ(spec.quantization->scale, request.scale);
        EXPECT_EQ(spec.quantization->zero_point, request.zero_point);
    }

    TEST(TensorConverterTest, AQuantizedSourceIsDequantized)
    {
        const auto source = container_of<int8_t>({ -3, -1, 0, 5 });
        ASSERT_TRUE(source->insert().set_quantization(TensorQuantization { .scale = { 0.5 }, .zero_point = { -3 }, .axis = std::nullopt }));
        const auto operation = converter(TensorDType::of<float>());

        const auto result = operation->apply_operation(TensorContainerIO { source });

        EXPECT_EQ(values_of<float>(*result.data), (std::vector<float> { 0.0F, 1.0F, 1.5F, 4.0F }));
        EXPECT_FALSE(result.data->get_data().spec().quantization.has_value());
    }

    TEST(TensorConverterTest, NameAxesFacetsAndMetadataTravelWithTheResult)
    {
        const auto source = container_of<float>({ 1, 2, 3, 4, 5, 6 }, { 2, 3 }, "kernel");
        ASSERT_TRUE(source->insert().set_axes({ Kakshya::DataDimension::time(2), Kakshya::DataDimension::channel(3) }));
        const Facet<int> marker;
        TensorContainerIO input(source);
        input.facets.set(marker, 7);
        input.set_metadata("origin", std::string("weights.safetensors"));
        const auto operation = converter(TensorDType::bfloat16());

        const auto result = operation->apply_operation(input);

        const auto& spec = result.data->get_data().spec();
        EXPECT_EQ(spec.name, "kernel");
        ASSERT_EQ(spec.axes.size(), 2U);
        EXPECT_EQ(spec.axes[1].role, Kakshya::DataDimension::Role::CHANNEL);
        ASSERT_NE(result.facets.get(marker), nullptr);
        EXPECT_EQ(*result.facets.get(marker), 7);
        EXPECT_EQ(result.get_metadata<std::string>("origin"), std::optional<std::string> { "weights.safetensors" });
    }

    TEST(TensorConverterTest, TheResultDatumDescribesTheNewContainer)
    {
        const auto source = container_of<double>({ 1, 2, 3, 4, 5, 6 }, { 2, 3 });
        const auto operation = converter(TensorDType::of<float>());

        const auto result = operation->apply_operation(TensorContainerIO { source });

        ASSERT_EQ(result.dimensions.size(), 2U);
        EXPECT_EQ(result.dimensions[0].size, 2U);
        EXPECT_EQ(result.dimensions[1].size, 3U);
        EXPECT_EQ(result.modality, Kakshya::DataModality::TENSOR_ND);
    }

    TEST(TensorConverterTest, InputThatTheConfiguredConversionRejectsIsInvalid)
    {
        const auto source = container_of<float>({ 1, 2 });
        const auto supported = converter(TensorDType::float16());
        const auto complex_target = converter(TensorDType::of<std::complex<float>>());
        const auto float8 = converter(Kakshya::TensorDType { .kind = Kakshya::TensorDTypeKind::FLOAT8_E4M3FN, .bits = 8, .lanes = 1 });
        const auto too_wide = converter(TensorDType::of<int64_t>(),
            TensorQuantization { .scale = { 1.0 }, .zero_point = {}, .axis = std::nullopt });

        EXPECT_TRUE(supported->validate_input(TensorContainerIO { source }));
        EXPECT_FALSE(complex_target->validate_input(TensorContainerIO { source }));
        EXPECT_FALSE(float8->validate_input(TensorContainerIO { source }));
        EXPECT_FALSE(too_wide->validate_input(TensorContainerIO { source }));
        EXPECT_FALSE(supported->validate_input(TensorContainerIO {}));
    }

    TEST(TensorConverterTest, ApplyingAnInvalidConversionThrowsTheReason)
    {
        const auto source = container_of<float>({ 1, 2 });
        const auto complex_target = converter(TensorDType::of<std::complex<float>>());
        const auto operation = converter(TensorDType::float16());

        const auto reason = message_of([&] { complex_target->apply_operation(TensorContainerIO { source }); });
        const auto missing = message_of([&] { operation->apply_operation(TensorContainerIO {}); });

        EXPECT_NE(reason.find("complex"), std::string::npos);
        EXPECT_NE(missing.find("no tensor container"), std::string::npos);
    }

    TEST(TensorConverterTest, DescribesItselfAsATransformerWithItsConfig)
    {
        TensorConverter bare;
        const auto configured = converter(TensorDType::float16());

        EXPECT_EQ(bare.get_name(), "TensorConverter");
        EXPECT_EQ(bare.get_operation_type(), OperationType::TRANSFORMER);
        EXPECT_TRUE(bare.configs().empty());
        ASSERT_EQ(configured->configs().size(), 1U);
        EXPECT_EQ(configured->config<TensorConversionConfig>().target, TensorDType::float16());
    }

    TEST(TensorConverterTest, RunsThroughTheComputeMatrix)
    {
        const auto matrix = ComputeMatrix::create();
        const auto operation = matrix->create_operation<TensorConverter>("to_half");
        ASSERT_NE(operation, nullptr);
        TensorConversionConfig config;
        config.target = TensorDType::float16();
        operation->set_config(config);
        const TensorContainerIO input(container_of<float>({ 1, 2, 3 }));

        const auto named = matrix->execute_named<TensorConverter, TensorPtr>("to_half", input);
        const auto fluent = matrix->with(input).then<TensorConverter>("to_half").to_io();

        ASSERT_TRUE(named.has_value());
        EXPECT_EQ(named->data->get_data().spec().dtype, TensorDType::float16());
        EXPECT_EQ(fluent.data->get_data().spec().dtype, TensorDType::float16());
        EXPECT_EQ(values_of<float>(*input.data), (std::vector<float> { 1, 2, 3 }));
    }

    TEST(TensorConverterTest, AMatrixReportsAFailedConversionAsNoResult)
    {
        const auto matrix = ComputeMatrix::create();
        const auto operation = matrix->create_operation<TensorConverter>("bad");
        TensorConversionConfig config;
        config.target = TensorDType::of<std::complex<double>>();
        operation->set_config(config);

        const auto result = matrix->execute_named<TensorConverter, TensorPtr>("bad", TensorContainerIO { container_of<float>({ 1 }) });

        EXPECT_FALSE(result.has_value());
    }

}
}
