#include "gtest/gtest.h"
#include "../../support/test_metadata.hpp"

#include "MayaFlux/Portal/Graphics/GraphicsUtils.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using ElementType = Portal::Graphics::GpuBufferBinding::ElementType;
    using Portal::Graphics::element_type_bytes;

    TEST(GraphicsElementTypeTest, Float32IsFourBytes)
    {
        EXPECT_EQ(element_type_bytes(ElementType::FLOAT32), 4U);
    }

    TEST(GraphicsElementTypeTest, IntegerTypesAreFourBytes)
    {
        EXPECT_EQ(element_type_bytes(ElementType::UINT32), 4U);
        EXPECT_EQ(element_type_bytes(ElementType::INT32), 4U);
    }

    TEST(GraphicsElementTypeTest, Vec2U32IsEightBytes)
    {
        EXPECT_EQ(element_type_bytes(ElementType::VEC2_U32), 8U);
    }

    TEST(GraphicsElementTypeTest, Float64IsEightBytes)
    {
        EXPECT_EQ(element_type_bytes(ElementType::FLOAT64), 8U);
    }

    TEST(GraphicsElementTypeTest, Float64IsTheSizeOfADouble)
    {
        EXPECT_EQ(element_type_bytes(ElementType::FLOAT64), sizeof(double));
    }

    TEST(GraphicsElementTypeTest, TypesWithoutAFixedElementSizeReportZero)
    {
        EXPECT_EQ(element_type_bytes(ElementType::PASSTHROUGH), 0U);
        EXPECT_EQ(element_type_bytes(ElementType::IMAGE_STORAGE), 0U);
        EXPECT_EQ(element_type_bytes(ElementType::IMAGE_SAMPLED), 0U);
    }

} // namespace
} // namespace MayaFlux::Test
