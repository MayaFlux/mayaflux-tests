#pragma once

#include "MayaFlux/Core/Input/SerialLink.hpp"

#if !defined(MAYAFLUX_PLATFORM_WINDOWS)
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace MayaFlux::Test {

class FakeSerialLink : public Core::ISerialLink {
public:
    struct Timer {
        std::chrono::milliseconds delay {};
        std::function<void()> callback;
    };

    struct ControlCall {
        std::optional<bool> dtr;
        std::optional<bool> rts;
    };

    bool open(const std::string& path, const Core::SerialLineSettings& line) override
    {
        opened.emplace_back(path, line);
        current = open_result ? Core::EndpointState::OPEN : Core::EndpointState::CLOSED;
        return open_result;
    }

    void close() override
    {
        ++closes;
        current = Core::EndpointState::CLOSED;
    }

    [[nodiscard]] Core::EndpointState state() const override { return current; }

    bool write(std::span<const uint8_t> bytes) override
    {
        if (on_write) {
            on_write();
        }
        if (!write_result) {
            return false;
        }
        writes.emplace_back(bytes.begin(), bytes.end());
        return true;
    }

    [[nodiscard]] size_t backlog() const override { return backlog_value; }

    bool set_control_lines(std::optional<bool> dtr, std::optional<bool> rts) override
    {
        control_calls.push_back({ dtr, rts });
        return true;
    }

    bool send_break() override
    {
        ++breaks;
        return true;
    }

    TimerToken post_after(std::chrono::milliseconds delay, std::function<void()> callback) override
    {
        const TimerToken token = next_token++;
        timers[token] = { delay, std::move(callback) };
        if (on_post_after) {
            on_post_after();
        }
        return token;
    }

    void cancel_post(TimerToken token) override
    {
        cancelled.push_back(token);
        timers.erase(token);
    }

    void set_chunk_callback(ChunkCallback callback) override { chunk = std::move(callback); }
    void set_state_callback(StateCallback callback) override { state_callback = std::move(callback); }

    void feed(const std::vector<uint8_t>& bytes) const
    {
        chunk(std::span<const uint8_t> { bytes });
    }

    void feed(std::string_view text) const
    {
        feed(std::vector<uint8_t>(text.begin(), text.end()));
    }

    void transition(Core::EndpointState previous, Core::EndpointState now)
    {
        current = now;
        if (state_callback) {
            state_callback(previous, now);
        }
    }

    bool fire(TimerToken token)
    {
        const auto it = timers.find(token);
        if (it == timers.end()) {
            return false;
        }
        const auto callback = std::move(it->second.callback);
        timers.erase(it);
        callback();
        return true;
    }

    bool open_result { true };
    bool write_result { true };
    size_t backlog_value {};
    Core::EndpointState current { Core::EndpointState::CLOSED };
    TimerToken next_token { 1 };
    int closes {};
    int breaks {};

    std::vector<std::pair<std::string, Core::SerialLineSettings>> opened;
    std::vector<std::vector<uint8_t>> writes;
    std::vector<ControlCall> control_calls;
    std::map<TimerToken, Timer> timers;
    std::vector<TimerToken> cancelled;

    ChunkCallback chunk;
    StateCallback state_callback;

    std::function<void()> on_write;
    std::function<void()> on_post_after;
};

#if !defined(MAYAFLUX_PLATFORM_WINDOWS)

/**
 * A device on the far side of a pseudo-terminal, the way the MIDI tests put a virtual
 * source on the other side of the sequencer. It runs on its own thread, reads what the
 * library writes, may answer it, and can talk on its own, so the library sees a port
 * behave like a board does. Data written before the library opens the slave is
 * processed by the terminal's line discipline, so write after the port is open.
 */
class VirtualSerialDevice {
public:
    using Responder = std::function<void(VirtualSerialDevice& device, const std::vector<uint8_t>& chunk)>;
    using LineSource = std::function<std::string(uint64_t index)>;

    VirtualSerialDevice()
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
        m_thread = std::jthread([this](const std::stop_token& stop) { run(stop); });
    }

    VirtualSerialDevice(const VirtualSerialDevice&) = delete;
    VirtualSerialDevice& operator=(const VirtualSerialDevice&) = delete;
    VirtualSerialDevice(VirtualSerialDevice&&) = delete;
    VirtualSerialDevice& operator=(VirtualSerialDevice&&) = delete;

    ~VirtualSerialDevice() { hang_up(); }

    [[nodiscard]] bool ok() const { return m_slave.size() > 0; }
    [[nodiscard]] const std::string& slave_path() const { return m_slave; }

    void set_responder(Responder responder)
    {
        std::scoped_lock lock(m_mutex);
        m_responder = std::move(responder);
    }

    void start_telemetry(std::chrono::milliseconds period, LineSource source)
    {
        std::scoped_lock lock(m_mutex);
        m_period = period;
        m_source = std::move(source);
        m_next_line = std::chrono::steady_clock::now();
    }

    void stop_telemetry()
    {
        std::scoped_lock lock(m_mutex);
        m_source = nullptr;
    }

    /** @brief Write to the port. Blocks while the terminal is full, gives up after a hang-up. */
    void write(const std::vector<uint8_t>& bytes)
    {
        std::scoped_lock lock(m_write_mutex);
        size_t done = 0;
        while (done < bytes.size()) {
            const int fd = m_master.load();
            if (fd < 0) {
                return;
            }
            const ssize_t put = ::write(fd, bytes.data() + done, bytes.size() - done);
            if (put > 0) {
                done += static_cast<size_t>(put);
                continue;
            }
            pollfd waiting { .fd = fd, .events = POLLOUT, .revents = 0 };
            ::poll(&waiting, 1, 5);
        }
    }

    void write(std::string_view text)
    {
        write(std::vector<uint8_t>(text.begin(), text.end()));
    }

    /** @brief Everything the library has written to the port so far. */
    [[nodiscard]] std::vector<uint8_t> received() const
    {
        std::scoped_lock lock(m_mutex);
        return m_received;
    }

    [[nodiscard]] std::string received_text() const
    {
        const auto bytes = received();
        return std::string(bytes.begin(), bytes.end());
    }

    /** @brief The device is unplugged: the thread stops and the terminal closes. */
    void hang_up()
    {
        if (m_thread.joinable()) {
            m_thread.request_stop();
            m_thread.join();
        }
        const int fd = m_master.exchange(-1);
        if (fd >= 0) {
            ::close(fd);
        }
    }

private:
    void run(const std::stop_token& stop)
    {
        while (!stop.stop_requested()) {
            pollfd watching { .fd = m_master.load(), .events = POLLIN, .revents = 0 };
            ::poll(&watching, 1, 2);

            if ((watching.revents & POLLIN) != 0) {
                std::vector<uint8_t> chunk(4096);
                const ssize_t got = ::read(watching.fd, chunk.data(), chunk.size());
                if (got > 0) {
                    chunk.resize(static_cast<size_t>(got));
                    Responder responder;
                    {
                        std::scoped_lock lock(m_mutex);
                        m_received.insert(m_received.end(), chunk.begin(), chunk.end());
                        responder = m_responder;
                    }
                    if (responder) {
                        responder(*this, chunk);
                    }
                }
            } else if ((watching.revents & (POLLHUP | POLLERR)) != 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }

            LineSource source;
            std::chrono::milliseconds period {};
            bool due = false;
            {
                std::scoped_lock lock(m_mutex);
                source = m_source;
                period = m_period;
                if (source && std::chrono::steady_clock::now() >= m_next_line) {
                    due = true;
                    m_next_line += period;
                }
            }
            if (due) {
                write(source(m_line_index++));
            }
        }
    }

    std::atomic<int> m_master { -1 };
    std::string m_slave;
    mutable std::mutex m_mutex;
    std::mutex m_write_mutex;
    std::vector<uint8_t> m_received;
    Responder m_responder;
    LineSource m_source;
    std::chrono::milliseconds m_period {};
    std::chrono::steady_clock::time_point m_next_line;
    uint64_t m_line_index {};
    std::jthread m_thread;
};

/**
 * A stable path to a port that comes and goes, the way /dev/serial/by-id does: a
 * symbolic link in a temporary directory that points at whichever virtual device is
 * attached. A replug is detach followed by attach on a new device.
 */
class VirtualPort {
public:
    VirtualPort()
    {
        static std::atomic<uint64_t> counter { 0 };
        m_directory = std::filesystem::temp_directory_path()
            / ("mayaflux-port-" + std::to_string(::getpid()) + "-" + std::to_string(counter.fetch_add(1)));
        std::filesystem::create_directories(m_directory);
        m_path = m_directory / "port";
    }

    VirtualPort(const VirtualPort&) = delete;
    VirtualPort& operator=(const VirtualPort&) = delete;
    VirtualPort(VirtualPort&&) = delete;
    VirtualPort& operator=(VirtualPort&&) = delete;

    ~VirtualPort()
    {
        std::error_code ec;
        std::filesystem::remove_all(m_directory, ec);
    }

    void attach(const VirtualSerialDevice& device)
    {
        detach();
        std::filesystem::create_symlink(device.slave_path(), m_path);
    }

    void detach()
    {
        std::error_code ec;
        std::filesystem::remove(m_path, ec);
    }

    [[nodiscard]] std::string path() const { return m_path.string(); }

private:
    std::filesystem::path m_directory;
    std::filesystem::path m_path;
};

#endif

} // namespace MayaFlux::Test
