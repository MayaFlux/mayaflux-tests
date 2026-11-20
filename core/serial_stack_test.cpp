#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include <future>
#include <random>

#include "../support/serial_test_support.hpp"
#include "MayaFlux/Core/Backends/Input/SerialInputBackend.hpp"
#include "MayaFlux/Core/ProcessingArchitecture.hpp"
#include "MayaFlux/Core/Subsystems/NetworkSubsystem.hpp"
#include "MayaFlux/Registry/BackendRegistry.hpp"
#include "MayaFlux/Registry/Service/NetworkService.hpp"
#include "MayaFlux/Vruta/Scheduler.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::integration, ::MayaFlux::Test::Meta::requirements<::MayaFlux::Test::Meta::Capability::writable_temp, ::MayaFlux::Test::Meta::Capability::pseudo_terminal>() }))

#if !defined(MAYAFLUX_PLATFORM_WINDOWS)

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
    using Numbers = std::vector<double>;

    template <typename Predicate>
    bool eventually(Predicate predicate, std::chrono::milliseconds timeout = 5s)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (predicate())
                return true;
            std::this_thread::sleep_for(1ms);
        }
        return predicate();
    }

    struct Inbox {
        void push(const InputValue& value)
        {
            std::scoped_lock lock(mutex);
            items.push_back(value);
        }

        [[nodiscard]] size_t size() const
        {
            std::scoped_lock lock(mutex);
            return items.size();
        }

        [[nodiscard]] std::vector<InputValue> all() const
        {
            std::scoped_lock lock(mutex);
            return items;
        }

        [[nodiscard]] std::vector<Numbers> vectors() const
        {
            std::vector<Numbers> out;
            for (const auto& value : all()) {
                if (value.type == InputValue::Type::VECTOR) {
                    out.push_back(value.as_vector());
                }
            }
            return out;
        }

        [[nodiscard]] std::vector<std::string> texts() const
        {
            std::vector<std::string> out;
            for (const auto& value : all()) {
                const auto& bytes = value.as_bytes();
                out.emplace_back(bytes.begin(), bytes.end());
            }
            return out;
        }

        mutable std::mutex mutex;
        std::vector<InputValue> items;
    };

    class StackRig {
    public:
        explicit StackRig(uint32_t reconnect_ms = 20)
            : scheduler(std::make_shared<Vruta::TaskScheduler>(48000))
            , subsystem(make_config(reconnect_ms))
            , handle(nullptr, nullptr, scheduler, nullptr, nullptr, subsystem.get_tokens())
        {
            subsystem.initialize(handle);
            subsystem.start();
        }

        StackRig(const StackRig&) = delete;
        StackRig& operator=(const StackRig&) = delete;
        StackRig(StackRig&&) = delete;
        StackRig& operator=(StackRig&&) = delete;

        ~StackRig()
        {
            backend.reset();
            subsystem.shutdown();
        }

        static Core::GlobalNetworkConfig make_config(uint32_t reconnect_ms)
        {
            auto config = Core::GlobalNetworkConfig::with_serial();
            config.serial.reconnect_interval_ms = reconnect_ms;
            return config;
        }

        bool open_ports(const std::vector<std::pair<std::string, SerialSpec>>& ports)
        {
            auto* service = Registry::BackendRegistry::instance().get_service<Registry::Service::NetworkService>();
            if (!service) {
                return false;
            }

            SerialInputBackend::Config config;
            config.network = service;
            for (const auto& [path, spec] : ports) {
                Core::SerialPortConfig port;
                port.port_name = path;
                port.line = spec.line;
                config.ports.push_back(port);
            }

            backend = std::make_unique<SerialInputBackend>(std::move(config));
            backend->set_input_callback([this](const InputValue& value) { inbox.push(value); });
            backend->set_device_callback([this](const InputDeviceInfo& info, bool connected) {
                std::scoped_lock lock(events_mutex);
                events.emplace_back(info.id, connected);
            });
            if (!backend->initialize()) {
                return false;
            }

            for (const auto& [path, spec] : ports) {
                for (const auto& device : backend->get_devices()) {
                    if (device.port_name == path) {
                        ids.push_back(device.id);
                        backend->set_device_spec(device.id, spec);
                    }
                }
            }
            backend->start();
            for (const uint32_t id : ids) {
                if (!backend->open_device(id)) {
                    return false;
                }
            }
            return true;
        }

        bool open_port(const std::string& path, const SerialSpec& spec)
        {
            return open_ports({ { path, spec } });
        }

        [[nodiscard]] std::shared_ptr<Core::SerialDevice> device(size_t index = 0) const
        {
            return backend->get_device(ids.at(index));
        }

        [[nodiscard]] bool in_state(EndpointState wanted, size_t index = 0) const
        {
            const auto found = device(index);
            return found && found->state() == wanted;
        }

        [[nodiscard]] std::vector<std::pair<uint32_t, bool>> device_events() const
        {
            std::scoped_lock lock(events_mutex);
            return events;
        }

        [[nodiscard]] size_t count_events(bool connected) const
        {
            const auto all = device_events();
            return static_cast<size_t>(std::ranges::count_if(all, [&](const auto& e) { return e.second == connected; }));
        }

        std::shared_ptr<Vruta::TaskScheduler> scheduler;
        Core::NetworkSubsystem subsystem;
        Core::SubsystemProcessingHandle handle;
        Inbox inbox;
        std::vector<uint32_t> ids;
        mutable std::mutex events_mutex;
        std::vector<std::pair<uint32_t, bool>> events;
        std::unique_ptr<SerialInputBackend> backend;
    };

    std::shared_ptr<std::string> make_line_buffer()
    {
        return std::make_shared<std::string>();
    }

    VirtualSerialDevice::Responder line_responder(
        const std::function<void(VirtualSerialDevice&, const std::string&)>& on_line)
    {
        const auto buffer = make_line_buffer();
        return [buffer, on_line](VirtualSerialDevice& device, const Bytes& chunk) {
            buffer->append(chunk.begin(), chunk.end());
            for (size_t end = buffer->find('\n'); end != std::string::npos; end = buffer->find('\n')) {
                const std::string line = buffer->substr(0, end);
                buffer->erase(0, end + 1);
                on_line(device, line);
            }
        };
    }

#define SKIP_WITHOUT_PTY(device)                         \
    if (!(device).ok()) {                                \
        GTEST_SKIP() << "no pseudo-terminal available"; \
    }

    // ------------------------------------------------------------------
    // Streams
    // ------------------------------------------------------------------

    TEST(SerialStackTest, TelemetryArrivesInOrderWithoutLoss)
    {
        VirtualSerialDevice board;
        SKIP_WITHOUT_PTY(board)
        VirtualPort port;
        port.attach(board);
        StackRig rig;
        ASSERT_TRUE(rig.open_port(port.path(), SerialSpec::lines()));
        ASSERT_TRUE(eventually([&] { return rig.in_state(EndpointState::OPEN); }));

        board.start_telemetry(2ms, [](uint64_t i) { return std::to_string(i) + "," + std::to_string(i * 2) + "\n"; });
        ASSERT_TRUE(eventually([&] { return rig.inbox.size() >= 150; }));
        board.stop_telemetry();
        size_t settled = 0;
        ASSERT_TRUE(eventually([&] {
            const size_t now = rig.inbox.size();
            const bool quiet = now == settled;
            settled = now;
            std::this_thread::sleep_for(20ms);
            return quiet;
        }));

        const auto vectors = rig.inbox.vectors();
        ASSERT_GE(vectors.size(), 150U);
        for (size_t i = 0; i < vectors.size(); ++i) {
            ASSERT_EQ(vectors[i], (Numbers { static_cast<double>(i), static_cast<double>(i * 2) })) << "line " << i;
        }
        const auto stats = rig.device()->stats();
        EXPECT_EQ(stats.dropped, 0U);
        EXPECT_EQ(stats.unparsed, 0U);
        for (const auto& value : rig.inbox.all()) {
            EXPECT_EQ(value.device_id, rig.ids[0]);
            EXPECT_EQ(value.source_type, InputType::SERIAL);
        }
    }

    TEST(SerialStackTest, ABurstOfTwentyThousandLinesLosesNothing)
    {
        VirtualSerialDevice board;
        SKIP_WITHOUT_PTY(board)
        VirtualPort port;
        port.attach(board);
        StackRig rig;
        ASSERT_TRUE(rig.open_port(port.path(), SerialSpec::lines()));
        ASSERT_TRUE(eventually([&] { return rig.in_state(EndpointState::OPEN); }));
        constexpr size_t k_lines = 20000;
        std::string burst;
        for (size_t i = 0; i < k_lines; ++i) {
            burst += std::to_string(i) + "\n";
        }

        board.write(burst);

        ASSERT_TRUE(eventually([&] { return rig.inbox.size() >= k_lines; }, 15s));
        const auto vectors = rig.inbox.vectors();
        ASSERT_EQ(vectors.size(), k_lines);
        for (size_t i = 0; i < k_lines; ++i) {
            ASSERT_EQ(vectors[i], (Numbers { static_cast<double>(i) })) << "line " << i;
        }
    }

    TEST(SerialStackTest, GarbageAndOverlongLinesDoNotBreakTheStream)
    {
        VirtualSerialDevice board;
        SKIP_WITHOUT_PTY(board)
        VirtualPort port;
        port.attach(board);
        StackRig rig;
        ASSERT_TRUE(rig.open_port(port.path(), SerialSpec::lines()));
        ASSERT_TRUE(eventually([&] { return rig.in_state(EndpointState::OPEN); }));

        board.write("1 2\n");
        board.write(Bytes { 0xFF, 0x00, 0xFE, '\n' });
        board.write("abc\n");
        board.write(std::string(6000, 'x') + "\n");
        board.write("7 8");
        board.write("\r\n");
        board.write("3\n");

        ASSERT_TRUE(eventually([&] { return rig.inbox.size() >= 3; }));
        EXPECT_EQ(rig.inbox.vectors(), (std::vector<Numbers> { { 1, 2 }, { 7, 8 }, { 3 } }));
        const auto stats = rig.device()->stats();
        EXPECT_EQ(stats.dropped, 1U);
        EXPECT_GE(stats.unparsed, 2U);
        EXPECT_TRUE(rig.in_state(EndpointState::OPEN));
    }

    TEST(SerialStackTest, AFloodWithoutALineEndNeitherGrowsNorWedgesTheStream)
    {
        VirtualSerialDevice board;
        SKIP_WITHOUT_PTY(board)
        VirtualPort port;
        port.attach(board);
        StackRig rig;
        ASSERT_TRUE(rig.open_port(port.path(), SerialSpec::lines().max_message(1024)));
        ASSERT_TRUE(eventually([&] { return rig.in_state(EndpointState::OPEN); }));

        board.write(std::string(size_t { 1 } << 20, 'x'));
        board.write("\n9\n");

        ASSERT_TRUE(eventually([&] { return rig.inbox.size() >= 1; }, 10s));
        EXPECT_EQ(rig.inbox.vectors().back(), (Numbers { 9 }));
        EXPECT_GE(rig.device()->stats().dropped, 1U);
    }

    TEST(SerialStackTest, DataTheLibraryWritesReachesTheDevice)
    {
        VirtualSerialDevice board;
        SKIP_WITHOUT_PTY(board)
        VirtualPort port;
        port.attach(board);
        StackRig rig;
        ASSERT_TRUE(rig.open_port(port.path(), SerialSpec::lines()));
        ASSERT_TRUE(eventually([&] { return rig.in_state(EndpointState::OPEN); }));
        Bytes payload(100000);
        std::mt19937_64 rng(5);
        for (auto& byte : payload) {
            byte = static_cast<uint8_t>(rng() % 256);
        }

        ASSERT_EQ(rig.backend->write(rig.ids[0], payload), static_cast<int>(payload.size()));

        ASSERT_TRUE(eventually([&] { return board.received().size() >= payload.size(); }, 10s));
        EXPECT_EQ(board.received(), payload);
    }

    // ------------------------------------------------------------------
    // Protocols
    // ------------------------------------------------------------------

    TEST(SerialStackTest, ARequestIsAnsweredWhileTelemetryKeepsFlowing)
    {
        VirtualSerialDevice board;
        SKIP_WITHOUT_PTY(board)
        board.set_responder(line_responder([](VirtualSerialDevice& device, const std::string& line) {
            if (line == "?") {
                device.write("ok v1\n");
            }
        }));
        VirtualPort port;
        port.attach(board);
        StackRig rig;
        ASSERT_TRUE(rig.open_port(port.path(), SerialSpec::lines()));
        ASSERT_TRUE(eventually([&] { return rig.in_state(EndpointState::OPEN); }));
        board.start_telemetry(2ms, [](uint64_t i) { return std::to_string(i) + "\n"; });
        ASSERT_TRUE(eventually([&] { return rig.inbox.size() >= 20; }));

        std::promise<std::pair<SerialReply, std::string>> answer;
        auto future = answer.get_future();
        const size_t before = rig.inbox.size();
        ASSERT_TRUE(rig.device()->request(
            Bytes { '?', '\n' },
            [](P::Message m) { return P::as_text(m) == "ok v1"; },
            2s,
            [&answer](SerialReply how, P::Message reply) {
                answer.set_value({ how, std::string(P::as_text(reply)) });
            }));

        ASSERT_EQ(future.wait_for(3s), std::future_status::ready);
        const auto result = future.get();
        EXPECT_EQ(result.first, SerialReply::REPLIED);
        EXPECT_EQ(result.second, "ok v1");
        ASSERT_TRUE(eventually([&] { return rig.inbox.size() > before + 5; }));
        board.stop_telemetry();
        EXPECT_EQ(rig.device()->stats().unparsed, 0U);
    }

    TEST(SerialStackTest, ARequestToADeviceThatNeverAnswersTimesOut)
    {
        VirtualSerialDevice board;
        SKIP_WITHOUT_PTY(board)
        VirtualPort port;
        port.attach(board);
        StackRig rig;
        ASSERT_TRUE(rig.open_port(port.path(), SerialSpec::lines()));
        ASSERT_TRUE(eventually([&] { return rig.in_state(EndpointState::OPEN); }));

        std::promise<SerialReply> answer;
        auto future = answer.get_future();
        ASSERT_TRUE(rig.device()->request(
            Bytes { '?', '\n' }, [](P::Message) { return true; }, 60ms,
            [&answer](SerialReply how, P::Message) { answer.set_value(how); }));

        ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
        EXPECT_EQ(future.get(), SerialReply::TIMED_OUT);
        EXPECT_EQ(board.received_text(), "?\n");
    }

    TEST(SerialStackTest, ACobsStreamCarriesEveryByteValueAndSurvivesACorruptFrame)
    {
        VirtualSerialDevice board;
        SKIP_WITHOUT_PTY(board)
        VirtualPort port;
        port.attach(board);
        StackRig rig;
        ASSERT_TRUE(rig.open_port(port.path(), SerialSpec::cobs()));
        ASSERT_TRUE(eventually([&] { return rig.in_state(EndpointState::OPEN); }));
        Bytes everything(256);
        std::iota(everything.begin(), everything.end(), uint8_t { 0 });
        Bytes wire;
        P::encode_cobs(everything, wire);
        const Bytes corrupt { 0x09, 0x01, 0x00 };
        wire.insert(wire.end(), corrupt.begin(), corrupt.end());
        P::encode_cobs(Bytes { 0, 0, 0 }, wire);

        board.write(wire);

        ASSERT_TRUE(eventually([&] { return rig.inbox.size() >= 2; }));
        const auto values = rig.inbox.all();
        ASSERT_EQ(values.size(), 2U);
        EXPECT_EQ(values[0].as_bytes(), everything);
        EXPECT_EQ(values[1].as_bytes(), (Bytes { 0, 0, 0 }));
        EXPECT_EQ(rig.device()->stats().dropped, 1U);
    }

    TEST(SerialStackTest, AMidiStreamAtThirtyOneThousandBaudBecomesMidiValues)
    {
        VirtualSerialDevice board;
        SKIP_WITHOUT_PTY(board)
        VirtualPort port;
        port.attach(board);
        StackRig rig;
        ASSERT_TRUE(rig.open_port(port.path(), SerialSpec::midi()));
        ASSERT_TRUE(eventually([&] { return rig.in_state(EndpointState::OPEN); }));

        board.write(Bytes { 0x90, 60, 100, 0xF8, 64, 90, 0xC1, 7, 0x80, 60, 0 });

        ASSERT_TRUE(eventually([&] { return rig.inbox.size() >= 5; }));
        const auto values = rig.inbox.all();
        ASSERT_EQ(values.size(), 5U);
        for (const auto& value : values) {
            EXPECT_EQ(value.type, InputValue::Type::MIDI);
        }
        EXPECT_EQ(values[0].as_midi().status, 0x90);
        EXPECT_EQ(values[1].as_midi().status, 0xF8);
        EXPECT_EQ(values[2].as_midi().status, 0x90);
        EXPECT_EQ(values[2].as_midi().data1, 64);
        EXPECT_EQ(values[3].as_midi().status, 0xC1);
        EXPECT_EQ(values[4].as_midi().status, 0x80);
    }

    TEST(SerialStackTest, SilenceOnTheLineEndsAnIdleGapMessage)
    {
        VirtualSerialDevice board;
        SKIP_WITHOUT_PTY(board)
        VirtualPort port;
        port.attach(board);
        StackRig rig;
        ASSERT_TRUE(rig.open_port(port.path(), SerialSpec::idle_gap(20ms)));
        ASSERT_TRUE(eventually([&] { return rig.in_state(EndpointState::OPEN); }));

        board.write("ab");
        board.write("cd");
        ASSERT_TRUE(eventually([&] { return rig.inbox.size() >= 1; }));
        std::this_thread::sleep_for(100ms);
        board.write("ef");

        ASSERT_TRUE(eventually([&] { return rig.inbox.size() >= 2; }));
        EXPECT_EQ(rig.inbox.texts(), (std::vector<std::string> { "abcd", "ef" }));
    }

    // ------------------------------------------------------------------
    // Opening
    // ------------------------------------------------------------------

    TEST(SerialStackTest, SettleTimeHoldsTheEndpointAndDiscardsWhatTheBoardSaidWhileItBooted)
    {
        VirtualSerialDevice board;
        SKIP_WITHOUT_PTY(board)
        VirtualPort port;
        port.attach(board);
        StackRig rig;
        ASSERT_TRUE(rig.open_port(port.path(), SerialSpec::lines().settle_ms(250)));

        EXPECT_TRUE(rig.in_state(EndpointState::OPENING));
        board.write("boot 9\n");
        ASSERT_TRUE(eventually([&] { return rig.in_state(EndpointState::OPEN); }));
        std::this_thread::sleep_for(60ms);
        EXPECT_EQ(rig.inbox.size(), 0U);

        board.write("5\n");

        ASSERT_TRUE(eventually([&] { return rig.inbox.size() >= 1; }));
        EXPECT_EQ(rig.inbox.vectors(), (std::vector<Numbers> { { 5 } }));
    }

    TEST(SerialStackTest, ClosingAndReopeningADeviceResumesTheStream)
    {
        VirtualSerialDevice board;
        SKIP_WITHOUT_PTY(board)
        VirtualPort port;
        port.attach(board);
        StackRig rig;
        ASSERT_TRUE(rig.open_port(port.path(), SerialSpec::lines()));
        ASSERT_TRUE(eventually([&] { return rig.in_state(EndpointState::OPEN); }));
        board.write("1\n");
        ASSERT_TRUE(eventually([&] { return rig.inbox.size() >= 1; }));

        rig.backend->close_device(rig.ids[0]);
        EXPECT_FALSE(rig.backend->is_device_open(rig.ids[0]));
        ASSERT_TRUE(rig.backend->open_device(rig.ids[0]));
        ASSERT_TRUE(eventually([&] { return rig.in_state(EndpointState::OPEN); }));
        board.write("2\n");

        ASSERT_TRUE(eventually([&] { return rig.inbox.size() >= 2; }));
        EXPECT_EQ(rig.inbox.vectors(), (std::vector<Numbers> { { 1 }, { 2 } }));
    }

    TEST(SerialStackTest, TwoDevicesAtOnceStayApart)
    {
        VirtualSerialDevice first;
        SKIP_WITHOUT_PTY(first)
        VirtualSerialDevice second;
        SKIP_WITHOUT_PTY(second)
        VirtualPort port_a;
        VirtualPort port_b;
        port_a.attach(first);
        port_b.attach(second);
        StackRig rig;
        ASSERT_TRUE(rig.open_ports({ { port_a.path(), SerialSpec::lines() },
            { port_b.path(), SerialSpec::delimited(">").bytes() } }));
        ASSERT_TRUE(eventually([&] { return rig.in_state(EndpointState::OPEN, 0) && rig.in_state(EndpointState::OPEN, 1); }));

        first.write("1 2\n3 4\n");
        second.write("AA>BB>");

        ASSERT_TRUE(eventually([&] { return rig.inbox.size() >= 4; }));
        for (const auto& value : rig.inbox.all()) {
            if (value.type == InputValue::Type::VECTOR) {
                EXPECT_EQ(value.device_id, rig.ids[0]);
            } else {
                EXPECT_EQ(value.device_id, rig.ids[1]);
            }
        }
        EXPECT_EQ(rig.inbox.vectors(), (std::vector<Numbers> { { 1, 2 }, { 3, 4 } }));
        const auto bytes_seen = std::ranges::count_if(rig.inbox.all(),
            [](const InputValue& v) { return v.type == InputValue::Type::BYTES; });
        EXPECT_EQ(bytes_seen, 2);
    }

    // ------------------------------------------------------------------
    // Unplugging
    // ------------------------------------------------------------------

    TEST(SerialStackTest, ReplugKeepsTheIdEndsAPendingRequestAndNeverJoinsAPartialLine)
    {
        VirtualSerialDevice first;
        SKIP_WITHOUT_PTY(first)
        VirtualPort port;
        port.attach(first);
        StackRig rig;
        ASSERT_TRUE(rig.open_port(port.path(), SerialSpec::lines()));
        ASSERT_TRUE(eventually([&] { return rig.in_state(EndpointState::OPEN); }));
        const uint32_t id = rig.ids[0];
        ASSERT_EQ(rig.count_events(true), 1U);

        std::promise<SerialReply> answer;
        auto future = answer.get_future();
        ASSERT_TRUE(rig.device()->request(
            Bytes { '?', '\n' }, [](P::Message) { return true; }, 10s,
            [&answer](SerialReply how, P::Message) { answer.set_value(how); }));
        first.write("12");
        ASSERT_TRUE(eventually([&] { return first.received().size() >= 2; }));
        std::this_thread::sleep_for(30ms);

        first.hang_up();
        port.detach();

        ASSERT_TRUE(eventually([&] { return !rig.in_state(EndpointState::OPEN); }));
        ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
        EXPECT_EQ(future.get(), SerialReply::LINK_LOST);
        ASSERT_TRUE(eventually([&] { return rig.count_events(false) >= 1; }));

        VirtualSerialDevice second;
        SKIP_WITHOUT_PTY(second)
        port.attach(second);
        ASSERT_TRUE(eventually([&] { return rig.in_state(EndpointState::OPEN); }));
        ASSERT_TRUE(eventually([&] { return rig.count_events(true) >= 2; }));
        second.write("3\n");

        ASSERT_TRUE(eventually([&] { return rig.inbox.size() >= 1; }));
        EXPECT_EQ(rig.inbox.vectors(), (std::vector<Numbers> { { 3 } }));
        EXPECT_EQ(rig.ids[0], id);
        EXPECT_EQ(rig.backend->get_devices().front().id, id);
        EXPECT_TRUE(rig.backend->get_devices().front().is_connected);
    }

    TEST(SerialStackTest, ADevicePluggedInAfterTheLibraryAsksStartsWhenItAppears)
    {
        VirtualPort port;
        StackRig rig;
        ASSERT_TRUE(rig.open_port(port.path(), SerialSpec::lines()));
        EXPECT_TRUE(rig.in_state(EndpointState::RECONNECTING));
        EXPECT_EQ(rig.count_events(true), 0U);

        VirtualSerialDevice board;
        SKIP_WITHOUT_PTY(board)
        port.attach(board);

        ASSERT_TRUE(eventually([&] { return rig.in_state(EndpointState::OPEN); }));
        board.write("4\n");
        ASSERT_TRUE(eventually([&] { return rig.inbox.size() >= 1; }));
        EXPECT_EQ(rig.inbox.vectors(), (std::vector<Numbers> { { 4 } }));
        EXPECT_EQ(rig.count_events(true), 1U);
    }

    TEST(SerialStackTest, RepeatedUnpluggingWhileDataFlowsNeverDeliversAMalformedValue)
    {
        VirtualPort port;
        StackRig rig(10);
        ASSERT_TRUE(rig.open_port(port.path(), SerialSpec::lines()));

        for (int round = 0; round < 6; ++round) {
            VirtualSerialDevice board;
            SKIP_WITHOUT_PTY(board)
            port.attach(board);
            ASSERT_TRUE(eventually([&] { return rig.in_state(EndpointState::OPEN); })) << "round " << round;
            board.start_telemetry(1ms, [](uint64_t i) { return std::to_string(i) + " " + std::to_string(i + 1) + "\n"; });
            std::this_thread::sleep_for(40ms);
            board.hang_up();
            port.detach();
            ASSERT_TRUE(eventually([&] { return !rig.in_state(EndpointState::OPEN); })) << "round " << round;
        }

        for (const auto& vector : rig.inbox.vectors()) {
            ASSERT_EQ(vector.size(), 2U);
            ASSERT_EQ(vector[1], vector[0] + 1);
        }
        EXPECT_GE(rig.inbox.size(), 6U);
    }

    TEST(SerialStackTest, TearingTheStackDownWhileTheBoardIsTalkingIsClean)
    {
        for (int round = 0; round < 5; ++round) {
            VirtualSerialDevice board;
            SKIP_WITHOUT_PTY(board)
            VirtualPort port;
            port.attach(board);
            {
                StackRig rig;
                ASSERT_TRUE(rig.open_port(port.path(), SerialSpec::lines()));
                ASSERT_TRUE(eventually([&] { return rig.in_state(EndpointState::OPEN); }));
                board.start_telemetry(1ms, [](uint64_t i) { return std::to_string(i) + "\n"; });
                std::this_thread::sleep_for(30ms);
            }
            board.stop_telemetry();
        }
        SUCCEED();
    }

} // namespace
} // namespace MayaFlux::Test

#endif
