#pragma once

#include "MayaFlux/Nodes/Network/NodeNetwork.hpp"
#include "network_controls_struct.hpp"

namespace MayaFlux::Test {

class ControlsProbeNetwork final : public Nodes::Network::NodeNetwork {
    ControlsProbeControls& m_controls;

public:
    using Controls = ControlsProbeControls;

    explicit ControlsProbeNetwork(size_t nodes = 3)
        : m_controls(declare_controls<ControlsProbeControls>())
        , outputs(nodes, 0.0)
    {
    }

    void process_batch(unsigned int /*num_samples*/) override
    {
        ++batches;
        refresh_controls();

        gain_seen = m_controls.gain.pending();

        tilt_seen.clear();
        if (m_controls.tilt.due()) {
            for (size_t index = 0; index < outputs.size(); ++index) {
                if (const auto value = m_controls.tilt.at(index)) {
                    tilt_seen.push_back(*value);
                }
            }
            m_controls.tilt.settle();
        }
    }

    [[nodiscard]] size_t get_node_count() const override { return outputs.size(); }

    [[nodiscard]] std::optional<double> get_node_output(size_t index) const override
    {
        if (index >= outputs.size()) {
            return std::nullopt;
        }
        return outputs[index];
    }

    void read_controls(ControlsProbeControls& controls) const
    {
        controls.gain.observe(actual_gain);
    }

    std::vector<double> outputs;
    std::optional<double> gain_seen;
    std::vector<double> tilt_seen;
    double actual_gain { 1.0 };
    int batches {};
};

class ControlsProbeOperator final : public Nodes::Network::NetworkOperator {
    ControlsProbeOperatorControls& m_controls;

public:
    using Controls = ControlsProbeOperatorControls;

    ControlsProbeOperator()
        : m_controls(declare_controls<ControlsProbeOperatorControls>())
    {
    }

    void process(float /*dt*/) override
    {
        ++blocks;
        refresh_controls();

        gain_seen = m_controls.gain.pending();
        direction_seen = m_controls.direction.pending<glm::vec3>();

        tilt_seen.clear();
        if (m_controls.tilt.due()) {
            m_controls.tilt.settle();
            const size_t count = m_controls.tilt.per_element() ? m_controls.tilt.element_count() : 1;
            for (size_t index = 0; index < count; ++index) {
                if (const auto value = m_controls.tilt.at(index)) {
                    tilt_seen.push_back(*value);
                }
            }
        }
    }

    void set_parameter(std::string_view /*param*/, double /*value*/) override { }

    [[nodiscard]] std::optional<double> query_state(std::string_view /*query*/) const override
    {
        return std::nullopt;
    }

    [[nodiscard]] std::string_view get_type_name() const override { return "ControlsProbe"; }

    void read_controls(ControlsProbeOperatorControls& controls) const
    {
        controls.gain.observe(actual_gain);
    }

    std::optional<double> gain_seen;
    std::optional<glm::vec3> direction_seen;
    std::vector<double> tilt_seen;
    double actual_gain { 1.0 };
    int blocks {};
};

}
