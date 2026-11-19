#include "gtest/gtest.h"
#include "../../support/test_metadata.hpp"

#include "MayaFlux/Portal/Network/NetworkUtils.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Core::EndpointInfo;
    using Core::FramingPolicy;
    using Core::NetworkTransport;
    using Portal::Network::NetworkTransportHint;
    using Portal::Network::to_stream_config;

    TEST(StreamConfigTest, ATcpEndpointKeepsItsRemoteSideAndLocalPort)
    {
        EndpointInfo info;
        info.transport = NetworkTransport::TCP;
        info.label = "tcp connect 10.0.0.2:9000";
        info.remote_address = "10.0.0.2";
        info.remote_port = 9000;
        info.local_port = 7000;

        const auto config = to_stream_config(info);

        EXPECT_EQ(config.transport, NetworkTransportHint::TCP);
        EXPECT_EQ(config.name, "tcp connect 10.0.0.2:9000");
        EXPECT_EQ(config.endpoint.address, "10.0.0.2");
        EXPECT_EQ(config.endpoint.port, 9000);
        EXPECT_EQ(config.endpoint.local_port, 7000);
        EXPECT_TRUE(config.endpoint.path.empty());
    }

    TEST(StreamConfigTest, AUdpEndpointMapsToTheUdpHint)
    {
        EndpointInfo info;
        info.transport = NetworkTransport::UDP;
        info.remote_address = "127.0.0.1";
        info.remote_port = 8000;

        const auto config = to_stream_config(info);

        EXPECT_EQ(config.transport, NetworkTransportHint::UDP);
        EXPECT_EQ(config.endpoint.address, "127.0.0.1");
        EXPECT_EQ(config.endpoint.port, 8000);
    }

    TEST(StreamConfigTest, AUnixSocketEndpointCarriesTheRemotePathAndNoAddress)
    {
        EndpointInfo info;
        info.transport = NetworkTransport::UDS;
        info.remote_path = "/run/user/1000/mayaflux.sock";
        info.local_path = "ignored.sock";
        info.remote_address = "ignored";
        info.remote_port = 1;

        const auto config = to_stream_config(info);

        EXPECT_EQ(config.transport, NetworkTransportHint::UDS);
        EXPECT_EQ(config.endpoint.path, "/run/user/1000/mayaflux.sock");
        EXPECT_TRUE(config.endpoint.address.empty());
        EXPECT_EQ(config.endpoint.port, 0);
    }

    TEST(StreamConfigTest, SharedMemoryMapsToItsHint)
    {
        EndpointInfo info;
        info.transport = NetworkTransport::SHARED_MEMORY;

        EXPECT_EQ(to_stream_config(info).transport, NetworkTransportHint::SHARED_MEMORY);
    }

    TEST(StreamConfigTest, FramingAndTheMessageSizeLimitPassThrough)
    {
        EndpointInfo info;
        info.transport = NetworkTransport::TCP;
        info.framing = FramingPolicy::LINE;
        info.max_message_size = 4096;

        const auto config = to_stream_config(info);

        EXPECT_EQ(config.framing, FramingPolicy::LINE);
        EXPECT_EQ(config.max_message_size, 4096U);
    }

    TEST(StreamConfigTest, AnEndpointLeftAtItsDefaultsKeepsAutomaticFraming)
    {
        const auto config = to_stream_config(EndpointInfo {});

        EXPECT_EQ(config.framing, FramingPolicy::AUTO);
        EXPECT_EQ(config.max_message_size, 0U);
        EXPECT_TRUE(config.name.empty());
    }

} // namespace
} // namespace MayaFlux::Test
