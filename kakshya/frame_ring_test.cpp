#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kakshya/FrameSource.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Kakshya::FrameField;
    using Kakshya::FrameRing;
    using Kakshya::SampledFrames;

    std::shared_ptr<const int> ring_frame(int value)
    {
        return std::make_shared<const int>(value);
    }

    /** Frame i holds 10 * i and was written at i seconds. */
    std::shared_ptr<FrameRing<int>> filled_ring(size_t capacity, int count)
    {
        auto ring = FrameRing<int>::create(capacity);
        for (int i = 0; i < count; ++i) {
            if (!ring->append(ring_frame(i * 10), static_cast<double>(i))) {
                throw std::runtime_error("append was refused");
            }
        }
        return ring;
    }

    class WordField final : public FrameField<int, std::string> {
    public:
        [[nodiscard]] std::shared_ptr<const int> sample(const std::string& key) const override
        {
            return key.empty() ? nullptr : ring_frame(static_cast<int>(key.size()));
        }
    };

    class PairField final : public FrameField<int, std::pair<int, int>> {
    public:
        [[nodiscard]] std::shared_ptr<const int> sample(const std::pair<int, int>& key) const override
        {
            return ring_frame(key.first * 100 + key.second);
        }
    };

    class OneSecondField final : public SampledFrames<int> {
    public:
        [[nodiscard]] std::pair<double, double> extent() const override { return { 0.0, 1.0 }; }

        [[nodiscard]] std::shared_ptr<const int> sample(double time) const override
        {
            return ring_frame(static_cast<int>(time * 100.0));
        }
    };

    TEST(FrameRingTest, ACapacityOfZeroMakesNoRing)
    {
        EXPECT_EQ(FrameRing<int>::create(0), nullptr);
        EXPECT_NE(FrameRing<int>::create(1), nullptr);
    }

    TEST(FrameRingTest, AnEmptyRingAnswersNothing)
    {
        const auto ring = FrameRing<int>::create(4);
        ASSERT_NE(ring, nullptr);

        EXPECT_EQ(ring->head(), 0U);
        EXPECT_EQ(ring->capacity(), 4U);
        EXPECT_EQ(ring->read(0), nullptr);
        EXPECT_EQ(ring->locate(0.0), std::nullopt);
        EXPECT_EQ(ring->sample(0.0), nullptr);
        EXPECT_EQ(ring->extent(), (std::pair<double, double> { 0.0, 0.0 }));
    }

    TEST(FrameRingTest, AppendRefusesWhatCannotBeStored)
    {
        const auto ring = FrameRing<int>::create(4);
        ASSERT_NE(ring, nullptr);

        EXPECT_FALSE(ring->append(nullptr, 0.0));
        EXPECT_FALSE(ring->append(ring_frame(1), std::numeric_limits<double>::quiet_NaN()));
        EXPECT_FALSE(ring->append(ring_frame(1), std::numeric_limits<double>::infinity()));
        EXPECT_EQ(ring->head(), 0U);

        EXPECT_TRUE(ring->append(ring_frame(1), 1.0));
        EXPECT_FALSE(ring->append(ring_frame(2), 0.5));
        EXPECT_TRUE(ring->append(ring_frame(3), 1.0));
        EXPECT_EQ(ring->head(), 2U);
    }

    TEST(FrameRingTest, FramesAreReadByTheNumberTheyWereAppendedAs)
    {
        const auto ring = filled_ring(8, 4);

        EXPECT_EQ(ring->head(), 4U);
        EXPECT_EQ(*ring->read(0), 0);
        EXPECT_EQ(*ring->read(3), 30);
        EXPECT_EQ(ring->read(4), nullptr);
    }

    TEST(FrameRingTest, ThePastTheCapacityIsDropped)
    {
        const auto ring = filled_ring(3, 5);

        EXPECT_EQ(ring->head(), 5U);
        EXPECT_EQ(ring->read(0), nullptr);
        EXPECT_EQ(ring->read(1), nullptr);
        EXPECT_EQ(*ring->read(2), 20);
        EXPECT_EQ(*ring->read(4), 40);
        EXPECT_EQ(ring->extent(), (std::pair<double, double> { 2.0, 4.0 }));
    }

    TEST(FrameRingTest, ATimeFindsTheNewestFrameAtOrBeforeIt)
    {
        const auto ring = filled_ring(8, 5);

        EXPECT_EQ(ring->locate(2.5), std::optional<uint64_t> { 2 });
        EXPECT_EQ(ring->locate(2.0), std::optional<uint64_t> { 2 });
        EXPECT_EQ(ring->locate(0.0), std::optional<uint64_t> { 0 });
        EXPECT_EQ(ring->locate(100.0), std::optional<uint64_t> { 4 });
        EXPECT_EQ(ring->locate(-0.5), std::nullopt);
        EXPECT_EQ(*ring->sample(3.9), 30);
        EXPECT_EQ(ring->sample(-1.0), nullptr);
    }

    TEST(FrameRingTest, ATimeBeforeTheOldestHeldFrameHasNoFrame)
    {
        const auto ring = filled_ring(3, 6);

        EXPECT_EQ(ring->locate(2.0), std::nullopt);
        EXPECT_EQ(ring->locate(3.0), std::optional<uint64_t> { 3 });
        EXPECT_EQ(ring->sample(2.5), nullptr);
    }

    TEST(FrameRingTest, FramesWrittenAtTheSameTimeAreBothKeptAndTheLaterWinsTheTime)
    {
        const auto ring = FrameRing<int>::create(4);
        ASSERT_NE(ring, nullptr);
        ASSERT_TRUE(ring->append(ring_frame(1), 1.0));
        ASSERT_TRUE(ring->append(ring_frame(2), 1.0));

        EXPECT_EQ(ring->locate(1.0), std::optional<uint64_t> { 1 });
        EXPECT_EQ(*ring->read(0), 1);
    }

    TEST(FrameRingTest, ARingIsASampledSourceOverTheTimesItHolds)
    {
        const auto ring = filled_ring(8, 3);
        const std::shared_ptr<const SampledFrames<int>> sampled = ring;

        EXPECT_EQ(sampled->extent(), (std::pair<double, double> { 0.0, 2.0 }));
        EXPECT_EQ(*sampled->sample(1.5), 10);
    }

    TEST(FrameRingTest, AFrameHeldByAReaderSurvivesItsDropFromTheRing)
    {
        const auto ring = filled_ring(2, 2);
        const auto held = ring->read(0);
        ASSERT_NE(held, nullptr);

        ASSERT_TRUE(ring->append(ring_frame(20), 2.0));
        ASSERT_TRUE(ring->append(ring_frame(30), 3.0));

        EXPECT_EQ(ring->read(0), nullptr);
        EXPECT_EQ(*held, 0);
    }

    TEST(FrameRingTest, AReaderOnAnotherThreadNeverSeesAFrameThatDoesNotMatchItsTime)
    {
        const auto ring = FrameRing<int>::create(32);
        ASSERT_NE(ring, nullptr);
        constexpr int k_total = 4000;
        std::atomic<bool> done { false };
        std::atomic<int> wrong { 0 };

        std::thread reader([&] {
            while (!done.load()) {
                const auto head = ring->head();
                if (head == 0) {
                    continue;
                }
                const double time = static_cast<double>(head - 1);
                if (const auto frame = ring->sample(time)) {
                    if (static_cast<double>(*frame) > time) {
                        wrong.fetch_add(1);
                    }
                }
            }
        });

        for (int i = 0; i < k_total; ++i) {
            ASSERT_TRUE(ring->append(ring_frame(i), static_cast<double>(i)));
        }
        done.store(true);
        reader.join();

        EXPECT_EQ(wrong.load(), 0);
        EXPECT_EQ(ring->head(), static_cast<uint64_t>(k_total));
        EXPECT_EQ(*ring->read(k_total - 1), k_total - 1);
    }

    TEST(FrameFieldTest, AFieldMayBeKeyedByAnythingAndTakesTheKeyByReference)
    {
        const WordField words;
        const FrameField<int, std::string>& field = words;

        EXPECT_EQ(*field.sample(std::string("four")), 4);
        EXPECT_EQ(field.sample(std::string()), nullptr);

        const PairField pairs;
        const FrameField<int, std::pair<int, int>>& by_pair = pairs;
        EXPECT_EQ(*by_pair.sample(std::pair<int, int> { 3, 4 }), 304);
    }

    TEST(FrameFieldTest, ASampledSourceIsAFieldKeyedByTimeTakenByValue)
    {
        const OneSecondField seconds;
        const FrameField<int, double>& field = seconds;

        EXPECT_EQ(*field.sample(0.5), 50);
        EXPECT_EQ(seconds.extent(), (std::pair<double, double> { 0.0, 1.0 }));
    }

} // namespace
} // namespace MayaFlux::Test
