#include "gtest/gtest.h"

#include "MayaFlux/Kakshya/DataProcessingChain.hpp"

#include "../mock_signalsourcecontainer.hpp"

namespace MayaFlux::Test {

using Kakshya::DataProcessingChain;
using Kakshya::DataProcessor;
using Kakshya::SignalSourceContainer;

namespace {

    using Log = std::shared_ptr<std::vector<std::string>>;

    Log make_log()
    {
        return std::make_shared<std::vector<std::string>>();
    }

    std::vector<std::string> take(const Log& log)
    {
        auto copy = *log;
        log->clear();
        return copy;
    }

    class ChainDataProbe : public DataProcessor {
    public:
        ChainDataProbe(Log log, std::string name)
            : m_log(std::move(log))
            , m_name(std::move(name))
        {
        }

        void on_attach(const std::shared_ptr<SignalSourceContainer>&) override { ++attached; }
        void on_detach(const std::shared_ptr<SignalSourceContainer>&) override { ++detached; }

        void process(const std::shared_ptr<SignalSourceContainer>&) override
        {
            m_log->push_back(m_name);
            if (hook) {
                hook();
            }
        }

        [[nodiscard]] bool is_processing() const override { return false; }

        std::function<void()> hook;
        int attached { 0 };
        int detached { 0 };

    private:
        Log m_log;
        std::string m_name;
    };

    class OtherDataProbe final : public ChainDataProbe {
    public:
        using ChainDataProbe::ChainDataProbe;
    };

    std::shared_ptr<ChainDataProbe> make_probe(const Log& log, const std::string& name)
    {
        return std::make_shared<ChainDataProbe>(log, name);
    }

    std::shared_ptr<OtherDataProbe> make_other(const Log& log, const std::string& name)
    {
        return std::make_shared<OtherDataProbe>(log, name);
    }

}

class DataProcessingChainTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        chain = std::make_shared<DataProcessingChain>();
        container = std::make_shared<MockSignalSourceContainer>();
        log = make_log();
    }

    std::shared_ptr<DataProcessingChain> chain;
    std::shared_ptr<MockSignalSourceContainer> container;
    Log log;
};

TEST_F(DataProcessingChainTest, ProcessRunsProcessorsInInsertionOrderAndAttaches)
{
    auto first = make_probe(log, "first");
    auto second = make_probe(log, "second");

    chain->add_processor(first, container);
    chain->add_processor(second, container);

    EXPECT_EQ(first->attached, 1);
    EXPECT_EQ(second->attached, 1);

    chain->process(container);

    EXPECT_EQ(take(log), (std::vector<std::string> { "first", "second" }));
}

TEST_F(DataProcessingChainTest, AddAtInsertsAtThePositionAndAppendsWhenOutOfRange)
{
    chain->add_processor(make_probe(log, "a"), container);
    chain->add_processor(make_probe(log, "b"), container);

    chain->add_processor_at(make_probe(log, "front"), container, 0);
    chain->add_processor_at(make_probe(log, "middle"), container, 2);
    chain->add_processor_at(make_probe(log, "far"), container, 99);

    chain->process(container);

    EXPECT_EQ(take(log), (std::vector<std::string> { "front", "a", "middle", "b", "far" }));
}

TEST_F(DataProcessingChainTest, AddAtAttachesTheProcessor)
{
    auto probe = make_probe(log, "probe");

    chain->add_processor_at(probe, container, 0);

    EXPECT_EQ(probe->attached, 1);
}

TEST_F(DataProcessingChainTest, ProcessingAnUnknownContainerIsHarmless)
{
    chain->process(container);
    chain->process_tagged(container, "tag");
    chain->process_filtered(container, [](const auto&) { return true; });
    chain->process_typed<ChainDataProbe>(container);

    EXPECT_TRUE(log->empty());
}

TEST_F(DataProcessingChainTest, ContainersKeepIndependentPipelines)
{
    auto other_container = std::make_shared<MockSignalSourceContainer>();
    chain->add_processor(make_probe(log, "left"), container);
    chain->add_processor(make_probe(log, "right"), other_container);

    chain->process(container);
    EXPECT_EQ(take(log), (std::vector<std::string> { "left" }));

    chain->process(other_container);
    EXPECT_EQ(take(log), (std::vector<std::string> { "right" }));
}

TEST_F(DataProcessingChainTest, DuplicateAddRunsTwiceAndRemoveTakesOneEntry)
{
    auto probe = make_probe(log, "probe");
    chain->add_processor(probe, container);
    chain->add_processor(probe, container);

    chain->process(container);
    EXPECT_EQ(take(log), (std::vector<std::string> { "probe", "probe" }));

    chain->remove_processor(probe, container);
    chain->process(container);

    EXPECT_EQ(take(log), (std::vector<std::string> { "probe" }));
    EXPECT_EQ(probe->detached, 1);
}

TEST_F(DataProcessingChainTest, RemoveDetachesAndStopsRunning)
{
    auto kept = make_probe(log, "kept");
    auto doomed = make_probe(log, "doomed");
    chain->add_processor(kept, container);
    chain->add_processor(doomed, container);

    chain->remove_processor(doomed, container);
    chain->process(container);

    EXPECT_EQ(take(log), (std::vector<std::string> { "kept" }));
    EXPECT_EQ(doomed->detached, 1);
    EXPECT_EQ(kept->detached, 0);
}

TEST_F(DataProcessingChainTest, RemovingAnAbsentProcessorOrFromTheWrongContainerIsHarmless)
{
    auto other_container = std::make_shared<MockSignalSourceContainer>();
    auto resident = make_probe(log, "resident");
    auto stranger = make_probe(log, "stranger");
    chain->add_processor(resident, container);

    chain->remove_processor(stranger, container);
    chain->remove_processor(resident, other_container);
    chain->process(container);

    EXPECT_EQ(take(log), (std::vector<std::string> { "resident" }));
    EXPECT_EQ(resident->detached, 0);
    EXPECT_EQ(stranger->detached, 0);
}

TEST_F(DataProcessingChainTest, ContainerCanBeRefilledAfterItsLastProcessorLeaves)
{
    auto first = make_probe(log, "first");
    chain->add_processor(first, container);
    chain->remove_processor(first, container);
    chain->process(container);
    EXPECT_TRUE(log->empty());

    chain->add_processor(make_probe(log, "second"), container);
    chain->process(container);

    EXPECT_EQ(take(log), (std::vector<std::string> { "second" }));
}

TEST_F(DataProcessingChainTest, TaggedProcessingRunsOnlyMatchingProcessors)
{
    chain->add_processor(make_probe(log, "tagged_a"), container, "a");
    chain->add_processor(make_probe(log, "tagged_b"), container, "b");
    chain->add_processor(make_probe(log, "untagged"), container);
    chain->add_processor(make_probe(log, "tagged_a2"), container, "a");

    chain->process_tagged(container, "a");
    EXPECT_EQ(take(log), (std::vector<std::string> { "tagged_a", "tagged_a2" }));

    chain->process_tagged(container, "missing");
    chain->process_tagged(container, "");
    EXPECT_TRUE(log->empty());
}

TEST_F(DataProcessingChainTest, TagIsForgottenWhenTheProcessorIsRemoved)
{
    auto probe = make_probe(log, "probe");
    chain->add_processor(probe, container, "a");
    chain->remove_processor(probe, container);

    chain->add_processor(probe, container);
    chain->process_tagged(container, "a");

    EXPECT_TRUE(log->empty());
}

TEST_F(DataProcessingChainTest, FilteredProcessingRunsOnlyWhatThePredicateAccepts)
{
    auto keep = make_probe(log, "keep");
    chain->add_processor(keep, container);
    chain->add_processor(make_probe(log, "skip"), container);
    chain->add_processor(make_probe(log, "keep_too"), container);

    chain->process_filtered(container, [](const std::shared_ptr<DataProcessor>&) { return true; });
    EXPECT_EQ(take(log), (std::vector<std::string> { "keep", "skip", "keep_too" }));

    chain->process_filtered(container, [&](const std::shared_ptr<DataProcessor>& processor) {
        return processor == keep;
    });
    EXPECT_EQ(take(log), (std::vector<std::string> { "keep" }));

    chain->process_filtered(container, [](const std::shared_ptr<DataProcessor>&) { return false; });
    EXPECT_TRUE(log->empty());
}

TEST_F(DataProcessingChainTest, TypedProcessingRunsOnlyThatDerivedType)
{
    chain->add_processor(make_probe(log, "base_a"), container);
    chain->add_processor(make_other(log, "other"), container);
    chain->add_processor(make_probe(log, "base_b"), container);

    chain->process_typed<OtherDataProbe>(container);
    EXPECT_EQ(take(log), (std::vector<std::string> { "other" }));

    chain->process_typed<ChainDataProbe>(container);
    EXPECT_EQ(take(log), (std::vector<std::string> { "base_a", "other", "base_b" }));
}

TEST_F(DataProcessingChainTest, RemovalDuringAPassTakesEffectWhenThePassFinishes)
{
    auto host = make_probe(log, "host");
    auto doomed = make_probe(log, "doomed");
    chain->add_processor(host, container);
    chain->add_processor(doomed, container);
    host->hook = [this, doomed] {
        chain->remove_processor(doomed, container);
        EXPECT_EQ(doomed->detached, 0);
    };

    chain->process(container);

    EXPECT_EQ(take(log), (std::vector<std::string> { "host", "doomed" }));
    EXPECT_EQ(doomed->detached, 1);

    host->hook = nullptr;
    chain->process(container);

    EXPECT_EQ(take(log), (std::vector<std::string> { "host" }));
}

TEST_F(DataProcessingChainTest, AProcessorMayRemoveItselfDuringItsOwnPass)
{
    auto host = make_probe(log, "host");
    chain->add_processor(host, container);
    host->hook = [this, &host] { chain->remove_processor(host, container); };

    chain->process(container);
    chain->process(container);

    EXPECT_EQ(take(log), (std::vector<std::string> { "host" }));
    EXPECT_EQ(host->detached, 1);
}

TEST_F(DataProcessingChainTest, SeveralRemovalsDuringAPassAreAllApplied)
{
    auto host = make_probe(log, "host");
    auto b = make_probe(log, "b");
    auto c = make_probe(log, "c");
    auto d = make_probe(log, "d");
    for (const auto& probe : { host, b, c, d }) {
        chain->add_processor(probe, container);
    }
    host->hook = [this, b, c] {
        chain->remove_processor(b, container);
        chain->remove_processor(c, container);
    };

    chain->process(container);
    take(log);
    host->hook = nullptr;
    chain->process(container);

    EXPECT_EQ(take(log), (std::vector<std::string> { "host", "d" }));
    EXPECT_EQ(b->detached, 1);
    EXPECT_EQ(c->detached, 1);
    EXPECT_EQ(d->detached, 0);
}

TEST_F(DataProcessingChainTest, DeferredRemovalOfAnAbsentProcessorIsHarmless)
{
    auto host = make_probe(log, "host");
    auto stranger = make_probe(log, "stranger");
    chain->add_processor(host, container);
    host->hook = [this, stranger] { chain->remove_processor(stranger, container); };

    chain->process(container);

    EXPECT_EQ(stranger->detached, 0);
    EXPECT_EQ(take(log), (std::vector<std::string> { "host" }));
}

TEST_F(DataProcessingChainTest, ProcessingInsideAPassDoesNotReenter)
{
    auto host = make_probe(log, "host");
    chain->add_processor(host, container);
    host->hook = [this] {
        chain->process(container);
        chain->process_tagged(container, "x");
        chain->process_typed<ChainDataProbe>(container);
    };

    chain->process(container);

    EXPECT_EQ(take(log), (std::vector<std::string> { "host" }));
}

TEST_F(DataProcessingChainTest, TaggedFilteredAndTypedPassesAlsoFlushDeferredRemovals)
{
    auto host = make_probe(log, "host");
    auto tagged_victim = make_probe(log, "tagged_victim");
    auto filtered_victim = make_probe(log, "filtered_victim");
    auto typed_victim = make_probe(log, "typed_victim");
    chain->add_processor(host, container, "run");
    chain->add_processor(tagged_victim, container);
    chain->add_processor(filtered_victim, container);
    chain->add_processor(typed_victim, container);

    host->hook = [this, tagged_victim] { chain->remove_processor(tagged_victim, container); };
    chain->process_tagged(container, "run");
    EXPECT_EQ(tagged_victim->detached, 1);

    host->hook = [this, filtered_victim] { chain->remove_processor(filtered_victim, container); };
    chain->process_filtered(container, [&](const std::shared_ptr<DataProcessor>& processor) {
        return processor == host;
    });
    EXPECT_EQ(filtered_victim->detached, 1);

    host->hook = [this, typed_victim] { chain->remove_processor(typed_victim, container); };
    chain->process_typed<ChainDataProbe>(container);
    EXPECT_EQ(typed_victim->detached, 1);
}

}
