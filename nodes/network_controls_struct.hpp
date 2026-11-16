#pragma once

#include "MayaFlux/Nodes/Network/Control.hpp"
#include "MayaFlux/Transitive/Reflect/Describe.hpp"

namespace MayaFlux::Test {

struct ControlsProbeControls {
    Nodes::Network::Control scale;
    Nodes::Network::Control gain;
    Nodes::Network::Control tilt;
};

MF_DESCRIBE(ControlsProbeControls)

struct ControlsProbeOperatorControls {
    Nodes::Network::Control gain;
    Nodes::Network::Control tilt;
    Nodes::Network::Control direction;
};

MF_DESCRIBE(ControlsProbeOperatorControls)

}
#include "network_controls_struct.describe.hpp"
