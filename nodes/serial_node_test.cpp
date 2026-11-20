#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Nodes/Input/SerialNode.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Core::InputType;
    using Core::InputValue;
    using Nodes::Input::SerialConfig;
    using Nodes::Input::SerialNode;
    using Nodes::Input::SmoothingMode;

    SerialConfig unsmoothed(SerialConfig config)
    {
        config.smoothing = SmoothingMode::NONE;
        return config;
    }

    void feed(SerialNode& node, std::vector<double> numbers)
    {
        node.process_input(InputValue::make_vector(std::move(numbers), 3, InputType::SERIAL));
    }

    TEST(SerialNodeTest, FollowsTheFirstNumberByDefault)
    {
        SerialNode node(unsmoothed(SerialConfig::value()));

        feed(node, { 3, 5 });

        EXPECT_DOUBLE_EQ(node.get_target_value(), 3.0);
        EXPECT_DOUBLE_EQ(node.get_current_value(), 3.0);
    }

    TEST(SerialNodeTest, TheChannelChoosesWhichNumberIsFollowed)
    {
        SerialNode node(unsmoothed(SerialConfig::at(1)));

        feed(node, { 3, 5, 7 });

        EXPECT_DOUBLE_EQ(node.get_target_value(), 5.0);
    }

    TEST(SerialNodeTest, AMessageWithTooFewNumbersLeavesTheOutputWhereItWas)
    {
        SerialNode node(unsmoothed(SerialConfig::at(2)));
        feed(node, { 1, 2, 3 });
        ASSERT_DOUBLE_EQ(node.get_current_value(), 3.0);

        feed(node, { 9 });
        feed(node, {});

        EXPECT_DOUBLE_EQ(node.get_current_value(), 3.0);
    }

    TEST(SerialNodeTest, NormalizedMapsTheRangeToUnitAndClamps)
    {
        SerialNode node(unsmoothed(SerialConfig::normalized(0.0, 1023.0, 0)));

        feed(node, { 511.5 });
        EXPECT_NEAR(node.get_current_value(), 0.5, 1e-12);
        feed(node, { 5000 });
        EXPECT_DOUBLE_EQ(node.get_current_value(), 1.0);
        feed(node, { -40 });
        EXPECT_DOUBLE_EQ(node.get_current_value(), 0.0);
    }

    TEST(SerialNodeTest, ADegenerateRangeIsNotUsedToNormalize)
    {
        SerialNode node(unsmoothed(SerialConfig::normalized(5.0, 5.0, 0)));

        feed(node, { 7 });

        EXPECT_DOUBLE_EQ(node.get_current_value(), 7.0);
    }

    TEST(SerialNodeTest, ScalarAndMidiValuesFallBackToTheInputNodeRules)
    {
        SerialNode node(unsmoothed(SerialConfig::value()));

        node.process_input(InputValue::make_scalar(0.25, 3, InputType::SERIAL));
        EXPECT_DOUBLE_EQ(node.get_current_value(), 0.25);

        node.process_input(InputValue::make_midi(0x90, 60, 127, 3));
        EXPECT_DOUBLE_EQ(node.get_current_value(), 1.0);
    }

    TEST(SerialNodeTest, BytesLeaveTheOutputWhereItWas)
    {
        SerialNode node(unsmoothed(SerialConfig::value()));
        feed(node, { 4 });

        node.process_input(InputValue::make_bytes({ 1, 2, 3 }, 3, InputType::SERIAL));

        EXPECT_DOUBLE_EQ(node.get_current_value(), 4.0);
    }

    TEST(SerialNodeTest, TheSmoothingOfTheConfigReachesTheNode)
    {
        SerialConfig config = SerialConfig::value();
        config.smoothing = SmoothingMode::EXPONENTIAL;
        config.smoothing_factor = 0.5;
        SerialNode node(config);

        feed(node, { 10 });

        EXPECT_DOUBLE_EQ(node.get_target_value(), 10.0);
        EXPECT_NEAR(node.get_current_value(), 5.0, 1e-12);
    }

    TEST(SerialNodeTest, TheLastInputIsKeptWhole)
    {
        SerialNode node(unsmoothed(SerialConfig::at(1)));

        feed(node, { 1, 2, 3 });

        const auto last = node.get_last_input();
        ASSERT_TRUE(last.has_value());
        EXPECT_EQ(last->as_vector(), (std::vector<double> { 1, 2, 3 }));
        EXPECT_EQ(last->device_id, 3U);
    }

} // namespace
} // namespace MayaFlux::Test
