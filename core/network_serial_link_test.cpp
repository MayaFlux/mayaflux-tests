#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "../support/input_test_support.hpp"
#include "MayaFlux/Core/Backends/Input/NetworkSerialLink.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using namespace std::chrono_literals;
    using Core::EndpointState;
    using Core::NetworkSerialLink;

    using Bytes = std::vector<uint8_t>;
    using Transition = std::pair<EndpointState, EndpointState>;

    struct LinkRig {
        LinkRig()
            : link(&network.service)
        {
            link.set_chunk_callback([this](std::span<const uint8_t> bytes) {
                chunks.emplace_back(bytes.begin(), bytes.end());
            });
            link.set_state_callback([this](EndpointState previous, EndpointState current) {
                transitions.emplace_back(previous, current);
            });
        }

        FakeNetworkService network;
        NetworkSerialLink link;
        std::vector<Bytes> chunks;
        std::vector<Transition> transitions;
    };

    constexpr uint64_t k_endpoint = 42;

    TEST(NetworkSerialLinkTest, WithoutAServiceTheLinkCannotOpen)
    {
        NetworkSerialLink link(nullptr);

        EXPECT_FALSE(link.open("/dev/ttyTEST", {}));
        EXPECT_EQ(link.state(), EndpointState::CLOSED);
        EXPECT_FALSE(link.write(Bytes { 1 }));
        EXPECT_EQ(link.backlog(), 0U);
        EXPECT_FALSE(link.set_control_lines(true, true));
        EXPECT_FALSE(link.send_break());
        EXPECT_EQ(link.post_after(1ms, [] { }), 0U);
        EXPECT_NO_THROW(link.cancel_post(5));
        EXPECT_NO_THROW(link.close());
    }

    TEST(NetworkSerialLinkTest, OpenAsksForARawSerialEndpointWithTheLineSettings)
    {
        LinkRig rig;
        Core::SerialLineSettings line;
        line.baud_rate = 31250;
        line.data_bits = 7;
        line.parity = Core::SerialParity::EVEN;
        line.stop_bits = Core::SerialStopBits::TWO;
        line.flow_control = Core::SerialFlowControl::SOFTWARE;
        line.dtr_on_open = false;
        line.rts_on_open = false;
        line.settle_ms = 250;

        ASSERT_TRUE(rig.link.open("/dev/ttyTEST", line));

        ASSERT_EQ(rig.network.opened.size(), 1U);
        const auto& info = rig.network.opened[0];
        EXPECT_EQ(info.transport, Core::NetworkTransport::SERIAL);
        EXPECT_EQ(info.role, Core::EndpointRole::BIDIRECTIONAL);
        EXPECT_EQ(info.framing, Core::FramingPolicy::NONE);
        EXPECT_EQ(info.remote_path, "/dev/ttyTEST");
        EXPECT_EQ(info.serial.baud_rate, 31250U);
        EXPECT_EQ(info.serial.data_bits, 7);
        EXPECT_EQ(info.serial.parity, Core::SerialParity::EVEN);
        EXPECT_EQ(info.serial.stop_bits, Core::SerialStopBits::TWO);
        EXPECT_EQ(info.serial.flow_control, Core::SerialFlowControl::SOFTWARE);
        EXPECT_FALSE(info.serial.dtr_on_open);
        EXPECT_FALSE(info.serial.rts_on_open);
        EXPECT_EQ(info.serial.settle_ms, 250U);
    }

    TEST(NetworkSerialLinkTest, AnEndpointThatCannotBeOpenedFailsTheOpen)
    {
        LinkRig rig;
        rig.network.next_endpoint_id = 0;

        EXPECT_FALSE(rig.link.open("/dev/ttyTEST", {}));
        EXPECT_EQ(rig.link.state(), EndpointState::CLOSED);
        EXPECT_TRUE(rig.network.callbacks.empty());
    }

    TEST(NetworkSerialLinkTest, ChunksReachTheCallbackWhole)
    {
        LinkRig rig;
        ASSERT_TRUE(rig.link.open("/dev/ttyTEST", {}));

        rig.network.deliver(k_endpoint, { 1, 2, 3 });
        rig.network.deliver(k_endpoint, {});

        ASSERT_EQ(rig.chunks.size(), 2U);
        EXPECT_EQ(rig.chunks[0], (Bytes { 1, 2, 3 }));
        EXPECT_TRUE(rig.chunks[1].empty());
    }

    TEST(NetworkSerialLinkTest, TheStateAtOpenIsReportedEvenIfTheEndpointMovedBeforeTheCallbackWasSet)
    {
        LinkRig rig;
        rig.network.endpoint_state = EndpointState::OPEN;

        ASSERT_TRUE(rig.link.open("/dev/ttyTEST", {}));

        EXPECT_EQ(rig.transitions, (std::vector<Transition> { { EndpointState::CLOSED, EndpointState::OPEN } }));
    }

    TEST(NetworkSerialLinkTest, AnEndpointWaitingToReconnectIsReportedAsSuch)
    {
        LinkRig rig;
        rig.network.endpoint_state = EndpointState::RECONNECTING;

        ASSERT_TRUE(rig.link.open("/dev/ttyTEST", {}));

        EXPECT_EQ(rig.transitions, (std::vector<Transition> { { EndpointState::CLOSED, EndpointState::RECONNECTING } }));
        EXPECT_EQ(rig.link.state(), EndpointState::RECONNECTING);
    }

    TEST(NetworkSerialLinkTest, TransitionsAreForwardedAndRepeatsAreCoalesced)
    {
        LinkRig rig;
        ASSERT_TRUE(rig.link.open("/dev/ttyTEST", {}));
        rig.transitions.clear();

        rig.network.transition(k_endpoint, EndpointState::OPEN, EndpointState::RECONNECTING);
        rig.network.transition(k_endpoint, EndpointState::OPEN, EndpointState::RECONNECTING);
        rig.network.transition(k_endpoint, EndpointState::RECONNECTING, EndpointState::OPEN);

        EXPECT_EQ(rig.transitions, (std::vector<Transition> {
                                       { EndpointState::OPEN, EndpointState::RECONNECTING },
                                       { EndpointState::RECONNECTING, EndpointState::OPEN } }));
    }

    TEST(NetworkSerialLinkTest, WritesBacklogLineControlBreakAndTimersGoToTheService)
    {
        LinkRig rig;
        ASSERT_TRUE(rig.link.open("/dev/ttyTEST", {}));
        rig.network.backlog = 77;

        EXPECT_TRUE(rig.link.write(Bytes { 9, 8 }));
        EXPECT_EQ(rig.network.sent.back(), (Bytes { 9, 8 }));
        rig.network.send_result = false;
        EXPECT_FALSE(rig.link.write(Bytes { 1 }));

        EXPECT_EQ(rig.link.backlog(), 77U);
        EXPECT_TRUE(rig.link.set_control_lines(true, std::nullopt));
        ASSERT_EQ(rig.network.control_calls.size(), 1U);
        EXPECT_EQ(rig.network.control_calls[0].id, k_endpoint);
        EXPECT_EQ(rig.network.control_calls[0].dtr, std::optional<bool> { true });
        EXPECT_TRUE(rig.link.send_break());
        EXPECT_EQ(rig.network.breaks, (std::vector<uint64_t> { k_endpoint }));

        int fired = 0;
        const auto token = rig.link.post_after(25ms, [&fired] { ++fired; });
        ASSERT_NE(token, 0U);
        ASSERT_EQ(rig.network.timers.count(token), 1U);
        EXPECT_EQ(rig.network.timers.at(token).delay, 25ms);
        ASSERT_TRUE(rig.network.fire(token));
        EXPECT_EQ(fired, 1);

        const auto cancelled = rig.link.post_after(25ms, [&fired] { ++fired; });
        rig.link.cancel_post(cancelled);
        EXPECT_EQ(rig.network.timers.count(cancelled), 0U);
    }

    TEST(NetworkSerialLinkTest, CloseClosesTheEndpointAndTheLinkStopsWriting)
    {
        LinkRig rig;
        ASSERT_TRUE(rig.link.open("/dev/ttyTEST", {}));

        rig.link.close();

        EXPECT_EQ(rig.network.closed, (std::vector<uint64_t> { k_endpoint }));
        EXPECT_EQ(rig.link.state(), EndpointState::CLOSED);
        EXPECT_FALSE(rig.link.write(Bytes { 1 }));
        EXPECT_NO_THROW(rig.link.close());
        EXPECT_EQ(rig.network.closed.size(), 1U);
    }

    TEST(NetworkSerialLinkTest, OpeningAgainClosesTheEndpointBefore)
    {
        LinkRig rig;
        ASSERT_TRUE(rig.link.open("/dev/ttyA", {}));

        ASSERT_TRUE(rig.link.open("/dev/ttyB", {}));

        EXPECT_EQ(rig.network.closed.size(), 1U);
        ASSERT_EQ(rig.network.opened.size(), 2U);
        EXPECT_EQ(rig.network.opened[1].remote_path, "/dev/ttyB");
    }

    TEST(NetworkSerialLinkTest, DestroyingTheLinkClosesItsEndpoint)
    {
        FakeNetworkService network;
        {
            NetworkSerialLink link(&network.service);
            ASSERT_TRUE(link.open("/dev/ttyTEST", {}));
        }

        EXPECT_EQ(network.closed, (std::vector<uint64_t> { k_endpoint }));
    }

    TEST(NetworkSerialLinkTest, CallbacksThatArriveAfterTheLinkIsGoneAreIgnored)
    {
        FakeNetworkService network;
        int chunks = 0;
        int transitions = 0;
        {
            NetworkSerialLink link(&network.service);
            link.set_chunk_callback([&](std::span<const uint8_t>) { ++chunks; });
            link.set_state_callback([&](EndpointState, EndpointState) { ++transitions; });
            ASSERT_TRUE(link.open("/dev/ttyTEST", {}));
        }
        const int transitions_at_open = transitions;

        network.deliver(k_endpoint, { 1 });
        network.transition(k_endpoint, EndpointState::OPEN, EndpointState::ERROR);

        EXPECT_EQ(chunks, 0);
        EXPECT_EQ(transitions, transitions_at_open);
    }

    TEST(NetworkSerialLinkTest, AChunkCallbackMayCloseTheLinkAndALaterChunkIsIgnored)
    {
        FakeNetworkService network;
        NetworkSerialLink link(&network.service);
        int chunks = 0;
        link.set_chunk_callback([&](std::span<const uint8_t>) {
            ++chunks;
            link.close();
        });
        ASSERT_TRUE(link.open("/dev/ttyTEST", {}));

        network.deliver(k_endpoint, { 1 });
        network.deliver(k_endpoint, { 2 });

        EXPECT_EQ(chunks, 1);
        EXPECT_EQ(network.closed, (std::vector<uint64_t> { k_endpoint }));
    }

} // namespace
} // namespace MayaFlux::Test
