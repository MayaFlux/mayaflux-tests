#include "gtest/gtest.h"

#include "MayaFlux/Nexus/Pheme/Influence.hpp"
#include "MayaFlux/Nexus/Pheme/Perception.hpp"

namespace MayaFlux::Test {
namespace {

    using Nexus::Influence;
    using Nexus::InfluenceBlock;
    using Nexus::InfluenceContext;
    using Nexus::Perception;
    using Nexus::PerceptionContext;

    TEST(NexusInfluenceTest, ProducerRunsOncePerInvokeAndEveryTargetGetsTheValue)
    {
        int produced {};
        Influence<int> influence([&produced](const InfluenceContext&) { return ++produced; });

        std::vector<int> first;
        std::vector<int> second;
        influence.add_target([&first](const int& v) { first.push_back(v); });
        influence.add_target([&second](const int& v) { second.push_back(v); });

        influence.invoke({});
        influence.invoke({});

        EXPECT_EQ(produced, 2);
        EXPECT_EQ(first, (std::vector<int> { 1, 2 }));
        EXPECT_EQ(second, (std::vector<int> { 1, 2 }));
    }

    TEST(NexusInfluenceTest, TargetsRunInInsertionOrder)
    {
        Influence<int> influence([](const InfluenceContext&) { return 0; });
        std::vector<int> order;
        for (int i = 0; i < 4; ++i) {
            influence.add_target([&order, i](const int&) { order.push_back(i); });
        }

        influence.invoke({});

        EXPECT_EQ(order, (std::vector<int> { 0, 1, 2, 3 }));
    }

    TEST(NexusInfluenceTest, AddTargetChainsAndCountsTargets)
    {
        Influence<int> influence([](const InfluenceContext&) { return 0; });
        EXPECT_EQ(influence.target_count(), 0U);

        influence.add_target([](const int&) { }).add_target([](const int&) { });
        EXPECT_EQ(influence.target_count(), 2U);

        influence.clear_targets();
        EXPECT_EQ(influence.target_count(), 0U);
    }

    TEST(NexusInfluenceTest, ClearedTargetsStopReceivingButProducerStillRuns)
    {
        int produced {};
        int received {};
        Influence<int> influence([&produced](const InfluenceContext&) { return ++produced; });
        influence.add_target([&received](const int&) { ++received; });

        influence.invoke({});
        influence.clear_targets();
        influence.invoke({});

        EXPECT_EQ(produced, 2);
        EXPECT_EQ(received, 1);
    }

    TEST(NexusInfluenceTest, EmptyProducerInvokesNothing)
    {
        Influence<int> influence(nullptr);
        int received {};
        influence.add_target([&received](const int&) { ++received; });

        influence.invoke({});

        EXPECT_EQ(received, 0);
    }

    TEST(NexusInfluenceTest, ProducerReadsTheContext)
    {
        Influence<glm::vec3> influence([](const InfluenceContext& ctx) { return ctx.position * ctx.intensity; });
        glm::vec3 received {};
        influence.add_target([&received](const glm::vec3& v) { received = v; });

        InfluenceContext ctx;
        ctx.position = { 1.0F, 2.0F, 3.0F };
        ctx.intensity = 2.0F;
        influence.invoke(ctx);

        EXPECT_EQ(received, glm::vec3(2.0F, 4.0F, 6.0F));
    }

    TEST(NexusInfluenceBlockTest, FromFillsDefaultsWhereTheContextHasNone)
    {
        InfluenceContext ctx;
        ctx.position = { 1.0F, 2.0F, 3.0F };
        ctx.intensity = 0.25F;
        ctx.radius = 4.0F;

        const InfluenceBlock block = InfluenceBlock::from(ctx);

        EXPECT_EQ(block.position, ctx.position);
        EXPECT_FLOAT_EQ(block.intensity, 0.25F);
        EXPECT_FLOAT_EQ(block.radius, 4.0F);
        EXPECT_EQ(block.color, glm::vec3(1.0F));
        EXPECT_FLOAT_EQ(block.size, 1.0F);
    }

    TEST(NexusInfluenceBlockTest, FromCarriesColorAndSizeWhenSet)
    {
        InfluenceContext ctx;
        ctx.color = glm::vec3(0.1F, 0.2F, 0.3F);
        ctx.size = 7.0F;

        const InfluenceBlock block = InfluenceBlock::from(ctx);

        EXPECT_EQ(block.color, glm::vec3(0.1F, 0.2F, 0.3F));
        EXPECT_FLOAT_EQ(block.size, 7.0F);
    }

    TEST(NexusInfluenceBlockTest, LayoutMatchesTheShaderBlock)
    {
        EXPECT_EQ(sizeof(InfluenceBlock), 48U);
        EXPECT_EQ(alignof(InfluenceBlock), 16U);
        EXPECT_EQ(offsetof(InfluenceBlock, position), 0U);
        EXPECT_EQ(offsetof(InfluenceBlock, intensity), 12U);
        EXPECT_EQ(offsetof(InfluenceBlock, color), 16U);
        EXPECT_EQ(offsetof(InfluenceBlock, radius), 28U);
        EXPECT_EQ(offsetof(InfluenceBlock, size), 32U);
    }

    TEST(NexusPerceptionTest, SourceRunsOncePerInvokeAndEveryTargetGetsTheValue)
    {
        int sensed {};
        Perception<int> perception([&sensed](const PerceptionContext&) { return ++sensed; });

        std::vector<int> first;
        std::vector<int> second;
        perception.add_target([&first](const int& v) { first.push_back(v); });
        perception.add_target([&second](const int& v) { second.push_back(v); });

        perception.invoke({});
        perception.invoke({});

        EXPECT_EQ(sensed, 2);
        EXPECT_EQ(first, (std::vector<int> { 1, 2 }));
        EXPECT_EQ(second, (std::vector<int> { 1, 2 }));
    }

    TEST(NexusPerceptionTest, TargetsRunInInsertionOrder)
    {
        Perception<int> perception([](const PerceptionContext&) { return 0; });
        std::vector<int> order;
        for (int i = 0; i < 4; ++i) {
            perception.add_target([&order, i](const int&) { order.push_back(i); });
        }

        perception.invoke({});

        EXPECT_EQ(order, (std::vector<int> { 0, 1, 2, 3 }));
    }

    TEST(NexusPerceptionTest, AddTargetChainsAndCountsTargets)
    {
        Perception<int> perception([](const PerceptionContext&) { return 0; });
        EXPECT_EQ(perception.target_count(), 0U);

        perception.add_target([](const int&) { }).add_target([](const int&) { });
        EXPECT_EQ(perception.target_count(), 2U);

        perception.clear_targets();
        EXPECT_EQ(perception.target_count(), 0U);
    }

    TEST(NexusPerceptionTest, EmptySourceInvokesNothing)
    {
        Perception<int> perception(nullptr);
        int received {};
        perception.add_target([&received](const int&) { ++received; });

        perception.invoke({});

        EXPECT_EQ(received, 0);
    }

    TEST(NexusPerceptionTest, SourceReadsNeighboursFromTheContext)
    {
        Perception<size_t> perception([](const PerceptionContext& ctx) { return ctx.spatial_results.size(); });
        size_t received {};
        perception.add_target([&received](const size_t& n) { received = n; });

        const std::vector<Kinesis::QueryResult> neighbours {
            { .id = 1, .distance_sq = 0.5F },
            { .id = 2, .distance_sq = 1.5F },
            { .id = 3, .distance_sq = 2.5F },
        };
        PerceptionContext ctx;
        ctx.spatial_results = neighbours;
        perception.invoke(ctx);

        EXPECT_EQ(received, 3U);
    }

    TEST(NexusPerceptionTest, ContextDefaultsHaveNoNeighboursAndUnitRadius)
    {
        const PerceptionContext ctx;

        EXPECT_TRUE(ctx.spatial_results.empty());
        EXPECT_FLOAT_EQ(ctx.radius, 1.0F);
        EXPECT_FALSE(ctx.orientation.has_value());
    }

}
}
