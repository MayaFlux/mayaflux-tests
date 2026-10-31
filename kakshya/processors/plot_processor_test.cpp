#include "gtest/gtest.h"

#include "MayaFlux/Buffers/AudioBuffer.hpp"
#include "MayaFlux/Kakshya/NDData/DataAccess.hpp"
#include "MayaFlux/Kakshya/Processors/PlotProcessor.hpp"
#include "MayaFlux/Kakshya/Source/PlotContainer.hpp"
#include "MayaFlux/Nodes/Generators/Sine.hpp"

namespace MayaFlux::Test {

using Kakshya::PlotContainer;
using Kakshya::PlotProcessor;
using Role = Kakshya::DataDimension::Role;

namespace {

    struct Rig {
        std::shared_ptr<PlotContainer> container = std::make_shared<PlotContainer>();
        std::shared_ptr<PlotProcessor> processor = std::make_shared<PlotProcessor>();

        explicit Rig(std::initializer_list<uint64_t> sizes)
        {
            for (const uint64_t size : sizes)
                container->add_series("s", size, Role::SPATIAL_Y);
            container->set_default_processor(processor);
        }

        void process()
        {
            container->mark_ready_for_processing(true);
            container->process_default();
        }

        [[nodiscard]] std::vector<double> data(uint32_t index) const
        {
            auto frame = container->get_frame(index).as<double>();
            return { frame.begin(), frame.end() };
        }

        [[nodiscard]] std::vector<double> processed(size_t index) const
        {
            return std::get<std::vector<double>>(container->get_processed_data()[index]);
        }
    };

    std::shared_ptr<Nodes::Node> make_node()
    {
        return std::make_shared<Nodes::Generator::Sine>(440.F, 1.0);
    }

    bool holds_a_buffer_reference(const std::shared_ptr<Nodes::Node>& node)
    {
        return node->mark_buffer_processed();
    }

}

// =============================================================================
// Sources
// =============================================================================

TEST(PlotProcessorTest, ACallableFillsTheSeriesAndTheProcessedCopy)
{
    Rig rig { 3 };
    rig.processor->bind_callable(0, [](std::vector<double>& out) { out = { 1.0, 2.0, 3.0 }; });

    rig.process();

    EXPECT_EQ(rig.data(0), (std::vector<double> { 1.0, 2.0, 3.0 }));
    EXPECT_EQ(rig.processed(0), (std::vector<double> { 1.0, 2.0, 3.0 }));
}

TEST(PlotProcessorTest, ACallableSeesASeriesSizedBufferSeededWithTheCurrentValues)
{
    Rig rig { 4 };
    std::vector<size_t> sizes;
    rig.processor->bind_callable(0, [&sizes](std::vector<double>& out) {
        sizes.push_back(out.size());
        for (auto& v : out)
            v += 1.0;
    });

    rig.process();
    rig.process();
    rig.process();

    EXPECT_EQ(sizes, (std::vector<size_t> { 4, 4, 4 }));
    EXPECT_EQ(rig.data(0), (std::vector<double> { 3.0, 3.0, 3.0, 3.0 }));
}

TEST(PlotProcessorTest, RawDataIsAppliedOnceAndZeroPaddedToTheSeries)
{
    Rig rig { 4 };
    rig.processor->set_raw(0, { 1.0, 2.0 });

    rig.process();
    EXPECT_EQ(rig.data(0), (std::vector<double> { 1.0, 2.0, 0.0, 0.0 }));

    rig.container->write_series(0, std::vector<double> { 9.0, 9.0, 9.0, 9.0 });
    rig.process();
    EXPECT_EQ(rig.data(0), (std::vector<double> { 9.0, 9.0, 9.0, 9.0 }));
}

TEST(PlotProcessorTest, SettingRawAgainReappliesIt)
{
    Rig rig { 2 };
    rig.processor->set_raw(0, { 1.0, 2.0 });
    rig.process();

    rig.processor->set_raw(0, { 5.0, 6.0 });
    rig.process();

    EXPECT_EQ(rig.data(0), (std::vector<double> { 5.0, 6.0 }));
}

TEST(PlotProcessorTest, RawDataLongerThanTheSeriesIsTruncated)
{
    Rig rig { 2 };
    rig.processor->set_raw(0, { 1.0, 2.0, 3.0, 4.0 });

    rig.process();

    EXPECT_EQ(rig.data(0), (std::vector<double> { 1.0, 2.0 }));
}

TEST(PlotProcessorTest, AnAudioBufferIsCopiedAndZeroPadded)
{
    Rig rig { 5 };
    auto audio = std::make_shared<Buffers::AudioBuffer>(0, 3);
    audio->get_data() = { 0.1, 0.2, 0.3 };
    rig.processor->bind_audio_buffer(0, audio);

    rig.process();

    EXPECT_EQ(rig.data(0), (std::vector<double> { 0.1, 0.2, 0.3, 0.0, 0.0 }));
}

TEST(PlotProcessorTest, AnAudioBufferLongerThanTheSeriesIsTruncated)
{
    Rig rig { 2 };
    auto audio = std::make_shared<Buffers::AudioBuffer>(0, 4);
    audio->get_data() = { 0.1, 0.2, 0.3, 0.4 };
    rig.processor->bind_audio_buffer(0, audio);

    rig.process();

    EXPECT_EQ(rig.data(0), (std::vector<double> { 0.1, 0.2 }));
}

TEST(PlotProcessorTest, ANodeIsSampledIntoTheSeries)
{
    Rig rig { 64 };
    rig.processor->bind_node(0, make_node());

    rig.process();

    const auto samples = rig.data(0);
    ASSERT_EQ(samples.size(), 64U);
    EXPECT_TRUE(std::ranges::any_of(samples, [](double v) { return v != 0.0; }));
    EXPECT_TRUE(std::ranges::all_of(samples, [](double v) { return v >= -1.0 && v <= 1.0; }));
}

TEST(PlotProcessorTest, UnboundSeriesAreCopiedThroughUnchanged)
{
    Rig rig { 2, 2 };
    rig.processor->bind_callable(0, [](std::vector<double>& out) { out = { 1.0, 1.0 }; });
    rig.container->write_series(1, std::vector<double> { 7.0, 8.0 });

    rig.process();

    EXPECT_EQ(rig.processed(0), (std::vector<double> { 1.0, 1.0 }));
    EXPECT_EQ(rig.processed(1), (std::vector<double> { 7.0, 8.0 }));
}

// =============================================================================
// Binding rules
// =============================================================================

TEST(PlotProcessorTest, RebindingASeriesReplacesTheSource)
{
    Rig rig { 2 };
    rig.processor->bind_callable(0, [](std::vector<double>& out) { out = { 1.0, 1.0 }; });
    rig.processor->bind_callable(0, [](std::vector<double>& out) { out = { 2.0, 2.0 }; });

    rig.process();

    EXPECT_EQ(rig.data(0), (std::vector<double> { 2.0, 2.0 }));
}

TEST(PlotProcessorTest, BindingsToSeriesTheContainerDoesNotHaveAreSkipped)
{
    Rig rig { 2 };
    int calls = 0;
    rig.processor->bind_callable(5, [&calls](std::vector<double>&) { ++calls; });

    rig.process();

    EXPECT_EQ(calls, 0);
    EXPECT_TRUE(rig.processor->has_binding(5));
}

TEST(PlotProcessorTest, NullSourcesAreRejected)
{
    PlotProcessor processor;

    processor.bind_node(0, nullptr);
    processor.bind_audio_buffer(1, nullptr);
    processor.bind_network(2, nullptr);
    processor.bind_callable(3, {});

    for (uint32_t i = 0; i < 4; ++i)
        EXPECT_FALSE(processor.has_binding(i)) << i;
}

TEST(PlotProcessorTest, UnbindRemovesTheBindingAndIgnoresUnknownSeries)
{
    PlotProcessor processor;
    processor.bind_callable(0, [](std::vector<double>&) { });
    ASSERT_TRUE(processor.has_binding(0));

    processor.unbind(0);
    processor.unbind(3);

    EXPECT_FALSE(processor.has_binding(0));
}

TEST(PlotProcessorTest, SemanticsApplyOnlyToAnExistingBinding)
{
    PlotProcessor processor;

    processor.set_series_semantics(0, Role::SPATIAL_X, Kakshya::DataModality::AUDIO_1D);

    EXPECT_FALSE(processor.has_binding(0));
}

TEST(PlotProcessorTest, DetachingDropsEveryBinding)
{
    Rig rig { 2, 2 };
    rig.processor->bind_callable(0, [](std::vector<double>&) { });
    rig.processor->bind_callable(1, [](std::vector<double>&) { });

    rig.container->set_default_processor(std::make_shared<PlotProcessor>());

    EXPECT_FALSE(rig.processor->has_binding(0));
    EXPECT_FALSE(rig.processor->has_binding(1));
}

TEST(PlotProcessorTest, IsNotProcessingOutsideAProcessCall)
{
    Rig rig { 2 };
    rig.processor->bind_callable(0, [&rig](std::vector<double>&) {
        EXPECT_TRUE(rig.processor->is_processing());
    });

    EXPECT_FALSE(rig.processor->is_processing());
    rig.process();
    EXPECT_FALSE(rig.processor->is_processing());
}

// =============================================================================
// Node buffer references
// =============================================================================

TEST(PlotProcessorNodeReferenceTest, BindingANodeTakesAReference)
{
    PlotProcessor processor;
    auto node = make_node();
    ASSERT_FALSE(holds_a_buffer_reference(node));

    processor.bind_node(0, node);

    EXPECT_TRUE(holds_a_buffer_reference(node));
}

TEST(PlotProcessorNodeReferenceTest, UnbindingReleasesTheReference)
{
    PlotProcessor processor;
    auto node = make_node();
    processor.bind_node(0, node);

    processor.unbind(0);

    EXPECT_FALSE(holds_a_buffer_reference(node));
}

TEST(PlotProcessorNodeReferenceTest, DetachingReleasesTheReference)
{
    Rig rig { 2 };
    auto node = make_node();
    rig.processor->bind_node(0, node);
    ASSERT_TRUE(holds_a_buffer_reference(node));

    rig.container->set_default_processor(std::make_shared<PlotProcessor>());

    EXPECT_FALSE(holds_a_buffer_reference(node));
}

TEST(PlotProcessorNodeReferenceTest, RebindingToAnotherSourceKindReleasesTheReference)
{
    PlotProcessor processor;
    auto audio = std::make_shared<Buffers::AudioBuffer>(0, 2);

    for (int kind = 0; kind < 3; ++kind) {
        auto node = make_node();
        processor.bind_node(0, node);
        ASSERT_TRUE(holds_a_buffer_reference(node)) << kind;

        if (kind == 0)
            processor.bind_callable(0, [](std::vector<double>&) { });
        else if (kind == 1)
            processor.bind_audio_buffer(0, audio);
        else
            processor.set_raw(0, { 1.0 });

        EXPECT_FALSE(holds_a_buffer_reference(node)) << kind;
    }
}

TEST(PlotProcessorNodeReferenceTest, RebindingToAnotherNodeMovesTheReference)
{
    PlotProcessor processor;
    auto first = make_node();
    auto second = make_node();
    processor.bind_node(0, first);

    processor.bind_node(0, second);

    EXPECT_FALSE(holds_a_buffer_reference(first));
    EXPECT_TRUE(holds_a_buffer_reference(second));
}

TEST(PlotProcessorNodeReferenceTest, RebindingTheSameNodeKeepsExactlyOneReference)
{
    PlotProcessor processor;
    auto node = make_node();

    processor.bind_node(0, node);
    processor.bind_node(0, node);
    EXPECT_TRUE(holds_a_buffer_reference(node));

    processor.unbind(0);
    EXPECT_FALSE(holds_a_buffer_reference(node));
}

}
