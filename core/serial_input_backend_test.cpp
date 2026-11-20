#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "../support/input_test_support.hpp"
#include "MayaFlux/Core/Backends/Input/SerialInputBackend.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    namespace P = MayaFlux::Transitive::Protocol;
    using namespace std::chrono_literals;
    using Core::EndpointState;
    using Core::InputDeviceInfo;
    using Core::InputType;
    using Core::InputValue;
    using Core::SerialInputBackend;
    using Core::SerialReply;
    using Core::SerialSpec;

    using Bytes = std::vector<uint8_t>;

    constexpr uint64_t k_endpoint = 42;

    SerialInputBackend::Config two_ports(FakeNetworkService& network)
    {
        SerialInputBackend::Config config;
        config.network = &network.service;
        Core::SerialPortConfig first;
        first.port_name = "/dev/ttyMF_A";
        first.line.baud_rate = 9600;
        Core::SerialPortConfig second;
        second.port_name = "/dev/ttyMF_B";
        config.ports = { first, second };
        return config;
    }

    struct BackendRig {
        BackendRig()
            : backend(two_ports(network))
        {
            backend.set_input_callback([this](const InputValue& value) { values.push_back(value); });
            backend.set_device_callback([this](const InputDeviceInfo& info, bool connected) {
                events.emplace_back(info.id, connected);
            });
        }

        [[nodiscard]] uint32_t id_of(const std::string& path) const
        {
            for (const auto& device : backend.get_devices()) {
                if (device.port_name == path) {
                    return device.id;
                }
            }
            return 0;
        }

        FakeNetworkService network;
        SerialInputBackend backend;
        std::vector<InputValue> values;
        std::vector<std::pair<uint32_t, bool>> events;
    };

    // ------------------------------------------------------------------
    // Identity and listing
    // ------------------------------------------------------------------

    TEST(SerialInputBackendTest, ReportsItsIdentityBeforeAndAfterInitialization)
    {
        BackendRig rig;

        EXPECT_EQ(rig.backend.get_type(), InputType::SERIAL);
        EXPECT_FALSE(rig.backend.get_name().empty());
        EXPECT_FALSE(rig.backend.get_version().empty());
        EXPECT_FALSE(rig.backend.is_initialized());
        EXPECT_FALSE(rig.backend.is_running());
        EXPECT_TRUE(rig.backend.get_devices().empty());

        ASSERT_TRUE(rig.backend.initialize());
        EXPECT_TRUE(rig.backend.is_initialized());
    }

    TEST(SerialInputBackendTest, WithoutANetworkServiceItCannotInitialize)
    {
        SerialInputBackend::Config config;
        SerialInputBackend backend(config);

        EXPECT_FALSE(backend.initialize());
        EXPECT_FALSE(backend.is_initialized());
    }

    TEST(SerialInputBackendTest, ConfiguredPortsAreListedWithDistinctIds)
    {
        BackendRig rig;
        ASSERT_TRUE(rig.backend.initialize());

        const auto devices = rig.backend.get_devices();

        ASSERT_EQ(devices.size(), 2U);
        EXPECT_NE(devices[0].id, 0U);
        EXPECT_NE(devices[1].id, 0U);
        EXPECT_NE(devices[0].id, devices[1].id);
        EXPECT_EQ(devices[0].backend_type, InputType::SERIAL);
        EXPECT_EQ(devices[0].port_name, "/dev/ttyMF_A");
        EXPECT_EQ(devices[0].baud_rate, 9600U);
        EXPECT_EQ(devices[1].port_name, "/dev/ttyMF_B");
        EXPECT_TRUE(devices[0].is_connected);
    }

    TEST(SerialInputBackendTest, InitializeIsIdempotentAndIdsSurviveAShutdownAndReinitialize)
    {
        BackendRig rig;
        ASSERT_TRUE(rig.backend.initialize());
        const auto id_a = rig.id_of("/dev/ttyMF_A");
        const auto id_b = rig.id_of("/dev/ttyMF_B");

        EXPECT_TRUE(rig.backend.initialize());
        EXPECT_EQ(rig.backend.get_devices().size(), 2U);
        rig.backend.shutdown();
        EXPECT_TRUE(rig.backend.get_devices().empty());
        ASSERT_TRUE(rig.backend.initialize());

        EXPECT_EQ(rig.id_of("/dev/ttyMF_A"), id_a);
        EXPECT_EQ(rig.id_of("/dev/ttyMF_B"), id_b);
    }

    TEST(SerialInputBackendTest, RefreshWithoutEnumerationKeepsTheConfiguredPorts)
    {
        BackendRig rig;
        ASSERT_TRUE(rig.backend.initialize());

        EXPECT_EQ(rig.backend.refresh_devices(), 2U);
        EXPECT_EQ(rig.backend.get_devices().size(), 2U);
    }

    TEST(SerialInputBackendTest, EnumerationKeepsConfiguredPortsAndIdsStableAcrossRefreshes)
    {
        FakeNetworkService network;
        auto config = two_ports(network);
        config.enumerate_ports = true;
        SerialInputBackend backend(config);
        ASSERT_TRUE(backend.initialize());

        const auto first = backend.get_devices();
        backend.refresh_devices();
        backend.refresh_devices();
        const auto again = backend.get_devices();

        ASSERT_GE(first.size(), 2U);
        ASSERT_EQ(again.size(), first.size());
        for (size_t i = 0; i < first.size(); ++i) {
            EXPECT_EQ(first[i].id, again[i].id);
            EXPECT_EQ(first[i].port_name, again[i].port_name);
        }
        const auto listed = [&](const std::string& path) {
            return std::ranges::any_of(again, [&](const InputDeviceInfo& d) { return d.port_name == path; });
        };
        EXPECT_TRUE(listed("/dev/ttyMF_A"));
        EXPECT_TRUE(listed("/dev/ttyMF_B"));
    }

    struct EnumeratedRig {
        EnumeratedRig()
        {
            auto config = two_ports(network);
            config.ports.clear();
            config.enumerate_ports = true;
            config.enumerator = [this] { return ports; };
            backend = std::make_unique<SerialInputBackend>(std::move(config));
            backend->set_device_callback([this](const InputDeviceInfo& info, bool connected) {
                events.emplace_back(info.id, connected);
            });
        }

        [[nodiscard]] uint32_t id_of(const std::string& path) const
        {
            for (const auto& device : backend->get_devices()) {
                if (device.port_name == path) {
                    return device.id;
                }
            }
            return 0;
        }

        FakeNetworkService network;
        std::vector<Platform::SerialPortEntry> ports;
        std::unique_ptr<SerialInputBackend> backend;
        std::vector<std::pair<uint32_t, bool>> events;
    };

    TEST(SerialInputBackendTest, EnumeratedPortsWithTheSameNameStayApartByPath)
    {
        EnumeratedRig rig;
        rig.ports = { { "/dev/mf_a", "usb-same" }, { "/dev/mf_b", "usb-same" } };
        ASSERT_TRUE(rig.backend->initialize());

        const auto devices = rig.backend->get_devices();

        ASSERT_EQ(devices.size(), 2U);
        EXPECT_NE(devices[0].id, devices[1].id);
        EXPECT_NE(rig.id_of("/dev/mf_a"), 0U);
        EXPECT_NE(rig.id_of("/dev/mf_b"), 0U);
    }

    TEST(SerialInputBackendTest, AnEnumeratedPortKeepsItsIdWhenItComesBackAndLeavesWhileUnopened)
    {
        EnumeratedRig rig;
        rig.ports = { { "/dev/mf_a", "usb-a" } };
        ASSERT_TRUE(rig.backend->initialize());
        const auto id = rig.id_of("/dev/mf_a");
        ASSERT_NE(id, 0U);

        rig.ports.clear();
        rig.backend->refresh_devices();
        EXPECT_TRUE(rig.backend->get_devices().empty());
        EXPECT_EQ(rig.events.back(), std::make_pair(id, false));

        rig.ports = { { "/dev/mf_a", "usb-a" } };
        rig.backend->refresh_devices();
        EXPECT_EQ(rig.id_of("/dev/mf_a"), id);
        EXPECT_EQ(rig.events.back(), std::make_pair(id, true));
    }

    TEST(SerialInputBackendTest, AnOpenEnumeratedPortStaysListedWhenItVanishes)
    {
        EnumeratedRig rig;
        rig.ports = { { "/dev/mf_a", "usb-a" } };
        ASSERT_TRUE(rig.backend->initialize());
        const auto id = rig.id_of("/dev/mf_a");
        ASSERT_TRUE(rig.backend->open_device(id));

        rig.ports.clear();
        rig.backend->refresh_devices();

        EXPECT_EQ(rig.id_of("/dev/mf_a"), id);
        EXPECT_TRUE(rig.backend->is_device_open(id));
    }

    TEST(SerialInputBackendTest, AnEnumeratedPortThatMovesIsOpenedAtItsNewPathNotTheOldOne)
    {
        EnumeratedRig rig;
        rig.ports = { { "/dev/mf_old", "usb-moving" } };
        ASSERT_TRUE(rig.backend->initialize());

        rig.ports = { { "/dev/mf_new", "usb-moving" } };
        rig.backend->refresh_devices();

        EXPECT_EQ(rig.id_of("/dev/mf_old"), 0U);
        const auto id = rig.id_of("/dev/mf_new");
        ASSERT_NE(id, 0U);
        ASSERT_TRUE(rig.backend->open_device(id));
        EXPECT_EQ(rig.network.opened.back().remote_path, "/dev/mf_new");
    }

    TEST(SerialInputBackendTest, OpeningFailsWhenTheDeviceLeavesTheListWhileItIsOpening)
    {
        EnumeratedRig rig;
        rig.ports = { { "/dev/mf_a", "usb-a" } };
        ASSERT_TRUE(rig.backend->initialize());
        const auto id = rig.id_of("/dev/mf_a");
        const auto original = rig.network.service.open_endpoint;
        rig.network.service.open_endpoint = [&](const Core::EndpointInfo& info) {
            const auto endpoint = original(info);
            rig.ports.clear();
            rig.backend->refresh_devices();
            return endpoint;
        };

        EXPECT_FALSE(rig.backend->open_device(id));

        EXPECT_FALSE(rig.backend->is_device_open(id));
        EXPECT_EQ(rig.network.closed.size(), 1U);
        EXPECT_TRUE(rig.backend->get_devices().empty());
    }

    TEST(SerialInputBackendTest, OpeningSucceedsWhenAnotherCallRegisteredTheDeviceFirstAndKeepsOnlyThatOne)
    {
        EnumeratedRig rig;
        rig.ports = { { "/dev/mf_a", "usb-a" } };
        ASSERT_TRUE(rig.backend->initialize());
        const auto id = rig.id_of("/dev/mf_a");
        const auto original = rig.network.service.open_endpoint;
        bool nested = false;
        rig.network.service.open_endpoint = [&](const Core::EndpointInfo& info) {
            const auto endpoint = original(info);
            if (!std::exchange(nested, true)) {
                EXPECT_TRUE(rig.backend->open_device(id));
            }
            return endpoint;
        };

        EXPECT_TRUE(rig.backend->open_device(id));

        EXPECT_TRUE(rig.backend->is_device_open(id));
        EXPECT_EQ(rig.network.opened.size(), 2U);
        EXPECT_EQ(rig.network.closed.size(), 1U);
    }

    // ------------------------------------------------------------------
    // Readings
    // ------------------------------------------------------------------

    SerialSpec bytes_between(std::string_view terminator)
    {
        auto spec = SerialSpec::delimited(terminator);
        spec.parse = Core::SerialParse::BYTES;
        return spec;
    }

    std::string text_of(const InputValue& value)
    {
        const auto& bytes = value.as_bytes();
        return { bytes.begin(), bytes.end() };
    }

    TEST(SerialInputBackendTest, AViewIsListedAsADeviceOfItsOwnThatReadsTheSamePort)
    {
        BackendRig rig;
        ASSERT_TRUE(rig.backend.initialize());
        const auto port = rig.id_of("/dev/ttyMF_A");

        const auto view = rig.backend.add_view(port, bytes_between(";"));

        ASSERT_NE(view, 0U);
        EXPECT_NE(view, port);
        EXPECT_EQ(rig.backend.get_devices().size(), 3U);
        EXPECT_EQ(rig.backend.add_view(view, bytes_between(";")), 0U);
        EXPECT_EQ(rig.backend.add_view(9999, bytes_between(";")), 0U);
        EXPECT_FALSE(rig.backend.is_device_open(view));
    }

    TEST(SerialInputBackendTest, OpeningAViewOpensItsPortAndEachReadingGetsItsOwnValues)
    {
        BackendRig rig;
        ASSERT_TRUE(rig.backend.initialize());
        rig.backend.start();
        const auto port = rig.id_of("/dev/ttyMF_A");
        const auto view = rig.backend.add_view(port, bytes_between(";"));

        ASSERT_TRUE(rig.backend.open_device(view));
        rig.network.deliver(k_endpoint, Bytes { '1', ';', '2', '\n' });

        EXPECT_TRUE(rig.backend.is_device_open(view));
        EXPECT_TRUE(rig.backend.is_device_open(port));
        EXPECT_EQ(rig.network.opened.size(), 1U);
        ASSERT_EQ(rig.values.size(), 2U);
        EXPECT_EQ(rig.values[0].device_id, port);
        EXPECT_EQ(rig.values[0].as_vector(), (std::vector<double> { 1, 2 }));
        EXPECT_EQ(rig.values[1].device_id, view);
        EXPECT_EQ(text_of(rig.values[1]), "1");
    }

    TEST(SerialInputBackendTest, ClosingAViewStopsOnlyThatReadingAndItCanBeOpenedAgain)
    {
        BackendRig rig;
        ASSERT_TRUE(rig.backend.initialize());
        rig.backend.start();
        const auto port = rig.id_of("/dev/ttyMF_A");
        const auto view = rig.backend.add_view(port, bytes_between(";"));
        ASSERT_TRUE(rig.backend.open_device(view));

        rig.backend.close_device(view);
        rig.network.deliver(k_endpoint, Bytes { '1', ';', '\n' });

        EXPECT_FALSE(rig.backend.is_device_open(view));
        EXPECT_TRUE(rig.backend.is_device_open(port));
        ASSERT_EQ(rig.values.size(), 1U);
        EXPECT_EQ(rig.values[0].device_id, port);

        rig.values.clear();
        ASSERT_TRUE(rig.backend.open_device(view));
        rig.network.deliver(k_endpoint, Bytes { '2', ';', '\n' });
        ASSERT_EQ(rig.values.size(), 2U);
        EXPECT_EQ(rig.values[1].device_id, view);
    }

    TEST(SerialInputBackendTest, AViewResumesWhenItsPortIsClosedAndOpenedAgain)
    {
        BackendRig rig;
        ASSERT_TRUE(rig.backend.initialize());
        rig.backend.start();
        const auto port = rig.id_of("/dev/ttyMF_A");
        const auto view = rig.backend.add_view(port, bytes_between(";"));
        ASSERT_TRUE(rig.backend.open_device(view));

        rig.backend.close_device(port);
        ASSERT_TRUE(rig.backend.open_device(port));
        rig.network.deliver(k_endpoint, Bytes { '5', ';', '\n' });

        ASSERT_EQ(rig.values.size(), 2U);
        EXPECT_EQ(rig.values[1].device_id, view);
        EXPECT_EQ(text_of(rig.values[1]), "5");
    }

    TEST(SerialInputBackendTest, WritesToAViewGoToItsPort)
    {
        BackendRig rig;
        ASSERT_TRUE(rig.backend.initialize());
        const auto port = rig.id_of("/dev/ttyMF_A");
        const auto view = rig.backend.add_view(port, bytes_between(";"));
        ASSERT_TRUE(rig.backend.open_device(view));

        EXPECT_EQ(rig.backend.write(view, Bytes { 'h', 'i' }), 2);

        ASSERT_EQ(rig.network.sent.size(), 1U);
        EXPECT_EQ(rig.network.sent[0], (Bytes { 'h', 'i' }));
        EXPECT_EQ(rig.backend.get_device(view), rig.backend.get_device(port));
    }

    TEST(SerialInputBackendTest, AReaderIsNotListedAndKeepsReadingAcrossAReopen)
    {
        BackendRig rig;
        ASSERT_TRUE(rig.backend.initialize());
        rig.backend.start();
        const auto port = rig.id_of("/dev/ttyMF_A");
        std::vector<std::string> seen;
        const auto token = rig.backend.add_reader(port, bytes_between(";"),
            [&](const InputValue& value) { seen.push_back(text_of(value)); });
        ASSERT_NE(token, 0U);
        ASSERT_TRUE(rig.backend.open_device(port));

        rig.network.deliver(k_endpoint, Bytes { 'a', ';' });
        rig.backend.close_device(port);
        ASSERT_TRUE(rig.backend.open_device(port));
        rig.network.deliver(k_endpoint, Bytes { 'b', ';' });
        rig.backend.remove_reader(token);
        rig.network.deliver(k_endpoint, Bytes { 'c', ';' });

        EXPECT_EQ(seen, (std::vector<std::string> { "a", "b" }));
        EXPECT_EQ(rig.backend.get_devices().size(), 2U);
        EXPECT_TRUE(rig.values.empty());
    }

    TEST(SerialInputBackendTest, AReaderOfAnUnknownDeviceOrAViewIsRefused)
    {
        BackendRig rig;
        ASSERT_TRUE(rig.backend.initialize());
        const auto port = rig.id_of("/dev/ttyMF_A");
        const auto view = rig.backend.add_view(port, bytes_between(";"));
        const auto callback = [](const InputValue&) { };

        EXPECT_EQ(rig.backend.add_reader(9999, bytes_between(";"), callback), 0U);
        EXPECT_EQ(rig.backend.add_reader(view, bytes_between(";"), callback), 0U);
        EXPECT_EQ(rig.backend.add_reader(port, bytes_between(";"), nullptr), 0U);
    }

    TEST(SerialInputBackendTest, AViewOfAnUnopenedEnumeratedPortLeavesWithIt)
    {
        EnumeratedRig rig;
        rig.ports = { { "/dev/mf_a", "usb-a" } };
        ASSERT_TRUE(rig.backend->initialize());
        const auto port = rig.id_of("/dev/mf_a");
        ASSERT_NE(rig.backend->add_view(port, bytes_between(";")), 0U);
        ASSERT_EQ(rig.backend->get_devices().size(), 2U);

        rig.ports.clear();
        rig.backend->refresh_devices();

        EXPECT_TRUE(rig.backend->get_devices().empty());
    }

    // ------------------------------------------------------------------
    // Opening and closing
    // ------------------------------------------------------------------

    TEST(SerialInputBackendTest, OpeningADeviceOpensASerialEndpointOnce)
    {
        BackendRig rig;
        ASSERT_TRUE(rig.backend.initialize());
        const auto id = rig.id_of("/dev/ttyMF_A");

        EXPECT_FALSE(rig.backend.is_device_open(id));
        ASSERT_TRUE(rig.backend.open_device(id));
        EXPECT_TRUE(rig.backend.open_device(id));

        ASSERT_EQ(rig.network.opened.size(), 1U);
        EXPECT_EQ(rig.network.opened[0].transport, Core::NetworkTransport::SERIAL);
        EXPECT_EQ(rig.network.opened[0].remote_path, "/dev/ttyMF_A");
        EXPECT_EQ(rig.network.opened[0].serial.baud_rate, 9600U);
        EXPECT_TRUE(rig.backend.is_device_open(id));
        EXPECT_EQ(rig.backend.get_open_devices(), (std::vector<uint32_t> { id }));
        EXPECT_NE(rig.backend.get_device(id), nullptr);
    }

    TEST(SerialInputBackendTest, AnEndpointThatWillNotOpenLeavesTheDeviceClosed)
    {
        BackendRig rig;
        ASSERT_TRUE(rig.backend.initialize());
        const auto id = rig.id_of("/dev/ttyMF_A");
        rig.network.next_endpoint_id = 0;

        EXPECT_FALSE(rig.backend.open_device(id));

        EXPECT_FALSE(rig.backend.is_device_open(id));
        EXPECT_TRUE(rig.backend.get_open_devices().empty());
        EXPECT_EQ(rig.backend.get_device(id), nullptr);
    }

    TEST(SerialInputBackendTest, UnknownIdsAreRejectedOrIgnored)
    {
        BackendRig rig;
        ASSERT_TRUE(rig.backend.initialize());

        EXPECT_FALSE(rig.backend.open_device(999));
        EXPECT_FALSE(rig.backend.is_device_open(999));
        EXPECT_EQ(rig.backend.get_device(999), nullptr);
        EXPECT_EQ(rig.backend.write(999, Bytes { 1 }), -1);
        EXPECT_FALSE(rig.backend.set_device_spec(999, SerialSpec::lines()));
        EXPECT_NO_THROW(rig.backend.close_device(999));
        EXPECT_TRUE(rig.network.opened.empty());
    }

    TEST(SerialInputBackendTest, ClosingADeviceClosesItsEndpointAndAllowsReopening)
    {
        BackendRig rig;
        ASSERT_TRUE(rig.backend.initialize());
        const auto id = rig.id_of("/dev/ttyMF_A");
        ASSERT_TRUE(rig.backend.open_device(id));

        rig.backend.close_device(id);

        EXPECT_EQ(rig.network.closed, (std::vector<uint64_t> { k_endpoint }));
        EXPECT_FALSE(rig.backend.is_device_open(id));
        EXPECT_NO_THROW(rig.backend.close_device(id));
        EXPECT_TRUE(rig.backend.open_device(id));
        EXPECT_EQ(rig.network.opened.size(), 2U);
    }

    TEST(SerialInputBackendTest, ShutdownClosesEveryOpenDeviceAndTheDestructorIsClean)
    {
        FakeNetworkService network;
        {
            SerialInputBackend backend(two_ports(network));
            ASSERT_TRUE(backend.initialize());
            for (const auto& device : backend.get_devices()) {
                ASSERT_TRUE(backend.open_device(device.id));
            }
            backend.shutdown();

            EXPECT_EQ(network.closed.size(), 2U);
            EXPECT_TRUE(backend.get_open_devices().empty());
            EXPECT_FALSE(backend.is_initialized());
            EXPECT_NO_THROW(backend.shutdown());
        }
        {
            SerialInputBackend backend(two_ports(network));
            ASSERT_TRUE(backend.initialize());
            ASSERT_TRUE(backend.open_device(backend.get_devices().front().id));
        }

        EXPECT_EQ(network.closed.size(), 3U);
    }

    // ------------------------------------------------------------------
    // Delivery
    // ------------------------------------------------------------------

    TEST(SerialInputBackendTest, ValuesReachTheInputCallbackOnlyWhileTheBackendIsRunning)
    {
        BackendRig rig;
        ASSERT_TRUE(rig.backend.initialize());
        const auto id = rig.id_of("/dev/ttyMF_A");
        ASSERT_TRUE(rig.backend.open_device(id));

        rig.network.deliver(k_endpoint, Bytes { '1', ' ', '2', '\n' });
        EXPECT_TRUE(rig.values.empty());

        rig.backend.start();
        rig.network.deliver(k_endpoint, Bytes { '3', '\n' });
        ASSERT_EQ(rig.values.size(), 1U);
        EXPECT_EQ(rig.values[0].as_vector(), (std::vector<double> { 3 }));
        EXPECT_EQ(rig.values[0].device_id, id);
        EXPECT_EQ(rig.values[0].source_type, InputType::SERIAL);

        rig.backend.stop();
        rig.network.deliver(k_endpoint, Bytes { '4', '\n' });
        EXPECT_EQ(rig.values.size(), 1U);

        rig.backend.start();
        rig.network.deliver(k_endpoint, Bytes { '5', '\n' });
        EXPECT_EQ(rig.values.size(), 2U);
    }

    TEST(SerialInputBackendTest, ASpecGivenBeforeOpeningDecidesFramingAndParsing)
    {
        BackendRig rig;
        ASSERT_TRUE(rig.backend.initialize());
        const auto id = rig.id_of("/dev/ttyMF_A");
        rig.backend.start();

        EXPECT_TRUE(rig.backend.set_device_spec(id, SerialSpec::delimited(">").bytes()));
        ASSERT_TRUE(rig.backend.open_device(id));
        EXPECT_FALSE(rig.backend.set_device_spec(id, SerialSpec::lines()));
        rig.network.deliver(k_endpoint, Bytes { 'O', 'K', '>' });

        ASSERT_EQ(rig.values.size(), 1U);
        EXPECT_EQ(rig.values[0].type, InputValue::Type::BYTES);
        EXPECT_EQ(rig.values[0].as_bytes(), (Bytes { 'O', 'K' }));
    }

    TEST(SerialInputBackendTest, AConfiguredPortIsReadTheWayItsProtocolSays)
    {
        FakeNetworkService network;
        auto config = two_ports(network);
        config.ports[1].protocol.framing = Core::SerialFraming::DELIMITED;
        config.ports[1].protocol.terminator = ">";
        config.ports[1].protocol.parse = Core::SerialParse::BYTES;
        SerialInputBackend backend(config);
        std::vector<InputValue> values;
        backend.set_input_callback([&](const InputValue& value) { values.push_back(value); });
        ASSERT_TRUE(backend.initialize());
        backend.start();
        uint32_t id = 0;
        for (const auto& device : backend.get_devices()) {
            if (device.port_name == "/dev/ttyMF_B") {
                id = device.id;
            }
        }
        ASSERT_TRUE(backend.open_device(id));

        network.deliver(k_endpoint, Bytes { 'A', 'B', '>' });

        ASSERT_EQ(values.size(), 1U);
        EXPECT_EQ(values[0].type, InputValue::Type::BYTES);
        EXPECT_EQ(values[0].as_bytes(), (Bytes { 'A', 'B' }));
    }

    TEST(SerialInputBackendTest, AddPortListsANewPortAndUpdatesAnUnopenedOne)
    {
        BackendRig rig;
        Core::SerialPortConfig port;
        port.port_name = "/dev/ttyMF_C";
        port.line.baud_rate = 31250;

        EXPECT_EQ(rig.backend.add_port(port), 0U);
        ASSERT_TRUE(rig.backend.initialize());
        const auto id = rig.backend.add_port(port);

        EXPECT_NE(id, 0U);
        EXPECT_EQ(rig.backend.get_devices().size(), 3U);
        EXPECT_EQ(rig.id_of("/dev/ttyMF_C"), id);

        port.line.baud_rate = 250000;
        EXPECT_EQ(rig.backend.add_port(port), id);
        EXPECT_EQ(rig.backend.get_devices().size(), 3U);
        ASSERT_TRUE(rig.backend.open_device(id));
        EXPECT_EQ(rig.network.opened.back().serial.baud_rate, 250000U);
    }

    TEST(SerialInputBackendTest, AddPortLeavesAnOpenDeviceAlone)
    {
        BackendRig rig;
        ASSERT_TRUE(rig.backend.initialize());
        const auto id = rig.id_of("/dev/ttyMF_A");
        ASSERT_TRUE(rig.backend.open_device(id));
        Core::SerialPortConfig port;
        port.port_name = "/dev/ttyMF_A";
        port.line.baud_rate = 4800;

        EXPECT_EQ(rig.backend.add_port(port), id);

        EXPECT_EQ(rig.backend.get_devices().front().baud_rate, 9600U);
        EXPECT_TRUE(rig.backend.is_device_open(id));
    }

    TEST(SerialInputBackendTest, WriteSendsBytesAsTheyAreAndReportsRefusals)
    {
        BackendRig rig;
        ASSERT_TRUE(rig.backend.initialize());
        const auto id = rig.id_of("/dev/ttyMF_A");

        EXPECT_EQ(rig.backend.write(id, Bytes { 1, 2 }), -1);
        ASSERT_TRUE(rig.backend.open_device(id));
        EXPECT_EQ(rig.backend.write(id, Bytes { 1, 2, 3 }), 3);
        EXPECT_EQ(rig.network.sent.back(), (Bytes { 1, 2, 3 }));
        rig.network.send_result = false;
        EXPECT_EQ(rig.backend.write(id, Bytes { 1 }), -1);
    }

    TEST(SerialInputBackendTest, FeatureReportsAreNotSupported)
    {
        BackendRig rig;
        ASSERT_TRUE(rig.backend.initialize());

        EXPECT_EQ(rig.backend.send_feature_report(1, Bytes { 1 }), -1);
    }

    TEST(SerialInputBackendTest, TheDeviceTakenFromTheBackendAnswersRequests)
    {
        BackendRig rig;
        ASSERT_TRUE(rig.backend.initialize());
        const auto id = rig.id_of("/dev/ttyMF_A");
        ASSERT_TRUE(rig.backend.open_device(id));
        const auto device = rig.backend.get_device(id);
        ASSERT_NE(device, nullptr);
        std::vector<SerialReply> outcomes;

        ASSERT_TRUE(device->request(
            Bytes { '?', '\n' },
            [](P::Message m) { return P::as_text(m) == "ok"; },
            50ms,
            [&](SerialReply how, P::Message) { outcomes.push_back(how); }));
        EXPECT_EQ(rig.network.sent.back(), (Bytes { '?', '\n' }));
        rig.network.deliver(k_endpoint, Bytes { 'o', 'k', '\n' });

        EXPECT_EQ(outcomes, (std::vector<SerialReply> { SerialReply::REPLIED }));
    }

    // ------------------------------------------------------------------
    // Device events
    // ------------------------------------------------------------------

    TEST(SerialInputBackendTest, DeviceEventsFollowTheStateOfThePort)
    {
        BackendRig rig;
        ASSERT_TRUE(rig.backend.initialize());
        const auto id = rig.id_of("/dev/ttyMF_A");

        ASSERT_TRUE(rig.backend.open_device(id));
        ASSERT_EQ(rig.events.size(), 1U);
        EXPECT_EQ(rig.events[0], std::make_pair(id, true));

        rig.network.transition(k_endpoint, EndpointState::OPEN, EndpointState::RECONNECTING);
        ASSERT_EQ(rig.events.size(), 2U);
        EXPECT_EQ(rig.events[1], std::make_pair(id, false));
        EXPECT_FALSE(rig.backend.get_devices().front().is_connected);

        rig.network.transition(k_endpoint, EndpointState::RECONNECTING, EndpointState::ERROR);
        EXPECT_EQ(rig.events.size(), 2U);

        rig.network.transition(k_endpoint, EndpointState::ERROR, EndpointState::OPEN);
        ASSERT_EQ(rig.events.size(), 3U);
        EXPECT_EQ(rig.events[2], std::make_pair(id, true));
        EXPECT_TRUE(rig.backend.get_devices().front().is_connected);
    }

    TEST(SerialInputBackendTest, ADeviceCallbackMayCallBackIntoTheBackend)
    {
        BackendRig rig;
        ASSERT_TRUE(rig.backend.initialize());
        const auto id = rig.id_of("/dev/ttyMF_A");
        rig.backend.set_device_callback([&](const InputDeviceInfo& info, bool connected) {
            if (connected) {
                EXPECT_TRUE(rig.backend.is_device_open(info.id) || !rig.backend.get_devices().empty());
                rig.backend.write(info.id, Bytes { 1 });
            } else {
                rig.backend.close_device(info.id);
            }
        });
        ASSERT_TRUE(rig.backend.open_device(id));

        rig.network.transition(k_endpoint, EndpointState::OPEN, EndpointState::RECONNECTING);

        EXPECT_FALSE(rig.backend.is_device_open(id));
        EXPECT_EQ(rig.network.closed.size(), 1U);
    }

    TEST(SerialInputBackendTest, WithoutCallbacksNothingBreaks)
    {
        FakeNetworkService network;
        SerialInputBackend backend(two_ports(network));
        ASSERT_TRUE(backend.initialize());
        backend.start();
        ASSERT_TRUE(backend.open_device(backend.get_devices().front().id));

        network.deliver(k_endpoint, Bytes { '1', '\n' });
        network.transition(k_endpoint, EndpointState::OPEN, EndpointState::RECONNECTING);

        EXPECT_TRUE(backend.is_running());
    }

} // namespace
} // namespace MayaFlux::Test
