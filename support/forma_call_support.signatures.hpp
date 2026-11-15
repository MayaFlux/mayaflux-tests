#pragma once

#include "MayaFlux/Transitive/Reflect/Describe.hpp"

#ifndef MAYAFLUX_REFLECT_SCAN

namespace MayaFlux::Reflect::Generated {

template <>
struct Signature<&::MayaFlux::Test::forma_call_fader> {
    static constexpr std::string_view name = "MayaFlux::Test::forma_call_fader";
    static constexpr std::size_t required = 2;
    static constexpr std::array<std::string_view, 4> parameters { "bounds", "handle_w", "track", "live_offset" };

    template <std::size_t K, typename Values>
    static decltype(auto) call(Values& values)
    {
        if constexpr (K == 2) {
            return ::MayaFlux::Test::forma_call_fader(std::get<0>(values), std::get<1>(values));
        } else if constexpr (K == 3) {
            return ::MayaFlux::Test::forma_call_fader(std::get<0>(values), std::get<1>(values), std::get<2>(values));
        } else if constexpr (K == 4) {
            return ::MayaFlux::Test::forma_call_fader(std::get<0>(values), std::get<1>(values), std::get<2>(values), std::get<3>(values));
        } else {
            static_assert(K != K, "the sidecar has no call with that many arguments");
        }
    }
};

template <>
struct Signature<&::MayaFlux::Test::forma_call_flag> {
    static constexpr std::string_view name = "MayaFlux::Test::forma_call_flag";
    static constexpr std::size_t required = 0;
    static constexpr std::array<std::string_view, 0> parameters {};

    template <std::size_t K, typename Values>
    static decltype(auto) call(Values& values)
    {
        if constexpr (K == 0) {
            return ::MayaFlux::Test::forma_call_flag();
        } else {
            static_assert(K != K, "the sidecar has no call with that many arguments");
        }
    }
};

template <>
struct Signature<&::MayaFlux::Test::forma_call_samples> {
    static constexpr std::string_view name = "MayaFlux::Test::forma_call_samples";
    static constexpr std::size_t required = 1;
    static constexpr std::array<std::string_view, 1> parameters { "count" };

    template <std::size_t K, typename Values>
    static decltype(auto) call(Values& values)
    {
        if constexpr (K == 1) {
            return ::MayaFlux::Test::forma_call_samples(std::get<0>(values));
        } else {
            static_assert(K != K, "the sidecar has no call with that many arguments");
        }
    }
};

template <>
struct Signature<&::MayaFlux::Test::forma_call_triple> {
    static constexpr std::string_view name = "MayaFlux::Test::forma_call_triple";
    static constexpr std::size_t required = 1;
    static constexpr std::array<std::string_view, 3> parameters { "a", "b", "c" };

    template <std::size_t K, typename Values>
    static decltype(auto) call(Values& values)
    {
        if constexpr (K == 1) {
            return ::MayaFlux::Test::forma_call_triple(std::get<0>(values));
        } else if constexpr (K == 2) {
            return ::MayaFlux::Test::forma_call_triple(std::get<0>(values), std::get<1>(values));
        } else if constexpr (K == 3) {
            return ::MayaFlux::Test::forma_call_triple(std::get<0>(values), std::get<1>(values), std::get<2>(values));
        } else {
            static_assert(K != K, "the sidecar has no call with that many arguments");
        }
    }
};

}

#endif
