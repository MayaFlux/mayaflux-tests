#include "gtest/gtest.h"

#include "audio_container.hpp"

#include "MayaFlux/Yantra/OperationSpec/OperationHelper.hpp"

#include <Eigen/Dense>

namespace MayaFlux::Test {
namespace {

    using Kakshya::DataVariant;
    using Kakshya::SignalSourceContainer;
    using Yantra::NumericChannels;
    using Yantra::OperationHelper;

    constexpr size_t k_frames = 64;

    std::vector<std::vector<double>> ramps(size_t channels)
    {
        std::vector<std::vector<double>> data(channels, std::vector<double>(k_frames));
        for (size_t ch = 0; ch < channels; ++ch) {
            for (size_t i = 0; i < k_frames; ++i) {
                data[ch][i] = static_cast<double>(ch + 1) * static_cast<double>(i + 1);
            }
        }
        return data;
    }

    void disturb_the_heap()
    {
        std::vector<std::vector<double>> noise;
        for (size_t size : { 2, 8, 16, 64, 128, 256, 1024 }) {
            for (int repeat = 0; repeat < 8; ++repeat) {
                noise.emplace_back(size, -999.0);
            }
        }
    }

    class NumericChannelsContainerTest : public ::testing::TestWithParam<size_t> {
    protected:
        void SetUp() override
        {
            expected = ramps(GetParam());
            container = make_audio_container(expected);
        }

        std::vector<std::vector<double>> expected;
        std::shared_ptr<Kakshya::SoundFileContainer> container;
    };

    INSTANTIATE_TEST_SUITE_P(ChannelCounts, NumericChannelsContainerTest,
        ::testing::Values(1U, 2U, 3U),
        [](const ::testing::TestParamInfo<size_t>& info) {
            return std::to_string(info.param) + "Channels";
        });

    TEST_P(NumericChannelsContainerTest, OneChannelIsReturnedPerContainerChannel)
    {
        const std::shared_ptr<SignalSourceContainer> source = container;
        const NumericChannels channels = OperationHelper::extract_numeric_data(source, false);

        ASSERT_EQ(channels.size(), GetParam());
        for (size_t ch = 0; ch < channels.size(); ++ch) {
            EXPECT_EQ(channels[ch].size(), k_frames);
        }
    }

    TEST_P(NumericChannelsContainerTest, DoubleChannelsAreViewedInTheContainersOwnStorage)
    {
        const std::shared_ptr<SignalSourceContainer> source = container;
        const NumericChannels channels = OperationHelper::extract_numeric_data(source, false);
        const auto& variants = source->get_data();

        ASSERT_EQ(channels.size(), variants.size());
        for (size_t ch = 0; ch < channels.size(); ++ch) {
            EXPECT_EQ(channels[ch].data(), std::get<std::vector<double>>(variants[ch]).data());
        }
    }

    TEST_P(NumericChannelsContainerTest, TheValuesStillMatchAfterTheHeapIsReused)
    {
        const std::shared_ptr<SignalSourceContainer> source = container;
        const NumericChannels channels = OperationHelper::extract_numeric_data(source, false);

        disturb_the_heap();

        ASSERT_EQ(channels.size(), expected.size());
        for (size_t ch = 0; ch < channels.size(); ++ch) {
            EXPECT_EQ(std::vector<double>(channels[ch].begin(), channels[ch].end()), expected[ch]);
        }
    }

    TEST_P(NumericChannelsContainerTest, StructuredExtractionKeepsItsValuesToo)
    {
        Yantra::Datum<std::shared_ptr<SignalSourceContainer>> datum(container);
        auto [channels, info] = OperationHelper::extract_structured_double(datum);

        disturb_the_heap();

        ASSERT_EQ(channels.size(), expected.size());
        for (size_t ch = 0; ch < channels.size(); ++ch) {
            EXPECT_EQ(std::vector<double>(channels[ch].begin(), channels[ch].end()), expected[ch]);
        }
    }

    class NumericChannelsEigenTest : public ::testing::Test { };

    TEST_F(NumericChannelsEigenTest, TwoLiveExtractionsDoNotShareStorage)
    {
        Eigen::MatrixXd first(4, 2);
        first << 1, 5, 2, 6, 3, 7, 4, 8;
        Eigen::MatrixXd second(4, 2);
        second << -1, -5, -2, -6, -3, -7, -4, -8;

        const NumericChannels a = OperationHelper::extract_numeric_data(first);
        const NumericChannels b = OperationHelper::extract_numeric_data(second);

        ASSERT_EQ(a.size(), 2U);
        ASSERT_EQ(b.size(), 2U);
        EXPECT_EQ(std::vector<double>(a[0].begin(), a[0].end()), (std::vector<double> { 1, 2, 3, 4 }));
        EXPECT_EQ(std::vector<double>(a[1].begin(), a[1].end()), (std::vector<double> { 5, 6, 7, 8 }));
        EXPECT_EQ(std::vector<double>(b[0].begin(), b[0].end()), (std::vector<double> { -1, -2, -3, -4 }));
        EXPECT_EQ(std::vector<double>(b[1].begin(), b[1].end()), (std::vector<double> { -5, -6, -7, -8 }));
    }

    TEST_F(NumericChannelsEigenTest, TheValuesSurviveACopyOfTheResult)
    {
        Eigen::MatrixXd matrix(3, 1);
        matrix << 1, 2, 3;

        NumericChannels copy;
        {
            const NumericChannels original = OperationHelper::extract_numeric_data(matrix);
            copy = original;
        }
        disturb_the_heap();

        ASSERT_EQ(copy.size(), 1U);
        EXPECT_EQ(std::vector<double>(copy[0].begin(), copy[0].end()), (std::vector<double> { 1, 2, 3 }));
    }

} // namespace
} // namespace MayaFlux::Test
