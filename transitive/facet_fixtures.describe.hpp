#pragma once

#include "MayaFlux/Transitive/Reflect/Describe.hpp"

#ifndef MAYAFLUX_REFLECT_SCAN

namespace MayaFlux::Reflect::Generated {

template <>
struct Description<::MayaFlux::Test::FacetFromNothing> {
    static constexpr std::string_view name = "MayaFlux::Test::FacetFromNothing";

    static constexpr auto members()
    {
        return std::tuple {
        };
    }
};

template <>
struct Description<::MayaFlux::Test::FacetFromSettings> {
    static constexpr std::string_view name = "MayaFlux::Test::FacetFromSettings";

    static constexpr auto members()
    {
        return std::tuple {
            Member<&::MayaFlux::Test::FacetFromSettings::window> { "window" },
            Member<&::MayaFlux::Test::FacetFromSettings::hop> { "hop" },
            Member<&::MayaFlux::Test::FacetFromSettings::threshold> { "threshold" },
        };
    }
};

}

#endif
