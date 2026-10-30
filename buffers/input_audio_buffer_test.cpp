#include "gtest/gtest.h"

#include "MayaFlux/Buffers/AudioBuffer.hpp"
#include "MayaFlux/Buffers/BufferManager.hpp"
#include "MayaFlux/Buffers/BufferSpec.hpp"
#include "MayaFlux/Buffers/Input/InputAudioBuffer.hpp"
#include "MayaFlux/Buffers/Managers/BufferInputControl.hpp"

namespace MayaFlux::Test {
namespace {

    using Buffers::AudioBuffer;
    using Buffers::BufferInputControl;
    using Buffers::BufferManager;
    using Buffers::InputAccessProcessor;
    using Buffers::InputAudioBuffer;

    static_assert(!std::is_copy_constructible_v<InputAudioBuffer>);
    static_assert(!std::is_move_constructible_v<InputAudioBuffer>);

    class PreferredSizeGuard {
    public:
        PreferredSizeGuard() = default;

        explicit PreferredSizeGuard(uint32_t block)
        {
            Buffers::s_preferred_buffer_size = block;
        }

        PreferredSizeGuard(const PreferredSizeGuard&) = delete;
        PreferredSizeGuard& operator=(const PreferredSizeGuard&) = delete;
        PreferredSizeGuard(PreferredSizeGuard&&) = delete;
        PreferredSizeGuard& operator=(PreferredSizeGuard&&) = delete;

        ~PreferredSizeGuard() { Buffers::s_preferred_buffer_size = m_saved; }

    private:
        uint32_t m_saved { Buffers::s_preferred_buffer_size };
    };

    std::shared_ptr<InputAudioBuffer> make_input(uint32_t channel = 0, uint32_t samples = 4)
    {
        auto input = std::make_shared<InputAudioBuffer>(channel, samples);
        input->resize(samples);
        input->set_default_processor(std::make_shared<InputAccessProcessor>());
        return input;
    }

    std::shared_ptr<AudioBuffer> make_listener(uint32_t samples = 4)
    {
        auto listener = std::make_shared<AudioBuffer>(0, samples);
        listener->resize(samples);
        return listener;
    }

    void fill(const std::shared_ptr<AudioBuffer>& buffer, std::vector<double> samples)
    {
        buffer->get_data() = std::move(samples);
    }

    std::vector<double> ramp()
    {
        return { 0.1, 0.2, 0.3, 0.4 };
    }

    TEST(InputAudioBufferTest, ReportsItsChannelAndStartsSilent)
    {
        const InputAudioBuffer input(3, 8);

        EXPECT_EQ(input.get_channel_id(), 3U);
        EXPECT_EQ(input.get_data().size(), input.get_num_samples());
        EXPECT_EQ(input.get_data(), std::vector<double>(input.get_num_samples(), 0.0));
    }

    TEST(InputAudioBufferTest, ResizingKeepsTheSampleCountAndDataTogether)
    {
        InputAudioBuffer input(0, 4);

        input.resize(8);

        EXPECT_EQ(input.get_num_samples(), 8U);
        EXPECT_EQ(input.get_data().size(), 8U);
    }

    TEST(InputAudioBufferTest, WritingToAListenerCopiesTheSamples)
    {
        const auto input = make_input();
        fill(input, ramp());
        const auto listener = make_listener();

        input->write_to(listener);

        EXPECT_EQ(listener->get_data(), ramp());
    }

    TEST(InputAudioBufferTest, WritingReplacesWhatTheListenerHeld)
    {
        const auto input = make_input();
        fill(input, ramp());
        const auto listener = make_listener();
        fill(listener, { 9, 9, 9, 9 });

        input->write_to(listener);

        EXPECT_EQ(listener->get_data(), ramp());
    }

    TEST(InputAudioBufferTest, AShorterListenerGrowsToTheInputAndKeepsItsSampleCount)
    {
        const auto input = make_input();
        fill(input, ramp());
        const auto listener = make_listener(2);

        input->write_to(listener);

        EXPECT_EQ(listener->get_data(), ramp());
        EXPECT_EQ(listener->get_num_samples(), 4U);
    }

    TEST(InputAudioBufferTest, ALongerListenerShrinksToTheInputAndKeepsItsSampleCount)
    {
        const auto input = make_input();
        fill(input, ramp());
        const auto listener = make_listener(8);

        input->write_to(listener);

        EXPECT_EQ(listener->get_data(), ramp());
        EXPECT_EQ(listener->get_num_samples(), 4U);
    }

    TEST(InputAudioBufferTest, WritingToNothingIsIgnored)
    {
        const auto input = make_input();

        EXPECT_NO_THROW(input->write_to(nullptr));
    }

    TEST(InputAudioBufferTest, AListenerRegistersOnceAndKeepsRegistrationOrder)
    {
        const auto input = make_input();
        const auto first = make_listener();
        const auto second = make_listener();

        input->register_listener(first);
        input->register_listener(second);
        input->register_listener(first);

        const auto listeners = input->get_listeners();
        ASSERT_EQ(listeners.size(), 2U);
        EXPECT_EQ(listeners[0], first);
        EXPECT_EQ(listeners[1], second);
    }

    TEST(InputAudioBufferTest, RegisteringNothingIsIgnored)
    {
        const auto input = make_input();

        EXPECT_NO_THROW(input->register_listener(nullptr));

        EXPECT_TRUE(input->get_listeners().empty());
    }

    TEST(InputAudioBufferTest, UnregisteringRemovesOnlyThatListener)
    {
        const auto input = make_input();
        const auto first = make_listener();
        const auto second = make_listener();
        input->register_listener(first);
        input->register_listener(second);

        input->unregister_listener(first);

        const auto listeners = input->get_listeners();
        ASSERT_EQ(listeners.size(), 1U);
        EXPECT_EQ(listeners[0], second);
    }

    TEST(InputAudioBufferTest, UnregisteringAnUnknownListenerChangesNothing)
    {
        const auto input = make_input();
        input->register_listener(make_listener());

        EXPECT_NO_THROW(input->unregister_listener(make_listener()));
        EXPECT_NO_THROW(input->unregister_listener(nullptr));

        EXPECT_EQ(input->get_listeners().size(), 1U);
    }

    TEST(InputAudioBufferTest, ClearingDropsEveryListener)
    {
        const auto input = make_input();
        input->register_listener(make_listener());
        input->register_listener(make_listener());

        input->clear_listeners();

        EXPECT_TRUE(input->get_listeners().empty());
    }

    TEST(InputAudioBufferTest, ProcessingDeliversTheInputToEveryListener)
    {
        const auto input = make_input();
        const auto first = make_listener();
        const auto second = make_listener();
        input->register_listener(first);
        input->register_listener(second);
        fill(input, ramp());

        input->process_default();

        EXPECT_EQ(first->get_data(), ramp());
        EXPECT_EQ(second->get_data(), ramp());
    }

    TEST(InputAudioBufferTest, ListenersAreCopiesNotAliases)
    {
        const auto input = make_input();
        const auto listener = make_listener();
        input->register_listener(listener);
        fill(input, ramp());
        input->process_default();

        listener->get_data()[0] = 5.0;

        EXPECT_DOUBLE_EQ(input->get_data()[0], ramp()[0]);
    }

    TEST(InputAudioBufferTest, EachBlockReplacesThePreviousOne)
    {
        const auto input = make_input();
        const auto listener = make_listener();
        input->register_listener(listener);

        fill(input, ramp());
        input->process_default();
        fill(input, { 1, 2, 3, 4 });
        input->process_default();

        EXPECT_EQ(listener->get_data(), (std::vector<double> { 1, 2, 3, 4 }));
    }

    TEST(InputAudioBufferTest, AnUnregisteredListenerReceivesNoMore)
    {
        const auto input = make_input();
        const auto listener = make_listener();
        input->register_listener(listener);
        fill(input, ramp());
        input->process_default();
        input->unregister_listener(listener);

        fill(input, { 1, 2, 3, 4 });
        input->process_default();

        EXPECT_EQ(listener->get_data(), ramp());
    }

    TEST(InputAudioBufferTest, ARegisteredListenerIsKeptAliveUntilItIsUnregistered)
    {
        const auto input = make_input();
        auto listener = make_listener();
        const std::weak_ptr<AudioBuffer> watch = listener;
        input->register_listener(listener);

        listener.reset();
        EXPECT_FALSE(watch.expired());

        input->clear_listeners();
        EXPECT_TRUE(watch.expired());
    }

    TEST(InputAudioBufferTest, WithoutADispatchProcessorTheBufferKeepsNoListeners)
    {
        const auto input = std::make_shared<InputAudioBuffer>(0, 4);
        const auto listener = make_listener();

        input->register_listener(listener);
        fill(input, ramp());
        input->process_default();

        EXPECT_TRUE(input->get_listeners().empty());
        EXPECT_EQ(listener->get_data(), std::vector<double>(4, 0.0));
    }

    TEST(InputAccessProcessorTest, OnlyAttachesToAnInputBuffer)
    {
        InputAccessProcessor processor;
        const auto plain = std::make_shared<AudioBuffer>(0, 4);

        EXPECT_THROW(processor.on_attach(plain), std::runtime_error);
        EXPECT_ANY_THROW(plain->set_default_processor(std::make_shared<InputAccessProcessor>()));
        EXPECT_NO_THROW(processor.on_attach(make_input()));
    }

    TEST(InputAccessProcessorTest, IsCompatibleWithInputBuffersOnly)
    {
        const InputAccessProcessor processor;

        EXPECT_TRUE(processor.is_compatible_with(make_input()));
        EXPECT_FALSE(processor.is_compatible_with(std::make_shared<AudioBuffer>(0, 4)));
        EXPECT_FALSE(processor.is_compatible_with(nullptr));
    }

    TEST(InputAccessProcessorTest, ListenersAreAddedOnceAndRemoved)
    {
        InputAccessProcessor processor;
        const auto first = make_listener();
        const auto second = make_listener();

        processor.add_listener(first);
        processor.add_listener(first);
        processor.add_listener(second);
        processor.add_listener(nullptr);
        EXPECT_EQ(processor.get_listener_count(), 2U);
        EXPECT_EQ(processor.get_listeners(), (std::vector<std::shared_ptr<AudioBuffer>> { first, second }));

        processor.remove_listener(first);
        processor.remove_listener(first);
        processor.remove_listener(nullptr);
        EXPECT_EQ(processor.get_listeners(), (std::vector<std::shared_ptr<AudioBuffer>> { second }));

        processor.clear_listeners();
        EXPECT_EQ(processor.get_listener_count(), 0U);
    }

    TEST(InputAccessProcessorTest, ProcessingDeliversTheBuffersSamples)
    {
        InputAccessProcessor processor;
        const auto input = make_input();
        const auto listener = make_listener();
        processor.add_listener(listener);
        fill(input, ramp());

        processor.processing_function(input);

        EXPECT_EQ(listener->get_data(), ramp());
    }

    TEST(InputAccessProcessorTest, ProcessingAnotherKindOfBufferDeliversNothing)
    {
        InputAccessProcessor processor;
        const auto plain = std::make_shared<AudioBuffer>(0, 4);
        const auto listener = make_listener();
        processor.add_listener(listener);
        fill(plain, ramp());

        EXPECT_NO_THROW(processor.processing_function(plain));
        EXPECT_NO_THROW(processor.processing_function(nullptr));

        EXPECT_EQ(listener->get_data(), std::vector<double>(4, 0.0));
    }

    TEST(BufferInputControlTest, StartsWithoutInputChannels)
    {
        const BufferInputControl control;

        EXPECT_EQ(control.get_audio_input_channel_count(), 0U);
        EXPECT_EQ(control.get_input_buffer(0), nullptr);
    }

    TEST(BufferInputControlTest, TheBufferLengthIsTheEnginesBlockNotTheRequestedSize)
    {
        const PreferredSizeGuard guard(8);
        BufferInputControl control;

        control.setup_audio_input_buffers(1, 16);

        EXPECT_EQ(control.get_input_buffer(0)->get_num_samples(), 8U);
    }

    TEST(BufferInputControlTest, SettingUpCreatesOneBufferPerChannel)
    {
        const PreferredSizeGuard guard(16);
        BufferInputControl control;

        control.setup_audio_input_buffers(3, 16);

        ASSERT_EQ(control.get_audio_input_channel_count(), 3U);
        for (uint32_t channel = 0; channel < 3; ++channel) {
            const auto buffer = control.get_input_buffer(channel);
            ASSERT_NE(buffer, nullptr) << channel;
            EXPECT_EQ(buffer->get_channel_id(), channel);
            EXPECT_EQ(buffer->get_num_samples(), 16U);
            EXPECT_NE(std::dynamic_pointer_cast<InputAccessProcessor>(buffer->get_default_processor()), nullptr) << channel;
        }
    }

    TEST(BufferInputControlTest, AChannelThatDoesNotExistHasNoBuffer)
    {
        BufferInputControl control;
        control.setup_audio_input_buffers(2, 4);

        EXPECT_EQ(control.get_input_buffer(2), nullptr);
        EXPECT_EQ(control.get_input_buffer(1000), nullptr);
    }

    TEST(BufferInputControlTest, SettingUpAgainReplacesTheBuffersAndTheirListeners)
    {
        const PreferredSizeGuard guard(8);
        BufferInputControl control;
        control.setup_audio_input_buffers(2, 4);
        const auto old_buffer = control.get_input_buffer(0);
        control.register_audio_input_listener(make_listener(), 0);

        control.setup_audio_input_buffers(1, 8);

        EXPECT_EQ(control.get_audio_input_channel_count(), 1U);
        EXPECT_NE(control.get_input_buffer(0), old_buffer);
        EXPECT_EQ(control.get_input_buffer(0)->get_num_samples(), 8U);
        EXPECT_TRUE(control.get_input_buffer(0)->get_listeners().empty());
    }

    TEST(BufferInputControlTest, InterleavedInputIsSplitIntoChannelsAndDelivered)
    {
        const PreferredSizeGuard guard(4);
        BufferInputControl control;
        control.setup_audio_input_buffers(2, 4);
        const auto left = make_listener();
        const auto right = make_listener();
        control.register_audio_input_listener(left, 0);
        control.register_audio_input_listener(right, 1);
        std::vector<double> input { 1, -1, 2, -2, 3, -3, 4, -4 };

        control.process_audio_input(input.data(), 2, 4);

        EXPECT_EQ(control.get_input_buffer(0)->get_data(), (std::vector<double> { 1, 2, 3, 4 }));
        EXPECT_EQ(control.get_input_buffer(1)->get_data(), (std::vector<double> { -1, -2, -3, -4 }));
        EXPECT_EQ(left->get_data(), (std::vector<double> { 1, 2, 3, 4 }));
        EXPECT_EQ(right->get_data(), (std::vector<double> { -1, -2, -3, -4 }));
    }

    TEST(BufferInputControlTest, InputBuildsTheBuffersWhenNoneExist)
    {
        const PreferredSizeGuard guard(3);
        BufferInputControl control;
        std::vector<double> input { 1, -1, 2, -2, 3, -3 };

        control.process_audio_input(input.data(), 2, 3);

        ASSERT_EQ(control.get_audio_input_channel_count(), 2U);
        EXPECT_EQ(control.get_input_buffer(0)->get_num_samples(), 3U);
        EXPECT_EQ(control.get_input_buffer(0)->get_data(), (std::vector<double> { 1, 2, 3 }));
        EXPECT_EQ(control.get_input_buffer(1)->get_data(), (std::vector<double> { -1, -2, -3 }));
    }

    TEST(BufferInputControlTest, MoreChannelsThanBuffersRebuildsTheBuffers)
    {
        const PreferredSizeGuard guard(2);
        BufferInputControl control;
        control.setup_audio_input_buffers(1, 2);
        std::vector<double> input { 1, 10, 100, 2, 20, 200 };

        control.process_audio_input(input.data(), 3, 2);

        ASSERT_EQ(control.get_audio_input_channel_count(), 3U);
        EXPECT_EQ(control.get_input_buffer(2)->get_data(), (std::vector<double> { 100, 200 }));
    }

    TEST(BufferInputControlTest, ABlockShorterThanTheBufferIsZeroPadded)
    {
        const PreferredSizeGuard guard(8);
        BufferInputControl control;
        control.setup_audio_input_buffers(1, 8);
        std::vector<double> input { 1, 2, 3, 4 };

        control.process_audio_input(input.data(), 1, 4);

        EXPECT_EQ(control.get_input_buffer(0)->get_data(), (std::vector<double> { 1, 2, 3, 4, 0, 0, 0, 0 }));
    }

    TEST(BufferInputControlTest, ABlockLongerThanTheBufferIsCutToTheBuffer)
    {
        const PreferredSizeGuard guard(4);
        BufferInputControl control;
        control.setup_audio_input_buffers(1, 4);
        std::vector<double> input { 1, 2, 3, 4, 5, 6, 7, 8 };

        control.process_audio_input(input.data(), 1, 8);

        EXPECT_EQ(control.get_input_buffer(0)->get_data(), (std::vector<double> { 1, 2, 3, 4 }));
        EXPECT_EQ(control.get_input_buffer(0)->get_num_samples(), 4U);
    }

    TEST(BufferInputControlTest, ABufferForAChannelTheDataLacksIsSilenced)
    {
        const PreferredSizeGuard guard(4);
        BufferInputControl control;
        control.setup_audio_input_buffers(3, 4);
        control.get_input_buffer(2)->get_data() = { 9, 9, 9, 9 };
        std::vector<double> input { 1, -1, 2, -2, 3, -3, 4, -4 };

        control.process_audio_input(input.data(), 2, 4);

        EXPECT_EQ(control.get_input_buffer(0)->get_data(), (std::vector<double> { 1, 2, 3, 4 }));
        EXPECT_EQ(control.get_input_buffer(1)->get_data(), (std::vector<double> { -1, -2, -3, -4 }));
        EXPECT_EQ(control.get_input_buffer(2)->get_data(), std::vector<double>(4, 0.0));
    }

    TEST(BufferInputControlTest, NullInputLeavesTheBuffersAlone)
    {
        const PreferredSizeGuard guard(4);
        BufferInputControl control;
        control.setup_audio_input_buffers(1, 4);
        control.get_input_buffer(0)->get_data() = { 1, 2, 3, 4 };

        EXPECT_NO_THROW(control.process_audio_input(nullptr, 1, 4));

        EXPECT_EQ(control.get_input_buffer(0)->get_data(), (std::vector<double> { 1, 2, 3, 4 }));
    }

    TEST(BufferInputControlTest, ListenersFollowEachBlock)
    {
        const PreferredSizeGuard guard(2);
        BufferInputControl control;
        control.setup_audio_input_buffers(1, 2);
        const auto listener = make_listener(2);
        control.register_audio_input_listener(listener, 0);
        std::vector<double> first { 1, 2 };
        std::vector<double> second { 3, 4 };

        control.process_audio_input(first.data(), 1, 2);
        EXPECT_EQ(listener->get_data(), (std::vector<double> { 1, 2 }));
        control.process_audio_input(second.data(), 1, 2);

        EXPECT_EQ(listener->get_data(), (std::vector<double> { 3, 4 }));
    }

    TEST(BufferInputControlTest, UnregisteringStopsDelivery)
    {
        const PreferredSizeGuard guard(2);
        BufferInputControl control;
        control.setup_audio_input_buffers(1, 2);
        const auto listener = make_listener(2);
        control.register_audio_input_listener(listener, 0);
        std::vector<double> first { 1, 2 };
        std::vector<double> second { 3, 4 };
        control.process_audio_input(first.data(), 1, 2);

        control.unregister_audio_input_listener(listener, 0);
        control.process_audio_input(second.data(), 1, 2);

        EXPECT_EQ(listener->get_data(), (std::vector<double> { 1, 2 }));
    }

    TEST(BufferInputControlTest, ListenerChangesOnAChannelThatDoesNotExistAreIgnored)
    {
        BufferInputControl control;
        control.setup_audio_input_buffers(1, 2);
        const auto listener = make_listener(2);

        EXPECT_NO_THROW(control.register_audio_input_listener(listener, 5));
        EXPECT_NO_THROW(control.unregister_audio_input_listener(listener, 5));

        EXPECT_TRUE(control.get_input_buffer(0)->get_listeners().empty());
    }

    TEST(BufferManagerInputTest, WithoutInputChannelsThereAreNoInputBuffers)
    {
        const PreferredSizeGuard guard;
        const BufferManager manager(2, 0, 48000, 4);

        EXPECT_EQ(manager.get_num_input_channels(), 0U);
        EXPECT_EQ(manager.get_input_buffer(0), nullptr);
    }

    TEST(BufferManagerInputTest, InputChannelsGetBuffersOfTheManagersBlockSize)
    {
        const PreferredSizeGuard guard;
        const BufferManager manager(2, 2, 48000, 4);

        ASSERT_EQ(manager.get_num_input_channels(), 2U);
        EXPECT_EQ(manager.get_input_buffer(1)->get_num_samples(), 4U);
        EXPECT_EQ(manager.get_input_buffer(2), nullptr);
    }

    TEST(BufferManagerInputTest, InputIsDeliveredToRegisteredListeners)
    {
        const PreferredSizeGuard guard;
        BufferManager manager(2, 2, 48000, 4);
        const auto left = make_listener();
        const auto right = make_listener();
        manager.register_input_listener(left, 0);
        manager.register_input_listener(right, 1);
        std::vector<double> input { 1, -1, 2, -2, 3, -3, 4, -4 };

        manager.process_input(input.data(), 2, 4);

        EXPECT_EQ(left->get_data(), (std::vector<double> { 1, 2, 3, 4 }));
        EXPECT_EQ(right->get_data(), (std::vector<double> { -1, -2, -3, -4 }));
    }

    TEST(BufferManagerInputTest, AnUnregisteredListenerStopsReceivingInput)
    {
        const PreferredSizeGuard guard;
        BufferManager manager(2, 1, 48000, 4);
        const auto listener = make_listener();
        manager.register_input_listener(listener, 0);
        std::vector<double> first { 1, 2, 3, 4 };
        std::vector<double> second { 5, 6, 7, 8 };
        manager.process_input(first.data(), 1, 4);

        manager.unregister_input_listener(listener, 0);
        manager.process_input(second.data(), 1, 4);

        EXPECT_EQ(listener->get_data(), first);
    }

} // namespace
} // namespace MayaFlux::Test
