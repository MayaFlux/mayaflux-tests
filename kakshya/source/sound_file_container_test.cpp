#include "gtest/gtest.h"

#include "MayaFlux/Kakshya/Processors/ContiguousAccessProcessor.hpp"
#include "MayaFlux/Kakshya/Source/SoundFileContainer.hpp"

namespace MayaFlux::Test {
namespace {

    using Kakshya::ContiguousAccessProcessor;
    using Kakshya::make_sound_file_container;
    using Kakshya::OrganizationStrategy;
    using Kakshya::ProcessingState;

    constexpr uint32_t k_rate = 44100;
    constexpr uint64_t k_frames = 8;

    double sample(uint64_t frame, uint32_t channel)
    {
        return 1.0 + 10.0 * static_cast<double>(frame) + static_cast<double>(channel);
    }

    std::vector<std::vector<double>> channel_data(uint32_t channels, uint64_t frames = k_frames)
    {
        std::vector<std::vector<double>> data(channels);
        for (uint32_t c = 0; c < channels; ++c) {
            for (uint64_t f = 0; f < frames; ++f) {
                data[c].push_back(sample(f, c));
            }
        }
        return data;
    }

    std::vector<double> expected_frame(uint32_t channels, uint64_t frame)
    {
        std::vector<double> values;
        for (uint32_t c = 0; c < channels; ++c) {
            values.push_back(sample(frame, c));
        }
        return values;
    }

    struct StreamShape {
        OrganizationStrategy organization;
        uint32_t channels;
    };

    class MakeSoundFileContainerTest : public ::testing::TestWithParam<StreamShape> {
    protected:
        void SetUp() override
        {
            container = make_sound_file_container(channel_data(channels()), channels(), k_rate, organization());
        }

        [[nodiscard]] OrganizationStrategy organization() const { return GetParam().organization; }
        [[nodiscard]] uint32_t channels() const { return GetParam().channels; }

        std::shared_ptr<Kakshya::SoundFileContainer> container;
    };

    INSTANTIATE_TEST_SUITE_P(Shapes, MakeSoundFileContainerTest,
        ::testing::Values(
            StreamShape { OrganizationStrategy::INTERLEAVED, 1 },
            StreamShape { OrganizationStrategy::INTERLEAVED, 2 },
            StreamShape { OrganizationStrategy::INTERLEAVED, 3 },
            StreamShape { OrganizationStrategy::PLANAR, 1 },
            StreamShape { OrganizationStrategy::PLANAR, 2 },
            StreamShape { OrganizationStrategy::PLANAR, 3 }),
        [](const ::testing::TestParamInfo<StreamShape>& info) {
            return std::string(info.param.organization == OrganizationStrategy::INTERLEAVED ? "Interleaved" : "Planar")
                + std::to_string(info.param.channels) + "ch";
        });

    TEST_P(MakeSoundFileContainerTest, TheContainerReportsWhatItWasBuiltFrom)
    {
        ASSERT_NE(container, nullptr);
        EXPECT_EQ(container->get_num_frames(), k_frames);
        EXPECT_EQ(container->get_num_channels(), channels());
        EXPECT_EQ(container->get_sample_rate(), k_rate);
        EXPECT_EQ(container->get_total_elements(), k_frames * channels());
        EXPECT_EQ(container->get_structure().organization, organization());
        EXPECT_TRUE(container->has_data());
    }

    TEST_P(MakeSoundFileContainerTest, EveryFrameReadsBackPerChannel)
    {
        for (uint64_t f = 0; f < k_frames; ++f) {
            const auto values = container->get_frame(f).as<double>();
            EXPECT_EQ(std::vector<double>(values.begin(), values.end()), expected_frame(channels(), f)) << f;
        }
    }

    TEST_P(MakeSoundFileContainerTest, ItIsReadyWithAContiguousProcessorAttached)
    {
        EXPECT_EQ(container->get_processing_state(), ProcessingState::READY);
        EXPECT_TRUE(container->is_ready_for_processing());
        EXPECT_NE(std::dynamic_pointer_cast<ContiguousAccessProcessor>(container->get_default_processor()), nullptr);
    }

    TEST_P(MakeSoundFileContainerTest, TheDefaultProcessorYieldsTheWholeStreamAsOneBlock)
    {
        container->process_default();

        EXPECT_EQ(container->get_processing_state(), ProcessingState::PROCESSED);
        const auto& processed = container->get_processed_data();

        if (organization() == OrganizationStrategy::INTERLEAVED) {
            ASSERT_EQ(processed.size(), 1U);
            std::vector<double> flat;
            for (uint64_t f = 0; f < k_frames; ++f) {
                const auto values = expected_frame(channels(), f);
                flat.insert(flat.end(), values.begin(), values.end());
            }
            EXPECT_EQ(std::get<std::vector<double>>(processed[0]), flat);
            return;
        }

        ASSERT_EQ(processed.size(), channels());
        for (uint32_t c = 0; c < channels(); ++c) {
            EXPECT_EQ(std::get<std::vector<double>>(processed[c]), channel_data(channels())[c]) << c;
        }
    }

    TEST(MakeSoundFileContainerDefaultsTest, TheRateIs48kAndTheLayoutIsPlanar)
    {
        const auto container = make_sound_file_container(channel_data(2), 2);

        ASSERT_NE(container, nullptr);
        EXPECT_EQ(container->get_sample_rate(), 48000U);
        EXPECT_EQ(container->get_structure().organization, OrganizationStrategy::PLANAR);
    }

    TEST(MakeSoundFileContainerDefaultsTest, NoChannelsAreRefused)
    {
        EXPECT_THROW((void)make_sound_file_container({}, 0), std::invalid_argument);
    }

    TEST(MakeSoundFileContainerDefaultsTest, AnEmptyFirstChannelIsRefused)
    {
        EXPECT_THROW((void)make_sound_file_container({ {}, { 1.0 } }, 2), std::invalid_argument);
    }

    TEST(MakeSoundFileContainerDefaultsTest, AnyEmptyChannelIsRefused)
    {
        EXPECT_THROW((void)make_sound_file_container({ { 1.0 }, {} }, 2), std::invalid_argument);
    }

    TEST(MakeSoundFileContainerDefaultsTest, AChannelCountThatDoesNotMatchTheDataIsRefused)
    {
        EXPECT_THROW((void)make_sound_file_container(channel_data(2), 3), std::invalid_argument);
        EXPECT_THROW((void)make_sound_file_container(channel_data(2), 1), std::invalid_argument);
    }

} // namespace
} // namespace MayaFlux::Test
