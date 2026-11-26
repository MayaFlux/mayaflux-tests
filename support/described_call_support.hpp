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

/** @brief A callable made from arguments, which is what a table is for. */
inline std::function<int()> described_call_scaled(int value, int scale = 2)
{
    return [value, scale] { return value * scale; };
}

}

MF_DESCRIBE_FUNCTION(&MayaFlux::Test::described_call_route)
MF_DESCRIBE_FUNCTION(&MayaFlux::Test::described_call_scaled)

#include "described_call_support.signatures.hpp"
