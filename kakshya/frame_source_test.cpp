#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Kakshya/FrameSource.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

using Kakshya::FrameSource;
using Kakshya::ResidentFrames;
using Kinesis::TimePlacement;
using Kinesis::TimeSampling;

namespace {

    TimeSampling must(std::optional<TimeSampling> sampling)
    {
        if (!sampling) {
            throw std::runtime_error("sampling was rejected");
        }
        return *sampling;
    }

    std::vector<std::string> words(size_t count)
    {
        std::vector<std::string> out;
        out.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            out.push_back("frame_" + std::to_string(i));
        }
        return out;
    }

    struct Tracked {
        explicit Tracked(std::shared_ptr<std::atomic<int>> live_count, int tag_value)
            : live(std::move(live_count))
            , tag(tag_value)
        {
            ++*live;
        }

        Tracked(const Tracked&) = delete;
        Tracked& operator=(const Tracked&) = delete;

        ~Tracked() { --*live; }

        std::shared_ptr<std::atomic<int>> live;
        int tag;
    };

    class CountingSource final : public FrameSource<int> {
    public:
        explicit CountingSource(size_t frames)
            : m_frames(frames)
            , m_times(must(TimeSampling::uniform(0.5, 10.0)))
        {
        }

        [[nodiscard]] size_t count() const override { return m_frames; }

        [[nodiscard]] const TimeSampling& time_sampling() const override { return m_times; }

        [[nodiscard]] std::shared_ptr<const int> read(size_t index) const override
        {
            ++m_reads;
            if (index >= m_frames) {
                return nullptr;
            }
            return std::make_shared<const int>(static_cast<int>(index) * 3);
        }

        mutable std::atomic<int> m_reads { 0 };

    private:
        size_t m_frames;
        TimeSampling m_times;
    };

    TEST(ResidentFramesTest, RejectsWhatCannotFormASequence)
    {
        const auto uniform = must(TimeSampling::uniform(1.0));
        const auto acyclic = must(TimeSampling::acyclic({ 0.0, 1.0, 2.0 }));

        EXPECT_EQ(ResidentFrames<std::string>::create(uniform, std::vector<std::string> {}), nullptr);
        EXPECT_EQ(ResidentFrames<std::string>::create(acyclic, words(2)), nullptr);
        EXPECT_EQ(ResidentFrames<std::string>::create(acyclic, words(4)), nullptr);
        EXPECT_NE(ResidentFrames<std::string>::create(acyclic, words(3)), nullptr);
        EXPECT_NE(ResidentFrames<std::string>::create(uniform, words(1)), nullptr);
        EXPECT_NE(ResidentFrames<std::string>::create(uniform, words(50)), nullptr);
    }

    TEST(ResidentFramesTest, RejectsANullFrameAnywhereInTheList)
    {
        const auto uniform = must(TimeSampling::uniform(1.0));
        std::vector<std::shared_ptr<const std::string>> frames {
            std::make_shared<const std::string>("a"),
            nullptr,
            std::make_shared<const std::string>("c"),
        };

        EXPECT_EQ(ResidentFrames<std::string>::create(uniform, frames), nullptr);

        frames[1] = std::make_shared<const std::string>("b");
        EXPECT_NE(ResidentFrames<std::string>::create(uniform, frames), nullptr);

        frames.back() = nullptr;
        EXPECT_EQ(ResidentFrames<std::string>::create(uniform, frames), nullptr);
    }

    TEST(ResidentFramesTest, RejectsASamplingThatCannotTimeTheLastFrame)
    {
        const auto overflowing = must(TimeSampling::uniform_rate({ 24000, 1001 }, std::numeric_limits<int64_t>::max() - 1));

        EXPECT_EQ(ResidentFrames<std::string>::create(overflowing, words(3)), nullptr);
    }

    TEST(ResidentFramesTest, ReadsEveryFrameInOrderAndNothingPastTheEnd)
    {
        const auto source = ResidentFrames<std::string>::create(must(TimeSampling::uniform(0.25, 1.0)), words(5));

        ASSERT_NE(source, nullptr);
        ASSERT_EQ(source->count(), 5U);
        for (size_t i = 0; i < 5; ++i) {
            const auto frame = source->read(i);
            ASSERT_NE(frame, nullptr);
            EXPECT_EQ(*frame, "frame_" + std::to_string(i));
        }
        EXPECT_EQ(source->read(5), nullptr);
        EXPECT_EQ(source->read(1000), nullptr);
        EXPECT_EQ(source->read(std::numeric_limits<size_t>::max()), nullptr);
    }

    TEST(ResidentFramesTest, EveryReadOfAnIndexIsTheSameSharedFrame)
    {
        const auto source = ResidentFrames<std::string>::create(must(TimeSampling::uniform(1.0)), words(3));

        const auto first = source->read(1);
        const auto second = source->read(1);

        EXPECT_EQ(first.get(), second.get());
        EXPECT_NE(source->read(0).get(), source->read(2).get());
    }

    TEST(ResidentFramesTest, SharedFramesAreHeldAsGivenWithoutACopy)
    {
        const auto frame = std::make_shared<const std::string>("shared");
        const auto source = ResidentFrames<std::string>::create(must(TimeSampling::uniform(1.0)),
            std::vector<std::shared_ptr<const std::string>> { frame, frame });

        ASSERT_NE(source, nullptr);
        EXPECT_EQ(source->read(0).get(), frame.get());
        EXPECT_EQ(source->read(1).get(), frame.get());
    }

    TEST(ResidentFramesTest, AFrameOutlivesTheSourceThatServedIt)
    {
        auto live = std::make_shared<std::atomic<int>>(0);
        std::shared_ptr<const Tracked> held;
        {
            std::vector<std::shared_ptr<const Tracked>> frames;
            frames.push_back(std::make_shared<const Tracked>(live, 1));
            frames.push_back(std::make_shared<const Tracked>(live, 2));
            auto source = ResidentFrames<Tracked>::create(must(TimeSampling::uniform(1.0)), std::move(frames));
            ASSERT_NE(source, nullptr);
            EXPECT_EQ(live->load(), 2);

            held = source->read(1);
        }

        EXPECT_EQ(live->load(), 1);
        ASSERT_NE(held, nullptr);
        EXPECT_EQ(held->tag, 2);

        held.reset();
        EXPECT_EQ(live->load(), 0);
    }

    TEST(ResidentFramesTest, TheByValueOverloadMovesEachFrameIntoItsOwnSharedFrame)
    {
        std::vector<std::string> frames { "kept", "moved", "away" };

        const auto source = ResidentFrames<std::string>::create(must(TimeSampling::uniform(1.0)), std::move(frames));

        ASSERT_NE(source, nullptr);
        EXPECT_EQ(*source->read(0), "kept");
        EXPECT_EQ(*source->read(2), "away");
    }

    TEST(ResidentFramesTest, ReportsTheSamplingItWasBuiltWith)
    {
        const auto sampling = must(TimeSampling::acyclic({ -1.0, 0.0, 0.5 }));
        const auto source = ResidentFrames<std::string>::create(sampling, words(3));

        ASSERT_NE(source, nullptr);
        EXPECT_EQ(source->time_sampling(), sampling);
        EXPECT_EQ(*source->time_sampling().time(2), 0.5);
    }

    TEST(ResidentFramesTest, LocatesATimeAmongItsFramesThroughTheBaseInterface)
    {
        const auto resident = ResidentFrames<std::string>::create(must(TimeSampling::uniform(0.5, 10.0)), words(4));
        const FrameSource<std::string>& source = *resident;

        const auto before = source.locate(9.0);
        const auto between = source.locate(10.75);
        const auto after = source.locate(12.0);

        ASSERT_TRUE(before && between && after);
        EXPECT_EQ(before->placement, TimePlacement::Before);
        EXPECT_EQ(between->lower, 1U);
        EXPECT_EQ(between->upper, 2U);
        EXPECT_DOUBLE_EQ(between->fraction, 0.5);
        EXPECT_EQ(after->placement, TimePlacement::After);
        EXPECT_EQ(after->lower, 3U);
        EXPECT_FALSE(source.locate(std::numeric_limits<double>::quiet_NaN()).has_value());
    }

    TEST(ResidentFramesTest, ASingleFrameIsAStaticSource)
    {
        const auto source = ResidentFrames<std::string>::create(must(TimeSampling::uniform(1.0, 3.0)), words(1));

        ASSERT_NE(source, nullptr);
        EXPECT_EQ(source->count(), 1U);
        EXPECT_EQ(source->locate(3.0)->lower, 0U);
        EXPECT_EQ(source->locate(0.0)->placement, TimePlacement::Before);
        EXPECT_EQ(source->locate(99.0)->placement, TimePlacement::After);
        EXPECT_EQ(*source->read(0), "frame_0");
    }

    TEST(ResidentFramesTest, ManyThreadsReadTheSameFramesWithoutCopyingOrTearing)
    {
        const auto source = ResidentFrames<std::string>::create(must(TimeSampling::uniform(1.0)), words(500));
        ASSERT_NE(source, nullptr);

        std::atomic<int> mismatches { 0 };
        std::vector<std::thread> pool;
        for (int t = 0; t < 8; ++t) {
            pool.emplace_back([&source, &mismatches, t] {
                for (size_t i = 0; i < 50000; ++i) {
                    const size_t index = (i * 7 + static_cast<size_t>(t) * 13) % 500;
                    const auto frame = source->read(index);
                    if (!frame || *frame != "frame_" + std::to_string(index)) {
                        ++mismatches;
                    }
                }
            });
        }
        for (auto& thread : pool) {
            thread.join();
        }

        EXPECT_EQ(mismatches.load(), 0);
    }

    TEST(FrameSourceTest, ALazySourceIsUsableThroughTheBaseInterface)
    {
        const CountingSource counting(6);
        const FrameSource<int>& source = counting;

        EXPECT_EQ(source.count(), 6U);
        EXPECT_EQ(*source.read(4), 12);
        EXPECT_EQ(source.read(6), nullptr);
        EXPECT_EQ(counting.m_reads.load(), 2);

        const auto where = source.locate(11.25);
        ASSERT_TRUE(where.has_value());
        EXPECT_EQ(where->lower, 2U);
        EXPECT_EQ(where->upper, 3U);
        EXPECT_EQ(counting.m_reads.load(), 2);
    }

    TEST(FrameSourceTest, LocatingNeverReadsAFrame)
    {
        const CountingSource counting(1000);

        for (int i = 0; i < 1000; ++i) {
            ASSERT_TRUE(counting.locate(10.0 + 0.1 * i).has_value());
        }

        EXPECT_EQ(counting.m_reads.load(), 0);
    }

}
}
