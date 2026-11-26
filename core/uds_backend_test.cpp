#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include <chrono>
#include <fstream>
#include <string_view>

#include <asio/local/stream_protocol.hpp>
#include <asio/write.hpp>

#include "MayaFlux/Core/Backends/Network/UDSBackend.hpp"
#include "MayaFlux/Transitive/Platform/UnixSocket.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp>() }))

namespace MayaFlux::Test {
namespace {

    using namespace std::chrono_literals;
    using Core::EndpointInfo;
    using Core::EndpointRole;
    using Core::EndpointState;
    using Core::UDSBackend;
    using Core::UDSBackendInfo;
    using Stream = asio::local::stream_protocol;

    constexpr uint64_t k_uds_backend_first_accepted_id = 100;

    std::atomic<uint32_t> g_directory_counter { 0 };

    class SocketDirectory {
    public:
        SocketDirectory()
            : m_path(std::filesystem::temp_directory_path()
                  / ("mfu" + std::to_string(g_directory_counter.fetch_add(1)) + "t" + std::to_string(run_token())))
        {
            std::error_code ec;
            std::filesystem::remove_all(m_path, ec);
            std::filesystem::create_directory(m_path);
        }

        ~SocketDirectory()
        {
            std::error_code ec;
            std::filesystem::remove_all(m_path, ec);
        }

        SocketDirectory(const SocketDirectory&) = delete;
        SocketDirectory& operator=(const SocketDirectory&) = delete;
        SocketDirectory(SocketDirectory&&) = delete;
        SocketDirectory& operator=(SocketDirectory&&) = delete;

        [[nodiscard]] std::string socket(const std::string& name = "s.sock") const
        {
            return (m_path / name).string();
        }

        [[nodiscard]] const std::filesystem::path& path() const { return m_path; }

    private:
        static uint64_t run_token()
        {
            static const uint64_t token = static_cast<uint64_t>(
                                              std::chrono::steady_clock::now().time_since_epoch().count())
                % 100000;
            return token;
        }

        std::filesystem::path m_path;
    };

    class ScopedRuntimeDirectory {
    public:
        explicit ScopedRuntimeDirectory(const std::filesystem::path& directory)
        {
            const char* current = std::getenv(variable());
            if (current != nullptr) {
                m_previous = current;
                m_had_previous = true;
            }
            set(directory.string().c_str());
        }

        ~ScopedRuntimeDirectory()
        {
            if (m_had_previous) {
                set(m_previous.c_str());
            } else {
                unset();
            }
        }

        ScopedRuntimeDirectory(const ScopedRuntimeDirectory&) = delete;
        ScopedRuntimeDirectory& operator=(const ScopedRuntimeDirectory&) = delete;
        ScopedRuntimeDirectory(ScopedRuntimeDirectory&&) = delete;
        ScopedRuntimeDirectory& operator=(ScopedRuntimeDirectory&&) = delete;

    private:
        static const char* variable()
        {
#if defined(_WIN32)
            return "TEMP";
#elif defined(__APPLE__)
            return "TMPDIR";
#else
            return "XDG_RUNTIME_DIR";
#endif
        }

        static void set(const char* value)
        {
#if defined(_WIN32)
            _putenv_s(variable(), value);
#else
            ::setenv(variable(), value, 1);
#endif
        }

        static void unset()
        {
#if defined(_WIN32)
            _putenv_s(variable(), "");
#else
            ::unsetenv(variable());
#endif
        }

        std::string m_previous;
        bool m_had_previous { false };
    };

    struct UdsBackendReceived {
        uint64_t id {};
        std::vector<uint8_t> bytes;
        std::string sender;
    };

    struct UdsBackendStateEvent {
        uint64_t id {};
        EndpointState previous {};
        EndpointState current {};
    };

    UDSBackendInfo uds_backend_reconnecting(bool enabled, uint32_t interval_ms = 20)
    {
        UDSBackendInfo info;
        info.auto_reconnect = enabled;
        info.reconnect_interval_ms = interval_ms;
        return info;
    }

    class UdsRig {
    public:
        explicit UdsRig(const UDSBackendInfo& config = uds_backend_reconnecting(false))
            : backend(config, context)
        {
            backend.set_endpoint_id_allocator([this] { return next_accepted_id++; });
            backend.set_receive_callback(
                [this](uint64_t id, const uint8_t* data, size_t size, std::string_view sender) {
                    received.push_back({ .id = id, .bytes = std::vector<uint8_t>(data, data + size), .sender = std::string(sender) });
                });
            backend.set_state_callback(
                [this](const EndpointInfo& info, EndpointState previous, EndpointState current) {
                    states.push_back({ .id = info.id, .previous = previous, .current = current });
                });
        }

        UdsRig(const UdsRig&) = delete;
        UdsRig& operator=(const UdsRig&) = delete;
        UdsRig(UdsRig&&) = delete;
        UdsRig& operator=(UdsRig&&) = delete;

        ~UdsRig() { backend.shutdown(); }

        bool up()
        {
            if (!backend.initialize())
                return false;
            backend.start();
            return true;
        }

        template <typename Predicate>
        bool pump_until(Predicate predicate, std::chrono::milliseconds timeout = 3s)
        {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (std::chrono::steady_clock::now() < deadline) {
                poll();
                if (predicate())
                    return true;
                std::this_thread::sleep_for(1ms);
            }
            poll();
            return predicate();
        }

        void poll()
        {
            if (context.stopped())
                context.restart();
            context.poll();
        }

        [[nodiscard]] size_t count_for(uint64_t id) const
        {
            size_t count = 0;
            for (const auto& item : received) {
                if (item.id == id)
                    ++count;
            }
            return count;
        }

        [[nodiscard]] std::vector<uint8_t> concatenated(uint64_t id) const
        {
            std::vector<uint8_t> all;
            for (const auto& item : received) {
                if (item.id == id)
                    all.insert(all.end(), item.bytes.begin(), item.bytes.end());
            }
            return all;
        }

        [[nodiscard]] bool saw(uint64_t id, EndpointState previous, EndpointState current) const
        {
            return std::ranges::any_of(states, [&](const UdsBackendStateEvent& event) {
                return event.id == id && event.previous == previous && event.current == current;
            });
        }

        [[nodiscard]] uint64_t accepted_id() const
        {
            for (const auto& event : states) {
                if (event.id >= k_uds_backend_first_accepted_id && event.previous == EndpointState::CLOSED
                    && event.current == EndpointState::OPEN)
                    return event.id;
            }
            return 0;
        }

        uint64_t connect_pair(const std::string& path)
        {
            if (backend.open_endpoint(listener(1, path)) != 1)
                return 0;
            if (backend.open_endpoint(outbound(2, path)) != 2)
                return 0;
            const bool connected = pump_until([&] {
                return backend.get_endpoint_state(2) == EndpointState::OPEN && accepted_id() != 0;
            });
            return connected ? accepted_id() : 0;
        }

        static EndpointInfo listener(uint64_t id, const std::string& path,
            Core::FramingPolicy framing = Core::FramingPolicy::AUTO, size_t max_message_size = 0)
        {
            EndpointInfo info;
            info.id = id;
            info.transport = Core::NetworkTransport::UDS;
            info.role = EndpointRole::BIDIRECTIONAL;
            info.local_path = path;
            info.framing = framing;
            info.max_message_size = max_message_size;
            return info;
        }

        static EndpointInfo outbound(uint64_t id, const std::string& path,
            Core::FramingPolicy framing = Core::FramingPolicy::AUTO, size_t max_message_size = 0)
        {
            EndpointInfo info;
            info.id = id;
            info.transport = Core::NetworkTransport::UDS;
            info.role = EndpointRole::BIDIRECTIONAL;
            info.remote_path = path;
            info.framing = framing;
            info.max_message_size = max_message_size;
            return info;
        }

        asio::io_context context;
        UDSBackend backend;
        uint64_t next_accepted_id { k_uds_backend_first_accepted_id };
        std::vector<UdsBackendReceived> received;
        std::vector<UdsBackendStateEvent> states;
    };

    struct UdsBackendRawPeer {
        UdsBackendRawPeer(UdsRig& rig, const std::string& path)
            : socket(rig.context)
        {
            socket.connect(Stream::endpoint(path));
        }

        void write(std::initializer_list<uint8_t> bytes) { asio::write(socket, asio::buffer(std::vector<uint8_t>(bytes))); }

        Stream::socket socket;
    };

    std::vector<uint8_t> uds_backend_bytes_of(std::initializer_list<uint8_t> values)
    {
        return values;
    }

    bool is_socket_file(const std::string& path)
    {
        return Platform::is_unix_socket(path);
    }

    // ------------------------------------------------------------------
    // Lifecycle
    // ------------------------------------------------------------------

    MAYAFLUX_TEST_WITH(TEST, UDSBackendTest, ReportsIdentity, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        UdsRig rig;

        EXPECT_EQ(rig.backend.get_transport(), Core::NetworkTransport::UDS);
        EXPECT_FALSE(rig.backend.get_name().empty());
        EXPECT_FALSE(rig.backend.get_version().empty());
        EXPECT_FALSE(rig.backend.is_initialized());
        EXPECT_FALSE(rig.backend.is_running());
    }

    MAYAFLUX_TEST_WITH(TEST, UDSBackendTest, LifecycleCallsAreIdempotent, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        UdsRig rig;

        ASSERT_TRUE(rig.backend.initialize());
        EXPECT_TRUE(rig.backend.initialize());

        EXPECT_NO_THROW(rig.backend.stop());
        rig.backend.start();
        EXPECT_TRUE(rig.backend.is_running());
        EXPECT_NO_THROW(rig.backend.start());

        rig.backend.stop();
        EXPECT_FALSE(rig.backend.is_running());

        rig.backend.shutdown();
        EXPECT_FALSE(rig.backend.is_initialized());
        EXPECT_NO_THROW(rig.backend.shutdown());
    }

    TEST(UDSBackendTest, ShutdownClosesEveryEndpointAndRemovesTheSocketFile)
    {
        SocketDirectory directory;
        UdsRig rig;
        ASSERT_TRUE(rig.up());
        ASSERT_NE(rig.connect_pair(directory.socket()), 0U);
        ASSERT_TRUE(is_socket_file(directory.socket()));

        rig.backend.shutdown();

        EXPECT_TRUE(rig.backend.get_endpoints().empty());
        EXPECT_FALSE(std::filesystem::exists(directory.socket()));
    }

    // ------------------------------------------------------------------
    // Listeners
    // ------------------------------------------------------------------

    TEST(UDSBackendTest, ListenerOpensAndAnnouncesItself)
    {
        SocketDirectory directory;
        UdsRig rig;
        ASSERT_TRUE(rig.up());

        const uint64_t id = rig.backend.open_endpoint(UdsRig::listener(1, directory.socket()));

        EXPECT_EQ(id, 1U);
        EXPECT_EQ(rig.backend.get_endpoint_state(1), EndpointState::OPEN);
        EXPECT_TRUE(rig.saw(1, EndpointState::OPENING, EndpointState::OPEN));
        EXPECT_TRUE(is_socket_file(directory.socket()));
        const auto endpoints = rig.backend.get_endpoints();
        ASSERT_EQ(endpoints.size(), 1U);
        EXPECT_EQ(endpoints.front().local_path, directory.socket());
    }

#if !defined(_WIN32)
    TEST(UDSBackendTest, ListenerSocketIsReadableAndWritableByItsOwnerOnly)
    {
        SocketDirectory directory;
        UdsRig rig;
        ASSERT_TRUE(rig.up());
        ASSERT_EQ(rig.backend.open_endpoint(UdsRig::listener(1, directory.socket())), 1U);

        const auto permissions = std::filesystem::status(directory.socket()).permissions();

        EXPECT_EQ(permissions & std::filesystem::perms::all,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    }
#endif

    TEST(UDSBackendTest, ClosingAListenerRemovesItsSocketFileAndAllowsReopening)
    {
        SocketDirectory directory;
        UdsRig rig;
        ASSERT_TRUE(rig.up());
        ASSERT_EQ(rig.backend.open_endpoint(UdsRig::listener(1, directory.socket())), 1U);

        rig.backend.close_endpoint(1);

        EXPECT_EQ(rig.backend.get_endpoint_state(1), EndpointState::CLOSED);
        EXPECT_TRUE(rig.backend.get_endpoints().empty());
        EXPECT_FALSE(std::filesystem::exists(directory.socket()));
        EXPECT_EQ(rig.backend.open_endpoint(UdsRig::listener(3, directory.socket())), 3U);
    }

    TEST(UDSBackendTest, SecondListenerOnTheSamePathIsRefusedAndTheFirstKeepsServing)
    {
        SocketDirectory directory;
        UdsRig rig;
        ASSERT_TRUE(rig.up());
        ASSERT_EQ(rig.backend.open_endpoint(UdsRig::listener(1, directory.socket())), 1U);

        EXPECT_EQ(rig.backend.open_endpoint(UdsRig::listener(2, directory.socket())), 0U);

        EXPECT_EQ(rig.backend.get_endpoints().size(), 1U);
        EXPECT_TRUE(is_socket_file(directory.socket()));
        EXPECT_EQ(rig.backend.open_endpoint(UdsRig::outbound(3, directory.socket())), 3U);
        ASSERT_TRUE(rig.pump_until([&] { return rig.backend.get_endpoint_state(3) == EndpointState::OPEN; }));
    }

    TEST(UDSBackendTest, StaleSocketFileIsReplaced)
    {
        SocketDirectory directory;
        {
            asio::io_context context;
            Stream::acceptor stale(context);
            stale.open(Stream());
            stale.bind(Stream::endpoint(directory.socket()));
            stale.listen(asio::socket_base::max_listen_connections);
        }
        ASSERT_TRUE(is_socket_file(directory.socket()));
        UdsRig rig;
        ASSERT_TRUE(rig.up());

        EXPECT_EQ(rig.backend.open_endpoint(UdsRig::listener(1, directory.socket())), 1U);
        EXPECT_EQ(rig.backend.get_endpoint_state(1), EndpointState::OPEN);
    }

    TEST(UDSBackendTest, RegularFileAtThePathIsRefusedAndLeftAlone)
    {
        SocketDirectory directory;
        {
            std::ofstream file(directory.socket());
            file << "not a socket";
        }
        UdsRig rig;
        ASSERT_TRUE(rig.up());

        EXPECT_EQ(rig.backend.open_endpoint(UdsRig::listener(1, directory.socket())), 0U);

        EXPECT_TRUE(std::filesystem::is_regular_file(directory.socket()));
    }

    TEST(UDSBackendTest, MissingDirectoryRefusesTheListener)
    {
        SocketDirectory directory;
        UdsRig rig;
        ASSERT_TRUE(rig.up());

        EXPECT_EQ(rig.backend.open_endpoint(UdsRig::listener(1, (directory.path() / "absent" / "s.sock").string())), 0U);
        EXPECT_TRUE(rig.backend.get_endpoints().empty());
    }

    TEST(UDSBackendTest, BareNameIsPlacedInTheRuntimeDirectory)
    {
        SocketDirectory directory;
        const ScopedRuntimeDirectory runtime(directory.path());
        UdsRig rig;
        ASSERT_TRUE(rig.up());

        ASSERT_EQ(rig.backend.open_endpoint(UdsRig::listener(1, "named.sock")), 1U);
        ASSERT_EQ(rig.backend.open_endpoint(UdsRig::outbound(2, "named.sock")), 2U);

        EXPECT_TRUE(is_socket_file((directory.path() / "named.sock").string()));
        ASSERT_TRUE(rig.pump_until([&] { return rig.backend.get_endpoint_state(2) == EndpointState::OPEN; }));
    }

    // ------------------------------------------------------------------
    // Connections
    // ------------------------------------------------------------------

    TEST(UDSBackendTest, OutboundConnectionOpensAndTheListenerAcceptsIt)
    {
        SocketDirectory directory;
        UdsRig rig;
        ASSERT_TRUE(rig.up());

        const uint64_t accepted = rig.connect_pair(directory.socket());

        ASSERT_GE(accepted, k_uds_backend_first_accepted_id);
        EXPECT_EQ(rig.backend.get_endpoint_state(2), EndpointState::OPEN);
        EXPECT_EQ(rig.backend.get_endpoint_state(accepted), EndpointState::OPEN);
        EXPECT_TRUE(rig.saw(2, EndpointState::OPENING, EndpointState::OPEN));
        EXPECT_EQ(rig.backend.get_endpoints().size(), 3U);
    }

    TEST(UDSBackendTest, AcceptedConnectionNamesItsListenerAndPath)
    {
        SocketDirectory directory;
        UdsRig rig;
        ASSERT_TRUE(rig.up());
        const uint64_t accepted = rig.connect_pair(directory.socket());
        ASSERT_NE(accepted, 0U);

        bool found = false;
        for (const auto& info : rig.backend.get_endpoints()) {
            if (info.id != accepted)
                continue;
            found = true;
            EXPECT_EQ(info.parent_id, 1U);
            EXPECT_EQ(info.local_path, directory.socket());
            EXPECT_EQ(info.role, EndpointRole::BIDIRECTIONAL);
            EXPECT_EQ(info.transport, Core::NetworkTransport::UDS);
        }
        EXPECT_TRUE(found);
    }

    TEST(UDSBackendTest, ConnectionToAMissingSocketWithoutReconnectReportsError)
    {
        SocketDirectory directory;
        UdsRig rig(uds_backend_reconnecting(false));
        ASSERT_TRUE(rig.up());

        ASSERT_EQ(rig.backend.open_endpoint(UdsRig::outbound(2, directory.socket())), 2U);

        ASSERT_TRUE(rig.pump_until([&] { return rig.backend.get_endpoint_state(2) == EndpointState::ERROR; }));
        EXPECT_TRUE(rig.saw(2, EndpointState::OPENING, EndpointState::ERROR));
    }

    TEST(UDSBackendTest, ConnectionToAMissingSocketWithReconnectRetriesUntilTheListenerAppears)
    {
        SocketDirectory directory;
        UdsRig rig(uds_backend_reconnecting(true, 20));
        ASSERT_TRUE(rig.up());

        ASSERT_EQ(rig.backend.open_endpoint(UdsRig::outbound(2, directory.socket())), 2U);
        ASSERT_TRUE(rig.pump_until([&] { return rig.backend.get_endpoint_state(2) == EndpointState::RECONNECTING; }));

        ASSERT_EQ(rig.backend.open_endpoint(UdsRig::listener(1, directory.socket())), 1U);

        ASSERT_TRUE(rig.pump_until([&] { return rig.backend.get_endpoint_state(2) == EndpointState::OPEN; }));
        EXPECT_TRUE(rig.saw(2, EndpointState::RECONNECTING, EndpointState::OPEN));
    }

    TEST(UDSBackendTest, PathLongerThanTheSocketLimitReportsErrorState)
    {
        UdsRig rig;
        ASSERT_TRUE(rig.up());
        const auto root = std::filesystem::temp_directory_path();
        const std::string too_long = (root / std::string(Platform::unix_socket_path_limit, 'x')).string();

        rig.backend.open_endpoint(UdsRig::outbound(2, too_long));

        EXPECT_EQ(rig.backend.get_endpoint_state(2), EndpointState::ERROR);
    }

    TEST(UDSBackendTest, PeerClosingMovesTheAcceptedSideToError)
    {
        SocketDirectory directory;
        UdsRig rig;
        ASSERT_TRUE(rig.up());
        const uint64_t accepted = rig.connect_pair(directory.socket());
        ASSERT_NE(accepted, 0U);

        rig.backend.close_endpoint(2);

        ASSERT_TRUE(rig.pump_until([&] { return rig.backend.get_endpoint_state(accepted) == EndpointState::ERROR; }));
        EXPECT_TRUE(rig.saw(accepted, EndpointState::OPEN, EndpointState::ERROR));
        EXPECT_EQ(rig.backend.get_endpoint_state(2), EndpointState::CLOSED);
    }

    TEST(UDSBackendTest, ClosingAConnectionRemovesIt)
    {
        SocketDirectory directory;
        UdsRig rig;
        ASSERT_TRUE(rig.up());
        ASSERT_NE(rig.connect_pair(directory.socket()), 0U);

        rig.backend.close_endpoint(2);

        EXPECT_EQ(rig.backend.get_endpoint_state(2), EndpointState::CLOSED);
        for (const auto& info : rig.backend.get_endpoints())
            EXPECT_NE(info.id, 2U);
        EXPECT_NO_THROW(rig.backend.close_endpoint(2));
    }

    // ------------------------------------------------------------------
    // Framed transfer
    // ------------------------------------------------------------------

    TEST(UDSBackendTest, SendFailsOnUnknownListenerAndUnconnectedEndpoints)
    {
        SocketDirectory directory;
        UdsRig rig;
        ASSERT_TRUE(rig.up());
        const std::vector<uint8_t> data = uds_backend_bytes_of({ 1 });
        ASSERT_EQ(rig.backend.open_endpoint(UdsRig::listener(1, directory.socket())), 1U);
        ASSERT_EQ(rig.backend.open_endpoint(UdsRig::outbound(2, directory.socket())), 2U);

        EXPECT_FALSE(rig.backend.send(99, data.data(), data.size()));
        EXPECT_FALSE(rig.backend.send(1, data.data(), data.size()));
        EXPECT_FALSE(rig.backend.send(2, data.data(), data.size()));
    }

    TEST(UDSBackendTest, FrameSentByTheClientArrivesAtTheAcceptedEndpoint)
    {
        SocketDirectory directory;
        UdsRig rig;
        ASSERT_TRUE(rig.up());
        const uint64_t accepted = rig.connect_pair(directory.socket());
        ASSERT_NE(accepted, 0U);
        const std::vector<uint8_t> data = uds_backend_bytes_of({ 0xDE, 0xAD, 0xBE, 0xEF });

        ASSERT_TRUE(rig.backend.send(2, data.data(), data.size()));

        ASSERT_TRUE(rig.pump_until([&] { return rig.count_for(accepted) == 1; }));
        EXPECT_EQ(rig.received.front().bytes, data);
        EXPECT_EQ(rig.received.front().sender, directory.socket());
    }

    TEST(UDSBackendTest, FrameSentByTheAcceptedEndpointArrivesAtTheClient)
    {
        SocketDirectory directory;
        UdsRig rig;
        ASSERT_TRUE(rig.up());
        const uint64_t accepted = rig.connect_pair(directory.socket());
        ASSERT_NE(accepted, 0U);
        const std::vector<uint8_t> data = uds_backend_bytes_of({ 7, 8, 9 });

        ASSERT_TRUE(rig.backend.send(accepted, data.data(), data.size()));

        ASSERT_TRUE(rig.pump_until([&] { return rig.count_for(2) == 1; }));
        EXPECT_EQ(rig.received.front().bytes, data);
    }

    TEST(UDSBackendTest, ConsecutiveFramesKeepTheirBoundaries)
    {
        SocketDirectory directory;
        UdsRig rig;
        ASSERT_TRUE(rig.up());
        const uint64_t accepted = rig.connect_pair(directory.socket());
        ASSERT_NE(accepted, 0U);
        const std::vector<std::vector<uint8_t>> frames {
            uds_backend_bytes_of({ 1 }), uds_backend_bytes_of({ 2, 2 }), uds_backend_bytes_of({ 3, 3, 3 }), uds_backend_bytes_of({ 4, 4, 4, 4 })
        };

        for (const auto& frame : frames)
            ASSERT_TRUE(rig.backend.send(2, frame.data(), frame.size()));

        ASSERT_TRUE(rig.pump_until([&] { return rig.count_for(accepted) == frames.size(); }));
        for (size_t i = 0; i < frames.size(); ++i)
            EXPECT_EQ(rig.received[i].bytes, frames[i]) << i;
    }

    TEST(UDSBackendTest, LargeFrameArrivesIntact)
    {
        SocketDirectory directory;
        UdsRig rig;
        ASSERT_TRUE(rig.up());
        const uint64_t accepted = rig.connect_pair(directory.socket());
        ASSERT_NE(accepted, 0U);
        std::vector<uint8_t> data(200000);
        for (size_t i = 0; i < data.size(); ++i)
            data[i] = static_cast<uint8_t>(i % 251);

        ASSERT_TRUE(rig.backend.send(2, data.data(), data.size()));

        ASSERT_TRUE(rig.pump_until([&] { return rig.count_for(accepted) == 1; }));
        EXPECT_EQ(rig.received.front().bytes, data);
    }

    TEST(UDSBackendTest, EmptyPayloadIsRefusedBeforeItReachesThePeer)
    {
        SocketDirectory directory;
        UdsRig rig;
        ASSERT_TRUE(rig.up());
        const uint64_t accepted = rig.connect_pair(directory.socket());
        ASSERT_NE(accepted, 0U);

        EXPECT_FALSE(rig.backend.send(2, nullptr, 0));

        EXPECT_EQ(rig.backend.get_endpoint_state(accepted), EndpointState::OPEN);
    }

    TEST(UDSBackendTest, FrameSplitAcrossWritesIsReassembled)
    {
        SocketDirectory directory;
        UdsRig rig;
        ASSERT_TRUE(rig.up());
        ASSERT_EQ(rig.backend.open_endpoint(UdsRig::listener(1, directory.socket())), 1U);
        UdsBackendRawPeer peer(rig, directory.socket());
        ASSERT_TRUE(rig.pump_until([&] { return rig.accepted_id() != 0; }));
        const uint64_t accepted = rig.accepted_id();

        peer.write({ 0, 0 });
        ASSERT_FALSE(rig.pump_until([&] { return rig.count_for(accepted) != 0; }, 50ms));
        peer.write({ 0, 3, 1 });
        ASSERT_FALSE(rig.pump_until([&] { return rig.count_for(accepted) != 0; }, 50ms));
        peer.write({ 2, 3 });

        ASSERT_TRUE(rig.pump_until([&] { return rig.count_for(accepted) == 1; }));
        EXPECT_EQ(rig.received.front().bytes, uds_backend_bytes_of({ 1, 2, 3 }));
    }

    TEST(UDSBackendTest, ZeroLengthFrameClosesTheConnection)
    {
        SocketDirectory directory;
        UdsRig rig;
        ASSERT_TRUE(rig.up());
        ASSERT_EQ(rig.backend.open_endpoint(UdsRig::listener(1, directory.socket())), 1U);
        UdsBackendRawPeer peer(rig, directory.socket());
        ASSERT_TRUE(rig.pump_until([&] { return rig.accepted_id() != 0; }));
        const uint64_t accepted = rig.accepted_id();

        peer.write({ 0, 0, 0, 0 });

        ASSERT_TRUE(rig.pump_until([&] { return rig.backend.get_endpoint_state(accepted) == EndpointState::ERROR; }));
        EXPECT_EQ(rig.count_for(accepted), 0U);
    }

    // ------------------------------------------------------------------
    // Line framing
    // ------------------------------------------------------------------

    TEST(UDSBackendTest, LineListenerDeliversEachLineAndAcceptsCrLf)
    {
        SocketDirectory directory;
        UdsRig rig;
        ASSERT_TRUE(rig.up());
        ASSERT_EQ(rig.backend.open_endpoint(UdsRig::listener(1, directory.socket(), Core::FramingPolicy::LINE)), 1U);
        UdsBackendRawPeer peer(rig, directory.socket());
        ASSERT_TRUE(rig.pump_until([&] { return rig.accepted_id() != 0; }));
        const uint64_t accepted = rig.accepted_id();

        peer.write({ 'h', 'i', '\n', 'y', 'o', '\r', '\n', '\n' });

        ASSERT_TRUE(rig.pump_until([&] { return rig.count_for(accepted) == 3; }));
        EXPECT_EQ(rig.received[0].bytes, uds_backend_bytes_of({ 'h', 'i' }));
        EXPECT_EQ(rig.received[1].bytes, uds_backend_bytes_of({ 'y', 'o' }));
        EXPECT_TRUE(rig.received[2].bytes.empty());
    }

    TEST(UDSBackendTest, LineSplitAcrossWritesIsReassembled)
    {
        SocketDirectory directory;
        UdsRig rig;
        ASSERT_TRUE(rig.up());
        ASSERT_EQ(rig.backend.open_endpoint(UdsRig::listener(1, directory.socket(), Core::FramingPolicy::LINE)), 1U);
        UdsBackendRawPeer peer(rig, directory.socket());
        ASSERT_TRUE(rig.pump_until([&] { return rig.accepted_id() != 0; }));
        const uint64_t accepted = rig.accepted_id();

        peer.write({ 'a', 'b' });
        ASSERT_FALSE(rig.pump_until([&] { return rig.count_for(accepted) != 0; }, 50ms));
        peer.write({ 'c', '\n' });

        ASSERT_TRUE(rig.pump_until([&] { return rig.count_for(accepted) == 1; }));
        EXPECT_EQ(rig.received.front().bytes, uds_backend_bytes_of({ 'a', 'b', 'c' }));
    }

    TEST(UDSBackendTest, AcceptedConnectionUsesItsListenersFraming)
    {
        SocketDirectory directory;
        UdsRig rig;
        ASSERT_TRUE(rig.up());
        ASSERT_EQ(rig.backend.open_endpoint(UdsRig::listener(1, directory.socket(), Core::FramingPolicy::LINE)), 1U);
        UdsBackendRawPeer peer(rig, directory.socket());
        ASSERT_TRUE(rig.pump_until([&] { return rig.accepted_id() != 0; }));
        const uint64_t accepted = rig.accepted_id();

        for (const auto& info : rig.backend.get_endpoints()) {
            if (info.id == accepted)
                EXPECT_EQ(info.framing, Core::FramingPolicy::LINE);
        }
    }

    TEST(UDSBackendTest, LineEndpointSendsATerminatedLine)
    {
        SocketDirectory directory;
        UdsRig rig;
        ASSERT_TRUE(rig.up());
        ASSERT_EQ(rig.backend.open_endpoint(UdsRig::listener(1, directory.socket(), Core::FramingPolicy::LINE)), 1U);
        UdsBackendRawPeer peer(rig, directory.socket());
        ASSERT_TRUE(rig.pump_until([&] { return rig.accepted_id() != 0; }));
        const uint64_t accepted = rig.accepted_id();
        const std::vector<uint8_t> text = uds_backend_bytes_of({ 'p', 'o', 'n', 'g' });

        ASSERT_TRUE(rig.backend.send(accepted, text.data(), text.size()));

        std::array<uint8_t, 5> received {};
        size_t got = 0;
        peer.socket.non_blocking(true);
        ASSERT_TRUE(rig.pump_until([&] {
            asio::error_code ec;
            got += peer.socket.read_some(asio::buffer(received.data() + got, received.size() - got), ec);
            return got == received.size();
        }));
        EXPECT_EQ(received, (std::array<uint8_t, 5> { 'p', 'o', 'n', 'g', '\n' }));
    }

    TEST(UDSBackendTest, LineEndpointRefusesAPayloadThatContainsANewline)
    {
        SocketDirectory directory;
        UdsRig rig;
        ASSERT_TRUE(rig.up());
        ASSERT_EQ(rig.backend.open_endpoint(UdsRig::listener(1, directory.socket(), Core::FramingPolicy::LINE)), 1U);
        ASSERT_EQ(rig.backend.open_endpoint(UdsRig::outbound(2, directory.socket(), Core::FramingPolicy::LINE)), 2U);
        ASSERT_TRUE(rig.pump_until([&] { return rig.backend.get_endpoint_state(2) == EndpointState::OPEN; }));
        const std::vector<uint8_t> text = uds_backend_bytes_of({ 'a', '\n', 'b' });

        EXPECT_FALSE(rig.backend.send(2, text.data(), text.size()));
    }

    TEST(UDSBackendTest, LineEndpointsExchangeLinesBothWays)
    {
        SocketDirectory directory;
        UdsRig rig;
        ASSERT_TRUE(rig.up());
        ASSERT_EQ(rig.backend.open_endpoint(UdsRig::listener(1, directory.socket(), Core::FramingPolicy::LINE)), 1U);
        ASSERT_EQ(rig.backend.open_endpoint(UdsRig::outbound(2, directory.socket(), Core::FramingPolicy::LINE)), 2U);
        ASSERT_TRUE(rig.pump_until([&] {
            return rig.backend.get_endpoint_state(2) == EndpointState::OPEN && rig.accepted_id() != 0;
        }));
        const uint64_t accepted = rig.accepted_id();
        const std::vector<uint8_t> ping = uds_backend_bytes_of({ 'p', 'i', 'n', 'g' });
        const std::vector<uint8_t> pong = uds_backend_bytes_of({ 'p', 'o', 'n', 'g' });

        ASSERT_TRUE(rig.backend.send(2, ping.data(), ping.size()));
        ASSERT_TRUE(rig.pump_until([&] { return rig.count_for(accepted) == 1; }));
        ASSERT_TRUE(rig.backend.send(accepted, pong.data(), pong.size()));
        ASSERT_TRUE(rig.pump_until([&] { return rig.count_for(2) == 1; }));

        EXPECT_EQ(rig.received[0].bytes, ping);
        EXPECT_EQ(rig.received[1].bytes, pong);
    }

    TEST(UDSBackendTest, AutoFramingMeansLengthPrefix)
    {
        SocketDirectory directory;
        UdsRig rig;
        ASSERT_TRUE(rig.up());
        ASSERT_EQ(rig.backend.open_endpoint(UdsRig::listener(1, directory.socket(), Core::FramingPolicy::LENGTH_PREFIX)), 1U);
        ASSERT_EQ(rig.backend.open_endpoint(UdsRig::outbound(2, directory.socket())), 2U);
        ASSERT_TRUE(rig.pump_until([&] {
            return rig.backend.get_endpoint_state(2) == EndpointState::OPEN && rig.accepted_id() != 0;
        }));
        const uint64_t accepted = rig.accepted_id();
        const std::vector<uint8_t> data = uds_backend_bytes_of({ 1, 2, 3 });

        ASSERT_TRUE(rig.backend.send(2, data.data(), data.size()));

        ASSERT_TRUE(rig.pump_until([&] { return rig.count_for(accepted) == 1; }));
        EXPECT_EQ(rig.received.front().bytes, data);
        EXPECT_EQ(EndpointInfo {}.framing, Core::FramingPolicy::AUTO);
    }

    // ------------------------------------------------------------------
    // Writes
    // ------------------------------------------------------------------

    TEST(UDSBackendTest, LargeFramesSentBackToBackArriveWholeAndInOrder)
    {
        SocketDirectory directory;
        UdsRig rig;
        ASSERT_TRUE(rig.up());
        const uint64_t accepted = rig.connect_pair(directory.socket());
        ASSERT_NE(accepted, 0U);
        const std::vector<uint8_t> first(size_t { 8 } * 1024 * 1024, 0xAA);
        const std::vector<uint8_t> second(size_t { 8 } * 1024 * 1024, 0xBB);

        ASSERT_TRUE(rig.backend.send(2, first.data(), first.size()));
        ASSERT_TRUE(rig.backend.send(2, second.data(), second.size()));

        ASSERT_TRUE(rig.pump_until([&] { return rig.count_for(accepted) == 2; }, 20s));
        EXPECT_EQ(rig.received[0].bytes, first);
        EXPECT_EQ(rig.received[1].bytes, second);
    }

    TEST(UDSBackendTest, SendsFromSeveralThreadsKeepEveryFrameWholeAndEachThreadsOrder)
    {
        SocketDirectory directory;
        UdsRig rig;
        ASSERT_TRUE(rig.up());
        const uint64_t accepted = rig.connect_pair(directory.socket());
        ASSERT_NE(accepted, 0U);
        constexpr uint8_t threads = 4;
        constexpr uint16_t per_thread = 200;
        std::atomic<bool> go { false };

        std::vector<std::thread> senders;
        for (uint8_t thread_id = 0; thread_id < threads; ++thread_id) {
            senders.emplace_back([&rig, &go, thread_id] {
                while (!go.load())
                    std::this_thread::yield();
                for (uint16_t sequence = 0; sequence < per_thread; ++sequence) {
                    const std::vector<uint8_t> frame {
                        thread_id, static_cast<uint8_t>(sequence >> 8), static_cast<uint8_t>(sequence & 0xFF), thread_id
                    };
                    EXPECT_TRUE(rig.backend.send(2, frame.data(), frame.size()));
                }
            });
        }
        go.store(true);

        const bool arrived = rig.pump_until([&] { return rig.count_for(accepted) == size_t { threads } * per_thread; }, 20s);
        for (auto& sender : senders)
            sender.join();
        ASSERT_TRUE(arrived);

        std::array<int, threads> next {};
        for (const auto& item : rig.received) {
            if (item.id != accepted)
                continue;
            ASSERT_EQ(item.bytes.size(), 4U);
            const uint8_t thread_id = item.bytes[0];
            ASSERT_LT(thread_id, threads);
            EXPECT_EQ(item.bytes[3], thread_id);
            const int sequence = (item.bytes[1] << 8) | item.bytes[2];
            EXPECT_EQ(sequence, next[thread_id]++);
        }
    }

    TEST(UDSBackendTest, SendRefusesWhenThePeerHasLeftTooMuchUnread)
    {
        SocketDirectory directory;
        UdsRig rig;
        ASSERT_TRUE(rig.up());
        constexpr size_t limit = size_t { 1 } * 1024 * 1024;
        ASSERT_EQ(rig.backend.open_endpoint(UdsRig::listener(1, directory.socket(), Core::FramingPolicy::AUTO, limit)), 1U);
        ASSERT_EQ(rig.backend.open_endpoint(UdsRig::outbound(2, directory.socket(), Core::FramingPolicy::AUTO, limit)), 2U);
        ASSERT_TRUE(rig.pump_until([&] { return rig.backend.get_endpoint_state(2) == EndpointState::OPEN; }));
        const std::vector<uint8_t> chunk(limit / 2, 0x11);

        int accepted_sends = 0;
        bool refused = false;
        for (int i = 0; i < 12 && !refused; ++i) {
            if (rig.backend.send(2, chunk.data(), chunk.size()))
                ++accepted_sends;
            else
                refused = true;
        }

        EXPECT_TRUE(refused);
        EXPECT_GE(accepted_sends, 1);
        EXPECT_LT(accepted_sends, 12);
    }

    // ------------------------------------------------------------------
    // Callbacks that call back into the backend
    // ------------------------------------------------------------------

    TEST(UDSBackendTest, HandlerMayCloseItsOwnConnection)
    {
        SocketDirectory directory;
        UdsRig rig;
        ASSERT_TRUE(rig.up());
        const uint64_t accepted = rig.connect_pair(directory.socket());
        ASSERT_NE(accepted, 0U);
        int calls = 0;
        rig.backend.set_receive_callback(
            [&rig, &calls](uint64_t id, const uint8_t*, size_t, std::string_view) {
                ++calls;
                rig.backend.close_endpoint(id);
            });
        const std::vector<uint8_t> data = uds_backend_bytes_of({ 1 });

        ASSERT_TRUE(rig.backend.send(2, data.data(), data.size()));

        ASSERT_TRUE(rig.pump_until([&] { return rig.backend.get_endpoint_state(accepted) == EndpointState::CLOSED; }));
        EXPECT_EQ(calls, 1);
    }

    TEST(UDSBackendTest, HandlerMayReplyOnItsOwnConnection)
    {
        SocketDirectory directory;
        UdsRig rig;
        ASSERT_TRUE(rig.up());
        const uint64_t accepted = rig.connect_pair(directory.socket());
        ASSERT_NE(accepted, 0U);
        std::vector<uint8_t> answer;
        rig.backend.set_receive_callback(
            [&rig, &answer, accepted](uint64_t id, const uint8_t* data, size_t size, std::string_view) {
                if (id == accepted) {
                    const std::vector<uint8_t> reply { 9, 8 };
                    EXPECT_TRUE(rig.backend.send(id, reply.data(), reply.size()));
                } else {
                    answer.assign(data, data + size);
                }
            });
        const std::vector<uint8_t> data = uds_backend_bytes_of({ 1 });

        ASSERT_TRUE(rig.backend.send(2, data.data(), data.size()));

        ASSERT_TRUE(rig.pump_until([&] { return !answer.empty(); }));
        EXPECT_EQ(answer, uds_backend_bytes_of({ 9, 8 }));
    }

    TEST(UDSBackendTest, HandlerMayOpenAnotherEndpoint)
    {
        SocketDirectory directory;
        UdsRig rig;
        ASSERT_TRUE(rig.up());
        const uint64_t accepted = rig.connect_pair(directory.socket());
        ASSERT_NE(accepted, 0U);
        const std::string second = (std::filesystem::path(directory.socket()).parent_path() / "second.sock").string();
        uint64_t opened = 0;
        rig.backend.set_receive_callback(
            [&rig, &opened, second](uint64_t, const uint8_t*, size_t, std::string_view) {
                opened = rig.backend.open_endpoint(UdsRig::listener(7, second));
            });
        const std::vector<uint8_t> data = uds_backend_bytes_of({ 1 });

        ASSERT_TRUE(rig.backend.send(2, data.data(), data.size()));

        ASSERT_TRUE(rig.pump_until([&] { return opened != 0; }));
        EXPECT_EQ(opened, 7U);
        EXPECT_EQ(rig.backend.get_endpoint_state(7), EndpointState::OPEN);
    }

    // ------------------------------------------------------------------
    // Message size limit
    // ------------------------------------------------------------------

    TEST(UDSBackendTest, LineOverTheConfiguredLimitIsDroppedAndTheNextOneArrives)
    {
        SocketDirectory directory;
        UdsRig rig;
        ASSERT_TRUE(rig.up());
        ASSERT_EQ(rig.backend.open_endpoint(UdsRig::listener(1, directory.socket(), Core::FramingPolicy::LINE, 8)), 1U);
        UdsBackendRawPeer peer(rig, directory.socket());
        ASSERT_TRUE(rig.pump_until([&] { return rig.accepted_id() != 0; }));
        const uint64_t accepted = rig.accepted_id();

        peer.write({ 't', 'o', 'o', ' ', 'l', 'o', 'n', 'g', ' ', 'b', 'y', ' ', 'f', 'a', 'r', '\n', 'o', 'k', '\n' });

        ASSERT_TRUE(rig.pump_until([&] { return rig.count_for(accepted) == 1; }));
        EXPECT_EQ(rig.received.front().bytes, uds_backend_bytes_of({ 'o', 'k' }));
        EXPECT_EQ(rig.backend.get_endpoint_state(accepted), EndpointState::OPEN);
    }

    TEST(UDSBackendTest, LengthOverTheConfiguredLimitClosesTheConnection)
    {
        SocketDirectory directory;
        UdsRig rig;
        ASSERT_TRUE(rig.up());
        ASSERT_EQ(rig.backend.open_endpoint(UdsRig::listener(1, directory.socket(), Core::FramingPolicy::LENGTH_PREFIX, 8)), 1U);
        UdsBackendRawPeer peer(rig, directory.socket());
        ASSERT_TRUE(rig.pump_until([&] { return rig.accepted_id() != 0; }));
        const uint64_t accepted = rig.accepted_id();

        peer.write({ 0, 0, 0, 9 });

        ASSERT_TRUE(rig.pump_until([&] { return rig.backend.get_endpoint_state(accepted) == EndpointState::ERROR; }));
        EXPECT_EQ(rig.count_for(accepted), 0U);
    }

    TEST(UDSBackendTest, LengthPrefixedSendOverTheConfiguredLimitIsRefused)
    {
        SocketDirectory directory;
        UdsRig rig;
        ASSERT_TRUE(rig.up());
        ASSERT_EQ(rig.backend.open_endpoint(UdsRig::listener(1, directory.socket())), 1U);
        ASSERT_EQ(rig.backend.open_endpoint(UdsRig::outbound(2, directory.socket(), Core::FramingPolicy::LENGTH_PREFIX, 4)), 2U);
        ASSERT_TRUE(rig.pump_until([&] { return rig.backend.get_endpoint_state(2) == EndpointState::OPEN; }));
        const std::vector<uint8_t> fits = uds_backend_bytes_of({ 1, 2, 3, 4 });
        const std::vector<uint8_t> too_big = uds_backend_bytes_of({ 1, 2, 3, 4, 5 });

        EXPECT_TRUE(rig.backend.send(2, fits.data(), fits.size()));
        EXPECT_FALSE(rig.backend.send(2, too_big.data(), too_big.size()));
    }

    TEST(UDSBackendTest, AcceptedConnectionInheritsTheLimit)
    {
        SocketDirectory directory;
        UdsRig rig;
        ASSERT_TRUE(rig.up());
        ASSERT_EQ(rig.backend.open_endpoint(UdsRig::listener(1, directory.socket(), Core::FramingPolicy::LINE, 8)), 1U);
        UdsBackendRawPeer peer(rig, directory.socket());
        ASSERT_TRUE(rig.pump_until([&] { return rig.accepted_id() != 0; }));
        const uint64_t accepted = rig.accepted_id();

        for (const auto& info : rig.backend.get_endpoints()) {
            if (info.id == accepted)
                EXPECT_EQ(info.max_message_size, 8U);
        }
    }

    // ------------------------------------------------------------------
    // Raw bytes
    // ------------------------------------------------------------------

    TEST(UDSBackendTest, RawListenerDeliversEveryByteWithoutBoundaries)
    {
        SocketDirectory directory;
        UdsRig rig;
        ASSERT_TRUE(rig.up());
        ASSERT_EQ(rig.backend.open_endpoint(UdsRig::listener(1, directory.socket(), Core::FramingPolicy::NONE)), 1U);
        UdsBackendRawPeer peer(rig, directory.socket());
        ASSERT_TRUE(rig.pump_until([&] { return rig.accepted_id() != 0; }));
        const uint64_t accepted = rig.accepted_id();

        peer.write({ 0, 0, 0, 0 });
        peer.write({ 9, '\n', 7 });

        ASSERT_TRUE(rig.pump_until([&] { return rig.concatenated(accepted).size() == 7; }));
        EXPECT_EQ(rig.concatenated(accepted), uds_backend_bytes_of({ 0, 0, 0, 0, 9, '\n', 7 }));
        EXPECT_EQ(rig.backend.get_endpoint_state(accepted), EndpointState::OPEN);
    }

    TEST(UDSBackendTest, RawEndpointSendsItsPayloadUnchanged)
    {
        SocketDirectory directory;
        UdsRig rig;
        ASSERT_TRUE(rig.up());
        ASSERT_EQ(rig.backend.open_endpoint(UdsRig::listener(1, directory.socket(), Core::FramingPolicy::NONE)), 1U);
        UdsBackendRawPeer peer(rig, directory.socket());
        ASSERT_TRUE(rig.pump_until([&] { return rig.accepted_id() != 0; }));
        const uint64_t accepted = rig.accepted_id();
        const std::vector<uint8_t> data = uds_backend_bytes_of({ 1, '\n', 0, 255 });

        ASSERT_TRUE(rig.backend.send(accepted, data.data(), data.size()));

        std::array<uint8_t, 4> received {};
        size_t got = 0;
        peer.socket.non_blocking(true);
        ASSERT_TRUE(rig.pump_until([&] {
            asio::error_code ec;
            got += peer.socket.read_some(asio::buffer(received.data() + got, received.size() - got), ec);
            return got == received.size();
        }));
        EXPECT_EQ(received, (std::array<uint8_t, 4> { 1, '\n', 0, 255 }));
    }

    TEST(UDSBackendTest, RawEndpointRefusesAnEmptyPayload)
    {
        SocketDirectory directory;
        UdsRig rig;
        ASSERT_TRUE(rig.up());
        ASSERT_EQ(rig.backend.open_endpoint(UdsRig::listener(1, directory.socket(), Core::FramingPolicy::NONE)), 1U);
        ASSERT_EQ(rig.backend.open_endpoint(UdsRig::outbound(2, directory.socket(), Core::FramingPolicy::NONE)), 2U);
        ASSERT_TRUE(rig.pump_until([&] { return rig.backend.get_endpoint_state(2) == EndpointState::OPEN; }));

        EXPECT_FALSE(rig.backend.send(2, nullptr, 0));
    }

    TEST(UDSBackendTest, RawEndpointsCarryTheBytesBothWays)
    {
        SocketDirectory directory;
        UdsRig rig;
        ASSERT_TRUE(rig.up());
        ASSERT_EQ(rig.backend.open_endpoint(UdsRig::listener(1, directory.socket(), Core::FramingPolicy::NONE)), 1U);
        ASSERT_EQ(rig.backend.open_endpoint(UdsRig::outbound(2, directory.socket(), Core::FramingPolicy::NONE)), 2U);
        ASSERT_TRUE(rig.pump_until([&] {
            return rig.backend.get_endpoint_state(2) == EndpointState::OPEN && rig.accepted_id() != 0;
        }));
        const uint64_t accepted = rig.accepted_id();
        const std::vector<uint8_t> request = uds_backend_bytes_of({ 4, 5, 6 });
        const std::vector<uint8_t> reply = uds_backend_bytes_of({ 7, 8 });

        ASSERT_TRUE(rig.backend.send(2, request.data(), request.size()));
        ASSERT_TRUE(rig.pump_until([&] { return rig.concatenated(accepted).size() == request.size(); }));
        ASSERT_TRUE(rig.backend.send(accepted, reply.data(), reply.size()));
        ASSERT_TRUE(rig.pump_until([&] { return rig.concatenated(2).size() == reply.size(); }));

        EXPECT_EQ(rig.concatenated(accepted), request);
        EXPECT_EQ(rig.concatenated(2), reply);
    }

    TEST(UDSBackendTest, ChunkedFramingIsRefused)
    {
        SocketDirectory directory;
        UdsRig rig;
        ASSERT_TRUE(rig.up());

        EXPECT_EQ(rig.backend.open_endpoint(UdsRig::listener(1, directory.socket(), Core::FramingPolicy::CHUNKED)), 0U);
        EXPECT_EQ(rig.backend.open_endpoint(UdsRig::outbound(2, directory.socket(), Core::FramingPolicy::CHUNKED)), 0U);
        EXPECT_TRUE(rig.backend.get_endpoints().empty());
    }

} // namespace
} // namespace MayaFlux::Test
