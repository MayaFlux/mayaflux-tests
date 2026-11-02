#include "gtest/gtest.h"

#include "MayaFlux/Nodes/Generators/Phasor.hpp"
#include "MayaFlux/Nodes/Generators/Polynomial.hpp"
#include "MayaFlux/Nodes/Generators/Sine.hpp"
#include "MayaFlux/Nodes/RootNode.hpp"

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
