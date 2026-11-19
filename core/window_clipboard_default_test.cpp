#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "../support/scripted_window.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

TEST(WindowClipboardDefaultTest, ABackendWithoutAClipboardDeliversNothingAtOnce)
{
    ScriptedWindow window(200U, 100U);
    int calls = 0;
    std::optional<std::string> got = "unset";

    window.Core::Window::request_clipboard_text([&](std::optional<std::string> text) {
        got = std::move(text);
        ++calls;
    });

    EXPECT_EQ(calls, 1);
    EXPECT_FALSE(got.has_value());
}

TEST(WindowClipboardDefaultTest, CopyingAndAnEmptyCallbackAreIgnored)
{
    ScriptedWindow window(200U, 100U);

    EXPECT_NO_THROW(window.Core::Window::set_clipboard_text("text"));
    EXPECT_NO_THROW(window.Core::Window::request_clipboard_text({}));
}

}
