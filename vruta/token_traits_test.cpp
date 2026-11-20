#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kriya/RoutineDomain.hpp"
#include "MayaFlux/Vruta/ChronUtils.hpp"
#include "MayaFlux/Vruta/Scheduler.hpp"
#include "MayaFlux/Vruta/TokenTraits.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

using SchedToken = Vruta::ProcessingToken;

namespace {

    constexpr unsigned int SAMPLE_RATE = 48000;
    constexpr unsigned int FRAME_RATE = 60;

    using SampleDomain = Kriya::RoutineDomain<SchedToken::SAMPLE_ACCURATE>;
    using FrameDomain = Kriya::RoutineDomain<SchedToken::FRAME_ACCURATE>;
    using CrossDomain = Kriya::RoutineDomain<SchedToken::MULTI_RATE>;

    static_assert(std::same_as<Vruta::TokenTraits<SchedToken::SAMPLE_ACCURATE>::routine_type, Vruta::SoundRoutine>);
    static_assert(std::same_as<Vruta::TokenTraits<SchedToken::FRAME_ACCURATE>::routine_type, Vruta::GraphicsRoutine>);
    static_assert(std::same_as<Vruta::TokenTraits<SchedToken::MULTI_RATE>::routine_type, Vruta::CrossRoutine>);
    static_assert(std::same_as<Vruta::TokenTraits<SchedToken::CONDITIONAL>::routine_type, Vruta::FreeRoutine>);
    static_assert(std::is_void_v<Vruta::TokenTraits<SchedToken::ON_DEMAND>::routine_type>);
    static_assert(std::is_void_v<Vruta::TokenTraits<SchedToken::CUSTOM>::routine_type>);
    static_assert(std::is_void_v<Vruta::TokenTraits<SchedToken::EVENT_DRIVEN>::routine_type>);

    static_assert(std::same_as<Vruta::TokenTraits<SchedToken::SAMPLE_ACCURATE>::clock_type, Vruta::SampleClock>);
    static_assert(std::same_as<Vruta::TokenTraits<SchedToken::FRAME_ACCURATE>::clock_type, Vruta::FrameClock>);
    static_assert(std::same_as<Vruta::TokenTraits<SchedToken::MULTI_RATE>::clock_type, Vruta::SampleClock>);
    static_assert(std::is_void_v<Vruta::TokenTraits<SchedToken::CONDITIONAL>::clock_type>);
    static_assert(std::same_as<Vruta::TokenTraits<SchedToken::ON_DEMAND>::clock_type, Vruta::SampleClock>);
    static_assert(std::same_as<Vruta::TokenTraits<SchedToken::CUSTOM>::clock_type, Vruta::SampleClock>);
    static_assert(std::same_as<Vruta::TokenTraits<SchedToken::EVENT_DRIVEN>::clock_type, Vruta::SampleClock>);

    static_assert(std::same_as<SampleDomain::Routine, Vruta::SoundRoutine>);
    static_assert(std::same_as<FrameDomain::Routine, Vruta::GraphicsRoutine>);
    static_assert(std::same_as<CrossDomain::Routine, Vruta::CrossRoutine>);

    static_assert(std::same_as<SampleDomain::GetPromise, Kriya::GetAudioPromise>);
    static_assert(std::same_as<FrameDomain::GetPromise, Kriya::GetGraphicsPromise>);
    static_assert(std::same_as<CrossDomain::GetPromise, Kriya::GetCrossPromise>);

    static_assert(std::same_as<decltype(SampleDomain::delay(1)), Kriya::SampleDelay>);
    static_assert(std::same_as<decltype(FrameDomain::delay(1)), Kriya::FrameDelay>);
    static_assert(std::same_as<decltype(CrossDomain::delay(1)), Kriya::MultiRateDelay>);

} // namespace

class TokenTraitsTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        scheduler = std::make_shared<Vruta::TaskScheduler>(SAMPLE_RATE, FRAME_RATE);
    }

    void TearDown() override
    {
        scheduler.reset();
    }

    void register_noop(SchedToken token)
    {
        scheduler->register_token_processor(
            token, [](const std::vector<std::shared_ptr<Vruta::Routine>>&, uint64_t) { });
    }

    std::shared_ptr<Vruta::TaskScheduler> scheduler;
};

struct ClockCase {
    const char* name;
    SchedToken token;
    bool frame;
    uint32_t rate;
};

class TokenClockTest : public TokenTraitsTest, public ::testing::WithParamInterface<ClockCase> { };

TEST_P(TokenClockTest, SchedulerCreatesTheClockTheTraitNames)
{
    const auto& param = GetParam();
    register_noop(param.token);

    const auto& clock = scheduler->get_clock(param.token);

    EXPECT_EQ(dynamic_cast<const Vruta::FrameClock*>(&clock) != nullptr, param.frame);
    EXPECT_EQ(dynamic_cast<const Vruta::SampleClock*>(&clock) != nullptr, !param.frame);
    EXPECT_EQ(clock.rate(), param.rate);
    EXPECT_EQ(scheduler->get_rate(param.token), param.rate);
}

INSTANTIATE_TEST_SUITE_P(
    Tokens,
    TokenClockTest,
    ::testing::Values(
        ClockCase { "Sample", SchedToken::SAMPLE_ACCURATE, false, SAMPLE_RATE },
        ClockCase { "Frame", SchedToken::FRAME_ACCURATE, true, FRAME_RATE },
        ClockCase { "MultiRate", SchedToken::MULTI_RATE, false, SAMPLE_RATE },
        ClockCase { "OnDemand", SchedToken::ON_DEMAND, false, 1 },
        ClockCase { "Custom", SchedToken::CUSTOM, false, 1000 },
        ClockCase { "EventDriven", SchedToken::EVENT_DRIVEN, false, SAMPLE_RATE }),
    [](const ::testing::TestParamInfo<ClockCase>& info) { return std::string(info.param.name); });

TEST_F(TokenTraitsTest, ConditionalTokenGetsNoClock)
{
    register_noop(SchedToken::CONDITIONAL);

    EXPECT_EQ(scheduler->get_rate(SchedToken::CONDITIONAL), 0U);
}

struct VisitCase {
    const char* name;
    SchedToken token;
    SchedToken visited;
};

class VisitTokenTest : public ::testing::TestWithParam<VisitCase> { };

TEST_P(VisitTokenTest, VisitorReceivesTheMatchingConstant)
{
    const auto& param = GetParam();

    const auto visited = Vruta::visit_token(param.token, [](auto tag) { return decltype(tag)::value; });

    EXPECT_EQ(visited, param.visited);
}

INSTANTIATE_TEST_SUITE_P(
    Tokens,
    VisitTokenTest,
    ::testing::Values(
        VisitCase { "Sample", SchedToken::SAMPLE_ACCURATE, SchedToken::SAMPLE_ACCURATE },
        VisitCase { "Frame", SchedToken::FRAME_ACCURATE, SchedToken::FRAME_ACCURATE },
        VisitCase { "MultiRate", SchedToken::MULTI_RATE, SchedToken::MULTI_RATE },
        VisitCase { "Conditional", SchedToken::CONDITIONAL, SchedToken::CONDITIONAL },
        VisitCase { "OnDemand", SchedToken::ON_DEMAND, SchedToken::ON_DEMAND },
        VisitCase { "Custom", SchedToken::CUSTOM, SchedToken::ON_DEMAND },
        VisitCase { "EventDriven", SchedToken::EVENT_DRIVEN, SchedToken::ON_DEMAND }),
    [](const ::testing::TestParamInfo<VisitCase>& info) { return std::string(info.param.name); });

TEST_F(TokenTraitsTest, DomainUnitsFollowTheTokenClock)
{
    EXPECT_EQ(SampleDomain::units(0.5), Vruta::seconds_to_samples(0.5));
    EXPECT_EQ(SampleDomain::units(0.5), SAMPLE_RATE / 2);
    EXPECT_EQ(FrameDomain::units(1.0), Vruta::seconds_to_frames(1.0));
    EXPECT_EQ(FrameDomain::units(1.0), FRAME_RATE);
    EXPECT_EQ(CrossDomain::units(0.5), Vruta::seconds_to_samples(0.5));
}

TEST_F(TokenTraitsTest, DomainUnitsAreLimited)
{
    constexpr double infinite = std::numeric_limits<double>::infinity();

    EXPECT_EQ(SampleDomain::units(infinite), Vruta::max_duration_units);
    EXPECT_EQ(FrameDomain::units(infinite), Vruta::max_duration_units);
    EXPECT_EQ(CrossDomain::units(infinite), Vruta::max_duration_units);
    EXPECT_EQ(SampleDomain::units(-1.0), 0U);
    EXPECT_EQ(FrameDomain::units(std::numeric_limits<double>::quiet_NaN()), 0U);
}

TEST_F(TokenTraitsTest, DomainDelaysCarryTheirUnitCount)
{
    EXPECT_EQ(SampleDomain::delay(7).samples_to_wait, 7U);
    EXPECT_EQ(FrameDomain::delay(7).frames_to_wait, 7U);
    EXPECT_EQ(CrossDomain::delay(7).samples_to_wait, 7U);
    EXPECT_EQ(CrossDomain::delay(7).frames_to_wait, 0U);

    EXPECT_EQ(SampleDomain::pause().samples_to_wait, 1U);
    EXPECT_EQ(FrameDomain::pause().frames_to_wait, 1U);
    EXPECT_EQ(CrossDomain::pause().samples_to_wait, 1U);
}

}
