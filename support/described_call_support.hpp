#pragma once

#include "MayaFlux/Transitive/Reflect/Describe.hpp"

namespace MayaFlux::Test {

/** @brief An object a document cannot write, only name. */
struct DescribedCallVoice {
    std::string label;
};

/** @brief Defaults, a live object and a callable, reported as text. */
inline std::string described_call_route(int count, float gain = 1.F,
    const std::shared_ptr<DescribedCallVoice>& voice = {}, const std::function<int()>& source = {})
{
    return std::to_string(count) + " " + std::to_string(static_cast<int>(gain * 10.F)) + " "
        + (voice ? voice->label : "none") + " " + (source ? std::to_string(source()) : "none");
}

}

MF_DESCRIBE_FUNCTION(&MayaFlux::Test::described_call_route)

#include "described_call_support.signatures.hpp"
