#include "gtest/gtest.h"

#include "MayaFlux/Nodes/Conduit/Constant.hpp"
#include "MayaFlux/Nodes/Conduit/NodeChain.hpp"
#include "MayaFlux/Nodes/Conduit/NodeCombine.hpp"
#include "MayaFlux/Nodes/Filters/FIR.hpp"
#include "MayaFlux/Nodes/Generators/Counter.hpp"
#include "MayaFlux/Nodes/Generators/Impulse.hpp"
#include "MayaFlux/Nodes/Generators/Logic.hpp"
#include "MayaFlux/Nodes/Generators/Phasor.hpp"
#include "MayaFlux/Nodes/Generators/Random.hpp"
#include "MayaFlux/Nodes/Generators/Sine.hpp"
#include "MayaFlux/Nodes/Graphics/GpuComputeNode.hpp"
#include "MayaFlux/Nodes/Input/InputNode.hpp"
#include "MayaFlux/Nodes/Input/MIDINode.hpp"
#include "MayaFlux/Nodes/Input/OSCNode.hpp"
#include "MayaFlux/Nodes/Input/TabletNode.hpp"
#include "MayaFlux/Nodes/NodeGraphManager.hpp"

namespace MayaFlux::Test {
namespace {

    using Nodes::Node;
    using Nodes::NodeContext;
    using Nodes::NodeState;

    using NodeFactory = std::function<std::shared_ptr<Node>()>;
    using ProbeHook = std::function<void(const NodeContext&)>;

    constexpr int TICKS = 3;

    class NodeProbeInput : public Nodes::Input::InputNode {
    public:
        explicit NodeProbeInput(Nodes::Input::InputConfig config = {})
            : InputNode(config)
        {
        }

        void save_state() override { }
        void restore_state() override { }
    };

    std::shared_ptr<Node> make_probe_input()
    {
        Nodes::Input::InputConfig config;
        config.smoothing = Nodes::Input::SmoothingMode::NONE;
        return std::make_shared<NodeProbeInput>(config);
    }

    void feed_input(Node& node, double value)
    {
        static_cast<Nodes::Input::InputNode&>(node).process_input(
            Core::InputValue::make_scalar(value, 1, Core::InputType::HID));
    }

    void tick_sample(Node& node)
    {
        node.process_sample(0.0);
    }

    void tick_input(Node& node)
    {
        feed_input(node, 1.0);
    }

    std::shared_ptr<Node> make_fir()
    {
        return std::make_shared<Nodes::Filters::FIR>(std::vector<double> { 0.5, 0.5 });
    }

    template <typename Concrete>
    Nodes::HookId add_typed_tick(Node& node, const ProbeHook& hook)
    {
        return static_cast<Concrete&>(node).on_tick(hook);
    }

    template <typename Concrete>
    Nodes::HookId add_typed_tick_if(Node& node, const Nodes::NodeCondition& condition, const ProbeHook& hook)
    {
        return static_cast<Concrete&>(node).on_tick_if(condition, hook);
    }

    struct HookCase {
        const char* name;
        NodeFactory make;
        std::function<void(Node&)> drive = tick_sample;
    };

    const std::vector<HookCase>& hook_cases()
    {
        static const std::vector<HookCase> all = {
            { "Sine", [] { return std::make_shared<Nodes::Generator::Sine>(440.0F, 0.5); } },
            { "Random", [] { return std::make_shared<Nodes::Generator::Random>(); } },
            { "Impulse", [] { return std::make_shared<Nodes::Generator::Impulse>(); } },
            { "Phasor", [] { return std::make_shared<Nodes::Generator::Phasor>(); } },
            { "Counter", [] { return std::make_shared<Nodes::Generator::Counter>(); } },
            { "Logic", [] { return std::make_shared<Nodes::Generator::Logic>(0.5); } },
            { "FIR", [] { return make_fir(); } },
            { "Constant", [] { return std::make_shared<Nodes::Constant>(1.0); } },
            { "BinaryOp", [] {
                 return std::make_shared<Nodes::BinaryOpNode>(
                     std::make_shared<Nodes::Constant>(1.0),
                     std::make_shared<Nodes::Constant>(2.0),
                     [](double a, double b) { return a + b; });
             } },
            { "Chain", [] {
                 return std::make_shared<Nodes::ChainNode>(
                     std::make_shared<Nodes::Generator::Sine>(440.0F, 0.5), make_fir());
             } },
            { "InputNode", [] { return make_probe_input(); }, tick_input },
        };
        return all;
    }

    std::string hook_case_name(const ::testing::TestParamInfo<HookCase>& info)
    {
        return info.param.name;
    }

    class NodeHookMachineryTest : public ::testing::TestWithParam<HookCase> {
    protected:
        void SetUp() override { node = GetParam().make(); }

        void tick(int times = TICKS)
        {
            for (int i = 0; i < times; ++i) {
                GetParam().drive(*node);
            }
        }

        std::shared_ptr<Node> node;
        int count {};
        bool gate {};
    };

    TEST_P(NodeHookMachineryTest, TickHookFiresOncePerProcessedSample)
    {
        node->on_tick([this](NodeContext&) { ++count; });

        tick();

        EXPECT_EQ(count, TICKS);
    }

    TEST_P(NodeHookMachineryTest, SameCallbackRegisteredTwiceIsDropped)
    {
        auto hook = [this](NodeContext&) { ++count; };
        const auto first = node->on_tick(hook);
        const auto second = node->on_tick(hook);

        tick(1);

        EXPECT_EQ(count, 1);
        EXPECT_EQ(first, second);
    }

    TEST_P(NodeHookMachineryTest, UnhookRemovesExactlyOneRegistration)
    {
        int other = 0;
        const auto first = node->on_tick([this](NodeContext&) { ++count; });
        const auto second = node->on_tick([&other](NodeContext&) { ++other; });

        EXPECT_NE(first, 0U);
        EXPECT_NE(first, second);
        EXPECT_TRUE(node->unhook(first));
        EXPECT_FALSE(node->unhook(first));
        tick(1);

        EXPECT_EQ(count, 0);
        EXPECT_EQ(other, 1);
    }

    TEST_P(NodeHookMachineryTest, UnhookRemovesAConditionalRegistration)
    {
        gate = true;
        const auto id = node->on_tick_if(
            [this](NodeContext&) { return gate; },
            [this](NodeContext&) { ++count; });

        EXPECT_NE(id, 0U);
        EXPECT_TRUE(node->unhook(id));
        tick(1);

        EXPECT_EQ(count, 0);
    }

    TEST_P(NodeHookMachineryTest, DistinctCallbacksAllFire)
    {
        int other = 0;
        node->on_tick([this](NodeContext&) { ++count; });
        node->on_tick([&other](NodeContext&) { ++other; });

        tick(1);

        EXPECT_EQ(count, 1);
        EXPECT_EQ(other, 1);
    }

    TEST_P(NodeHookMachineryTest, ConditionalHookFollowsItsCondition)
    {
        node->on_tick_if(
            [this](NodeContext&) { return gate; },
            [this](NodeContext&) { ++count; });

        gate = false;
        tick(1);
        EXPECT_EQ(count, 0);

        gate = true;
        tick(1);
        EXPECT_EQ(count, 1);
    }

    TEST_P(NodeHookMachineryTest, RemoveHookReportsAndStopsTheCallback)
    {
        auto hook = [this](NodeContext&) { ++count; };
        node->on_tick(hook);

        const bool removed = node->remove_hook(hook);
        tick(1);

        EXPECT_TRUE(removed);
        EXPECT_EQ(count, 0);
    }

    TEST_P(NodeHookMachineryTest, RemoveHookOfUnknownCallbackReturnsFalse)
    {
        node->on_tick([this](NodeContext&) { ++count; });

        EXPECT_FALSE(node->remove_hook([](NodeContext&) { }));
    }

    TEST_P(NodeHookMachineryTest, RemoveConditionalHookReportsAndStopsTheCallback)
    {
        auto condition = [this](NodeContext&) { return gate; };
        node->on_tick_if(condition, [this](NodeContext&) { ++count; });
        gate = true;

        const bool removed = node->remove_conditional_hook(condition);
        tick(1);

        EXPECT_TRUE(removed);
        EXPECT_EQ(count, 0);
    }

    TEST_P(NodeHookMachineryTest, RemoveAllHooksClearsTheBaseLists)
    {
        auto hook = [this](NodeContext&) { ++count; };
        auto condition = [](NodeContext&) { return true; };
        node->on_tick(hook);
        node->on_tick_if(condition, hook);

        node->remove_all_hooks();
        tick(1);

        EXPECT_EQ(count, 0);
        EXPECT_FALSE(node->remove_hook(hook));
        EXPECT_FALSE(node->remove_conditional_hook(condition));
    }

    TEST_P(NodeHookMachineryTest, HooksCanBeRegisteredAgainAfterRemoveAll)
    {
        auto hook = [this](NodeContext&) { ++count; };
        node->on_tick(hook);
        node->remove_all_hooks();
        node->on_tick(hook);

        tick(1);

        EXPECT_EQ(count, 1);
    }

    INSTANTIATE_TEST_SUITE_P(
        Nodes,
        NodeHookMachineryTest,
        ::testing::ValuesIn(hook_cases()),
        hook_case_name);

    struct TypedCase {
        const char* name;
        NodeFactory make;
        std::function<Nodes::HookId(Node&, const ProbeHook&)> add_tick;
        std::function<Nodes::HookId(Node&, const Nodes::NodeCondition&, const ProbeHook&)> add_tick_if;
        std::function<void(Node&)> drive = tick_sample;
    };

    const std::vector<TypedCase>& typed_cases()
    {
        static const std::vector<TypedCase> all = {
            { "Sine", [] { return std::make_shared<Nodes::Generator::Sine>(440.0F, 0.5); },
                add_typed_tick<Nodes::Generator::Generator>,
                add_typed_tick_if<Nodes::Generator::Generator> },
            { "Phasor", [] { return std::make_shared<Nodes::Generator::Phasor>(); },
                add_typed_tick<Nodes::Generator::Generator>,
                add_typed_tick_if<Nodes::Generator::Generator> },
            { "FIR", [] { return make_fir(); },
                add_typed_tick<Nodes::Filters::Filter>,
                add_typed_tick_if<Nodes::Filters::Filter> },
            { "Logic", [] { return std::make_shared<Nodes::Generator::Logic>(0.5); },
                add_typed_tick<Nodes::Generator::Logic>,
                add_typed_tick_if<Nodes::Generator::Logic> },
            { "InputNode", [] { return make_probe_input(); },
                add_typed_tick<Nodes::Input::InputNode>,
                add_typed_tick_if<Nodes::Input::InputNode>,
                tick_input },
        };
        return all;
    }

    std::string typed_case_name(const ::testing::TestParamInfo<TypedCase>& info)
    {
        return info.param.name;
    }

    class NodeTypedHookMachineryTest : public ::testing::TestWithParam<TypedCase> {
    protected:
        void SetUp() override { node = GetParam().make(); }

        void tick(int times = TICKS)
        {
            for (int i = 0; i < times; ++i) {
                GetParam().drive(*node);
            }
        }

        std::shared_ptr<Node> node;
        int count {};
        bool gate {};
    };

    TEST_P(NodeTypedHookMachineryTest, TypedTickHookFiresOncePerProcessedSample)
    {
        GetParam().add_tick(*node, [this](const NodeContext&) { ++count; });

        tick();

        EXPECT_EQ(count, TICKS);
    }

    TEST_P(NodeTypedHookMachineryTest, TypedTickHookIsNotDeduplicated)
    {
        const ProbeHook hook = [this](const NodeContext&) { ++count; };
        GetParam().add_tick(*node, hook);
        GetParam().add_tick(*node, hook);

        tick(1);

        EXPECT_EQ(count, 2);
    }

    TEST_P(NodeTypedHookMachineryTest, TypedConditionalHookFollowsItsCondition)
    {
        GetParam().add_tick_if(
            *node,
            [this](NodeContext&) { return gate; },
            [this](const NodeContext&) { ++count; });

        gate = false;
        tick(1);
        EXPECT_EQ(count, 0);

        gate = true;
        tick(1);
        EXPECT_EQ(count, 1);
    }

    TEST_P(NodeTypedHookMachineryTest, TypedTickHookIsClearedByRemoveAllHooks)
    {
        GetParam().add_tick(*node, [this](const NodeContext&) { ++count; });

        node->remove_all_hooks();
        tick(1);

        EXPECT_EQ(count, 0);
    }

    TEST_P(NodeTypedHookMachineryTest, TypedTickHookIsRemovableByItsCallableThroughTheBaseInterface)
    {
        const ProbeHook hook = [this](const NodeContext&) { ++count; };
        GetParam().add_tick(*node, hook);

        const bool removed = node->remove_hook(hook);
        tick(1);

        EXPECT_TRUE(removed);
        EXPECT_EQ(count, 0);
    }

    TEST_P(NodeTypedHookMachineryTest, TypedTickHookIsRemovableById)
    {
        const auto id = GetParam().add_tick(*node, [this](const NodeContext&) { ++count; });

        EXPECT_NE(id, 0U);
        EXPECT_TRUE(node->unhook(id));
        tick(1);

        EXPECT_EQ(count, 0);
    }

    TEST_P(NodeTypedHookMachineryTest, TypedConditionalHookIsRemovableById)
    {
        gate = true;
        const auto id = GetParam().add_tick_if(
            *node,
            [this](NodeContext&) { return gate; },
            [this](const NodeContext&) { ++count; });

        EXPECT_NE(id, 0U);
        EXPECT_TRUE(node->unhook(id));
        tick(1);

        EXPECT_EQ(count, 0);
    }

    INSTANTIATE_TEST_SUITE_P(
        Nodes,
        NodeTypedHookMachineryTest,
        ::testing::ValuesIn(typed_cases()),
        typed_case_name);

    struct EventCase {
        const char* name;
        NodeFactory make;
        std::function<Nodes::HookId(Node&, const ProbeHook&)> attach;
        std::function<void(Node&)> drive;
    };

    void tick_times(Node& node, int times)
    {
        for (int i = 0; i < times; ++i) {
            node.process_sample(0.0);
        }
    }

    void drive_logic(Node& node)
    {
        auto& logic = static_cast<Nodes::Generator::Logic&>(node);
        logic.process_sample(1.0);
        logic.process_sample(0.0);
        logic.process_sample(1.0);
    }

    void drive_input(Node& node)
    {
        feed_input(node, 1.0);
        feed_input(node, 0.0);
        feed_input(node, 1.0);
    }

    void drive_tablet(Node& node)
    {
        auto& tablet = static_cast<Nodes::Input::TabletNode&>(node);
        std::vector<double> frame(Core::TabletFrame::SLOT_COUNT, 0.0);

        frame[Core::TabletFrame::STATE] = static_cast<double>(
            static_cast<uint32_t>(Core::TabletState::IN_PROXIMITY) | static_cast<uint32_t>(Core::TabletState::IN_CONTACT));
        tablet.process_input(Core::InputValue::make_vector(frame, 1, Core::InputType::TABLET));

        frame[Core::TabletFrame::STATE] = 0.0;
        tablet.process_input(Core::InputValue::make_vector(frame, 1, Core::InputType::TABLET));
    }

    void drive_midi(Node& node, uint8_t status, uint8_t data1, uint8_t data2)
    {
        static_cast<Nodes::Input::MIDINode&>(node).process_input(
            Core::InputValue::make_midi(status, data1, data2, 1));
    }

    void drive_osc(Node& node)
    {
        static_cast<Nodes::Input::OSCNode&>(node).process_input(
            Core::InputValue::make_osc("/probe", std::vector<Core::InputValue::OSCArg> { 1.0F }, 1));
    }

    std::shared_ptr<Node> make_gpu_compute()
    {
        return std::make_shared<Nodes::GpuSync::GpuComputeNode>(
            std::make_shared<Yantra::ShaderExecutionContext<>>(Portal::Graphics::GpuComputeConfig {}));
    }

    void drive_gpu(Node& node)
    {
        static_cast<Nodes::GpuSync::GpuComputeNode&>(node).notify_tick(0.0);
    }

    const std::vector<EventCase>& event_cases()
    {
        using Nodes::Generator::Counter;
        using Nodes::Generator::Impulse;
        using Nodes::Generator::Logic;
        using Nodes::Generator::Phasor;
        using Nodes::Input::InputNode;

        static const std::vector<EventCase> all = {
            { "CounterIncrement", [] { return std::make_shared<Counter>(3U, 1); },
                [](Node& n, const ProbeHook& h) { return static_cast<Counter&>(n).on_increment(h); },
                [](Node& n) { tick_times(n, TICKS); } },
            { "CounterWrap", [] { return std::make_shared<Counter>(3U, 1); },
                [](Node& n, const ProbeHook& h) { return static_cast<Counter&>(n).on_wrap(h); },
                [](Node& n) { tick_times(n, TICKS); } },
            { "CounterCount", [] { return std::make_shared<Counter>(3U, 1); },
                [](Node& n, const ProbeHook& h) { return static_cast<Counter&>(n).on_count(2U, h); },
                [](Node& n) { tick_times(n, TICKS); } },
            { "PhasorWrap", [] { return std::make_shared<Phasor>(24000.0F); },
                [](Node& n, const ProbeHook& h) { return static_cast<Phasor&>(n).on_phase_wrap(h); },
                [](Node& n) { tick_times(n, 8); } },
            { "PhasorThreshold", [] { return std::make_shared<Phasor>(24000.0F); },
                [](Node& n, const ProbeHook& h) { return static_cast<Phasor&>(n).on_threshold(h, 0.25); },
                [](Node& n) { tick_times(n, 8); } },
            { "ImpulseImpulse", [] { return std::make_shared<Impulse>(24000.0F); },
                [](Node& n, const ProbeHook& h) { return static_cast<Impulse&>(n).on_impulse(h); },
                [](Node& n) { tick_times(n, 8); } },
            { "LogicChange", [] { return std::make_shared<Logic>(0.5); },
                [](Node& n, const ProbeHook& h) { return static_cast<Logic&>(n).on_change(h); },
                drive_logic },
            { "LogicWhileTrue", [] { return std::make_shared<Logic>(0.5); },
                [](Node& n, const ProbeHook& h) { return static_cast<Logic&>(n).while_true(h); },
                drive_logic },
            { "LogicChangeToTrue", [] { return std::make_shared<Logic>(0.5); },
                [](Node& n, const ProbeHook& h) { return static_cast<Logic&>(n).on_change_to(true, h); },
                drive_logic },
            { "InputValueChange", [] { return make_probe_input(); },
                [](Node& n, const ProbeHook& h) { return static_cast<InputNode&>(n).on_value_change(h); },
                drive_input },
            { "InputThresholdRising", [] { return make_probe_input(); },
                [](Node& n, const ProbeHook& h) { return static_cast<InputNode&>(n).on_threshold_rising(0.5, h); },
                drive_input },
            { "TabletFrame", [] { return std::make_shared<Nodes::Input::TabletNode>(); },
                [](Node& n, const ProbeHook& h) { return static_cast<Nodes::Input::TabletNode&>(n).on_frame(h); },
                drive_tablet },
            { "TabletProximityEnter", [] { return std::make_shared<Nodes::Input::TabletNode>(); },
                [](Node& n, const ProbeHook& h) { return static_cast<Nodes::Input::TabletNode&>(n).on_proximity_enter(h); },
                drive_tablet },
            { "TabletProximityExit", [] { return std::make_shared<Nodes::Input::TabletNode>(); },
                [](Node& n, const ProbeHook& h) { return static_cast<Nodes::Input::TabletNode&>(n).on_proximity_exit(h); },
                drive_tablet },
            { "TabletContactBegin", [] { return std::make_shared<Nodes::Input::TabletNode>(); },
                [](Node& n, const ProbeHook& h) { return static_cast<Nodes::Input::TabletNode&>(n).on_contact_begin(h); },
                drive_tablet },
            { "TabletContactEnd", [] { return std::make_shared<Nodes::Input::TabletNode>(); },
                [](Node& n, const ProbeHook& h) { return static_cast<Nodes::Input::TabletNode&>(n).on_contact_end(h); },
                drive_tablet },
            { "MidiNote", [] { return std::make_shared<Nodes::Input::MIDINode>(); },
                [](Node& n, const ProbeHook& h) {
                    return static_cast<Nodes::Input::MIDINode&>(n).on_note([h, &n](uint8_t, uint8_t, bool) { h(n.get_last_context()); });
                },
                [](Node& n) { drive_midi(n, 0x90, 60, 100); } },
            { "MidiCc", [] { return std::make_shared<Nodes::Input::MIDINode>(); },
                [](Node& n, const ProbeHook& h) {
                    return static_cast<Nodes::Input::MIDINode&>(n).on_cc([h, &n](uint8_t, uint8_t) { h(n.get_last_context()); });
                },
                [](Node& n) { drive_midi(n, 0xB0, 1, 64); } },
            { "MidiPitchBend", [] { return std::make_shared<Nodes::Input::MIDINode>(); },
                [](Node& n, const ProbeHook& h) {
                    return static_cast<Nodes::Input::MIDINode&>(n).on_pitch_bend([h, &n](int16_t) { h(n.get_last_context()); });
                },
                [](Node& n) { drive_midi(n, 0xE0, 0, 64); } },
            { "OscMessage", [] { return std::make_shared<Nodes::Input::OSCNode>(); },
                [](Node& n, const ProbeHook& h) {
                    return static_cast<Nodes::Input::OSCNode&>(n).on_message(
                        [h, &n](const std::string&, const std::vector<Core::InputValue::OSCArg>&) { h(n.get_last_context()); });
                },
                drive_osc },
            { "GpuComplete", [] { return make_gpu_compute(); },
                [](Node& n, const ProbeHook& h) { return static_cast<Nodes::GpuSync::GpuComputeNode&>(n).on_complete(h); },
                drive_gpu },
            { "GpuBaseTick", [] { return make_gpu_compute(); },
                [](Node& n, const ProbeHook& h) { return n.on_tick([h](NodeContext& ctx) { h(ctx); }); },
                drive_gpu },
            { "InputButtonPress", [] { return make_probe_input(); },
                [](Node& n, const ProbeHook& h) { return static_cast<InputNode&>(n).on_button_press(h); },
                drive_input },
        };
        return all;
    }

    std::string event_case_name(const ::testing::TestParamInfo<EventCase>& info)
    {
        return info.param.name;
    }

    class NodeEventChannelTest : public ::testing::TestWithParam<EventCase> {
    protected:
        void SetUp() override { node = GetParam().make(); }

        std::shared_ptr<Node> node;
        int count {};
    };

    TEST_P(NodeEventChannelTest, EventHookFiresForItsEvent)
    {
        GetParam().attach(*node, [this](const NodeContext&) { ++count; });

        GetParam().drive(*node);

        EXPECT_GT(count, 0);
    }

    TEST_P(NodeEventChannelTest, RemoveAllHooksReachesTheEventChannel)
    {
        GetParam().attach(*node, [this](const NodeContext&) { ++count; });

        node->remove_all_hooks();
        GetParam().drive(*node);

        EXPECT_EQ(count, 0);
    }

    TEST_P(NodeEventChannelTest, EventHookIsRemovableById)
    {
        const auto id = GetParam().attach(*node, [this](const NodeContext&) { ++count; });

        EXPECT_NE(id, 0U);
        EXPECT_TRUE(node->unhook(id));
        EXPECT_FALSE(node->unhook(id));
        GetParam().drive(*node);

        EXPECT_EQ(count, 0);
    }

    INSTANTIATE_TEST_SUITE_P(
        Channels,
        NodeEventChannelTest,
        ::testing::ValuesIn(event_cases()),
        event_case_name);

    class NodeChannelUsageTest : public ::testing::TestWithParam<uint32_t> {
    protected:
        Nodes::Constant node { 1.0 };
    };

    TEST_P(NodeChannelUsageTest, RegisteredChannelIsReportedAndOthersAreNot)
    {
        const uint32_t channel = GetParam();

        node.register_channel_usage(channel);

        EXPECT_TRUE(node.is_used_by_channel(channel));
        EXPECT_EQ(node.get_channel_mask().load(), 1U << channel);
        EXPECT_FALSE(node.is_used_by_channel((channel + 1) % 32));
    }

    TEST_P(NodeChannelUsageTest, UnregisteredChannelIsNoLongerReported)
    {
        const uint32_t channel = GetParam();
        node.register_channel_usage(channel);

        node.unregister_channel_usage(channel);

        EXPECT_FALSE(node.is_used_by_channel(channel));
        EXPECT_EQ(node.get_channel_mask().load(), 0U);
    }

    TEST_P(NodeChannelUsageTest, RegisteringTwiceKeepsOneBit)
    {
        const uint32_t channel = GetParam();

        node.register_channel_usage(channel);
        node.register_channel_usage(channel);

        EXPECT_EQ(node.get_channel_mask().load(), 1U << channel);
    }

    INSTANTIATE_TEST_SUITE_P(
        Channels,
        NodeChannelUsageTest,
        ::testing::Values(0U, 1U, 7U, 16U, 31U));

    class NodeChannelRangeTest : public ::testing::TestWithParam<uint32_t> {
    protected:
        Nodes::Constant node { 1.0 };
    };

    TEST_P(NodeChannelRangeTest, ChannelsBeyondTheMaskAreIgnored)
    {
        const uint32_t channel = GetParam();

        node.register_channel_usage(channel);

        EXPECT_FALSE(node.is_used_by_channel(channel));
        EXPECT_EQ(node.get_channel_mask().load(), 0U);
    }

    INSTANTIATE_TEST_SUITE_P(
        Channels,
        NodeChannelRangeTest,
        ::testing::Values(32U, 33U, 1000U));

    struct ResetCase {
        const char* name;
        std::vector<uint32_t> channels;
    };

    class NodeResetRequestTest : public ::testing::TestWithParam<ResetCase> {
    protected:
        void SetUp() override
        {
            for (const uint32_t channel : GetParam().channels) {
                node.register_channel_usage(channel);
            }
        }

        [[nodiscard]] bool processed() const
        {
            return (node.m_state.load() & NodeState::PROCESSED) != 0;
        }

        Nodes::Constant node { 1.0 };
    };

    TEST_P(NodeResetRequestTest, ProcessedStateClearsOnlyWhenEveryActiveChannelHasRequested)
    {
        const auto& channels = GetParam().channels;
        Nodes::atomic_add_flag(node.m_state, NodeState::PROCESSED);

        for (size_t i = 0; i + 1 < channels.size(); ++i) {
            node.request_reset_from_channel(channels[i]);
            EXPECT_TRUE(processed());
        }

        node.request_reset_from_channel(channels.back());
        EXPECT_FALSE(processed());
    }

    TEST_P(NodeResetRequestTest, PendingRequestsAreConsumedByTheReset)
    {
        const auto& channels = GetParam().channels;
        for (const uint32_t channel : channels) {
            node.request_reset_from_channel(channel);
        }
        Nodes::atomic_add_flag(node.m_state, NodeState::PROCESSED);

        node.request_reset_from_channel(channels.front());

        EXPECT_EQ(processed(), channels.size() > 1);
    }

    TEST_P(NodeResetRequestTest, ResetProcessedStateIsIgnoredWhileChannelsAreActive)
    {
        Nodes::atomic_add_flag(node.m_state, NodeState::PROCESSED);

        node.reset_processed_state();

        EXPECT_TRUE(processed());
    }

    INSTANTIATE_TEST_SUITE_P(
        Channels,
        NodeResetRequestTest,
        ::testing::Values(
            ResetCase { "One", { 0 } },
            ResetCase { "Two", { 0, 1 } },
            ResetCase { "Three", { 0, 1, 2 } },
            ResetCase { "Sparse", { 3, 7, 31 } }),
        [](const ::testing::TestParamInfo<ResetCase>& info) { return std::string(info.param.name); });

    class NodeProcessedStateTest : public ::testing::TestWithParam<bool> {
    protected:
        Nodes::Constant node { 1.0 };
    };

    TEST_P(NodeProcessedStateTest, ResetProcessedStateClearsOnlyWithoutActiveChannels)
    {
        const bool has_channel = GetParam();
        if (has_channel) {
            node.register_channel_usage(0);
        }
        Nodes::atomic_add_flag(node.m_state, NodeState::PROCESSED);

        node.reset_processed_state();

        EXPECT_EQ((node.m_state.load() & NodeState::PROCESSED) != 0, has_channel);
    }

    INSTANTIATE_TEST_SUITE_P(
        Channels,
        NodeProcessedStateTest,
        ::testing::Bool());

    class NodeFlagsTest : public ::testing::TestWithParam<bool> {
    protected:
        Nodes::Constant node { 1.0 };
    };

    TEST_P(NodeFlagsTest, OutputExclusionFollowsTheRequest)
    {
        EXPECT_FALSE(node.is_output_excluded_from_graph());

        node.exclude_output_from_graph(GetParam());

        EXPECT_EQ(node.is_output_excluded_from_graph(), GetParam());
    }

    TEST_P(NodeFlagsTest, OutputExclusionCanBeRestored)
    {
        node.exclude_output_from_graph(true);

        node.exclude_output_from_graph(!GetParam());

        EXPECT_EQ(node.is_output_excluded_from_graph(), !GetParam());
    }

    TEST_P(NodeFlagsTest, GpuCompatibilityFollowsTheRequest)
    {
        EXPECT_FALSE(node.is_gpu_compatible());

        node.set_gpu_compatible(GetParam());

        EXPECT_EQ(node.is_gpu_compatible(), GetParam());
    }

    TEST_P(NodeFlagsTest, NetworkMembershipFollowsTheRequest)
    {
        EXPECT_FALSE(node.is_in_network());

        node.set_in_network(GetParam());

        EXPECT_EQ(node.is_in_network(), GetParam());
    }

    INSTANTIATE_TEST_SUITE_P(
        Flags,
        NodeFlagsTest,
        ::testing::Bool());

    struct RateCase {
        uint32_t sample_rate;
        uint32_t frame_rate;
    };

    class NodeRateTest : public ::testing::TestWithParam<RateCase> {
    protected:
        Nodes::Constant node { 1.0 };
    };

    TEST_P(NodeRateTest, DefaultsAreAudioAndDisplayRates)
    {
        EXPECT_EQ(node.get_sample_rate(), 48000U);
        EXPECT_EQ(node.get_frame_rate(), 60U);
    }

    TEST_P(NodeRateTest, RatesFollowTheirSetters)
    {
        node.set_sample_rate(GetParam().sample_rate);
        node.set_frame_rate(GetParam().frame_rate);

        EXPECT_EQ(node.get_sample_rate(), GetParam().sample_rate);
        EXPECT_EQ(node.get_frame_rate(), GetParam().frame_rate);
    }

    INSTANTIATE_TEST_SUITE_P(
        Rates,
        NodeRateTest,
        ::testing::Values(
            RateCase { 44100, 30 },
            RateCase { 48000, 60 },
            RateCase { 96000, 144 }));

    class NodeSnapshotContextTest : public ::testing::TestWithParam<uint64_t> {
    protected:
        Nodes::Constant node { 1.0 };
    };

    TEST_P(NodeSnapshotContextTest, StartsWithoutAnActiveSnapshot)
    {
        EXPECT_FALSE(node.has_active_snapshot());
        EXPECT_EQ(node.get_active_snapshot_context(), 0U);
        EXPECT_FALSE(node.is_in_snapshot_context(GetParam()));
    }

    TEST_P(NodeSnapshotContextTest, FirstClaimWinsAndHoldsUntilReleased)
    {
        const uint64_t id = GetParam();
        const uint64_t other = id ^ 1U;

        EXPECT_TRUE(node.try_claim_snapshot_context(id));
        EXPECT_TRUE(node.has_active_snapshot());
        EXPECT_TRUE(node.is_in_snapshot_context(id));
        EXPECT_EQ(node.get_active_snapshot_context(), id);

        EXPECT_FALSE(node.try_claim_snapshot_context(id));
        EXPECT_FALSE(node.try_claim_snapshot_context(other));
        EXPECT_TRUE(node.is_in_snapshot_context(id));
    }

    TEST_P(NodeSnapshotContextTest, OnlyTheOwnerCanRelease)
    {
        const uint64_t id = GetParam();
        const uint64_t other = id ^ 1U;
        node.try_claim_snapshot_context(id);

        node.release_snapshot_context(other);
        EXPECT_TRUE(node.is_in_snapshot_context(id));

        node.release_snapshot_context(id);
        EXPECT_FALSE(node.has_active_snapshot());
        EXPECT_FALSE(node.is_in_snapshot_context(id));
    }

    TEST_P(NodeSnapshotContextTest, ReleasedNodeCanBeClaimedByAnotherContext)
    {
        const uint64_t id = GetParam();
        const uint64_t other = id ^ 1U;
        node.try_claim_snapshot_context(id);
        node.release_snapshot_context(id);

        EXPECT_TRUE(node.try_claim_snapshot_context(other));
        EXPECT_TRUE(node.is_in_snapshot_context(other));
    }

    INSTANTIATE_TEST_SUITE_P(
        Contexts,
        NodeSnapshotContextTest,
        ::testing::Values(uint64_t { 1 }, uint64_t { 42 }, uint64_t { 0xFFFFFFFFFFFFFFFFULL }));

    class NodeBufferReferenceTest : public ::testing::TestWithParam<uint32_t> {
    protected:
        void SetUp() override
        {
            for (uint32_t i = 0; i < GetParam(); ++i) {
                node.add_buffer_reference();
            }
        }

        Nodes::Constant node { 1.0 };
    };

    TEST_P(NodeBufferReferenceTest, FirstMarkSucceedsOnlyWithAReference)
    {
        EXPECT_EQ(node.mark_buffer_processed(), GetParam() >= 1);
    }

    TEST_P(NodeBufferReferenceTest, SecondMarkSucceedsOnlyForASingleReference)
    {
        node.mark_buffer_processed();

        EXPECT_EQ(node.mark_buffer_processed(), GetParam() == 1);
    }

    TEST_P(NodeBufferReferenceTest, SharedReferencesFlagTheNodeAsProcessed)
    {
        node.mark_buffer_processed();

        EXPECT_EQ(node.is_buffer_processed(), GetParam() >= 2);
    }

    TEST_P(NodeBufferReferenceTest, ProcessedFlagClearsOnTheLastExpectedReset)
    {
        const uint32_t references = GetParam();
        node.mark_buffer_processed();

        for (uint32_t request = 1; request < references; ++request) {
            node.request_buffer_reset();
            EXPECT_EQ(node.is_buffer_processed(), request < references - 1);
        }
    }

    TEST_P(NodeBufferReferenceTest, ActiveNodeRefusesTheMark)
    {
        Nodes::atomic_add_flag(node.m_state, NodeState::ACTIVE);

        EXPECT_FALSE(node.mark_buffer_processed());
    }

    TEST_P(NodeBufferReferenceTest, RemovingEveryReferenceRestoresTheRefusal)
    {
        for (uint32_t i = 0; i < GetParam(); ++i) {
            node.remove_buffer_reference();
        }

        EXPECT_FALSE(node.mark_buffer_processed());
    }

    INSTANTIATE_TEST_SUITE_P(
        References,
        NodeBufferReferenceTest,
        ::testing::Values(0U, 1U, 2U, 3U, 4U));

    struct ModulatorCase {
        const char* name;
        NodeFactory make;
        std::vector<Nodes::ModulatorRole> roles;
        size_t tree_size;
    };

    size_t tree_size(const std::vector<Nodes::ModulatorTree>& tree)
    {
        size_t total = 0;
        for (const auto& entry : tree) {
            total += 1 + tree_size(entry.modulators);
        }
        return total;
    }

    const std::vector<ModulatorCase>& modulator_cases()
    {
        using Nodes::ModulatorRole;

        static const std::vector<ModulatorCase> all = {
            { "Constant", [] { return std::make_shared<Nodes::Constant>(1.0); }, {}, 0 },
            { "FirWithoutInput", [] { return make_fir(); }, {}, 0 },
            { "FirWithInput", [] {
                 return std::make_shared<Nodes::Filters::FIR>(
                     std::make_shared<Nodes::Constant>(1.0), std::vector<double> { 0.5, 0.5 });
             },
                { ModulatorRole::SignalMod }, 1 },
            { "BinaryOp", [] {
                 return std::make_shared<Nodes::BinaryOpNode>(
                     std::make_shared<Nodes::Constant>(1.0),
                     std::make_shared<Nodes::Constant>(2.0),
                     [](double a, double b) { return a + b; });
             },
                { ModulatorRole::Lhs, ModulatorRole::Rhs }, 2 },
            { "ChainOfFilterWithInput", [] {
                 return std::make_shared<Nodes::ChainNode>(
                     std::make_shared<Nodes::Constant>(1.0),
                     std::make_shared<Nodes::Filters::FIR>(
                         std::make_shared<Nodes::Constant>(2.0), std::vector<double> { 0.5, 0.5 }));
             },
                { ModulatorRole::ChainLink, ModulatorRole::ChainLink }, 3 },
        };
        return all;
    }

    std::string modulator_case_name(const ::testing::TestParamInfo<ModulatorCase>& info)
    {
        return info.param.name;
    }

    class NodeModulatorTreeTest : public ::testing::TestWithParam<ModulatorCase> { };

    TEST_P(NodeModulatorTreeTest, DirectModulatorsReportTheirRoles)
    {
        const auto node = GetParam().make();

        const auto modulators = node->get_modulators();

        ASSERT_EQ(modulators.size(), GetParam().roles.size());
        for (size_t i = 0; i < modulators.size(); ++i) {
            EXPECT_EQ(modulators[i].first, GetParam().roles[i]);
            EXPECT_NE(modulators[i].second, nullptr);
        }
    }

    TEST_P(NodeModulatorTreeTest, TreeCoversEveryReachableModulator)
    {
        const auto node = GetParam().make();

        EXPECT_EQ(tree_size(node->get_modulator_tree()), GetParam().tree_size);
    }

    INSTANTIATE_TEST_SUITE_P(
        Nodes,
        NodeModulatorTreeTest,
        ::testing::ValuesIn(modulator_cases()),
        modulator_case_name);

    std::vector<EventCase> input_event_cases()
    {
        std::vector<EventCase> selected;
        std::ranges::copy_if(event_cases(), std::back_inserter(selected), [](const EventCase& entry) {
            const std::string_view name = entry.name;
            return name.starts_with("Input") || name.starts_with("Tablet") || name.starts_with("Midi") || name.starts_with("Osc");
        });
        return selected;
    }

    class NodeInputNotifyTest : public ::testing::TestWithParam<EventCase> {
    protected:
        void SetUp() override { node = GetParam().make(); }

        std::shared_ptr<Node> node;
        int count {};
    };

    TEST_P(NodeInputNotifyTest, ProcessSampleDoesNotRunInputHooks)
    {
        GetParam().attach(*node, [this](const NodeContext&) { ++count; });

        for (int i = 0; i < 10; ++i) {
            node->process_sample(0.0);
        }

        EXPECT_EQ(count, 0);
    }

    TEST_P(NodeInputNotifyTest, HooksRunOnlyOnTheThreadThatDeliversInput)
    {
        std::atomic<size_t> owner_thread { 0 };
        std::atomic<bool> foreign { false };
        std::atomic<int> fired { 0 };

        GetParam().attach(*node, [&](const NodeContext&) {
            const size_t me = std::hash<std::thread::id> {}(std::this_thread::get_id());
            size_t expected = 0;
            if (!owner_thread.compare_exchange_strong(expected, me) && expected != me) {
                foreign.store(true);
            }
            fired.fetch_add(1);
        });

        std::atomic<bool> stop { false };
        std::thread graph([&] {
            while (!stop.load()) {
                node->process_sample(0.0);
            }
        });
        std::thread feeder([&] {
            for (int i = 0; i < 200; ++i) {
                GetParam().drive(*node);
            }
        });

        feeder.join();
        stop.store(true);
        graph.join();

        EXPECT_GT(fired.load(), 0);
        EXPECT_FALSE(foreign.load());
    }

    INSTANTIATE_TEST_SUITE_P(
        Inputs,
        NodeInputNotifyTest,
        ::testing::ValuesIn(input_event_cases()),
        event_case_name);

    class NodeExternalHookNode : public Node {
    public:
        double process_sample(double /*input*/) override
        {
            notify_tick(1.0);
            return 1.0;
        }

        std::vector<double> process_batch(unsigned int num_samples) override
        {
            return std::vector<double>(num_samples, 1.0);
        }

        void update_context(double value) override { m_context.value = value; }
        NodeContext& get_last_context() override { return m_context; }
        void save_state() override { }
        void restore_state() override { }

    protected:
        void notify_tick(double value) override
        {
            update_context(value);
            m_callbacks.fire(m_context);
            m_conditional_callbacks.fire(m_context);
        }

    private:
        struct Context : NodeContext {
            Context()
                : NodeContext(0.0)
            {
            }
        };

        Context m_context;
    };

    class NodeExternalSubclassTest : public ::testing::TestWithParam<bool> {
    protected:
        Nodes::HookId attach()
        {
            if (GetParam()) {
                return node.on_tick_if([](NodeContext&) { return true; }, [this](NodeContext&) { ++count; });
            }
            return node.on_tick([this](NodeContext&) { ++count; });
        }

        NodeExternalHookNode node;
        int count {};
    };

    TEST_P(NodeExternalSubclassTest, HooksFireThroughTheBaseLists)
    {
        attach();

        node.process_sample(0.0);

        EXPECT_EQ(count, 1);
    }

    TEST_P(NodeExternalSubclassTest, UnhookRemovesTheRegistration)
    {
        const auto id = attach();

        EXPECT_TRUE(node.unhook(id));
        node.process_sample(0.0);

        EXPECT_EQ(count, 0);
    }

    TEST_P(NodeExternalSubclassTest, RemoveAllHooksClearsTheBaseLists)
    {
        attach();

        node.remove_all_hooks();
        node.process_sample(0.0);

        EXPECT_EQ(count, 0);
    }

    INSTANTIATE_TEST_SUITE_P(
        Kinds,
        NodeExternalSubclassTest,
        ::testing::Bool());

    class NodeInputThreadingTest : public ::testing::TestWithParam<int> { };

    TEST_P(NodeInputThreadingTest, HooksCanBeAddedAndRemovedWhileInputArrives)
    {
        auto node = make_probe_input();
        std::atomic<bool> stop { false };
        std::atomic<int> fired { 0 };

        std::thread feeder([&] {
            for (int i = 0; !stop.load(); ++i) {
                feed_input(*node, static_cast<double>(i % 2));
            }
        });

        auto& input = static_cast<Nodes::Input::InputNode&>(*node);
        for (int i = 0; i < GetParam(); ++i) {
            const auto tick = input.on_tick([&fired](const NodeContext&) { fired.fetch_add(1); });
            const auto change = input.on_value_change([&fired](const NodeContext&) { fired.fetch_add(1); });
            node->unhook(tick);
            node->unhook(change);
        }

        stop.store(true);
        feeder.join();

        node->remove_all_hooks();
        const int after = fired.load();
        feed_input(*node, 1.0);
        feed_input(*node, 0.0);

        EXPECT_EQ(fired.load(), after);
    }

    INSTANTIATE_TEST_SUITE_P(
        Cycles,
        NodeInputThreadingTest,
        ::testing::Values(10, 500));

}
}
