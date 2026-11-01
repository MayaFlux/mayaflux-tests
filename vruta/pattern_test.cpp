#include "gtest/gtest.h"

#include "MayaFlux/Nodes/Generators/Logic.hpp"
#include "MayaFlux/Vruta/Scheduler.hpp"

#include "MayaFlux/MayaFlux.hpp"

namespace MayaFlux::Test {

using SchedToken = Vruta::ProcessingToken;

namespace {

    constexpr unsigned int SAMPLE_RATE = 48000;

    template <typename T>
    class Log {
    public:
        void add(T value)
        {
            std::lock_guard<std::mutex> lock(m_guard);
            m_items.push_back(std::move(value));
        }

        [[nodiscard]] std::vector<T> items() const
        {
            std::lock_guard<std::mutex> lock(m_guard);
            return m_items;
        }

    private:
        mutable std::mutex m_guard;
        std::vector<T> m_items;
    };

    std::vector<uint64_t> first_n(size_t count)
    {
        std::vector<uint64_t> out;
        out.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            out.push_back(i);
        }
        return out;
    }

    size_t ones(const std::string& mask)
    {
        return static_cast<size_t>(std::ranges::count(mask, '1'));
    }

    size_t rises(const std::string& mask)
    {
        size_t count = 0;
        bool was = false;
        for (const char symbol : mask) {
            const bool on = symbol == '1';
            if (on && !was) {
                ++count;
            }
            was = on;
        }
        return count;
    }

    auto level_condition(std::string mask)
    {
        return [mask = std::move(mask), calls = size_t { 0 }](uint64_t) mutable {
            const bool on = calls < mask.size() && mask[calls] == '1';
            ++calls;
            return on;
        };
    }

    auto rise_condition(std::string mask)
    {
        return [mask = std::move(mask), calls = size_t { 0 }, was = false](uint64_t) mutable {
            const bool on = calls < mask.size() && mask[calls] == '1';
            ++calls;
            const bool rose = on && !was;
            was = on;
            return rose;
        };
    }

    const char* token_name(SchedToken token)
    {
        switch (token) {
        case SchedToken::SAMPLE_ACCURATE:
            return "Sample";
        case SchedToken::FRAME_ACCURATE:
            return "Frame";
        case SchedToken::MULTI_RATE:
            return "Cross";
        case SchedToken::CONDITIONAL:
            return "Free";
        case SchedToken::EVENT_DRIVEN:
            return "EventDriven";
        case SchedToken::ON_DEMAND:
            return "OnDemand";
        case SchedToken::CUSTOM:
            return "Custom";
        }
        return "Unknown";
    }

    std::string milliseconds(double seconds)
    {
        return "Ms" + std::to_string(static_cast<int>(std::lround(seconds * 1000.0)));
    }

} // namespace

template <typename Base>
class PatternFixture : public Base {
protected:
    void SetUp() override
    {
        scheduler = std::make_shared<Vruta::TaskScheduler>(SAMPLE_RATE);
    }

    void TearDown() override
    {
        scheduler.reset();
    }

    void run_samples(uint64_t samples)
    {
        scheduler->process_token(SchedToken::SAMPLE_ACCURATE, samples);
    }

    void run_seconds(double seconds)
    {
        run_samples(scheduler->seconds_to_samples(seconds));
    }

    void advance(const std::shared_ptr<Vruta::Routine>& routine, SchedToken token, uint64_t units)
    {
        if (token == SchedToken::FRAME_ACCURATE) {
            for (uint64_t i = 0; i < units; ++i) {
                routine->try_resume_with_context(m_frame_position++, Vruta::DelayContext::FRAME_BASED);
            }
            return;
        }

        if (m_added.insert(routine.get()).second) {
            scheduler->add_task(routine);
        }
        scheduler->process_token(token, units);
    }

    std::shared_ptr<Vruta::TaskScheduler> scheduler;

private:
    std::set<const Vruta::Routine*> m_added;
    uint64_t m_frame_position { 0 };
};

using PatternTest = PatternFixture<::testing::Test>;

template <typename Param>
using PatternParamTest = PatternFixture<::testing::TestWithParam<Param>>;

//-----------------------------------------------------------------------------
// pattern: duration forms
//-----------------------------------------------------------------------------

class IntervalPatternTest : public PatternParamTest<double> { };

TEST_P(IntervalPatternTest, ProducesTheGeneratedValuesInOrder)
{
    const double interval = GetParam();
    std::vector<int> received;

    auto routine = Kriya::pattern(
        [](uint64_t index) -> std::any { return static_cast<int>(index * 10); },
        [&](std::any value) { received.push_back(std::any_cast<int>(value)); },
        std::vector<double> { interval });

    scheduler->add_task(routine);

    run_seconds(interval * 0.9);
    ASSERT_EQ(received.size(), 1U);
    EXPECT_EQ(received[0], 0);

    run_seconds(interval * 2);
    ASSERT_EQ(received.size(), 3U);
    EXPECT_EQ(received[1], 10);
    EXPECT_EQ(received[2], 20);

    EXPECT_TRUE(scheduler->cancel_task(routine));
}

INSTANTIATE_TEST_SUITE_P(
    Intervals,
    IntervalPatternTest,
    ::testing::Values(0.004, 0.01, 0.02),
    [](const ::testing::TestParamInfo<double>& info) { return milliseconds(info.param); });

struct DurationCase {
    const char* name;
    std::vector<double> durations;
    std::vector<std::pair<double, int>> checkpoints;
};

class DurationListTest : public PatternParamTest<DurationCase> { };

TEST_P(DurationListTest, StepsFollowTheListAndCycle)
{
    const auto& param = GetParam();
    auto seen = std::make_shared<Log<uint64_t>>();

    auto routine = Kriya::pattern(
        [](uint64_t index) { return index; },
        [seen](uint64_t index) { seen->add(index); },
        param.durations);

    ASSERT_NE(routine, nullptr);
    scheduler->add_task(routine);

    for (const auto& [seconds, expected] : param.checkpoints) {
        run_seconds(seconds);
        EXPECT_EQ(static_cast<int>(seen->items().size()), expected);
    }

    EXPECT_EQ(seen->items(), first_n(seen->items().size()));
}

INSTANTIATE_TEST_SUITE_P(
    Lists,
    DurationListTest,
    ::testing::Values(
        DurationCase { "Single", { 0.25 }, { { 0.125, 1 }, { 0.25, 2 }, { 0.25, 3 } } },
        DurationCase { "TwoCycling", { 0.25, 0.5 }, { { 0.125, 1 }, { 0.25, 2 }, { 0.5, 3 }, { 0.25, 4 } } },
        DurationCase { "Descending", { 0.5, 0.25, 0.125 }, { { 0.25, 1 }, { 0.375, 2 }, { 0.1875, 3 }, { 0.125, 4 }, { 0.3125, 4 }, { 0.25, 5 } } }),
    [](const ::testing::TestParamInfo<DurationCase>& info) { return std::string(info.param.name); });

struct DomainCase {
    const char* name;
    SchedToken token;
    uint64_t units;
};

class DurationDomainTest : public PatternParamTest<DomainCase> { };

TEST_P(DurationDomainTest, StepsFollowTheDomainUnits)
{
    const auto& param = GetParam();
    auto seen = std::make_shared<Log<uint64_t>>();

    auto routine = Kriya::pattern(
        [](uint64_t index) { return index; },
        [seen](uint64_t index) { seen->add(index); },
        std::vector<double> { 0.25, 0.5 },
        param.token);

    ASSERT_NE(routine, nullptr);
    advance(routine, param.token, param.units);

    EXPECT_EQ(seen->items(), first_n(3));
}

INSTANTIATE_TEST_SUITE_P(
    Domains,
    DurationDomainTest,
    ::testing::Values(
        DomainCase { "Sample", SchedToken::SAMPLE_ACCURATE, 40000 },
        DomainCase { "Frame", SchedToken::FRAME_ACCURATE, 50 }),
    [](const ::testing::TestParamInfo<DomainCase>& info) { return std::string(info.param.name); });

struct ClampCase {
    const char* name;
    double value;
};

class DurationClampTest : public PatternParamTest<ClampCase> { };

TEST_P(DurationClampTest, DurationsAreAtLeastOneUnit)
{
    auto seen = std::make_shared<Log<uint64_t>>();

    auto routine = Kriya::pattern(
        [](uint64_t index) { return index; },
        [seen](uint64_t index) { seen->add(index); },
        std::vector<double> { GetParam().value });

    ASSERT_NE(routine, nullptr);
    scheduler->add_task(routine);
    run_samples(10);

    EXPECT_EQ(seen->items(), first_n(10));
}

INSTANTIATE_TEST_SUITE_P(
    Values,
    DurationClampTest,
    ::testing::Values(
        ClampCase { "Zero", 0.0 },
        ClampCase { "Negative", -1.0 },
        ClampCase { "NaN", std::numeric_limits<double>::quiet_NaN() }),
    [](const ::testing::TestParamInfo<ClampCase>& info) { return std::string(info.param.name); });

TEST_F(PatternTest, BracedDurationsBindDirectly)
{
    auto one = std::make_shared<Log<uint64_t>>();
    auto two = std::make_shared<Log<uint64_t>>();

    auto single = Kriya::pattern(
        [](uint64_t index) { return index; },
        [one](uint64_t index) { one->add(index); },
        { 0.25 });
    auto pair = Kriya::pattern(
        [](uint64_t index) { return index; },
        [two](uint64_t index) { two->add(index); },
        { 0.25, 0.5 });

    ASSERT_NE(single, nullptr);
    ASSERT_NE(pair, nullptr);
    scheduler->add_task(single);
    scheduler->add_task(pair);
    run_seconds(0.375);

    EXPECT_EQ(one->items(), first_n(2));
    EXPECT_EQ(two->items(), first_n(2));
}

TEST_F(PatternTest, EmptyDurationListReturnsNull)
{
    auto routine = Kriya::pattern(
        [](uint64_t index) { return index; },
        [](uint64_t) { },
        std::vector<double> {});

    EXPECT_EQ(routine, nullptr);
}

TEST_F(PatternTest, DurationFunctionIsAskedOncePerPassAndEndsOnAnEmptyArray)
{
    auto seen = std::make_shared<Log<uint64_t>>();
    auto passes = std::make_shared<Log<uint64_t>>();

    auto routine = Kriya::pattern(
        [](uint64_t index) { return index; },
        [seen](uint64_t index) { seen->add(index); },
        [passes](uint64_t pass) -> std::vector<double> {
            passes->add(pass);
            if (pass == 0) {
                return { 0.25, 0.25 };
            }
            if (pass == 1) {
                return { 0.5 };
            }
            return {};
        });

    ASSERT_NE(routine, nullptr);
    scheduler->add_task(routine);
    run_seconds(1.25);

    EXPECT_EQ(seen->items(), first_n(4));
    EXPECT_EQ(passes->items(), (std::vector<uint64_t> { 0, 1, 2 }));
    EXPECT_FALSE(routine->is_active());
}

TEST_F(PatternTest, DurationFunctionThatThrowsEndsThePattern)
{
    auto seen = std::make_shared<Log<uint64_t>>();

    auto routine = Kriya::pattern(
        [](uint64_t index) { return index; },
        [seen](uint64_t index) { seen->add(index); },
        [](uint64_t pass) -> std::vector<double> {
            if (pass == 1) {
                throw std::runtime_error("durations");
            }
            return { 0.25 };
        });

    ASSERT_NE(routine, nullptr);
    scheduler->add_task(routine);
    run_seconds(1.0);

    EXPECT_EQ(seen->items(), first_n(2));
    EXPECT_FALSE(routine->is_active());
}

TEST_F(PatternTest, DurationFunctionAcceptsAnyRangeOfNumbers)
{
    auto seen = std::make_shared<Log<uint64_t>>();

    auto routine = Kriya::pattern(
        [](uint64_t index) { return index; },
        [seen](uint64_t index) { seen->add(index); },
        [](uint64_t) { return std::array<float, 2> { 0.25F, 0.5F }; });

    ASSERT_NE(routine, nullptr);
    scheduler->add_task(routine);
    run_seconds(0.875);

    EXPECT_EQ(seen->items(), first_n(3));
}

//-----------------------------------------------------------------------------
// pattern: condition form
//-----------------------------------------------------------------------------

using MaskParam = std::tuple<SchedToken, std::string>;

std::string mask_name(const ::testing::TestParamInfo<MaskParam>& info)
{
    return std::string(token_name(std::get<0>(info.param))) + "_" + std::get<1>(info.param);
}

const auto sound_and_graphics = ::testing::Values(SchedToken::SAMPLE_ACCURATE, SchedToken::FRAME_ACCURATE);

class PatternLevelTest : public PatternParamTest<MaskParam> { };

TEST_P(PatternLevelTest, RunsOneStepPerSatisfiedCheck)
{
    const auto& [token, mask] = GetParam();
    auto seen = std::make_shared<Log<uint64_t>>();

    auto routine = Kriya::pattern(
        [](uint64_t index) { return index; },
        [seen](uint64_t index) { seen->add(index); },
        level_condition(mask),
        token);

    ASSERT_NE(routine, nullptr);
    advance(routine, token, mask.size());

    EXPECT_EQ(seen->items(), first_n(ones(mask)));
}

TEST_P(PatternLevelTest, IndexAdvancesOnlyWhenTheConditionHolds)
{
    const auto& [token, mask] = GetParam();
    auto asked = std::make_shared<Log<uint64_t>>();

    auto routine = Kriya::pattern(
        [](uint64_t index) { return index; },
        [](uint64_t) { },
        [asked, inner = level_condition(mask)](uint64_t index) mutable {
            asked->add(index);
            return inner(index);
        },
        token);

    ASSERT_NE(routine, nullptr);
    advance(routine, token, mask.size());

    const auto checks = asked->items();
    ASSERT_EQ(checks.size(), mask.size());

    uint64_t expected = 0;
    for (size_t i = 0; i < checks.size(); ++i) {
        EXPECT_EQ(checks[i], expected) << "check " << i;
        if (mask[i] == '1') {
            ++expected;
        }
    }
}

INSTANTIATE_TEST_SUITE_P(
    Masks,
    PatternLevelTest,
    ::testing::Combine(
        sound_and_graphics,
        ::testing::Values(
            std::string("1111111111"),
            std::string("0000011111"),
            std::string("1010101010"),
            std::string("0011100111"),
            std::string("0000000000"),
            std::string("1"))),
    mask_name);

class PatternEdgeTest : public PatternParamTest<MaskParam> { };

TEST_P(PatternEdgeTest, AMemoryConditionRunsOncePerRise)
{
    const auto& [token, mask] = GetParam();
    auto seen = std::make_shared<Log<uint64_t>>();

    auto routine = Kriya::pattern(
        [](uint64_t index) { return index; },
        [seen](uint64_t index) { seen->add(index); },
        rise_condition(mask),
        token);

    ASSERT_NE(routine, nullptr);
    advance(routine, token, mask.size());

    EXPECT_EQ(seen->items(), first_n(rises(mask)));
}

INSTANTIATE_TEST_SUITE_P(
    Masks,
    PatternEdgeTest,
    ::testing::Combine(
        sound_and_graphics,
        ::testing::Values(
            std::string("1111111111"),
            std::string("0011100111"),
            std::string("1010101010"),
            std::string("0000000000"),
            std::string("0110110110"))),
    mask_name);

class PatternLogicTest : public PatternParamTest<std::string> { };

TEST_P(PatternLogicTest, ALogicNodeOutputDrivesTheSteps)
{
    const std::string& levels = GetParam();
    auto gate = std::make_shared<Nodes::Generator::Logic>(0.5);
    auto seen = std::make_shared<Log<uint64_t>>();

    auto routine = Kriya::pattern(
        [](uint64_t index) { return index; },
        [seen](uint64_t index) { seen->add(index); },
        [gate](uint64_t) { return gate->get_last_output() > 0.5; });

    scheduler->add_task(routine);

    for (const char symbol : levels) {
        gate->process_sample(symbol == '1' ? 0.9 : 0.1);
        run_samples(1);
    }

    EXPECT_EQ(seen->items(), first_n(ones(levels)));
}

INSTANTIATE_TEST_SUITE_P(
    Levels,
    PatternLogicTest,
    ::testing::Values(
        std::string("1111"),
        std::string("0011"),
        std::string("0110110"),
        std::string("000000"),
        std::string("1000001")),
    [](const ::testing::TestParamInfo<std::string>& info) { return "L" + info.param; });

TEST_F(PatternTest, ConditionFollowsExternalState)
{
    auto open = std::make_shared<bool>(false);
    auto seen = std::make_shared<Log<uint64_t>>();

    auto routine = Kriya::pattern(
        [](uint64_t index) { return index; },
        [seen](uint64_t index) { seen->add(index); },
        [open](uint64_t) { return *open; });

    scheduler->add_task(routine);

    run_samples(10);
    EXPECT_TRUE(seen->items().empty());

    *open = true;
    run_samples(5);
    EXPECT_EQ(seen->items(), first_n(5));

    *open = false;
    run_samples(5);
    EXPECT_EQ(seen->items(), first_n(5));

    *open = true;
    run_samples(3);
    EXPECT_EQ(seen->items(), first_n(8));
}

TEST_F(PatternTest, PatternsSharingACondition)
{
    auto open = std::make_shared<bool>(true);
    auto first = std::make_shared<Log<uint64_t>>();
    auto second = std::make_shared<Log<uint64_t>>();

    auto a = Kriya::pattern(
        [](uint64_t index) { return index; },
        [first](uint64_t index) { first->add(index); },
        [open](uint64_t) { return *open; });
    auto b = Kriya::pattern(
        [](uint64_t index) { return index * 2; },
        [second](uint64_t value) { second->add(value); },
        [open](uint64_t) { return *open; });

    scheduler->add_task(a);
    scheduler->add_task(b);

    run_samples(4);

    EXPECT_EQ(first->items(), first_n(4));
    EXPECT_EQ(second->items(), (std::vector<uint64_t> { 0, 2, 4, 6 }));
}

TEST_F(PatternTest, CancelStopsAConditionPattern)
{
    auto seen = std::make_shared<Log<uint64_t>>();

    auto routine = Kriya::pattern(
        [](uint64_t index) { return index; },
        [seen](uint64_t index) { seen->add(index); },
        [](uint64_t) { return true; });

    scheduler->add_task(routine);

    run_samples(5);
    EXPECT_EQ(seen->items().size(), 5U);

    EXPECT_TRUE(scheduler->cancel_task(routine));

    run_samples(5);
    EXPECT_EQ(seen->items().size(), 5U);
}

//-----------------------------------------------------------------------------
// pattern: value and index types
//-----------------------------------------------------------------------------

TEST_F(PatternTest, PatternCarriesAPairValue)
{
    auto seen = std::make_shared<Log<std::pair<int, double>>>();

    auto routine = Kriya::pattern(
        [](uint64_t index) { return std::make_pair(static_cast<int>(index), index * 0.5); },
        [seen](std::pair<int, double> value) { seen->add(value); },
        [](uint64_t) { return true; });

    scheduler->add_task(routine);
    run_samples(3);

    const auto items = seen->items();
    ASSERT_EQ(items.size(), 3U);
    EXPECT_EQ(items[2].first, 2);
    EXPECT_DOUBLE_EQ(items[2].second, 1.0);
}

TEST_F(PatternTest, PatternCarriesAString)
{
    auto seen = std::make_shared<Log<std::string>>();

    auto routine = Kriya::pattern(
        [](uint64_t index) { return "step" + std::to_string(index); },
        [seen](const std::string& value) { seen->add(value); },
        [](uint64_t) { return true; });

    scheduler->add_task(routine);
    run_samples(3);

    EXPECT_EQ(seen->items(), (std::vector<std::string> { "step0", "step1", "step2" }));
}

TEST_F(PatternTest, PatternCarriesAMoveOnlyValue)
{
    auto seen = std::make_shared<Log<int>>();

    auto routine = Kriya::pattern(
        [](uint64_t index) { return std::make_unique<int>(static_cast<int>(index) * 3); },
        [seen](std::unique_ptr<int> value) { seen->add(*value); },
        [](uint64_t) { return true; });

    scheduler->add_task(routine);
    run_samples(3);

    EXPECT_EQ(seen->items(), (std::vector<int> { 0, 3, 6 }));
}

TEST_F(PatternTest, PatternAcceptsAnotherIndexType)
{
    auto seen = std::make_shared<Log<int32_t>>();

    auto routine = Kriya::pattern<int32_t>(
        [](int32_t index) { return index; },
        [seen](int32_t value) { seen->add(value); },
        [](int32_t) { return true; });

    scheduler->add_task(routine);
    run_samples(4);

    EXPECT_EQ(seen->items(), (std::vector<int32_t> { 0, 1, 2, 3 }));
}

TEST_F(PatternTest, ErasedFormMatchesTheTemplate)
{
    auto seen = std::make_shared<Log<int>>();

    std::function<std::any(uint64_t)> generator = [](uint64_t index) -> std::any { return static_cast<int>(index * 10); };
    std::function<void(std::any)> callback = [seen](std::any value) { seen->add(std::any_cast<int>(value)); };
    std::function<bool(uint64_t)> ready = [](uint64_t) { return true; };

    auto routine = Kriya::pattern(generator, callback, ready);

    ASSERT_NE(routine, nullptr);
    scheduler->add_task(routine);
    run_samples(3);

    EXPECT_EQ(seen->items(), (std::vector<int> { 0, 10, 20 }));
}

//-----------------------------------------------------------------------------
// pattern: routine types
//-----------------------------------------------------------------------------

class PatternTokenTest : public PatternParamTest<SchedToken> { };

TEST_P(PatternTokenTest, BuildsTheRoutineForTheToken)
{
    const SchedToken token = GetParam();

    auto routine = Kriya::pattern(
        [](uint64_t index) { return index; },
        [](uint64_t) { },
        [](uint64_t) { return false; },
        token);

    ASSERT_NE(routine, nullptr);
    EXPECT_EQ(routine->get_processing_token(), token);
}

INSTANTIATE_TEST_SUITE_P(
    Supported,
    PatternTokenTest,
    ::testing::Values(
        SchedToken::SAMPLE_ACCURATE,
        SchedToken::FRAME_ACCURATE,
        SchedToken::MULTI_RATE,
        SchedToken::CONDITIONAL),
    [](const ::testing::TestParamInfo<SchedToken>& info) { return std::string(token_name(info.param)); });

class PatternRejectedTokenTest : public PatternParamTest<SchedToken> { };

TEST_P(PatternRejectedTokenTest, ReturnsNullForUnsupportedTokens)
{
    auto routine = Kriya::pattern(
        [](uint64_t index) { return index; },
        [](uint64_t) { },
        [](uint64_t) { return true; },
        GetParam());

    EXPECT_EQ(routine, nullptr);
}

TEST_P(PatternRejectedTokenTest, ReturnsNullForUnsupportedTokensWithDurations)
{
    auto routine = Kriya::pattern(
        [](uint64_t index) { return index; },
        [](uint64_t) { },
        std::vector<double> { 0.25 },
        GetParam());

    EXPECT_EQ(routine, nullptr);
}

INSTANTIATE_TEST_SUITE_P(
    Unsupported,
    PatternRejectedTokenTest,
    ::testing::Values(SchedToken::EVENT_DRIVEN, SchedToken::ON_DEMAND),
    [](const ::testing::TestParamInfo<SchedToken>& info) { return std::string(token_name(info.param)); });

TEST_F(PatternTest, CrossPatternRunsConsecutiveSteps)
{
    auto seen = std::make_shared<Log<uint64_t>>();

    auto routine = Kriya::pattern(
        [](uint64_t index) { return index; },
        [seen](uint64_t index) { seen->add(index); },
        [](uint64_t) { return true; },
        SchedToken::MULTI_RATE);

    ASSERT_NE(routine, nullptr);
    scheduler->add_task(routine);
    run_samples(50);

    const auto items = seen->items();
    ASSERT_FALSE(items.empty());
    EXPECT_EQ(items, first_n(items.size()));

    EXPECT_TRUE(scheduler->cancel_task(routine));
}

TEST_F(PatternTest, FreePatternRunsOnTheConditionThread)
{
    auto seen = std::make_shared<Log<uint64_t>>();

    auto routine = Kriya::pattern(
        [](uint64_t index) { return index; },
        [seen](uint64_t index) { seen->add(index); },
        [](uint64_t index) { return index < 5; },
        SchedToken::CONDITIONAL);

    ASSERT_NE(routine, nullptr);
    EXPECT_TRUE(seen->items().empty());

    scheduler->add_task(routine);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (seen->items().size() < 5 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_EQ(seen->items(), first_n(5));

    EXPECT_TRUE(scheduler->cancel_task(routine));
}

//-----------------------------------------------------------------------------
// pattern: failures
//-----------------------------------------------------------------------------

enum class Stage : uint8_t {
    Generator,
    Callback,
    Condition
};

struct FailureCase {
    const char* name;
    Stage stage;
    std::vector<uint64_t> expected;
};

class PatternFailureTest : public PatternParamTest<FailureCase> { };

TEST_P(PatternFailureTest, AThrowEndsThePatternWithoutTerminating)
{
    const auto& param = GetParam();
    auto seen = std::make_shared<Log<uint64_t>>();

    auto routine = Kriya::pattern(
        [stage = param.stage](uint64_t index) {
            if (stage == Stage::Generator && index == 2) {
                throw std::runtime_error("generator");
            }
            return index;
        },
        [seen, stage = param.stage](uint64_t index) {
            seen->add(index);
            if (stage == Stage::Callback && index == 2) {
                throw std::runtime_error("callback");
            }
        },
        [stage = param.stage](uint64_t index) {
            if (stage == Stage::Condition && index == 2) {
                throw std::runtime_error("condition");
            }
            return true;
        });

    ASSERT_NE(routine, nullptr);
    scheduler->add_task(routine);
    run_samples(10);

    EXPECT_EQ(seen->items(), param.expected);
    EXPECT_FALSE(routine->is_active());
}

INSTANTIATE_TEST_SUITE_P(
    Stages,
    PatternFailureTest,
    ::testing::Values(
        FailureCase { "Generator", Stage::Generator, { 0, 1 } },
        FailureCase { "Callback", Stage::Callback, { 0, 1, 2 } },
        FailureCase { "Condition", Stage::Condition, { 0, 1 } }),
    [](const ::testing::TestParamInfo<FailureCase>& info) { return std::string(info.param.name); });

class PatternEmptyArgumentTest : public PatternParamTest<int> { };

TEST_P(PatternEmptyArgumentTest, ReturnsNullForAnEmptyFunction)
{
    const int empty = GetParam();

    std::function<std::any(uint64_t)> generator;
    std::function<void(std::any)> callback;
    std::function<bool(uint64_t)> ready;

    if (empty != 0) {
        generator = [](uint64_t index) -> std::any { return index; };
    }
    if (empty != 1) {
        callback = [](std::any) { };
    }
    if (empty != 2) {
        ready = [](uint64_t) { return true; };
    }

    EXPECT_EQ(Kriya::pattern(generator, callback, ready), nullptr);
}

std::string argument_name(int index)
{
    static const char* const names[] = { "Generator", "Callback", "Condition" };
    return names[index];
}

TEST_P(PatternEmptyArgumentTest, ReturnsNullForAnEmptyFunctionWithDurations)
{
    const int empty = GetParam();

    std::function<std::any(uint64_t)> generator;
    std::function<void(std::any)> callback;

    if (empty != 0) {
        generator = [](uint64_t index) -> std::any { return index; };
    }
    if (empty != 1) {
        callback = [](std::any) { };
    }

    std::vector<double> durations;
    if (empty != 2) {
        durations = { 0.25 };
    }

    EXPECT_EQ(Kriya::pattern(generator, callback, std::move(durations)), nullptr);
}

INSTANTIATE_TEST_SUITE_P(
    Arguments,
    PatternEmptyArgumentTest,
    ::testing::Values(0, 1, 2),
    [](const ::testing::TestParamInfo<int>& info) { return argument_name(info.param); });

//-----------------------------------------------------------------------------
// pattern: timed form failures and routine types
//-----------------------------------------------------------------------------

using TimedFailureParam = std::tuple<SchedToken, FailureCase>;

std::string timed_failure_name(const ::testing::TestParamInfo<TimedFailureParam>& info)
{
    return std::string(token_name(std::get<0>(info.param))) + "_" + std::get<1>(info.param).name;
}

class PatternTimedFailureTest : public PatternParamTest<TimedFailureParam> { };

TEST_P(PatternTimedFailureTest, AThrowEndsTheTimedPatternWithoutTerminating)
{
    const auto& [token, param] = GetParam();
    auto seen = std::make_shared<Log<uint64_t>>();

    auto routine = Kriya::pattern(
        [stage = param.stage](uint64_t index) {
            if (stage == Stage::Generator && index == 2) {
                throw std::runtime_error("generator");
            }
            return index;
        },
        [seen, stage = param.stage](uint64_t index) {
            seen->add(index);
            if (stage == Stage::Callback && index == 2) {
                throw std::runtime_error("callback");
            }
        },
        std::vector<double> { 0.25 },
        token);

    ASSERT_NE(routine, nullptr);

    if (token == SchedToken::FRAME_ACCURATE) {
        advance(routine, token, 120);
    } else {
        scheduler->add_task(routine);
        run_samples(60000);
    }

    EXPECT_EQ(seen->items(), param.expected);
    EXPECT_FALSE(routine->is_active());
}

INSTANTIATE_TEST_SUITE_P(
    Stages,
    PatternTimedFailureTest,
    ::testing::Combine(
        ::testing::Values(SchedToken::SAMPLE_ACCURATE, SchedToken::FRAME_ACCURATE, SchedToken::MULTI_RATE),
        ::testing::Values(
            FailureCase { "Generator", Stage::Generator, { 0, 1 } },
            FailureCase { "Callback", Stage::Callback, { 0, 1, 2 } })),
    timed_failure_name);

class PatternTimedTokenTest : public PatternParamTest<SchedToken> { };

TEST_P(PatternTimedTokenTest, BuildsTheRoutineForTheToken)
{
    const SchedToken token = GetParam();

    auto routine = Kriya::pattern(
        [](uint64_t index) { return index; },
        [](uint64_t) { },
        std::vector<double> { 0.25 },
        token);

    ASSERT_NE(routine, nullptr);
    EXPECT_EQ(routine->get_processing_token(), token);
}

INSTANTIATE_TEST_SUITE_P(
    Supported,
    PatternTimedTokenTest,
    ::testing::Values(
        SchedToken::SAMPLE_ACCURATE,
        SchedToken::FRAME_ACCURATE,
        SchedToken::MULTI_RATE,
        SchedToken::CONDITIONAL),
    [](const ::testing::TestParamInfo<SchedToken>& info) { return std::string(token_name(info.param)); });

TEST_F(PatternTest, CrossTimedPatternFollowsTheList)
{
    auto seen = std::make_shared<Log<uint64_t>>();

    auto routine = Kriya::pattern(
        [](uint64_t index) { return index; },
        [seen](uint64_t index) { seen->add(index); },
        std::vector<double> { 0.25, 0.5 },
        SchedToken::MULTI_RATE);

    ASSERT_NE(routine, nullptr);
    scheduler->add_task(routine);
    run_samples(40000);

    EXPECT_EQ(seen->items(), first_n(3));

    EXPECT_TRUE(scheduler->cancel_task(routine));
}

TEST_F(PatternTest, FreeTimedPatternRunsOnTheConditionThread)
{
    auto seen = std::make_shared<Log<uint64_t>>();

    auto routine = Kriya::pattern(
        [](uint64_t index) { return index; },
        [seen](uint64_t index) { seen->add(index); },
        std::vector<double> { 0.01 },
        SchedToken::CONDITIONAL);

    ASSERT_NE(routine, nullptr);
    scheduler->add_task(routine);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (seen->items().size() < 3 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const auto items = seen->items();
    ASSERT_GE(items.size(), 3U);
    EXPECT_EQ(items, first_n(items.size()));

    EXPECT_TRUE(scheduler->cancel_task(routine));
}

class PatternFreeTimedFailureTest : public PatternParamTest<FailureCase> { };

TEST_P(PatternFreeTimedFailureTest, AThrowEndsTheTimedPatternWithoutTerminating)
{
    const auto& param = GetParam();
    auto seen = std::make_shared<Log<uint64_t>>();

    auto routine = Kriya::pattern(
        [stage = param.stage](uint64_t index) {
            if (stage == Stage::Generator && index == 2) {
                throw std::runtime_error("generator");
            }
            return index;
        },
        [seen, stage = param.stage](uint64_t index) {
            seen->add(index);
            if (stage == Stage::Callback && index == 2) {
                throw std::runtime_error("callback");
            }
        },
        std::vector<double> { 0.005 },
        SchedToken::CONDITIONAL);

    ASSERT_NE(routine, nullptr);
    scheduler->add_task(routine);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (routine->is_active() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_EQ(seen->items(), param.expected);
    EXPECT_FALSE(routine->is_active());
}

INSTANTIATE_TEST_SUITE_P(
    Stages,
    PatternFreeTimedFailureTest,
    ::testing::Values(
        FailureCase { "Generator", Stage::Generator, { 0, 1 } },
        FailureCase { "Callback", Stage::Callback, { 0, 1, 2 } }),
    [](const ::testing::TestParamInfo<FailureCase>& info) { return std::string(info.param.name); });

class PatternTimedEmptyArgumentTest : public PatternParamTest<int> { };

TEST_P(PatternTimedEmptyArgumentTest, ReturnsNullForAnEmptyFunction)
{
    const int empty = GetParam();

    std::function<std::any(uint64_t)> generator;
    std::function<void(std::any)> callback;
    std::function<std::vector<double>(uint64_t)> source;

    if (empty != 0) {
        generator = [](uint64_t index) -> std::any { return index; };
    }
    if (empty != 1) {
        callback = [](std::any) { };
    }
    if (empty != 2) {
        source = [](uint64_t) { return std::vector<double> { 0.25 }; };
    }

    EXPECT_EQ(Kriya::pattern(generator, callback, source), nullptr);
}

std::string timed_argument_name(int index)
{
    static const char* const names[] = { "Generator", "Callback", "Durations" };
    return names[index];
}

INSTANTIATE_TEST_SUITE_P(
    Arguments,
    PatternTimedEmptyArgumentTest,
    ::testing::Values(0, 1, 2),
    [](const ::testing::TestParamInfo<int>& info) { return timed_argument_name(info.param); });

//-----------------------------------------------------------------------------
// pattern: range values
//-----------------------------------------------------------------------------

TEST_F(PatternTest, RangeValueTypesAreDeduced)
{
    static_assert(Kriya::PatternValues<std::vector<int>, uint64_t>);
    static_assert(Kriya::PatternValues<Kriya::Recurring<std::vector<int>>, uint64_t>);
    static_assert(!Kriya::PatternValues<int, uint64_t>);
    static_assert(std::same_as<Kriya::pattern_value_t<std::vector<int>, uint64_t>, int&>);
    static_assert(std::same_as<Kriya::pattern_value_t<Kriya::Recurring<std::vector<int>>, uint64_t>, int&>);
    SUCCEED();
}

struct RangeCase {
    const char* name;
    std::vector<int> values;
    std::vector<double> durations;
    std::vector<std::pair<double, int>> checkpoints;
};

class PatternRangeTest : public PatternParamTest<RangeCase> { };

TEST_P(PatternRangeTest, PlaysOnceFollowingTheDurationsThenEnds)
{
    const auto& param = GetParam();
    auto seen = std::make_shared<Log<int>>();

    auto routine = Kriya::pattern(
        param.values,
        [seen](int value) { seen->add(value); },
        param.durations);

    ASSERT_NE(routine, nullptr);
    scheduler->add_task(routine);

    for (const auto& [seconds, expected] : param.checkpoints) {
        run_seconds(seconds);
        EXPECT_EQ(static_cast<int>(seen->items().size()), expected);
        EXPECT_TRUE(routine->is_active());
    }

    run_seconds(1.0);
    EXPECT_EQ(seen->items(), param.values);
    EXPECT_FALSE(routine->is_active());
}

INSTANTIATE_TEST_SUITE_P(
    Phrases,
    PatternRangeTest,
    ::testing::Values(
        RangeCase { "LongerThanTheDurations", { 1, 2, 3, 4, 5 }, { 0.25, 0.5 }, { { 0.2, 1 }, { 0.1, 2 }, { 0.5, 3 }, { 0.25, 4 }, { 0.5, 5 } } },
        RangeCase { "ShorterThanTheDurations", { 1, 2 }, { 0.25, 0.5, 0.125 }, { { 0.125, 1 }, { 0.25, 2 } } },
        RangeCase { "SingleValue", { 7 }, { 0.25 }, { { 0.125, 1 } } }),
    [](const ::testing::TestParamInfo<RangeCase>& info) { return std::string(info.param.name); });

TEST_F(PatternTest, ARangeEndsOnlyAfterTheLastGap)
{
    auto seen = std::make_shared<Log<int>>();

    auto routine = Kriya::pattern(
        std::vector<int> { 60, 62, 64, 67 },
        [seen](int value) { seen->add(value); },
        std::vector<double> { 0.25 });

    ASSERT_NE(routine, nullptr);
    scheduler->add_task(routine);

    run_seconds(0.9);
    EXPECT_EQ(seen->items(), (std::vector<int> { 60, 62, 64, 67 }));
    EXPECT_TRUE(routine->is_active());

    run_seconds(0.2);
    EXPECT_EQ(seen->items().size(), 4U);
    EXPECT_FALSE(routine->is_active());
}

TEST_F(PatternTest, ARecurringRangeStartsAgain)
{
    auto seen = std::make_shared<Log<int>>();

    auto routine = Kriya::pattern(
        Kriya::recurring(std::vector<int> { 1, 2, 3 }),
        [seen](int value) { seen->add(value); },
        std::vector<double> { 0.25 });

    ASSERT_NE(routine, nullptr);
    scheduler->add_task(routine);
    run_seconds(0.9);

    EXPECT_EQ(seen->items(), (std::vector<int> { 1, 2, 3, 1 }));
    EXPECT_TRUE(routine->is_active());

    EXPECT_TRUE(scheduler->cancel_task(routine));
}

TEST_F(PatternTest, ARecurringRangeDriftsAgainstTheDurations)
{
    auto seen = std::make_shared<Log<int>>();

    auto routine = Kriya::pattern(
        Kriya::recurring(std::vector<int> { 1, 2, 3, 4, 5 }),
        [seen](int value) { seen->add(value); },
        std::vector<double> { 0.25, 0.25, 0.5 });

    ASSERT_NE(routine, nullptr);
    scheduler->add_task(routine);
    run_seconds(2.1);

    EXPECT_EQ(seen->items(), (std::vector<int> { 1, 2, 3, 4, 5, 1, 2 }));

    EXPECT_TRUE(scheduler->cancel_task(routine));
}

TEST_F(PatternTest, AnEmptyRangeEndsAtOnce)
{
    auto seen = std::make_shared<Log<int>>();

    auto routine = Kriya::pattern(
        std::vector<int> {},
        [seen](int value) { seen->add(value); },
        std::vector<double> { 0.25 });

    ASSERT_NE(routine, nullptr);
    scheduler->add_task(routine);
    run_seconds(0.1);

    EXPECT_TRUE(seen->items().empty());
    EXPECT_FALSE(routine->is_active());
}

TEST_F(PatternTest, AnEmptyRecurringRangeEndsAtOnce)
{
    auto seen = std::make_shared<Log<int>>();

    auto routine = Kriya::pattern(
        Kriya::recurring(std::vector<int> {}),
        [seen](int value) { seen->add(value); },
        std::vector<double> { 0.25 });

    ASSERT_NE(routine, nullptr);
    scheduler->add_task(routine);
    run_seconds(0.1);

    EXPECT_TRUE(seen->items().empty());
    EXPECT_FALSE(routine->is_active());
}

TEST_F(PatternTest, ACallbackThatThrowsEndsARangePattern)
{
    auto seen = std::make_shared<Log<int>>();

    auto routine = Kriya::pattern(
        std::vector<int> { 1, 2, 3 },
        [seen](int value) {
            seen->add(value);
            if (value == 2) {
                throw std::runtime_error("callback");
            }
        },
        std::vector<double> { 0.25 });

    ASSERT_NE(routine, nullptr);
    scheduler->add_task(routine);
    run_seconds(1.5);

    EXPECT_EQ(seen->items(), (std::vector<int> { 1, 2 }));
    EXPECT_FALSE(routine->is_active());
}

TEST_F(PatternTest, ARangeThatThrowsWhileReadingEndsThePattern)
{
    auto seen = std::make_shared<Log<int>>();

    auto phrase = std::views::iota(0, 4) | std::views::transform([](int value) {
        if (value == 2) {
            throw std::runtime_error("range");
        }
        return value;
    });

    auto routine = Kriya::pattern(
        phrase,
        [seen](int value) { seen->add(value); },
        std::vector<double> { 0.25 });

    ASSERT_NE(routine, nullptr);
    scheduler->add_task(routine);
    run_seconds(1.5);

    EXPECT_EQ(seen->items(), (std::vector<int> { 0, 1 }));
    EXPECT_FALSE(routine->is_active());
}

TEST_F(PatternTest, ARangeOfMoveOnlyElements)
{
    auto seen = std::make_shared<Log<int>>();

    std::vector<std::unique_ptr<int>> phrase;
    phrase.push_back(std::make_unique<int>(3));
    phrase.push_back(std::make_unique<int>(6));

    auto routine = Kriya::pattern(
        std::move(phrase),
        [seen](const std::unique_ptr<int>& value) { seen->add(*value); },
        std::vector<double> { 0.25 });

    ASSERT_NE(routine, nullptr);
    scheduler->add_task(routine);
    run_seconds(1.0);

    EXPECT_EQ(seen->items(), (std::vector<int> { 3, 6 }));
    EXPECT_FALSE(routine->is_active());
}

TEST_F(PatternTest, ARangeFollowsADurationFunction)
{
    auto seen = std::make_shared<Log<int>>();

    auto routine = Kriya::pattern(
        std::vector<int> { 1, 2, 3 },
        [seen](int value) { seen->add(value); },
        [](uint64_t) { return std::vector<double> { 0.25 }; });

    ASSERT_NE(routine, nullptr);
    scheduler->add_task(routine);
    run_seconds(1.0);

    EXPECT_EQ(seen->items(), (std::vector<int> { 1, 2, 3 }));
    EXPECT_FALSE(routine->is_active());
}

class PatternRangeTokenTest : public PatternParamTest<SchedToken> { };

TEST_P(PatternRangeTokenTest, ARangePlaysOnceOnEveryToken)
{
    const SchedToken token = GetParam();
    auto seen = std::make_shared<Log<int>>();

    auto routine = Kriya::pattern(
        std::views::iota(0, 4),
        [seen](int value) { seen->add(value); },
        std::vector<double> { 0.25 },
        token);

    ASSERT_NE(routine, nullptr);

    if (token == SchedToken::FRAME_ACCURATE) {
        advance(routine, token, 200);
    } else {
        scheduler->add_task(routine);
        run_samples(60000);
    }

    EXPECT_EQ(seen->items(), (std::vector<int> { 0, 1, 2, 3 }));
    EXPECT_FALSE(routine->is_active());
}

INSTANTIATE_TEST_SUITE_P(
    Tokens,
    PatternRangeTokenTest,
    ::testing::Values(SchedToken::SAMPLE_ACCURATE, SchedToken::FRAME_ACCURATE, SchedToken::MULTI_RATE),
    [](const ::testing::TestParamInfo<SchedToken>& info) { return std::string(token_name(info.param)); });

TEST_F(PatternTest, AFreeRangePatternEndsOnTheConditionThread)
{
    auto seen = std::make_shared<Log<int>>();

    auto routine = Kriya::pattern(
        std::views::iota(0, 3),
        [seen](int value) { seen->add(value); },
        std::vector<double> { 0.005 },
        SchedToken::CONDITIONAL);

    ASSERT_NE(routine, nullptr);
    scheduler->add_task(routine);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (routine->is_active() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_EQ(seen->items(), (std::vector<int> { 0, 1, 2 }));
    EXPECT_FALSE(routine->is_active());
}

} // namespace MayaFlux::Test
