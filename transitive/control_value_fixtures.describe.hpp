#pragma once

#include "MayaFlux/Transitive/Reflect/Describe.hpp"

#ifndef MAYAFLUX_REFLECT_SCAN

namespace MayaFlux::Reflect::Generated {

template <>
struct Description<::MayaFlux::Test::SlotFlatTuning> {
    static constexpr std::string_view name = "MayaFlux::Test::SlotFlatTuning";

    static constexpr auto members()
    {
        return std::tuple {
            Member<&::MayaFlux::Test::SlotFlatTuning::scale> { "scale" },
            Member<&::MayaFlux::Test::SlotFlatTuning::decay> { "decay" },
        };
    }
};

template <>
struct Description<::MayaFlux::Test::SlotLabelTuning> {
    static constexpr std::string_view name = "MayaFlux::Test::SlotLabelTuning";

    static constexpr auto members()
    {
        return std::tuple {
            Member<&::MayaFlux::Test::SlotLabelTuning::label> { "label" },
            Member<&::MayaFlux::Test::SlotLabelTuning::note> { "note" },
        };
    }
};

template <>
struct Description<::MayaFlux::Test::SlotPadTuning> {
    static constexpr std::string_view name = "MayaFlux::Test::SlotPadTuning";

    static constexpr auto members()
    {
        return std::tuple {
            Member<&::MayaFlux::Test::SlotPadTuning::scale> { "scale" },
            Member<&::MayaFlux::Test::SlotPadTuning::left> { "left" },
            Member<&::MayaFlux::Test::SlotPadTuning::right> { "right" },
        };
    }
};

template <>
struct Description<::MayaFlux::Test::SlotStickTuning> {
    static constexpr std::string_view name = "MayaFlux::Test::SlotStickTuning";

    static constexpr auto members()
    {
        return std::tuple {
            Member<&::MayaFlux::Test::SlotStickTuning::deadzone> { "deadzone" },
            Member<&::MayaFlux::Test::SlotStickTuning::invert> { "invert" },
        };
    }
};

}

#endif
