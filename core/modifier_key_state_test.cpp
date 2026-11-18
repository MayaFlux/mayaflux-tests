#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Core/Backends/Windowing/ModifierKeyState.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Core::ModifierKeyState;
    using IO::Keys;
    using IO::Modifiers;

    struct SideCase {
        const char* name;
        Keys left;
        Keys right;
        Modifiers flag;
    };

    class ModifierKeyStateSideTest : public ::testing::TestWithParam<SideCase> { };

    MAYAFLUX_TEST_WITH(TEST_P, ModifierKeyStateSideTest, EitherSideSetsTheFamilyBit, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        const auto& param = GetParam();

        for (const Keys key : { param.left, param.right }) {
            ModifierKeyState state;
            state.set(key, true);
            EXPECT_EQ(state.held(), param.flag);
            state.set(key, false);
            EXPECT_EQ(state.held(), Modifiers::None);
        }
    }

    MAYAFLUX_TEST_WITH(TEST_P, ModifierKeyStateSideTest, FamilyStaysSetUntilBothSidesAreUp, (::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))
    {
        const auto& param = GetParam();
        ModifierKeyState state;

        state.set(param.left, true);
        state.set(param.right, true);
        state.set(param.left, false);
        EXPECT_EQ(state.held(), param.flag);
        state.set(param.right, false);
        EXPECT_EQ(state.held(), Modifiers::None);
    }

    INSTANTIATE_TEST_SUITE_P(
        ModifierKeyState,
        ModifierKeyStateSideTest,
        ::testing::Values(
            SideCase { "Shift", Keys::LShift, Keys::RShift, Modifiers::Shift },
            SideCase { "Ctrl", Keys::LCtrl, Keys::RCtrl, Modifiers::Ctrl },
            SideCase { "Alt", Keys::LAlt, Keys::RAlt, Modifiers::Alt },
            SideCase { "Super", Keys::LSuper, Keys::RSuper, Modifiers::Super }),
        [](const ::testing::TestParamInfo<SideCase>& info) { return std::string(info.param.name); });

    TEST(ModifierKeyStateTest, FamiliesCombineIndependently)
    {
        ModifierKeyState state;

        state.set(Keys::LCtrl, true);
        state.set(Keys::RShift, true);
        EXPECT_EQ(state.held(), Modifiers::Ctrl | Modifiers::Shift);

        state.set(Keys::LCtrl, false);
        EXPECT_EQ(state.held(), Modifiers::Shift);
    }

    TEST(ModifierKeyStateTest, NonModifierKeysAreIgnored)
    {
        ModifierKeyState state;

        for (const Keys key : { Keys::A, Keys::Space, Keys::CapsLock, Keys::Menu, Keys::Unknown }) {
            state.set(key, true);
            EXPECT_EQ(state.held(), Modifiers::None) << static_cast<int>(key);
        }
    }

    TEST(ModifierKeyStateTest, ClearReleasesEverything)
    {
        ModifierKeyState state;
        state.set(Keys::LShift, true);
        state.set(Keys::RAlt, true);
        state.set(Keys::LSuper, true);

        state.clear();

        EXPECT_EQ(state.held(), Modifiers::None);
    }

    TEST(ModifiersTest, HasModifiersRequiresEveryRequestedBit)
    {
        const auto mods = static_cast<int32_t>(Modifiers::Ctrl | Modifiers::Shift);

        EXPECT_TRUE(IO::has_modifiers(mods, Modifiers::Ctrl));
        EXPECT_TRUE(IO::has_modifiers(mods, Modifiers::Ctrl | Modifiers::Shift));
        EXPECT_FALSE(IO::has_modifiers(mods, Modifiers::Ctrl | Modifiers::Alt));
        EXPECT_FALSE(IO::has_modifiers(mods, Modifiers::Super));
        EXPECT_TRUE(IO::has_modifiers(mods, Modifiers::None));
    }

    TEST(ModifiersTest, BitsMatchTheValuesBackendsAlreadyEmit)
    {
        EXPECT_EQ(static_cast<int>(Modifiers::Shift), 0x01);
        EXPECT_EQ(static_cast<int>(Modifiers::Ctrl), 0x02);
        EXPECT_EQ(static_cast<int>(Modifiers::Alt), 0x04);
        EXPECT_EQ(static_cast<int>(Modifiers::Super), 0x08);
        EXPECT_EQ(static_cast<int>(Modifiers::CapsLock), 0x10);
        EXPECT_EQ(static_cast<int>(Modifiers::NumLock), 0x20);
    }

}
}
