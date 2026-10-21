#include "gtest/gtest.h"

#include "MayaFlux/Buffers/Textures/TextBuffer.hpp"

namespace MayaFlux::Test {
namespace {

    using Buffers::TextBuffer;
    using Portal::Graphics::ImageFormat;

    class TextBufferStateTest : public ::testing::Test {
    protected:
        TextBuffer buffer { 128, 64, ImageFormat::RGBA8 };
    };

    TEST_F(TextBufferStateTest, StartsWithNoBudgetAndTheTextureExtentAsItsFallback)
    {
        EXPECT_FALSE(buffer.has_budget());
        EXPECT_EQ(buffer.get_budget_width(), 128U);
        EXPECT_EQ(buffer.get_budget_height(), 64U);
    }

    TEST_F(TextBufferStateTest, SetBudgetOverridesTheTextureExtent)
    {
        buffer.set_budget(300, 200);
        EXPECT_TRUE(buffer.has_budget());
        EXPECT_EQ(buffer.get_budget_width(), 300U);
        EXPECT_EQ(buffer.get_budget_height(), 200U);
    }

    TEST_F(TextBufferStateTest, ZeroWidthBudgetFallsBackToTheTextureExtent)
    {
        buffer.set_budget(300, 200);
        buffer.set_budget(0, 0);
        EXPECT_FALSE(buffer.has_budget());
        EXPECT_EQ(buffer.get_budget_width(), 128U);
        EXPECT_EQ(buffer.get_budget_height(), 64U);
    }

    TEST_F(TextBufferStateTest, CursorIsWritableThroughItsReferencesAndResettable)
    {
        EXPECT_EQ(buffer.get_cursor_x(), 0U);
        EXPECT_EQ(buffer.get_cursor_y(), 0U);

        buffer.get_cursor_x() = 12;
        buffer.get_cursor_y() = 34;
        const TextBuffer& view = buffer;
        EXPECT_EQ(view.get_cursor_x(), 12U);
        EXPECT_EQ(view.get_cursor_y(), 34U);

        buffer.reset_cursor();
        EXPECT_EQ(buffer.get_cursor_x(), 0U);
        EXPECT_EQ(buffer.get_cursor_y(), 0U);
    }

    TEST_F(TextBufferStateTest, RenderBoundsDefaultToHdAndAreSettable)
    {
        EXPECT_EQ(buffer.get_render_bounds_w(), 1280U);
        EXPECT_EQ(buffer.get_render_bounds_h(), 720U);

        buffer.set_render_bounds(640, 480);
        EXPECT_EQ(buffer.get_render_bounds_w(), 640U);
        EXPECT_EQ(buffer.get_render_bounds_h(), 480U);
    }

    TEST_F(TextBufferStateTest, AccumulatedTextAppendsReplacesAndClears)
    {
        EXPECT_TRUE(buffer.get_accumulated_text().empty());

        buffer.append_accumulated_text("ab");
        buffer.append_accumulated_text("cd");
        EXPECT_EQ(buffer.get_accumulated_text(), "abcd");

        buffer.set_accumulated_text("xy");
        EXPECT_EQ(buffer.get_accumulated_text(), "xy");

        buffer.clear_accumulated_text();
        EXPECT_TRUE(buffer.get_accumulated_text().empty());
    }

    TEST_F(TextBufferStateTest, BackgroundDefaultsToTransparentBlackAndIsSettable)
    {
        EXPECT_EQ(buffer.get_background(), glm::vec4(0.F, 0.F, 0.F, 0.F));

        buffer.set_background({ 0.1F, 0.2F, 0.3F, 0.4F });
        EXPECT_EQ(buffer.get_background(), glm::vec4(0.1F, 0.2F, 0.3F, 0.4F));
    }

} // namespace
} // namespace MayaFlux::Test
