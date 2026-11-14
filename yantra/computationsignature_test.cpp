#include "MayaFlux/Yantra/Analyzers/EnergyAnalyzer.hpp"
#include "MayaFlux/Yantra/Analyzers/StatisticalAnalyzer.hpp"
#include "MayaFlux/Yantra/Extractors/FeatureExtractor.hpp"
#include "MayaFlux/Yantra/OperationSpec/ComputationSignature.hpp"
#include "MayaFlux/Yantra/Transformers/MathematicalTransformer.hpp"
#include "MayaFlux/Yantra/Transformers/TemporalTransformer.hpp"

#include <gtest/gtest.h>
#include "../support/test_metadata.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

using namespace MayaFlux::Yantra;

namespace MayaFlux::Test::SignatureExternal {

enum class Stage { Prepare, Process };
enum Method { First = 1, Second = 2 };
enum class Flags : unsigned { Read = 1, Write = 2 };

constexpr Flags operator|(Flags left, Flags right)
{
    return static_cast<Flags>(static_cast<unsigned>(left) | static_cast<unsigned>(right));
}

}

namespace MayaFlux::Test::SignatureOptIn {

enum Method { Begin = 1, Finish = 2 };
void computation_enum(Method);

}

namespace MayaFlux::Test {
namespace {

    template <typename Left, typename Right>
    concept StartsSignature = requires(Left left, Right right) {
        { left | right } -> std::same_as<ComputationSignature>;
    };

    static_assert(std::same_as<decltype(Eigen::AutoAlign | Eigen::ColMajor), int>);
    static_assert(std::same_as<decltype(Eigen::AutoAlign | Eigen::RowMajor), int>);
    static_assert(!StartsSignature<Eigen::StorageOptions, Eigen::StorageOptions>);
    static_assert(!StartsSignature<SignatureExternal::Method, SignatureExternal::Method>);
    static_assert(std::same_as<decltype(SignatureExternal::First | SignatureExternal::Second), int>);
    static_assert(std::same_as<decltype(SignatureExternal::Flags::Read | SignatureExternal::Flags::Write), SignatureExternal::Flags>);
    static_assert(StartsSignature<SignatureExternal::Stage, SignatureExternal::Stage>);
    static_assert(StartsSignature<SignatureOptIn::Method, SignatureOptIn::Method>);
    static_assert(StartsSignature<EnergyMethod, StatisticalMethod>);
    static_assert(StartsSignature<ExtractionMethod, ExtractionMethod>);

    TEST(ComputationSignatureTest, EigenExpressionsKeepTheirNativeStorageFlags)
    {
        Eigen::MatrixXd identity = Eigen::MatrixXd::Identity(3, 3);
        Eigen::MatrixXd zero = Eigen::MatrixXd::Zero(3, 3);
        Eigen::VectorXd input(3);
        input << 2, -3, 7;
        const Eigen::VectorXd output = identity * input;
        EXPECT_DOUBLE_EQ(output[0], 2);
        EXPECT_DOUBLE_EQ(output[1], -3);
        EXPECT_DOUBLE_EQ(output[2], 7);
        EXPECT_DOUBLE_EQ(output.head(2).sum(), -1);
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) {
                EXPECT_DOUBLE_EQ(identity(row, column), row == column ? 1 : 0);
                EXPECT_DOUBLE_EQ(zero(row, column), 0);
            }
        }
        EXPECT_EQ(Eigen::AutoAlign | Eigen::RowMajor,
            static_cast<int>(Eigen::AutoAlign) | static_cast<int>(Eigen::RowMajor));
    }

    TEST(ComputationSignatureTest, ScopedYantraSelectionsRetainTheirOrderAndTypes)
    {
        const auto signature = MathematicalOperation::GAIN | TemporalOperation::TIME_REVERSE
            | MathematicalOperation::GAIN;
        ASSERT_EQ(signature.size(), 3);
        EXPECT_EQ(signature.selection<MathematicalOperation>(0), MathematicalOperation::GAIN);
        EXPECT_EQ(signature.selection<TemporalOperation>(1), TemporalOperation::TIME_REVERSE);
        EXPECT_EQ(signature.selection<MathematicalOperation>(2), MathematicalOperation::GAIN);
        EXPECT_FALSE(signature.selection<TemporalOperation>(0).has_value());
    }

    TEST(ComputationSignatureTest, UnscopedYantraSelectionsComposeWithoutIndividualOptIn)
    {
        const auto signature = EnergyMethod::RMS | StatisticalMethod::MEAN | ExtractionMethod::PEAK_DATA;
        ASSERT_EQ(signature.size(), 3);
        EXPECT_EQ(signature.selection<EnergyMethod>(0), EnergyMethod::RMS);
        EXPECT_EQ(signature.selection<StatisticalMethod>(1), StatisticalMethod::MEAN);
        EXPECT_EQ(signature.selection<ExtractionMethod>(2), ExtractionMethod::PEAK_DATA);
        const auto repeated = ExtractionMethod::PEAK_DATA | ExtractionMethod::PEAK_DATA;
        ASSERT_EQ(repeated.size(), 2);
        EXPECT_EQ(repeated.selection<ExtractionMethod>(1), ExtractionMethod::PEAK_DATA);
    }

    TEST(ComputationSignatureTest, ExternalScopedSelectionsNeedNoOptIn)
    {
        const auto signature = SignatureExternal::Stage::Prepare | SignatureExternal::Stage::Process
            | MathematicalOperation::GAIN;
        ASSERT_EQ(signature.size(), 3);
        EXPECT_EQ(signature.selection<SignatureExternal::Stage>(0), SignatureExternal::Stage::Prepare);
        EXPECT_EQ(signature.selection<SignatureExternal::Stage>(1), SignatureExternal::Stage::Process);
        EXPECT_EQ(signature.selection<MathematicalOperation>(2), MathematicalOperation::GAIN);
    }

    TEST(ComputationSignatureTest, ExternalUnscopedSelectionsCanExtendAnExplicitSignature)
    {
        EXPECT_EQ(SignatureExternal::First | SignatureExternal::Second, 3);
        const auto signature = ComputationSignature { SignatureExternal::First }
            | SignatureExternal::Second | SignatureExternal::First;
        ASSERT_EQ(signature.size(), 3);
        EXPECT_EQ(signature.selection<SignatureExternal::Method>(0), SignatureExternal::First);
        EXPECT_EQ(signature.selection<SignatureExternal::Method>(1), SignatureExternal::Second);
        EXPECT_EQ(signature.selection<SignatureExternal::Method>(2), SignatureExternal::First);
    }

    TEST(ComputationSignatureTest, ScopedAndExternalUnscopedSelectionsCanStartAChain)
    {
        const auto signature = SignatureExternal::Stage::Prepare | SignatureExternal::First;
        ASSERT_EQ(signature.size(), 2);
        EXPECT_EQ(signature.selection<SignatureExternal::Stage>(0), SignatureExternal::Stage::Prepare);
        EXPECT_EQ(signature.selection<SignatureExternal::Method>(1), SignatureExternal::First);
    }

    TEST(ComputationSignatureTest, ExternalUnscopedNamespaceCanOptInThroughAdl)
    {
        const auto signature = SignatureOptIn::Begin | SignatureOptIn::Finish;
        ASSERT_EQ(signature.size(), 2);
        EXPECT_EQ(signature.selection<SignatureOptIn::Method>(0), SignatureOptIn::Begin);
        EXPECT_EQ(signature.selection<SignatureOptIn::Method>(1), SignatureOptIn::Finish);
    }

    TEST(ComputationSignatureTest, ExistingScopedFlagOperatorsKeepTheirMeaning)
    {
        const auto flags = SignatureExternal::Flags::Read | SignatureExternal::Flags::Write;
        EXPECT_EQ(static_cast<unsigned>(flags), 3);
    }

}
}
