#include "gtest/gtest.h"
#include "../../support/test_metadata.hpp"

#include "../../support/input_test_support.hpp"
#include "MayaFlux/Core/Backends/Input/SerialInputBackend.hpp"
#include "MayaFlux/Portal/Network/SerialSource.hpp"
#include "MayaFlux/Vruta/NetworkSource.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Bytes = std::vector<uint8_t>;
    constexpr uint64_t k_endpoint = 42;

    struct SourceRig {
        SourceRig()
            : backend(configuration(network))
        {
            EXPECT_TRUE(backend.initialize());
            backend.start();
            backend.set_input_callback([this](const Core::InputValue& value) { values.push_back(value); });
            port = backend.get_devices().front().id;
            input.service.add_serial_reader = [this](uint32_t id, const Core::SerialSpec& spec,
                                                  std::function<void(const Core::InputValue&)> callback) {
                return backend.add_reader(id, spec, std::move(callback));
            };
            input.service.remove_serial_reader = [this](uint64_t token) { backend.remove_reader(token); };
            EXPECT_TRUE(backend.open_device(port));
        }

        static Core::SerialInputBackend::Config configuration(FakeNetworkService& network)
        {
            Core::SerialInputBackend::Config config;
            config.network = &network.service;
            Core::SerialPortConfig first;
            first.port_name = "/dev/ttyMF_A";
            config.ports = { first };
            return config;
        }

        FakeNetworkService network;
        FakeInputService input;
        Core::SerialInputBackend backend;
        std::vector<Core::InputValue> values;
        uint32_t port {};
    };

    TEST(SerialSourceTest, EveryFramedMessageBecomesOneNetworkMessageWhileTheDeviceReadsOn)
    {
        SourceRig rig;
        const auto source = Portal::Network::serial_source(rig.port, Core::SerialSpec::delimited(";"));
        ASSERT_NE(source, nullptr);

        rig.network.deliver(k_endpoint, Bytes { 'a', ';', 'b', ';', 'c' });
        rig.network.deliver(k_endpoint, Bytes { ';', '\n' });

        EXPECT_EQ(source->pending_count(), 3U);
        EXPECT_TRUE(rig.values.empty());
        rig.network.deliver(k_endpoint, Bytes { '7', '\n' });
        ASSERT_EQ(rig.values.size(), 1U);
        EXPECT_EQ(rig.values[0].as_vector(), (std::vector<double> { 7 }));
    }

    TEST(SerialSourceTest, TwoSourcesFrameTheSameBytesDifferently)
    {
        SourceRig rig;
        const auto semicolons = Portal::Network::serial_source(rig.port, Core::SerialSpec::delimited(";"));
        const auto lines = Portal::Network::serial_source(rig.port, Core::SerialSpec::lines());

        rig.network.deliver(k_endpoint, Bytes { 'a', ';', 'b', ';', 'c', '\n' });

        EXPECT_EQ(semicolons->pending_count(), 2U);
        EXPECT_EQ(lines->pending_count(), 1U);
    }

    TEST(SerialSourceTest, DroppingTheSourceStopsItsReading)
    {
        SourceRig rig;
        auto source = Portal::Network::serial_source(rig.port, Core::SerialSpec::delimited(";"));
        const auto keep = Portal::Network::serial_source(rig.port, Core::SerialSpec::delimited(";"));

        rig.network.deliver(k_endpoint, Bytes { 'a', ';' });
        source.reset();
        rig.network.deliver(k_endpoint, Bytes { 'b', ';' });

        EXPECT_EQ(keep->pending_count(), 2U);
    }

    TEST(SerialSourceTest, ASourceOfAnUnknownDeviceOrWithoutTheServiceIsRefused)
    {
        {
            SourceRig rig;
            EXPECT_EQ(Portal::Network::serial_source(9999, Core::SerialSpec::lines()), nullptr);
        }
        EXPECT_EQ(Portal::Network::serial_source(1, Core::SerialSpec::lines()), nullptr);
    }

} // namespace
} // namespace MayaFlux::Test
