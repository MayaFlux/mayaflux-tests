#pragma once

#include "MayaFlux/Nexus/Principals/Agent.hpp"
#include "MayaFlux/Transitive/Reflect/Describe.hpp"

namespace MayaFlux::Test {

/** @brief What the functions made below added up when they ran. */
inline int& state_call_total()
{
    static int total = 0;
    return total;
}

/** @brief An influence that adds its amount. */
inline std::function<void(const Nexus::InfluenceContext&)> state_call_influence(int amount)
{
    return [amount](const Nexus::InfluenceContext&) { state_call_total() += amount; };
}

/** @brief A plain callback that adds its amount. */
inline std::function<void()> state_call_note(int amount)
{
    return [amount] { state_call_total() += amount; };
}

/** @brief A scroll function that adds its amount. */
inline std::function<void(double, double)> state_call_scrolled(int amount)
{
    return [amount](double, double) { state_call_total() += amount; };
}

/** @brief A setup that adds a typed influence delivering its amount. */
inline std::function<void(Nexus::Agent&)> state_call_typed(int amount)
{
    return [amount](Nexus::Agent& agent) {
        agent.add_influence<int>([amount](const Nexus::InfluenceContext&) { return amount; })
            ->add_target([](const int& value) { state_call_total() += value; });
    };
}

}

MF_DESCRIBE_FUNCTION(&MayaFlux::Test::state_call_typed)
MF_DESCRIBE_FUNCTION(&MayaFlux::Test::state_call_influence)
MF_DESCRIBE_FUNCTION(&MayaFlux::Test::state_call_note)
MF_DESCRIBE_FUNCTION(&MayaFlux::Test::state_call_scrolled)

#include "state_call_support.signatures.hpp"
