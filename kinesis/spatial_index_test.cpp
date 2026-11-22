#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include <chrono>
#include <condition_variable>

#include "MayaFlux/Kinesis/Spatial/SpatialIndex.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

using Kinesis::QueryResult;
using Kinesis::SpatialIndex3D;
using Kinesis::SpatialIndexND;

namespace {

    std::set<uint32_t> ids_of(const std::vector<QueryResult>& results)
    {
        std::set<uint32_t> ids;
        for (const auto& result : results) {
            ids.insert(result.id);
        }
        return ids;
    }

    class KinesisSpatialIndexTest : public ::testing::Test {
    protected:
        std::unique_ptr<SpatialIndex3D> index = Kinesis::make_spatial_index_3d(1.0F);
    };

    class KinesisSpatialIndexNDTest : public ::testing::Test {
    protected:
        static Eigen::VectorXd point(std::initializer_list<double> values)
        {
            Eigen::VectorXd p(static_cast<Eigen::Index>(values.size()));
            Eigen::Index i = 0;
            for (const double v : values) {
                p(i++) = v;
            }
            return p;
        }
    };

    TEST_F(KinesisSpatialIndexTest, NothingIsVisibleBeforeAPublish)
    {
        index->insert({ 0.0F, 0.0F, 0.0F });

        EXPECT_TRUE(index->within_radius({ 0.0F, 0.0F, 0.0F }, 5.0F).empty());
        EXPECT_EQ(index->count(), 0U);
    }

    TEST_F(KinesisSpatialIndexTest, AnEmptyPublishFindsNothing)
    {
        index->publish();

        EXPECT_TRUE(index->within_radius({ 0.0F, 0.0F, 0.0F }, 5.0F).empty());
    }

    TEST_F(KinesisSpatialIndexTest, FindsEveryPointInsideTheRadiusAndNoneOutside)
    {
        const uint32_t near = index->insert({ 1.0F, 0.0F, 0.0F });
        const uint32_t far = index->insert({ 10.0F, 0.0F, 0.0F });
        const uint32_t behind = index->insert({ -2.0F, -1.0F, 0.5F });
        index->publish();

        const auto results = index->within_radius({ 0.0F, 0.0F, 0.0F }, 3.0F);

        EXPECT_EQ(ids_of(results), (std::set<uint32_t> { near, behind }));
        EXPECT_EQ(results.size(), 2U);
        EXPECT_EQ(ids_of(results).count(far), 0U);
    }

    TEST_F(KinesisSpatialIndexTest, ReportsSquaredDistances)
    {
        index->insert({ 3.0F, 4.0F, 0.0F });
        index->publish();

        const auto results = index->within_radius({ 0.0F, 0.0F, 0.0F }, 5.0F);

        ASSERT_EQ(results.size(), 1U);
        EXPECT_FLOAT_EQ(results.front().distance_sq, 25.0F);
    }

    TEST_F(KinesisSpatialIndexTest, ThePointAtExactlyTheRadiusIsIncluded)
    {
        const uint32_t edge = index->insert({ 2.0F, 0.0F, 0.0F });
        index->publish();

        EXPECT_EQ(ids_of(index->within_radius({ 0.0F, 0.0F, 0.0F }, 2.0F)), (std::set<uint32_t> { edge }));
    }

    TEST_F(KinesisSpatialIndexTest, ARadiusOfZeroFindsOnlyACoincidentPoint)
    {
        const uint32_t here = index->insert({ 1.5F, 2.5F, -3.5F });
        index->insert({ 1.6F, 2.5F, -3.5F });
        index->publish();

        EXPECT_EQ(ids_of(index->within_radius({ 1.5F, 2.5F, -3.5F }, 0.0F)), (std::set<uint32_t> { here }));
    }

    TEST_F(KinesisSpatialIndexTest, PointsAcrossNegativeCellsAreFound)
    {
        const uint32_t a = index->insert({ -0.4F, -0.4F, -0.4F });
        const uint32_t b = index->insert({ 0.4F, 0.4F, 0.4F });
        index->publish();

        EXPECT_EQ(ids_of(index->within_radius({ 0.0F, 0.0F, 0.0F }, 1.0F)), (std::set<uint32_t> { a, b }));
    }

    TEST_F(KinesisSpatialIndexTest, UpdatingMovesAPointOnlyAfterThePublish)
    {
        const uint32_t id = index->insert({ 0.0F, 0.0F, 0.0F });
        index->publish();

        index->update(id, { 50.0F, 0.0F, 0.0F });
        EXPECT_EQ(ids_of(index->within_radius({ 0.0F, 0.0F, 0.0F }, 1.0F)), (std::set<uint32_t> { id }));

        index->publish();
        EXPECT_TRUE(index->within_radius({ 0.0F, 0.0F, 0.0F }, 1.0F).empty());
        EXPECT_EQ(ids_of(index->within_radius({ 50.0F, 0.0F, 0.0F }, 1.0F)), (std::set<uint32_t> { id }));
    }

    TEST_F(KinesisSpatialIndexTest, RemovingAPointDropsItFromTheNextSnapshot)
    {
        const uint32_t keep = index->insert({ 0.0F, 0.0F, 0.0F });
        const uint32_t drop = index->insert({ 0.5F, 0.0F, 0.0F });
        index->publish();

        index->remove(drop);
        index->publish();

        EXPECT_EQ(ids_of(index->within_radius({ 0.0F, 0.0F, 0.0F }, 2.0F)), (std::set<uint32_t> { keep }));
    }

    TEST_F(KinesisSpatialIndexTest, ARadiusFarLargerThanTheIndexedExtentFindsEveryPoint)
    {
        std::set<uint32_t> all;
        for (int i = 0; i < 50; ++i) {
            all.insert(index->insert({ static_cast<float>(i), static_cast<float>(-i), 0.5F * static_cast<float>(i) }));
        }
        index->publish();

        for (const float radius : { 1.0e3F, 1.0e5F, 1.0e8F, 1.0e12F }) {
            const auto results = index->within_radius({ 0.0F, 0.0F, 0.0F }, radius);
            EXPECT_EQ(ids_of(results), all);
            EXPECT_EQ(results.size(), all.size());
        }
    }

    TEST_F(KinesisSpatialIndexTest, ALargeRadiusOnAnEmptySnapshotFindsNothing)
    {
        index->publish();

        EXPECT_TRUE(index->within_radius({ 0.0F, 0.0F, 0.0F }, 1.0e12F).empty());
    }

    TEST_F(KinesisSpatialIndexTest, ALargeRadiusStillExcludesPointsBeyondIt)
    {
        const uint32_t inside = index->insert({ 400.0F, 0.0F, 0.0F });
        index->insert({ 5000.0F, 0.0F, 0.0F });
        index->publish();

        EXPECT_EQ(ids_of(index->within_radius({ 0.0F, 0.0F, 0.0F }, 1000.0F)), (std::set<uint32_t> { inside }));
    }

    TEST_F(KinesisSpatialIndexTest, NearestPointsComeBackClosestFirst)
    {
        const uint32_t closest = index->insert({ 1.0F, 0.0F, 0.0F });
        const uint32_t middle = index->insert({ 2.0F, 0.0F, 0.0F });
        index->insert({ 9.0F, 0.0F, 0.0F });
        index->publish();

        const auto results = index->k_nearest({ 0.0F, 0.0F, 0.0F }, 2U);

        ASSERT_EQ(results.size(), 2U);
        EXPECT_EQ(results[0].id, closest);
        EXPECT_EQ(results[1].id, middle);
    }

    TEST_F(KinesisSpatialIndexTest, AskingForMoreNeighboursThanThereArePointsReturnsThemAll)
    {
        index->insert({ 1.0F, 0.0F, 0.0F });
        index->insert({ 2.0F, 0.0F, 0.0F });
        index->publish();

        EXPECT_EQ(index->k_nearest({ 0.0F, 0.0F, 0.0F }, 10U).size(), 2U);
    }

    TEST_F(KinesisSpatialIndexNDTest, AGridBackedIndexFindsEveryPointForALargeRadius)
    {
        const auto index = Kinesis::make_spatial_index_nd(1.0F, 3U);
        std::set<uint32_t> all;
        for (int i = 0; i < 20; ++i) {
            all.insert(index->insert(point({ static_cast<double>(i), 0.0, 0.0 })));
        }
        index->publish();

        const auto results = index->within_radius(point({ 0.0, 0.0, 0.0 }), 1.0e6F);

        EXPECT_EQ(ids_of(results), all);
        EXPECT_EQ(results.size(), all.size());
    }

    TEST_F(KinesisSpatialIndexNDTest, AGridBackedIndexStillRespectsASmallRadius)
    {
        const auto index = Kinesis::make_spatial_index_nd(1.0F, 3U);
        const uint32_t near = index->insert(point({ 0.5, 0.0, 0.0 }));
        index->insert(point({ 7.0, 0.0, 0.0 }));
        index->publish();

        EXPECT_EQ(ids_of(index->within_radius(point({ 0.0, 0.0, 0.0 }), 1.0F)), (std::set<uint32_t> { near }));
    }

    TEST_F(KinesisSpatialIndexNDTest, AHighDimensionalIndexScansAndFindsWithinTheRadius)
    {
        const auto index = Kinesis::make_spatial_index_nd(1.0F, 8U);
        const uint32_t near = index->insert(point({ 0.1, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 }));
        index->insert(point({ 5.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 }));
        index->publish();

        EXPECT_EQ(ids_of(index->within_radius(point({ 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 }), 1.0F)),
            (std::set<uint32_t> { near }));
    }

    TEST(KinesisSpatialIndexSnapshotTest, ConcurrentReadersKeepEarlierGenerationsAlive)
    {
        using namespace std::chrono_literals;

        constexpr size_t readers_per_generation = 20;
        std::mutex mutex;
        std::condition_variable changed;
        size_t old_readers = 0;
        size_t new_readers = 0;
        bool release_readers = false;

        Kinesis::SpatialIndex3D index(1.0F, [&](const glm::vec3&, const glm::vec3& candidate) {
            std::unique_lock lock(mutex);
            if (candidate.x == 1.0F)
                ++old_readers;
            if (candidate.x == 2.0F)
                ++new_readers;
            changed.notify_all();
            changed.wait_for(lock, 30s, [&] { return release_readers; });
            return candidate.x * candidate.x;
        });

        const uint32_t id = index.insert({ 1.0F, 0.0F, 0.0F });
        index.publish();

        std::array<std::vector<QueryResult>, readers_per_generation * 2> answers;
        std::vector<std::jthread> readers;
        readers.reserve(answers.size());

        for (size_t i = 0; i < readers_per_generation; ++i) {
            readers.emplace_back([&, i] {
                answers[i] = index.k_nearest({ 0.0F, 0.0F, 0.0F }, 1);
            });
        }

        bool old_ready = false;
        {
            std::unique_lock lock(mutex);
            old_ready = changed.wait_for(lock, 10s, [&] { return old_readers == readers_per_generation; });
        }

        bool new_ready = false;
        if (old_ready) {
            index.update(id, { 2.0F, 0.0F, 0.0F });
            index.publish();

            for (size_t i = readers_per_generation; i < answers.size(); ++i) {
                readers.emplace_back([&, i] {
                    answers[i] = index.k_nearest({ 0.0F, 0.0F, 0.0F }, 1);
                });
            }

            std::unique_lock lock(mutex);
            new_ready = changed.wait_for(lock, 10s, [&] { return new_readers == readers_per_generation; });
        }

        if (new_ready) {
            index.update(id, { 3.0F, 0.0F, 0.0F });
            index.publish();
        }

        {
            std::lock_guard lock(mutex);
            release_readers = true;
        }
        changed.notify_all();
        for (auto& reader : readers)
            reader.join();

        ASSERT_TRUE(old_ready);
        ASSERT_TRUE(new_ready);
        for (size_t i = 0; i < readers_per_generation; ++i) {
            ASSERT_EQ(answers[i].size(), 1U);
            EXPECT_EQ(answers[i][0].id, id);
            EXPECT_FLOAT_EQ(answers[i][0].distance_sq, 1.0F);
        }
        for (size_t i = readers_per_generation; i < answers.size(); ++i) {
            ASSERT_EQ(answers[i].size(), 1U);
            EXPECT_EQ(answers[i][0].id, id);
            EXPECT_FLOAT_EQ(answers[i][0].distance_sq, 4.0F);
        }
    }

    TEST(KinesisSpatialIndexSnapshotTest, ConcurrentPublicationExposesOnlyCompleteGenerations)
    {
        Kinesis::SpatialIndex3D index(1.0F, [](const glm::vec3& a, const glm::vec3& b) {
            const glm::vec3 delta = a - b;
            return glm::dot(delta, delta);
        });
        const uint32_t first = index.insert({ 1.0F, 0.0F, 0.0F });
        const uint32_t second = index.insert({ 1.0F, 1.0F, 0.0F });
        index.publish();

        std::atomic<bool> start { false };
        std::atomic<int> failure { 0 };
        std::vector<std::jthread> readers;
        readers.reserve(6);

        for (int reader = 0; reader < 6; ++reader) {
            readers.emplace_back([&] {
                while (!start.load(std::memory_order_acquire))
                    std::this_thread::yield();

                for (int turn = 0; turn < 400 && failure.load() == 0; ++turn) {
                    const auto entries = index.all();
                    if (entries.size() != 2 || entries[0].second.x != entries[1].second.x
                        || (entries[0].second.x != 1.0F && entries[0].second.x != 100.0F)) {
                        failure.store(1);
                        return;
                    }

                    const auto radius = index.within_radius({ 0.0F, 0.0F, 0.0F }, 3.0F);
                    if (!radius.empty() && (radius.size() != 2 || ids_of(radius) != std::set<uint32_t> { first, second })) {
                        failure.store(2);
                        return;
                    }

                    const auto nearest = index.k_nearest({ 0.0F, 0.0F, 0.0F }, 2);
                    if (nearest.size() != 2 || nearest[0].id != first || nearest[1].id != second
                        || (nearest[0].distance_sq != 1.0F && nearest[0].distance_sq != 10000.0F)) {
                        failure.store(3);
                        return;
                    }

                    const auto position = index.position_of(first);
                    if (!position || (position->x != 1.0F && position->x != 100.0F)) {
                        failure.store(4);
                        return;
                    }

                    if (index.count() != 2U) {
                        failure.store(5);
                        return;
                    }
                }
            });
        }

        std::jthread writer([&] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();

            for (int turn = 0; turn < 400; ++turn) {
                const float x = turn % 2 == 0 ? 100.0F : 1.0F;
                index.update(first, { x, 0.0F, 0.0F });
                index.update(second, { x, 1.0F, 0.0F });
                index.publish();
            }
        });

        start.store(true, std::memory_order_release);
        writer.join();
        for (auto& reader : readers)
            reader.join();

        EXPECT_EQ(failure.load(), 0);
    }

}
}
