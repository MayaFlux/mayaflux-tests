#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include <chrono>
#include <deque>
#include <string_view>

#include "MayaFlux/Core/Backends/Network/StreamBackend.inl"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

#if !defined(MAYAFLUX_PLATFORM_WINDOWS)

#include <asio/posix/stream_descriptor.hpp>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace MayaFlux::Test {
namespace {

    using namespace std::chrono_literals;
    using Core::EndpointInfo;
    using Core::EndpointState;
    using Core::FramingPolicy;

    struct PairConfig {
        bool auto_reconnect { false };
        uint32_t reconnect_interval_ms { 20 };
    };

    /**
     * A transport with no listeners whose stream is handed over by the test, one
     * descriptor per open, so the synchronous open, the reconnect and the framing
     * default run without a serial port.
     */
    struct PairTransport {
        using Stream = asio::posix::stream_descriptor;
        using Config = PairConfig;

        static constexpr bool listens = false;
        static constexpr FramingPolicy automatic_framing = FramingPolicy::NONE;
        static constexpr Core::NetworkTransport id = Core::NetworkTransport::SERIAL;
        static constexpr std::string_view label = "Pair";
        static constexpr std::string_view name = "Pair (test)";

        static inline std::deque<int> descriptors;
        static inline int opens = 0;

        static asio::error_code open(Stream& stream, const EndpointInfo&)
        {
            ++opens;
            if (descriptors.empty()) {
                return asio::error::no_such_device;
            }
            asio::error_code ec;
            stream.assign(descriptors.front(), ec);
            if (!ec) {
                descriptors.pop_front();
            }
            return ec;
        }

        static std::string describe(const EndpointInfo& info) { return info.remote_path; }
        static std::string peer_label(const EndpointInfo& info) { return info.remote_path; }
    };

    using PairBackend = Core::StreamBackend<PairTransport>;

    struct StreamBackendSyncOpenReceived {
        uint64_t id {};
        std::vector<uint8_t> bytes;
        std::string sender;
    };

    struct StreamBackendSyncOpenStateEvent {
        uint64_t id {};
        EndpointState previous {};
        EndpointState current {};
    };

    class FarEnd {
    public:
        FarEnd()
        {
            int fds[2] {};
            EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
            m_fd = fds[1];
            ::fcntl(m_fd, F_SETFL, ::fcntl(m_fd, F_GETFL, 0) | O_NONBLOCK);
            PairTransport::descriptors.push_back(fds[0]);
        }

        FarEnd(const FarEnd&) = delete;
        FarEnd& operator=(const FarEnd&) = delete;
        FarEnd(FarEnd&&) = delete;
        FarEnd& operator=(FarEnd&&) = delete;

        ~FarEnd() { hang_up(); }

        void hang_up()
        {
            if (m_fd >= 0) {
                ::close(m_fd);
                m_fd = -1;
            }
        }

        void write(const std::vector<uint8_t>& bytes) const
        {
            ASSERT_EQ(::write(m_fd, bytes.data(), bytes.size()), static_cast<ssize_t>(bytes.size()));
        }

        std::vector<uint8_t> read_available() const
        {
            std::vector<uint8_t> chunk(4096);
            const ssize_t got = ::read(m_fd, chunk.data(), chunk.size());
            chunk.resize(got > 0 ? static_cast<size_t>(got) : 0);
            return chunk;
        }

    private:
        int m_fd { -1 };
    };

    class PairRig {
    public:
        explicit PairRig(bool reconnect = false)
            : backend(PairConfig { .auto_reconnect = reconnect, .reconnect_interval_ms = 20 }, context)
        {
            PairTransport::descriptors.clear();
            PairTransport::opens = 0;
            backend.set_receive_callback(
                [this](uint64_t id, const uint8_t* data, size_t size, std::string_view sender) {
                    received.push_back({ .id = id, .bytes = std::vector<uint8_t>(data, data + size), .sender = std::string(sender) });
                });
            backend.set_state_callback(
                [this](const EndpointInfo& info, EndpointState previous, EndpointState current) {
                    states.push_back({ .id = info.id, .previous = previous, .current = current });
                });
            backend.initialize();
            backend.start();
        }

        PairRig(const PairRig&) = delete;
        PairRig& operator=(const PairRig&) = delete;
        PairRig(PairRig&&) = delete;
        PairRig& operator=(PairRig&&) = delete;

        ~PairRig()
        {
            backend.shutdown();
            for (const int fd : PairTransport::descriptors) {
                ::close(fd);
            }
            PairTransport::descriptors.clear();
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

        [[nodiscard]] bool saw(uint64_t id, EndpointState previous, EndpointState current) const
        {
            return std::ranges::any_of(states, [&](const StreamBackendSyncOpenStateEvent& event) {
                return event.id == id && event.previous == previous && event.current == current;
            });
        }

        [[nodiscard]] size_t total_received() const
        {
            size_t sum = 0;
            for (const auto& item : received)
                sum += item.bytes.size();
            return sum;
        }

        static EndpointInfo endpoint(uint64_t id, FramingPolicy framing = FramingPolicy::AUTO)
        {
            EndpointInfo info;
            info.id = id;
            info.transport = Core::NetworkTransport::SERIAL;
            info.role = Core::EndpointRole::BIDIRECTIONAL;
            info.remote_path = "pair";
            info.framing = framing;
            return info;
        }

        asio::io_context context;
        PairBackend backend;
        std::vector<StreamBackendSyncOpenReceived> received;
        std::vector<StreamBackendSyncOpenStateEvent> states;
    };

    // ------------------------------------------------------------------
    // Synchronous open
    // ------------------------------------------------------------------

    TEST(StreamBackendSyncOpenTest, OpensIntoOpenBeforeOpenEndpointReturns)
    {
        PairRig rig;
        FarEnd far;

        const uint64_t id = rig.backend.open_endpoint(PairRig::endpoint(1));

        EXPECT_EQ(id, 1U);
        EXPECT_EQ(rig.backend.get_endpoint_state(1), EndpointState::OPEN);
        EXPECT_TRUE(rig.saw(1, EndpointState::OPENING, EndpointState::OPEN));
        EXPECT_EQ(PairTransport::opens, 1);
        EXPECT_EQ(rig.backend.get_endpoints().size(), 1U);
    }

    TEST(StreamBackendSyncOpenTest, FirstOpenFailureWithoutReconnectReturnsZeroAndKeepsNothing)
    {
        PairRig rig(false);

        const uint64_t id = rig.backend.open_endpoint(PairRig::endpoint(1));

        EXPECT_EQ(id, 0U);
        EXPECT_EQ(rig.backend.get_endpoint_state(1), EndpointState::CLOSED);
        EXPECT_TRUE(rig.backend.get_endpoints().empty());
        EXPECT_TRUE(rig.states.empty());
    }

    TEST(StreamBackendSyncOpenTest, FirstOpenFailureWithReconnectWaitsThenOpensWhenTheDeviceAppears)
    {
        PairRig rig(true);

        ASSERT_EQ(rig.backend.open_endpoint(PairRig::endpoint(1)), 1U);
        EXPECT_EQ(rig.backend.get_endpoint_state(1), EndpointState::RECONNECTING);
        EXPECT_TRUE(rig.saw(1, EndpointState::OPENING, EndpointState::RECONNECTING));

        FarEnd far;

        ASSERT_TRUE(rig.pump_until([&] { return rig.backend.get_endpoint_state(1) == EndpointState::OPEN; }));
        EXPECT_TRUE(rig.saw(1, EndpointState::RECONNECTING, EndpointState::OPEN));
        far.write({ 7 });
        ASSERT_TRUE(rig.pump_until([&] { return rig.total_received() == 1; }));
    }

    TEST(StreamBackendSyncOpenTest, PeerHangUpReopensTheEndpointWhenReconnectIsOn)
    {
        PairRig rig(true);
        auto first = std::make_unique<FarEnd>();
        ASSERT_EQ(rig.backend.open_endpoint(PairRig::endpoint(1)), 1U);
        ASSERT_EQ(rig.backend.get_endpoint_state(1), EndpointState::OPEN);

        first->hang_up();

        ASSERT_TRUE(rig.pump_until([&] { return rig.backend.get_endpoint_state(1) == EndpointState::RECONNECTING; }));
        FarEnd second;
        ASSERT_TRUE(rig.pump_until([&] { return rig.backend.get_endpoint_state(1) == EndpointState::OPEN; }));
        second.write({ 1, 2 });
        ASSERT_TRUE(rig.pump_until([&] { return rig.total_received() == 2; }));
        EXPECT_GE(PairTransport::opens, 2);
    }

    TEST(StreamBackendSyncOpenTest, PeerHangUpWithoutReconnectReportsError)
    {
        PairRig rig(false);
        FarEnd far;
        ASSERT_EQ(rig.backend.open_endpoint(PairRig::endpoint(1)), 1U);

        far.hang_up();

        ASSERT_TRUE(rig.pump_until([&] { return rig.backend.get_endpoint_state(1) == EndpointState::ERROR; }));
        EXPECT_TRUE(rig.saw(1, EndpointState::OPEN, EndpointState::ERROR));
        EXPECT_EQ(PairTransport::opens, 1);
    }

    TEST(StreamBackendSyncOpenTest, SendIsRefusedWhileTheEndpointWaitsToReconnect)
    {
        PairRig rig(true);
        ASSERT_EQ(rig.backend.open_endpoint(PairRig::endpoint(1)), 1U);
        const std::vector<uint8_t> data { 1 };

        EXPECT_FALSE(rig.backend.send(1, data.data(), data.size()));
        EXPECT_EQ(rig.backend.get_write_backlog(1), 0U);
    }

    TEST(StreamBackendSyncOpenTest, ClosingAWaitingEndpointStopsTheRetries)
    {
        PairRig rig(true);
        ASSERT_EQ(rig.backend.open_endpoint(PairRig::endpoint(1)), 1U);
        ASSERT_EQ(PairTransport::opens, 1);

        rig.backend.close_endpoint(1);
        FarEnd far;
        rig.pump_until([] { return false; }, 120ms);

        EXPECT_EQ(PairTransport::opens, 1);
        EXPECT_EQ(rig.backend.get_endpoint_state(1), EndpointState::CLOSED);
        EXPECT_TRUE(rig.backend.get_endpoints().empty());
    }

    // ------------------------------------------------------------------
    // Framing default
    // ------------------------------------------------------------------

    TEST(StreamBackendSyncOpenTest, AutomaticFramingIsTheTransportsOwnAndStillReportedAsAuto)
    {
        PairRig rig;
        FarEnd far;
        ASSERT_EQ(rig.backend.open_endpoint(PairRig::endpoint(1)), 1U);

        far.write({ 1, 2, 3 });
        ASSERT_TRUE(rig.pump_until([&] { return rig.total_received() == 3; }));

        EXPECT_EQ(rig.received.front().bytes, (std::vector<uint8_t> { 1, 2, 3 }));
        EXPECT_EQ(rig.received.front().sender, "pair");
        EXPECT_EQ(rig.backend.get_endpoints().front().framing, FramingPolicy::AUTO);
    }

    TEST(StreamBackendSyncOpenTest, AutomaticFramingSendsThePayloadAsItIs)
    {
        PairRig rig;
        FarEnd far;
        ASSERT_EQ(rig.backend.open_endpoint(PairRig::endpoint(1)), 1U);
        const std::vector<uint8_t> data { 4, 5 };

        ASSERT_TRUE(rig.backend.send(1, data.data(), data.size()));

        std::vector<uint8_t> arrived;
        ASSERT_TRUE(rig.pump_until([&] {
            const auto chunk = far.read_available();
            arrived.insert(arrived.end(), chunk.begin(), chunk.end());
            return arrived.size() >= data.size();
        }));
        EXPECT_EQ(arrived, data);
    }

    TEST(StreamBackendSyncOpenTest, ExplicitFramingOverridesTheAutomaticOne)
    {
        PairRig rig;
        FarEnd far;
        ASSERT_EQ(rig.backend.open_endpoint(PairRig::endpoint(1, FramingPolicy::LENGTH_PREFIX)), 1U);

        far.write({ 0, 0, 0, 2, 9, 8 });

        ASSERT_TRUE(rig.pump_until([&] { return rig.received.size() == 1; }));
        EXPECT_EQ(rig.received.front().bytes, (std::vector<uint8_t> { 9, 8 }));
    }

    // ------------------------------------------------------------------
    // Read size and backlog
    // ------------------------------------------------------------------

    TEST(StreamBackendSyncOpenTest, ReadSizeBoundsEachDelivery)
    {
        PairRig rig;
        FarEnd far;
        EndpointInfo info = PairRig::endpoint(1);
        info.read_size = 4;
        ASSERT_EQ(rig.backend.open_endpoint(info), 1U);

        far.write({ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 });

        ASSERT_TRUE(rig.pump_until([&] { return rig.total_received() == 10; }));
        std::vector<uint8_t> joined;
        for (const auto& item : rig.received) {
            EXPECT_LE(item.bytes.size(), 4U);
            joined.insert(joined.end(), item.bytes.begin(), item.bytes.end());
        }
        EXPECT_GE(rig.received.size(), 3U);
        EXPECT_EQ(joined, (std::vector<uint8_t> { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 }));
    }

    TEST(StreamBackendSyncOpenTest, WriteBacklogCountsQueuedBytesUntilTheyAreWritten)
    {
        PairRig rig;
        FarEnd far;
        ASSERT_EQ(rig.backend.open_endpoint(PairRig::endpoint(1)), 1U);
        const std::vector<uint8_t> data(1000, 0x5A);

        ASSERT_TRUE(rig.backend.send(1, data.data(), data.size()));
        ASSERT_TRUE(rig.backend.send(1, data.data(), data.size()));
        ASSERT_TRUE(rig.backend.send(1, data.data(), data.size()));

        EXPECT_EQ(rig.backend.get_write_backlog(1), 3000U);
        ASSERT_TRUE(rig.pump_until([&] { return rig.backend.get_write_backlog(1) == 0; }));
        size_t arrived = 0;
        ASSERT_TRUE(rig.pump_until([&] {
            arrived += far.read_available().size();
            return arrived == 3000;
        }));
    }

    // ------------------------------------------------------------------
    // No listeners
    // ------------------------------------------------------------------

    TEST(StreamBackendSyncOpenTest, AnEndpointShapedLikeAListenerIsStillAConnection)
    {
        PairRig rig;
        FarEnd far;
        EndpointInfo info = PairRig::endpoint(1);
        info.remote_path.clear();
        info.local_port = 9000;

        EXPECT_EQ(rig.backend.open_endpoint(info), 1U);
        EXPECT_EQ(rig.backend.get_endpoint_state(1), EndpointState::OPEN);
        EXPECT_EQ(PairTransport::opens, 1);
    }

    TEST(StreamBackendSyncOpenTest, ChunkedFramingIsRefusedBeforeAnyOpenIsAttempted)
    {
        PairRig rig;
        FarEnd far;

        EXPECT_EQ(rig.backend.open_endpoint(PairRig::endpoint(1, FramingPolicy::CHUNKED)), 0U);
        EXPECT_EQ(PairTransport::opens, 0);
    }

    TEST(StreamBackendSyncOpenTest, ShutdownClosesTheEndpoint)
    {
        PairRig rig;
        FarEnd far;
        ASSERT_EQ(rig.backend.open_endpoint(PairRig::endpoint(1)), 1U);

        rig.backend.shutdown();

        EXPECT_TRUE(rig.backend.get_endpoints().empty());
        EXPECT_EQ(rig.backend.get_endpoint_state(1), EndpointState::CLOSED);
    }

} // namespace
} // namespace MayaFlux::Test

#endif
