#include "MayaFlux/Yantra/Data/StructureIntrospection.hpp"
#include "MayaFlux/Kakshya/Source/CompositeContainer.hpp"
#include "../support/audio_container.hpp"

#include <gtest/gtest.h>

namespace MayaFlux::Test {
namespace {

    using namespace Yantra;
    using Dimension = Kakshya::DataDimension;
    using Role = Dimension::Role;
    using Modality = Kakshya::DataModality;
    using Variant = Kakshya::DataVariant;
    using Variants = std::vector<Variant>;
    using Samples = std::vector<double>;
    using Container = std::shared_ptr<Kakshya::SignalSourceContainer>;
    using Structure = std::pair<std::vector<Dimension>, Modality>;

    enum class InferencePath { Dedicated, Generic };

    class YantraStructureInference : public ::testing::TestWithParam<InferencePath> { };

    void expect_structure(const Structure& actual,
        std::initializer_list<std::pair<Role, uint64_t>> axes, Modality modality)
    {
        EXPECT_EQ(actual.second, modality);
        ASSERT_EQ(actual.first.size(), axes.size());
        size_t index = 0;
        for (const auto& [role, size] : axes) {
            SCOPED_TRACE(index);
            EXPECT_EQ(actual.first[index].role, role);
            EXPECT_EQ(actual.first[index].size, size);
            ++index;
        }
    }

    TEST_P(YantraStructureInference, StereoVariantsRetainChannelAndSampleAxes)
    {
        Variants source { Samples { 1, 2, 3, 4, 5 }, std::vector<float> { 6, 7, 8, 9, 10 } };
        const auto result = GetParam() == InferencePath::Dedicated
            ? infer_from_data_variant_vector(source) : infer_structure(source);
        expect_structure(result, { { Role::CHANNEL, 2 }, { Role::TIME, 5 } }, Modality::AUDIO_MULTICHANNEL);
        EXPECT_EQ(std::get<Samples>(source[0]), (Samples { 1, 2, 3, 4, 5 }));
        EXPECT_EQ(std::get<std::vector<float>>(source[1]), (std::vector<float> { 6, 7, 8, 9, 10 }));
    }

    TEST_P(YantraStructureInference, SingleChannelVariantsRetainSampleExtent)
    {
        const Variants source { Samples { 1, 2, 3, 4, 5 } };
        const auto result = GetParam() == InferencePath::Dedicated
            ? infer_from_data_variant_vector(source) : infer_structure(source);
        expect_structure(result, { { Role::TIME, 5 } }, Modality::AUDIO_1D);
    }

    TEST_P(YantraStructureInference, SignalContainerRetainsItsPlanarLayout)
    {
        Container source = make_audio_container(std::vector<Samples> {
            { 1, 2, 3, 4, 5 }, { 6, 7, 8, 9, 10 } });
        const auto result = GetParam() == InferencePath::Dedicated
            ? infer_from_container(source) : infer_structure(source);
        expect_structure(result, { { Role::TIME, 5 }, { Role::CHANNEL, 2 } }, Modality::AUDIO_MULTICHANNEL);
        const auto& original = source->get_structure().dimensions;
        ASSERT_EQ(original.size(), result.first.size());
        for (size_t i = 0; i < original.size(); ++i) {
            EXPECT_EQ(result.first[i].stride, original[i].stride);
            EXPECT_EQ(result.first[i].name, original[i].name);
        }
    }

    TEST_P(YantraStructureInference, NullSignalContainerIsRejected)
    {
        const Container source;
        if (GetParam() == InferencePath::Dedicated) {
            EXPECT_THROW(infer_from_container(source), std::invalid_argument);
        } else {
            EXPECT_THROW(infer_structure(source), std::invalid_argument);
        }
    }

    TEST_P(YantraStructureInference, EmptyMarkersWithoutSourceKeepUnknownModality)
    {
        const Kakshya::Region region;
        const Kakshya::RegionGroup group;
        const std::vector<Kakshya::RegionSegment> segments;
        const auto region_result = GetParam() == InferencePath::Dedicated
            ? infer_from_region(region) : infer_structure(region);
        const auto group_result = GetParam() == InferencePath::Dedicated
            ? infer_from_region_group(group) : infer_structure(group);
        const auto segment_result = GetParam() == InferencePath::Dedicated
            ? infer_from_segments(segments) : infer_structure(segments);
        expect_structure(region_result, { { Role::TIME, 1 } }, Modality::UNKNOWN);
        expect_structure(group_result, { { Role::TIME, 0 } }, Modality::UNKNOWN);
        expect_structure(segment_result, { { Role::TIME, 0 } }, Modality::UNKNOWN);
    }

    TEST_P(YantraStructureInference, EmptyGroupsAndSegmentsDoNotAcquireTheSourceExtent)
    {
        Container source = make_audio_container(Samples { 1, 2, 3, 4, 5 });
        const Kakshya::RegionGroup group;
        const std::vector<Kakshya::RegionSegment> segments;
        const auto grouped = GetParam() == InferencePath::Dedicated
            ? infer_from_region_group(group, source) : infer_structure(group, source);
        const auto segmented = GetParam() == InferencePath::Dedicated
            ? infer_from_segments(segments, source) : infer_structure(segments, source);
        expect_structure(grouped, { { Role::TIME, 0 } }, Modality::UNKNOWN);
        expect_structure(segmented, { { Role::TIME, 0 } }, Modality::UNKNOWN);
    }

    TEST_P(YantraStructureInference, MonoFrameOnlyBoundsRetainTheirInclusiveExtent)
    {
        Container source = make_audio_container(Samples { 1, 2, 3, 4, 5, 6 });
        const Kakshya::Region region({ 2 }, { 4 });
        Kakshya::RegionGroup group;
        group.regions.push_back(region);
        const std::vector<Kakshya::RegionSegment> segments { Kakshya::RegionSegment(region) };
        const auto selected = GetParam() == InferencePath::Dedicated
            ? infer_from_region(region, source) : infer_structure(region, source);
        const auto grouped = GetParam() == InferencePath::Dedicated
            ? infer_from_region_group(group, source) : infer_structure(group, source);
        const auto segmented = GetParam() == InferencePath::Dedicated
            ? infer_from_segments(segments, source) : infer_structure(segments, source);
        expect_structure(selected, { { Role::TIME, 3 } }, Modality::AUDIO_1D);
        expect_structure(grouped, { { Role::TIME, 3 } }, Modality::AUDIO_1D);
        expect_structure(segmented, { { Role::TIME, 3 } }, Modality::AUDIO_1D);
    }

    TEST_P(YantraStructureInference, MismatchedRegionCoordinateDimensionsAreRejected)
    {
        Container source = make_audio_container(Samples { 1, 2, 3, 4 });
        const Kakshya::Region region({ 0, 0 }, { 2 });
        if (GetParam() == InferencePath::Dedicated) {
            EXPECT_THROW(infer_from_region(region, source), std::invalid_argument);
        } else {
            EXPECT_THROW(infer_structure(region, source), std::invalid_argument);
        }
    }

    TEST_P(YantraStructureInference, SegmentSelectionRetainsFirstOrMostDimensionalRegionPolicy)
    {
        Container source = make_audio_container(Samples { 1, 2, 3, 4, 5, 6, 7, 8 });
        std::vector<Kakshya::RegionSegment> segments {
            Kakshya::RegionSegment(Kakshya::Region({ 1, 0 }, { 2, 0 })),
            Kakshya::RegionSegment(Kakshya::Region({ 3, 0 }, { 6, 0 }))
        };
        const auto first = GetParam() == InferencePath::Dedicated
            ? infer_from_segments(segments, source) : infer_structure(segments, source);
        expect_structure(first, { { Role::TIME, 2 } }, Modality::AUDIO_1D);
        segments[0].source_region = Kakshya::Region({ 1 }, { 2 });
        const auto most_dimensional = GetParam() == InferencePath::Dedicated
            ? infer_from_segments(segments, source) : infer_structure(segments, source);
        expect_structure(most_dimensional, { { Role::TIME, 4 } }, Modality::AUDIO_1D);
    }

    INSTANTIATE_TEST_SUITE_P(Paths, YantraStructureInference,
        ::testing::Values(InferencePath::Dedicated, InferencePath::Generic),
        [](const auto& info) { return info.param == InferencePath::Dedicated ? "Dedicated" : "Generic"; });

    enum class Marker { Region, Group, Segment };

    struct RegionCase {
        std::string name;
        uint32_t channels;
        uint64_t first_frame;
        uint64_t last_frame;
        uint64_t first_channel;
        uint64_t last_channel;
        Marker marker;
        InferencePath path;
    };

    std::vector<RegionCase> region_cases()
    {
        std::vector<RegionCase> result;
        for (auto marker : { Marker::Region, Marker::Group, Marker::Segment }) {
            const std::string prefix = marker == Marker::Region ? "Region" : marker == Marker::Group ? "Group" : "Segment";
            for (auto path : { InferencePath::Dedicated, InferencePath::Generic }) {
                const auto suffix = path == InferencePath::Dedicated ? "Dedicated" : "Generic";
                result.push_back({ prefix + "MonoGrain" + suffix, 1, 2, 4, 0, 0, marker, path });
                result.push_back({ prefix + "StereoGrain" + suffix, 2, 2, 4, 0, 1, marker, path });
                result.push_back({ prefix + "SingleFrame" + suffix, 2, 3, 3, 0, 1, marker, path });
                result.push_back({ prefix + "LastChannel" + suffix, 4, 1, 5, 3, 3, marker, path });
            }
        }
        return result;
    }

    class YantraStructureRegions : public ::testing::TestWithParam<RegionCase> { };

    TEST_P(YantraStructureRegions, InclusiveBoundsDetermineExtentInsteadOfCoordinateValues)
    {
        const auto& value = GetParam();
        Container source = make_audio_container(std::vector<Samples>(value.channels,
            Samples { 1, 2, 3, 4, 5, 6, 7, 8 }));
        const Kakshya::Region region({ value.first_frame, value.first_channel },
            { value.last_frame, value.last_channel });
        Structure result;
        switch (value.marker) {
        case Marker::Region:
            result = value.path == InferencePath::Dedicated
                ? infer_from_region(region, source) : infer_structure(region, source);
            break;
        case Marker::Group: {
            Kakshya::RegionGroup group;
            group.regions.push_back(region);
            result = value.path == InferencePath::Dedicated
                ? infer_from_region_group(group, source) : infer_structure(group, source);
            break;
        }
        case Marker::Segment: {
            const std::vector<Kakshya::RegionSegment> segments { Kakshya::RegionSegment(region) };
            result = value.path == InferencePath::Dedicated
                ? infer_from_segments(segments, source) : infer_structure(segments, source);
            break;
        }
        }
        const auto frames = value.last_frame - value.first_frame + 1;
        if (value.channels == 1) {
            expect_structure(result, { { Role::TIME, frames } }, Modality::AUDIO_1D);
        } else {
            expect_structure(result, { { Role::TIME, frames },
                                         { Role::CHANNEL, value.last_channel - value.first_channel + 1 } },
                Modality::AUDIO_MULTICHANNEL);
        }
    }

    INSTANTIATE_TEST_SUITE_P(Bounds, YantraStructureRegions,
        ::testing::ValuesIn(region_cases()), [](const auto& info) { return info.param.name; });

    TEST(YantraStructure, GroupBoundingExtentIncludesGapsAndIsIndependentOfRegionOrder)
    {
        Container source = make_audio_container(std::vector<Samples>(2,
            Samples { 1, 2, 3, 4, 5, 6, 7, 8 }));
        Kakshya::RegionGroup group;
        group.regions.emplace_back(std::vector<uint64_t> { 5, 1 }, std::vector<uint64_t> { 6, 1 });
        group.regions.emplace_back(std::vector<uint64_t> { 1, 0 }, std::vector<uint64_t> { 2, 0 });
        expect_structure(infer_from_region_group(group, source),
            { { Role::TIME, 6 }, { Role::CHANNEL, 2 } }, Modality::AUDIO_MULTICHANNEL);
        std::ranges::reverse(group.regions);
        expect_structure(infer_from_region_group(group, source),
            { { Role::TIME, 6 }, { Role::CHANNEL, 2 } }, Modality::AUDIO_MULTICHANNEL);
    }

    TEST(YantraStructure, GroupUsesUniformModalityAndFallsBackToSourceForMixedModalities)
    {
        Container source = make_audio_container(std::vector<Samples>(3,
            Samples { 1, 2, 3, 4, 5, 6 }));
        Kakshya::RegionGroup group;
        group.regions.emplace_back(std::vector<uint64_t> { 1, 0 }, std::vector<uint64_t> { 2, 1 });
        group.regions.emplace_back(std::vector<uint64_t> { 3, 1 }, std::vector<uint64_t> { 4, 2 });
        for (auto& region : group.regions)
            region.modality = Modality::SPECTRAL_2D;
        expect_structure(infer_from_region_group(group, source),
            { { Role::TIME, 4 }, { Role::FREQUENCY, 3 } }, Modality::SPECTRAL_2D);
        group.regions[1].modality.reset();
        expect_structure(infer_from_region_group(group, source),
            { { Role::TIME, 4 }, { Role::CHANNEL, 3 } }, Modality::AUDIO_MULTICHANNEL);
        EXPECT_EQ(group.regions[0].modality, Modality::SPECTRAL_2D);
        EXPECT_FALSE(group.regions[1].modality.has_value());
    }

    TEST(YantraStructure, EigenVectorsRespectScalarCategoryAndOrientation)
    {
        Eigen::RowVectorXf row(5);
        row.setConstant(0.25F);
        Eigen::VectorXcf spectral(3);
        spectral.setConstant(std::complex<float> { 1, -2 });
        Eigen::MatrixXi image(3, 7);
        image.setConstant(255);
        expect_structure(infer_structure(row), { { Role::TIME, 5 } }, Modality::AUDIO_1D);
        expect_structure(infer_structure(spectral), { { Role::FREQUENCY, 3 } }, Modality::SPECTRAL_2D);
        expect_structure(infer_structure(image),
            { { Role::SPATIAL_Y, 3 }, { Role::SPATIAL_X, 7 } }, Modality::IMAGE_2D);
        EXPECT_FLOAT_EQ(row[4], 0.25F);
        EXPECT_EQ(spectral[2], (std::complex<float> { 1, -2 }));
        EXPECT_EQ(image(2, 6), 255);
    }

    TEST(YantraStructure, CompositeElementAxisDoesNotFlattenFieldsIntoDimensions)
    {
        Kakshya::CompositeLayout layout;
        ASSERT_TRUE(layout.add_field<double>("gain"));
        ASSERT_TRUE(layout.add_field<uint32_t>("id"));
        Kakshya::CompositeArray data(std::move(layout));
        expect_structure(infer_structure(data), { { Role::CUSTOM, 0 } }, Modality::TENSOR_ND);
        data.append();
        data.append();
        ASSERT_TRUE(data.set(0, "gain", 0.75));
        expect_structure(infer_structure(data), { { Role::CUSTOM, 2 } }, Modality::TENSOR_ND);
        auto source = std::make_shared<Kakshya::CompositeContainer>(std::move(data));
        expect_structure(infer_structure(source), { { Role::CUSTOM, 2 } }, Modality::TENSOR_ND);
        const auto row = source->get_data().at(0);
        ASSERT_TRUE(row.has_value());
        EXPECT_EQ(row->get<double>("gain"), 0.75);
        EXPECT_FALSE(row->has("id"));
        EXPECT_THROW(infer_structure(std::shared_ptr<Kakshya::CompositeContainer> {}), std::invalid_argument);
    }

}
}
