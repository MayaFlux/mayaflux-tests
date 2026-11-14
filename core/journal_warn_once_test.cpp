#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "../support/log_capture.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    size_t count_of(std::string_view marker)
    {
        const auto messages = LogCapture::messages();
        return static_cast<size_t>(std::ranges::count_if(messages, [marker](const std::string& message) {
            return message.find(marker) != std::string::npos;
        }));
    }

    TEST(JournalWarnOnceTest, ACallSiteLogsOnceHoweverOftenItRuns)
    {
        LogCapture::install();
        LogCapture::clear();

        for (int i = 0; i < 5; ++i) {
            MF_WARN_ONCE(Journal::Component::Core, Journal::Context::Runtime, "{}", "warn-once-marker-a");
        }

        EXPECT_EQ(count_of("warn-once-marker-a"), 1U);
    }

    TEST(JournalWarnOnceTest, EachCallSiteHasItsOwnOnce)
    {
        LogCapture::install();
        LogCapture::clear();

        for (int i = 0; i < 3; ++i) {
            MF_WARN_ONCE(Journal::Component::Core, Journal::Context::Runtime, "{}", "warn-once-marker-b");
            MF_WARN_ONCE(Journal::Component::Core, Journal::Context::Runtime, "{}", "warn-once-marker-c");
        }

        EXPECT_EQ(count_of("warn-once-marker-b"), 1U);
        EXPECT_EQ(count_of("warn-once-marker-c"), 1U);
    }

} // namespace
} // namespace MayaFlux::Test
