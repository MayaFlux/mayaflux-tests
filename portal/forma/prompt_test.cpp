#include "gtest/gtest.h"
#include "../../support/test_metadata.hpp"

#include "../../support/scripted_window.hpp"
#include "../../support/text_test_support.hpp"

#include "MayaFlux/Portal/Forma/Context.hpp"
#include "MayaFlux/Portal/Forma/Primitives/Prompt.hpp"
#include "MayaFlux/Portal/Forma/Primitives/TextField.hpp"
#include "MayaFlux/Portal/Forma/Surface.hpp"
#include "MayaFlux/Vruta/EventManager.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::system_font>() }))

namespace MayaFlux::Test {

namespace Forma = Portal::Forma;
using IO::Keys;
using Portal::Graphics::PrimitiveTopology;

namespace {

    constexpr uint32_t k_pixel_size = 24;
    constexpr uint32_t k_row_px = 48;

    std::shared_ptr<Buffers::FormaBuffer> make_buffer()
    {
        return std::make_shared<Buffers::FormaBuffer>(4096, PrimitiveTopology::TRIANGLE_LIST);
    }

    bool digits_only(std::string_view text)
    {
        return std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; });
    }

}

class FormaPromptTest : public SystemFontTest {
protected:
    void SetUp() override
    {
        SystemFontTest::SetUp();
        if (HasFatalFailure())
            return;

        auto& foundry = Portal::Text::TypeFaceFoundry::instance();
        ASSERT_TRUE(foundry.set_default_font(font_path, k_pixel_size, 512));
    }

    std::shared_ptr<Vruta::EventManager> manager = std::make_shared<Vruta::EventManager>();
    std::shared_ptr<ScriptedWindow> window = std::make_shared<ScriptedWindow>(200U, 100U);
    std::shared_ptr<Forma::Layer> layer = std::make_shared<Forma::Layer>();
    std::shared_ptr<Forma::Context> ctx = std::make_shared<Forma::Context>(layer, window, manager, "prompt");
    Forma::Surface surface { Forma::SurfaceConfig { .window = window, .layer = layer, .ctx = ctx, .detach_on_close = false } };

    std::vector<std::optional<std::string>> results;

    Forma::AskCallback record()
    {
        return [this](std::optional<std::string> result) { results.push_back(std::move(result)); };
    }

    std::function<void()> open(std::string initial, Forma::AskValidator validator = {})
    {
        return Forma::internal::ask_in(
            surface, make_buffer(), k_row_px, std::move(initial), std::move(validator), record());
    }

    void tap(Keys key)
    {
        window->key_down(key);
        window->key_up(key);
    }

    void type_text(std::string_view text)
    {
        for (const char c : text)
            window->type(static_cast<uint32_t>(c));
    }
};

TEST_F(FormaPromptTest, EnterAcceptsTheInitialTextAsItStands)
{
    open("localhost");

    tap(Keys::Enter);

    ASSERT_EQ(results.size(), 1U);
    EXPECT_EQ(results.front(), std::optional<std::string> { "localhost" });
}

TEST_F(FormaPromptTest, TypingGoesToTheFieldWithoutAnyClick)
{
    open("80");

    type_text("80");
    tap(Keys::Enter);

    ASSERT_EQ(results.size(), 1U);
    EXPECT_EQ(results.front(), std::optional<std::string> { "8080" });
}

TEST_F(FormaPromptTest, AcceptingRequestsTheWindowToClose)
{
    open("x");
    EXPECT_FALSE(window->should_close());

    tap(Keys::Enter);

    EXPECT_TRUE(window->should_close());
}

TEST_F(FormaPromptTest, EscapeCancelsWithNothingAndRequestsTheWindowToClose)
{
    open("keep");

    tap(Keys::Escape);

    ASSERT_EQ(results.size(), 1U);
    EXPECT_FALSE(results.front().has_value());
    EXPECT_TRUE(window->should_close());
}

TEST_F(FormaPromptTest, EnterNeverInsertsALineBreak)
{
    open("");

    tap(Keys::Enter);

    ASSERT_EQ(results.size(), 1U);
    EXPECT_EQ(results.front(), std::optional<std::string> { "" });
}

TEST_F(FormaPromptTest, TheCallbackFiresExactlyOnce)
{
    open("a");

    tap(Keys::Enter);
    tap(Keys::Enter);
    tap(Keys::Escape);

    EXPECT_EQ(results.size(), 1U);
}

TEST_F(FormaPromptTest, ClosingTheWindowCancelsWithoutAskingItToCloseAgain)
{
    const auto closed = open("x");

    closed();

    ASSERT_EQ(results.size(), 1U);
    EXPECT_FALSE(results.front().has_value());
    EXPECT_FALSE(window->should_close());

    tap(Keys::Enter);
    EXPECT_EQ(results.size(), 1U);
}

TEST_F(FormaPromptTest, EverythingIsAcceptedWithoutAValidator)
{
    open("");

    type_text("not a number");
    tap(Keys::Enter);

    ASSERT_EQ(results.size(), 1U);
    EXPECT_EQ(results.front(), std::optional<std::string> { "not a number" });
}

TEST_F(FormaPromptTest, EnterDoesNothingWhileTheValidatorRefusesTheText)
{
    open("80", digits_only);

    window->type('a');
    tap(Keys::Enter);

    EXPECT_TRUE(results.empty());
    EXPECT_FALSE(window->should_close());

    tap(Keys::Backspace);
    tap(Keys::Enter);

    ASSERT_EQ(results.size(), 1U);
    EXPECT_EQ(results.front(), std::optional<std::string> { "80" });
}

TEST_F(FormaPromptTest, TheValidatorSeesTheStartingTextAndEveryEdit)
{
    std::vector<std::string> seen;
    open("", [&](std::string_view text) {
        seen.emplace_back(text);
        return true;
    });

    window->type('a');
    window->type('b');
    tap(Keys::Backspace);

    EXPECT_EQ(seen, (std::vector<std::string> { "", "a", "ab", "a" }));
}

TEST_F(FormaPromptTest, EscapeStillCancelsWhileTheTextIsRefused)
{
    open("x1", digits_only);

    tap(Keys::Escape);

    ASSERT_EQ(results.size(), 1U);
    EXPECT_FALSE(results.front().has_value());
}

TEST_F(FormaPromptTest, TheDeliveredTextIsAlwaysTextTheValidatorAccepted)
{
    std::vector<std::string> accepted_texts;
    open("1", [&](std::string_view text) {
        const bool ok = digits_only(text) && !text.empty();
        if (ok)
            accepted_texts.emplace_back(text);
        return ok;
    });

    type_text("x2");
    tap(Keys::Enter);
    EXPECT_TRUE(results.empty());

    tap(Keys::Backspace);
    tap(Keys::Backspace);
    tap(Keys::Enter);

    ASSERT_EQ(results.size(), 1U);
    ASSERT_TRUE(results.front().has_value());
    EXPECT_NE(std::ranges::find(accepted_texts, *results.front()), accepted_texts.end());
}

}
