#include "gtest/gtest.h"

#include "MayaFlux/Portal/Forma/Link.hpp"

namespace MayaFlux::Test {

using Portal::Forma::Link;

static_assert(!std::is_copy_constructible_v<Link>);
static_assert(!std::is_copy_assignable_v<Link>);
static_assert(std::is_nothrow_move_constructible_v<Link>);

TEST(FormaLinkTest, EstablishRunsOnceDuringConstruction)
{
    int established = 0;

    Link link([&established] { ++established; });

    EXPECT_EQ(established, 1);
}

TEST(FormaLinkTest, TheDefaultTapDoesNothing)
{
    int established = 0;
    Link link([&established] { ++established; });

    link.tap();
    link.tap();

    EXPECT_EQ(established, 1);
}

TEST(FormaLinkTest, AGivenTapRunsEachTimeItIsCalledAndNotAtConstruction)
{
    int established = 0;
    int taps = 0;

    Link link([&established] { ++established; }, [&taps] { ++taps; });
    EXPECT_EQ(taps, 0);

    link.tap();
    link.tap();

    EXPECT_EQ(established, 1);
    EXPECT_EQ(taps, 2);
}

TEST(FormaLinkTest, AnEmptyEstablishIsAccepted)
{
    Link link({});

    EXPECT_FALSE(static_cast<bool>(link.establish));
    link.tap();
}

TEST(FormaLinkTest, MovingKeepsTheFunctionsAndDoesNotEstablishAgain)
{
    int established = 0;
    int taps = 0;
    Link original([&established] { ++established; }, [&taps] { ++taps; });

    Link moved(std::move(original));
    moved.tap();

    EXPECT_EQ(established, 1);
    EXPECT_EQ(taps, 1);
}

TEST(FormaLinkTest, MoveAssignmentReplacesTheFunctions)
{
    int a_taps = 0;
    int b_taps = 0;
    Link a({}, [&a_taps] { ++a_taps; });
    Link b({}, [&b_taps] { ++b_taps; });

    a = std::move(b);
    a.tap();

    EXPECT_EQ(a_taps, 0);
    EXPECT_EQ(b_taps, 1);
}

}
