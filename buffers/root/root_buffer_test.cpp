#include "gtest/gtest.h"

#include "MayaFlux/Buffers/AudioBuffer.hpp"
#include "MayaFlux/Buffers/BufferProcessingChain.hpp"
#include "MayaFlux/Buffers/Root/RootAudioBuffer.hpp"

namespace MayaFlux::Test {

using Buffers::AudioBuffer;
using Buffers::BufferProcessor;
using Buffers::RootAudioBuffer;
using Buffers::TokenEnforcementStrategy;

namespace {

    class RootProbe final : public BufferProcessor {
    public:
        explicit RootProbe(Buffers::ProcessingToken token = Buffers::AUDIO_BACKEND)
        {
            m_processing_token = token;
        }

        void processing_function(const std::shared_ptr<Buffers::Buffer>&) override
        {
            ++runs;
            if (hook) {
                hook();
            }
        }

        std::function<void()> hook;
        int runs { 0 };
    };

    class ConfigurableRoot final : public RootAudioBuffer {
    public:
        using RootAudioBuffer::RootAudioBuffer;

        void configure(TokenEnforcementStrategy strategy, Buffers::ProcessingToken token)
        {
            m_token_enforcement_strategy = strategy;
            m_preferred_processing_token = token;
        }
    };

    constexpr size_t PENDING_CAPACITY = 64;

    constexpr auto NATIVE_TOKEN = Buffers::AUDIO_BACKEND;
    constexpr auto SAME_FAMILY_TOKEN = Buffers::WINDOW_EVENTS;
    constexpr auto FOREIGN_TOKEN = Buffers::GRAPHICS_BACKEND;

    std::shared_ptr<AudioBuffer> make_child()
    {
        return std::make_shared<AudioBuffer>();
    }

    std::shared_ptr<AudioBuffer> make_child(Buffers::ProcessingToken token)
    {
        auto child = make_child();
        child->set_default_processor(std::make_shared<RootProbe>(token));
        return child;
    }

}

class RootBufferTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        root = std::make_shared<ConfigurableRoot>(0);
        root->initialize();
    }

    void make_busy() { ASSERT_TRUE(root->try_acquire_processing()); }
    void make_idle() { root->release_processing(); }

    std::shared_ptr<ConfigurableRoot> root;
};

TEST_F(RootBufferTest, StartsEmptyWithNothingPending)
{
    EXPECT_EQ(root->get_num_children(), 0U);
    EXPECT_TRUE(root->get_child_buffers().empty());
    EXPECT_FALSE(root->has_pending_operations());
}

TEST_F(RootBufferTest, DirectAddAppendsInOrder)
{
    auto first = make_child();
    auto second = make_child();

    root->add_child_buffer(first);
    root->add_child_buffer(second);

    EXPECT_EQ(root->get_num_children(), 2U);
    EXPECT_EQ(root->get_child_buffers(), (std::vector<std::shared_ptr<AudioBuffer>> { first, second }));
    EXPECT_FALSE(root->has_pending_operations());
}

TEST_F(RootBufferTest, DirectAddGivesAChainlessChildTheRootChain)
{
    auto child = make_child();
    child->set_processing_chain(nullptr, true);
    ASSERT_EQ(child->get_processing_chain(), nullptr);

    root->add_child_buffer(child);

    EXPECT_EQ(child->get_processing_chain(), root->get_processing_chain());
}

TEST_F(RootBufferTest, DirectAddKeepsAChildsOwnChain)
{
    auto child = make_child();
    auto own_chain = child->get_processing_chain();
    ASSERT_NE(own_chain, nullptr);

    root->add_child_buffer(child);

    EXPECT_EQ(child->get_processing_chain(), own_chain);
}

TEST_F(RootBufferTest, DirectAddOfAnUnacceptableChildThrowsAndAddsNothing)
{
    auto foreign = make_child(FOREIGN_TOKEN);

    EXPECT_THROW(root->add_child_buffer(foreign), std::runtime_error);
    EXPECT_EQ(root->get_num_children(), 0U);
}

TEST_F(RootBufferTest, TryAddReturnsFalseWithAReasonForAnUnacceptableChild)
{
    auto foreign = make_child(FOREIGN_TOKEN);
    auto native = make_child(NATIVE_TOKEN);
    std::string reason;

    EXPECT_FALSE(root->try_add_child_buffer(foreign, &reason));
    EXPECT_NE(reason.find("STRICT"), std::string::npos);
    EXPECT_TRUE(root->try_add_child_buffer(native));
    EXPECT_EQ(root->get_child_buffers(), (std::vector<std::shared_ptr<AudioBuffer>> { native }));
}

TEST_F(RootBufferTest, DirectRemoveTakesTheChildOut)
{
    auto kept = make_child();
    auto doomed = make_child();
    root->add_child_buffer(kept);
    root->add_child_buffer(doomed);

    root->remove_child_buffer(doomed);

    EXPECT_EQ(root->get_child_buffers(), (std::vector<std::shared_ptr<AudioBuffer>> { kept }));
}

TEST_F(RootBufferTest, RemovingAnAbsentChildIsHarmless)
{
    auto kept = make_child();
    root->add_child_buffer(kept);

    root->remove_child_buffer(make_child());

    EXPECT_EQ(root->get_num_children(), 1U);
    EXPECT_FALSE(root->has_pending_operations());
}

TEST_F(RootBufferTest, AddingTheSameChildTwiceKeepsBothEntriesAndRemoveTakesOne)
{
    auto child = make_child();
    root->add_child_buffer(child);
    root->add_child_buffer(child);

    EXPECT_EQ(root->get_num_children(), 2U);

    root->remove_child_buffer(child);

    EXPECT_EQ(root->get_num_children(), 1U);
}

TEST_F(RootBufferTest, ClearZeroesTheRootAndEveryChild)
{
    auto child = make_child();
    std::ranges::fill(child->get_data(), 0.5);
    std::ranges::fill(root->get_data(), 0.25);
    root->add_child_buffer(child);

    root->clear();

    EXPECT_TRUE(std::ranges::all_of(root->get_data(), [](double v) { return v == 0.0; }));
    EXPECT_TRUE(std::ranges::all_of(child->get_data(), [](double v) { return v == 0.0; }));
}

TEST_F(RootBufferTest, RateHintCrossModalFlagAndTokenActivityRoundTrip)
{
    root->set_processing_rate_hint(60);
    root->enable_cross_modal_sharing(true);
    root->set_token_active(true);

    EXPECT_EQ(root->get_processing_rate_hint(), 60U);
    EXPECT_TRUE(root->is_cross_modal_sharing_enabled());
    EXPECT_TRUE(root->is_token_active());

    root->enable_cross_modal_sharing(false);
    root->set_token_active(false);

    EXPECT_FALSE(root->is_cross_modal_sharing_enabled());
    EXPECT_FALSE(root->is_token_active());
}

TEST_F(RootBufferTest, AChildWithoutADefaultProcessorIsAlwaysAcceptable)
{
    std::string reason;

    EXPECT_TRUE(root->is_buffer_acceptable(make_child(), &reason));
    EXPECT_TRUE(reason.empty());
}

TEST_F(RootBufferTest, StrictRequiresTheExactToken)
{
    root->configure(TokenEnforcementStrategy::STRICT, NATIVE_TOKEN);
    std::string reason;

    EXPECT_TRUE(root->is_buffer_acceptable(make_child(NATIVE_TOKEN)));
    EXPECT_FALSE(root->is_buffer_acceptable(make_child(SAME_FAMILY_TOKEN), &reason));
    EXPECT_NE(reason.find("STRICT"), std::string::npos);
    EXPECT_FALSE(root->is_buffer_acceptable(make_child(FOREIGN_TOKEN)));
}

TEST_F(RootBufferTest, FilteredAcceptsCompatibleTokensOnly)
{
    root->configure(TokenEnforcementStrategy::FILTERED, NATIVE_TOKEN);
    std::string reason;

    EXPECT_TRUE(root->is_buffer_acceptable(make_child(NATIVE_TOKEN)));
    EXPECT_TRUE(root->is_buffer_acceptable(make_child(SAME_FAMILY_TOKEN)));
    EXPECT_FALSE(root->is_buffer_acceptable(make_child(FOREIGN_TOKEN), &reason));
    EXPECT_NE(reason.find("FILTERED"), std::string::npos);
}

TEST_F(RootBufferTest, OverrideSkipAcceptsEverythingAndExplainsTheIncompatibleOne)
{
    root->configure(TokenEnforcementStrategy::OVERRIDE_SKIP, NATIVE_TOKEN);
    std::string compatible_reason;
    std::string foreign_reason;

    EXPECT_TRUE(root->is_buffer_acceptable(make_child(NATIVE_TOKEN), &compatible_reason));
    EXPECT_TRUE(compatible_reason.empty());
    EXPECT_TRUE(root->is_buffer_acceptable(make_child(FOREIGN_TOKEN), &foreign_reason));
    EXPECT_NE(foreign_reason.find("OVERRIDE_SKIP"), std::string::npos);
}

TEST_F(RootBufferTest, OverrideRejectAcceptsEverythingAndExplainsTheIncompatibleOne)
{
    root->configure(TokenEnforcementStrategy::OVERRIDE_REJECT, NATIVE_TOKEN);
    std::string reason;

    EXPECT_TRUE(root->is_buffer_acceptable(make_child(FOREIGN_TOKEN), &reason));
    EXPECT_NE(reason.find("OVERRIDE_REJECT"), std::string::npos);
}

TEST_F(RootBufferTest, IgnoreAcceptsEverythingWithoutAReason)
{
    root->configure(TokenEnforcementStrategy::IGNORE, NATIVE_TOKEN);
    std::string reason;

    EXPECT_TRUE(root->is_buffer_acceptable(make_child(FOREIGN_TOKEN), &reason));
    EXPECT_TRUE(reason.empty());
}

TEST_F(RootBufferTest, AddWhileProcessingIsDeferredUntilTheNextProcessDefault)
{
    auto child = make_child();

    make_busy();
    root->add_child_buffer(child);

    EXPECT_EQ(root->get_num_children(), 0U);
    EXPECT_TRUE(root->has_pending_operations());

    make_idle();
    root->process_default();

    EXPECT_EQ(root->get_child_buffers(), (std::vector<std::shared_ptr<AudioBuffer>> { child }));
    EXPECT_FALSE(root->has_pending_operations());
}

TEST_F(RootBufferTest, RemoveWhileProcessingIsDeferredUntilTheNextProcessDefault)
{
    auto child = make_child();
    root->add_child_buffer(child);

    make_busy();
    root->remove_child_buffer(child);

    EXPECT_EQ(root->get_num_children(), 1U);
    EXPECT_TRUE(root->has_pending_operations());

    make_idle();
    root->process_default();

    EXPECT_EQ(root->get_num_children(), 0U);
    EXPECT_FALSE(root->has_pending_operations());
}

TEST_F(RootBufferTest, TryAddWhileProcessingQueuesAnAcceptableChildAndRefusesAnUnacceptableOne)
{
    auto native = make_child(NATIVE_TOKEN);
    auto foreign = make_child(FOREIGN_TOKEN);

    make_busy();
    EXPECT_TRUE(root->try_add_child_buffer(native));
    EXPECT_FALSE(root->try_add_child_buffer(foreign));
    make_idle();
    root->process_default();

    EXPECT_EQ(root->get_child_buffers(), (std::vector<std::shared_ptr<AudioBuffer>> { native }));
}

TEST_F(RootBufferTest, DeferredOperationsApplyInSubmissionOrder)
{
    std::vector<std::shared_ptr<AudioBuffer>> children;
    for (int i = 0; i < 6; ++i) {
        children.push_back(make_child());
    }

    make_busy();
    for (const auto& child : children) {
        root->add_child_buffer(child);
    }
    make_idle();
    root->process_default();

    EXPECT_EQ(root->get_child_buffers(), children);
}

TEST_F(RootBufferTest, DeferredAddThenRemoveLeavesTheChildOut)
{
    auto child = make_child();

    make_busy();
    root->add_child_buffer(child);
    root->remove_child_buffer(child);
    make_idle();
    root->process_default();

    EXPECT_EQ(root->get_num_children(), 0U);
}

TEST_F(RootBufferTest, DeferredRemoveThenAddLeavesTheChildInOnce)
{
    auto child = make_child();
    root->add_child_buffer(child);

    make_busy();
    root->remove_child_buffer(child);
    root->add_child_buffer(child);
    make_idle();
    root->process_default();

    EXPECT_EQ(root->get_child_buffers(), (std::vector<std::shared_ptr<AudioBuffer>> { child }));
}

TEST_F(RootBufferTest, AddMadeInsideTheDefaultProcessorTakesEffectOnTheNextPass)
{
    auto host = std::make_shared<RootProbe>();
    auto late = make_child();
    root->set_default_processor(host);
    host->hook = [this, late] {
        root->add_child_buffer(late);
        EXPECT_EQ(root->get_num_children(), 0U);
    };

    root->process_default();

    EXPECT_EQ(host->runs, 1);
    EXPECT_EQ(root->get_num_children(), 0U);
    EXPECT_TRUE(root->has_pending_operations());

    host->hook = nullptr;
    root->process_default();

    EXPECT_EQ(root->get_child_buffers(), (std::vector<std::shared_ptr<AudioBuffer>> { late }));
    EXPECT_EQ(host->runs, 2);
    EXPECT_FALSE(root->has_pending_operations());
}

TEST_F(RootBufferTest, ChildMarkedForRemovalLeavesOnTheCycleAfterTheChannelProcessorSeesIt)
{
    auto child = make_child();
    root->add_child_buffer(child);
    child->mark_for_removal();

    root->process_default();

    EXPECT_EQ(root->get_num_children(), 1U);
    EXPECT_TRUE(root->has_pending_operations());

    root->process_default();

    EXPECT_EQ(root->get_num_children(), 0U);
    EXPECT_FALSE(root->has_pending_operations());
}

TEST_F(RootBufferTest, PendingQueueTakesItsCapacityAndSilentlyDropsTheRest)
{
    std::vector<std::shared_ptr<AudioBuffer>> children;
    for (size_t i = 0; i < PENDING_CAPACITY + 1; ++i) {
        children.push_back(make_child());
    }

    make_busy();
    for (const auto& child : children) {
        root->add_child_buffer(child);
    }
    make_idle();
    root->process_default();

    ASSERT_EQ(root->get_num_children(), PENDING_CAPACITY);
    for (size_t i = 0; i < PENDING_CAPACITY; ++i) {
        EXPECT_EQ(root->get_child_buffers()[i], children[i]) << "child " << i;
    }
    EXPECT_FALSE(root->has_pending_operations());
}

TEST_F(RootBufferTest, PendingQueueIsReusableAfterItDrains)
{
    for (int round = 0; round < 3; ++round) {
        std::vector<std::shared_ptr<AudioBuffer>> batch;
        for (size_t i = 0; i < PENDING_CAPACITY; ++i) {
            batch.push_back(make_child());
        }

        make_busy();
        for (const auto& child : batch) {
            root->add_child_buffer(child);
        }
        make_idle();
        root->process_default();

        EXPECT_EQ(root->get_child_buffers(), batch) << "round " << round;
        EXPECT_FALSE(root->has_pending_operations());

        for (const auto& child : batch) {
            root->remove_child_buffer(child);
        }
        EXPECT_EQ(root->get_num_children(), 0U);
    }
}

}
