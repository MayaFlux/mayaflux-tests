#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kinesis/Spatial/ProximityGraphs.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

namespace {

    using Kinesis::EdgeList;

    Eigen::MatrixXd proximity_line()
    {
        Eigen::MatrixXd points = Eigen::MatrixXd::Zero(3, 4);
        points.row(0) << 0.0, 1.0, 3.0, 7.0;
        return points;
    }

    Eigen::MatrixXd proximity_flat_triangle()
    {
        Eigen::MatrixXd points = Eigen::MatrixXd::Zero(3, 3);
        points.col(1) << 2.0, 0.0, 0.0;
        points.col(2) << 1.0, 1.1, 0.0;
        return points;
    }

    EdgeList proximity_sorted(EdgeList edges)
    {
        for (auto& [a, b] : edges) {
            if (a > b)
                std::swap(a, b);
        }
        std::ranges::sort(edges);
        return edges;
    }

}

TEST(KinesisProximityTest, OnALineEveryPlanarGraphIsTheChain)
{
    const EdgeList chain { { 0U, 1U }, { 1U, 2U }, { 2U, 3U } };
    const auto points = proximity_line();

    EXPECT_EQ(Kinesis::sequential_chain(points), chain);
    EXPECT_EQ(proximity_sorted(Kinesis::minimum_spanning_tree(points)), chain);
    EXPECT_EQ(proximity_sorted(Kinesis::gabriel_graph(points)), chain);
    EXPECT_EQ(proximity_sorted(Kinesis::relative_neighborhood_graph(points)), chain);
}

TEST(KinesisProximityTest, NeighbourAndRadiusGraphsFollowDistance)
{
    const auto points = proximity_line();

    EXPECT_EQ(Kinesis::k_nearest_neighbors(points, 1),
        (EdgeList { { 0U, 1U }, { 1U, 0U }, { 2U, 1U }, { 3U, 2U } }));
    EXPECT_EQ(Kinesis::nearest_neighbor_graph(points),
        (EdgeList { { 0U, 1U }, { 1U, 0U }, { 2U, 1U }, { 3U, 2U } }));
    EXPECT_EQ(Kinesis::k_nearest_neighbors(points, 99).size(), 12U);
    EXPECT_TRUE(Kinesis::k_nearest_neighbors(points, 0).empty());
    EXPECT_EQ(Kinesis::radius_threshold_graph(points, 2.0), (EdgeList { { 0U, 1U }, { 1U, 2U } }));
}

TEST(KinesisProximityTest, TheGabrielGraphKeepsAnEdgeTheRelativeNeighbourhoodDrops)
{
    const auto points = proximity_flat_triangle();

    EXPECT_EQ(Kinesis::gabriel_graph(points).size(), 3U);
    EXPECT_EQ(proximity_sorted(Kinesis::relative_neighborhood_graph(points)),
        (EdgeList { { 0U, 2U }, { 1U, 2U } }));
}

TEST(KinesisProximityTest, FewerThanTwoPointsHaveNoEdgesAndCustomNeedsAFunction)
{
    const Eigen::MatrixXd one = Eigen::MatrixXd::Zero(3, 1);

    EXPECT_TRUE(Kinesis::sequential_chain(one).empty());
    EXPECT_TRUE(Kinesis::minimum_spanning_tree(one).empty());
    EXPECT_TRUE(Kinesis::gabriel_graph(one).empty());
    EXPECT_TRUE(Kinesis::generate_proximity_graph(proximity_line(), { .mode = Kinesis::ProximityMode::CUSTOM }).empty());
}

}
