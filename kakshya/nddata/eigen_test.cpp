#include "gtest/gtest.h"

#include <glm/gtc/matrix_transform.hpp>

#include "MayaFlux/Kakshya/NDData/EigenAccess.hpp"
#include "MayaFlux/Kakshya/NDData/EigenInsertion.hpp"

namespace MayaFlux::Test {
namespace {

    using Kakshya::DataVariant;
    using Kakshya::EigenAccess;
    using Kakshya::EigenInsertion;
    using Kakshya::MatrixInterpretation;

    TEST(EigenAccessVectorTest, DoublesConvertDirectly)
    {
        const DataVariant variant = std::vector<double> { 0.5, -1.5, 2.0 };

        const Eigen::VectorXd v = EigenAccess(variant).to_vector();

        ASSERT_EQ(v.size(), 3);
        EXPECT_DOUBLE_EQ(v(0), 0.5);
        EXPECT_DOUBLE_EQ(v(1), -1.5);
        EXPECT_DOUBLE_EQ(v(2), 2.0);
    }

    TEST(EigenAccessVectorTest, OtherArithmeticTypesWidenToDouble)
    {
        const DataVariant floats = std::vector<float> { 0.25F, 0.5F };
        const DataVariant bytes = std::vector<uint8_t> { 0, 255 };
        const DataVariant halves = std::vector<uint16_t> { 1000, 65535 };
        const DataVariant words = std::vector<uint32_t> { 4000000000U };

        EXPECT_DOUBLE_EQ(EigenAccess(floats).to_vector()(1), 0.5);
        EXPECT_DOUBLE_EQ(EigenAccess(bytes).to_vector()(1), 255.0);
        EXPECT_DOUBLE_EQ(EigenAccess(halves).to_vector()(1), 65535.0);
        EXPECT_DOUBLE_EQ(EigenAccess(words).to_vector()(0), 4000000000.0);
    }

    TEST(EigenAccessVectorTest, EmptyDataGivesAnEmptyVector)
    {
        const DataVariant variant = std::vector<float> {};

        EXPECT_EQ(EigenAccess(variant).to_vector().size(), 0);
    }

    TEST(EigenAccessVectorTest, ComplexDataBecomesMagnitudes)
    {
        const DataVariant variant = std::vector<std::complex<double>> { { 3.0, 4.0 }, { 0.0, -2.0 } };

        const Eigen::VectorXd v = EigenAccess(variant).to_vector();

        ASSERT_EQ(v.size(), 2);
        EXPECT_DOUBLE_EQ(v(0), 5.0);
        EXPECT_DOUBLE_EQ(v(1), 2.0);
    }

    TEST(EigenAccessVectorTest, StructuredDataRefusesToBeAVector)
    {
        EXPECT_THROW((void)EigenAccess(DataVariant { std::vector<glm::vec2> { { 1, 2 } } }).to_vector(), std::invalid_argument);
        EXPECT_THROW((void)EigenAccess(DataVariant { std::vector<glm::vec3> { { 1, 2, 3 } } }).to_vector(), std::invalid_argument);
        EXPECT_THROW((void)EigenAccess(DataVariant { std::vector<glm::mat4> { glm::mat4(1.0F) } }).to_vector(), std::invalid_argument);
    }

    TEST(EigenAccessMatrixTest, ScalarsAreASingleRow)
    {
        const DataVariant variant = std::vector<float> { 1, 2, 3, 4 };

        const Eigen::MatrixXd m = EigenAccess(variant).to_matrix();

        ASSERT_EQ(m.rows(), 1);
        ASSERT_EQ(m.cols(), 4);
        EXPECT_DOUBLE_EQ(m(0, 3), 4.0);
    }

    TEST(EigenAccessMatrixTest, ComplexDataIsRealOverImaginary)
    {
        const DataVariant variant = std::vector<std::complex<float>> { { 1.0F, 2.0F }, { 3.0F, 4.0F } };

        const Eigen::MatrixXd m = EigenAccess(variant).to_matrix();

        ASSERT_EQ(m.rows(), 2);
        ASSERT_EQ(m.cols(), 2);
        EXPECT_DOUBLE_EQ(m(0, 0), 1.0);
        EXPECT_DOUBLE_EQ(m(1, 0), 2.0);
        EXPECT_DOUBLE_EQ(m(0, 1), 3.0);
        EXPECT_DOUBLE_EQ(m(1, 1), 4.0);
    }

    TEST(EigenAccessMatrixTest, VectorsAreOneColumnPerElement)
    {
        const DataVariant v2 = std::vector<glm::vec2> { { 1, 2 }, { 3, 4 } };
        const DataVariant v3 = std::vector<glm::vec3> { { 1, 2, 3 }, { 4, 5, 6 } };
        const DataVariant v4 = std::vector<glm::vec4> { { 1, 2, 3, 4 }, { 5, 6, 7, 8 } };

        const Eigen::MatrixXd m2 = EigenAccess(v2).to_matrix();
        const Eigen::MatrixXd m3 = EigenAccess(v3).to_matrix();
        const Eigen::MatrixXd m4 = EigenAccess(v4).to_matrix();

        EXPECT_EQ(m2.rows(), 2);
        EXPECT_EQ(m3.rows(), 3);
        EXPECT_EQ(m4.rows(), 4);
        EXPECT_DOUBLE_EQ(m2(1, 1), 4.0);
        EXPECT_DOUBLE_EQ(m3(2, 0), 3.0);
        EXPECT_DOUBLE_EQ(m3(0, 1), 4.0);
        EXPECT_DOUBLE_EQ(m4(3, 1), 8.0);
    }

    TEST(EigenAccessMatrixTest, MatricesFlattenColumnMajor)
    {
        glm::mat4 source(0.0F);
        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 4; ++r) {
                source[c][r] = static_cast<float>(c * 4 + r);
            }
        }
        const DataVariant variant = std::vector<glm::mat4> { source };

        const Eigen::MatrixXd m = EigenAccess(variant).to_matrix();

        ASSERT_EQ(m.rows(), 16);
        ASSERT_EQ(m.cols(), 1);
        for (int j = 0; j < 16; ++j) {
            EXPECT_DOUBLE_EQ(m(j, 0), static_cast<double>(j)) << j;
        }
    }

    TEST(EigenAccessMatrixTest, EmptyDataGivesAnEmptyRow)
    {
        const DataVariant variant = std::vector<double> {};

        const Eigen::MatrixXd m = EigenAccess(variant).to_matrix();

        EXPECT_EQ(m.rows(), 1);
        EXPECT_EQ(m.cols(), 0);
    }

    TEST(EigenAccessComplexTest, MagnitudeIsTheModulus)
    {
        const DataVariant variant = std::vector<std::complex<float>> { { 3.0F, 4.0F }, { -5.0F, 12.0F } };

        const Eigen::VectorXd v = EigenAccess(variant).to_magnitude_vector();

        ASSERT_EQ(v.size(), 2);
        EXPECT_NEAR(v(0), 5.0, 1e-6);
        EXPECT_NEAR(v(1), 13.0, 1e-6);
    }

    TEST(EigenAccessComplexTest, ComplexVectorKeepsRealAndImaginaryParts)
    {
        const DataVariant singles = std::vector<std::complex<float>> { { 1.5F, -2.5F } };
        const DataVariant doubles = std::vector<std::complex<double>> { { 1.5, -2.5 } };

        const Eigen::VectorXcd from_singles = EigenAccess(singles).to_complex_vector();
        const Eigen::VectorXcd from_doubles = EigenAccess(doubles).to_complex_vector();

        ASSERT_EQ(from_singles.size(), 1);
        EXPECT_DOUBLE_EQ(from_singles(0).real(), 1.5);
        EXPECT_DOUBLE_EQ(from_singles(0).imag(), -2.5);
        EXPECT_EQ(from_doubles(0), std::complex<double>(1.5, -2.5));
    }

    TEST(EigenAccessComplexTest, PhaseIsTheArgument)
    {
        const DataVariant variant = std::vector<std::complex<double>> { { 1.0, 0.0 }, { 0.0, 1.0 }, { -1.0, 0.0 }, { 0.0, -1.0 } };

        const Eigen::VectorXd phase = EigenAccess(variant).to_phase_vector();

        ASSERT_EQ(phase.size(), 4);
        EXPECT_NEAR(phase(0), 0.0, 1e-12);
        EXPECT_NEAR(phase(1), M_PI / 2.0, 1e-12);
        EXPECT_NEAR(phase(2), M_PI, 1e-12);
        EXPECT_NEAR(phase(3), -M_PI / 2.0, 1e-12);
    }

    TEST(EigenAccessComplexTest, ComplexOnlyViewsRejectRealData)
    {
        const DataVariant variant = std::vector<double> { 1.0, 2.0 };
        const EigenAccess access(variant);

        EXPECT_THROW((void)access.to_magnitude_vector(), std::invalid_argument);
        EXPECT_THROW((void)access.to_complex_vector(), std::invalid_argument);
        EXPECT_THROW((void)access.to_phase_vector(), std::invalid_argument);
    }

    TEST(EigenAccessMetadataTest, ElementCountIsTheStorageSize)
    {
        EXPECT_EQ(EigenAccess(DataVariant { std::vector<float>(7) }).element_count(), 7U);
        EXPECT_EQ(EigenAccess(DataVariant { std::vector<glm::vec3>(5) }).element_count(), 5U);
        EXPECT_EQ(EigenAccess(DataVariant { std::vector<double> {} }).element_count(), 0U);
    }

    TEST(EigenAccessMetadataTest, ComponentCountIsThePerElementRowCount)
    {
        EXPECT_EQ(EigenAccess(DataVariant { std::vector<double>(2) }).component_count(), 1U);
        EXPECT_EQ(EigenAccess(DataVariant { std::vector<uint8_t>(2) }).component_count(), 1U);
        EXPECT_EQ(EigenAccess(DataVariant { std::vector<std::complex<float>>(2) }).component_count(), 2U);
        EXPECT_EQ(EigenAccess(DataVariant { std::vector<std::complex<double>>(2) }).component_count(), 2U);
        EXPECT_EQ(EigenAccess(DataVariant { std::vector<glm::vec2>(2) }).component_count(), 2U);
        EXPECT_EQ(EigenAccess(DataVariant { std::vector<glm::vec3>(2) }).component_count(), 3U);
        EXPECT_EQ(EigenAccess(DataVariant { std::vector<glm::vec4>(2) }).component_count(), 4U);
        EXPECT_EQ(EigenAccess(DataVariant { std::vector<glm::mat4>(2) }).component_count(), 16U);
    }

    TEST(EigenAccessMetadataTest, MatrixDimensionsAreComponentsByElements)
    {
        const DataVariant variant = std::vector<glm::vec3>(5);
        const EigenAccess access(variant);

        EXPECT_EQ(access.matrix_dimensions(), (std::pair<size_t, size_t> { 3, 5 }));

        const Eigen::MatrixXd m = access.to_matrix();
        EXPECT_EQ(static_cast<size_t>(m.rows()), access.matrix_dimensions().first);
        EXPECT_EQ(static_cast<size_t>(m.cols()), access.matrix_dimensions().second);
    }

    TEST(EigenAccessMetadataTest, ClassifiesComplexAndStructuredData)
    {
        const DataVariant real = std::vector<double>(2);
        const DataVariant complex = std::vector<std::complex<double>>(2);
        const DataVariant glm_data = std::vector<glm::vec4>(2);

        EXPECT_FALSE(EigenAccess(real).is_complex());
        EXPECT_FALSE(EigenAccess(real).is_structured());
        EXPECT_TRUE(EigenAccess(complex).is_complex());
        EXPECT_FALSE(EigenAccess(complex).is_structured());
        EXPECT_FALSE(EigenAccess(glm_data).is_complex());
        EXPECT_TRUE(EigenAccess(glm_data).is_structured());
    }

    TEST(EigenAccessMetadataTest, TypeNameIsTheElementTypeName)
    {
        EXPECT_EQ(EigenAccess(DataVariant { std::vector<float>(1) }).type_name(), typeid(float).name());
        EXPECT_EQ(EigenAccess(DataVariant { std::vector<glm::vec3>(1) }).type_name(), typeid(glm::vec3).name());
    }

    TEST(EigenAccessMetadataTest, DimensionsFitEigen)
    {
        EXPECT_TRUE(EigenAccess(DataVariant { std::vector<double>(100) }).validate_dimensions());
    }

    TEST(EigenAccessViewTest, DoublesAreMappedWithoutCopying)
    {
        const DataVariant variant = std::vector<double> { 1.0, 2.0, 3.0 };

        const auto view = EigenAccess(variant).view<Eigen::VectorXd>();

        ASSERT_TRUE(view.has_value());
        EXPECT_EQ(view->data(), std::get<std::vector<double>>(variant).data());
        EXPECT_EQ(view->size(), 3);
        EXPECT_DOUBLE_EQ((*view)(2), 3.0);
    }

    TEST(EigenAccessViewTest, FloatsAreMappedWithoutCopying)
    {
        const DataVariant variant = std::vector<float> { 1.0F, 2.0F };

        const auto view = EigenAccess(variant).view<Eigen::VectorXf>();

        ASSERT_TRUE(view.has_value());
        EXPECT_EQ(view->data(), std::get<std::vector<float>>(variant).data());
    }

    TEST(EigenAccessViewTest, ComplexDataIsMappedWithoutCopying)
    {
        const DataVariant doubles = std::vector<std::complex<double>> { { 1.0, 2.0 } };
        const DataVariant floats = std::vector<std::complex<float>> { { 1.0F, 2.0F } };

        const auto vd = EigenAccess(doubles).view<Eigen::VectorXcd>();
        const auto vf = EigenAccess(floats).view<Eigen::VectorXcf>();

        ASSERT_TRUE(vd.has_value());
        ASSERT_TRUE(vf.has_value());
        EXPECT_EQ(vd->data(), std::get<std::vector<std::complex<double>>>(doubles).data());
        EXPECT_EQ(vf->data(), std::get<std::vector<std::complex<float>>>(floats).data());
    }

    TEST(EigenAccessViewTest, MismatchedTypesHaveNoView)
    {
        const DataVariant floats = std::vector<float> { 1.0F };
        const DataVariant doubles = std::vector<double> { 1.0 };
        const DataVariant complex = std::vector<std::complex<float>> { { 1.0F, 0.0F } };

        EXPECT_FALSE(EigenAccess(floats).view<Eigen::VectorXd>().has_value());
        EXPECT_FALSE(EigenAccess(doubles).view<Eigen::VectorXf>().has_value());
        EXPECT_FALSE(EigenAccess(complex).view<Eigen::VectorXcd>().has_value());
        EXPECT_FALSE(EigenAccess(doubles).view<Eigen::VectorXcd>().has_value());
    }

    TEST(EigenAccessViewTest, EmptyDataHasNoView)
    {
        const DataVariant variant = std::vector<double> {};

        EXPECT_FALSE(EigenAccess(variant).view<Eigen::VectorXd>().has_value());
    }

    TEST(EigenAccessViewTest, MatrixViewReadsRowMajor)
    {
        const DataVariant variant = std::vector<double> { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };

        const auto view = EigenAccess(variant).view_as_matrix(3);

        ASSERT_TRUE(view.has_value());
        ASSERT_EQ(view->rows(), 3);
        ASSERT_EQ(view->cols(), 4);
        EXPECT_EQ(view->data(), std::get<std::vector<double>>(variant).data());
        EXPECT_DOUBLE_EQ((*view)(0, 3), 3.0);
        EXPECT_DOUBLE_EQ((*view)(1, 0), 4.0);
        EXPECT_DOUBLE_EQ((*view)(2, 3), 11.0);
    }

    TEST(EigenAccessViewTest, MatrixViewSupportsFloats)
    {
        const DataVariant variant = std::vector<float> { 0, 1, 2, 3 };

        const auto view = EigenAccess(variant).view_as_matrix<float>(2);

        ASSERT_TRUE(view.has_value());
        EXPECT_EQ(view->rows(), 2);
        EXPECT_EQ(view->cols(), 2);
        EXPECT_FLOAT_EQ((*view)(1, 0), 2.0F);
    }

    TEST(EigenAccessViewTest, MatrixViewNeedsWholeColumns)
    {
        const DataVariant variant = std::vector<double>(10, 1.0);

        EXPECT_FALSE(EigenAccess(variant).view_as_matrix(3).has_value());
    }

    TEST(EigenAccessViewTest, MatrixViewNeedsTheMatchingScalarType)
    {
        const DataVariant variant = std::vector<float>(12, 1.0F);

        EXPECT_FALSE(EigenAccess(variant).view_as_matrix(3).has_value());
    }

    TEST(EigenAccessViewTest, MatrixViewOfEmptyDataIsAbsent)
    {
        const DataVariant variant = std::vector<double> {};

        EXPECT_FALSE(EigenAccess(variant).view_as_matrix(3).has_value());
    }

    TEST(EigenAccessViewTest, GlmViewAliasesTheStorage)
    {
        const DataVariant variant = std::vector<glm::vec3> { { 1, 2, 3 }, { 4, 5, 6 } };

        const auto view = EigenAccess(variant).view_as_glm<glm::vec3>();

        ASSERT_TRUE(view.has_value());
        EXPECT_EQ(view->data(), std::get<std::vector<glm::vec3>>(variant).data());
        ASSERT_EQ(view->size(), 2U);
        EXPECT_EQ((*view)[1], glm::vec3(4, 5, 6));
    }

    TEST(EigenAccessViewTest, GlmViewNeedsTheExactType)
    {
        const DataVariant variant = std::vector<glm::vec3> { { 1, 2, 3 } };

        EXPECT_FALSE(EigenAccess(variant).view_as_glm<glm::vec2>().has_value());
        EXPECT_FALSE(EigenAccess(variant).view_as_glm<glm::vec4>().has_value());
        EXPECT_FALSE(EigenAccess(DataVariant { std::vector<float>(3) }).view_as_glm<glm::vec3>().has_value());
    }

    TEST(EigenAccessConvenienceTest, FreeFunctionsMatchTheAccessor)
    {
        const DataVariant variant = std::vector<glm::vec2> { { 1, 2 }, { 3, 4 } };
        const DataVariant scalars = std::vector<double> { 5, 6 };

        EXPECT_EQ(Kakshya::to_eigen_matrix(variant), EigenAccess(variant).to_matrix());
        EXPECT_EQ(Kakshya::to_eigen_vector(scalars), EigenAccess(scalars).to_vector());
    }

    TEST(EigenInsertionVectorTest, PreservesPrecisionByDefault)
    {
        DataVariant variant;
        EigenInsertion insertion(variant);
        Eigen::VectorXd v(3);
        v << 0.1, 0.2, 0.3;

        insertion.insert_vector(v);

        ASSERT_TRUE(std::holds_alternative<std::vector<double>>(variant));
        EXPECT_EQ(std::get<std::vector<double>>(variant), (std::vector<double> { 0.1, 0.2, 0.3 }));
    }

    TEST(EigenInsertionVectorTest, CanNarrowToFloat)
    {
        DataVariant variant;
        EigenInsertion insertion(variant);
        Eigen::VectorXd v(2);
        v << 0.1, 0.2;

        insertion.insert_vector(v, false);

        ASSERT_TRUE(std::holds_alternative<std::vector<float>>(variant));
        EXPECT_EQ(std::get<std::vector<float>>(variant), (std::vector<float> { 0.1F, 0.2F }));
    }

    TEST(EigenInsertionVectorTest, ReplacesEarlierContent)
    {
        DataVariant variant = std::vector<glm::vec3>(4);
        EigenInsertion insertion(variant);

        insertion.insert_vector(Eigen::VectorXd::Ones(2));

        EXPECT_EQ(std::get<std::vector<double>>(variant).size(), 2U);
    }

    TEST(EigenInsertionVectorTest, EmptyVectorGivesEmptyStorage)
    {
        DataVariant variant;
        EigenInsertion insertion(variant);

        insertion.insert_vector(Eigen::VectorXd(0));

        EXPECT_TRUE(std::get<std::vector<double>>(variant).empty());
    }

    TEST(EigenInsertionMatrixTest, AutoOneRowIsScalarDouble)
    {
        DataVariant variant;
        EigenInsertion insertion(variant);
        Eigen::MatrixXd m(1, 3);
        m << 1, 2, 3;

        insertion.insert_matrix(m);

        EXPECT_EQ(std::get<std::vector<double>>(variant), (std::vector<double> { 1, 2, 3 }));
    }

    TEST(EigenInsertionMatrixTest, ScalarCanBeFloat)
    {
        DataVariant variant;
        EigenInsertion insertion(variant);
        Eigen::MatrixXd m(1, 2);
        m << 0.25, 0.5;

        insertion.insert_matrix(m, MatrixInterpretation::SCALAR, false);

        EXPECT_EQ(std::get<std::vector<float>>(variant), (std::vector<float> { 0.25F, 0.5F }));
    }

    TEST(EigenInsertionMatrixTest, AutoThreeRowsIsVec3)
    {
        DataVariant variant;
        EigenInsertion insertion(variant);
        Eigen::MatrixXd m(3, 2);
        m << 1, 4,
            2, 5,
            3, 6;

        insertion.insert_matrix(m);

        EXPECT_EQ(std::get<std::vector<glm::vec3>>(variant), (std::vector<glm::vec3> { { 1, 2, 3 }, { 4, 5, 6 } }));
    }

    TEST(EigenInsertionMatrixTest, AutoFourRowsIsVec4)
    {
        DataVariant variant;
        EigenInsertion insertion(variant);
        Eigen::MatrixXd m(4, 1);
        m << 1, 2, 3, 4;

        insertion.insert_matrix(m);

        EXPECT_EQ(std::get<std::vector<glm::vec4>>(variant), (std::vector<glm::vec4> { { 1, 2, 3, 4 } }));
    }

    TEST(EigenInsertionMatrixTest, AutoSixteenRowsIsMat4)
    {
        DataVariant variant;
        EigenInsertion insertion(variant);
        Eigen::MatrixXd m(16, 1);
        for (int j = 0; j < 16; ++j) {
            m(j, 0) = j;
        }

        insertion.insert_matrix(m);

        const auto& mats = std::get<std::vector<glm::mat4>>(variant);
        ASSERT_EQ(mats.size(), 1U);
        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 4; ++r) {
                EXPECT_FLOAT_EQ(mats[0][c][r], static_cast<float>(c * 4 + r)) << c << "," << r;
            }
        }
    }

    TEST(EigenInsertionMatrixTest, TwoRowsCanBeComplexDouble)
    {
        DataVariant variant;
        EigenInsertion insertion(variant);
        Eigen::MatrixXd m(2, 2);
        m << 1, 3,
            2, 4;

        insertion.insert_matrix(m, MatrixInterpretation::COMPLEX);

        EXPECT_EQ(std::get<std::vector<std::complex<double>>>(variant),
            (std::vector<std::complex<double>> { { 1, 2 }, { 3, 4 } }));
    }

    TEST(EigenInsertionMatrixTest, TwoRowsCanBeComplexFloat)
    {
        DataVariant variant;
        EigenInsertion insertion(variant);
        Eigen::MatrixXd m(2, 1);
        m << 1.5, -2.5;

        insertion.insert_matrix(m, MatrixInterpretation::COMPLEX, false);

        EXPECT_EQ(std::get<std::vector<std::complex<float>>>(variant),
            (std::vector<std::complex<float>> { { 1.5F, -2.5F } }));
    }

    TEST(EigenInsertionMatrixTest, TwoRowsCanBeVec2)
    {
        DataVariant variant;
        EigenInsertion insertion(variant);
        Eigen::MatrixXd m(2, 2);
        m << 1, 3,
            2, 4;

        insertion.insert_matrix(m, MatrixInterpretation::VEC2);

        EXPECT_EQ(std::get<std::vector<glm::vec2>>(variant), (std::vector<glm::vec2> { { 1, 2 }, { 3, 4 } }));
    }

    TEST(EigenInsertionMatrixTest, AutoRefusesAmbiguousTwoRows)
    {
        DataVariant variant = std::vector<float> { 9.0F };
        EigenInsertion insertion(variant);

        EXPECT_THROW(insertion.insert_matrix(Eigen::MatrixXd::Ones(2, 2)), std::invalid_argument);
        EXPECT_EQ(std::get<std::vector<float>>(variant), (std::vector<float> { 9.0F }));
    }

    TEST(EigenInsertionMatrixTest, AutoRefusesUnlistedRowCounts)
    {
        DataVariant variant;
        EigenInsertion insertion(variant);

        EXPECT_THROW(insertion.insert_matrix(Eigen::MatrixXd::Ones(5, 2)), std::invalid_argument);
        EXPECT_THROW(insertion.insert_matrix(Eigen::MatrixXd::Ones(8, 2)), std::invalid_argument);
    }

    TEST(EigenInsertionMatrixTest, ExplicitInterpretationMustMatchTheRows)
    {
        DataVariant variant;
        EigenInsertion insertion(variant);

        EXPECT_THROW(insertion.insert_matrix(Eigen::MatrixXd::Ones(4, 2), MatrixInterpretation::VEC3), std::invalid_argument);
        EXPECT_THROW(insertion.insert_matrix(Eigen::MatrixXd::Ones(3, 2), MatrixInterpretation::SCALAR), std::invalid_argument);
        EXPECT_THROW(insertion.insert_matrix(Eigen::MatrixXd::Ones(3, 2), MatrixInterpretation::COMPLEX), std::invalid_argument);
        EXPECT_THROW(insertion.insert_matrix(Eigen::MatrixXd::Ones(3, 2), MatrixInterpretation::MAT4), std::invalid_argument);
    }

    TEST(EigenInsertionMatrixTest, ReplacesEarlierContent)
    {
        DataVariant variant = std::vector<glm::vec3>(4);
        EigenInsertion insertion(variant);

        insertion.insert_matrix(Eigen::MatrixXd::Ones(1, 2));

        EXPECT_TRUE(std::holds_alternative<std::vector<double>>(variant));
        EXPECT_EQ(std::get<std::vector<double>>(variant).size(), 2U);
    }

    TEST(EigenInsertionMatrixTest, MatrixWithNoColumnsGivesEmptyStorage)
    {
        DataVariant variant;
        EigenInsertion insertion(variant);

        insertion.insert_matrix(Eigen::MatrixXd(3, 0));

        EXPECT_TRUE(std::get<std::vector<glm::vec3>>(variant).empty());
    }

    TEST(EigenInsertionClearTest, LeavesAnEmptyDoubleVector)
    {
        DataVariant variant = std::vector<glm::vec3>(4);
        EigenInsertion insertion(variant);

        insertion.clear();

        ASSERT_TRUE(std::holds_alternative<std::vector<double>>(variant));
        EXPECT_TRUE(std::get<std::vector<double>>(variant).empty());
    }

    TEST(EigenInsertionConvenienceTest, MatrixFunctionInfersTheType)
    {
        const DataVariant variant = Kakshya::from_eigen_matrix(Eigen::MatrixXd::Ones(3, 2));

        EXPECT_TRUE(std::holds_alternative<std::vector<glm::vec3>>(variant));
    }

    TEST(EigenInsertionConvenienceTest, MatrixFunctionAcceptsAnInterpretation)
    {
        const DataVariant variant = Kakshya::from_eigen_matrix(Eigen::MatrixXd::Ones(2, 2), MatrixInterpretation::COMPLEX);

        EXPECT_TRUE(std::holds_alternative<std::vector<std::complex<double>>>(variant));
    }

    TEST(EigenInsertionConvenienceTest, VectorFunctionGivesDoubles)
    {
        const DataVariant variant = Kakshya::from_eigen_vector(Eigen::VectorXd::Ones(4));

        EXPECT_EQ(std::get<std::vector<double>>(variant).size(), 4U);
    }

    TEST(EigenRoundTripTest, StructuredDataSurvivesAccessThenInsertion)
    {
        const std::vector<DataVariant> sources {
            std::vector<glm::vec3> { { 1, 2, 3 }, { 4, 5, 6 } },
            std::vector<glm::vec4> { { 1, 2, 3, 4 }, { 5, 6, 7, 8 } },
            std::vector<glm::mat4> { glm::mat4(2.0F), glm::translate(glm::mat4(1.0F), glm::vec3(1, 2, 3)) },
            std::vector<double> { 0.5, 1.5, 2.5 },
        };

        for (const auto& source : sources) {
            DataVariant rebuilt;
            EigenInsertion(rebuilt).insert_matrix(EigenAccess(source).to_matrix());

            EXPECT_EQ(rebuilt, source) << source.index();
        }
    }

    TEST(EigenRoundTripTest, ComplexDataSurvivesWithAnExplicitInterpretation)
    {
        const DataVariant source = std::vector<std::complex<double>> { { 1, 2 }, { 3, 4 } };
        DataVariant rebuilt;

        EigenInsertion(rebuilt).insert_matrix(EigenAccess(source).to_matrix(), MatrixInterpretation::COMPLEX);

        EXPECT_EQ(rebuilt, source);
    }

} // namespace
} // namespace MayaFlux::Test
