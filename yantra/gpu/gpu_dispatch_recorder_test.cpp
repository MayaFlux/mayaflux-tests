#include "gtest/gtest.h"
#include "../../support/test_metadata.hpp"

#include "../../support/gpu_dispatch_test_support.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::vulkan>() }))

namespace MayaFlux::Test {
namespace {

    using namespace GpuDispatch;
    using Portal::Graphics::INVALID_FENCE;
    using Yantra::GpuDispatchRecorder;

    template <typename Fixture>
    class GpuDispatchRecorderTest : public Fixture {
    };

    using HardwareOptions = ::testing::Types<GpuDispatchFixture<true>, GpuDispatchFixture<false>>;
    TYPED_TEST_SUITE(GpuDispatchRecorderTest, HardwareOptions);

    TYPED_TEST(GpuDispatchRecorderTest, CommandBeginsLazilyAndIsOpenFollowsIt)
    {
        GpuDispatchRecorder recording(true);
        EXPECT_FALSE(recording.is_open());

        const auto first = recording.command();
        EXPECT_TRUE(recording.is_open());
        EXPECT_EQ(recording.command(), first);

        recording.wait();
        EXPECT_FALSE(recording.is_open());
    }

    TYPED_TEST(GpuDispatchRecorderTest, ReportsItsIsolationSetting)
    {
        EXPECT_TRUE(GpuDispatchRecorder(true).compute_isolated());
        EXPECT_FALSE(GpuDispatchRecorder(false).compute_isolated());
    }

    TYPED_TEST(GpuDispatchRecorderTest, SubmitWithNothingRecordedReturnsInvalidWhenNothingIsOutstanding)
    {
        GpuDispatchRecorder recording(true);

        EXPECT_EQ(recording.submit(), INVALID_FENCE);
        EXPECT_FALSE(recording.is_open());
    }

    TYPED_TEST(GpuDispatchRecorderTest, SubmitClosesTheRecordingAndReturnsItsFenceUntilTheNextSubmit)
    {
        GpuDispatchRecorder recording(true);
        static_cast<void>(recording.command());

        const auto fence = recording.submit();
        ASSERT_NE(fence, INVALID_FENCE);
        EXPECT_FALSE(recording.is_open());
        EXPECT_EQ(recording.submit(), fence);

        recording.wait();
    }

    TYPED_TEST(GpuDispatchRecorderTest, IsCompleteAnswersTrueForInvalidAndSignaledFences)
    {
        GpuDispatchRecorder recording(true);
        EXPECT_TRUE(recording.is_complete(INVALID_FENCE));

        static_cast<void>(recording.command());
        const auto fence = recording.submit();
        ASSERT_NE(fence, INVALID_FENCE);
        foundry().wait_for_fence(fence);

        EXPECT_TRUE(recording.is_complete(fence));
        recording.wait();
    }

    TYPED_TEST(GpuDispatchRecorderTest, AnEarlierFenceCountsAsCompleteOnceTheLatestSignals)
    {
        GpuDispatchRecorder recording(true);
        static_cast<void>(recording.command());
        const auto first = recording.submit();
        ASSERT_NE(first, INVALID_FENCE);
        static_cast<void>(recording.command());
        const auto second = recording.submit();
        ASSERT_NE(second, INVALID_FENCE);

        foundry().wait_for_fence(second);

        EXPECT_TRUE(recording.is_complete(first));
        recording.wait();
    }

    TYPED_TEST(GpuDispatchRecorderTest, WaitLeavesNothingOutstanding)
    {
        GpuDispatchRecorder recording(true);
        static_cast<void>(recording.command());
        static_cast<void>(recording.submit());
        static_cast<void>(recording.command());

        recording.wait();

        EXPECT_FALSE(recording.is_open());
        EXPECT_EQ(recording.submit(), INVALID_FENCE);
        EXPECT_NO_THROW(recording.wait());
    }

    TYPED_TEST(GpuDispatchRecorderTest, ReleaseAfterWhileOpenRunsOnlyOnceTheSubmissionCompletes)
    {
        GpuDispatchRecorder recording(true);
        const auto released = std::make_shared<int>(0);
        static_cast<void>(recording.command());
        recording.release_after([released] { ++*released; });
        EXPECT_EQ(*released, 0);

        static_cast<void>(recording.submit());
        recording.wait();
        flush_retired_resources();

        EXPECT_EQ(*released, 1);
    }

    TYPED_TEST(GpuDispatchRecorderTest, ReleaseAfterWhileClosedRunsOnceTheLastSubmissionCompletes)
    {
        GpuDispatchRecorder recording(true);
        static_cast<void>(recording.command());
        static_cast<void>(recording.submit());
        const auto released = std::make_shared<int>(0);

        recording.release_after([released] { ++*released; });
        recording.wait();
        flush_retired_resources();

        EXPECT_EQ(*released, 1);
    }

    TYPED_TEST(GpuDispatchRecorderTest, ReleaseAfterWithAnEmptyFunctionIsIgnored)
    {
        GpuDispatchRecorder recording(true);
        static_cast<void>(recording.command());

        EXPECT_NO_THROW(recording.release_after({}));
        recording.wait();
    }

    TYPED_TEST(GpuDispatchRecorderTest, DestroyingAnOpenRecordingSubmitsAndWaitsForIt)
    {
        const auto released = std::make_shared<int>(0);
        {
            GpuDispatchRecorder recording(true);
            static_cast<void>(recording.command());
            recording.release_after([released] { ++*released; });
        }
        flush_retired_resources();

        EXPECT_EQ(*released, 1);
    }

} // namespace
} // namespace MayaFlux::Test
