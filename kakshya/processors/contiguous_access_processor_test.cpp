#include "gtest/gtest.h"

#include "MayaFlux/Kakshya/Processors/ContiguousAccessProcessor.hpp"
#include "MayaFlux/Kakshya/Source/SoundFileContainer.hpp"

namespace MayaFlux::Test {
namespace {

    using Kakshya::ContiguousAccessProcessor;
    using Kakshya::DataVariant;
    using Kakshya::OrganizationStrategy;
    using Kakshya::ProcessingState;
    using Kakshya::SoundFileContainer;

    constexpr uint32_t k_rate = 44100;
    constexpr uint64_t k_frames = 4;

    double sample(uint64_t frame, uint32_t channel)
    {
        return 1.0 + 10.0 * static_cast<double>(frame) + static_cast<double>(channel);
    }

    std::vector<double> flat_block(uint32_t channels, uint64_t first, uint64_t frames)
    {
        std::vector<double> flat;
        for (uint64_t f = first; f < first + frames; ++f) {
            for (uint32_t c = 0; c < channels; ++c) {
                flat.push_back(sample(f, c));
            }
        }
        return flat;
    }

    std::vector<double> channel_block(uint32_t channel, uint64_t first, uint64_t frames)
    {
        std::vector<double> values;
        for (uint64_t f = first; f < first + frames; ++f) {
            values.push_back(sample(f, channel));
        }
        return values;
    }

    std::vector<DataVariant> ramp_variants(OrganizationStrategy organization, uint32_t channels, uint64_t frames)
    {
        if (organization == OrganizationStrategy::INTERLEAVED) {
            return { DataVariant(flat_block(channels, 0, frames)) };
        }
        std::vector<DataVariant> variants;
        for (uint32_t c = 0; c < channels; ++c) {
            variants.emplace_back(channel_block(c, 0, frames));
        }
        return variants;
    }

    void expect_block(const std::vector<DataVariant>& processed, OrganizationStrategy organization,
        uint32_t channels, uint64_t first, uint64_t frames)
    {
        if (organization == OrganizationStrategy::INTERLEAVED) {
            ASSERT_EQ(processed.size(), 1U);
            EXPECT_EQ(std::get<std::vector<double>>(processed[0]), flat_block(channels, first, frames));
            return;
        }
        ASSERT_EQ(processed.size(), channels);
        for (uint32_t c = 0; c < channels; ++c) {
            EXPECT_EQ(std::get<std::vector<double>>(processed[c]), channel_block(c, first, frames)) << c;
        }
    }

    struct StreamShape {
        OrganizationStrategy organization;
        uint32_t channels;
    };

    std::string shape_name(const ::testing::TestParamInfo<StreamShape>& info)
    {
        return std::string(info.param.organization == OrganizationStrategy::INTERLEAVED ? "Interleaved" : "Planar")
            + std::to_string(info.param.channels) + "ch";
    }

    const auto k_shapes = ::testing::Values(
        StreamShape { OrganizationStrategy::INTERLEAVED, 1 },
        StreamShape { OrganizationStrategy::INTERLEAVED, 2 },
        StreamShape { OrganizationStrategy::INTERLEAVED, 3 },
        StreamShape { OrganizationStrategy::PLANAR, 1 },
        StreamShape { OrganizationStrategy::PLANAR, 2 },
        StreamShape { OrganizationStrategy::PLANAR, 3 });

    class ContiguousAccessProcessorFileTest : public ::testing::TestWithParam<StreamShape> {
    protected:
        void SetUp() override
        {
            container = std::make_shared<SoundFileContainer>(k_rate, channels(), 0);
            container->setup(k_frames, k_rate, channels());
            container->get_structure().organization = organization();
            container->set_raw_data(ramp_variants(organization(), channels(), k_frames));
            processor = std::make_shared<ContiguousAccessProcessor>();
        }

        [[nodiscard]] uint32_t channels() const { return GetParam().channels; }
        [[nodiscard]] OrganizationStrategy organization() const { return GetParam().organization; }

        void attach(const std::vector<uint64_t>& shape)
        {
            processor->set_output_size(shape);
            processor->on_attach(container);
        }

        void run() { processor->process(container); }

        void expect_processed(uint64_t first, uint64_t frames) const
        {
            expect_block(container->get_processed_data(), organization(), channels(), first, frames);
        }

        [[nodiscard]] std::vector<uint64_t> read_position() const
        {
            return container->get_read_position();
        }

        std::shared_ptr<SoundFileContainer> container;
        std::shared_ptr<ContiguousAccessProcessor> processor;
    };

    INSTANTIATE_TEST_SUITE_P(Shapes, ContiguousAccessProcessorFileTest, k_shapes, shape_name);

    TEST_P(ContiguousAccessProcessorFileTest, AttachingNothingChangesNothing)
    {
        EXPECT_NO_THROW(processor->on_attach(nullptr));

        run();

        EXPECT_TRUE(container->get_processed_data().empty());
    }

    TEST_P(ContiguousAccessProcessorFileTest, AttachingMarksTheContainerReady)
    {
        attach({ 2, channels() });

        EXPECT_EQ(container->get_processing_state(), ProcessingState::READY);
        EXPECT_TRUE(container->is_ready_for_processing());
    }

    TEST_P(ContiguousAccessProcessorFileTest, ProcessingBeforeAttachingDoesNothing)
    {
        run();

        EXPECT_TRUE(container->get_processed_data().empty());
        EXPECT_FALSE(processor->is_processing());
    }

    TEST_P(ContiguousAccessProcessorFileTest, ProcessingAfterDetachingDoesNothing)
    {
        attach({ 2, channels() });
        processor->on_detach(container);

        run();

        EXPECT_TRUE(container->get_processed_data().empty());
    }

    TEST_P(ContiguousAccessProcessorFileTest, AnotherContainerIsLeftAlone)
    {
        attach({ 2, channels() });
        auto other = std::make_shared<SoundFileContainer>(k_rate, channels(), 0);
        other->setup(k_frames, k_rate, channels());
        other->get_structure().organization = organization();
        other->set_raw_data(ramp_variants(organization(), channels(), k_frames));

        processor->process(other);

        EXPECT_TRUE(other->get_processed_data().empty());
        EXPECT_TRUE(container->get_processed_data().empty());
    }

    TEST_P(ContiguousAccessProcessorFileTest, ProcessingFillsTheFirstBlock)
    {
        attach({ 2, channels() });

        run();

        expect_processed(0, 2);
    }

    TEST_P(ContiguousAccessProcessorFileTest, ProcessingMarksTheContainerProcessed)
    {
        attach({ 2, channels() });

        run();

        EXPECT_EQ(container->get_processing_state(), ProcessingState::PROCESSED);
        EXPECT_FALSE(processor->is_processing());
    }

    TEST_P(ContiguousAccessProcessorFileTest, SuccessiveBlocksFollowOneAnother)
    {
        attach({ 2, channels() });

        run();
        expect_processed(0, 2);
        run();
        expect_processed(2, 2);
    }

    TEST_P(ContiguousAccessProcessorFileTest, AdvancingWritesTheReadPositionBackToTheContainer)
    {
        attach({ 2, channels() });

        run();

        EXPECT_EQ(read_position(), std::vector<uint64_t>(channels(), 2));
    }

    TEST_P(ContiguousAccessProcessorFileTest, WithoutAutoAdvanceTheSameBlockRepeats)
    {
        processor->set_auto_advance(false);
        attach({ 2, channels() });

        run();
        run();

        expect_processed(0, 2);
        EXPECT_EQ(read_position(), std::vector<uint64_t>(channels(), 0));
    }

    TEST_P(ContiguousAccessProcessorFileTest, ReachingTheEndHoldsOnTheLastFrame)
    {
        attach({ 2, channels() });

        run();
        run();

        EXPECT_EQ(read_position(), std::vector<uint64_t>(channels(), k_frames - 1));
    }

    TEST_P(ContiguousAccessProcessorFileTest, ALoopingStreamWrapsToTheLoopStart)
    {
        container->set_looping(true);
        attach({ 2, channels() });

        run();
        run();
        run();

        expect_processed(0, 2);
        EXPECT_EQ(read_position(), std::vector<uint64_t>(channels(), 2));
    }

    TEST_P(ContiguousAccessProcessorFileTest, TheDefaultBlockCoversTheWholeStream)
    {
        processor->on_attach(container);

        run();

        expect_processed(0, k_frames);
    }

    TEST_P(ContiguousAccessProcessorFileTest, TheContainerReadPositionIsPickedUpOnAttach)
    {
        container->set_read_position(std::vector<uint64_t>(channels(), 2));
        attach({ 2, channels() });

        run();

        expect_processed(2, 2);
    }

    TEST_P(ContiguousAccessProcessorFileTest, ASetPositionMovesTheBlock)
    {
        attach({ 2, channels() });

        processor->set_current_position(std::vector<uint64_t>(channels(), 2));
        run();

        expect_processed(2, 2);
    }

    TEST_P(ContiguousAccessProcessorFileTest, ChangingTheOutputSizeAfterAttachingTakesEffect)
    {
        attach({ 2, channels() });

        processor->set_output_size({ 1, channels() });
        run();

        expect_processed(0, 1);
    }

    TEST_P(ContiguousAccessProcessorFileTest, ABadOutputSizeAfterAttachingIsRefused)
    {
        attach({ 2, channels() });

        EXPECT_THROW(processor->set_output_size({ k_frames + 1, channels() }), std::runtime_error);
    }

    TEST_P(ContiguousAccessProcessorFileTest, AFewerChannelBlockTakesTheLeadingChannels)
    {
        if (channels() == 1) {
            GTEST_SKIP() << "a mono stream has no fewer channels to take";
        }
        attach({ 2, 1 });

        run();

        const auto& processed = container->get_processed_data();
        ASSERT_EQ(processed.size(), 1U);
        if (organization() == OrganizationStrategy::INTERLEAVED) {
            EXPECT_EQ(std::get<std::vector<double>>(processed[0]), (std::vector<double> { sample(0, 0), sample(1, 0) }));
        } else {
            EXPECT_EQ(std::get<std::vector<double>>(processed[0]), channel_block(0, 0, 2));
        }
    }

    TEST_P(ContiguousAccessProcessorFileTest, AnOutputOfTheWrongRankIsRefused)
    {
        EXPECT_THROW(attach({ 2 }), std::runtime_error);
        EXPECT_THROW(attach({ 2, channels(), 1 }), std::runtime_error);
    }

    TEST_P(ContiguousAccessProcessorFileTest, AZeroSizedOutputIsRefused)
    {
        EXPECT_THROW(attach({ 0, channels() }), std::runtime_error);
        EXPECT_THROW(attach({ 2, 0 }), std::runtime_error);
    }

    TEST_P(ContiguousAccessProcessorFileTest, MoreFramesThanTheStreamHoldsIsRefused)
    {
        EXPECT_THROW(attach({ k_frames + 1, channels() }), std::runtime_error);
    }

    TEST_P(ContiguousAccessProcessorFileTest, MoreChannelsThanTheStreamHoldsIsRefused)
    {
        EXPECT_THROW(attach({ 2, channels() + 1 }), std::runtime_error);
    }

    TEST_P(ContiguousAccessProcessorFileTest, ARefusedAttachLeavesTheProcessorUnprepared)
    {
        EXPECT_THROW(attach({ 0, channels() }), std::runtime_error);

        run();

        EXPECT_TRUE(container->get_processed_data().empty());
    }
} // namespace
} // namespace MayaFlux::Test
