#include "gtest/gtest.h"

#include "MayaFlux/Nodes/Conduit/Constant.hpp"
#include "MayaFlux/Nodes/Network/NodeNetwork.hpp"
#include "MayaFlux/Nodes/NodeGraphManager.hpp"

namespace MayaFlux::Test {

namespace {

    using Nodes::Constant;
    using Nodes::Node;
    using Nodes::NodeGraphManager;
    using Nodes::ProcessingToken;
    using Nodes::RootNode;
    using Nodes::RoutingState;
    using Nodes::Network::NodeNetwork;
    using Nodes::Network::OutputMode;

    class RecordingNetwork : public NodeNetwork {
    public:
        explicit RecordingNetwork(OutputMode mode = OutputMode::GRAPHICS_BIND, double level = 0.0)
            : m_level(level)
        {
            set_output_mode(mode);
        }

        void process_batch(unsigned int num_samples) override
        {
            ++batches;
            last_samples = num_samples;
            if (get_output_mode() == OutputMode::AUDIO_SINK || get_output_mode() == OutputMode::AUDIO_COMPUTE) {
                m_last_audio_buffer.assign(num_samples, m_level);
            }
            if (on_batch) {
                on_batch();
            }
        }

        [[nodiscard]] size_t get_node_count() const override { return 1; }

        int batches {};
        unsigned int last_samples {};
        std::function<void()> on_batch;

    private:
        double m_level;
    };

    std::shared_ptr<Constant> constant(double value, int* ticks = nullptr)
    {
        auto node = std::make_shared<Constant>(value);
        if (ticks != nullptr) {
            node->on_tick([ticks](Nodes::NodeContext&) { ++*ticks; });
        }
        return node;
    }

    void commit(NodeGraphManager& manager, ProcessingToken token, unsigned int channel = 0)
    {
        manager.get_root_node(token, channel).process_sample();
    }

    std::set<unsigned int> channels_of(const NodeGraphManager& manager, ProcessingToken token)
    {
        const auto all = manager.get_all_channels(token);
        return { all.begin(), all.end() };
    }

    bool listed(const NodeGraphManager& manager, ProcessingToken token, unsigned int channel, const std::shared_ptr<Node>& node)
    {
        const auto& nodes = manager.get_nodes(token, channel);
        return std::ranges::find(nodes, node) != nodes.end();
    }

    struct TokenCase {
        const char* name;
        ProcessingToken token;
        bool processes_roots;
        bool gpu_compatible;
    };

    const TokenCase k_tokens[] = {
        { "AudioRate", ProcessingToken::AUDIO_RATE, true, false },
        { "VisualRate", ProcessingToken::VISUAL_RATE, true, true },
        { "EventRate", ProcessingToken::EVENT_RATE, false, false },
        { "CustomRate", ProcessingToken::CUSTOM_RATE, false, false },
    };

    std::string token_name(const ::testing::TestParamInfo<TokenCase>& info)
    {
        return info.param.name;
    }

    class NodeGraphManagerTokenTest : public ::testing::TestWithParam<TokenCase> {
    protected:
        NodeGraphManager manager;
    };

    TEST_P(NodeGraphManagerTokenTest, AddingToARootCreatesItAndRecordsTheChannel)
    {
        auto node = constant(1.0);

        manager.add_to_root(node, GetParam().token, 2);

        EXPECT_TRUE(channels_of(manager, GetParam().token).contains(2));
        EXPECT_EQ(node->get_channel_mask().load(), 1U << 2U);
        EXPECT_TRUE(manager.is_node_registered(node));
    }

    TEST_P(NodeGraphManagerTokenTest, AddingTakesTheManagersRates)
    {
        NodeGraphManager custom(44100, 256, 90);
        auto node = constant(1.0);

        custom.add_to_root(node, GetParam().token, 0);

        EXPECT_EQ(node->get_sample_rate(), 44100U);
        EXPECT_EQ(node->get_frame_rate(), 90U);
    }

    TEST_P(NodeGraphManagerTokenTest, OnlyTheVisualTokenMakesNodesGpuCompatible)
    {
        auto node = constant(1.0);

        manager.add_to_root(node, GetParam().token, 0);

        EXPECT_EQ(node->is_gpu_compatible(), GetParam().gpu_compatible);
    }

    TEST_P(NodeGraphManagerTokenTest, NodesAreListedOnceTheRootHasProcessed)
    {
        auto node = constant(1.0);
        manager.add_to_root(node, GetParam().token, 0);
        EXPECT_FALSE(listed(manager, GetParam().token, 0, node));
        EXPECT_EQ(manager.get_node_count(GetParam().token), 0U);

        commit(manager, GetParam().token);

        EXPECT_TRUE(listed(manager, GetParam().token, 0, node));
        EXPECT_EQ(manager.get_node_count(GetParam().token), 1U);
    }

    TEST_P(NodeGraphManagerTokenTest, RemovingClearsTheChannelAndTheListing)
    {
        auto node = constant(1.0);
        manager.add_to_root(node, GetParam().token, 0);
        commit(manager, GetParam().token);

        manager.remove_from_root(node, GetParam().token, 0);
        commit(manager, GetParam().token);

        EXPECT_EQ(node->get_channel_mask().load(), 0U);
        EXPECT_FALSE(listed(manager, GetParam().token, 0, node));
        EXPECT_FALSE(manager.is_node_registered(node));
    }

    TEST_P(NodeGraphManagerTokenTest, ProcessTokenRunsRootsOnlyForAudioAndVisual)
    {
        int ticks = 0;
        manager.add_to_root(constant(1.0, &ticks), GetParam().token, 0);

        manager.process_token(GetParam().token, 3);

        EXPECT_EQ(ticks, GetParam().processes_roots ? 3 : 0);
    }

    TEST_P(NodeGraphManagerTokenTest, ACustomTokenProcessorReplacesTheDefaultProcessing)
    {
        int ticks = 0;
        manager.add_to_root(constant(1.0, &ticks), GetParam().token, 0);
        manager.add_to_root(constant(1.0), GetParam().token, 1);
        auto network = std::make_shared<RecordingNetwork>();
        manager.add_network(network, GetParam().token);
        size_t seen = 0;
        int calls = 0;
        manager.register_token_processor(GetParam().token, [&](std::span<RootNode*> roots) {
            seen = roots.size();
            ++calls;
        });

        manager.process_token(GetParam().token, 2);

        EXPECT_EQ(calls, 1);
        EXPECT_EQ(seen, manager.get_channel_count(GetParam().token));
        EXPECT_EQ(ticks, 0);
        EXPECT_EQ(network->batches, 0);
    }

    TEST_P(NodeGraphManagerTokenTest, ALaterTokenProcessorReplacesAnEarlierOne)
    {
        int first = 0;
        int second = 0;
        manager.register_token_processor(GetParam().token, [&](std::span<RootNode*>) { ++first; });
        manager.register_token_processor(GetParam().token, [&](std::span<RootNode*>) { ++second; });

        manager.process_token(GetParam().token, 1);

        EXPECT_EQ(first, 0);
        EXPECT_EQ(second, 1);
    }

    TEST_P(NodeGraphManagerTokenTest, ATerminatedManagerNoLongerProcessesTheToken)
    {
        auto network = std::make_shared<RecordingNetwork>();
        manager.add_network(network, GetParam().token);
        manager.terminate_active_processing();

        manager.process_token(GetParam().token, 1);

        EXPECT_EQ(network->batches, 0);
    }

    TEST_P(NodeGraphManagerTokenTest, MissingChannelsAreEmptyAndAreNotCreatedByLookup)
    {
        const auto before = channels_of(manager, GetParam().token);

        EXPECT_TRUE(manager.get_nodes(GetParam().token, 7).empty());

        EXPECT_EQ(channels_of(manager, GetParam().token), before);
    }

    TEST_P(NodeGraphManagerTokenTest, GettingARootNodeCreatesItOnDemand)
    {
        auto& root = manager.get_root_node(GetParam().token, 3);

        EXPECT_EQ(root.get_token(), GetParam().token);
        EXPECT_EQ(root.get_channel(), 3U);
        EXPECT_TRUE(channels_of(manager, GetParam().token).contains(3));
        EXPECT_EQ(&manager.get_root_node(GetParam().token, 3), &root);
    }

    TEST_P(NodeGraphManagerTokenTest, EveryRootOfATokenIsReachableThroughTheAccessors)
    {
        manager.get_root_node(GetParam().token, 1);
        manager.get_root_node(GetParam().token, 4);

        const auto roots = manager.get_all_root_nodes(GetParam().token);
        std::set<unsigned int> from_roots;
        for (auto* root : roots) {
            from_roots.insert(root->get_channel());
            EXPECT_EQ(root, &manager.get_root_node(GetParam().token, root->get_channel()));
        }

        EXPECT_EQ(from_roots, channels_of(manager, GetParam().token));
        EXPECT_EQ(manager.get_channel_count(GetParam().token), from_roots.size());
        const auto tokens = manager.get_active_tokens();
        EXPECT_NE(std::ranges::find(tokens, GetParam().token), tokens.end());
    }

    INSTANTIATE_TEST_SUITE_P(
        Tokens,
        NodeGraphManagerTokenTest,
        ::testing::ValuesIn(k_tokens),
        token_name);

    class NodeGraphManagerTest : public ::testing::Test {
    protected:
        NodeGraphManager manager;
    };

    TEST_F(NodeGraphManagerTest, AFreshManagerHoldsOnlyTheAudioRootOnChannelZero)
    {
        EXPECT_EQ(manager.get_active_tokens(), std::vector<ProcessingToken> { ProcessingToken::AUDIO_RATE });
        EXPECT_EQ(channels_of(manager, ProcessingToken::AUDIO_RATE), std::set<unsigned int> { 0 });
        EXPECT_EQ(manager.get_node_count(ProcessingToken::AUDIO_RATE), 0U);
        EXPECT_EQ(manager.get_network_count(ProcessingToken::AUDIO_RATE), 0U);
        EXPECT_TRUE(manager.get_all_channels(ProcessingToken::VISUAL_RATE).empty());
    }

    TEST_F(NodeGraphManagerTest, NodeConfigRoundTrips)
    {
        Nodes::NodeConfig config;
        config.max_channels = 8;
        config.callback_cache_size = 5;

        manager.set_node_config(config);

        EXPECT_EQ(std::as_const(manager).get_node_config().max_channels, 8U);
        EXPECT_EQ(manager.get_node_config().callback_cache_size, 5U);
    }

    TEST_F(NodeGraphManagerTest, ANodeOnTwoChannelsIsOneRegistrationWithBothBits)
    {
        auto node = constant(1.0);

        manager.add_to_root(node, ProcessingToken::AUDIO_RATE, 0);
        manager.add_to_root(node, ProcessingToken::AUDIO_RATE, 1);

        EXPECT_EQ(node->get_channel_mask().load(), 0b11U);
        EXPECT_TRUE(manager.is_node_registered(node));
        commit(manager, ProcessingToken::AUDIO_RATE, 0);
        commit(manager, ProcessingToken::AUDIO_RATE, 1);
        EXPECT_TRUE(listed(manager, ProcessingToken::AUDIO_RATE, 0, node));
        EXPECT_TRUE(listed(manager, ProcessingToken::AUDIO_RATE, 1, node));
    }

    TEST_F(NodeGraphManagerTest, ANodeStaysRegisteredWhileAnyChannelStillUsesIt)
    {
        auto node = constant(1.0);
        manager.add_to_root(node, ProcessingToken::AUDIO_RATE, 0);
        manager.add_to_root(node, ProcessingToken::AUDIO_RATE, 1);

        manager.remove_from_root(node, ProcessingToken::AUDIO_RATE, 0);

        EXPECT_EQ(node->get_channel_mask().load(), 0b10U);
        EXPECT_TRUE(manager.is_node_registered(node));
    }

    TEST_F(NodeGraphManagerTest, NodeCountSumsEveryChannelOfTheToken)
    {
        manager.add_to_root(constant(1.0), ProcessingToken::AUDIO_RATE, 0);
        manager.add_to_root(constant(1.0), ProcessingToken::AUDIO_RATE, 1);
        manager.add_to_root(constant(1.0), ProcessingToken::AUDIO_RATE, 1);
        manager.add_to_root(constant(1.0), ProcessingToken::VISUAL_RATE, 0);
        commit(manager, ProcessingToken::AUDIO_RATE, 0);
        commit(manager, ProcessingToken::AUDIO_RATE, 1);
        commit(manager, ProcessingToken::VISUAL_RATE, 0);

        EXPECT_EQ(manager.get_node_count(ProcessingToken::AUDIO_RATE), 3U);
        EXPECT_EQ(manager.get_node_count(ProcessingToken::VISUAL_RATE), 1U);
        EXPECT_EQ(manager.get_node_count(ProcessingToken::EVENT_RATE), 0U);
    }

    TEST_F(NodeGraphManagerTest, AnUnregisteredNodeIsNotRegistered)
    {
        EXPECT_FALSE(manager.is_node_registered(constant(1.0)));
        EXPECT_FALSE(manager.is_node_registered(nullptr));
    }

    TEST_F(NodeGraphManagerTest, RootMapsListEveryChannelOfAToken)
    {
        manager.get_root_node(ProcessingToken::AUDIO_RATE, 2);

        const auto roots = manager.get_all_channel_root_nodes(ProcessingToken::AUDIO_RATE);

        EXPECT_EQ(roots.size(), 2U);
        EXPECT_TRUE(roots.contains(0));
        EXPECT_TRUE(roots.contains(2));
        EXPECT_TRUE(manager.get_all_channel_root_nodes(ProcessingToken::EVENT_RATE).empty());
    }

    TEST_F(NodeGraphManagerTest, AnEarlierRootMapIsNotChangedByALaterRequestForAnotherToken)
    {
        manager.get_root_node(ProcessingToken::VISUAL_RATE, 5);
        const auto& audio = manager.get_all_channel_root_nodes(ProcessingToken::AUDIO_RATE);
        ASSERT_EQ(audio.size(), 1U);

        (void)manager.get_all_channel_root_nodes(ProcessingToken::VISUAL_RATE);

        EXPECT_TRUE(audio.contains(0));
        EXPECT_EQ(audio.size(), 1U);
    }

    TEST(NodeGraphManagerDeathTest, AddingANullNodeIsIgnored)
    {
        const auto previous = GTEST_FLAG_GET(death_test_style);
        GTEST_FLAG_SET(death_test_style, "threadsafe");

        EXPECT_EXIT(
            {
                NodeGraphManager manager;
                manager.add_to_root(nullptr, ProcessingToken::AUDIO_RATE, 0);
                std::_Exit(0);
            },
            ::testing::ExitedWithCode(0), "");

        GTEST_FLAG_SET(death_test_style, previous);
    }

    TEST(NodeGraphManagerDeathTest, RemovingANullNodeIsIgnored)
    {
        const auto previous = GTEST_FLAG_GET(death_test_style);
        GTEST_FLAG_SET(death_test_style, "threadsafe");

        EXPECT_EXIT(
            {
                NodeGraphManager manager;
                manager.remove_from_root(nullptr, ProcessingToken::AUDIO_RATE, 0);
                std::_Exit(0);
            },
            ::testing::ExitedWithCode(0), "");

        GTEST_FLAG_SET(death_test_style, previous);
    }

    TEST_F(NodeGraphManagerTest, ProcessAllTokensVisitsEveryActiveToken)
    {
        int audio = 0;
        int visual = 0;
        manager.add_to_root(constant(1.0, &audio), ProcessingToken::AUDIO_RATE, 0);
        manager.add_to_root(constant(1.0, &visual), ProcessingToken::VISUAL_RATE, 0);

        manager.process_all_tokens(2);

        EXPECT_EQ(audio, 2);
        EXPECT_EQ(visual, 2);
    }

    struct NormalisationCase {
        const char* name;
        int nodes;
        double value;
        double expected;
    };

    class NodeGraphManagerNormalisationTest : public ::testing::TestWithParam<NormalisationCase> {
    protected:
        NodeGraphManager manager;

        void SetUp() override
        {
            for (int i = 0; i < GetParam().nodes; ++i) {
                manager.add_to_root(constant(GetParam().value), ProcessingToken::AUDIO_RATE, 0);
            }
        }
    };

    TEST_P(NodeGraphManagerNormalisationTest, ProcessChannelScalesByTheRootOfTheNodeCountAndSoftLimits)
    {
        const auto samples = manager.process_channel(ProcessingToken::AUDIO_RATE, 0, 4);

        ASSERT_EQ(samples.size(), 4U);
        for (const double sample : samples) {
            EXPECT_NEAR(sample, GetParam().expected, 1e-9);
        }
    }

    TEST_P(NodeGraphManagerNormalisationTest, ProcessSampleAppliesTheSameNormalisation)
    {
        EXPECT_NEAR(manager.process_sample(ProcessingToken::AUDIO_RATE, 0), GetParam().expected, 1e-9);
    }

    INSTANTIATE_TEST_SUITE_P(
        Levels,
        NodeGraphManagerNormalisationTest,
        ::testing::Values(
            NormalisationCase { "NoNodesGiveSilence", 0, 0.0, 0.0 },
            NormalisationCase { "OneNodeBelowTheThresholdIsUntouched", 1, 0.5, 0.5 },
            NormalisationCase { "ExactlyTheThresholdIsUntouched", 1, 0.95, 0.95 },
            NormalisationCase { "TwoNodesScaleByRootTwo", 2, 0.5, 0.70710678118654752 },
            NormalisationCase { "FourNodesAreSoftLimited", 4, 0.5, 0.99621171572600098 },
            NormalisationCase { "ALargeSignalStaysBelowOnePointOhFive", 1, 3.0, 1.05 },
            NormalisationCase { "TheLimiterKeepsTheSign", 1, -3.0, -1.05 }),
        [](const ::testing::TestParamInfo<NormalisationCase>& info) { return std::string(info.param.name); });

    TEST_F(NodeGraphManagerTest, ACustomChannelProcessorOutputIsReturnedUntouched)
    {
        manager.add_to_root(constant(3.0), ProcessingToken::AUDIO_RATE, 0);
        RootNode* seen = nullptr;
        unsigned int asked = 0;
        manager.register_token_channel_processor(ProcessingToken::AUDIO_RATE,
            [&](RootNode* root, uint32_t samples) {
                seen = root;
                asked = samples;
                return std::vector<double> { 7.0, 8.0 };
            });

        const auto out = manager.process_channel(ProcessingToken::AUDIO_RATE, 0, 5);

        EXPECT_EQ(out, (std::vector<double> { 7.0, 8.0 }));
        EXPECT_EQ(seen, &manager.get_root_node(ProcessingToken::AUDIO_RATE, 0));
        EXPECT_EQ(asked, 5U);
    }

    TEST_F(NodeGraphManagerTest, ACustomSampleProcessorReplacesProcessSample)
    {
        manager.add_to_root(constant(3.0), ProcessingToken::AUDIO_RATE, 0);
        uint32_t channel_seen = 99;
        manager.register_token_sample_processor(ProcessingToken::AUDIO_RATE,
            [&](RootNode*, uint32_t channel) {
                channel_seen = channel;
                return 3.5;
            });

        EXPECT_DOUBLE_EQ(manager.process_sample(ProcessingToken::AUDIO_RATE, 0), 3.5);
        EXPECT_EQ(channel_seen, 0U);
    }

    TEST_F(NodeGraphManagerTest, ProcessSampleIsSilentOnceTerminated)
    {
        manager.add_to_root(constant(0.5), ProcessingToken::AUDIO_RATE, 0);
        manager.terminate_active_processing();

        EXPECT_DOUBLE_EQ(manager.process_sample(ProcessingToken::AUDIO_RATE, 0), 0.0);
    }

    TEST_F(NodeGraphManagerTest, ChannelDataCoversEveryChannelOfTheToken)
    {
        manager.add_to_root(constant(0.25), ProcessingToken::AUDIO_RATE, 0);
        manager.add_to_root(constant(0.5), ProcessingToken::AUDIO_RATE, 1);

        const auto data = manager.process_token_with_channel_data(ProcessingToken::AUDIO_RATE, 3);

        ASSERT_EQ(data.size(), 2U);
        EXPECT_EQ(data.at(0), std::vector<double>(3, 0.25));
        EXPECT_EQ(data.at(1), std::vector<double>(3, 0.5));
    }

    struct NetworkRoutingCase {
        const char* name;
        OutputMode mode;
        bool audio_list;
    };

    class NodeGraphManagerNetworkTest : public ::testing::TestWithParam<NetworkRoutingCase> {
    protected:
        NodeGraphManager manager;
    };

    TEST_P(NodeGraphManagerNetworkTest, AddingRoutesByOutputMode)
    {
        auto network = std::make_shared<RecordingNetwork>(GetParam().mode);
        const auto token = GetParam().audio_list ? ProcessingToken::AUDIO_RATE : ProcessingToken::VISUAL_RATE;
        network->set_enabled(false);

        manager.add_network(network, token);

        EXPECT_TRUE(network->is_enabled());
        EXPECT_TRUE(manager.is_network_registered(network));
        EXPECT_TRUE(manager.is_network_registered(network, token));
        EXPECT_EQ(manager.get_network_count(token), 1U);
        EXPECT_EQ(manager.get_networks(token), std::vector<std::shared_ptr<NodeNetwork>> { network });
    }

    TEST_P(NodeGraphManagerNetworkTest, AddingGivesTheNetworkTheManagersRates)
    {
        NodeGraphManager custom(44100, 256, 90);
        auto network = std::make_shared<RecordingNetwork>(GetParam().mode);

        custom.add_network(network, ProcessingToken::VISUAL_RATE);

        EXPECT_EQ(network->get_sample_rate(), 44100U);
        EXPECT_EQ(network->get_block_size(), 256U);
        EXPECT_EQ(network->get_frame_rate(), 90U);
    }

    TEST_P(NodeGraphManagerNetworkTest, RemovingUndoesTheRegistration)
    {
        auto network = std::make_shared<RecordingNetwork>(GetParam().mode);
        const auto token = GetParam().audio_list ? ProcessingToken::AUDIO_RATE : ProcessingToken::VISUAL_RATE;
        manager.add_network(network, token);

        manager.remove_network(network, token);

        EXPECT_FALSE(manager.is_network_registered(network));
        EXPECT_FALSE(manager.is_network_registered(network, token));
        EXPECT_EQ(manager.get_network_count(token), 0U);
    }

    TEST_P(NodeGraphManagerNetworkTest, ClearingEmptiesTheTokensNetworks)
    {
        const auto token = GetParam().audio_list ? ProcessingToken::AUDIO_RATE : ProcessingToken::VISUAL_RATE;
        manager.add_network(std::make_shared<RecordingNetwork>(GetParam().mode), token);
        manager.add_network(std::make_shared<RecordingNetwork>(GetParam().mode), token);

        manager.clear_networks(token);

        EXPECT_EQ(manager.get_network_count(token), 0U);
        EXPECT_TRUE(manager.get_networks(token).empty());
    }

    INSTANTIATE_TEST_SUITE_P(
        OutputModes,
        NodeGraphManagerNetworkTest,
        ::testing::Values(
            NetworkRoutingCase { "None", OutputMode::NONE, false },
            NetworkRoutingCase { "GraphicsBind", OutputMode::GRAPHICS_BIND, false },
            NetworkRoutingCase { "Custom", OutputMode::CUSTOM, false },
            NetworkRoutingCase { "AudioSink", OutputMode::AUDIO_SINK, true },
            NetworkRoutingCase { "AudioCompute", OutputMode::AUDIO_COMPUTE, true }),
        [](const ::testing::TestParamInfo<NetworkRoutingCase>& info) { return std::string(info.param.name); });

    TEST_F(NodeGraphManagerTest, NullNetworksAreIgnored)
    {
        manager.add_network(nullptr, ProcessingToken::VISUAL_RATE);
        manager.remove_network(nullptr, ProcessingToken::VISUAL_RATE);

        EXPECT_EQ(manager.get_network_count(ProcessingToken::VISUAL_RATE), 0U);
    }

    TEST_F(NodeGraphManagerTest, AnAudioNetworkWithoutChannelsGetsChannelZero)
    {
        auto network = std::make_shared<RecordingNetwork>(OutputMode::AUDIO_SINK);

        manager.add_network(network, ProcessingToken::AUDIO_RATE);

        EXPECT_EQ(network->get_registered_channels(), std::vector<uint32_t> { 0 });
    }

    TEST_F(NodeGraphManagerTest, AnAudioNetworkKeepsItsChannelsAndGetsRootsForThem)
    {
        auto network = std::make_shared<RecordingNetwork>(OutputMode::AUDIO_SINK);
        network->add_channel_usage(2);
        network->add_channel_usage(5);

        manager.add_network(network, ProcessingToken::AUDIO_RATE);

        EXPECT_EQ(network->get_registered_channels(), (std::vector<uint32_t> { 2, 5 }));
        EXPECT_TRUE(channels_of(manager, ProcessingToken::AUDIO_RATE).contains(2));
        EXPECT_TRUE(channels_of(manager, ProcessingToken::AUDIO_RATE).contains(5));
    }

    TEST_F(NodeGraphManagerTest, AudioNetworksAreListedPerChannelAndOtherNetworksPerToken)
    {
        auto left = std::make_shared<RecordingNetwork>(OutputMode::AUDIO_SINK);
        auto right = std::make_shared<RecordingNetwork>(OutputMode::AUDIO_SINK);
        right->add_channel_usage(1);
        auto visual = std::make_shared<RecordingNetwork>(OutputMode::GRAPHICS_BIND);
        manager.add_network(left, ProcessingToken::AUDIO_RATE);
        manager.add_network(right, ProcessingToken::AUDIO_RATE);
        manager.add_network(visual, ProcessingToken::VISUAL_RATE);

        EXPECT_EQ(manager.get_networks(ProcessingToken::AUDIO_RATE, 0), std::vector<std::shared_ptr<NodeNetwork>> { left });
        EXPECT_EQ(manager.get_networks(ProcessingToken::AUDIO_RATE, 1), std::vector<std::shared_ptr<NodeNetwork>> { right });
        EXPECT_EQ(manager.get_networks(ProcessingToken::VISUAL_RATE, 5), std::vector<std::shared_ptr<NodeNetwork>> { visual });
        EXPECT_TRUE(manager.get_networks(ProcessingToken::EVENT_RATE).empty());
    }

    TEST_F(NodeGraphManagerTest, AddingTheSameNetworkTwiceRegistersItOnce)
    {
        auto network = std::make_shared<RecordingNetwork>();

        manager.add_network(network, ProcessingToken::VISUAL_RATE);
        manager.add_network(network, ProcessingToken::VISUAL_RATE);

        EXPECT_EQ(manager.get_network_count(ProcessingToken::VISUAL_RATE), 1U);
    }

    TEST_F(NodeGraphManagerTest, RemovingANetworkWorksAfterItsOutputModeChanged)
    {
        auto network = std::make_shared<RecordingNetwork>(OutputMode::AUDIO_SINK);
        manager.add_network(network, ProcessingToken::AUDIO_RATE);
        network->set_output_mode(OutputMode::GRAPHICS_BIND);

        manager.remove_network(network, ProcessingToken::AUDIO_RATE);

        EXPECT_EQ(manager.get_network_count(ProcessingToken::AUDIO_RATE), 0U);
    }

    TEST_F(NodeGraphManagerTest, RemovingANetworkThatWasNeverAddedChangesNothing)
    {
        auto added = std::make_shared<RecordingNetwork>();
        manager.add_network(added, ProcessingToken::VISUAL_RATE);

        manager.remove_network(std::make_shared<RecordingNetwork>(), ProcessingToken::VISUAL_RATE);

        EXPECT_EQ(manager.get_network_count(ProcessingToken::VISUAL_RATE), 1U);
    }

    TEST_F(NodeGraphManagerTest, TheGlobalRegistryIsSeparateFromAnyTokenList)
    {
        auto network = std::make_shared<RecordingNetwork>();

        manager.register_network_global(network);
        EXPECT_TRUE(manager.is_network_registered(network));
        EXPECT_FALSE(manager.is_network_registered(network, ProcessingToken::VISUAL_RATE));

        manager.unregister_network_global(network);
        EXPECT_FALSE(manager.is_network_registered(network));
    }

    TEST_F(NodeGraphManagerTest, GlobalRegistrationSetsRatesOnlyTheFirstTime)
    {
        NodeGraphManager custom(44100, 256, 90);
        auto network = std::make_shared<RecordingNetwork>();
        custom.register_network_global(network);
        network->set_frame_rate(24);

        custom.register_network_global(network);

        EXPECT_EQ(network->get_frame_rate(), 24U);
    }

    TEST_F(NodeGraphManagerTest, ProcessTokenRunsEnabledNetworksOncePerCycle)
    {
        auto network = std::make_shared<RecordingNetwork>();
        manager.add_network(network, ProcessingToken::VISUAL_RATE);

        manager.process_token(ProcessingToken::VISUAL_RATE, 3);
        EXPECT_EQ(network->batches, 1);
        EXPECT_EQ(network->last_samples, 3U);
        EXPECT_FALSE(network->is_processing());
        EXPECT_FALSE(network->is_processed_this_cycle());

        manager.process_token(ProcessingToken::VISUAL_RATE, 1);
        EXPECT_EQ(network->batches, 2);
    }

    TEST_F(NodeGraphManagerTest, ADisabledNetworkIsSkipped)
    {
        auto network = std::make_shared<RecordingNetwork>();
        manager.add_network(network, ProcessingToken::VISUAL_RATE);
        network->set_enabled(false);

        manager.process_token(ProcessingToken::VISUAL_RATE, 1);

        EXPECT_EQ(network->batches, 0);
    }

    TEST_F(NodeGraphManagerTest, ANetworkAlreadyProcessedThisCycleIsSkippedAndThenReleased)
    {
        auto network = std::make_shared<RecordingNetwork>();
        manager.add_network(network, ProcessingToken::VISUAL_RATE);
        network->mark_processed(true);

        manager.process_token(ProcessingToken::VISUAL_RATE, 1);
        EXPECT_EQ(network->batches, 0);

        manager.process_token(ProcessingToken::VISUAL_RATE, 1);
        EXPECT_EQ(network->batches, 1);
    }

    TEST_F(NodeGraphManagerTest, ProcessTokenLeavesAudioNetworksToTheAudioPath)
    {
        auto network = std::make_shared<RecordingNetwork>(OutputMode::AUDIO_SINK, 0.5);
        manager.add_network(network, ProcessingToken::AUDIO_RATE);

        manager.process_token(ProcessingToken::AUDIO_RATE, 4);

        EXPECT_EQ(network->batches, 0);
    }

    TEST_F(NodeGraphManagerTest, NetworksRunBeforeTheRootsOfTheSameCall)
    {
        std::vector<std::string> order;
        auto node = constant(1.0);
        node->on_tick([&](Nodes::NodeContext&) { order.emplace_back("node"); });
        manager.add_to_root(node, ProcessingToken::VISUAL_RATE, 0);
        auto network = std::make_shared<RecordingNetwork>();
        network->on_batch = [&order] { order.emplace_back("network"); };
        manager.add_network(network, ProcessingToken::VISUAL_RATE);

        manager.process_token(ProcessingToken::VISUAL_RATE, 1);

        EXPECT_EQ(order, (std::vector<std::string> { "network", "node" }));
    }

    TEST_F(NodeGraphManagerTest, AReentrantProcessTokenIsRefused)
    {
        int ticks = 0;
        manager.add_to_root(constant(1.0, &ticks), ProcessingToken::VISUAL_RATE, 0);
        auto network = std::make_shared<RecordingNetwork>();
        network->on_batch = [this] { manager.process_token(ProcessingToken::VISUAL_RATE, 1); };
        manager.add_network(network, ProcessingToken::VISUAL_RATE);

        manager.process_token(ProcessingToken::VISUAL_RATE, 1);

        EXPECT_EQ(network->batches, 1);
        EXPECT_EQ(ticks, 1);
    }

    struct AudioPathCase {
        const char* name;
        OutputMode mode;
        bool on_channel;
        bool enabled;
        size_t expected_outputs;
        int expected_batches;
    };

    class NodeGraphManagerAudioPathTest : public ::testing::TestWithParam<AudioPathCase> {
    protected:
        NodeGraphManager manager;
    };

    TEST_P(NodeGraphManagerAudioPathTest, AudioNetworksProcessAndReturnOnlyWhenTheyShould)
    {
        auto network = std::make_shared<RecordingNetwork>(GetParam().mode, 0.5);
        network->add_channel_usage(GetParam().on_channel ? 0U : 3U);
        manager.add_network(network, ProcessingToken::AUDIO_RATE);
        network->set_enabled(GetParam().enabled);

        const auto outputs = manager.process_audio_networks(ProcessingToken::AUDIO_RATE, 4, 0);

        EXPECT_EQ(outputs.size(), GetParam().expected_outputs);
        EXPECT_EQ(network->batches, GetParam().expected_batches);
        for (const auto& buffer : outputs) {
            EXPECT_EQ(buffer, std::vector<double>(4, 0.5));
        }
    }

    INSTANTIATE_TEST_SUITE_P(
        Cases,
        NodeGraphManagerAudioPathTest,
        ::testing::Values(
            AudioPathCase { "SinkOnTheChannel", OutputMode::AUDIO_SINK, true, true, 1, 1 },
            AudioPathCase { "ComputeIsProcessedButNotReturned", OutputMode::AUDIO_COMPUTE, true, true, 0, 1 },
            AudioPathCase { "SinkOnAnotherChannelIsSkipped", OutputMode::AUDIO_SINK, false, true, 0, 0 },
            AudioPathCase { "DisabledSinkIsSkipped", OutputMode::AUDIO_SINK, true, false, 0, 0 }),
        [](const ::testing::TestParamInfo<AudioPathCase>& info) { return std::string(info.param.name); });

    struct RoutedAmountCase {
        const char* name;
        double amount;
        bool returned;
        double scale;
    };

    class NodeGraphManagerRoutedAudioTest : public ::testing::TestWithParam<RoutedAmountCase> { };

    TEST_P(NodeGraphManagerRoutedAudioTest, ARoutingNetworkIsScaledByItsChannelAmount)
    {
        NodeGraphManager manager;
        auto network = std::make_shared<RecordingNetwork>(OutputMode::AUDIO_SINK, 1.0);
        manager.add_network(network, ProcessingToken::AUDIO_RATE);
        network->get_routing_state().phase = RoutingState::ACTIVE;
        network->get_routing_state().amount[0] = GetParam().amount;

        const auto outputs = manager.process_audio_networks(ProcessingToken::AUDIO_RATE, 3, 0);

        ASSERT_EQ(outputs.size(), GetParam().returned ? 1U : 0U);
        if (GetParam().returned) {
            EXPECT_EQ(outputs[0], std::vector<double>(3, GetParam().scale));
        }
    }

    INSTANTIATE_TEST_SUITE_P(
        Amounts,
        NodeGraphManagerRoutedAudioTest,
        ::testing::Values(
            RoutedAmountCase { "SilentChannelIsOmitted", 0.0, false, 0.0 },
            RoutedAmountCase { "PartialAmountScales", 0.25, true, 0.25 },
            RoutedAmountCase { "FullAmountPassesThrough", 1.0, true, 1.0 }),
        [](const ::testing::TestParamInfo<RoutedAmountCase>& info) { return std::string(info.param.name); });

    TEST_F(NodeGraphManagerTest, ATwoChannelNetworkProcessesOncePerCycleAcrossItsChannels)
    {
        auto network = std::make_shared<RecordingNetwork>(OutputMode::AUDIO_SINK, 0.5);
        network->add_channel_usage(0);
        network->add_channel_usage(1);
        manager.add_network(network, ProcessingToken::AUDIO_RATE);

        const auto first = manager.process_audio_networks(ProcessingToken::AUDIO_RATE, 2, 0);
        const auto second = manager.process_audio_networks(ProcessingToken::AUDIO_RATE, 2, 1);
        EXPECT_EQ(network->batches, 1);
        EXPECT_EQ(first.size(), 1U);
        EXPECT_EQ(second.size(), 1U);

        manager.process_audio_networks(ProcessingToken::AUDIO_RATE, 2, 0);
        EXPECT_EQ(network->batches, 2);
    }

    TEST_F(NodeGraphManagerTest, ProcessingChannelZeroRequestsAResetOfTheAudioNetworksOnIt)
    {
        auto network = std::make_shared<RecordingNetwork>(OutputMode::AUDIO_SINK, 0.5);
        manager.add_network(network, ProcessingToken::AUDIO_RATE);
        network->mark_processed(true);

        manager.process_channel(ProcessingToken::AUDIO_RATE, 0, 1);

        EXPECT_FALSE(network->is_processed_this_cycle());
    }

    TEST_F(NodeGraphManagerTest, ProcessingOtherChannelsDoesNotRequestAReset)
    {
        auto network = std::make_shared<RecordingNetwork>(OutputMode::AUDIO_SINK, 0.5);
        network->add_channel_usage(0);
        network->add_channel_usage(1);
        manager.add_network(network, ProcessingToken::AUDIO_RATE);
        network->mark_processed(true);

        manager.process_channel(ProcessingToken::AUDIO_RATE, 1, 1);

        EXPECT_TRUE(network->is_processed_this_cycle());
    }

    TEST_F(NodeGraphManagerTest, AReentrantAudioNetworkPassIsRefused)
    {
        auto network = std::make_shared<RecordingNetwork>(OutputMode::AUDIO_SINK, 0.5);
        std::vector<std::vector<double>> inner;
        network->on_batch = [&] { inner = manager.process_audio_networks(ProcessingToken::AUDIO_RATE, 2, 0); };
        manager.add_network(network, ProcessingToken::AUDIO_RATE);

        manager.process_audio_networks(ProcessingToken::AUDIO_RATE, 2, 0);

        EXPECT_TRUE(inner.empty());
        EXPECT_EQ(network->batches, 1);
    }

    struct FadeCase {
        const char* name;
        uint32_t fade_cycles;
        uint32_t expected_blocks;
    };

    class NodeGraphManagerFadeTest : public ::testing::TestWithParam<FadeCase> {
    protected:
        NodeGraphManager manager { 48000, 512, 60 };
    };

    TEST_P(NodeGraphManagerFadeTest, FadeCyclesRoundUpToWholeBlocksWithAMinimumOfOne)
    {
        auto node = constant(1.0);
        manager.add_to_root(node, ProcessingToken::AUDIO_RATE, 0);

        manager.route_node_to_channels(node, { 1 }, GetParam().fade_cycles, ProcessingToken::AUDIO_RATE);

        EXPECT_EQ(std::as_const(*node).get_routing_state().fade_cycles, GetParam().expected_blocks);
    }

    TEST_P(NodeGraphManagerFadeTest, NetworksUseTheSameRounding)
    {
        auto network = std::make_shared<RecordingNetwork>(OutputMode::AUDIO_SINK);
        manager.add_network(network, ProcessingToken::AUDIO_RATE);

        manager.route_network_to_channels(network, { 1 }, GetParam().fade_cycles, ProcessingToken::AUDIO_RATE);

        EXPECT_EQ(network->get_routing_state().fade_cycles, GetParam().expected_blocks);
    }

    INSTANTIATE_TEST_SUITE_P(
        Cycles,
        NodeGraphManagerFadeTest,
        ::testing::Values(
            FadeCase { "Zero", 0, 1 },
            FadeCase { "One", 1, 1 },
            FadeCase { "OneBlock", 512, 1 },
            FadeCase { "OnePastABlock", 513, 2 },
            FadeCase { "TwoBlocks", 1024, 2 },
            FadeCase { "TwoThousand", 2000, 4 }),
        [](const ::testing::TestParamInfo<FadeCase>& info) { return std::string(info.param.name); });

    TEST_F(NodeGraphManagerTest, RoutingANodeStartsAFadeAndAddsOnlyTheMissingChannels)
    {
        auto node = constant(1.0);
        manager.add_to_root(node, ProcessingToken::AUDIO_RATE, 0);

        manager.route_node_to_channels(node, { 0, 1 }, 1024, ProcessingToken::AUDIO_RATE);

        const auto& state = std::as_const(*node).get_routing_state();
        EXPECT_EQ(state.phase, RoutingState::ACTIVE);
        EXPECT_EQ(state.from_channels, 0b01U);
        EXPECT_EQ(state.to_channels, 0b11U);
        EXPECT_DOUBLE_EQ(state.amount[0], 1.0);
        EXPECT_DOUBLE_EQ(state.amount[1], 0.0);
        EXPECT_EQ(node->get_channel_mask().load(), 0b11U);
    }

    struct StepCase {
        const char* name;
        uint32_t from;
        uint32_t to;
        uint32_t fade;
        int steps;
        std::array<double, 3> amounts;
        uint8_t phase;
    };

    class NodeGraphManagerRoutingStepTest : public ::testing::TestWithParam<StepCase> {
    protected:
        NodeGraphManager manager;
    };

    TEST_P(NodeGraphManagerRoutingStepTest, EachCycleAdvancesTheFadeTowardTheTargetChannels)
    {
        auto node = constant(1.0);
        manager.add_to_root(node, ProcessingToken::AUDIO_RATE, 0);
        RoutingState state {};
        state.from_channels = GetParam().from;
        state.to_channels = GetParam().to;
        state.fade_cycles = GetParam().fade;
        state.phase = RoutingState::ACTIVE;
        node->get_routing_state() = state;

        for (int i = 0; i < GetParam().steps; ++i) {
            manager.update_routing_states_for_cycle(ProcessingToken::AUDIO_RATE);
        }

        const auto& after = std::as_const(*node).get_routing_state();
        for (size_t ch = 0; ch < 3; ++ch) {
            EXPECT_NEAR(after.amount[ch], GetParam().amounts[ch], 1e-12) << "channel " << ch;
        }
        EXPECT_EQ(static_cast<uint8_t>(after.phase), GetParam().phase);
    }

    INSTANTIATE_TEST_SUITE_P(
        Steps,
        NodeGraphManagerRoutingStepTest,
        ::testing::Values(
            StepCase { "QuarterWay", 0b001, 0b010, 4, 1, { 0.75, 0.25, 0.0 }, RoutingState::ACTIVE },
            StepCase { "HalfWay", 0b001, 0b010, 4, 2, { 0.5, 0.5, 0.0 }, RoutingState::ACTIVE },
            StepCase { "Complete", 0b001, 0b010, 4, 4, { 0.0, 1.0, 0.0 }, RoutingState::COMPLETED },
            StepCase { "StaysCompleteAfterwards", 0b001, 0b010, 4, 6, { 0.0, 1.0, 0.0 }, RoutingState::COMPLETED },
            StepCase { "KeptChannelStaysFull", 0b011, 0b001, 2, 1, { 1.0, 0.5, 0.0 }, RoutingState::ACTIVE },
            StepCase { "FadeFromNothing", 0b000, 0b100, 2, 1, { 0.0, 0.0, 0.5 }, RoutingState::ACTIVE },
            StepCase { "ZeroFadeSnapsAndClears", 0b001, 0b110, 0, 1, { 0.0, 1.0, 1.0 }, RoutingState::NONE }),
        [](const ::testing::TestParamInfo<StepCase>& info) { return std::string(info.param.name); });

    TEST_F(NodeGraphManagerTest, NodesWithoutARoutingPhaseAreNotUpdated)
    {
        auto node = constant(1.0);
        manager.add_to_root(node, ProcessingToken::AUDIO_RATE, 0);

        manager.update_routing_states_for_cycle(ProcessingToken::AUDIO_RATE);

        EXPECT_EQ(std::as_const(*node).get_routing_state().cycles_elapsed, 0U);
    }

    TEST_F(NodeGraphManagerTest, CompletedRoutingRemovesTheNodeFromTheChannelsItLeft)
    {
        auto node = constant(1.0);
        manager.add_to_root(node, ProcessingToken::AUDIO_RATE, 0);
        commit(manager, ProcessingToken::AUDIO_RATE, 0);
        manager.route_node_to_channels(node, { 1 }, 512, ProcessingToken::AUDIO_RATE);
        manager.update_routing_states_for_cycle(ProcessingToken::AUDIO_RATE);
        ASSERT_EQ(std::as_const(*node).get_routing_state().phase, RoutingState::COMPLETED);

        manager.cleanup_completed_routing(ProcessingToken::AUDIO_RATE);
        commit(manager, ProcessingToken::AUDIO_RATE, 0);
        commit(manager, ProcessingToken::AUDIO_RATE, 1);

        EXPECT_EQ(node->get_channel_mask().load(), 0b10U);
        EXPECT_EQ(std::as_const(*node).get_routing_state().phase, RoutingState::NONE);
        EXPECT_FALSE(listed(manager, ProcessingToken::AUDIO_RATE, 0, node));
        EXPECT_TRUE(listed(manager, ProcessingToken::AUDIO_RATE, 1, node));
    }

    TEST_F(NodeGraphManagerTest, CleanupLeavesAnUnfinishedFadeAlone)
    {
        auto node = constant(1.0);
        manager.add_to_root(node, ProcessingToken::AUDIO_RATE, 0);
        manager.route_node_to_channels(node, { 1 }, 2048, ProcessingToken::AUDIO_RATE);
        manager.update_routing_states_for_cycle(ProcessingToken::AUDIO_RATE);

        manager.cleanup_completed_routing(ProcessingToken::AUDIO_RATE);

        EXPECT_EQ(std::as_const(*node).get_routing_state().phase, RoutingState::ACTIVE);
        EXPECT_EQ(node->get_channel_mask().load(), 0b11U);
    }

    TEST_F(NodeGraphManagerTest, ANodeThatKeepsEveryChannelIsNotRemovedFromAny)
    {
        auto node = constant(1.0);
        manager.add_to_root(node, ProcessingToken::AUDIO_RATE, 0);
        manager.route_node_to_channels(node, { 0, 1 }, 512, ProcessingToken::AUDIO_RATE);
        manager.update_routing_states_for_cycle(ProcessingToken::AUDIO_RATE);

        manager.cleanup_completed_routing(ProcessingToken::AUDIO_RATE);

        EXPECT_EQ(node->get_channel_mask().load(), 0b11U);
        EXPECT_TRUE(manager.is_node_registered(node));
    }

    TEST_F(NodeGraphManagerTest, OnlyAudioSinkNetworksCanBeRouted)
    {
        auto compute = std::make_shared<RecordingNetwork>(OutputMode::AUDIO_COMPUTE);

        manager.route_network_to_channels(compute, { 1 }, 512, ProcessingToken::AUDIO_RATE);

        EXPECT_FALSE(manager.is_network_registered(compute));
        EXPECT_EQ(manager.get_network_count(ProcessingToken::AUDIO_RATE), 0U);
        EXPECT_EQ(compute->get_routing_state().phase, RoutingState::NONE);
    }

    TEST_F(NodeGraphManagerTest, RoutingANetworkRegistersItOnceAndWidensItsChannels)
    {
        auto network = std::make_shared<RecordingNetwork>(OutputMode::AUDIO_SINK);
        network->add_channel_usage(0);
        network->set_enabled(false);

        manager.route_network_to_channels(network, { 2 }, 512, ProcessingToken::AUDIO_RATE);
        manager.route_network_to_channels(network, { 2 }, 512, ProcessingToken::AUDIO_RATE);

        EXPECT_TRUE(network->is_enabled());
        EXPECT_TRUE(manager.is_network_registered(network));
        EXPECT_EQ(manager.get_network_count(ProcessingToken::AUDIO_RATE), 1U);
        EXPECT_EQ(network->get_channel_mask(), 0b101U);
        EXPECT_TRUE(channels_of(manager, ProcessingToken::AUDIO_RATE).contains(2));
    }

    TEST_F(NodeGraphManagerTest, RoutingANetworkStartsAFadeFromItsCurrentChannels)
    {
        auto network = std::make_shared<RecordingNetwork>(OutputMode::AUDIO_SINK);
        network->add_channel_usage(0);
        manager.add_network(network, ProcessingToken::AUDIO_RATE);

        manager.route_network_to_channels(network, { 1 }, 1024, ProcessingToken::AUDIO_RATE);

        const auto& state = network->get_routing_state();
        EXPECT_EQ(state.phase, RoutingState::ACTIVE);
        EXPECT_EQ(state.from_channels, 0b01U);
        EXPECT_EQ(state.to_channels, 0b10U);
        EXPECT_DOUBLE_EQ(state.amount[0], 1.0);
        EXPECT_DOUBLE_EQ(state.amount[1], 0.0);
    }

    TEST_F(NodeGraphManagerTest, UpdatesAdvanceOnlyTheNetworksOfTheGivenToken)
    {
        auto network = std::make_shared<RecordingNetwork>(OutputMode::AUDIO_SINK);
        manager.add_network(network, ProcessingToken::AUDIO_RATE);
        manager.route_network_to_channels(network, { 1 }, 1024, ProcessingToken::AUDIO_RATE);

        manager.update_routing_states_for_cycle(ProcessingToken::VISUAL_RATE);
        EXPECT_EQ(network->get_routing_state().cycles_elapsed, 0U);

        manager.update_routing_states_for_cycle(ProcessingToken::AUDIO_RATE);
        EXPECT_EQ(network->get_routing_state().cycles_elapsed, 1U);
    }

    TEST_F(NodeGraphManagerTest, CompletedNetworkRoutingNarrowsTheMaskToTheTargets)
    {
        auto network = std::make_shared<RecordingNetwork>(OutputMode::AUDIO_SINK);
        network->add_channel_usage(0);
        manager.add_network(network, ProcessingToken::AUDIO_RATE);
        manager.route_network_to_channels(network, { 1 }, 512, ProcessingToken::AUDIO_RATE);
        manager.update_routing_states_for_cycle(ProcessingToken::AUDIO_RATE);

        manager.cleanup_completed_routing(ProcessingToken::AUDIO_RATE);

        EXPECT_EQ(network->get_registered_channels(), std::vector<uint32_t> { 1 });
        EXPECT_EQ(network->get_routing_state().phase, RoutingState::NONE);
        EXPECT_TRUE(manager.is_network_registered(network));
    }

    TEST_F(NodeGraphManagerTest, ANetworkRoutedToNoChannelsIsRemovedWhenTheFadeCompletes)
    {
        auto network = std::make_shared<RecordingNetwork>(OutputMode::AUDIO_SINK);
        network->add_channel_usage(0);
        manager.add_network(network, ProcessingToken::AUDIO_RATE);
        manager.route_network_to_channels(network, {}, 512, ProcessingToken::AUDIO_RATE);
        manager.update_routing_states_for_cycle(ProcessingToken::AUDIO_RATE);

        manager.cleanup_completed_routing(ProcessingToken::AUDIO_RATE);

        EXPECT_EQ(manager.get_network_count(ProcessingToken::AUDIO_RATE), 0U);
        EXPECT_FALSE(manager.is_network_registered(network));
    }

    TEST_F(NodeGraphManagerTest, TerminationEmptiesTheRootsAndUnregistersNetworks)
    {
        manager.add_to_root(constant(1.0), ProcessingToken::AUDIO_RATE, 0);
        manager.add_to_root(constant(1.0), ProcessingToken::VISUAL_RATE, 0);
        commit(manager, ProcessingToken::AUDIO_RATE);
        commit(manager, ProcessingToken::VISUAL_RATE);
        auto audio = std::make_shared<RecordingNetwork>(OutputMode::AUDIO_SINK);
        auto visual = std::make_shared<RecordingNetwork>();
        manager.add_network(audio, ProcessingToken::AUDIO_RATE);
        manager.add_network(visual, ProcessingToken::VISUAL_RATE);

        manager.terminate_active_processing();

        EXPECT_EQ(manager.get_node_count(ProcessingToken::AUDIO_RATE), 0U);
        EXPECT_EQ(manager.get_node_count(ProcessingToken::VISUAL_RATE), 0U);
        EXPECT_FALSE(manager.is_network_registered(audio));
        EXPECT_FALSE(manager.is_network_registered(visual));
    }

    TEST_F(NodeGraphManagerTest, TerminatingTwiceIsHarmless)
    {
        manager.add_to_root(constant(1.0), ProcessingToken::AUDIO_RATE, 0);

        manager.terminate_active_processing();

        EXPECT_NO_THROW(manager.terminate_active_processing());
    }

    TEST_F(NodeGraphManagerTest, ADestroyedManagerWithLiveNodesAndNetworksIsSafe)
    {
        auto network = std::make_shared<RecordingNetwork>();
        auto node = constant(1.0);
        {
            NodeGraphManager scoped;
            scoped.add_to_root(node, ProcessingToken::AUDIO_RATE, 0);
            scoped.add_network(network, ProcessingToken::VISUAL_RATE);
        }

        EXPECT_EQ(network->batches, 0);
        EXPECT_NE(node, nullptr);
    }

} // namespace

} // namespace MayaFlux::Test
