#include "MayaFlux/Yantra/Data/DataIO.hpp"
#include "audio_container.hpp"

#include <gtest/gtest.h>

namespace MayaFlux::Test {
namespace {

    using namespace Yantra;
    using Variant = Kakshya::DataVariant;
    using Variants = std::vector<Variant>;
    using Samples = std::vector<double>;
    using Container = std::shared_ptr<Kakshya::SignalSourceContainer>;
    using Dimension = Kakshya::DataDimension;
    using Role = Dimension::Role;
    using Modality = Kakshya::DataModality;

    struct SpecCase {
        std::string name;
        bool compute;
        bool single;
        bool multi;
        bool region;
        bool container;
        bool native;
        bool algorithm;
        bool variant;
        bool ready;
        bool consistent_flags;
        bool expected_single;
        bool expected_multi;
        bool expected_region;
        bool expected_ready;
    };

    template <typename T, typename Native, typename Algorithm, typename Result>
    SpecCase spec(std::string name, bool single, bool multi, bool region, bool ready)
    {
        return { std::move(name), ComputeData<T>, SingleVariant<T>,
            extraction_traits<T>::is_multi_variant, RegionLike<T>,
            RequiresContainer<T> && extraction_traits<T>::requires_container,
            std::same_as<native_result_t<T>, Native>,
            std::same_as<extraction_result_t<T>, Algorithm>
                && std::same_as<typename extraction_traits<T>::algorithm_result_type, Algorithm>,
            std::same_as<variant_result_t<T>, Result>, OperationReadyData<T>,
            extraction_traits_d<T>::requires_container == RequiresContainer<T>
                && extraction_traits<T>::requires_container == RequiresContainer<T>
                && extraction_traits_d<T>::is_region_like == RegionLike<T>
                && extraction_traits<T>::is_region_like == RegionLike<T>
                && extraction_traits_d<T>::is_multi_variant == extraction_traits<T>::is_multi_variant,
            single, multi, region, ready };
    }

    using Single = std::span<double>;
    using Multi = std::vector<Single>;

    class YantraDataSpec : public ::testing::TestWithParam<SpecCase> { };

    TEST_P(YantraDataSpec, PreservesRoutingAndExtractionContracts)
    {
        const auto& value = GetParam();
        EXPECT_TRUE(value.compute);
        EXPECT_EQ(value.single, value.expected_single);
        EXPECT_EQ(value.multi, value.expected_multi);
        EXPECT_EQ(value.region, value.expected_region);
        EXPECT_EQ(value.container, value.expected_region);
        EXPECT_TRUE(value.native);
        EXPECT_TRUE(value.algorithm);
        EXPECT_TRUE(value.variant);
        EXPECT_TRUE(value.consistent_flags);
        EXPECT_EQ(value.ready, value.expected_ready);
    }

    INSTANTIATE_TEST_SUITE_P(Representations, YantraDataSpec, ::testing::Values(
        spec<Variant, Single, Single, Variant>("Variant", true, false, false, false),
        spec<Variants, Multi, Multi, Variants>("Channels", false, true, false, true),
        spec<Samples, Single, Single, Variant>("DoubleSamples", true, false, false, false),
        spec<std::vector<float>, std::span<float>, Single, Variant>("FloatSamples", true, false, false, false),
        spec<std::vector<uint8_t>, std::span<uint8_t>, Single, Variant>("ByteSamples", true, false, false, false),
        spec<std::vector<uint16_t>, std::span<uint16_t>, Single, Variant>("Uint16Samples", true, false, false, false),
        spec<std::vector<uint32_t>, std::span<uint32_t>, Single, Variant>("Uint32Samples", true, false, false, false),
        spec<std::vector<std::complex<float>>, std::span<std::complex<float>>, Single, Variant>("ComplexFloatSamples", true, false, false, false),
        spec<std::vector<std::complex<double>>, std::span<std::complex<double>>, Single, Variant>("ComplexDoubleSamples", true, false, false, false),
        spec<std::vector<glm::vec2>, std::span<glm::vec2>, Single, Variant>("Vec2Samples", true, false, false, false),
        spec<std::vector<glm::vec3>, std::span<glm::vec3>, Single, Variant>("Vec3Samples", true, false, false, false),
        spec<std::vector<glm::vec4>, std::span<glm::vec4>, Single, Variant>("Vec4Samples", true, false, false, false),
        spec<std::vector<glm::mat4>, std::span<glm::mat4>, Single, Variant>("Mat4Samples", true, false, false, false),
        spec<Container, Multi, Multi, Variants>("Container", false, true, false, true),
        spec<Kakshya::Region, Multi, Multi, Variants>("Region", false, true, true, true),
        spec<Kakshya::RegionGroup, Multi, Multi, Variants>("RegionGroup", false, true, true, true),
        spec<std::vector<Kakshya::RegionSegment>, Multi, Multi, Variants>("Segments", false, true, true, true),
        spec<Eigen::VectorXf, std::vector<std::span<float>>, Multi, Variants>("FloatVector", true, true, false, true),
        spec<Eigen::RowVectorXd, Multi, Multi, Variants>("RowVector", false, true, false, true),
        spec<Eigen::MatrixXf, std::vector<std::span<float>>, Multi, Variants>("FloatMatrix", false, true, false, true),
        spec<Eigen::MatrixXi, std::vector<std::span<int>>, Multi, Variants>("IntegerMatrix", false, true, false, true),
        spec<Eigen::VectorXcd, std::vector<std::span<std::complex<double>>>, Multi, Variants>("ComplexVector", true, true, false, true),
        spec<Kakshya::CompositeArray, const Kakshya::CompositeArray&, void, void>("CompositeArray", false, false, false, true),
        spec<std::shared_ptr<Kakshya::CompositeContainer>, const Kakshya::CompositeArray&, void, void>("CompositeContainer", false, false, false, true)),
        [](const auto& info) { return info.param.name; });

    static_assert(VariantVector<Variants>);
    static_assert(!VariantVector<Samples>);
    static_assert(!VariantVector<std::vector<std::string>>);
    static_assert(!SingleVariant<std::string>);
    static_assert(!CompositeData<Container>);
    static_assert(!RequiresContainer<Container>);
    static_assert(std::same_as<native_element_t<Variant>, void>);
    static_assert(std::same_as<native_element_t<Variants>, void>);
    static_assert(std::same_as<native_element_t<std::vector<float>>, float>);
    static_assert(std::same_as<native_element_t<std::vector<uint8_t>>, uint8_t>);
    static_assert(std::same_as<native_element_t<Eigen::MatrixXf>, float>);
    static_assert(is_IO<DataIO>::value);
    static_assert(is_IO<CompositeIO>::value);
    static_assert(!is_IO<Variants>::value);
    static_assert(OperationReadyData<Datum<Samples>>);
    static_assert(!std::default_initializable<CompositeIO>);
    static_assert(std::same_as<decltype(*std::declval<Datum<Samples>&>()), Samples&>);
    static_assert(std::same_as<decltype(*std::declval<const Datum<Samples>&>()), const Samples&>);

    void expect_axis(const std::vector<Dimension>& dimensions, Role role, uint64_t size)
    {
        const auto found = std::ranges::find(dimensions, role, &Dimension::role);
        ASSERT_NE(found, dimensions.end());
        EXPECT_EQ(found->size, size);
    }

    enum class Construction { Copy, Move };

    class YantraDatumConstruction : public ::testing::TestWithParam<Construction> { };

    TEST_P(YantraDatumConstruction, OwnsSamplesAndInfersAfterTransfer)
    {
        Samples source { 2.0, -3.0, 7.0, 11.0 };
        const auto* allocation = source.data();
        Datum<Samples> datum = GetParam() == Construction::Copy
            ? Datum<Samples>(source) : Datum<Samples>(std::move(source));
        if (GetParam() == Construction::Copy) {
            source[0] = 99.0;
            EXPECT_NE(datum.data.data(), allocation);
        } else {
            EXPECT_EQ(datum.data.data(), allocation);
        }
        EXPECT_EQ(datum.data, (Samples { 2.0, -3.0, 7.0, 11.0 }));
        ASSERT_EQ(datum.dimensions.size(), 1);
        expect_axis(datum.dimensions, Role::TIME, 4);
        EXPECT_EQ(datum.get_total_elements(), 4);
        EXPECT_FALSE(datum.has_container());
        (*datum)[1] = 13.0;
        EXPECT_EQ(datum.data[1], 13.0);
    }

    TEST_P(YantraDatumConstruction, InfersChannelsRatherThanCountingChannelObjects)
    {
        Variants source { Samples { 1, 2, 3, 4 }, std::vector<float> { 5, 6, 7, 8 } };
        DataIO datum = GetParam() == Construction::Copy
            ? DataIO(source) : DataIO(std::move(source));
        ASSERT_EQ(datum.data.size(), 2);
        EXPECT_EQ(std::get<Samples>(datum.data[0]), (Samples { 1, 2, 3, 4 }));
        EXPECT_EQ(std::get<std::vector<float>>(datum.data[1]), (std::vector<float> { 5, 6, 7, 8 }));
        ASSERT_EQ(datum.dimensions.size(), 2);
        expect_axis(datum.dimensions, Role::TIME, 4);
        expect_axis(datum.dimensions, Role::CHANNEL, 2);
        EXPECT_EQ(datum.get_total_elements(), 8);
        EXPECT_EQ(datum.modality, Modality::AUDIO_MULTICHANNEL);
    }

    TEST_P(YantraDatumConstruction, RegionInferenceUsesAssociatedContainer)
    {
        Container container = make_audio_container(Samples { 1, 2, 3, 4, 5, 6 });
        Kakshya::Region source({ 0, 0 }, { 5, 0 });
        RegionIO datum = GetParam() == Construction::Copy
            ? RegionIO(source, container) : RegionIO(std::move(source), container);
        ASSERT_TRUE(datum.has_container());
        EXPECT_EQ(*datum.container, container);
        expect_axis(datum.dimensions, Role::TIME, 6);
        EXPECT_EQ(datum.modality, Modality::AUDIO_1D);
        EXPECT_TRUE(datum.is_suitable_for_processing(Modality::AUDIO_1D));
        EXPECT_EQ(datum.data.start_coordinates, (std::vector<uint64_t> { 0, 0 }));
        EXPECT_EQ(datum.data.end_coordinates, (std::vector<uint64_t> { 5, 0 }));
    }

    TEST_P(YantraDatumConstruction, BatchPreservesOrderEmptyMembersAndIndependentStorage)
    {
        std::vector<Samples> source { { 3, -1, 5 }, {}, { 9 } };
        const auto* first = source[0].data();
        auto batch = GetParam() == Construction::Copy
            ? as_io_batch(source) : as_io_batch(std::move(source));
        ASSERT_EQ(batch.size(), 3);
        EXPECT_EQ(batch[0].data, (Samples { 3, -1, 5 }));
        EXPECT_TRUE(batch[1].data.empty());
        EXPECT_EQ(batch[2].data, (Samples { 9 }));
        EXPECT_EQ(batch[0].get_total_elements(), 3);
        EXPECT_EQ(batch[1].get_total_elements(), 0);
        EXPECT_EQ(batch[2].get_total_elements(), 1);
        if (GetParam() == Construction::Copy) {
            source[0][0] = 100;
            EXPECT_NE(batch[0].data.data(), first);
        } else {
            EXPECT_EQ(batch[0].data.data(), first);
        }
        batch[0].data[0] = 42;
        EXPECT_EQ(batch[2].data, (Samples { 9 }));
    }

    TEST_P(YantraDatumConstruction, GroupAndSegmentInferenceUsesAssociatedContainer)
    {
        Container container = make_audio_container(std::vector<Samples> {
            { 1, 2, 3, 4, 5, 6 }, { 7, 8, 9, 10, 11, 12 } });
        Kakshya::Region region({ 0, 0 }, { 5, 1 });
        Kakshya::RegionGroup group;
        group.regions.push_back(region);
        std::vector<Kakshya::RegionSegment> segments { Kakshya::RegionSegment(region) };
        RegionGroupIO grouped = GetParam() == Construction::Copy
            ? RegionGroupIO(group, container) : RegionGroupIO(std::move(group), container);
        SegmentIO segmented = GetParam() == Construction::Copy
            ? SegmentIO(segments, container) : SegmentIO(std::move(segments), container);
        expect_axis(grouped.dimensions, Role::TIME, 6);
        expect_axis(segmented.dimensions, Role::TIME, 6);
        expect_axis(grouped.dimensions, Role::CHANNEL, 2);
        expect_axis(segmented.dimensions, Role::CHANNEL, 2);
        EXPECT_EQ(grouped.modality, Modality::AUDIO_MULTICHANNEL);
        EXPECT_EQ(segmented.modality, Modality::AUDIO_MULTICHANNEL);
        EXPECT_TRUE(grouped.is_suitable_for_processing(Modality::AUDIO_MULTICHANNEL));
        EXPECT_TRUE(segmented.is_suitable_for_processing(Modality::AUDIO_MULTICHANNEL));
    }

    INSTANTIATE_TEST_SUITE_P(Ownership, YantraDatumConstruction,
        ::testing::Values(Construction::Copy, Construction::Move),
        [](const auto& info) { return info.param == Construction::Copy ? "Copy" : "Move"; });

    struct VariantShape {
        std::string name;
        Variant data;
        Role role;
        uint64_t size;
    };

    class YantraDatumVariant : public ::testing::TestWithParam<VariantShape> { };

    TEST_P(YantraDatumVariant, PreservesAlternativeValuesAndAxisThroughCopyAndMove)
    {
        const auto& shape = GetParam();
        Datum<Variant> copied(shape.data);
        auto source = shape.data;
        Datum<Variant> moved(std::move(source));
        EXPECT_EQ(copied.data, shape.data);
        EXPECT_EQ(moved.data, shape.data);
        ASSERT_EQ(copied.dimensions.size(), 1);
        ASSERT_EQ(moved.dimensions.size(), 1);
        expect_axis(copied.dimensions, shape.role, shape.size);
        expect_axis(moved.dimensions, shape.role, shape.size);
        EXPECT_EQ(copied.get_total_elements(), shape.size);
        EXPECT_EQ(moved.get_total_elements(), shape.size);
    }

    INSTANTIATE_TEST_SUITE_P(Alternatives, YantraDatumVariant, ::testing::Values(
        VariantShape { "EmptyDouble", Samples {}, Role::TIME, 0 },
        VariantShape { "Float", std::vector<float> { -1.5F, 0.25F, 3.5F }, Role::TIME, 3 },
        VariantShape { "Byte", std::vector<uint8_t> { 0, 127, 255 }, Role::SPATIAL_X, 3 },
        VariantShape { "Complex", std::vector<std::complex<float>> { { 1, -2 }, { -3, 4 } }, Role::FREQUENCY, 2 }),
        [](const auto& info) { return info.param.name; });

    struct EigenShape {
        std::string name;
        int rows;
        int columns;
        Modality modality;
        size_t axes;
    };

    class YantraDatumEigen : public ::testing::TestWithParam<EigenShape> { };

    TEST_P(YantraDatumEigen, RetainsNativeValuesAndInfersRuntimeShape)
    {
        const auto& shape = GetParam();
        Eigen::MatrixXf matrix(shape.rows, shape.columns);
        for (int row = 0; row < shape.rows; ++row)
            for (int column = 0; column < shape.columns; ++column)
                matrix(row, column) = static_cast<float>(row * 32 + column) + 0.25F;
        Datum<Eigen::MatrixXf> datum(std::move(matrix));
        EXPECT_EQ(datum.modality, shape.modality);
        EXPECT_EQ(datum.dimensions.size(), shape.axes);
        EXPECT_EQ(datum.get_total_elements(), static_cast<uint64_t>(shape.rows * shape.columns));
        EXPECT_EQ(datum.data.rows(), shape.rows);
        EXPECT_EQ(datum.data.cols(), shape.columns);
        for (int row = 0; row < shape.rows; ++row)
            for (int column = 0; column < shape.columns; ++column)
                EXPECT_FLOAT_EQ(datum.data(row, column), static_cast<float>(row * 32 + column) + 0.25F);
    }

    INSTANTIATE_TEST_SUITE_P(Shapes, YantraDatumEigen, ::testing::Values(
        EigenShape { "EmptyRows", 0, 2, Modality::AUDIO_MULTICHANNEL, 2 },
        EigenShape { "SingleColumn", 5, 1, Modality::AUDIO_1D, 1 },
        EigenShape { "Stereo", 5, 2, Modality::AUDIO_MULTICHANNEL, 2 },
        EigenShape { "ChannelBoundary", 3, 16, Modality::AUDIO_MULTICHANNEL, 2 },
        EigenShape { "FeatureBoundary", 3, 17, Modality::SPECTRAL_2D, 2 }),
        [](const auto& info) { return info.param.name; });

    class YantraDatumStructure : public ::testing::TestWithParam<uint64_t> { };

    TEST_P(YantraDatumStructure, ExplicitStructureControlsSuitabilityWithoutRewritingData)
    {
        Datum<Samples> datum(Samples { 8, 9 },
            { Dimension::time(GetParam()), Dimension::channel(3) }, Modality::AUDIO_MULTICHANNEL);
        EXPECT_EQ(datum.get_total_elements(), GetParam() * 3);
        EXPECT_EQ(datum.find_dimension_by_role(Role::TIME), 0);
        EXPECT_EQ(datum.find_dimension_by_role(Role::CHANNEL), 1);
        EXPECT_EQ(datum.find_dimension_by_role(Role::FREQUENCY), -1);
        EXPECT_TRUE(datum.is_suitable_for_processing(Modality::UNKNOWN, 2));
        EXPECT_TRUE(datum.is_suitable_for_processing(Modality::AUDIO_MULTICHANNEL, 2));
        EXPECT_FALSE(datum.is_suitable_for_processing(Modality::SPECTRAL_2D));
        EXPECT_FALSE(datum.is_suitable_for_processing(Modality::UNKNOWN, 3));
        datum.update_structure({ Dimension::frequency(7) }, Modality::SPECTRAL_2D);
        EXPECT_EQ(datum.get_total_elements(), 7);
        EXPECT_TRUE(datum.has_modality(Modality::SPECTRAL_2D));
        EXPECT_EQ(datum.find_dimension_by_role(Role::TIME), -1);
        EXPECT_EQ(datum.find_dimension_by_role(Role::FREQUENCY), 0);
        EXPECT_EQ(datum.data, (Samples { 8, 9 }));
    }

    INSTANTIATE_TEST_SUITE_P(Extents, YantraDatumStructure,
        ::testing::Values(uint64_t { 0 }, uint64_t { 1 }, uint64_t { 7 }, uint64_t { 1 } << 33));

    TEST(YantraDatum, CopiesDataMetadataAndFacetsWithoutCouplingLaterWrites)
    {
        Facet<Samples> peaks;
        Datum<Samples> source(Samples { 1, 4, -2 });
        source.set_metadata("labels", std::vector<std::string> { "attack", "decay" });
        source.set_metadata("count", 3);
        source.facets.set(peaks, Samples { 4 });
        auto copy = source;
        copy.data[0] = 99;
        copy.set_metadata("count", std::string("three"));
        copy.facets.set(peaks, Samples { 8, 9 });
        copy.update_structure({ Dimension::frequency(2) }, Modality::SPECTRAL_2D);
        EXPECT_EQ(source.data, (Samples { 1, 4, -2 }));
        EXPECT_EQ(source.get_metadata<int>("count"), 3);
        EXPECT_THROW(copy.get_metadata<int>("count"), std::runtime_error);
        EXPECT_EQ(copy.get_metadata<std::string>("count"), std::string("three"));
        EXPECT_FALSE(source.get_metadata<std::string>("missing").has_value());
        auto labels = copy.get_metadata<std::vector<std::string>>("labels");
        ASSERT_TRUE(labels.has_value());
        (*labels)[0] = "changed";
        EXPECT_EQ(source.get_metadata<std::vector<std::string>>("labels"),
            (std::vector<std::string> { "attack", "decay" }));
        ASSERT_NE(source.facets.get(peaks), nullptr);
        ASSERT_NE(copy.facets.get(peaks), nullptr);
        EXPECT_EQ(*source.facets.get(peaks), (Samples { 4 }));
        EXPECT_EQ(*copy.facets.get(peaks), (Samples { 8, 9 }));
        expect_axis(source.dimensions, Role::TIME, 3);
        EXPECT_NE(source.modality, copy.modality);
    }

    TEST(YantraDatum, RejectsNullOwningContainersDuringInference)
    {
        Container signal;
        std::shared_ptr<Kakshya::CompositeContainer> composite;
        EXPECT_THROW((ContainerIO(signal)), std::invalid_argument);
        EXPECT_THROW((ContainerIO(std::move(signal))), std::invalid_argument);
        EXPECT_THROW((CompositeContainerIO(composite)), std::invalid_argument);
        EXPECT_THROW((CompositeContainerIO(std::move(composite))), std::invalid_argument);
    }

    TEST(YantraDatum, RegionSuitabilityRequiresALiveContainer)
    {
        RegionIO datum(Kakshya::Region({ 0 }, { 3 }),
            std::vector<Dimension> { Dimension::time(4) }, Modality::AUDIO_1D);
        EXPECT_FALSE(datum.is_suitable_for_processing(Modality::UNKNOWN));
        datum.set_container(nullptr);
        EXPECT_FALSE(datum.is_suitable_for_processing(Modality::UNKNOWN));
        Container container = make_audio_container(Samples { 1, 2, 3, 4 });
        std::weak_ptr<Kakshya::SignalSourceContainer> lifetime = container;
        datum.set_container(container);
        container.reset();
        EXPECT_FALSE(lifetime.expired());
        EXPECT_TRUE(datum.is_suitable_for_processing(Modality::AUDIO_1D));
        datum.container.reset();
        EXPECT_TRUE(lifetime.expired());
        EXPECT_FALSE(datum.is_suitable_for_processing(Modality::AUDIO_1D));
    }

    TEST(YantraDatum, ProcessingFlagAppliesOnlyToSignalContainerData)
    {
        ContainerIO container(make_audio_container(Samples { 1, 2, 3 }));
        Datum<Samples> samples(Samples { 1, 2, 3 });
        RegionIO region;
        EXPECT_FALSE(container.needs_processig());
        container.set_container_processing(true);
        samples.set_container_processing(true);
        region.set_container_processing(true);
        EXPECT_TRUE(container.needs_processig());
        EXPECT_FALSE(samples.needs_processig());
        EXPECT_FALSE(region.needs_processig());
        container.set_container_processing(false);
        EXPECT_FALSE(container.needs_processig());
    }

    Kakshya::CompositeArray records()
    {
        Kakshya::CompositeLayout layout;
        layout.add_field<float>("weight");
        layout.add_field<uint32_t>("id");
        Kakshya::CompositeArray result(std::move(layout));
        result.append();
        result.append();
        result.set(0, "weight", 1.25F);
        result.set(0, "id", uint32_t { 42 });
        result.set(1, "weight", -2.5F);
        return result;
    }

    TEST(YantraDatum, CompositeOwnershipRetainsSchemaExactTypesAndAbsentFields)
    {
        auto source = records();
        CompositeIO copied(source);
        CompositeIO moved(std::move(source));
        ASSERT_TRUE(moved.data.set(0, "weight", 9.5F));
        auto row = copied.data.at(0);
        ASSERT_TRUE(row.has_value());
        EXPECT_EQ(row->get<float>("weight"), 1.25F);
        EXPECT_EQ(row->get<uint32_t>("id"), uint32_t { 42 });
        EXPECT_FALSE(row->get<double>("weight").has_value());
        auto second = copied.data.at(1);
        ASSERT_TRUE(second.has_value());
        EXPECT_FALSE(second->has("id"));
        EXPECT_EQ(second->get<float>("weight"), -2.5F);
        EXPECT_EQ(copied.data.layout().fields().size(), 2);
        expect_axis(copied.dimensions, Role::CUSTOM, 2);
        EXPECT_EQ(copied.modality, Modality::TENSOR_ND);
        auto owner = std::make_shared<Kakshya::CompositeContainer>(std::move(moved.data));
        std::weak_ptr<Kakshya::CompositeContainer> lifetime = owner;
        CompositeContainerIO datum(owner);
        owner.reset();
        EXPECT_FALSE(lifetime.expired());
        expect_axis(datum.dimensions, Role::CUSTOM, 2);
        EXPECT_EQ(datum.modality, Modality::TENSOR_ND);
        auto retained = datum.data->get_data().at(0);
        ASSERT_TRUE(retained.has_value());
        EXPECT_EQ(retained->get<float>("weight"), 9.5F);
    }

    TEST(YantraDatum, EmptyBatchesStayEmptyAndZeroLengthRecordsStayStructured)
    {
        const std::vector<Samples> empty;
        EXPECT_TRUE(as_io_batch(empty).empty());
        EXPECT_TRUE(as_io_batch(std::vector<Samples> {}).empty());
        Kakshya::CompositeLayout layout;
        ASSERT_TRUE(layout.add_field<double>("value"));
        CompositeIO datum(Kakshya::CompositeArray(std::move(layout)));
        EXPECT_TRUE(datum.data.empty());
        ASSERT_EQ(datum.dimensions.size(), 1);
        expect_axis(datum.dimensions, Role::CUSTOM, 0);
        EXPECT_EQ(datum.get_total_elements(), 0);
        EXPECT_EQ(datum.modality, Modality::TENSOR_ND);
    }

}
}
