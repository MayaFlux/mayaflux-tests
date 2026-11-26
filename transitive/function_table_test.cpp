#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "../support/described_call_support.hpp"
#include "MayaFlux/Transitive/Reflect/FunctionTable.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using FunctionTableVoices = std::map<std::string, std::shared_ptr<DescribedCallVoice>, std::less<>>;

}

TEST(FunctionTable, MakesWhatACallNamesWithItsArguments)
{
    Reflect::FunctionTable<> calls;
    calls.expose<&described_call_scaled>();
    std::string error;

    const auto defaulted = calls.make<std::function<int()>>({ "described_call_scaled", { { "value", "4" } } }, error);
    const auto given = calls.make<std::function<int()>>(
        { "described_call_scaled", { { "value", "4" }, { "scale", "10" } } }, error);

    ASSERT_TRUE(defaulted && given) << error;
    EXPECT_EQ((*defaulted)(), 8);
    EXPECT_EQ((*given)(), 40);
}

TEST(FunctionTable, ArgumentsReferIntoTheCallersTables)
{
    Reflect::FunctionTable<FunctionTableVoices> calls;
    calls.expose<&described_call_route>("route");
    const FunctionTableVoices voices { { "low", std::make_shared<DescribedCallVoice>(DescribedCallVoice { "bass" }) } };
    std::string error;

    const auto made = calls.make<std::string>(
        { "route", { { "count", "1" }, { "gain", "1" }, { "voice", "low" } } }, error, voices);

    EXPECT_EQ(made, "1 10 bass none") << error;
}

TEST(FunctionTable, RefusesUnknownNamesOtherTypesAndBadArguments)
{
    Reflect::FunctionTable<> calls;
    calls.expose<&described_call_scaled>("scaled");
    std::string error;

    EXPECT_FALSE(calls.make<std::function<int()>>({ "absent", {} }, error));
    EXPECT_NE(error.find("no function named absent"), std::string::npos);

    EXPECT_FALSE(calls.make<std::string>({ "scaled", { { "value", "1" } } }, error));
    EXPECT_NE(error.find("does not make"), std::string::npos);

    EXPECT_FALSE(calls.make<std::function<int()>>({ "scaled", {} }, error));
    EXPECT_NE(error.find("needs value"), std::string::npos);
}

TEST(FunctionTable, MergesUnderAPrefix)
{
    Reflect::FunctionTable<> first;
    Reflect::FunctionTable<> second;
    second.expose<&described_call_scaled>("scaled");
    std::string error;

    ASSERT_TRUE(first.merge(second, "other.", error)) << error;

    EXPECT_EQ(first.names(), std::vector<std::string> { "other.scaled" });
}

}
