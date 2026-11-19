#pragma once

#include "MayaFlux/Transitive/Facet/ControlSnapshot.hpp"
#include "MayaFlux/Transitive/Facet/ControlValue.hpp"
#include "MayaFlux/Transitive/Reflect/Describe.hpp"

namespace MayaFlux::Test {

struct SlotFlatTuning {
    ControlValue scale;
    ControlValue decay;
};

MF_DESCRIBE(SlotFlatTuning)

struct SlotStickTuning {
    ControlValue deadzone;
    ControlValue invert;
};

MF_DESCRIBE(SlotStickTuning)

struct SlotPadTuning {
    ControlValue scale;
    SlotStickTuning left;
    SlotStickTuning right;
};

MF_DESCRIBE(SlotPadTuning)

struct SlotLabelTuning {
    ControlSnapshot<std::string> label;
    ControlSnapshot<std::string> note;
};

MF_DESCRIBE(SlotLabelTuning)

}

#include "control_value_fixtures.describe.hpp"
