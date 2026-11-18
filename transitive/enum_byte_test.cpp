#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Transitive/Reflect/EnumReflect.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    enum class ByteTestFlags : uint8_t {
        None = 0,
        First = 1U << 0U,
        Second = 1U << 1U,
    };
    MF_BITMASK_OPERATORS(ByteTestFlags)

    enum class ByteTestMode : uint8_t {
        Alpha = 0,
        Beta = 1,
    };

    using FlagByte = Reflect::EnumByte<ByteTestFlags>;
    using ModeByte = Reflect::EnumByte<ByteTestMode>;

    struct FlagHolder {
        int32_t other;
        FlagByte flags;
    };

} // namespace

TEST(EnumByteTest, StoresTheEnumInItsUnderlyingType)
{
    static_assert(sizeof(FlagByte) == sizeof(uint8_t));

    const FlagByte value { ByteTestFlags::Second };

    EXPECT_EQ(value.raw, 2U);
    EXPECT_EQ(static_cast<ByteTestFlags>(value), ByteTestFlags::Second);
}

TEST(EnumByteTest, DefaultConstructedHoldsZero)
{
    const FlagByte flags;
    const ModeByte mode;

    EXPECT_EQ(flags.raw, 0U);
    EXPECT_EQ(static_cast<ByteTestFlags>(flags), ByteTestFlags::None);
    EXPECT_EQ(static_cast<ByteTestMode>(mode), ByteTestMode::Alpha);
}

TEST(EnumByteTest, AggregateWithAnEnumByteMemberValueInitialises)
{
    const FlagHolder holder {};

    EXPECT_EQ(holder.other, 0);
    EXPECT_EQ(static_cast<ByteTestFlags>(holder.flags), ByteTestFlags::None);
}

TEST(EnumByteTest, LiteralZeroConvertsForAFlagEnum)
{
    constexpr FlagByte constant = 0;
    FlagByte assigned { ByteTestFlags::First };
    assigned = 0;
    const FlagHolder holder { 7, 0 };

    EXPECT_EQ(constant.raw, 0U);
    EXPECT_EQ(assigned.raw, 0U);
    EXPECT_EQ(static_cast<ByteTestFlags>(holder.flags), ByteTestFlags::None);
    EXPECT_EQ(holder.other, 7);
}

TEST(EnumByteTest, LiteralZeroDoesNotConvertForAnEnumThatIsNotAFlagSet)
{
    static_assert(!std::is_constructible_v<ModeByte, int>);
    static_assert(!std::is_convertible_v<int, ModeByte>);
    static_assert(std::is_constructible_v<ModeByte, ByteTestMode>);
}

TEST(EnumByteTest, BitmaskOperatorsWorkThroughTheImplicitConversion)
{
    const FlagByte held { ByteTestFlags::First | ByteTestFlags::Second };

    EXPECT_TRUE(has_flag(held, ByteTestFlags::First));
    EXPECT_TRUE(has_flag(held, ByteTestFlags::First | ByteTestFlags::Second));
    EXPECT_EQ(held & ByteTestFlags::Second, ByteTestFlags::Second);
    EXPECT_NE(static_cast<ByteTestFlags>(held), ByteTestFlags::First);
}

TEST(EnumByteTest, AssignableFromTheEnumAndReadableBack)
{
    FlagByte value;
    value = ByteTestFlags::First | ByteTestFlags::Second;

    const ByteTestFlags read = value;

    EXPECT_EQ(read, ByteTestFlags::First | ByteTestFlags::Second);
}

}
