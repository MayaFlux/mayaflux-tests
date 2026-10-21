#include "gtest/gtest.h"

#include "MayaFlux/Nodes/Generators/Sine.hpp"
#include "MayaFlux/Nodes/Network/NodeNetwork.hpp"

namespace MayaFlux::Test {

namespace {

    using Nodes::Generator::Sine;
    using Nodes::Network::MappingMode;
    using Nodes::Network::NodeNetwork;
    using Nodes::Network::OutputMode;
    using Nodes::Network::Topology;
    using Nodes::RoutingState;

    class ProbeNetwork : public NodeNetwork {
    public:
        explicit ProbeNetwork(size_t nodes = 0)
            : m_nodes(nodes)
        {
        }

        void process_batch(unsigned int num_samples) override
        {
            ++batches;
            last_samples = num_samples;
        }

        [[nodiscard]] size_t get_node_count() const override { return m_nodes; }

        void initialize() override { ++initialize_calls; }
        void reset() override { ++reset_calls; }

        void ensure() { ensure_initialized(); }
        void fill(std::vector<double> samples) { m_last_audio_buffer = std::move(samples); }
        void scale() { apply_output_scale(); }
        [[nodiscard]] const std::vector<double>& buffer() const { return m_last_audio_buffer; }

        [[nodiscard]] size_t mapping_count() const { return m_parameter_mappings.size(); }
        [[nodiscard]] const ParameterMapping& mapping(size_t i) const { return m_parameter_mappings[i]; }

        int batches {};
        unsigned int last_samples {};
        int initialize_calls {};
        int reset_calls {};

    private:
        size_t m_nodes;
    };

    TEST(NodeNetworkTest, StartsWithDocumentedDefaults)
    {
        ProbeNetwork net;

        EXPECT_EQ(net.get_topology(), Topology::INDEPENDENT);
        EXPECT_EQ(net.get_output_mode(), OutputMode::NONE);
        EXPECT_TRUE(net.is_enabled());
        EXPECT_EQ(net.get_sample_rate(), 48000U);
        EXPECT_EQ(net.get_block_size(), 512U);
        EXPECT_EQ(net.get_frame_rate(), 60U);
        EXPECT_DOUBLE_EQ(net.get_output_scale(), 1.0);
        EXPECT_FALSE(net.has_operator());
        EXPECT_EQ(net.get_operator(), nullptr);
        EXPECT_FALSE(net.is_processing());
        EXPECT_FALSE(net.is_processed_this_cycle());
        EXPECT_EQ(net.get_channel_mask(), 0U);
        EXPECT_TRUE(net.get_registered_channels().empty());
        EXPECT_FALSE(net.needs_channel_routing());
    }

    TEST(NodeNetworkTest, ConfigurationIsStoredAndReadBack)
    {
        ProbeNetwork net;

        net.set_topology(Topology::RING);
        net.set_output_mode(OutputMode::AUDIO_SINK);
        net.set_enabled(false);
        net.set_output_scale(0.25);
        net.set_sample_rate(44100);
        net.set_block_size(128);
        net.set_frame_rate(144);

        EXPECT_EQ(net.get_topology(), Topology::RING);
        EXPECT_EQ(net.get_output_mode(), OutputMode::AUDIO_SINK);
        EXPECT_FALSE(net.is_enabled());
        EXPECT_DOUBLE_EQ(net.get_output_scale(), 0.25);
        EXPECT_EQ(net.get_sample_rate(), 44100U);
        EXPECT_EQ(net.get_block_size(), 128U);
        EXPECT_EQ(net.get_frame_rate(), 144U);
    }

    TEST(NodeNetworkTest, EnsureInitializedRunsTheHookOnlyOnce)
    {
        ProbeNetwork net;

        net.ensure();
        net.ensure();

        EXPECT_EQ(net.initialize_calls, 1);
    }

    TEST(NodeNetworkTest, ResetDoesNotRearmInitialization)
    {
        ProbeNetwork net;
        net.ensure();

        net.reset();
        net.ensure();

        EXPECT_EQ(net.reset_calls, 1);
        EXPECT_EQ(net.initialize_calls, 1);
    }

    struct AudioExposure {
        const char* name;
        OutputMode mode;
        bool exposes_audio;
    };

    class NodeNetworkAudioTest : public ::testing::TestWithParam<AudioExposure> { };

    TEST_P(NodeNetworkAudioTest, AudioBufferIsExposedOnlyForAudioModes)
    {
        ProbeNetwork net;
        net.set_output_mode(GetParam().mode);
        net.fill({ 1.0, 2.0, 3.0 });

        const auto audio = net.get_audio_buffer();

        EXPECT_EQ(audio.has_value(), GetParam().exposes_audio);
        if (GetParam().exposes_audio) {
            EXPECT_EQ(*audio, (std::vector<double> { 1.0, 2.0, 3.0 }));
        }
    }

    TEST_P(NodeNetworkAudioTest, AnEmptyBufferIsNeverExposed)
    {
        ProbeNetwork net;
        net.set_output_mode(GetParam().mode);

        EXPECT_FALSE(net.get_audio_buffer().has_value());
    }

    INSTANTIATE_TEST_SUITE_P(
        OutputModes,
        NodeNetworkAudioTest,
        ::testing::Values(
            AudioExposure { "None", OutputMode::NONE, false },
            AudioExposure { "AudioSink", OutputMode::AUDIO_SINK, true },
            AudioExposure { "AudioCompute", OutputMode::AUDIO_COMPUTE, true },
            AudioExposure { "GraphicsBind", OutputMode::GRAPHICS_BIND, false },
            AudioExposure { "Custom", OutputMode::CUSTOM, false }),
        [](const ::testing::TestParamInfo<AudioExposure>& info) { return std::string(info.param.name); });

    TEST(NodeNetworkTest, AudioBufferIsACopy)
    {
        ProbeNetwork net;
        net.set_output_mode(OutputMode::AUDIO_SINK);
        net.fill({ 1.0, 2.0 });

        auto first = net.get_audio_buffer();
        first->push_back(9.0);

        EXPECT_EQ(net.get_audio_buffer()->size(), 2U);
    }

    TEST(NodeNetworkTest, OutputScaleMultipliesTheBufferInPlace)
    {
        ProbeNetwork net;
        net.fill({ 1.0, -2.0, 4.0 });

        net.set_output_scale(0.5);
        net.scale();

        EXPECT_EQ(net.buffer(), (std::vector<double> { 0.5, -1.0, 2.0 }));
    }

    TEST(NodeNetworkTest, UnitOutputScaleLeavesTheBufferAlone)
    {
        ProbeNetwork net;
        net.fill({ 1.0, -2.0, 4.0 });

        net.scale();

        EXPECT_EQ(net.buffer(), (std::vector<double> { 1.0, -2.0, 4.0 }));
    }

    TEST(NodeNetworkTest, ZeroOutputScaleSilencesTheBuffer)
    {
        ProbeNetwork net;
        net.fill({ 1.0, -2.0 });

        net.set_output_scale(0.0);
        net.scale();

        EXPECT_EQ(net.buffer(), (std::vector<double> { 0.0, -0.0 }));
    }

    TEST(NodeNetworkTest, MetadataReportsTheBaseState)
    {
        ProbeNetwork net(7);

        auto meta = net.get_metadata();
        EXPECT_EQ(meta.at("node_count"), "7");
        EXPECT_EQ(meta.at("enabled"), "true");
        EXPECT_FALSE(meta.at("topology").empty());
        EXPECT_FALSE(meta.at("output_mode").empty());

        net.set_enabled(false);
        net.set_topology(Topology::RING);
        net.set_output_mode(OutputMode::AUDIO_SINK);
        const auto changed = net.get_metadata();

        EXPECT_EQ(changed.at("enabled"), "false");
        EXPECT_NE(changed.at("topology"), meta.at("topology"));
        EXPECT_NE(changed.at("output_mode"), meta.at("output_mode"));
    }

    TEST(NodeNetworkTest, NodeOutputIsUnsupportedByDefault)
    {
        ProbeNetwork net(3);

        EXPECT_FALSE(net.get_node_output(0).has_value());
        EXPECT_FALSE(net.get_node_audio_buffer(0).has_value());
    }

    TEST(NodeNetworkMappingTest, BroadcastIsTheDefaultMode)
    {
        ProbeNetwork net;
        auto source = std::make_shared<Sine>();

        net.map_parameter("gain", source);

        ASSERT_EQ(net.mapping_count(), 1U);
        EXPECT_EQ(net.mapping(0).param_name, "gain");
        EXPECT_EQ(net.mapping(0).mode, MappingMode::BROADCAST);
        EXPECT_EQ(net.mapping(0).broadcast_source, source);
        EXPECT_EQ(net.mapping(0).network_source, nullptr);
    }

    TEST(NodeNetworkMappingTest, ANetworkSourceIsAlwaysOneToOne)
    {
        ProbeNetwork net;
        auto source = std::make_shared<ProbeNetwork>(3);

        net.map_parameter("color", source);

        ASSERT_EQ(net.mapping_count(), 1U);
        EXPECT_EQ(net.mapping(0).mode, MappingMode::ONE_TO_ONE);
        EXPECT_EQ(net.mapping(0).network_source, source);
        EXPECT_EQ(net.mapping(0).broadcast_source, nullptr);
    }

    TEST(NodeNetworkMappingTest, ModeCanBeChosenForANodeSource)
    {
        ProbeNetwork net;

        net.map_parameter("gain", std::make_shared<Sine>(), MappingMode::ONE_TO_ONE);

        EXPECT_EQ(net.mapping(0).mode, MappingMode::ONE_TO_ONE);
    }

    TEST(NodeNetworkMappingTest, UnmappingRemovesEveryMappingOfThatName)
    {
        ProbeNetwork net;
        net.map_parameter("gain", std::make_shared<Sine>());
        net.map_parameter("pitch", std::make_shared<Sine>());
        net.map_parameter("gain", std::make_shared<ProbeNetwork>(2));

        net.unmap_parameter("gain");

        ASSERT_EQ(net.mapping_count(), 1U);
        EXPECT_EQ(net.mapping(0).param_name, "pitch");
    }

    TEST(NodeNetworkMappingTest, UnmappingAnUnknownNameChangesNothing)
    {
        ProbeNetwork net;
        net.map_parameter("gain", std::make_shared<Sine>());

        net.unmap_parameter("missing");

        EXPECT_EQ(net.mapping_count(), 1U);
    }

    TEST(NodeNetworkChannelTest, ChannelsAreTrackedAsABitmask)
    {
        ProbeNetwork net;

        net.add_channel_usage(0);
        net.add_channel_usage(3);
        net.add_channel_usage(3);

        EXPECT_EQ(net.get_channel_mask(), 0b1001U);
        EXPECT_TRUE(net.is_registered_on_channel(3));
        EXPECT_FALSE(net.is_registered_on_channel(1));
        EXPECT_EQ(net.get_registered_channels(), (std::vector<uint32_t> { 0, 3 }));
    }

    TEST(NodeNetworkChannelTest, RemovingAChannelClearsOnlyThatBit)
    {
        ProbeNetwork net;
        net.add_channel_usage(0);
        net.add_channel_usage(3);

        net.remove_channel_usage(0);
        net.remove_channel_usage(5);

        EXPECT_EQ(net.get_registered_channels(), (std::vector<uint32_t> { 3 }));
    }

    TEST(NodeNetworkChannelTest, TheTopChannelWorksAndOutOfRangeChannelsAreIgnored)
    {
        ProbeNetwork net;

        net.add_channel_usage(31);
        net.add_channel_usage(32);
        net.add_channel_usage(1000);

        EXPECT_EQ(net.get_channel_mask(), 1U << 31U);
        EXPECT_TRUE(net.is_registered_on_channel(31));
        EXPECT_FALSE(net.is_registered_on_channel(32));
        net.remove_channel_usage(32);
        EXPECT_EQ(net.get_registered_channels(), (std::vector<uint32_t> { 31 }));
    }

    TEST(NodeNetworkChannelTest, SetChannelMaskReplacesTheRegistration)
    {
        ProbeNetwork net;
        net.add_channel_usage(0);

        net.set_channel_mask(0b110U);

        EXPECT_EQ(net.get_registered_channels(), (std::vector<uint32_t> { 1, 2 }));
    }

    TEST(NodeNetworkCycleTest, ProcessingAndProcessedFlagsAreIndependent)
    {
        ProbeNetwork net;

        net.mark_processing(true);
        EXPECT_TRUE(net.is_processing());
        EXPECT_FALSE(net.is_processed_this_cycle());

        net.mark_processed(true);
        net.mark_processing(false);
        EXPECT_FALSE(net.is_processing());
        EXPECT_TRUE(net.is_processed_this_cycle());
    }

    TEST(NodeNetworkCycleTest, AResetNeedsEveryActiveChannelBeforeTheNetworkReprocesses)
    {
        ProbeNetwork net;
        net.add_channel_usage(0);
        net.add_channel_usage(1);
        net.mark_processed(true);

        net.request_reset_from_channel(0);
        EXPECT_TRUE(net.is_processed_this_cycle());

        net.request_reset_from_channel(1);
        EXPECT_FALSE(net.is_processed_this_cycle());
    }

    TEST(NodeNetworkCycleTest, ThePendingRequestsClearAfterAReset)
    {
        ProbeNetwork net;
        net.add_channel_usage(0);
        net.add_channel_usage(1);
        net.request_reset_from_channel(0);
        net.request_reset_from_channel(1);
        net.mark_processed(true);

        net.request_reset_from_channel(1);

        EXPECT_TRUE(net.is_processed_this_cycle());
    }

    TEST(NodeNetworkCycleTest, ResetRequestsFromInactiveChannelsDoNotCount)
    {
        ProbeNetwork net;
        net.add_channel_usage(0);
        net.mark_processed(true);

        net.request_reset_from_channel(2);
        EXPECT_TRUE(net.is_processed_this_cycle());

        net.request_reset_from_channel(0);
        EXPECT_FALSE(net.is_processed_this_cycle());
    }

    TEST(NodeNetworkCycleTest, WithoutRegisteredChannelsNothingResets)
    {
        ProbeNetwork net;
        net.mark_processed(true);

        net.request_reset_from_channel(0);

        EXPECT_TRUE(net.is_processed_this_cycle());
    }

    TEST(NodeNetworkCycleTest, OutOfRangeResetChannelsAreIgnored)
    {
        ProbeNetwork net;
        net.add_channel_usage(0);
        net.mark_processed(true);

        net.request_reset_from_channel(32);
        net.request_reset_from_channel(500);
        EXPECT_TRUE(net.is_processed_this_cycle());

        net.request_reset_from_channel(0);
        EXPECT_FALSE(net.is_processed_this_cycle());
    }

    TEST(NodeNetworkRoutingTest, RoutingIsNeededWhileActiveOrCompleted)
    {
        ProbeNetwork net;
        EXPECT_EQ(net.get_routing_state().phase, RoutingState::NONE);
        EXPECT_FALSE(net.needs_channel_routing());

        net.get_routing_state().phase = RoutingState::ACTIVE;
        EXPECT_TRUE(net.needs_channel_routing());

        net.get_routing_state().phase = RoutingState::COMPLETED;
        EXPECT_TRUE(net.needs_channel_routing());

        net.get_routing_state().phase = RoutingState::NONE;
        EXPECT_FALSE(net.needs_channel_routing());
    }

    TEST(NodeNetworkTimeTest, TheTimestepDefaultsToTheFramePeriod)
    {
        ProbeNetwork net;
        EXPECT_NEAR(net.get_timestep(), 1.0F / 60.0F, 1e-7F);

        net.set_frame_rate(120);
        EXPECT_NEAR(net.get_timestep(), 1.0F / 120.0F, 1e-7F);
    }

    TEST(NodeNetworkTimeTest, AnOverrideBeatsTheFrameRateUntilCleared)
    {
        ProbeNetwork net;

        net.set_timestep(0.25F);
        net.set_frame_rate(30);
        EXPECT_FLOAT_EQ(net.get_timestep(), 0.25F);

        net.clear_timestep();
        EXPECT_NEAR(net.get_timestep(), 1.0F / 30.0F, 1e-7F);
    }

    TEST(NodeNetworkTimeTest, ZeroAndNegativeOverridesAreHonoured)
    {
        ProbeNetwork net;

        net.set_timestep(0.0F);
        EXPECT_FLOAT_EQ(net.get_timestep(), 0.0F);

        net.set_timestep(-0.5F);
        EXPECT_FLOAT_EQ(net.get_timestep(), -0.5F);
    }

    TEST(NodeNetworkTimeTest, AZeroFrameRateGivesAZeroTimestep)
    {
        ProbeNetwork net;

        net.set_frame_rate(0);

        EXPECT_FLOAT_EQ(net.get_timestep(), 0.0F);
    }

} // namespace

} // namespace MayaFlux::Test
