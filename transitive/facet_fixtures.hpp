#pragma once

#include "MayaFlux/Transitive/Reflect/Describe.hpp"

namespace MayaFlux::Test {

struct FacetFromSettings {
    uint32_t window { 512 };
    uint32_t hop { 256 };
    double threshold { 0.1 };
};

MF_DESCRIBE(FacetFromSettings)

struct FacetFromNothing {
};

MF_DESCRIBE(FacetFromNothing)

}
#include "facet_fixtures.describe.hpp"
