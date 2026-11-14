#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Buffers/BufferUtils.hpp"
#include "MayaFlux/Nodes/Generators/Phasor.hpp"
#include "MayaFlux/Nodes/Generators/Polynomial.hpp"
#include "MayaFlux/Nodes/Generators/Sine.hpp"
#include "MayaFlux/Nodes/RootNode.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Nodes::RootNode;
    using Nodes::Generator::Phasor;
    using Nodes::Generator::Polynomial;
    using Nodes::Generator::Sine;

    constexpr uint32_t k_samples = 100;

    double reference_phase(uint32_t advances)
    {
        Phasor reference(1.0F, 1.0);
        for (uint32_t i = 0; i < advances; ++i) {
            reference.process_sample(0.0);
        }
        return reference.get_phase();
    }

    class ModulatorSharingTest : public ::testing::Test {
    protected:
        RootNode root;
        std::shared_ptr<Phasor> phasor { std::make_shared<Phasor>(1.0F, 1.0) };

        std::shared_ptr<Polynomial> decay(double rate, double scale = 1.0) const
        {
            auto node = std::make_shared<Polynomial>(
                [rate, scale](double x) { return scale * std::exp(-x * rate); });
            node->set_input_node(phasor);
            return node;
        }

        RootNode second_root { Nodes::ProcessingToken::AUDIO_RATE, 1 };

        static std::shared_ptr<Polynomial> link(const std::shared_ptr<Nodes::Node>& source, double rate = 25.0)
        {
            auto node = std::make_shared<Polynomial>(
                [rate](double x) { return std::exp(-x * rate); });
            node->set_input_node(source);
            return node;
        }

        static bool processed(const Nodes::Node& node)
        {
            return (node.m_state.load() & Nodes::NodeState::PROCESSED) != 0;
        }

        std::shared_ptr<Sine> routed_sine_on(RootNode& target, uint32_t channel)
        {
            auto sine = std::make_shared<Sine>(0.0F, 0.0);
            sine->register_channel_usage(channel);
            target.register_node(sine);
            return sine;
        }

        std::shared_ptr<Sine> routed_sine()
        {
            return routed_sine_on(root, 0);
        }

        static uint32_t mask_of(const Nodes::Node& node)
        {
            return node.get_channel_mask().load();
        }

        void run()
        {
            for (uint32_t i = 0; i < k_samples; ++i) {
                root.process_sample();
            }
        }
    };

    TEST_F(ModulatorSharingTest, SingleConsumerAdvancesSharedInputOncePerSample)
    {
        auto tone = routed_sine();
        tone->set_amplitude_modulator(decay(25.0));

        run();

        EXPECT_DOUBLE_EQ(phasor->get_phase(), reference_phase(k_samples));
    }

    TEST_F(ModulatorSharingTest, TwoModulatorsOfOneNodeAdvanceSharedInputOncePerSample)
    {
        auto tone = routed_sine();
        tone->set_amplitude_modulator(decay(25.0));
        tone->set_frequency_modulator(decay(20.0, 220.0));

        run();

        EXPECT_DOUBLE_EQ(phasor->get_phase(), reference_phase(k_samples));
    }

    TEST_F(ModulatorSharingTest, TwoConsumersOfOneModulatorAdvanceSharedInputOncePerSample)
    {
        auto envelope = decay(25.0);

        auto first = routed_sine();
        first->set_amplitude_modulator(envelope);
        auto second = routed_sine();
        second->set_amplitude_modulator(envelope);

        run();

        EXPECT_DOUBLE_EQ(phasor->get_phase(), reference_phase(k_samples));
    }

    TEST_F(ModulatorSharingTest, FullKickGraphAdvancesSharedInputOncePerSample)
    {
        auto envelope = decay(25.0);

        auto tone = routed_sine();
        tone->set_amplitude_modulator(envelope);
        tone->set_frequency_modulator(decay(20.0, 220.0));

        auto harmonic = routed_sine();
        harmonic->set_amplitude_modulator(envelope);
        harmonic->set_frequency_modulator(decay(20.0, 360.0));

        run();

        EXPECT_DOUBLE_EQ(phasor->get_phase(), reference_phase(k_samples));
    }

    TEST_F(ModulatorSharingTest, ModulatorsAttachedBeforeRegistrationAdvanceSharedInputOncePerSample)
    {
        auto tone = std::make_shared<Sine>(0.0F, 0.0);
        tone->set_amplitude_modulator(decay(25.0));
        tone->set_frequency_modulator(decay(20.0, 220.0));

        tone->register_channel_usage(0);
        root.register_node(tone);

        run();

        EXPECT_DOUBLE_EQ(phasor->get_phase(), reference_phase(k_samples));
    }

    TEST_F(ModulatorSharingTest, RemainingConsumerKeepsSharedInputAtOncePerSampleAfterAnotherLeaves)
    {
        auto envelope = decay(25.0);

        auto staying = routed_sine();
        staying->set_amplitude_modulator(envelope);
        auto leaving = routed_sine();
        leaving->set_amplitude_modulator(envelope);

        leaving->unregister_channel_usage(0);
        root.unregister_node(leaving);

        run();

        EXPECT_DOUBLE_EQ(phasor->get_phase(), reference_phase(k_samples));
    }

    TEST_F(ModulatorSharingTest, SharedModulatorAcrossChannelsAdvancesSharedInputOncePerFrame)
    {
        auto envelope = decay(25.0);

        auto left = routed_sine_on(root, 0);
        left->set_amplitude_modulator(envelope);
        auto right = routed_sine_on(second_root, 1);
        right->set_amplitude_modulator(envelope);

        for (uint32_t i = 0; i < k_samples; ++i) {
            root.process_sample();
            second_root.process_sample();
        }

        EXPECT_DOUBLE_EQ(phasor->get_phase(), reference_phase(k_samples));
    }

    TEST_F(ModulatorSharingTest, ModulatorInheritsEveryChannelOfItsConsumers)
    {
        auto envelope = decay(25.0);

        auto left = routed_sine_on(root, 0);
        left->set_amplitude_modulator(envelope);
        auto right = routed_sine_on(second_root, 1);
        right->set_amplitude_modulator(envelope);

        EXPECT_EQ(mask_of(*envelope), 0b11U);
        EXPECT_EQ(mask_of(*phasor), 0b11U);
    }

    TEST_F(ModulatorSharingTest, ChannelStaysWhileAnyConsumerOfTheModulatorRemains)
    {
        auto envelope = decay(25.0);

        auto first = routed_sine();
        first->set_amplitude_modulator(envelope);
        auto second = routed_sine();
        second->set_amplitude_modulator(envelope);

        first->unregister_channel_usage(0);
        EXPECT_EQ(mask_of(*envelope), 0b1U);
        EXPECT_EQ(mask_of(*phasor), 0b1U);

        second->unregister_channel_usage(0);
        EXPECT_EQ(mask_of(*envelope), 0U);
        EXPECT_EQ(mask_of(*phasor), 0U);
    }

    TEST_F(ModulatorSharingTest, ReplacingAModulatorWithdrawsItsChannels)
    {
        auto original = decay(25.0);
        auto replacement = std::make_shared<Polynomial>(
            [](double x) { return x; });

        auto tone = routed_sine();
        tone->set_amplitude_modulator(original);
        ASSERT_EQ(mask_of(*original), 0b1U);

        tone->set_amplitude_modulator(replacement);

        EXPECT_EQ(mask_of(*original), 0U);
        EXPECT_EQ(mask_of(*phasor), 0U);
        EXPECT_EQ(mask_of(*replacement), 0b1U);
    }

    TEST_F(ModulatorSharingTest, ClearingModulatorsWithdrawsTheirChannels)
    {
        auto envelope = decay(25.0);
        auto pitch = decay(20.0, 220.0);

        auto tone = routed_sine();
        tone->set_amplitude_modulator(envelope);
        tone->set_frequency_modulator(pitch);

        tone->clear_modulators();

        EXPECT_EQ(mask_of(*envelope), 0U);
        EXPECT_EQ(mask_of(*pitch), 0U);
        EXPECT_EQ(mask_of(*phasor), 0U);
    }

    TEST_F(ModulatorSharingTest, ExtractionBetweenRootCyclesDoesNotDisturbAModulatedNode)
    {
        auto routed_modulator = std::make_shared<Sine>(5.0F, 0.5);
        auto routed = std::make_shared<Sine>(routed_modulator, 880.0F, 0.5);
        auto reference_modulator = std::make_shared<Sine>(5.0F, 0.5);
        auto reference = std::make_shared<Sine>(reference_modulator, 880.0F, 0.5);

        routed->register_channel_usage(0);
        root.register_node(routed);

        for (int cycle = 0; cycle < 16; ++cycle) {
            EXPECT_DOUBLE_EQ(root.process_sample(), reference->process_sample(0.0)) << "cycle " << cycle;
            if (cycle == 2 || cycle == 6 || cycle == 9) {
                Buffers::extract_single_sample(routed);
                Buffers::extract_multiple_samples(routed, 5);
            }
        }
    }

    TEST_F(ModulatorSharingTest, ThreeLevelChainAdvancesItsLeafOncePerSample)
    {
        auto lower = link(phasor);
        auto upper = link(lower);

        auto tone = routed_sine();
        tone->set_amplitude_modulator(upper);

        run();

        EXPECT_DOUBLE_EQ(phasor->get_phase(), reference_phase(k_samples));
        EXPECT_EQ(mask_of(*upper), 0b1U);
        EXPECT_EQ(mask_of(*lower), 0b1U);
        EXPECT_EQ(mask_of(*phasor), 0b1U);
    }

    TEST_F(ModulatorSharingTest, DiamondWithUnevenPathsAdvancesTheLeafOncePerSample)
    {
        auto direct = link(phasor);
        auto indirect = link(link(phasor));

        auto tone = routed_sine();
        tone->set_frequency_modulator(direct);
        tone->set_amplitude_modulator(indirect);

        run();

        EXPECT_DOUBLE_EQ(phasor->get_phase(), reference_phase(k_samples));
    }

    TEST_F(ModulatorSharingTest, DiamondLeafIsCountedOncePerPath)
    {
        auto direct = link(phasor);
        auto indirect = link(link(phasor));

        auto tone = routed_sine();
        tone->set_frequency_modulator(direct);
        tone->set_amplitude_modulator(indirect);
        ASSERT_EQ(mask_of(*phasor), 0b1U);

        tone->set_frequency_modulator(nullptr);
        EXPECT_EQ(mask_of(*phasor), 0b1U);

        tone->set_amplitude_modulator(nullptr);
        EXPECT_EQ(mask_of(*phasor), 0U);
    }

    TEST_F(ModulatorSharingTest, NestedShareAcrossChannelsAdvancesTheLeafOncePerFrame)
    {
        auto middle = link(phasor);
        auto top = link(middle);

        auto left = routed_sine_on(root, 0);
        left->set_amplitude_modulator(top);
        auto right = routed_sine_on(second_root, 1);
        right->set_amplitude_modulator(middle);

        for (uint32_t i = 0; i < k_samples; ++i) {
            root.process_sample();
            second_root.process_sample();
        }

        EXPECT_DOUBLE_EQ(phasor->get_phase(), reference_phase(k_samples));
        EXPECT_EQ(mask_of(*top), 0b01U);
        EXPECT_EQ(mask_of(*middle), 0b11U);
        EXPECT_EQ(mask_of(*phasor), 0b11U);
    }

    TEST_F(ModulatorSharingTest, DeepSharedLeafKeepsItsChannelUntilTheLastConsumerLeaves)
    {
        auto middle = link(phasor);
        auto top = link(middle);

        auto through_top = routed_sine();
        through_top->set_amplitude_modulator(top);
        auto through_middle = routed_sine();
        through_middle->set_amplitude_modulator(middle);

        through_top->unregister_channel_usage(0);
        EXPECT_EQ(mask_of(*top), 0U);
        EXPECT_EQ(mask_of(*middle), 0b1U);
        EXPECT_EQ(mask_of(*phasor), 0b1U);

        through_middle->unregister_channel_usage(0);
        EXPECT_EQ(mask_of(*middle), 0U);
        EXPECT_EQ(mask_of(*phasor), 0U);
    }

    TEST_F(ModulatorSharingTest, AttachingBelowARoutedModulatorGivesTheNewNodeItsChannels)
    {
        auto middle = std::make_shared<Polynomial>([](double x) { return x; });
        auto tone = routed_sine();
        tone->set_amplitude_modulator(middle);
        ASSERT_EQ(mask_of(*middle), 0b1U);

        middle->set_input_node(phasor);

        EXPECT_EQ(mask_of(*phasor), 0b1U);

        middle->set_input_node(nullptr);

        EXPECT_EQ(mask_of(*phasor), 0U);
    }

    TEST_F(ModulatorSharingTest, ClearingModulatorFlagsReachesEveryDepth)
    {
        auto lower = link(phasor);
        auto upper = link(lower);
        auto tone = std::make_shared<Sine>(0.0F, 0.0);
        tone->set_amplitude_modulator(upper);

        for (const auto& node : { std::static_pointer_cast<Nodes::Node>(upper),
                 std::static_pointer_cast<Nodes::Node>(lower),
                 std::static_pointer_cast<Nodes::Node>(phasor) }) {
            Nodes::atomic_add_flag(node->m_state, Nodes::NodeState::PROCESSED);
            ASSERT_TRUE(processed(*node));
        }

        tone->clear_modulator_processed_state();

        EXPECT_FALSE(processed(*upper));
        EXPECT_FALSE(processed(*lower));
        EXPECT_FALSE(processed(*phasor));
    }

    TEST_F(ModulatorSharingTest, ExtractionBetweenRootCyclesDoesNotDisturbANestedChain)
    {
        const auto build = [] {
            auto leaf = std::make_shared<Phasor>(500.0F, 1.0);
            auto lower = std::make_shared<Polynomial>([](double x) { return std::exp(-x); });
            lower->set_input_node(leaf);
            auto upper = std::make_shared<Polynomial>([](double x) { return 0.5 * x; });
            upper->set_input_node(lower);
            auto tone = std::make_shared<Sine>(440.0F, 0.5);
            tone->set_amplitude_modulator(upper);
            return tone;
        };

        auto routed = build();
        auto reference = build();

        routed->register_channel_usage(0);
        root.register_node(routed);

        for (int cycle = 0; cycle < 16; ++cycle) {
            EXPECT_DOUBLE_EQ(root.process_sample(), reference->process_sample(0.0)) << "cycle " << cycle;
            if (cycle == 2 || cycle == 6 || cycle == 9) {
                Buffers::extract_single_sample(routed);
                Buffers::extract_multiple_samples(routed, 5);
            }
        }
    }

    TEST_F(ModulatorSharingTest, ModulatorAttachedToAnUnregisteredConsumerStaysUnclaimed)
    {
        auto envelope = decay(25.0);

        auto tone = std::make_shared<Sine>(0.0F, 0.0);
        tone->set_amplitude_modulator(envelope);

        EXPECT_EQ(mask_of(*envelope), 0U);
        EXPECT_EQ(mask_of(*phasor), 0U);
    }

}
}
