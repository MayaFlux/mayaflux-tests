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

    constexpr uint64_t k_first_accepted_id = 100;

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

    struct Received {
        uint64_t id {};
        std::vector<uint8_t> bytes;
        std::string sender;
    };

    struct StateEvent {
        uint64_t id {};
        EndpointState previous {};
        EndpointState current {};
    };

    UDSBackendInfo reconnecting(bool enabled, uint32_t interval_ms = 20)
    {
        UDSBackendInfo info;
        info.auto_reconnect = enabled;
        info.reconnect_interval_ms = interval_ms;
        return info;
    }

    class UdsRig {
    public:
        explicit UdsRig(const UDSBackendInfo& config = reconnecting(false))
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

        [[nodiscard]] bool saw(uint64_t id, EndpointState previous, EndpointState current) const
        {
            return std::ranges::any_of(states, [&](const StateEvent& event) {
                return event.id == id && event.previous == previous && event.current == current;
            });
        }

        [[nodiscard]] uint64_t accepted_id() const
        {
            for (const auto& event : states) {
                if (event.id >= k_first_accepted_id && event.previous == EndpointState::CLOSED
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

        static EndpointInfo listener(uint64_t id, const std::string& path)
        {
            EndpointInfo info;
            info.id = id;
            info.transport = Core::NetworkTransport::UDS;
            info.role = EndpointRole::BIDIRECTIONAL;
            info.local_path = path;
            return info;
        }

        static EndpointInfo outbound(uint64_t id, const std::string& path)
        {
            EndpointInfo info;
            info.id = id;
            info.transport = Core::NetworkTransport::UDS;
            info.role = EndpointRole::BIDIRECTIONAL;
            info.remote_path = path;
            return info;
        }

        asio::io_context context;
        UDSBackend backend;
        uint64_t next_accepted_id { k_first_accepted_id };
        std::vector<Received> received;
        std::vector<StateEvent> states;
    };

    struct RawPeer {
        RawPeer(UdsRig& rig, const std::string& path)
            : socket(rig.context)
        {
            socket.connect(Stream::endpoint(path));
        }

        void write(std::initializer_list<uint8_t> bytes) { asio::write(socket, asio::buffer(std::vector<uint8_t>(bytes))); }

        Stream::socket socket;
    };

    std::vector<uint8_t> bytes_of(std::initializer_list<uint8_t> values)
    {
        return values;
    }

    bool is_socket_file(const std::string& path)
    {
        std::error_code ec;
        return std::filesystem::symlink_status(path, ec).type() == std::filesystem::file_type::socket;
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
            Stream::acceptor stale(context, Stream::endpoint(directory.socket()));
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

        ASSERT_GE(accepted, k_first_accepted_id);
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
        UdsRig rig(reconnecting(false));
        ASSERT_TRUE(rig.up());

        ASSERT_EQ(rig.backend.open_endpoint(UdsRig::outbound(2, directory.socket())), 2U);

        ASSERT_TRUE(rig.pump_until([&] { return rig.backend.get_endpoint_state(2) == EndpointState::ERROR; }));
        EXPECT_TRUE(rig.saw(2, EndpointState::OPENING, EndpointState::ERROR));
    }

    TEST(UDSBackendTest, ConnectionToAMissingSocketWithReconnectRetriesUntilTheListenerAppears)
    {
        SocketDirectory directory;
        UdsRig rig(reconnecting(true, 20));
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
        const std::vector<uint8_t> data = bytes_of({ 1 });
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
        const std::vector<uint8_t> data = bytes_of({ 0xDE, 0xAD, 0xBE, 0xEF });

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
        const std::vector<uint8_t> data = bytes_of({ 7, 8, 9 });

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
            bytes_of({ 1 }), bytes_of({ 2, 2 }), bytes_of({ 3, 3, 3 }), bytes_of({ 4, 4, 4, 4 })
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
        RawPeer peer(rig, directory.socket());
        ASSERT_TRUE(rig.pump_until([&] { return rig.accepted_id() != 0; }));
        const uint64_t accepted = rig.accepted_id();

        peer.write({ 0, 0 });
        ASSERT_FALSE(rig.pump_until([&] { return rig.count_for(accepted) != 0; }, 50ms));
        peer.write({ 0, 3, 1 });
        ASSERT_FALSE(rig.pump_until([&] { return rig.count_for(accepted) != 0; }, 50ms));
        peer.write({ 2, 3 });

        ASSERT_TRUE(rig.pump_until([&] { return rig.count_for(accepted) == 1; }));
        EXPECT_EQ(rig.received.front().bytes, bytes_of({ 1, 2, 3 }));
    }

    TEST(UDSBackendTest, ZeroLengthFrameClosesTheConnection)
    {
        SocketDirectory directory;
        UdsRig rig;
        ASSERT_TRUE(rig.up());
        ASSERT_EQ(rig.backend.open_endpoint(UdsRig::listener(1, directory.socket())), 1U);
        RawPeer peer(rig, directory.socket());
        ASSERT_TRUE(rig.pump_until([&] { return rig.accepted_id() != 0; }));
        const uint64_t accepted = rig.accepted_id();

        peer.write({ 0, 0, 0, 0 });

        ASSERT_TRUE(rig.pump_until([&] { return rig.backend.get_endpoint_state(accepted) == EndpointState::ERROR; }));
        EXPECT_EQ(rig.count_for(accepted), 0U);
    }

} // namespace
} // namespace MayaFlux::Test
