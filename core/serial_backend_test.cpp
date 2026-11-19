#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include <chrono>
#include <string_view>

#include "MayaFlux/Core/Backends/Network/SerialBackend.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

#if !defined(MAYAFLUX_PLATFORM_WINDOWS)

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

namespace MayaFlux::Test {
namespace {

    using namespace std::chrono_literals;
    using Core::EndpointInfo;
    using Core::EndpointState;
    using Core::SerialBackend;
    using Core::SerialLinkInfo;

    class Pty {
    public:
        Pty()
        {
            m_master = ::posix_openpt(O_RDWR | O_NOCTTY);
            if (m_master < 0) {
                return;
            }
            const char* name = nullptr;
            if (::grantpt(m_master) == 0 && ::unlockpt(m_master) == 0) {
                name = ::ptsname(m_master);
            }
            if (!name) {
                ::close(m_master);
                m_master = -1;
                return;
            }
            m_slave = name;
            ::fcntl(m_master, F_SETFL, ::fcntl(m_master, F_GETFL, 0) | O_NONBLOCK);
        }

        Pty(const Pty&) = delete;
        Pty& operator=(const Pty&) = delete;
        Pty(Pty&&) = delete;
        Pty& operator=(Pty&&) = delete;

        ~Pty() { hang_up(); }

        [[nodiscard]] bool ok() const { return m_master >= 0; }
        [[nodiscard]] const std::string& slave_path() const { return m_slave; }
        [[nodiscard]] int master() const { return m_master; }

        void hang_up()
        {
            if (m_master >= 0) {
                ::close(m_master);
                m_master = -1;
            }
        }

        void write(const std::vector<uint8_t>& bytes) const
        {
            ASSERT_EQ(::write(m_master, bytes.data(), bytes.size()), static_cast<ssize_t>(bytes.size()));
        }

        [[nodiscard]] std::vector<uint8_t> read_available() const
        {
            std::vector<uint8_t> chunk(4096);
            const ssize_t got = ::read(m_master, chunk.data(), chunk.size());
            chunk.resize(got > 0 ? static_cast<size_t>(got) : 0);
            return chunk;
        }

    private:
        int m_master { -1 };
        std::string m_slave;
    };

    struct Received {
        uint64_t id {};
        std::vector<uint8_t> bytes;
        std::string sender;
    };

    class SerialRig {
    public:
        explicit SerialRig(bool reconnect = false)
            : backend(SerialLinkInfo { .enabled = true, .auto_reconnect = reconnect, .reconnect_interval_ms = 20 }, context)
        {
            backend.set_receive_callback(
                [this](uint64_t id, const uint8_t* data, size_t size, std::string_view sender) {
                    received.push_back({ .id = id, .bytes = std::vector<uint8_t>(data, data + size), .sender = std::string(sender) });
                });
            backend.initialize();
            backend.start();
        }

        SerialRig(const SerialRig&) = delete;
        SerialRig& operator=(const SerialRig&) = delete;
        SerialRig(SerialRig&&) = delete;
        SerialRig& operator=(SerialRig&&) = delete;

        ~SerialRig() { backend.shutdown(); }

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

        [[nodiscard]] size_t total_received() const
        {
            size_t sum = 0;
            for (const auto& item : received)
                sum += item.bytes.size();
            return sum;
        }

        static EndpointInfo port(uint64_t id, const std::string& path)
        {
            EndpointInfo info;
            info.id = id;
            info.transport = Core::NetworkTransport::SERIAL;
            info.role = Core::EndpointRole::BIDIRECTIONAL;
            info.remote_path = path;
            return info;
        }

        asio::io_context context;
        SerialBackend backend;
        std::vector<Received> received;
    };

    termios settings_of(const Pty& pty)
    {
        termios settings {};
        EXPECT_EQ(::tcgetattr(pty.master(), &settings), 0);
        return settings;
    }

    // ------------------------------------------------------------------
    // Identity and refusals
    // ------------------------------------------------------------------

    TEST(SerialBackendTest, ReportsItsIdentity)
    {
        SerialRig rig;

        EXPECT_EQ(rig.backend.get_transport(), Core::NetworkTransport::SERIAL);
        EXPECT_FALSE(rig.backend.get_name().empty());
        EXPECT_TRUE(rig.backend.is_running());
    }

    TEST(SerialBackendTest, ADeviceThatIsNotThereIsRefusedWithoutReconnect)
    {
        SerialRig rig(false);

        EXPECT_EQ(rig.backend.open_endpoint(SerialRig::port(1, "/dev/mayaflux-no-such-port")), 0U);
        EXPECT_TRUE(rig.backend.get_endpoints().empty());
    }

    TEST(SerialBackendTest, ADeviceThatIsNotThereWaitsWhenReconnectIsOn)
    {
        SerialRig rig(true);

        EXPECT_EQ(rig.backend.open_endpoint(SerialRig::port(1, "/dev/mayaflux-no-such-port")), 1U);
        EXPECT_EQ(rig.backend.get_endpoint_state(1), EndpointState::RECONNECTING);
    }

    TEST(SerialBackendTest, AnEmptyPathAndUnusableDataBitsAreRefused)
    {
        Pty pty;
        if (!pty.ok())
            GTEST_SKIP() << "no pseudo-terminal";
        SerialRig rig;

        EXPECT_EQ(rig.backend.open_endpoint(SerialRig::port(1, "")), 0U);

        EndpointInfo few = SerialRig::port(2, pty.slave_path());
        few.serial.data_bits = 4;
        EXPECT_EQ(rig.backend.open_endpoint(few), 0U);

        EndpointInfo many = SerialRig::port(3, pty.slave_path());
        many.serial.data_bits = 9;
        EXPECT_EQ(rig.backend.open_endpoint(many), 0U);
    }

    TEST(SerialBackendTest, LineControlOfAnUnknownEndpointIsRefused)
    {
        SerialRig rig;

        EXPECT_FALSE(rig.backend.set_control_lines(99, true, true));
        EXPECT_FALSE(rig.backend.send_break(99));
    }

    // ------------------------------------------------------------------
    // Data over a pseudo-terminal
    // ------------------------------------------------------------------

    TEST(SerialBackendTest, BytesMoveBothWaysRawOverAPty)
    {
        Pty pty;
        if (!pty.ok())
            GTEST_SKIP() << "no pseudo-terminal";
        SerialRig rig;

        ASSERT_EQ(rig.backend.open_endpoint(SerialRig::port(1, pty.slave_path())), 1U);
        EXPECT_EQ(rig.backend.get_endpoint_state(1), EndpointState::OPEN);

        pty.write({ 1, 2, 3 });
        ASSERT_TRUE(rig.pump_until([&] { return rig.total_received() == 3; }));
        EXPECT_EQ(rig.received.front().bytes, (std::vector<uint8_t> { 1, 2, 3 }));
        EXPECT_EQ(rig.received.front().sender, pty.slave_path());

        const std::vector<uint8_t> out { 4, 5 };
        ASSERT_TRUE(rig.backend.send(1, out.data(), out.size()));
        std::vector<uint8_t> arrived;
        ASSERT_TRUE(rig.pump_until([&] {
            const auto chunk = pty.read_available();
            arrived.insert(arrived.end(), chunk.begin(), chunk.end());
            return arrived.size() >= out.size();
        }));
        EXPECT_EQ(arrived, out);
    }

    TEST(SerialBackendTest, BaudRatesOutsideTheStandardTableOpenAndCarryData)
    {
        for (const uint32_t baud : { 31250U, 250000U }) {
            Pty pty;
            if (!pty.ok())
                GTEST_SKIP() << "no pseudo-terminal";
            SerialRig rig;
            EndpointInfo info = SerialRig::port(1, pty.slave_path());
            info.serial.baud_rate = baud;

            ASSERT_EQ(rig.backend.open_endpoint(info), 1U) << baud;
            EXPECT_EQ(rig.backend.get_endpoint_state(1), EndpointState::OPEN) << baud;

            pty.write({ 7 });
            EXPECT_TRUE(rig.pump_until([&] { return rig.total_received() == 1; })) << baud;
        }
    }

    TEST(SerialBackendTest, ReadSizeBoundsEachDelivery)
    {
        Pty pty;
        if (!pty.ok())
            GTEST_SKIP() << "no pseudo-terminal";
        SerialRig rig;
        EndpointInfo info = SerialRig::port(1, pty.slave_path());
        info.read_size = 2;
        ASSERT_EQ(rig.backend.open_endpoint(info), 1U);

        pty.write({ 1, 2, 3, 4, 5, 6 });

        ASSERT_TRUE(rig.pump_until([&] { return rig.total_received() == 6; }));
        for (const auto& item : rig.received)
            EXPECT_LE(item.bytes.size(), 2U);
    }

    TEST(SerialBackendTest, ThePtyHangingUpMovesTheEndpointToError)
    {
        Pty pty;
        if (!pty.ok())
            GTEST_SKIP() << "no pseudo-terminal";
        SerialRig rig(false);
        ASSERT_EQ(rig.backend.open_endpoint(SerialRig::port(1, pty.slave_path())), 1U);

        pty.hang_up();

        ASSERT_TRUE(rig.pump_until([&] { return rig.backend.get_endpoint_state(1) == EndpointState::ERROR; }));
    }

    // ------------------------------------------------------------------
    // Line settings
    // ------------------------------------------------------------------

    TEST(SerialBackendTest, LineSettingsReachTheTerminalWithHardwareFlowControl)
    {
        Pty pty;
        if (!pty.ok())
            GTEST_SKIP() << "no pseudo-terminal";
        SerialRig rig;
        EndpointInfo info = SerialRig::port(1, pty.slave_path());
        info.serial.baud_rate = 9600;
        info.serial.data_bits = 7;
        info.serial.parity = Core::SerialParity::EVEN;
        info.serial.stop_bits = Core::SerialStopBits::TWO;
        info.serial.flow_control = Core::SerialFlowControl::HARDWARE;

        ASSERT_EQ(rig.backend.open_endpoint(info), 1U);

        const termios settings = settings_of(pty);
        EXPECT_EQ(::cfgetospeed(&settings), static_cast<speed_t>(B9600));
        EXPECT_EQ(settings.c_cflag & PARODD, 0U);
        EXPECT_NE(settings.c_cflag & CSTOPB, 0U);
        EXPECT_NE(settings.c_cflag & CRTSCTS, 0U);
    }

    TEST(SerialBackendTest, LineSettingsReachTheTerminalWithSoftwareFlowControl)
    {
        Pty pty;
        if (!pty.ok())
            GTEST_SKIP() << "no pseudo-terminal";
        SerialRig rig;
        EndpointInfo info = SerialRig::port(1, pty.slave_path());
        info.serial.baud_rate = 19200;
        info.serial.parity = Core::SerialParity::ODD;
        info.serial.flow_control = Core::SerialFlowControl::SOFTWARE;

        ASSERT_EQ(rig.backend.open_endpoint(info), 1U);

        const termios settings = settings_of(pty);
        EXPECT_EQ(::cfgetospeed(&settings), static_cast<speed_t>(B19200));
        EXPECT_NE(settings.c_cflag & PARODD, 0U);
        EXPECT_EQ(settings.c_cflag & CSTOPB, 0U);
        EXPECT_NE(settings.c_iflag & IXON, 0U);
        EXPECT_NE(settings.c_iflag & IXOFF, 0U);
    }

    // ------------------------------------------------------------------
    // Settle time
    // ------------------------------------------------------------------

    TEST(SerialBackendTest, SettleTimeHoldsTheEndpointOpeningAndDiscardsEarlyInput)
    {
        Pty pty;
        if (!pty.ok())
            GTEST_SKIP() << "no pseudo-terminal";
        SerialRig rig;
        EndpointInfo info = SerialRig::port(1, pty.slave_path());
        info.serial.settle_ms = 150;

        ASSERT_EQ(rig.backend.open_endpoint(info), 1U);
        EXPECT_EQ(rig.backend.get_endpoint_state(1), EndpointState::OPENING);
        pty.write({ 9, 9, 9 });

        const std::vector<uint8_t> data { 1 };
        EXPECT_FALSE(rig.backend.send(1, data.data(), data.size()));
        EXPECT_FALSE(rig.backend.set_control_lines(1, true, std::nullopt));
        EXPECT_FALSE(rig.backend.send_break(1));

        ASSERT_TRUE(rig.pump_until([&] { return rig.backend.get_endpoint_state(1) == EndpointState::OPEN; }));
        rig.pump_until([] { return false; }, 50ms);
        EXPECT_EQ(rig.total_received(), 0U);

        pty.write({ 5 });
        ASSERT_TRUE(rig.pump_until([&] { return rig.total_received() == 1; }));
        EXPECT_EQ(rig.received.front().bytes, (std::vector<uint8_t> { 5 }));
    }

    TEST(SerialBackendTest, ClosingDuringTheSettleTimeNeverOpensTheEndpoint)
    {
        Pty pty;
        if (!pty.ok())
            GTEST_SKIP() << "no pseudo-terminal";
        SerialRig rig;
        EndpointInfo info = SerialRig::port(1, pty.slave_path());
        info.serial.settle_ms = 80;
        ASSERT_EQ(rig.backend.open_endpoint(info), 1U);

        rig.backend.close_endpoint(1);
        rig.pump_until([] { return false; }, 200ms);

        EXPECT_EQ(rig.backend.get_endpoint_state(1), EndpointState::CLOSED);
        EXPECT_TRUE(rig.backend.get_endpoints().empty());
    }

} // namespace
} // namespace MayaFlux::Test

#endif
