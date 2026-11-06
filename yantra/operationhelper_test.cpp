#include "MayaFlux/Yantra/OperationSpec/OperationHelper.hpp"

#include <gtest/gtest.h>

namespace MayaFlux::Test {
namespace {

    using namespace Yantra;
    using Samples = std::vector<double>;
    using Channels = std::vector<Samples>;
    using Variant = Kakshya::DataVariant;
    using Variants = std::vector<Variant>;
    using Dimension = Kakshya::DataDimension;
    using Role = Dimension::Role;
    using Modality = Kakshya::DataModality;

    enum class MatrixPath { Raw, Datum };
    enum class AxisOrder { TimeFirst, ChannelFirst };

    struct MatrixCase {
        std::string name;
        MatrixPath path;
        AxisOrder order;
    };

    DataStructureInfo audio_structure(uint64_t frames, uint64_t channels, AxisOrder order)
    {
        std::vector<Dimension> dimensions { Dimension::time(frames), Dimension::channel(channels) };
        if (order == AxisOrder::ChannelFirst)
            std::ranges::reverse(dimensions);
        return { Modality::AUDIO_MULTICHANNEL, std::move(dimensions), typeid(Variants) };
    }

    void expect_channels(const NumericChannels& actual, const Channels& expected)
    {
        ASSERT_EQ(actual.size(), expected.size());
        for (size_t channel = 0; channel < expected.size(); ++channel) {
            SCOPED_TRACE(channel);
            ASSERT_EQ(actual[channel].size(), expected[channel].size());
            for (size_t sample = 0; sample < expected[channel].size(); ++sample)
                EXPECT_DOUBLE_EQ(actual[channel][sample], expected[channel][sample]);
        }
    }

    void expect_matrix(const Eigen::MatrixXd& actual, const Channels& columns)
    {
        ASSERT_EQ(actual.cols(), columns.size());
        ASSERT_EQ(actual.rows(), columns.empty() ? 0 : columns[0].size());
        for (size_t column = 0; column < columns.size(); ++column) {
            SCOPED_TRACE(column);
            for (size_t row = 0; row < columns[column].size(); ++row)
                EXPECT_DOUBLE_EQ(actual(row, column), columns[column][row]);
        }
    }

    class YantraOperationHelperMatrix : public ::testing::TestWithParam<MatrixCase> {
    protected:
        Eigen::MatrixXd reconstruct(const Channels& channels, const DataStructureInfo& info) const
        {
            if (GetParam().path == MatrixPath::Raw)
                return OperationHelper::reconstruct_from_double<Eigen::MatrixXd>(channels, info);
            return OperationHelper::reconstruct_from_double<Datum<Eigen::MatrixXd>>(channels, info).data;
        }
    };

    TEST_P(YantraOperationHelperMatrix, StereoChannelsBecomeColumnsWithoutTransposition)
    {
        const Channels channels { { 1, -2, 3, 4, 5 }, { 11, 12, -13, 14, 15 } };
        const auto info = audio_structure(5, 2, GetParam().order);
        expect_matrix(reconstruct(channels, info), channels);
    }

    TEST_P(YantraOperationHelperMatrix, ShortenedResultsReconcileSourceExtents)
    {
        const Channels result { { 1.5, 4.5, 7.5 }, { 11.5, 14.5, 17.5 } };
        const auto source = audio_structure(8, 2, GetParam().order);
        const auto output = audio_structure(3, 2, GetParam().order);
        expect_matrix(reconstruct(result, source), result);
        expect_matrix(reconstruct(result, output), result);
    }

    TEST_P(YantraOperationHelperMatrix, ResultChannelCountReplacesSourceChannelCount)
    {
        const Channels result { { 1, 2, 3 }, { 11, 12, 13 } };
        expect_matrix(reconstruct(result, audio_structure(3, 4, GetParam().order)), result);
    }

    TEST_P(YantraOperationHelperMatrix, RectangularityChecksEveryChannel)
    {
        const Channels result { { 1, 2, 3 }, { 11, 12 } };
        EXPECT_THROW(reconstruct(result, audio_structure(3, 2, GetParam().order)), std::invalid_argument);
    }

    TEST_P(YantraOperationHelperMatrix, EmptyChannelsRetainTheirColumnCount)
    {
        const Channels result { {}, {} };
        expect_matrix(reconstruct(result, audio_structure(0, 2, GetParam().order)), result);
    }

    TEST_P(YantraOperationHelperMatrix, RectangularPayloadWorksWithoutDeclaredDimensions)
    {
        const Channels result { { 1, 2, 3 }, { 11, 12, 13 } };
        expect_matrix(reconstruct(result, DataStructureInfo {}), result);
    }

    TEST_P(YantraOperationHelperMatrix, RaggedPayloadIsRejectedWithoutDeclaredDimensions)
    {
        const Channels result { { 1, 2, 3 }, { 11 } };
        EXPECT_THROW(reconstruct(result, DataStructureInfo {}), std::invalid_argument);
    }

    TEST_P(YantraOperationHelperMatrix, CustomAxesRetainPositionalShapeValidation)
    {
        const Channels result { { 1, 2, 3 }, { 11, 12, 13 } };
        DataStructureInfo info { Modality::TENSOR_ND,
            { Dimension("rows", 3), Dimension("columns", 2) }, typeid(Eigen::MatrixXd) };
        expect_matrix(reconstruct(result, info), result);
        info.dimensions[0].size = 5;
        EXPECT_THROW(reconstruct(result, info), std::invalid_argument);
    }

    TEST_P(YantraOperationHelperMatrix, DatumReconcilesSizesAndDependentStridesWithoutChangingSource)
    {
        auto source = audio_structure(8, 4, GetParam().order);
        const int time = Kakshya::find_dimension_by_role(source.dimensions, Role::TIME);
        const int channel = Kakshya::find_dimension_by_role(source.dimensions, Role::CHANNEL);
        source.dimensions[time].name = "analysis_windows";
        source.dimensions[time].stride = 1;
        source.dimensions[channel].stride = 8;
        const Channels values { { 1, 2, 3 }, { 11, 12, 13 } };
        expect_matrix(reconstruct(values, source), values);
        const auto output = OperationHelper::reconstruct_from_double<Datum<Eigen::MatrixXd>>(values, source);
        expect_matrix(output.data, values);
        ASSERT_EQ(output.dimensions.size(), 2);
        EXPECT_EQ(output.dimensions[time].size, 3);
        EXPECT_EQ(output.dimensions[time].name, "analysis_windows");
        EXPECT_EQ(output.dimensions[time].stride, 1);
        EXPECT_EQ(output.dimensions[channel].size, 2);
        EXPECT_EQ(output.dimensions[channel].stride, 3);
        EXPECT_EQ(output.modality, source.modality);
        EXPECT_EQ(source.dimensions[time].size, 8);
        EXPECT_EQ(source.dimensions[channel].size, 4);
        EXPECT_EQ(source.dimensions[channel].stride, 8);
    }

    INSTANTIATE_TEST_SUITE_P(Paths, YantraOperationHelperMatrix, ::testing::Values(
        MatrixCase { "RawTimeFirst", MatrixPath::Raw, AxisOrder::TimeFirst },
        MatrixCase { "RawChannelFirst", MatrixPath::Raw, AxisOrder::ChannelFirst },
        MatrixCase { "DatumTimeFirst", MatrixPath::Datum, AxisOrder::TimeFirst },
        MatrixCase { "DatumChannelFirst", MatrixPath::Datum, AxisOrder::ChannelFirst }),
        [](const auto& info) { return info.param.name; });

    enum class ChannelPath { Raw, Variants, Datum };

    class YantraOperationHelperChannels : public ::testing::TestWithParam<ChannelPath> { };

    TEST_P(YantraOperationHelperChannels, UnequalExtractionResultsPreserveChannelBoundariesAndValues)
    {
        Channels source { { 1, -2, 3 }, {}, { 11 } };
        const auto info = audio_structure(3, 3, AxisOrder::TimeFirst);
        Channels result;
        if (GetParam() == ChannelPath::Raw) {
            result = OperationHelper::reconstruct_from_double<Channels>(source, info);
        } else {
            Variants variants;
            if (GetParam() == ChannelPath::Variants)
                variants = OperationHelper::reconstruct_from_double<Variants>(source, info);
            else
                variants = OperationHelper::reconstruct_from_double<DataIO>(source, info).data;
            ASSERT_EQ(variants.size(), 3);
            for (const auto& channel : variants) {
                ASSERT_TRUE(std::holds_alternative<Samples>(channel));
                result.push_back(std::get<Samples>(channel));
            }
        }
        EXPECT_EQ(result, (Channels { { 1, -2, 3 }, {}, { 11 } }));
        source[0][0] = 99;
        EXPECT_EQ(result[0][0], 1);
        EXPECT_TRUE(result[1].empty());
        EXPECT_EQ(result[2], (Samples { 11 }));
    }

    TEST(YantraOperationHelper, RaggedDatumDescribesTheChannelAxisWithoutARectangularSampleExtent)
    {
        const auto source = audio_structure(8, 2, AxisOrder::TimeFirst);
        const auto output = OperationHelper::reconstruct_from_double<DataIO>(
            { { 1, 2, 3 }, {}, { 11 } }, source);
        ASSERT_EQ(output.data.size(), 3);
        ASSERT_EQ(output.dimensions.size(), 1);
        EXPECT_EQ(output.dimensions[0].role, Role::CHANNEL);
        EXPECT_EQ(output.dimensions[0].size, 3);
        EXPECT_EQ(output.modality, Modality::TENSOR_ND);
        EXPECT_EQ(source.dimensions[0].size, 8);
        EXPECT_EQ(source.dimensions[1].size, 2);
    }

    TEST_P(YantraOperationHelperChannels, NoChannelsRemainDistinctFromOneEmptyChannel)
    {
        const auto info = audio_structure(0, 0, AxisOrder::TimeFirst);
        if (GetParam() == ChannelPath::Raw) {
            EXPECT_TRUE(OperationHelper::reconstruct_from_double<Channels>({}, info).empty());
            EXPECT_EQ(OperationHelper::reconstruct_from_double<Channels>({ {} }, info).size(), 1);
        } else if (GetParam() == ChannelPath::Variants) {
            EXPECT_TRUE(OperationHelper::reconstruct_from_double<Variants>({}, info).empty());
            EXPECT_EQ(OperationHelper::reconstruct_from_double<Variants>({ {} }, info).size(), 1);
        } else {
            EXPECT_TRUE(OperationHelper::reconstruct_from_double<DataIO>({}, info).data.empty());
            EXPECT_EQ(OperationHelper::reconstruct_from_double<DataIO>({ {} }, info).data.size(), 1);
        }
    }

    INSTANTIATE_TEST_SUITE_P(Paths, YantraOperationHelperChannels,
        ::testing::Values(ChannelPath::Raw, ChannelPath::Variants, ChannelPath::Datum),
        [](const auto& info) {
            return info.param == ChannelPath::Raw ? "Raw" : info.param == ChannelPath::Variants ? "Variants" : "Datum";
        });

    enum class NumericSource { RawFloat, EigenFloat };

    class YantraOperationHelperNumeric : public ::testing::TestWithParam<NumericSource> { };

    TEST_P(YantraOperationHelperNumeric, ConvertedStorageSurvivesSourceDestructionCopyAndMove)
    {
        NumericChannels converted;
        switch (GetParam()) {
        case NumericSource::RawFloat: {
            const std::vector<float> source { 1.25F, -2.5F, 3.75F };
            converted = OperationHelper::extract_numeric_data(source);
            break;
        }
        case NumericSource::EigenFloat: {
            Eigen::MatrixXf source(3, 1);
            source << 1.25F, -2.5F, 3.75F;
            converted = OperationHelper::extract_numeric_data(source);
            break;
        }
        }
        auto retained = converted;
        converted = NumericChannels {};
        NumericChannels moved = std::move(retained);
        expect_channels(moved, { { 1.25, -2.5, 3.75 } });
        ASSERT_FALSE(moved.empty());
        moved[0][1] = 9;
        EXPECT_DOUBLE_EQ(moved[0][1], 9);
    }

    INSTANTIATE_TEST_SUITE_P(Sources, YantraOperationHelperNumeric,
        ::testing::Values(NumericSource::RawFloat, NumericSource::EigenFloat),
        [](const auto& info) {
            switch (info.param) {
            case NumericSource::RawFloat:
                return "RawFloat";
            case NumericSource::EigenFloat:
                return "EigenFloat";
            }
            return "Unknown";
        });

    TEST(YantraOperationHelper, DoubleVariantExtractionAliasesLiveSourceStorage)
    {
        Variant source = Samples { 1, -2, 3 };
        auto channels = OperationHelper::extract_numeric_data(source);
        ASSERT_EQ(channels.size(), 1);
        EXPECT_EQ(channels[0].data(), std::get<Samples>(source).data());
        channels[0][1] = 7;
        EXPECT_EQ(std::get<Samples>(source), (Samples { 1, 7, 3 }));
    }

    TEST(YantraOperationHelper, MixedNumericVariantsRetainChannelOrderAndIndependentStorage)
    {
        Variants source { std::vector<float> { 1.25F, -2.5F, 3.75F },
            std::vector<uint8_t> { 0, 127, 255 } };
        auto channels = OperationHelper::extract_numeric_data(source);
        expect_channels(channels, { { 1.25, -2.5, 3.75 }, { 0, 127, 255 } });
        Variant other = std::vector<float> { 99.0F, 98.0F };
        auto unrelated = OperationHelper::extract_numeric_data(other);
        expect_channels(unrelated, { { 99, 98 } });
        expect_channels(channels, { { 1.25, -2.5, 3.75 }, { 0, 127, 255 } });
        ASSERT_EQ(channels.size(), 2);
        channels[0][0] = 7;
        EXPECT_DOUBLE_EQ(channels[1][0], 0);
    }

    TEST(YantraOperationHelper, WorkingBufferOwnsCopiesAndResizesBetweenDifferentInputs)
    {
        DataIO source(Variants { Samples { 1, 2, 3 }, Samples { 11, 12, 13 } },
            { Dimension::time(3), Dimension::channel(2) }, Modality::AUDIO_MULTICHANNEL);
        Channels working { { 99 }, { 98 }, { 97 } };
        auto [spans, info] = OperationHelper::setup_operation_buffer(source, working);
        ASSERT_EQ(spans.size(), 2);
        EXPECT_EQ(working, (Channels { { 1, 2, 3 }, { 11, 12, 13 } }));
        EXPECT_EQ(spans[0].data(), working[0].data());
        spans[0][0] = 7;
        EXPECT_EQ(working[0][0], 7);
        EXPECT_EQ(std::get<Samples>(source.data[0])[0], 1);
        EXPECT_EQ(info.modality, Modality::AUDIO_MULTICHANNEL);
        EXPECT_EQ(info.original_type, std::type_index(typeid(Variants)));
        ASSERT_EQ(info.dimensions.size(), 2);
        EXPECT_EQ(info.dimensions[0].role, Role::TIME);
        EXPECT_EQ(info.dimensions[0].size, 3);
        DataIO shorter(Variants { Samples { -4 } });
        auto [replacement, replacement_info] = OperationHelper::setup_operation_buffer(shorter, working);
        ASSERT_EQ(replacement.size(), 1);
        EXPECT_EQ(working, (Channels { { -4 } }));
        EXPECT_EQ(replacement[0].data(), working[0].data());
        EXPECT_EQ(replacement_info.original_type, std::type_index(typeid(Variants)));
    }

    TEST(YantraOperationHelper, MatrixDatumPreservesExplicitResultStructure)
    {
        const Channels values { { 2.5, 6.5 }, { 12.5, 16.5 } };
        const auto info = audio_structure(2, 2, AxisOrder::TimeFirst);
        const auto result = OperationHelper::reconstruct_from_double<Datum<Eigen::MatrixXd>>(values, info);
        expect_matrix(result.data, values);
        ASSERT_EQ(result.dimensions.size(), 2);
        EXPECT_EQ(result.dimensions[0].role, Role::TIME);
        EXPECT_EQ(result.dimensions[0].size, 2);
        EXPECT_EQ(result.dimensions[1].role, Role::CHANNEL);
        EXPECT_EQ(result.dimensions[1].size, 2);
        EXPECT_EQ(result.modality, Modality::AUDIO_MULTICHANNEL);
    }

    TEST(YantraOperationHelper, ScalarVariantReconstructionInterleavesChannelsBySample)
    {
        const Channels channels { { 1, 2, 3 }, { 11, 12, 13 } };
        const auto info = audio_structure(3, 2, AxisOrder::TimeFirst);
        const auto result = OperationHelper::reconstruct_from_double<Variant>(channels, info);
        ASSERT_TRUE(std::holds_alternative<Samples>(result));
        EXPECT_EQ(std::get<Samples>(result), (Samples { 1, 11, 2, 12, 3, 13 }));
    }

    TEST(YantraOperationHelper, ByteReconstructionUsesSourceScalarAndClampsItsRange)
    {
        const DataStructureInfo info { Modality::IMAGE_2D,
            { Dimension::spatial(1, 'y'), Dimension::spatial(5, 'x') }, typeid(std::vector<uint8_t>) };
        const auto result = OperationHelper::reconstruct_from_double<Variant>({ { -3, 0, 127, 255, 300 } }, info);
        ASSERT_TRUE(std::holds_alternative<std::vector<uint8_t>>(result));
        EXPECT_EQ(std::get<std::vector<uint8_t>>(result), (std::vector<uint8_t> { 0, 0, 127, 255, 255 }));
    }

    TEST(YantraOperationHelper, EmptyMonoResultsRetainMonoStructureAndDistinguishChannelPresence)
    {
        const DataStructureInfo source { Modality::AUDIO_1D, { Dimension::time(8) }, typeid(Variants) };
        const auto absent = OperationHelper::reconstruct_from_double<DataIO>({}, source);
        const auto empty = OperationHelper::reconstruct_from_double<DataIO>({ {} }, source);
        EXPECT_TRUE(absent.data.empty());
        ASSERT_EQ(empty.data.size(), 1);
        ASSERT_TRUE(std::holds_alternative<Samples>(empty.data[0]));
        EXPECT_TRUE(std::get<Samples>(empty.data[0]).empty());
        for (const auto* output : { &absent, &empty }) {
            ASSERT_EQ(output->dimensions.size(), 1);
            EXPECT_EQ(output->dimensions[0].role, Role::TIME);
            EXPECT_EQ(output->dimensions[0].size, 0);
            EXPECT_EQ(output->modality, Modality::AUDIO_1D);
            EXPECT_EQ(output->get_total_elements(), 0);
        }
        const auto absent_matrix = OperationHelper::reconstruct_from_double<Datum<Eigen::MatrixXd>>({}, source);
        const auto empty_matrix = OperationHelper::reconstruct_from_double<Datum<Eigen::MatrixXd>>({ {} }, source);
        EXPECT_EQ(absent_matrix.data.rows(), 0);
        EXPECT_EQ(absent_matrix.data.cols(), 0);
        EXPECT_EQ(empty_matrix.data.rows(), 0);
        EXPECT_EQ(empty_matrix.data.cols(), 1);
        EXPECT_EQ(absent_matrix.modality, Modality::AUDIO_1D);
        EXPECT_EQ(empty_matrix.modality, Modality::AUDIO_1D);
        EXPECT_EQ(source.dimensions[0].size, 8);
    }

    TEST(YantraOperationHelper, MissingSourceStructureCanFeedRepeatedLengthChangingReconstruction)
    {
        const auto initial = OperationHelper::reconstruct_from_double<Datum<Eigen::MatrixXd>>(
            { { 1, 2, 3, 4 }, { 11, 12, 13, 14 } }, DataStructureInfo {});
        expect_matrix(initial.data, { { 1, 2, 3, 4 }, { 11, 12, 13, 14 } });
        const int time = initial.find_dimension_by_role(Role::TIME);
        const int channels = initial.find_dimension_by_role(Role::CHANNEL);
        ASSERT_GE(time, 0);
        ASSERT_GE(channels, 0);
        EXPECT_EQ(initial.dimensions[time].size, 4);
        EXPECT_EQ(initial.dimensions[channels].size, 2);
        const auto shortened = OperationHelper::reconstruct_from_double<Datum<Eigen::MatrixXd>>(
            { { 7, 8 }, { 17, 18 } }, OperationHelper::get_structure_info(initial));
        expect_matrix(shortened.data, { { 7, 8 }, { 17, 18 } });
        EXPECT_EQ(shortened.get_total_elements(), 4);
        ASSERT_EQ(shortened.dimensions.size(), initial.dimensions.size());
        EXPECT_EQ(shortened.dimensions[time].size, 2);
        const auto final = OperationHelper::reconstruct_from_double<Datum<Eigen::MatrixXd>>(
            { { 9 }, { 19 } }, OperationHelper::get_structure_info(shortened));
        expect_matrix(final.data, { { 9 }, { 19 } });
        EXPECT_EQ(final.get_total_elements(), 2);
        ASSERT_EQ(final.dimensions.size(), initial.dimensions.size());
        EXPECT_EQ(final.dimensions[time].size, 1);
        EXPECT_EQ(initial.dimensions[time].size, 4);
    }

    struct GroupedResult {
        std::string name;
        Samples components;
        std::vector<glm::vec3> points;
    };

    class YantraOperationHelperGrouped : public ::testing::TestWithParam<GroupedResult> { };

    TEST_P(YantraOperationHelperGrouped, CompleteComponentsRetainVertexTypeAndGrouping)
    {
        const auto& value = GetParam();
        const DataStructureInfo source { Modality::VERTEX_POSITIONS_3D,
            { Dimension::grouped("vertices", 2, 3, Role::POSITION) }, typeid(std::vector<glm::vec3>) };
        const auto output = OperationHelper::reconstruct_from_double<Datum<Variant>>({ value.components }, source);
        ASSERT_TRUE(std::holds_alternative<std::vector<glm::vec3>>(output.data));
        const auto& points = std::get<std::vector<glm::vec3>>(output.data);
        ASSERT_EQ(points.size(), value.points.size());
        for (size_t point = 0; point < points.size(); ++point)
            for (size_t component = 0; component < 3; ++component)
                EXPECT_FLOAT_EQ(points[point][component], value.points[point][component]);
        ASSERT_EQ(output.dimensions.size(), 1);
        EXPECT_EQ(output.dimensions[0].name, "vertices");
        EXPECT_EQ(output.dimensions[0].role, Role::POSITION);
        EXPECT_EQ(output.dimensions[0].size, value.points.size());
        ASSERT_TRUE(output.dimensions[0].grouping.has_value());
        EXPECT_EQ(output.dimensions[0].grouping->count, 3);
        EXPECT_EQ(output.modality, Modality::VERTEX_POSITIONS_3D);
        EXPECT_EQ(source.dimensions[0].size, 2);
    }

    INSTANTIATE_TEST_SUITE_P(Vertices, YantraOperationHelperGrouped, ::testing::Values(
        GroupedResult { "Preserved", { 1, 2, 3, 11, 12, 13 }, { { 1, 2, 3 }, { 11, 12, 13 } } },
        GroupedResult { "Shortened", { -1, 2, -3 }, { { -1, 2, -3 } } },
        GroupedResult { "Expanded", { 1, 2, 3, 11, 12, 13, 21, 22, 23 },
            { { 1, 2, 3 }, { 11, 12, 13 }, { 21, 22, 23 } } }),
        [](const auto& info) { return info.param.name; });

    class YantraOperationHelperPartialGroup : public ::testing::TestWithParam<size_t> { };

    TEST_P(YantraOperationHelperPartialGroup, IncompleteVertexComponentsAreRejected)
    {
        const DataStructureInfo source { Modality::VERTEX_POSITIONS_3D,
            { Dimension::grouped("vertices", 2, 3, Role::POSITION) }, typeid(std::vector<glm::vec3>) };
        const Channels components { Samples(GetParam(), 1.0) };
        EXPECT_THROW(OperationHelper::reconstruct_from_double<Datum<Variant>>(components, source), std::invalid_argument);
        EXPECT_EQ(source.dimensions[0].size, 2);
        ASSERT_TRUE(source.dimensions[0].grouping.has_value());
        EXPECT_EQ(source.dimensions[0].grouping->count, 3);
    }

    INSTANTIATE_TEST_SUITE_P(Components, YantraOperationHelperPartialGroup,
        ::testing::Values(size_t { 1 }, size_t { 2 }, size_t { 4 }, size_t { 5 }));

}
}
