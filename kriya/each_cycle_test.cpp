#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "../support/scripted_window.hpp"
#include "MayaFlux/Kriya/InputEvents.hpp"
#include "MayaFlux/Kriya/Tasks.hpp"
#include "MayaFlux/Vruta/EventManager.hpp"
#include "MayaFlux/Vruta/Scheduler.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

TEST(EachCycleHelperTest, SampleTokenCallsOncePerSample)
{
    auto scheduler = std::make_shared<Vruta::TaskScheduler>(48000U);
    int calls = 0;

    auto routine = Kriya::each_cycle([&calls]() { ++calls; }, Vruta::ProcessingToken::SAMPLE_ACCURATE);
    ASSERT_NE(routine, nullptr);
    EXPECT_EQ(routine->get_processing_token(), Vruta::ProcessingToken::SAMPLE_ACCURATE);
    scheduler->add_task(routine);

    scheduler->process_token(Vruta::ProcessingToken::SAMPLE_ACCURATE, 10);
    EXPECT_EQ(calls, 10);

    EXPECT_TRUE(scheduler->cancel_task(routine));
}

TEST(EachCycleHelperTest, FrameTokenIsTheDefault)
{
    auto routine = Kriya::each_cycle([]() { });
    ASSERT_NE(routine, nullptr);
    EXPECT_EQ(routine->get_processing_token(), Vruta::ProcessingToken::FRAME_ACCURATE);
}

TEST(EachCycleHelperTest, EmptyCallbackYieldsNoRoutine)
{
    EXPECT_EQ(Kriya::each_cycle({}), nullptr);
}

TEST(MetroTokensTest, EveryTokenYieldsARoutineOfThatToken)
{
    for (auto token : { Vruta::ProcessingToken::SAMPLE_ACCURATE,
             Vruta::ProcessingToken::FRAME_ACCURATE,
             Vruta::ProcessingToken::MULTI_RATE,
             Vruta::ProcessingToken::CONDITIONAL }) {
        auto routine = Kriya::metro(0.01, []() { }, token);
        ASSERT_NE(routine, nullptr);
        EXPECT_EQ(routine->get_processing_token(), token);
    }
}

TEST(WindowEventHelperTest, CallbackReceivesTheMatchingEvent)
{
    auto window = std::make_shared<ScriptedWindow>(400U, 300U);
    Vruta::EventManager manager;
    int calls = 0;
    Core::WindowEventType seen {};

    manager.add_event(std::make_shared<Vruta::Event>(
                          Kriya::window_event(window,
                              Vruta::WindowEventFilter { Core::WindowEventType::MOUSE_MOTION },
                              [&](const Core::WindowEvent& event) {
                                  ++calls;
                                  seen = event.type;
                              })),
        "motion");

    window->key_down(IO::Keys::A);
    EXPECT_EQ(calls, 0);

    window->move_to_pixel(10.0, 20.0);
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(seen, Core::WindowEventType::MOUSE_MOTION);
}

} // namespace MayaFlux::Test
