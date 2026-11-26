#pragma once

#include "MayaFlux/Transitive/Reflect/Describe.hpp"

#ifndef MAYAFLUX_REFLECT_SCAN

namespace MayaFlux::Reflect::Generated {

template <>
struct Signature<&::MayaFlux::Test::state_call_influence> {
    static constexpr std::string_view name = "MayaFlux::Test::state_call_influence";
    static constexpr std::size_t required = 1;
    static constexpr std::array<std::string_view, 1> parameters { "amount" };

    template <std::size_t K, typename Values>
    static decltype(auto) call(Values& values)
    {
        if constexpr (K == 1) {
            return ::MayaFlux::Test::state_call_influence(std::get<0>(values));
        } else {
            static_assert(K != K, "the sidecar has no call with that many arguments");
        }
    }
};

template <>
struct Signature<&::MayaFlux::Test::state_call_note> {
    static constexpr std::string_view name = "MayaFlux::Test::state_call_note";
    static constexpr std::size_t required = 1;
    static constexpr std::array<std::string_view, 1> parameters { "amount" };

    template <std::size_t K, typename Values>
    static decltype(auto) call(Values& values)
    {
        if constexpr (K == 1) {
            return ::MayaFlux::Test::state_call_note(std::get<0>(values));
        } else {
            static_assert(K != K, "the sidecar has no call with that many arguments");
        }
    }
};

template <>
struct Signature<&::MayaFlux::Test::state_call_scrolled> {
    static constexpr std::string_view name = "MayaFlux::Test::state_call_scrolled";
    static constexpr std::size_t required = 1;
    static constexpr std::array<std::string_view, 1> parameters { "amount" };

    template <std::size_t K, typename Values>
    static decltype(auto) call(Values& values)
    {
        if constexpr (K == 1) {
            return ::MayaFlux::Test::state_call_scrolled(std::get<0>(values));
        } else {
            static_assert(K != K, "the sidecar has no call with that many arguments");
        }
    }
};

template <>
struct Signature<&::MayaFlux::Test::state_call_typed> {
    static constexpr std::string_view name = "MayaFlux::Test::state_call_typed";
    static constexpr std::size_t required = 1;
    static constexpr std::array<std::string_view, 1> parameters { "amount" };

    template <std::size_t K, typename Values>
    static decltype(auto) call(Values& values)
    {
        if constexpr (K == 1) {
            return ::MayaFlux::Test::state_call_typed(std::get<0>(values));
        } else {
            static_assert(K != K, "the sidecar has no call with that many arguments");
        }
    }
};

}

#endif
