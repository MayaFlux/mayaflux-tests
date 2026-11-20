#pragma once

#include <string_view>

#include "MayaFlux/Nodes/Input/InputNode.hpp"
#include "MayaFlux/Registry/BackendRegistry.hpp"
#include "MayaFlux/Registry/Service/InputService.hpp"
#include "MayaFlux/Registry/Service/NetworkService.hpp"

namespace MayaFlux::Test {

class RecordingNode : public Nodes::Input::InputNode {
public:
    void process_input(const Core::InputValue& value) override
    {
        {
            std::scoped_lock lock(m_mutex);
            m_values.push_back(value);
        }
        InputNode::process_input(value);
    }

    void save_state() override { }
    void restore_state() override { }

    [[nodiscard]] size_t count() const
    {
        std::scoped_lock lock(m_mutex);
        return m_values.size();
    }

    [[nodiscard]] std::vector<Core::InputValue> values() const
    {
        std::scoped_lock lock(m_mutex);
        return m_values;
    }

private:
    mutable std::mutex m_mutex;
    std::vector<Core::InputValue> m_values;
};

class FakeInputService {
public:
    FakeInputService()
    {
        service.get_all_devices = [this] { return devices; };
        service.open_device = [this](Core::InputType type, uint32_t id) {
            opened.emplace_back(type, id);
            return true;
        };
        service.close_device = [this](Core::InputType type, uint32_t id) {
            closed.emplace_back(type, id);
        };
        Registry::BackendRegistry::instance().register_service<Registry::Service::InputService>(
            [this]() -> void* { return &service; });
    }

    ~FakeInputService()
    {
        Registry::BackendRegistry::instance().unregister_service<Registry::Service::InputService>();
    }

    FakeInputService(const FakeInputService&) = delete;
    FakeInputService& operator=(const FakeInputService&) = delete;
    FakeInputService(FakeInputService&&) = delete;
    FakeInputService& operator=(FakeInputService&&) = delete;

    Registry::Service::InputService service;
    std::vector<Core::InputDeviceInfo> devices;
    std::vector<std::pair<Core::InputType, uint32_t>> opened;
    std::vector<std::pair<Core::InputType, uint32_t>> closed;
};

class FakeNetworkService {
public:
    using ReceiveCallback = std::function<void(uint64_t, const uint8_t*, size_t, std::string_view)>;
    using StateCallback = std::function<void(uint64_t, Core::EndpointState, Core::EndpointState)>;

    struct ControlCall {
        uint64_t id {};
        std::optional<bool> dtr;
        std::optional<bool> rts;
    };

    struct PendingTimer {
        std::chrono::milliseconds delay {};
        std::function<void()> callback;
    };

    FakeNetworkService()
    {
        service.open_endpoint = [this](const Core::EndpointInfo& info) {
            opened.push_back(info);
            return next_endpoint_id;
        };
        service.close_endpoint = [this](uint64_t id) { closed.push_back(id); };
        service.set_endpoint_receive_callback = [this](uint64_t id, const ReceiveCallback& callback) {
            callbacks[id] = callback;
        };
        service.set_endpoint_state_callback = [this](uint64_t id, const StateCallback& callback) {
            state_callbacks[id] = callback;
        };
        service.get_write_backlog = [this](uint64_t) { return backlog; };
        service.send = [this](uint64_t, const uint8_t* data, size_t size) {
            sent.emplace_back(data, data + size);
            return send_result;
        };
        service.get_endpoint_state = [this](uint64_t) { return endpoint_state; };
        service.set_control_lines = [this](uint64_t id, std::optional<bool> dtr, std::optional<bool> rts) {
            control_calls.push_back({ id, dtr, rts });
            return true;
        };
        service.send_break = [this](uint64_t id) {
            breaks.push_back(id);
            return true;
        };
        service.post_after = [this](std::chrono::milliseconds delay, std::function<void()> callback) {
            const uint64_t token = next_timer_token++;
            timers[token] = { delay, std::move(callback) };
            return token;
        };
        service.cancel_post = [this](uint64_t token) { timers.erase(token); };
        Registry::BackendRegistry::instance().register_service<Registry::Service::NetworkService>(
            [this]() -> void* { return &service; });
    }

    ~FakeNetworkService()
    {
        Registry::BackendRegistry::instance().unregister_service<Registry::Service::NetworkService>();
    }

    FakeNetworkService(const FakeNetworkService&) = delete;
    FakeNetworkService& operator=(const FakeNetworkService&) = delete;
    FakeNetworkService(FakeNetworkService&&) = delete;
    FakeNetworkService& operator=(FakeNetworkService&&) = delete;

    void deliver(uint64_t id, const std::vector<uint8_t>& bytes)
    {
        callbacks.at(id)(id, bytes.data(), bytes.size(), "127.0.0.1");
    }

    void transition(uint64_t id, Core::EndpointState from, Core::EndpointState to)
    {
        state_callbacks.at(id)(id, from, to);
    }

    bool fire(uint64_t token)
    {
        const auto it = timers.find(token);
        if (it == timers.end())
            return false;
        const auto callback = std::move(it->second.callback);
        timers.erase(it);
        callback();
        return true;
    }

    Registry::Service::NetworkService service;
    uint64_t next_endpoint_id { 42 };
    size_t backlog {};
    bool send_result { true };
    Core::EndpointState endpoint_state { Core::EndpointState::OPEN };
    std::vector<std::vector<uint8_t>> sent;
    uint64_t next_timer_token { 1 };
    std::map<uint64_t, StateCallback> state_callbacks;
    std::vector<ControlCall> control_calls;
    std::vector<uint64_t> breaks;
    std::map<uint64_t, PendingTimer> timers;
    std::vector<Core::EndpointInfo> opened;
    std::vector<uint64_t> closed;
    std::map<uint64_t, ReceiveCallback> callbacks;
};

} // namespace MayaFlux::Test
