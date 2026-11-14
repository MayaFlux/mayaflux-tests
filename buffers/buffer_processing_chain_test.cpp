#include "gtest/gtest.h"
#include "../support/test_metadata.hpp"

#include "MayaFlux/Buffers/AudioBuffer.hpp"
#include "MayaFlux/Buffers/BufferProcessingChain.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {

using Buffers::AudioBuffer;
using Buffers::Buffer;
using Buffers::BufferProcessingChain;
using Buffers::BufferProcessor;

namespace {

    using Log = std::shared_ptr<std::vector<std::string>>;

    Log make_log()
    {
        return std::make_shared<std::vector<std::string>>();
    }

    class ChainProbe final : public BufferProcessor {
    public:
        ChainProbe(Log log, std::string name)
            : m_log(std::move(log))
            , m_name(std::move(name))
        {
        }

        void processing_function(const std::shared_ptr<Buffer>&) override
        {
            m_log->push_back(m_name);
            if (hook) {
                hook();
            }
        }

        void on_attach(const std::shared_ptr<Buffer>&) override { ++attached; }
        void on_detach(const std::shared_ptr<Buffer>&) override { ++detached; }

        std::function<void()> hook;
        int attached { 0 };
        int detached { 0 };

    private:
        Log m_log;
        std::string m_name;
    };

    std::shared_ptr<ChainProbe> make_probe(const Log& log, const std::string& name)
    {
        return std::make_shared<ChainProbe>(log, name);
    }

    std::vector<std::string> take(const Log& log)
    {
        auto copy = *log;
        log->clear();
        return copy;
    }

    class OtherProbe final : public BufferProcessor {
    public:
        void processing_function(const std::shared_ptr<Buffer>&) override { }
    };

    constexpr size_t PENDING_CAPACITY = 32;

    constexpr auto NATIVE_TOKEN = Buffers::AUDIO_BACKEND;
    constexpr auto SAME_FAMILY_TOKEN = Buffers::WINDOW_EVENTS;
    constexpr auto FOREIGN_TOKEN = Buffers::GRAPHICS_BACKEND;

    std::shared_ptr<ChainProbe> make_probe(const Log& log, const std::string& name, Buffers::ProcessingToken token)
    {
        auto probe = make_probe(log, name);
        probe->set_processing_token(token);
        return probe;
    }

}

class BufferProcessingChainTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        chain = std::make_shared<BufferProcessingChain>();
        buffer = std::make_shared<AudioBuffer>(0, 64);
        log = make_log();
    }

    [[nodiscard]] bool attached(const std::shared_ptr<BufferProcessor>& processor) const
    {
        const auto& processors = chain->get_processors(buffer);
        return std::ranges::find(processors, processor) != processors.end();
    }

    std::shared_ptr<BufferProcessingChain> chain;
    std::shared_ptr<AudioBuffer> buffer;
    Log log;
};

TEST_F(BufferProcessingChainTest, DirectAddAttachesAndRunsInInsertionOrder)
{
    auto first = make_probe(log, "first");
    auto second = make_probe(log, "second");

    EXPECT_TRUE(chain->add_processor(first, buffer));
    EXPECT_TRUE(chain->add_processor(second, buffer));
    EXPECT_FALSE(chain->has_pending_operations());
    EXPECT_EQ(first->attached, 1);

    chain->process(buffer);

    EXPECT_EQ(take(log), (std::vector<std::string> { "first", "second" }));
}

TEST_F(BufferProcessingChainTest, DuplicateDirectAddIsRejectedWithAReason)
{
    auto probe = make_probe(log, "probe");
    ASSERT_TRUE(chain->add_processor(probe, buffer));

    std::string reason;
    EXPECT_FALSE(chain->add_processor(probe, buffer, &reason));
    EXPECT_FALSE(reason.empty());
    EXPECT_EQ(chain->get_processors(buffer).size(), 1U);
    EXPECT_EQ(probe->attached, 1);
}

TEST_F(BufferProcessingChainTest, DirectRemoveDetachesAndStopsRunning)
{
    auto probe = make_probe(log, "probe");
    ASSERT_TRUE(chain->add_processor(probe, buffer));

    chain->remove_processor(probe, buffer);
    chain->process(buffer);

    EXPECT_FALSE(attached(probe));
    EXPECT_EQ(probe->detached, 1);
    EXPECT_TRUE(take(log).empty());
}

TEST_F(BufferProcessingChainTest, CompleteRunsPreMainPostFinalInOrder)
{
    ASSERT_TRUE(chain->add_processor(make_probe(log, "main"), buffer));
    chain->add_preprocessor(make_probe(log, "pre"), buffer);
    chain->add_postprocessor(make_probe(log, "post"), buffer);
    chain->add_final_processor(make_probe(log, "final"), buffer);

    chain->process_complete(buffer);

    EXPECT_EQ(take(log), (std::vector<std::string> { "pre", "main", "post", "final" }));
}

TEST_F(BufferProcessingChainTest, AddDuringProcessingIsDeferredToTheNextPass)
{
    auto late = make_probe(log, "late");
    auto host = make_probe(log, "host");
    host->hook = [this, late] {
        EXPECT_TRUE(chain->add_processor(late, buffer));
        EXPECT_TRUE(chain->has_pending_operations());
        EXPECT_FALSE(attached(late));
    };
    ASSERT_TRUE(chain->add_processor(host, buffer));

    chain->process(buffer);

    EXPECT_EQ(take(log), (std::vector<std::string> { "host" }));
    EXPECT_TRUE(chain->has_pending_operations());
    EXPECT_EQ(late->attached, 0);

    host->hook = nullptr;
    chain->process(buffer);

    EXPECT_FALSE(chain->has_pending_operations());
    EXPECT_TRUE(attached(late));
    EXPECT_EQ(late->attached, 1);
    EXPECT_EQ(take(log), (std::vector<std::string> { "host", "late" }));
}

TEST_F(BufferProcessingChainTest, RemoveDuringProcessingTakesEffectBeforeTheNextPass)
{
    auto doomed = make_probe(log, "doomed");
    auto host = make_probe(log, "host");
    ASSERT_TRUE(chain->add_processor(host, buffer));
    ASSERT_TRUE(chain->add_processor(doomed, buffer));
    host->hook = [this, doomed] { chain->remove_processor(doomed, buffer); };

    chain->process(buffer);

    EXPECT_EQ(take(log), (std::vector<std::string> { "host", "doomed" }));
    EXPECT_TRUE(chain->has_pending_operations());
    EXPECT_EQ(doomed->detached, 0);

    host->hook = nullptr;
    chain->process(buffer);

    EXPECT_EQ(take(log), (std::vector<std::string> { "host" }));
    EXPECT_FALSE(attached(doomed));
    EXPECT_EQ(doomed->detached, 1);
    EXPECT_FALSE(chain->has_pending_operations());
}

TEST_F(BufferProcessingChainTest, DeferredAddThenRemoveLeavesTheProcessorOut)
{
    auto transient = make_probe(log, "transient");
    auto host = make_probe(log, "host");
    ASSERT_TRUE(chain->add_processor(host, buffer));
    host->hook = [this, transient] {
        EXPECT_TRUE(chain->add_processor(transient, buffer));
        chain->remove_processor(transient, buffer);
    };

    chain->process(buffer);
    host->hook = nullptr;
    chain->process(buffer);

    EXPECT_FALSE(attached(transient));
    EXPECT_EQ(transient->attached, 1);
    EXPECT_EQ(transient->detached, 1);
    EXPECT_EQ(take(log), (std::vector<std::string> { "host", "host" }));
}

TEST_F(BufferProcessingChainTest, DeferredRemoveThenAddLeavesTheProcessorIn)
{
    auto steady = make_probe(log, "steady");
    auto host = make_probe(log, "host");
    ASSERT_TRUE(chain->add_processor(host, buffer));
    ASSERT_TRUE(chain->add_processor(steady, buffer));
    host->hook = [this, steady] {
        chain->remove_processor(steady, buffer);
        EXPECT_TRUE(chain->add_processor(steady, buffer));
    };

    chain->process(buffer);
    host->hook = nullptr;
    take(log);
    chain->process(buffer);

    EXPECT_TRUE(attached(steady));
    EXPECT_EQ(steady->attached, 2);
    EXPECT_EQ(steady->detached, 1);
    EXPECT_EQ(take(log), (std::vector<std::string> { "host", "steady" }));
}

TEST_F(BufferProcessingChainTest, DeferredOperationsApplyInSubmissionOrder)
{
    auto host = make_probe(log, "host");
    std::vector<std::shared_ptr<ChainProbe>> added;
    for (int i = 0; i < 6; ++i) {
        added.push_back(make_probe(log, "p" + std::to_string(i)));
    }
    ASSERT_TRUE(chain->add_processor(host, buffer));
    host->hook = [this, &added] {
        for (const auto& probe : added) {
            EXPECT_TRUE(chain->add_processor(probe, buffer));
        }
    };

    chain->process(buffer);
    host->hook = nullptr;
    take(log);
    chain->process(buffer);

    EXPECT_EQ(take(log), (std::vector<std::string> { "host", "p0", "p1", "p2", "p3", "p4", "p5" }));
}

TEST_F(BufferProcessingChainTest, DuplicateDeferredAddIsIgnoredWhenApplied)
{
    auto twice = make_probe(log, "twice");
    auto host = make_probe(log, "host");
    ASSERT_TRUE(chain->add_processor(host, buffer));
    host->hook = [this, twice] {
        EXPECT_TRUE(chain->add_processor(twice, buffer));
        EXPECT_TRUE(chain->add_processor(twice, buffer));
    };

    chain->process(buffer);
    host->hook = nullptr;
    chain->process(buffer);

    EXPECT_EQ(chain->get_processors(buffer).size(), 2U);
    EXPECT_EQ(twice->attached, 1);
}

TEST_F(BufferProcessingChainTest, RemoveAnywhereDuringProcessingClearsASlotProcessor)
{
    auto pre = make_probe(log, "pre");
    auto host = make_probe(log, "host");
    chain->add_preprocessor(pre, buffer);
    ASSERT_TRUE(chain->add_processor(host, buffer));
    host->hook = [this, pre] { chain->remove_processor_anywhere(pre, buffer); };

    chain->process(buffer);

    EXPECT_EQ(chain->get_preprocessor(buffer), pre);
    EXPECT_TRUE(chain->has_pending_operations());

    host->hook = nullptr;
    chain->process(buffer);

    EXPECT_EQ(chain->get_preprocessor(buffer), nullptr);
    EXPECT_EQ(pre->detached, 1);
    EXPECT_FALSE(chain->has_pending_operations());
}

TEST_F(BufferProcessingChainTest, AddWhileTheProcessorIsMidProcessIsDeferredEvenOutsideTheChain)
{
    auto probe = make_probe(log, "probe");
    probe->hook = [this, &probe] {
        EXPECT_TRUE(chain->add_processor(probe, buffer));
    };

    probe->process(buffer);

    EXPECT_TRUE(chain->has_pending_operations());
    EXPECT_FALSE(attached(probe));

    probe->hook = nullptr;
    take(log);
    chain->process(buffer);

    EXPECT_TRUE(attached(probe));
    EXPECT_EQ(take(log), (std::vector<std::string> { "probe" }));
}

TEST_F(BufferProcessingChainTest, PendingQueueRejectsTheAddPastItsCapacityWithAReason)
{
    auto host = make_probe(log, "host");
    ASSERT_TRUE(chain->add_processor(host, buffer));

    std::vector<std::shared_ptr<ChainProbe>> extras;
    for (size_t i = 0; i < PENDING_CAPACITY + 1; ++i) {
        extras.push_back(make_probe(log, "x" + std::to_string(i)));
    }

    std::vector<bool> accepted;
    std::string reason;
    host->hook = [&] {
        for (size_t i = 0; i < extras.size(); ++i) {
            std::string why;
            accepted.push_back(chain->add_processor(extras[i], buffer, &why));
            if (!accepted.back()) {
                reason = why;
            }
        }
    };

    chain->process(buffer);

    ASSERT_EQ(accepted.size(), PENDING_CAPACITY + 1);
    for (size_t i = 0; i < PENDING_CAPACITY; ++i) {
        EXPECT_TRUE(accepted[i]) << "add " << i;
    }
    EXPECT_FALSE(accepted[PENDING_CAPACITY]);
    EXPECT_EQ(reason, "Processor operation queue full");

    host->hook = nullptr;
    chain->process(buffer);

    EXPECT_EQ(chain->get_processors(buffer).size(), PENDING_CAPACITY + 1);
    EXPECT_FALSE(attached(extras[PENDING_CAPACITY]));
    EXPECT_FALSE(chain->has_pending_operations());
}

TEST_F(BufferProcessingChainTest, PendingQueueIsReusableAfterItDrains)
{
    auto host = make_probe(log, "host");
    ASSERT_TRUE(chain->add_processor(host, buffer));

    for (int round = 0; round < 3; ++round) {
        std::vector<std::shared_ptr<ChainProbe>> batch;
        for (size_t i = 0; i < PENDING_CAPACITY; ++i) {
            batch.push_back(make_probe(log, "r" + std::to_string(round) + "_" + std::to_string(i)));
        }

        size_t accepted = 0;
        host->hook = [&] {
            for (const auto& probe : batch) {
                accepted += chain->add_processor(probe, buffer) ? 1U : 0U;
            }
        };
        chain->process(buffer);
        host->hook = nullptr;
        chain->process(buffer);

        EXPECT_EQ(accepted, PENDING_CAPACITY) << "round " << round;
        EXPECT_FALSE(chain->has_pending_operations());

        for (const auto& probe : batch) {
            chain->remove_processor(probe, buffer);
        }
    }

    EXPECT_EQ(chain->get_processors(buffer).size(), 1U);
}

TEST_F(BufferProcessingChainTest, ProcessingCallInsideAPassDoesNotReenter)
{
    auto inner = make_probe(log, "inner");
    auto host = make_probe(log, "host");
    ASSERT_TRUE(chain->add_processor(inner, buffer));
    ASSERT_TRUE(chain->add_processor(host, buffer));
    host->hook = [this] { chain->process(buffer); };

    chain->process(buffer);

    EXPECT_EQ(take(log), (std::vector<std::string> { "inner", "host" }));
}

TEST_F(BufferProcessingChainTest, DefaultsAndSettersForTokenAndStrategy)
{
    EXPECT_EQ(chain->get_preferred_token(), NATIVE_TOKEN);
    EXPECT_EQ(chain->get_enforcement_strategy(), Buffers::TokenEnforcementStrategy::FILTERED);

    chain->set_preferred_token(FOREIGN_TOKEN);
    chain->set_enforcement_strategy(Buffers::TokenEnforcementStrategy::STRICT);

    EXPECT_EQ(chain->get_preferred_token(), FOREIGN_TOKEN);
    EXPECT_EQ(chain->get_enforcement_strategy(), Buffers::TokenEnforcementStrategy::STRICT);
}

TEST_F(BufferProcessingChainTest, StrictAcceptsOnlyTheExactToken)
{
    chain->set_enforcement_strategy(Buffers::TokenEnforcementStrategy::STRICT);
    auto exact = make_probe(log, "exact", NATIVE_TOKEN);
    auto sibling = make_probe(log, "sibling", SAME_FAMILY_TOKEN);
    auto foreign = make_probe(log, "foreign", FOREIGN_TOKEN);

    std::string sibling_reason;
    std::string foreign_reason;

    EXPECT_TRUE(chain->add_processor(exact, buffer));
    EXPECT_FALSE(chain->add_processor(sibling, buffer, &sibling_reason));
    EXPECT_FALSE(chain->add_processor(foreign, buffer, &foreign_reason));

    EXPECT_NE(sibling_reason.find("STRICT"), std::string::npos);
    EXPECT_NE(foreign_reason.find("STRICT"), std::string::npos);
    EXPECT_EQ(chain->get_processors(buffer).size(), 1U);
    EXPECT_EQ(sibling->attached, 0);
}

TEST_F(BufferProcessingChainTest, FilteredKeepsCompatibleTokensAndRejectsTheRest)
{
    auto sibling = make_probe(log, "sibling", SAME_FAMILY_TOKEN);
    auto foreign = make_probe(log, "foreign", FOREIGN_TOKEN);
    std::string reason;

    EXPECT_TRUE(chain->add_processor(sibling, buffer));
    EXPECT_FALSE(chain->add_processor(foreign, buffer, &reason));

    EXPECT_NE(reason.find("FILTERED"), std::string::npos);
    EXPECT_TRUE(attached(sibling));
    EXPECT_FALSE(attached(foreign));
    EXPECT_EQ(foreign->attached, 0);
}

TEST_F(BufferProcessingChainTest, OverrideSkipKeepsIncompatibleProcessorsButNeverRunsThem)
{
    chain->set_enforcement_strategy(Buffers::TokenEnforcementStrategy::OVERRIDE_SKIP);
    auto native = make_probe(log, "native", NATIVE_TOKEN);
    auto foreign = make_probe(log, "foreign", FOREIGN_TOKEN);

    EXPECT_TRUE(chain->add_processor(native, buffer));
    EXPECT_TRUE(chain->add_processor(foreign, buffer));
    EXPECT_TRUE(attached(foreign));

    chain->process(buffer);
    chain->process(buffer);

    EXPECT_EQ(take(log), (std::vector<std::string> { "native", "native" }));
    EXPECT_TRUE(attached(foreign));
}

TEST_F(BufferProcessingChainTest, OverrideRejectRunsIncompatibleProcessorsOnceThenDropsThem)
{
    chain->set_enforcement_strategy(Buffers::TokenEnforcementStrategy::OVERRIDE_REJECT);
    auto native = make_probe(log, "native", NATIVE_TOKEN);
    auto foreign = make_probe(log, "foreign", FOREIGN_TOKEN);

    EXPECT_TRUE(chain->add_processor(native, buffer));
    EXPECT_TRUE(chain->add_processor(foreign, buffer));

    chain->process(buffer);
    EXPECT_EQ(take(log), (std::vector<std::string> { "native", "foreign" }));
    EXPECT_FALSE(attached(foreign));

    chain->process(buffer);
    EXPECT_EQ(take(log), (std::vector<std::string> { "native" }));
}

TEST_F(BufferProcessingChainTest, IgnoreAcceptsAndRunsEveryToken)
{
    chain->set_enforcement_strategy(Buffers::TokenEnforcementStrategy::IGNORE);

    EXPECT_TRUE(chain->add_processor(make_probe(log, "native", NATIVE_TOKEN), buffer));
    EXPECT_TRUE(chain->add_processor(make_probe(log, "foreign", FOREIGN_TOKEN), buffer));

    chain->process(buffer);

    EXPECT_EQ(take(log), (std::vector<std::string> { "native", "foreign" }));
}

TEST_F(BufferProcessingChainTest, ValidateAllListsEveryIncompatibleProcessor)
{
    chain->set_enforcement_strategy(Buffers::TokenEnforcementStrategy::IGNORE);
    auto foreign = make_probe(log, "foreign", FOREIGN_TOKEN);
    ASSERT_TRUE(chain->add_processor(make_probe(log, "native", NATIVE_TOKEN), buffer));

    EXPECT_TRUE(chain->validate_all_processors());

    ASSERT_TRUE(chain->add_processor(foreign, buffer));
    std::vector<std::string> reasons;

    EXPECT_FALSE(chain->validate_all_processors(&reasons));
    EXPECT_EQ(reasons.size(), 1U);
    EXPECT_FALSE(chain->validate_all_processors());

    chain->remove_processor(foreign, buffer);
    EXPECT_TRUE(chain->validate_all_processors());
}

TEST_F(BufferProcessingChainTest, AnalyzeReportsCompatibilityPerStrategy)
{
    chain->set_enforcement_strategy(Buffers::TokenEnforcementStrategy::OVERRIDE_SKIP);
    auto native = make_probe(log, "native", NATIVE_TOKEN);
    auto foreign = make_probe(log, "foreign", FOREIGN_TOKEN);
    ASSERT_TRUE(chain->add_processor(native, buffer));
    ASSERT_TRUE(chain->add_processor(foreign, buffer));

    auto reports = chain->analyze_token_compatibility();
    ASSERT_EQ(reports.size(), 1U);
    EXPECT_EQ(reports[0].buffer, buffer);
    EXPECT_EQ(reports[0].chain_preferred_token, NATIVE_TOKEN);
    EXPECT_EQ(reports[0].enforcement_strategy, Buffers::TokenEnforcementStrategy::OVERRIDE_SKIP);
    ASSERT_EQ(reports[0].total_processors(), 2U);
    EXPECT_EQ(reports[0].processor_infos[0].processor, native);
    EXPECT_TRUE(reports[0].processor_infos[0].is_compatible);
    EXPECT_EQ(reports[0].processor_infos[1].processor, foreign);
    EXPECT_FALSE(reports[0].processor_infos[1].is_compatible);
    EXPECT_EQ(reports[0].compatible_processors(), 1U);
    EXPECT_EQ(reports[0].skipped_processors(), 1U);
    EXPECT_EQ(reports[0].pending_removal_processors(), 0U);

    chain->set_enforcement_strategy(Buffers::TokenEnforcementStrategy::OVERRIDE_REJECT);
    reports = chain->analyze_token_compatibility();
    EXPECT_EQ(reports[0].skipped_processors(), 0U);
    EXPECT_EQ(reports[0].pending_removal_processors(), 1U);

    chain->set_enforcement_strategy(Buffers::TokenEnforcementStrategy::IGNORE);
    reports = chain->analyze_token_compatibility();
    EXPECT_EQ(reports[0].skipped_processors(), 0U);
    EXPECT_EQ(reports[0].pending_removal_processors(), 0U);
}

TEST_F(BufferProcessingChainTest, EnforceChainTokenRewritesOnlyCompatibleProcessors)
{
    chain->set_enforcement_strategy(Buffers::TokenEnforcementStrategy::IGNORE);
    auto sibling = make_probe(log, "sibling", SAME_FAMILY_TOKEN);
    auto foreign = make_probe(log, "foreign", FOREIGN_TOKEN);
    ASSERT_TRUE(chain->add_processor(sibling, buffer));
    ASSERT_TRUE(chain->add_processor(foreign, buffer));

    chain->enforce_chain_token_on_processors();

    EXPECT_EQ(sibling->get_processing_token(), NATIVE_TOKEN);
    EXPECT_EQ(foreign->get_processing_token(), FOREIGN_TOKEN);
}

TEST_F(BufferProcessingChainTest, OptimizeMovesCompatibleProcessorsAheadUnderPermissiveStrategies)
{
    chain->set_enforcement_strategy(Buffers::TokenEnforcementStrategy::IGNORE);
    ASSERT_TRUE(chain->add_processor(make_probe(log, "f1", FOREIGN_TOKEN), buffer));
    ASSERT_TRUE(chain->add_processor(make_probe(log, "c1", NATIVE_TOKEN), buffer));
    ASSERT_TRUE(chain->add_processor(make_probe(log, "f2", FOREIGN_TOKEN), buffer));
    ASSERT_TRUE(chain->add_processor(make_probe(log, "c2", NATIVE_TOKEN), buffer));

    chain->optimize_for_tokens(buffer);
    chain->process(buffer);

    EXPECT_EQ(take(log), (std::vector<std::string> { "c1", "c2", "f1", "f2" }));
}

TEST_F(BufferProcessingChainTest, OptimizeDropsIncompatibleProcessorsUnderFiltered)
{
    chain->set_enforcement_strategy(Buffers::TokenEnforcementStrategy::IGNORE);
    auto foreign = make_probe(log, "foreign", FOREIGN_TOKEN);
    auto native = make_probe(log, "native", NATIVE_TOKEN);
    ASSERT_TRUE(chain->add_processor(foreign, buffer));
    ASSERT_TRUE(chain->add_processor(native, buffer));

    chain->set_enforcement_strategy(Buffers::TokenEnforcementStrategy::FILTERED);
    chain->optimize_for_tokens(buffer);

    EXPECT_FALSE(attached(foreign));
    EXPECT_TRUE(attached(native));
    EXPECT_EQ(chain->get_processors(buffer).size(), 1U);
}

TEST_F(BufferProcessingChainTest, OptimizeOnAnEmptyBufferChangesNothing)
{
    chain->optimize_for_tokens(buffer);

    EXPECT_FALSE(chain->has_processors(buffer));
}

TEST_F(BufferProcessingChainTest, HasProcessorsAndGetProcessorsTrackTheList)
{
    auto other_buffer = std::make_shared<AudioBuffer>(1, 64);
    auto probe = make_probe(log, "probe");

    EXPECT_FALSE(chain->has_processors(buffer));
    EXPECT_TRUE(chain->get_processors(other_buffer).empty());

    ASSERT_TRUE(chain->add_processor(probe, buffer));

    EXPECT_TRUE(chain->has_processors(buffer));
    EXPECT_FALSE(chain->has_processors(other_buffer));

    chain->remove_processor(probe, buffer);

    EXPECT_FALSE(chain->has_processors(buffer));
}

TEST_F(BufferProcessingChainTest, GetProcessorFindsTheFirstOfAType)
{
    auto other = std::make_shared<OtherProbe>();
    auto first = make_probe(log, "first");
    auto second = make_probe(log, "second");
    ASSERT_TRUE(chain->add_processor(other, buffer));
    ASSERT_TRUE(chain->add_processor(first, buffer));
    ASSERT_TRUE(chain->add_processor(second, buffer));

    EXPECT_EQ(chain->get_processor<ChainProbe>(buffer), first);
    EXPECT_EQ(chain->get_processor<OtherProbe>(buffer), other);

    chain->remove_processor(other, buffer);

    EXPECT_EQ(chain->get_processor<OtherProbe>(buffer), nullptr);
}

TEST_F(BufferProcessingChainTest, BuffersKeepIndependentPipelines)
{
    auto other_buffer = std::make_shared<AudioBuffer>(1, 64);
    ASSERT_TRUE(chain->add_processor(make_probe(log, "left"), buffer));
    ASSERT_TRUE(chain->add_processor(make_probe(log, "right"), other_buffer));

    chain->process(buffer);
    EXPECT_EQ(take(log), (std::vector<std::string> { "left" }));

    chain->process(other_buffer);
    EXPECT_EQ(take(log), (std::vector<std::string> { "right" }));
}

TEST_F(BufferProcessingChainTest, GetChainReturnsASnapshotOfEveryBuffer)
{
    auto other_buffer = std::make_shared<AudioBuffer>(1, 64);
    ASSERT_TRUE(chain->add_processor(make_probe(log, "a"), buffer));
    ASSERT_TRUE(chain->add_processor(make_probe(log, "b"), other_buffer));

    auto snapshot = chain->get_chain();

    ASSERT_EQ(snapshot.size(), 2U);
    EXPECT_EQ(snapshot[buffer].size(), 1U);
    EXPECT_EQ(snapshot[other_buffer].size(), 1U);

    snapshot[buffer].clear();

    EXPECT_EQ(chain->get_processors(buffer).size(), 1U);
}

TEST_F(BufferProcessingChainTest, MergeAppendsMissingProcessorsWithoutDuplicates)
{
    auto other_buffer = std::make_shared<AudioBuffer>(1, 64);
    auto shared_probe = make_probe(log, "shared");
    auto own = make_probe(log, "own");
    auto incoming = make_probe(log, "incoming");
    auto elsewhere = make_probe(log, "elsewhere");

    ASSERT_TRUE(chain->add_processor(shared_probe, buffer));
    ASSERT_TRUE(chain->add_processor(own, buffer));

    auto donor = std::make_shared<BufferProcessingChain>();
    ASSERT_TRUE(donor->add_processor(shared_probe, buffer));
    ASSERT_TRUE(donor->add_processor(incoming, buffer));
    ASSERT_TRUE(donor->add_processor(elsewhere, other_buffer));

    chain->merge_chain(donor);
    chain->process(buffer);
    chain->process(other_buffer);

    EXPECT_EQ(take(log), (std::vector<std::string> { "shared", "own", "incoming", "elsewhere" }));
    EXPECT_EQ(chain->get_chain().size(), 2U);
    EXPECT_EQ(shared_probe->attached, 2);
}

TEST_F(BufferProcessingChainTest, SlotProcessorsReplaceAndRunIndividually)
{
    auto old_pre = make_probe(log, "old_pre");
    auto pre = make_probe(log, "pre");
    auto post = make_probe(log, "post");
    auto final_probe = make_probe(log, "final");

    chain->add_preprocessor(old_pre, buffer);
    chain->add_preprocessor(pre, buffer);
    chain->add_postprocessor(post, buffer);
    chain->add_final_processor(final_probe, buffer);

    EXPECT_EQ(chain->get_preprocessor(buffer), pre);
    EXPECT_EQ(chain->get_postprocessor(buffer), post);
    EXPECT_EQ(chain->get_final_processor(buffer), final_probe);

    chain->preprocess(buffer);
    EXPECT_EQ(take(log), (std::vector<std::string> { "pre" }));
    chain->postprocess(buffer);
    EXPECT_EQ(take(log), (std::vector<std::string> { "post" }));
    chain->process_final(buffer);
    EXPECT_EQ(take(log), (std::vector<std::string> { "final" }));

    chain->process_complete(buffer);
    EXPECT_EQ(take(log), (std::vector<std::string> { "pre", "post", "final" }));
}

TEST_F(BufferProcessingChainTest, SlotGettersAreNullWhenUnset)
{
    EXPECT_EQ(chain->get_preprocessor(buffer), nullptr);
    EXPECT_EQ(chain->get_postprocessor(buffer), nullptr);
    EXPECT_EQ(chain->get_final_processor(buffer), nullptr);

    chain->preprocess(buffer);
    chain->postprocess(buffer);
    chain->process_final(buffer);
    chain->process_complete(buffer);

    EXPECT_TRUE(log->empty());
}

TEST_F(BufferProcessingChainTest, RemoveAnywhereFindsEverySlotKind)
{
    auto regular = make_probe(log, "regular");
    auto pre = make_probe(log, "pre");
    auto post = make_probe(log, "post");
    auto final_probe = make_probe(log, "final");
    ASSERT_TRUE(chain->add_processor(regular, buffer));
    chain->add_preprocessor(pre, buffer);
    chain->add_postprocessor(post, buffer);
    chain->add_final_processor(final_probe, buffer);

    chain->remove_processor_anywhere(regular, buffer);
    chain->remove_processor_anywhere(pre, buffer);
    chain->remove_processor_anywhere(post, buffer);
    chain->remove_processor_anywhere(final_probe, buffer);

    EXPECT_FALSE(chain->has_processors(buffer));
    EXPECT_EQ(chain->get_preprocessor(buffer), nullptr);
    EXPECT_EQ(chain->get_postprocessor(buffer), nullptr);
    EXPECT_EQ(chain->get_final_processor(buffer), nullptr);
    EXPECT_EQ(regular->detached, 1);
    EXPECT_EQ(pre->detached, 1);
    EXPECT_EQ(post->detached, 1);
    EXPECT_EQ(final_probe->detached, 1);
}

TEST_F(BufferProcessingChainTest, RemoveAnywhereLeavesASlotHeldByAnotherProcessor)
{
    auto pre = make_probe(log, "pre");
    auto stranger = make_probe(log, "stranger");
    chain->add_preprocessor(pre, buffer);

    chain->remove_processor_anywhere(stranger, buffer);

    EXPECT_EQ(chain->get_preprocessor(buffer), pre);
    EXPECT_EQ(pre->detached, 0);
    EXPECT_EQ(stranger->detached, 0);
}

TEST_F(BufferProcessingChainTest, RemovingAnAbsentProcessorIsHarmless)
{
    auto never_added = make_probe(log, "never");

    chain->remove_processor(never_added, buffer);
    chain->remove_processor_anywhere(never_added, buffer);

    EXPECT_EQ(never_added->detached, 0);
    EXPECT_FALSE(chain->has_pending_operations());
}

TEST_F(BufferProcessingChainTest, ProcessingABufferWithNoPipelineIsHarmless)
{
    chain->process(buffer);
    chain->process_complete(buffer);

    EXPECT_FALSE(chain->has_processors(buffer));
}

}
