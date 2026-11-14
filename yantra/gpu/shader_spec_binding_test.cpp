#include "gtest/gtest.h"
#include "../../support/test_metadata.hpp"

#include "MayaFlux/Yantra/Executors/ShaderSpecBinding.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using ElementType = Portal::Graphics::GpuBufferBinding::ElementType;
    using Kakshya::DataModality;
    using Kakshya::GpuDataFormat;
    using Yantra::to_element_type;

    TEST(ShaderSpecBindingTest, Float64MapsToTheFloat64ElementNotToFloat32)
    {
        EXPECT_EQ(to_element_type(DataModality::SCALAR_F32, GpuDataFormat::FLOAT64), ElementType::FLOAT64);
    }

    TEST(ShaderSpecBindingTest, Float32StillMapsToFloat32)
    {
        EXPECT_EQ(to_element_type(DataModality::SCALAR_F32, GpuDataFormat::FLOAT32), ElementType::FLOAT32);
    }

    TEST(ShaderSpecBindingTest, Vec2U32StillMapsToVec2U32)
    {
        EXPECT_EQ(to_element_type(DataModality::SCALAR_F32, GpuDataFormat::VEC2_U32), ElementType::VEC2_U32);
    }

} // namespace
} // namespace MayaFlux::Test
