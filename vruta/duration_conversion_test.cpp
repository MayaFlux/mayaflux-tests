#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Vruta/ChronUtils.hpp"
#include "MayaFlux/Vruta/Scheduler.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

using SchedToken = Vruta::ProcessingToken;

namespace {

    constexpr unsigned int DURATION_CONVERSION_SAMPLE_RATE = 48000;
    constexpr unsigned int FRAME_RATE = 60;

} // namespace

struct ConversionLimitCase {
    const char* name;
    double seconds;
    uint64_t expected;
};

class ConversionLimitTest : public ::testing::TestWithParam<ConversionLimitCase> {
protected:
    void SetUp() override
    {
        scheduler = std::make_shared<Vruta::TaskScheduler>(DURATION_CONVERSION_SAMPLE_RATE, FRAME_RATE);
    }

    void TearDown() override
    {
        scheduler.reset();
    }

    std::shared_ptr<Vruta::TaskScheduler> scheduler;
};

TEST_P(ConversionLimitTest, FreeFunctionsReturnTheDefinedValue)
{
    const auto& param = GetParam();

    EXPECT_EQ(Vruta::seconds_to_units(param.seconds, DURATION_CONVERSION_SAMPLE_RATE), param.expected);
    EXPECT_EQ(Vruta::seconds_to_samples(param.seconds, DURATION_CONVERSION_SAMPLE_RATE), param.expected);
}

TEST_P(ConversionLimitTest, SchedulerMatchesTheFreeFunctions)
{
    const auto& param = GetParam();

    EXPECT_EQ(scheduler->seconds_to_samples(param.seconds), Vruta::seconds_to_samples(param.seconds, DURATION_CONVERSION_SAMPLE_RATE));
    EXPECT_EQ(scheduler->seconds_to_units(param.seconds, SchedToken::SAMPLE_ACCURATE), Vruta::seconds_to_units(param.seconds, DURATION_CONVERSION_SAMPLE_RATE));
    EXPECT_EQ(scheduler->seconds_to_units(param.seconds, SchedToken::FRAME_ACCURATE), Vruta::seconds_to_frames(param.seconds, FRAME_RATE));
}

INSTANTIATE_TEST_SUITE_P(
    Seconds,
    ConversionLimitTest,
    ::testing::Values(
        ConversionLimitCase { "Zero", 0.0, 0 },
        ConversionLimitCase { "Negative", -1.0, 0 },
        ConversionLimitCase { "NegativeInfinite", -std::numeric_limits<double>::infinity(), 0 },
        ConversionLimitCase { "NaN", std::numeric_limits<double>::quiet_NaN(), 0 },
        ConversionLimitCase { "OneBillion", 1.0e9, 48000000000000ULL },
        ConversionLimitCase { "Infinite", std::numeric_limits<double>::infinity(), Vruta::max_duration_units },
        ConversionLimitCase { "Huge", 1.0e300, Vruta::max_duration_units },
        ConversionLimitCase { "PastTheLimit", 4.0e14, Vruta::max_duration_units }),
    [](const ::testing::TestParamInfo<ConversionLimitCase>& info) { return std::string(info.param.name); });

TEST(DurationConversionLimit, LeavesRoomToAddAClockPosition)
{
    EXPECT_GE(std::numeric_limits<uint64_t>::max() - Vruta::max_duration_units, Vruta::max_duration_units);
}

TEST(DurationConversionLimit, FrameConversionIsLimitedToo)
{
    EXPECT_EQ(Vruta::seconds_to_frames(std::numeric_limits<double>::infinity(), FRAME_RATE), Vruta::max_duration_units);
    EXPECT_EQ(Vruta::seconds_to_frames(-1.0, FRAME_RATE), 0U);
    EXPECT_EQ(Vruta::seconds_to_frames(2.0, FRAME_RATE), 120U);
}

}
