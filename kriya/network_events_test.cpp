#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Core/GlobalNetworkConfig.hpp"
#include "MayaFlux/Kriya/NetworkEvents.hpp"
#include "MayaFlux/Vruta/Event.hpp"
#include "MayaFlux/Vruta/EventManager.hpp"
#include "MayaFlux/Vruta/NetworkSource.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

namespace {

    Core::NetworkMessage message_from(const std::string& sender, uint8_t tag)
    {
        Core::NetworkMessage m;
        m.sender_address = sender;
        m.data = { tag };
        return m;
    }

    Core::NetworkMessage message_on(const std::string& sender, uint64_t endpoint, uint8_t tag)
    {
        Core::NetworkMessage m = message_from(sender, tag);
        m.endpoint_id = endpoint;
        return m;
    }

} // namespace

TEST(NetworkListenerTest, OnMessageDeliversEachMessageInOrder)
{
    auto source = std::make_shared<Vruta::NetworkSource>(Core::EndpointInfo {});
    Vruta::EventManager manager;
    std::vector<uint8_t> seen;
    manager.add_event(std::make_shared<Vruta::Event>(Kriya::message_received(
                          source, [&seen](const Core::NetworkMessage& m) { seen.push_back(m.data.front()); })),
        "all");

    source->signal(message_from("a", 1));
    source->signal(message_from("b", 2));

    EXPECT_EQ(seen, (std::vector<uint8_t> { 1, 2 }));
}

TEST(NetworkListenerTest, OnMessageFromDeliversOnlyTheNamedSender)
{
    auto source = std::make_shared<Vruta::NetworkSource>(Core::EndpointInfo {});
    Vruta::EventManager manager;
    std::vector<uint8_t> seen;
    manager.add_event(std::make_shared<Vruta::Event>(Kriya::message_received(
                          source, "alpha", [&seen](const Core::NetworkMessage& m) { seen.push_back(m.data.front()); })),
        "from");

    source->signal(message_from("beta", 1));
    source->signal(message_from("alpha", 2));

    EXPECT_EQ(seen, std::vector<uint8_t> { 2 });
}

TEST(NetworkListenerTest, OnMessageFromConnectionSeparatesConnectionsThatShareAnAddress)
{
    auto source = std::make_shared<Vruta::NetworkSource>(Core::EndpointInfo {});
    Vruta::EventManager manager;
    std::vector<uint8_t> seen;
    manager.add_event(std::make_shared<Vruta::Event>(Kriya::message_received(
                          source, uint64_t { 7 }, [&seen](const Core::NetworkMessage& m) { seen.push_back(m.data.front()); })),
        "connection");

    source->signal(message_on("shared.sock", 5, 1));
    source->signal(message_on("shared.sock", 7, 2));
    source->signal(message_on("shared.sock", 9, 3));
    source->signal(message_on("shared.sock", 7, 4));

    EXPECT_EQ(seen, (std::vector<uint8_t> { 2, 4 }));
}

TEST(NetworkListenerTest, OnMessageMatchingDeliversOnlyAcceptedMessages)
{
    auto source = std::make_shared<Vruta::NetworkSource>(Core::EndpointInfo {});
    Vruta::EventManager manager;
    std::vector<uint8_t> seen;
    manager.add_event(std::make_shared<Vruta::Event>(Kriya::message_received(
                          source,
                          [](const Core::NetworkMessage& m) { return m.data.front() % 2 == 0; },
                          [&seen](const Core::NetworkMessage& m) { seen.push_back(m.data.front()); })),
        "matching");

    source->signal(message_from("a", 1));
    source->signal(message_from("a", 4));

    EXPECT_EQ(seen, std::vector<uint8_t> { 4 });
}

TEST(NetworkListenerTest, CancelledHeldListenerDoesNotRunItsCallbackOnTheNextMessage)
{
    auto source = std::make_shared<Vruta::NetworkSource>(Core::EndpointInfo {});
    Vruta::EventManager manager;
    std::vector<uint8_t> seen;
    auto held = std::make_shared<Vruta::Event>(Kriya::message_received(
        source, [&seen](const Core::NetworkMessage& m) { seen.push_back(m.data.front()); }));
    manager.add_event(held, "held");

    source->signal(message_from("a", 1));
    EXPECT_EQ(seen, std::vector<uint8_t> { 1 });

    EXPECT_TRUE(manager.cancel_event(std::string("held")));
    source->signal(message_from("a", 2));

    EXPECT_EQ(seen, std::vector<uint8_t> { 1 });
    EXPECT_FALSE(held->is_active());
}

} // namespace MayaFlux::Test
