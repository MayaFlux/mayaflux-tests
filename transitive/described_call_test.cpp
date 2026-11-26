#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "../support/described_call_support.hpp"
#include "MayaFlux/Transitive/Memory/FlatMap.hpp"
#include "MayaFlux/Transitive/Reflect/DescribedCall.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

struct DescribedCallElement {
    std::vector<std::pair<std::string, std::string>> attributes;
};

}

namespace MayaFlux::Reflect {

template <>
struct RecordAccess<Test::DescribedCallElement> {
    using Source = Test::DescribedCallElement;

    static std::string_view name(const Source&) { return "call"; }
    static uint64_t location(const Source&) { return 3; }

    static std::optional<std::string_view> attribute(const Source& source, std::string_view name)
    {
        for (const auto& [key, value] : source.attributes) {
            if (key == name)
                return std::string_view(value);
        }
        return std::nullopt;
    }

    template <typename Visitor>
    static bool for_each_attribute(const Source& source, Visitor&& visit)
    {
        for (const auto& [key, value] : source.attributes) {
            if (!visit(key, value))
                return false;
        }
        return true;
    }

    template <typename Visitor>
    static bool for_each_child(const Source&, Visitor&&)
    {
        return true;
    }
};

}

namespace MayaFlux::Test {
namespace {

    using Voices = Memory::FlatMap<std::string, std::shared_ptr<DescribedCallVoice>, std::less<>>;
    using Sources = std::map<std::string, std::function<int()>, std::less<>>;

    Voices described_call_voices()
    {
        Voices voices;
        voices.insert_or_assign("low", std::make_shared<DescribedCallVoice>(DescribedCallVoice { "bass" }));
        return voices;
    }

    std::optional<std::string> route(const DescribedCallElement& element, std::string& error)
    {
        static constexpr std::array<std::string_view, 1> ignored { "call" };
        const Voices voices = described_call_voices();
        const Sources sources { { "seven", [] { return 7; } } };
        return Reflect::call_described<&described_call_route>(element, error, ignored, voices, sources);
    }

}

TEST(DescribedCall, TextArgumentsLeaveTheRestToDefaults)
{
    std::string error;
    EXPECT_EQ(route({ { { "call", "route" }, { "count", "4" } } }, error), "4 10 none none");
    EXPECT_TRUE(error.empty());
}

TEST(DescribedCall, NamedArgumentsComeFromTheTableOfTheirType)
{
    std::string error;
    EXPECT_EQ(route({ { { "count", "2" }, { "gain", "0.5" }, { "voice", "low" }, { "source", "seven" } } }, error),
        "2 5 bass 7");
    EXPECT_TRUE(error.empty());
}

TEST(DescribedCall, UnknownNameIsRefused)
{
    std::string error;
    EXPECT_FALSE(route({ { { "count", "2" }, { "gain", "0.5" }, { "voice", "high" } } }, error));
    EXPECT_NE(error.find("unknown voice high"), std::string::npos);
}

TEST(DescribedCall, WithoutATableALiveParameterCannotBeWritten)
{
    std::string error;
    const DescribedCallElement element { { { "count", "2" }, { "gain", "0.5" }, { "voice", "low" } } };
    EXPECT_FALSE(Reflect::call_described<&described_call_route>(element, error));
    EXPECT_NE(error.find("cannot be written in a document"), std::string::npos);
}

TEST(DescribedCall, AttributeThatIsNotIgnoredOrAParameterIsRefused)
{
    std::string error;
    const DescribedCallElement element { { { "call", "route" }, { "count", "2" } } };
    EXPECT_FALSE(Reflect::call_described<&described_call_route>(element, error));
    EXPECT_NE(error.find("unsupported attribute call"), std::string::npos);
}

TEST(DescribedCall, SkippingAParameterOrARequiredOneIsRefused)
{
    std::string error;
    EXPECT_FALSE(route({ { { "count", "2" }, { "voice", "low" } } }, error));
    EXPECT_NE(error.find("voice is given without gain"), std::string::npos);

    error.clear();
    EXPECT_FALSE(route({}, error));
    EXPECT_NE(error.find("needs count"), std::string::npos);
}

}
