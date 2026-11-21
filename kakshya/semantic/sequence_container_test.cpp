#include "gtest/gtest.h"
#include "../../support/test_metadata.hpp"

#include "MayaFlux/Kakshya/Semantic/SequenceContainer.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Kakshya::DataModality;
    using Kakshya::FrameSource;
    using Kakshya::Region;
    using Kakshya::RegionGroup;
    using Kakshya::RegionSegment;
    using Kakshya::ResidentFrames;
    using Kakshya::SequenceContainer;
    using Kinesis::TimeSampling;

    using Words = SequenceContainer<std::string>;

    TimeSampling uniform(double step, double start = 0.0)
    {
        auto sampling = TimeSampling::uniform(step, start);
        if (!sampling) {
            throw std::runtime_error("sampling was rejected");
        }
        return *sampling;
    }

    std::shared_ptr<Words> words(size_t count)
    {
        std::vector<std::string> frames;
        frames.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            frames.push_back("f" + std::to_string(i));
        }
        return Words::create(ResidentFrames<std::string>::create(uniform(0.5), std::move(frames)),
            DataModality::TENSOR_ND);
    }

    RegionSegment segment(uint64_t first, uint64_t last, uint64_t offset, uint64_t size)
    {
        RegionSegment out;
        out.source_region = Region({ first }, { last });
        out.offset_in_region = { offset };
        out.segment_size = { size };
        return out;
    }

    TEST(SequenceContainerTest, ANullSourceMakesNoContainer)
    {
        EXPECT_EQ(Words::create(nullptr), nullptr);
    }

    TEST(SequenceContainerTest, ReportsTheFrameAxisAndTheSourceTimes)
    {
        const auto container = words(5);
        ASSERT_NE(container, nullptr);

        EXPECT_EQ(container->count(), 5U);
        EXPECT_EQ(container->get_num_frames(), 5U);
        EXPECT_EQ(container->get_total_elements(), 5U);
        EXPECT_EQ(container->get_frame_size(), 1U);
        EXPECT_TRUE(container->has_data());
        EXPECT_EQ(container->get_raw_data(), nullptr);
        EXPECT_EQ(container->get_structure().modality, DataModality::TENSOR_ND);

        const auto dimensions = container->get_dimensions();
        ASSERT_EQ(dimensions.size(), 1U);
        EXPECT_EQ(dimensions[0].size, 5U);
        EXPECT_EQ(*container->time_sampling().time(3), 1.5);
    }

    TEST(SequenceContainerTest, ReadingSharesTheSourcesFrameWithoutACopy)
    {
        const auto container = words(3);

        EXPECT_EQ(container->read(1).get(), container->source()->read(1).get());
        EXPECT_EQ(container->read(3), nullptr);
    }

    TEST(SequenceContainerTest, ARegionIsAnInclusiveFrameRangeCopiedInOrder)
    {
        const auto container = words(6);

        const auto span = container->get_region_data(Region({ 1 }, { 3 }));
        ASSERT_EQ(span.size(), 3U);
        EXPECT_EQ(span[0], "f1");
        EXPECT_EQ(span[2], "f3");

        const auto point = container->get_region_data(Region(std::vector<uint64_t> { 5 }));
        ASSERT_EQ(point.size(), 1U);
        EXPECT_EQ(point[0], "f5");
    }

    TEST(SequenceContainerTest, ARegionThatIsNotAFrameRangeIsRefused)
    {
        const auto container = words(4);

        for (const auto& bad : {
                 Region({ 2 }, { 1 }),
                 Region({ 0 }, { 4 }),
                 Region({ 4 }, { 4 }),
                 Region({ 0, 0 }, { 1, 1 }),
                 Region({ 0 }, { std::numeric_limits<uint64_t>::max() }),
                 Region(),
             }) {
            EXPECT_TRUE(container->get_region_data(bad).empty());
            EXPECT_FALSE(container->is_region_loaded(bad));
        }
        EXPECT_TRUE(container->is_region_loaded(Region({ 0 }, { 3 })));
    }

    TEST(SequenceContainerTest, CopiedFramesAreIndependentOfTheSource)
    {
        const auto container = words(2);

        auto copied = container->get_region_data(Region({ 0 }, { 1 }));
        copied[0] = "changed";

        EXPECT_EQ(*container->read(0), "f0");
    }

    TEST(SequenceContainerTest, WritesAreRefused)
    {
        const auto container = words(3);

        container->set_region_data(Region({ 0 }, { 1 }), { "x", "y" });

        EXPECT_EQ(*container->read(0), "f0");
        EXPECT_EQ(*container->read(1), "f1");
    }

    TEST(SequenceContainerTest, GroupsCopyTheFramesOfEveryValidRegion)
    {
        const auto container = words(6);
        RegionGroup group;
        group.name = "mixed";
        group.regions = { Region({ 0 }, { 1 }), Region({ 9 }, { 9 }), Region({ 4 }, { 4 }) };
        container->add_region_group(group);

        const auto copied = container->get_region_group_data(container->get_region_group("mixed"));

        ASSERT_EQ(copied.size(), 2U);
        EXPECT_EQ(copied[0], "f0");
        EXPECT_EQ(copied[1], "f4");
    }

    TEST(SequenceContainerTest, SegmentsStartInsideTheirRegionAndSpanTheirSize)
    {
        const auto container = words(10);

        const auto frames = container->get_segments_data({
            segment(2, 7, 1, 3),
            segment(2, 7, 4, 2),
            segment(2, 7, 5, 2),
            segment(2, 7, 0, 0),
            segment(7, 2, 0, 1),
            segment(0, 20, 18, 2),
        });

        ASSERT_EQ(frames.size(), 5U);
        EXPECT_EQ(frames[0], "f3");
        EXPECT_EQ(frames[2], "f5");
        EXPECT_EQ(frames[3], "f6");
        EXPECT_EQ(frames[4], "f7");
    }

    TEST(SequenceContainerTest, CoordinatesAndIndicesAreTheFrameIndex)
    {
        const auto container = words(4);

        EXPECT_EQ(container->coordinates_to_linear_index({ 3 }), 3U);
        EXPECT_EQ(container->coordinates_to_linear_index({ 4 }), 0U);
        EXPECT_EQ(container->coordinates_to_linear_index({ 1, 1 }), 0U);
        EXPECT_EQ(container->linear_index_to_coordinates(2), std::vector<uint64_t> { 2 });
        EXPECT_TRUE(container->linear_index_to_coordinates(4).empty());
    }

    TEST(SequenceContainerTest, ClearEmptiesTheContainerAndLeavesHeldFramesAlone)
    {
        const auto container = words(3);
        const auto held = container->read(1);
        container->add_region_group(RegionGroup {});

        container->clear();

        EXPECT_EQ(container->count(), 0U);
        EXPECT_FALSE(container->has_data());
        EXPECT_EQ(container->read(1), nullptr);
        EXPECT_TRUE(container->get_region_data(Region({ 0 }, { 0 })).empty());
        EXPECT_TRUE(container->get_all_region_groups().empty());
        EXPECT_EQ(*held, "f1");
        EXPECT_EQ(container->source()->count(), 3U);
    }

    TEST(SequenceContainerTest, ALoneFrameIsAStaticContainer)
    {
        const auto container = words(1);

        EXPECT_EQ(container->count(), 1U);
        EXPECT_EQ(container->locate(100.0)->lower, 0U);
        EXPECT_EQ(container->get_region_data(Region(std::vector<uint64_t> { 0 })).size(), 1U);
    }

    TEST(SequenceContainerTest, ItIsUsableAsAFrameSource)
    {
        const auto container = words(4);
        const FrameSource<std::string>& source = *container;

        const auto where = source.locate(0.75);
        ASSERT_TRUE(where.has_value());
        EXPECT_EQ(where->lower, 1U);
        EXPECT_EQ(where->upper, 2U);
    }

}
}
