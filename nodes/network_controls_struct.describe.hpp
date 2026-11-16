#pragma once

#include "MayaFlux/Transitive/Reflect/Describe.hpp"

#ifndef MAYAFLUX_REFLECT_SCAN

namespace MayaFlux::Reflect::Generated {

template <>
struct Description<::MayaFlux::Test::ControlsProbeControls> {
    static constexpr std::string_view name = "MayaFlux::Test::ControlsProbeControls";

    static constexpr auto members()
    {
        return std::tuple {
            Member<&::MayaFlux::Test::ControlsProbeControls::scale> { "scale" },
            Member<&::MayaFlux::Test::ControlsProbeControls::gain> { "gain" },
            Member<&::MayaFlux::Test::ControlsProbeControls::tilt> { "tilt" },
        };
    }
};

template <>
struct Description<::MayaFlux::Test::ControlsProbeOperatorControls> {
    static constexpr std::string_view name = "MayaFlux::Test::ControlsProbeOperatorControls";

    static constexpr auto members()
    {
        return std::tuple {
            Member<&::MayaFlux::Test::ControlsProbeOperatorControls::gain> { "gain" },
            Member<&::MayaFlux::Test::ControlsProbeOperatorControls::tilt> { "tilt" },
            Member<&::MayaFlux::Test::ControlsProbeOperatorControls::direction> { "direction" },
        };
    }
};

}

#endif
