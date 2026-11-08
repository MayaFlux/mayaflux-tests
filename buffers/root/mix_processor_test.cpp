#include "gtest/gtest.h"

#include "MayaFlux/Buffers/AudioBuffer.hpp"
#include "MayaFlux/Buffers/Root/MixProcessor.hpp"
#include "MayaFlux/Buffers/Root/RootAudioBuffer.hpp"

namespace MayaFlux::Test {

using Buffers::AudioBuffer;
using Buffers::MixProcessor;
using Buffers::RootAudioBuffer;

namespace {

    class MixProcessorFixture : public ::testing::Test {
    protected:
        void SetUp() override
        {
            root = std::make_shared<RootAudioBuffer>(0);
            root->set_mix_normalization(false);
            size = root->get_num_samples();
        }

        std::shared_ptr<AudioBuffer> make_source(double value, uint32_t samples)
        {
            auto source = std::make_shared<AudioBuffer>();
            source->resize(samples);
            std::ranges::fill(source->get_data(), value);
            held.push_back(source);
            return source;
        }

        void run()
        {
            mix.processing_function(root);
        }

        std::shared_ptr<RootAudioBuffer> root;
        uint32_t size { 0 };
        MixProcessor mix;
        std::vector<std::shared_ptr<AudioBuffer>> held;
    };

}

using MixProcessorTest = MixProcessorFixture;

TEST_F(MixProcessorTest, WithNoSourcesTheRootIsUntouched)
{
    std::ranges::fill(root->get_data(), 0.3);

    run();

    EXPECT_EQ(root->get_data(), std::vector<double>(size, 0.3));
}

TEST_F(MixProcessorTest, ProcessingAddsEachSourceScaledByItsLevelOntoTheExistingData)
{
    std::ranges::fill(root->get_data(), 0.1);
    ASSERT_TRUE(mix.register_source(make_source(1.0, size), 0.5));
    ASSERT_TRUE(mix.register_source(make_source(0.5, size), 1.0));

    run();

    EXPECT_DOUBLE_EQ(root->get_data().front(), 0.1 + 0.5 + 0.5);
    EXPECT_DOUBLE_EQ(root->get_data().back(), 0.1 + 0.5 + 0.5);
}

TEST_F(MixProcessorTest, NormalizationDividesTheSumByTheSourceCount)
{
    root->set_mix_normalization(true);
    ASSERT_TRUE(mix.register_source(make_source(1.0, size)));
    ASSERT_TRUE(mix.register_source(make_source(1.0, size)));

    run();

    EXPECT_DOUBLE_EQ(root->get_data().front(), 1.0);
    EXPECT_DOUBLE_EQ(root->get_data().back(), 1.0);
}

TEST_F(MixProcessorTest, ASourceShorterThanTheRootOnlyAffectsItsOwnLength)
{
    ASSERT_TRUE(mix.register_source(make_source(1.0, 4)));

    run();

    EXPECT_EQ(root->get_data()[3], 1.0);
    EXPECT_EQ(root->get_data()[4], 0.0);
}

TEST_F(MixProcessorTest, ASourceLongerThanTheRootIsCutAtItsLength)
{
    ASSERT_TRUE(mix.register_source(make_source(1.0, size + 64)));

    EXPECT_NO_THROW(run());

    EXPECT_EQ(root->get_data().size(), size);
    EXPECT_EQ(root->get_data().back(), 1.0);
}

TEST_F(MixProcessorTest, AZeroLevelSourceContributesNothing)
{
    ASSERT_TRUE(mix.register_source(make_source(1.0, size), 0.0));

    run();

    EXPECT_EQ(root->get_data(), std::vector<double>(size, 0.0));
}

TEST_F(MixProcessorTest, RegisteringTheSameSourceAgainUpdatesItsLevelWithoutDoubling)
{
    const auto source = make_source(1.0, size);
    ASSERT_TRUE(mix.register_source(source, 0.5));

    EXPECT_TRUE(mix.register_source(source, 0.25));
    run();

    EXPECT_DOUBLE_EQ(root->get_data().front(), 0.25);
}

TEST_F(MixProcessorTest, RegisteringANullOrEmptySourceIsRefused)
{
    const auto empty = make_source(1.0, 0);

    EXPECT_FALSE(mix.register_source(nullptr));
    EXPECT_FALSE(mix.register_source(empty));
}

TEST_F(MixProcessorTest, RemovingASourceStopsItsContribution)
{
    const auto source = make_source(1.0, size);
    ASSERT_TRUE(mix.register_source(source, 0.5));

    EXPECT_TRUE(mix.remove_source(source));
    EXPECT_FALSE(mix.remove_source(source));
    EXPECT_FALSE(mix.remove_source(nullptr));
    run();

    EXPECT_EQ(root->get_data(), std::vector<double>(size, 0.0));
}

TEST_F(MixProcessorTest, UpdatingALevelChangesOnlyTheKnownSource)
{
    const auto known = make_source(1.0, size);
    const auto stranger = make_source(1.0, size);
    ASSERT_TRUE(mix.register_source(known, 0.5));

    EXPECT_TRUE(mix.update_source_mix(known, 1.0));
    EXPECT_FALSE(mix.update_source_mix(stranger, 1.0));
    run();

    EXPECT_DOUBLE_EQ(root->get_data().front(), 1.0);
}

TEST_F(MixProcessorTest, AOnceSourceContributesOneCycleAndLeaves)
{
    ASSERT_TRUE(mix.register_source(make_source(1.0, size), 1.0, true));

    run();
    EXPECT_DOUBLE_EQ(root->get_data().front(), 1.0);

    std::ranges::fill(root->get_data(), 0.0);
    run();
    EXPECT_DOUBLE_EQ(root->get_data().front(), 0.0);
}

TEST_F(MixProcessorTest, ASourceThatIsNotOnceStaysAcrossCycles)
{
    ASSERT_TRUE(mix.register_source(make_source(1.0, size), 0.5));

    run();
    std::ranges::fill(root->get_data(), 0.0);
    run();

    EXPECT_DOUBLE_EQ(root->get_data().front(), 0.5);
}

TEST_F(MixProcessorTest, ASourceDestroyedBeforeProcessingIsDroppedWithoutContributing)
{
    auto source = std::make_shared<AudioBuffer>();
    source->resize(size);
    std::ranges::fill(source->get_data(), 1.0);
    ASSERT_TRUE(mix.register_source(source, 1.0));
    source.reset();

    EXPECT_NO_THROW(run());

    EXPECT_EQ(root->get_data(), std::vector<double>(size, 0.0));
}

TEST_F(MixProcessorTest, DroppingEverySourceWithNormalizationOnLeavesNoNaN)
{
    root->set_mix_normalization(true);
    auto source = std::make_shared<AudioBuffer>();
    source->resize(size);
    std::ranges::fill(source->get_data(), 1.0);
    ASSERT_TRUE(mix.register_source(source, 1.0));
    source.reset();

    run();

    EXPECT_TRUE(std::ranges::all_of(root->get_data(), [](double sample) { return std::isfinite(sample); }));
}

TEST_F(MixProcessorTest, ProcessingANonRootBufferDoesNothing)
{
    ASSERT_TRUE(mix.register_source(make_source(1.0, size), 1.0));
    const auto plain = std::make_shared<AudioBuffer>();
    plain->resize(size);

    mix.processing_function(plain);

    EXPECT_EQ(std::ranges::count(plain->get_data(), 0.0), static_cast<std::ptrdiff_t>(size));
    run();
    EXPECT_DOUBLE_EQ(root->get_data().front(), 1.0);
}

}
