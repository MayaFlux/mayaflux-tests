#include "gtest/gtest.h"

#include "../support/scripted_window.hpp"
#include "MayaFlux/IO/Keys.hpp"
#include "MayaFlux/Kriya/InputEvents.hpp"
#include "MayaFlux/Vruta/Event.hpp"
#include "MayaFlux/Vruta/EventManager.hpp"

namespace MayaFlux::Test {

namespace {

    struct InputRig {
        std::shared_ptr<ScriptedWindow> window = std::make_shared<ScriptedWindow>(400U, 300U);
        Vruta::EventManager manager;

        void start(const std::string& name, Vruta::Event handler)
        {
            manager.add_event(std::make_shared<Vruta::Event>(std::move(handler)), name);
        }
    };

} // namespace

TEST(InputEventsKeyTest, KeyPressedFiresOnlyForItsKeyOnPress)
{
    InputRig rig;
    int calls = 0;
    rig.start("press_a", Kriya::key_pressed(rig.window, IO::Keys::A, [&calls]() { ++calls; }));

    rig.window->key_down(IO::Keys::B);
    EXPECT_EQ(calls, 0);

    rig.window->key_down(IO::Keys::A);
    EXPECT_EQ(calls, 1);

    rig.window->key_up(IO::Keys::A);
    EXPECT_EQ(calls, 1);
}

TEST(InputEventsKeyTest, KeyReleasedFiresOnlyForItsKeyOnRelease)
{
    InputRig rig;
    int calls = 0;
    rig.start("release_a", Kriya::key_released(rig.window, IO::Keys::A, [&calls]() { ++calls; }));

    rig.window->key_down(IO::Keys::A);
    EXPECT_EQ(calls, 0);

    rig.window->key_up(IO::Keys::B);
    EXPECT_EQ(calls, 0);

    rig.window->key_up(IO::Keys::A);
    EXPECT_EQ(calls, 1);
}

TEST(InputEventsKeyTest, KeyHeldFiresOnPressAndIgnoresRelease)
{
    InputRig rig;
    int calls = 0;
    rig.start("held_a", Kriya::key_held(rig.window, IO::Keys::A, [&calls]() { ++calls; }));

    rig.window->key_down(IO::Keys::A);
    EXPECT_EQ(calls, 1);

    rig.window->key_up(IO::Keys::A);
    EXPECT_EQ(calls, 1);
}

TEST(InputEventsKeyTest, TextInputDeliversCodepointsInOrder)
{
    InputRig rig;
    std::vector<uint32_t> seen;
    rig.start("text", Kriya::text_input(rig.window, [&seen](uint32_t cp) { seen.push_back(cp); }));

    rig.window->type(0x61);
    rig.window->key_down(IO::Keys::B);
    rig.window->type(0x62);

    EXPECT_EQ(seen, (std::vector<uint32_t> { 0x61, 0x62 }));
}

TEST(InputEventsKeyTest, AnyKeyReportsEachPressedKey)
{
    InputRig rig;
    std::vector<IO::Keys> seen;
    rig.start("any", Kriya::any_key(rig.window, [&seen](IO::Keys key) { seen.push_back(key); }));

    rig.window->key_down(IO::Keys::A);
    rig.window->key_up(IO::Keys::A);
    rig.window->key_down(IO::Keys::B);

    EXPECT_EQ(seen, (std::vector<IO::Keys> { IO::Keys::A, IO::Keys::B }));
}

TEST(InputEventsMouseTest, MousePressedReportsThePositionAtTheClick)
{
    InputRig rig;
    std::vector<std::pair<double, double>> seen;
    rig.start("click_left", Kriya::mouse_pressed(rig.window, IO::MouseButtons::Left,
                                [&seen](double x, double y) { seen.emplace_back(x, y); }));

    rig.window->move_to_pixel(10.0, 20.0);
    rig.window->press(IO::MouseButtons::Right);
    EXPECT_TRUE(seen.empty());

    rig.window->press(IO::MouseButtons::Left);
    EXPECT_EQ(seen, (std::vector<std::pair<double, double>> { { 10.0, 20.0 } }));
}

TEST(InputEventsMouseTest, MouseReleasedFiresOnReleaseOfItsButton)
{
    InputRig rig;
    int calls = 0;
    rig.start("release_left", Kriya::mouse_released(rig.window, IO::MouseButtons::Left,
                                  [&calls](double, double) { ++calls; }));

    rig.window->press(IO::MouseButtons::Left);
    EXPECT_EQ(calls, 0);

    rig.window->release(IO::MouseButtons::Left);
    EXPECT_EQ(calls, 1);
}

TEST(InputEventsMouseTest, MouseMovedReportsEachMotion)
{
    InputRig rig;
    std::vector<std::pair<double, double>> seen;
    rig.start("move", Kriya::mouse_moved(rig.window,
                          [&seen](double x, double y) { seen.emplace_back(x, y); }));

    rig.window->move_to_pixel(1.0, 2.0);
    rig.window->move_to_pixel(3.0, 4.0);

    EXPECT_EQ(seen, (std::vector<std::pair<double, double>> { { 1.0, 2.0 }, { 3.0, 4.0 } }));
}

TEST(InputEventsMouseTest, MouseDraggedFiresOnlyWhileTheButtonIsHeld)
{
    InputRig rig;
    std::vector<std::pair<double, double>> seen;
    rig.start("drag", Kriya::mouse_dragged(rig.window, IO::MouseButtons::Left,
                          [&seen](double x, double y) { seen.emplace_back(x, y); }));

    rig.window->move_to_pixel(5.0, 5.0);
    EXPECT_TRUE(seen.empty());

    rig.window->press(IO::MouseButtons::Left);
    rig.window->move_to_pixel(6.0, 7.0);
    EXPECT_EQ(seen, (std::vector<std::pair<double, double>> { { 6.0, 7.0 } }));

    rig.window->release(IO::MouseButtons::Left);
    rig.window->move_to_pixel(8.0, 9.0);
    EXPECT_EQ(seen.size(), 1U);
}

TEST(InputEventsMouseTest, MouseScrolledReportsBothOffsets)
{
    InputRig rig;
    std::vector<std::pair<double, double>> seen;
    rig.start("scroll", Kriya::mouse_scrolled(rig.window,
                            [&seen](double x, double y) { seen.emplace_back(x, y); }));

    rig.window->scroll(0.5, -1.0);

    EXPECT_EQ(seen, (std::vector<std::pair<double, double>> { { 0.5, -1.0 } }));
}

TEST(InputEventsWindowTest, WindowResizedReportsTheNewSize)
{
    InputRig rig;
    std::vector<std::pair<uint32_t, uint32_t>> seen;
    rig.start("resize", Kriya::window_resized(rig.window,
                            [&seen](uint32_t w, uint32_t h) { seen.emplace_back(w, h); }));

    rig.window->move_to_pixel(1.0, 1.0);
    EXPECT_TRUE(seen.empty());

    rig.window->resize(640U, 480U);
    EXPECT_EQ(seen, (std::vector<std::pair<uint32_t, uint32_t>> { { 640U, 480U } }));
}

TEST(InputEventsWindowTest, WindowClosedFiresOnEachClose)
{
    InputRig rig;
    int calls = 0;
    rig.start("close", Kriya::window_closed(rig.window, [&calls]() { ++calls; }));

    rig.window->key_down(IO::Keys::A);
    EXPECT_EQ(calls, 0);

    rig.window->close();
    EXPECT_EQ(calls, 1);

    rig.window->close();
    EXPECT_EQ(calls, 2);
}

} // namespace MayaFlux::Test
