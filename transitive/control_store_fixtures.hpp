#pragma once

#include "MayaFlux/Nexus/Quantity.hpp"
#include "MayaFlux/Transitive/Facet/ControlSnapshot.hpp"
#include "MayaFlux/Transitive/Facet/ControlValue.hpp"
#include "MayaFlux/Transitive/Reflect/Describe.hpp"

namespace MayaFlux::Test {

struct StoreBloomTuning {
    ControlValue rate;
    ControlValue angle;
};

MF_DESCRIBE(StoreBloomTuning)

struct StoreMixedTuning {
    ControlValue gain;
    ControlSnapshot<std::string> label;
};

MF_DESCRIBE(StoreMixedTuning)

struct StoreQuantityTuning {
    Nexus::Quantity position;
    Nexus::Quantity intensity;
    Nexus::Quantity orientation;
};

MF_DESCRIBE(StoreQuantityTuning)

struct StoreScaleExtension {
    ControlValue scale;
};

MF_DESCRIBE(StoreScaleExtension)

}

#include "control_store_fixtures.describe.hpp"
