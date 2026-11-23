#include "gtest/gtest.h"
#include "../support/polyhedral_support.hpp"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kinesis/Polyhedral/Partition.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

namespace {

    using Kinesis::Incidence;
    using Kinesis::k_no_group;

    using Ids = std::vector<uint32_t>;

}

TEST(PolyhedralPartitionTest, FacesWithTheSameLabelGatherInAscendingLabelOrder)
{
    const Ids labels { 5, 2, 5, 2, 9 };

    const auto partition = Kinesis::partition_faces(labels);

    EXPECT_EQ(partition.group_labels, (Ids { 2, 5, 9 }));
    EXPECT_EQ(partition.group_offsets, (Ids { 0, 2, 4, 5 }));
    EXPECT_EQ(partition.face_order, (Ids { 1, 3, 0, 2, 4 }));
}

TEST(PolyhedralPartitionTest, NoFacesGiveNoGroups)
{
    const auto partition = Kinesis::partition_faces(std::span<const uint32_t> {});

    EXPECT_TRUE(partition.face_order.empty());
    EXPECT_TRUE(partition.group_labels.empty());
    EXPECT_EQ(partition.group_offsets, (Ids { 0 }));
}

TEST(PolyhedralPartitionTest, OneLabelMakesOneGroupInTheOriginalOrder)
{
    const Ids labels(6, 7U);

    const auto partition = Kinesis::partition_faces(labels);

    EXPECT_EQ(partition.group_labels, (Ids { 7 }));
    EXPECT_EQ(partition.face_order, (Ids { 0, 1, 2, 3, 4, 5 }));
}

TEST(PolyhedralPartitionTest, OverlappingGroupsRepeatSharedFacesAndAppendTheRest)
{
    const Ids first { 0, 1, 2 };
    const Ids second { 2, 3 };
    const std::vector<std::span<const uint32_t>> groups { first, second };

    const auto partition = Kinesis::partition_faces(6, groups);

    ASSERT_TRUE(partition.has_value());
    EXPECT_EQ(partition->face_order, (Ids { 0, 1, 2, 2, 3, 4, 5 }));
    EXPECT_EQ(partition->group_offsets, (Ids { 0, 3, 5, 7 }));
    EXPECT_EQ(partition->group_labels, (Ids { 0, 1, k_no_group }));
}

TEST(PolyhedralPartitionTest, WhenEveryFaceIsListedThereIsNoTrailingGroup)
{
    const Ids first { 0, 1 };
    const Ids second { 3, 2 };
    const std::vector<std::span<const uint32_t>> groups { first, second };

    const auto partition = Kinesis::partition_faces(4, groups);

    ASSERT_TRUE(partition.has_value());
    EXPECT_EQ(partition->face_order, (Ids { 0, 1, 3, 2 }));
    EXPECT_EQ(partition->group_labels, (Ids { 0, 1 }));
    EXPECT_EQ(partition->group_offsets, (Ids { 0, 2, 4 }));
}

TEST(PolyhedralPartitionTest, AMemberBeyondTheFaceCountIsRefused)
{
    const Ids bad { 0, 4 };
    const std::vector<std::span<const uint32_t>> groups { bad };

    EXPECT_FALSE(Kinesis::partition_faces(4, groups).has_value());
    EXPECT_TRUE(Kinesis::partition_faces(5, groups).has_value());
}

TEST(PolyhedralPartitionTest, AnEmptyMembershipListIsAllUngrouped)
{
    const auto partition = Kinesis::partition_faces(3, std::span<const std::span<const uint32_t>> {});

    ASSERT_TRUE(partition.has_value());
    EXPECT_EQ(partition->face_order, (Ids { 0, 1, 2 }));
    EXPECT_EQ(partition->group_labels, (Ids { k_no_group }));
}

TEST(PolyhedralPartitionTest, ReorderingFacesMovesWholeFacesAndAllowsRepeats)
{
    const Ids sizes { 3, 4, 3 };
    const Ids corners { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
    const auto incidence = Incidence::create(sizes, corners, 10);
    ASSERT_TRUE(incidence.has_value());

    const Ids order { 2, 0, 1, 0 };
    const auto reordered = Kinesis::reorder_faces(*incidence, order);

    ASSERT_TRUE(reordered.has_value());
    EXPECT_EQ(reordered->sizes, (Ids { 3, 3, 4, 3 }));
    EXPECT_EQ(reordered->corners, (Ids { 7, 8, 9, 0, 1, 2, 3, 4, 5, 6, 0, 1, 2 }));
}

TEST(PolyhedralPartitionTest, ReorderingMayLeaveFacesOut)
{
    const auto cube = Polyhedral::unit_cube();
    const auto incidence = Incidence::create(cube.sizes, cube.corners, cube.positions.size());
    ASSERT_TRUE(incidence.has_value());

    const Ids order { 5 };
    const auto reordered = Kinesis::reorder_faces(*incidence, order);

    ASSERT_TRUE(reordered.has_value());
    EXPECT_EQ(reordered->sizes, (Ids { 4 }));
    EXPECT_EQ(reordered->corners, (Ids { 1, 2, 6, 5 }));
}

TEST(PolyhedralPartitionTest, ReorderingToAnUnknownFaceIsRefused)
{
    const auto cube = Polyhedral::unit_cube();
    const auto incidence = Incidence::create(cube.sizes, cube.corners, cube.positions.size());
    ASSERT_TRUE(incidence.has_value());

    const Ids order { 0, 6 };
    EXPECT_FALSE(Kinesis::reorder_faces(*incidence, order).has_value());
}

TEST(PolyhedralPartitionTest, APartitionFeedsReorderingIntoContiguousGroups)
{
    const auto cube = Polyhedral::unit_cube();
    const auto incidence = Incidence::create(cube.sizes, cube.corners, cube.positions.size());
    ASSERT_TRUE(incidence.has_value());

    const Ids labels { 1, 0, 1, 0, 1, 0 };
    const auto partition = Kinesis::partition_faces(labels);
    const auto grouped = Kinesis::reorder_faces(*incidence, partition.face_order);

    ASSERT_TRUE(grouped.has_value());
    EXPECT_EQ(partition.face_order, (Ids { 1, 3, 5, 0, 2, 4 }));
    EXPECT_EQ(grouped->sizes.size(), 6U);
    const auto regrouped = Incidence::create(*grouped, cube.positions.size());
    ASSERT_TRUE(regrouped.has_value());
    EXPECT_EQ(std::vector<uint32_t>(regrouped->face(0).begin(), regrouped->face(0).end()), (Ids { 4, 5, 6, 7 }));
}

TEST(PolyhedralPartitionTest, LargeSurfacesTakeTheParallelPath)
{
    constexpr uint32_t faces = 45000;
    Ids labels(faces);
    for (uint32_t f = 0; f < faces; ++f) {
        labels[f] = f % 3;
    }

    const auto partition = Kinesis::partition_faces(labels);

    ASSERT_EQ(partition.group_labels, (Ids { 0, 1, 2 }));
    EXPECT_EQ(partition.group_offsets, (Ids { 0, 15000, 30000, 45000 }));
    EXPECT_EQ(partition.face_order[0], 0U);
    EXPECT_EQ(partition.face_order[1], 3U);
    EXPECT_EQ(partition.face_order[15000], 1U);
    EXPECT_EQ(partition.face_order[44999], 44999U);

    const auto grid = Polyhedral::unit_grid(150, 150);
    const auto incidence = Incidence::create(grid.sizes, grid.corners, grid.positions.size());
    ASSERT_TRUE(incidence.has_value());

    Ids reversed(22500);
    std::ranges::iota(reversed, 0U);
    std::ranges::reverse(reversed);
    const auto reordered = Kinesis::reorder_faces(*incidence, reversed);

    ASSERT_TRUE(reordered.has_value());
    ASSERT_EQ(reordered->corners.size(), 90000U);
    const auto last_face = incidence->face(22499);
    EXPECT_TRUE(std::equal(last_face.begin(), last_face.end(), reordered->corners.begin()));
    const auto first_face = incidence->face(0);
    EXPECT_TRUE(std::equal(first_face.begin(), first_face.end(), reordered->corners.end() - 4));
}

}
