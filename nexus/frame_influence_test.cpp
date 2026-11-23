#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kakshya/FrameSource.hpp"
#include "MayaFlux/Kinesis/Temporal/TimeSampling.hpp"
#include "MayaFlux/Kriya/Reflow.hpp"
#include "MayaFlux/Nexus/Pheme/FrameInfluence.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Kakshya::ResidentFrames;
    using Kriya::FrameContext;
    using Kriya::Reflow;
    using Kriya::ReflowBuilder;
    using Nexus::FrameInfluenceContext;
    using Nexus::Influence;

    using IntInfluence = Influence<int, FrameInfluenceContext<int>>;

    constexpr double k_influence_rate = 8.0;

    /** Frame i holds the value 10 * i. */
    std::shared_ptr<const ResidentFrames<int>> influence_numbers(size_t count)
    {
        std::vector<int> frames;
        for (size_t i = 0; i < count; ++i) {
            frames.push_back(static_cast<int>(i) * 10);
        }
        auto sampling = Kinesis::TimeSampling::uniform(0.125);
        if (!sampling) {
            throw std::runtime_error("sampling was rejected");
        }
        return ResidentFrames<int>::create(*sampling, std::move(frames));
    }

    TEST(NexusFrameInfluenceTest, ASinkHandsEveryFrameAndItsCycleToTheProducerAndTheValueToEveryTarget)
    {
        const auto influence = std::make_shared<IntInfluence>(
            [](const FrameInfluenceContext<int>& context) { return *context.frame + static_cast<int>(context.context.cycle); });
        std::vector<int> first;
        std::vector<int> second;
        influence->add_target([&first](const int& value) { first.push_back(value); });
        influence->add_target([&second](const int& value) { second.push_back(value * 2); });

        const auto reflow = ReflowBuilder<int>(influence_numbers(8), Nexus::frame_sink(influence), k_influence_rate).build();
        ASSERT_NE(reflow, nullptr);
        for (int i = 0; i < 3; ++i) {
            reflow->advance();
        }

        EXPECT_EQ(first, (std::vector<int> { 0, 11, 22 }));
        EXPECT_EQ(second, (std::vector<int> { 0, 22, 44 }));
    }

    TEST(NexusFrameInfluenceTest, ADeriveOperationReadsFramesWithoutChangingWhatTheSinkSees)
    {
        const auto influence = std::make_shared<IntInfluence>(
            [](const FrameInfluenceContext<int>& context) { return *context.frame; });
        std::vector<int> derived;
        influence->add_target([&derived](const int& value) { derived.push_back(value); });

        std::vector<int> shown;
        const auto reflow = ReflowBuilder<int>(
            influence_numbers(8),
            [&shown](const std::shared_ptr<const int>& frame, const FrameContext&) { shown.push_back(*frame); },
            k_influence_rate)
                                .operation(Nexus::frame_derive(influence))
                                .build();
        ASSERT_NE(reflow, nullptr);
        for (int i = 0; i < 3; ++i) {
            reflow->advance();
        }

        EXPECT_EQ(derived, (std::vector<int> { 0, 10, 20 }));
        EXPECT_EQ(shown, derived);
    }

    TEST(NexusFrameInfluenceTest, ATargetMayCarryAFrameOutOfTheReflowIntoTheWorld)
    {
        const auto influence = std::make_shared<IntInfluence>(
            [](const FrameInfluenceContext<int>& context) { return *context.frame; });
        const auto cell = std::make_shared<int>(-1);
        influence->add_target([cell](const int& value) { *cell = value; });

        const auto reflow = ReflowBuilder<int>(influence_numbers(8), Nexus::frame_sink(influence), k_influence_rate).build();
        ASSERT_NE(reflow, nullptr);
        for (int i = 0; i < 4; ++i) {
            reflow->advance();
        }

        EXPECT_EQ(*cell, 30);
    }

    TEST(NexusFrameInfluenceTest, ANullInfluenceMakesASinkAndAnOperationThatDoNothing)
    {
        const std::shared_ptr<IntInfluence> none;
        const auto sink = Nexus::frame_sink(none);
        const auto derive = Nexus::frame_derive(none);

        EXPECT_TRUE(static_cast<bool>(sink));
        EXPECT_TRUE(derive.valid());

        const FrameContext context;
        EXPECT_NO_THROW(sink(std::make_shared<const int>(1), context));
    }

} // namespace
} // namespace MayaFlux::Test
