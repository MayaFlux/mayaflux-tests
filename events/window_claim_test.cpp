#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "../support/scripted_window.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using IO::Keys;
    using IO::MouseButtons;

    using Covers = std::function<std::optional<float>(double, double)>;

    Covers everywhere(float distance = 0.F)
    {
        return [distance](double, double) { return std::optional<float>(distance); };
    }

    Covers left_of(double limit)
    {
        return [limit](double x, double) { return x < limit ? std::optional<float>(0.F) : std::nullopt; };
    }

}

class WindowClaimTest : public ::testing::Test {
protected:
    ScriptedWindow window { 200U, 100U };
    Vruta::WindowEventSource& source = window.get_event_source();
};

TEST_F(WindowClaimTest, TheHigherPriorityTakesWhatBothCoverAndTheLaterWinsATie)
{
    const uint32_t low = source.claim(0, everywhere());
    const uint32_t high = source.claim(5, left_of(100.0));

    window.move_to_pixel(50.0, 50.0);
    EXPECT_TRUE(source.owns_pointer(high));
    EXPECT_FALSE(source.owns_pointer(low));

    window.move_to_pixel(150.0, 50.0);
    EXPECT_TRUE(source.owns_pointer(low));

    const uint32_t tied = source.claim(0, everywhere());
    window.move_to_pixel(150.0, 50.0);
    EXPECT_TRUE(source.owns_pointer(tied));
}

TEST_F(WindowClaimTest, APressKeepsThePointerUntilEveryButtonIsUp)
{
    const uint32_t left = source.claim(0, left_of(100.0));
    const uint32_t all = source.claim(-1, everywhere());

    window.move_to_pixel(50.0, 50.0);
    window.press(MouseButtons::Left);
    window.press(MouseButtons::Right);
    window.move_to_pixel(150.0, 50.0);
    EXPECT_TRUE(source.owns_pointer(left));

    window.release(MouseButtons::Left);
    window.scroll(0.0, 1.0);
    EXPECT_TRUE(source.owns_pointer(left));

    window.release(MouseButtons::Right);
    window.move_to_pixel(150.0, 50.0);
    EXPECT_TRUE(source.owns_pointer(all));
}

TEST_F(WindowClaimTest, AfterAPressNothingCoversTheFirstClaimReachedKeepsThePointer)
{
    const uint32_t left = source.claim(0, left_of(100.0));

    window.move_to_pixel(150.0, 50.0);
    window.press(MouseButtons::Left);
    EXPECT_FALSE(source.owns_pointer(left));

    window.move_to_pixel(50.0, 50.0);
    EXPECT_TRUE(source.owns_pointer(left));

    window.move_to_pixel(150.0, 50.0);
    EXPECT_TRUE(source.owns_pointer(left));

    window.release(MouseButtons::Left);
    window.move_to_pixel(150.0, 50.0);
    EXPECT_FALSE(source.owns_pointer(left));
}

TEST_F(WindowClaimTest, WithdrawingTheKeeperMidPressFreesThePointer)
{
    const uint32_t all = source.claim(0, everywhere());
    const uint32_t keeper = source.claim(5, everywhere());

    window.move_to_pixel(50.0, 50.0);
    window.press(MouseButtons::Left);
    source.unclaim(keeper);
    EXPECT_FALSE(source.owns_pointer(keeper));

    window.move_to_pixel(60.0, 50.0);
    EXPECT_TRUE(source.owns_pointer(all));
}

TEST_F(WindowClaimTest, SettingAPriorityAlsoPutsAClaimAheadOfItsEquals)
{
    const uint32_t first = source.claim(0, everywhere());
    const uint32_t second = source.claim(0, everywhere());

    window.move_to_pixel(50.0, 50.0);
    EXPECT_TRUE(source.owns_pointer(second));

    source.set_claim_priority(first, 0);
    window.move_to_pixel(50.0, 50.0);
    EXPECT_TRUE(source.owns_pointer(first));
}

TEST_F(WindowClaimTest, KeysGoToTheHighestClaimThatWantsThatKey)
{
    const uint32_t camera = source.claim(-10, everywhere(), [](Keys key) { return key == Keys::W || key == Keys::A; });
    const uint32_t field = source.claim(0, everywhere(), [](Keys key) { return key == Keys::A; });

    window.key_down(Keys::W);
    EXPECT_TRUE(source.owns_key(camera));

    window.key_down(Keys::A);
    EXPECT_TRUE(source.owns_key(field));
    EXPECT_FALSE(source.owns_key(camera));

    window.key_down(Keys::B);
    EXPECT_FALSE(source.owns_key(camera));
    EXPECT_FALSE(source.owns_key(field));
}

TEST_F(WindowClaimTest, TheOwnerIsSettledBeforeAnyListenerRuns)
{
    bool covers = true;
    const uint32_t id = source.claim(0, [&covers](double, double) {
        return covers ? std::optional<float>(0.F) : std::nullopt;
    });
    window.move_to_pixel(50.0, 50.0);

    covers = false;

    EXPECT_TRUE(source.owns_pointer(id));
}

TEST_F(WindowClaimTest, AmongEqualPrioritiesTheNearerWinsWhateverTheOrder)
{
    const uint32_t near_one = source.claim(0, everywhere(2.F));
    const uint32_t far_one = source.claim(0, everywhere(9.F));

    window.move_to_pixel(50.0, 50.0);

    EXPECT_TRUE(source.owns_pointer(near_one));
    EXPECT_FALSE(source.owns_pointer(far_one));
}

TEST_F(WindowClaimTest, PriorityIsSettledBeforeDistance)
{
    const uint32_t near_one = source.claim(0, everywhere(1.F));
    const uint32_t ranked = source.claim(1, everywhere(100.F));

    window.move_to_pixel(50.0, 50.0);

    EXPECT_TRUE(source.owns_pointer(ranked));
    EXPECT_FALSE(source.owns_pointer(near_one));
}

TEST_F(WindowClaimTest, NoClaimIdIsZeroAndZeroOwnsNothing)
{
    window.move_to_pixel(50.0, 50.0);

    EXPECT_FALSE(source.owns_pointer(0));
    EXPECT_NE(source.claim(0, everywhere()), 0U);
}

}
