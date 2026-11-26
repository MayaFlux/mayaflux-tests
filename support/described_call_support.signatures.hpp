#pragma once

#include "MayaFlux/Transitive/Reflect/Describe.hpp"

#ifndef MAYAFLUX_REFLECT_SCAN

namespace MayaFlux::Reflect::Generated {

template <>
struct Signature<&::MayaFlux::Test::described_call_route> {
    static constexpr std::string_view name = "MayaFlux::Test::described_call_route";
    static constexpr std::size_t required = 1;
    static constexpr std::array<std::string_view, 4> parameters { "count", "gain", "voice", "source" };

    template <std::size_t K, typename Values>
    static decltype(auto) call(Values& values)
    {
        if constexpr (K == 1) {
            return ::MayaFlux::Test::described_call_route(std::get<0>(values));
        } else if constexpr (K == 2) {
            return ::MayaFlux::Test::described_call_route(std::get<0>(values), std::get<1>(values));
        } else if constexpr (K == 3) {
            return ::MayaFlux::Test::described_call_route(std::get<0>(values), std::get<1>(values), std::get<2>(values));
        } else if constexpr (K == 4) {
            return ::MayaFlux::Test::described_call_route(std::get<0>(values), std::get<1>(values), std::get<2>(values), std::get<3>(values));
        } else {
            static_assert(K != K, "the sidecar has no call with that many arguments");
        }
    }
};

template <>
struct Signature<&::MayaFlux::Test::described_call_scaled> {
    static constexpr std::string_view name = "MayaFlux::Test::described_call_scaled";
    static constexpr std::size_t required = 1;
    static constexpr std::array<std::string_view, 2> parameters { "value", "scale" };

    template <std::size_t K, typename Values>
    static decltype(auto) call(Values& values)
    {
        if constexpr (K == 1) {
            return ::MayaFlux::Test::described_call_scaled(std::get<0>(values));
        } else if constexpr (K == 2) {
            return ::MayaFlux::Test::described_call_scaled(std::get<0>(values), std::get<1>(values));
        } else {
            static_assert(K != K, "the sidecar has no call with that many arguments");
        }
    }
};

}

#endif
