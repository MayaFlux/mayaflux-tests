#include "../../test_config.h"

#include "MayaFlux/Buffers/AudioBuffer.hpp"
#include "MayaFlux/Kakshya/NDData/DataAccess.hpp"
#include "MayaFlux/Kakshya/Source/PlotContainer.hpp"
#include "MayaFlux/Nodes/Generators/Sine.hpp"
#include "MayaFlux/Portal/Forma/Plot/PlotSource.hpp"

namespace MayaFlux::Test {

namespace Plot = Portal::Forma::Plot;
using Kakshya::DataModality;
using Role = Kakshya::DataDimension::Role;

namespace {

    std::vector<double> processed(const std::shared_ptr<Kakshya::PlotContainer>& container, size_t index)
    {
        return std::get<std::vector<double>>(container->get_processed_data()[index]);
    }

    std::function<void(std::vector<double>&)> constant(double value)
    {
        return [value](std::vector<double>& out) { std::ranges::fill(out, value); };
    }

}

TEST(FormaPlotSourceTest, AsAddsANamedSeriesWithItsSizeAndRole)
{
    auto container = Plot::Source()
                         .as("left", 8, Role::SPATIAL_X, DataModality::AUDIO_1D)
                         .as("right", 4, Role::SPATIAL_Y, DataModality::AUDIO_1D)
                         .build();

    ASSERT_EQ(container->series_count(), 2U);
    EXPECT_EQ(container->series_name(0), "left");
    EXPECT_EQ(container->series_size(0), 8U);
    EXPECT_EQ(container->series_role(0), Role::SPATIAL_X);
    EXPECT_EQ(container->series_name(1), "right");
    EXPECT_EQ(container->series_size(1), 4U);
    EXPECT_EQ(container->series_role(1), Role::SPATIAL_Y);
}

TEST(FormaPlotSourceTest, EverySetterReturnsTheSameBuilder)
{
    Plot::Source source;

    EXPECT_EQ(&source.as("s", 2, Role::SPATIAL_Y, DataModality::AUDIO_1D), &source);
    EXPECT_EQ(&source.from(constant(1.0)), &source);
}

TEST(FormaPlotSourceTest, FromBindsACallableToTheSeriesJustAdded)
{
    auto container = Plot::Source()
                         .as("one", 3, Role::SPATIAL_Y, DataModality::AUDIO_1D)
                         .from(constant(1.0))
                         .as("two", 3, Role::SPATIAL_Y, DataModality::AUDIO_1D)
                         .from(constant(2.0))
                         .build();

    container->process_default();

    EXPECT_EQ(processed(container, 0), (std::vector<double> { 1.0, 1.0, 1.0 }));
    EXPECT_EQ(processed(container, 1), (std::vector<double> { 2.0, 2.0, 2.0 }));
}

TEST(FormaPlotSourceTest, ASeriesWithoutASourceStaysAtZeroButIsStillCopiedThrough)
{
    auto container = Plot::Source()
                         .as("bound", 2, Role::SPATIAL_Y, DataModality::AUDIO_1D)
                         .from(constant(5.0))
                         .as("free", 2, Role::SPATIAL_Y, DataModality::AUDIO_1D)
                         .build();

    container->process_default();

    EXPECT_EQ(processed(container, 0), (std::vector<double> { 5.0, 5.0 }));
    EXPECT_EQ(processed(container, 1), (std::vector<double> { 0.0, 0.0 }));
}

TEST(FormaPlotSourceTest, FromAnAudioBufferBindsItsSamples)
{
    auto audio = std::make_shared<Buffers::AudioBuffer>(0, 3);
    audio->get_data() = { 0.25, 0.5, 0.75 };

    auto container = Plot::Source()
                         .as("audio", 3, Role::SPATIAL_Y, DataModality::AUDIO_1D)
                         .from(audio)
                         .build();
    container->process_default();

    EXPECT_EQ(processed(container, 0), (std::vector<double> { 0.25, 0.5, 0.75 }));
}

TEST(FormaPlotSourceTest, FromANodeSamplesTheNode)
{
    std::shared_ptr<Nodes::Node> sine = std::make_shared<Nodes::Generator::Sine>(440.F, 1.0);

    auto container = Plot::Source()
                         .as("sine", 32, Role::SPATIAL_Y, DataModality::AUDIO_1D)
                         .from(sine)
                         .build();
    container->process_default();

    const auto samples = processed(container, 0);
    EXPECT_EQ(samples.size(), 32U);
    EXPECT_TRUE(std::ranges::any_of(samples, [](double v) { return v != 0.0; }));
}

TEST(FormaPlotSourceTest, FromBeforeAnySeriesExistsBindsNothing)
{
    auto container = Plot::Source().from(constant(1.0)).build();

    EXPECT_EQ(container->series_count(), 0U);
    EXPECT_EQ(container->get_default_processor(), nullptr);
}

TEST(FormaPlotSourceTest, BuildHandsOverTheContainerOnce)
{
    Plot::Source source;
    source.as("s", 2, Role::SPATIAL_Y, DataModality::AUDIO_1D);

    const auto first = source.build();
    const auto second = source.build();

    EXPECT_NE(first, nullptr);
    EXPECT_EQ(second, nullptr);
}

TEST(FormaPlotSourceTest, AnEmptyBuilderBuildsAnEmptyContainer)
{
    auto container = Plot::Source().build();

    ASSERT_NE(container, nullptr);
    EXPECT_EQ(container->series_count(), 0U);
    EXPECT_FALSE(container->has_data());
}

}
