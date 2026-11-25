#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kriya/Course.hpp"
#include "MayaFlux/Nexus/Expanse.hpp"
#include "MayaFlux/Transitive/Facet/ControlBind.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Kinesis::AABB3D;
    using Kriya::Course;
    using Kriya::CourseSetting;
    using Nexus::CrossingContext;
    using Nexus::Expanse;

    /** Eight steps a second, so every step is an exact eighth of a second. */
    constexpr double k_course_rate = 8.0;

    std::shared_ptr<Course> course_over(double in, double out, bool paused)
    {
        CourseSetting setting;
        setting.in = in;
        setting.out = out;
        return Course::create(k_course_rate, setting, paused);
    }

    /** One entity at @p p, as a Fabric hands it to an Expanse on commit. */
    void visit(Expanse& region, const glm::vec3& p)
    {
        const std::vector<std::pair<uint32_t, glm::vec3>> snapshot { { 7U, p } };
        region.evaluate(1U, snapshot);
    }

    const glm::vec3 k_inside { 5.0F, 0.0F, 0.0F };
    const glm::vec3 k_outside { 50.0F, 0.0F, 0.0F };

    class NexusCourseHookTest : public ::testing::Test {
    protected:
        Expanse region { AABB3D { .min = { 0.0F, -1.0F, -1.0F }, .max = { 10.0F, 1.0F, 1.0F } } };

        static bool yes(const CrossingContext&) { return true; }
    };

    TEST_F(NexusCourseHookTest, ARoomRunsACourseOnlyWhileItIsOccupied)
    {
        const auto walk = course_over(0.0, 10.0, true);
        ASSERT_NE(walk, nullptr);
        region.add_entry<bool>(yes)->add_target(call_writer<&Course::start, bool>(walk));
        region.add_exit<bool>(yes)->add_target(call_writer<&Course::pause, bool>(walk));

        visit(region, k_outside);
        ASSERT_TRUE(walk->advance().has_value());
        EXPECT_TRUE(walk->paused());
        EXPECT_DOUBLE_EQ(walk->time(), 0.0);

        visit(region, k_inside);
        ASSERT_TRUE(walk->advance().has_value());
        visit(region, k_inside);
        ASSERT_TRUE(walk->advance().has_value());
        EXPECT_FALSE(walk->paused());
        EXPECT_DOUBLE_EQ(walk->time(), 0.25);

        visit(region, k_outside);
        ASSERT_TRUE(walk->advance().has_value());
        ASSERT_TRUE(walk->advance().has_value());
        EXPECT_TRUE(walk->paused());
        EXPECT_DOUBLE_EQ(walk->time(), 0.25);
    }

    TEST_F(NexusCourseHookTest, OneEntryRunsCommandsControlsAndAnythingElseTogether)
    {
        const auto walk = course_over(0.0, 10.0, false);
        const auto smoke = course_over(0.0, 10.0, true);
        ASSERT_NE(walk, nullptr);
        ASSERT_NE(smoke, nullptr);
        ASSERT_TRUE(walk->advance().has_value());
        ASSERT_TRUE(walk->advance().has_value());
        ASSERT_DOUBLE_EQ(walk->time(), 0.25);

        int others {};
        region.add_entry<bool>(yes)
            ->add_target(call_writer<&Course::retrigger, bool>(walk))
            .add_target(call_writer<&Course::start, bool>(smoke))
            .add_target([&others](const bool&) { ++others; });
        region.add_entry<double>([](const CrossingContext&) { return 2.0; })
            ->add_target(control_writer<&Kriya::CourseControls::speed, double>(smoke));

        visit(region, k_inside);
        ASSERT_TRUE(walk->advance().has_value());
        ASSERT_TRUE(smoke->advance().has_value());

        EXPECT_DOUBLE_EQ(walk->time(), 0.125);
        EXPECT_DOUBLE_EQ(smoke->time(), 0.25);
        EXPECT_EQ(others, 1);
    }

    TEST_F(NexusCourseHookTest, ACorridorMakesPositionThePlayhead)
    {
        const auto recording = course_over(2.0, 6.0, true);
        ASSERT_NE(recording, nullptr);
        region.add_inside<double>([](const CrossingContext& ctx) {
                  return static_cast<double>(ctx.position->x) / 10.0;
              })
            ->add_target(call_writer<&Course::seek_normalized, double>(recording));

        visit(region, { 2.5F, 0.0F, 0.0F });
        ASSERT_TRUE(recording->advance().has_value());
        EXPECT_DOUBLE_EQ(recording->time(), 3.0);

        visit(region, { 7.5F, 0.0F, 0.0F });
        ASSERT_TRUE(recording->advance().has_value());
        EXPECT_DOUBLE_EQ(recording->time(), 5.0);

        visit(region, k_outside);
        ASSERT_TRUE(recording->advance().has_value());
        EXPECT_DOUBLE_EQ(recording->time(), 5.0);
        EXPECT_TRUE(recording->paused());
    }

    TEST_F(NexusCourseHookTest, ACommandThatTakesAValueIsGivenIt)
    {
        const auto recording = course_over(0.0, 10.0, true);
        ASSERT_NE(recording, nullptr);
        ASSERT_TRUE(recording->add_marker("door", 4.0).has_value());

        const auto jump = call_writer<&Course::jump, std::string>(recording);
        jump("door");
        ASSERT_TRUE(recording->advance().has_value());

        EXPECT_DOUBLE_EQ(recording->time(), 4.0);
    }

}
}
