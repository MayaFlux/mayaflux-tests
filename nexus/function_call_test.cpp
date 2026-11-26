#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "../support/frame_scheduler.hpp"

#include "MayaFlux/Buffers/BufferManager.hpp"
#include "MayaFlux/Nexus/Fabric.hpp"
#include "MayaFlux/Nexus/State/Orchestrator.hpp"
#include "MayaFlux/Transitive/Reflect/FunctionTable.hpp"
#include "MayaFlux/Vruta/EventManager.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    class NexusSavedCallTest : public ::testing::Test {
    protected:
        const Reflect::FunctionCall with_arguments { "pulse", { { "level", "2" } } };

        FrameScheduler frames;
        std::shared_ptr<Vruta::EventManager> events = std::make_shared<Vruta::EventManager>();
        std::shared_ptr<Buffers::BufferManager> buffers = std::make_shared<Buffers::BufferManager>();
        Nexus::Fabric fabric { frames.scheduler, events, buffers, 1.0F };
    };

    TEST_F(NexusSavedCallTest, ACallSitsBesideTheNameAndANameAloneClearsIt)
    {
        Nexus::Emitter emitter;
        emitter.set_fn_call(with_arguments);
        EXPECT_EQ(emitter.fn_name(), "pulse");
        EXPECT_EQ(emitter.fn_call(), with_arguments);

        emitter.set_fn_name("plain");
        EXPECT_EQ(emitter.fn_call(), (Reflect::FunctionCall { "plain", {} }));
    }

    TEST_F(NexusSavedCallTest, AWiringKeepsTheCallsItWasGiven)
    {
        const auto emitter = std::make_shared<Nexus::Emitter>([](const Nexus::InfluenceContext&) { });
        fabric.wire(emitter)
            .place_from(with_arguments, [] { return Nexus::PlacementControls {}; })
            .on_complete({ "done", {} }, [] { })
            .on_detach(with_arguments, [] { })
            .finalise();

        const auto* wiring = fabric.wiring_for(emitter->id());
        ASSERT_NE(wiring, nullptr);
        EXPECT_EQ(wiring->position_fn_name(), "pulse");
        EXPECT_EQ(wiring->position_call(), with_arguments);
        EXPECT_EQ(wiring->complete_call().first, "done");
        EXPECT_EQ(wiring->detach_call(), with_arguments);
    }

    TEST_F(NexusSavedCallTest, AFunctionMadeWithArgumentsIsNotRegisteredByNameAlone)
    {
        const auto made = std::make_shared<Nexus::Emitter>([](const Nexus::InfluenceContext&) { });
        made->set_fn_call(with_arguments);
        const auto named = std::make_shared<Nexus::Emitter>("plain", [](const Nexus::InfluenceContext&) { });

        fabric.wire(made).finalise();
        fabric.wire(named).finalise();

        EXPECT_EQ(fabric.resolve_influence_fn("pulse"), nullptr);
        EXPECT_NE(fabric.resolve_influence_fn("plain"), nullptr);
    }


    TEST_F(NexusSavedCallTest, FollowBufferIsMadeFromABufferNameInTheCallersTable)
    {
        using Followed = std::map<std::string, std::shared_ptr<Buffers::VKBuffer>, std::less<>>;
        Reflect::FunctionTable<Followed> calls;
        calls.expose<&Nexus::follow_buffer>();
        const Followed followed { { "wolf", nullptr } };
        std::string error;

        EXPECT_TRUE(calls.make<Nexus::Wiring::PlacementFn>(
            { "follow_buffer", { { "buffer", "wolf" }, { "slot", "3" } } }, error, followed))
            << error;

        EXPECT_FALSE(calls.make<Nexus::Wiring::PlacementFn>(
            { "follow_buffer", { { "buffer", "bear" } } }, error, followed));
        EXPECT_NE(error.find("unknown buffer bear"), std::string::npos);
    }

}
}
