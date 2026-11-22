#pragma once

#include "MayaFlux/Transitive/Reflect/Describe.hpp"

#ifndef MAYAFLUX_REFLECT_SCAN

namespace MayaFlux::Reflect::Generated {

template <>
struct Description<::MayaFlux::Test::StoreBloomTuning> {
    static constexpr std::string_view name = "MayaFlux::Test::StoreBloomTuning";

    static constexpr auto members()
    {
        return std::tuple {
            Member<&::MayaFlux::Test::StoreBloomTuning::rate> { "rate" },
            Member<&::MayaFlux::Test::StoreBloomTuning::angle> { "angle" },
        };
    }
};

template <>
struct Description<::MayaFlux::Test::StoreMixedTuning> {
    static constexpr std::string_view name = "MayaFlux::Test::StoreMixedTuning";

    static constexpr auto members()
    {
        return std::tuple {
            Member<&::MayaFlux::Test::StoreMixedTuning::gain> { "gain" },
            Member<&::MayaFlux::Test::StoreMixedTuning::label> { "label" },
        };
    }
};

template <>
struct Description<::MayaFlux::Test::StoreQuantityTuning> {
    static constexpr std::string_view name = "MayaFlux::Test::StoreQuantityTuning";

    static constexpr auto members()
    {
        return std::tuple {
            Member<&::MayaFlux::Test::StoreQuantityTuning::position> { "position" },
            Member<&::MayaFlux::Test::StoreQuantityTuning::intensity> { "intensity" },
            Member<&::MayaFlux::Test::StoreQuantityTuning::orientation> { "orientation" },
        };
    }
};

template <>
struct Description<::MayaFlux::Test::StoreScaleExtension> {
    static constexpr std::string_view name = "MayaFlux::Test::StoreScaleExtension";

    static constexpr auto members()
    {
        return std::tuple {
            Member<&::MayaFlux::Test::StoreScaleExtension::scale> { "scale" },
        };
    }
};

}

#endif
