#include "MayaFlux/Transitive/Parallel/AsyncGroup.hpp"
#include "MayaFlux/Yantra/ComputeMatrix.hpp"

#include <gtest/gtest.h>

namespace MayaFlux::Test {
namespace {

    using Parallel::AsyncGroup;

    std::future<void> ready_future()
    {
        std::promise<void> promise;
        promise.set_value();
        return promise.get_future();
    }

    std::future<void> failed_future(const std::string& message)
    {
        std::promise<void> promise;
        promise.set_exception(std::make_exception_ptr(std::runtime_error(message)));
        return promise.get_future();
    }

    std::string message_of(const std::exception_ptr& ep)
    {
        try {
            std::rethrow_exception(ep);
        } catch (const std::exception& e) {
            return e.what();
        } catch (...) {
            return "non-std exception";
        }
    }

    TEST(AsyncGroupDrainTest, DrainBlocksUntilEveryTaskHasRun)
    {
        AsyncGroup<void> group;
        std::atomic<int> completed { 0 };

        for (int i = 0; i < 8; ++i) {
            group.submit(std::async(std::launch::async, [&completed] {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                completed.fetch_add(1);
            }));
        }

        group.drain();

        EXPECT_EQ(completed.load(), 8);
        EXPECT_EQ(group.approx_pending(), 0U);
    }

    TEST(AsyncGroupDrainTest, DrainOnAnEmptyGroupReturnsAtOnce)
    {
        AsyncGroup<void> group;

        group.drain();

        EXPECT_EQ(group.approx_pending(), 0U);
        EXPECT_TRUE(group.take_errors().empty());
    }

    TEST(AsyncGroupDrainTest, RepeatedDrainAfterCompletionIsHarmless)
    {
        AsyncGroup<int> group;
        group.submit(std::async(std::launch::async, [] { return 1; }));

        group.drain();
        group.drain();

        EXPECT_EQ(group.approx_pending(), 0U);
        EXPECT_TRUE(group.take_errors().empty());
    }

    TEST(AsyncGroupDrainTest, DestructorDrainsWorkStillInFlight)
    {
        std::atomic<bool> finished { false };

        {
            AsyncGroup<void> group;
            group.submit(std::async(std::launch::async, [&finished] {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                finished.store(true);
            }));
        }

        EXPECT_TRUE(finished.load());
    }

    TEST(AsyncGroupErrorTest, FreshGroupHoldsNoErrors)
    {
        AsyncGroup<bool> group;

        EXPECT_TRUE(group.take_errors().empty());
    }

    TEST(AsyncGroupErrorTest, SuccessfulTasksLeaveNoErrors)
    {
        AsyncGroup<bool> group;
        group.submit(std::async(std::launch::async, [] { return true; }));
        group.submit(std::async(std::launch::async, [] { return false; }));

        group.drain();

        EXPECT_TRUE(group.take_errors().empty());
    }

    TEST(AsyncGroupErrorTest, UnhandledExceptionIsRetainedWithItsMessage)
    {
        AsyncGroup<bool> group;
        group.submit(std::async(std::launch::async, []() -> bool {
            throw std::runtime_error("deliberate failure");
        }));

        group.drain();
        const auto errors = group.take_errors();

        ASSERT_EQ(errors.size(), 1U);
        EXPECT_EQ(message_of(errors.front()), "deliberate failure");
    }

    TEST(AsyncGroupErrorTest, TakeErrorsReturnsAndClearsTheRetainedList)
    {
        AsyncGroup<bool> group;
        group.submit(std::async(std::launch::async, []() -> bool {
            throw std::runtime_error("once");
        }));
        group.drain();

        EXPECT_EQ(group.take_errors().size(), 1U);
        EXPECT_TRUE(group.take_errors().empty());
    }

    TEST(AsyncGroupErrorTest, EveryFailingTaskIsRetained)
    {
        AsyncGroup<bool> group;
        for (int i = 0; i < 3; ++i) {
            group.submit(std::async(std::launch::async, []() -> bool {
                throw std::runtime_error("failure");
            }));
        }

        group.drain();

        EXPECT_EQ(group.take_errors().size(), 3U);
    }

    TEST(AsyncGroupErrorTest, NonStandardExceptionTypesArePreserved)
    {
        AsyncGroup<void> group;
        group.submit(std::async(std::launch::async, [] { throw 42; }));

        group.drain();
        const auto errors = group.take_errors();

        ASSERT_EQ(errors.size(), 1U);
        int thrown = 0;
        try {
            std::rethrow_exception(errors.front());
        } catch (int value) {
            thrown = value;
        }
        EXPECT_EQ(thrown, 42);
    }

    TEST(AsyncGroupHandlerTest, HandlerReceivesEachFailureOnceAndNothingAccumulates)
    {
        std::atomic<int> calls { 0 };
        std::vector<std::string> messages;
        std::mutex messages_mutex;

        AsyncGroup<bool> group([&](std::exception_ptr ep) {
            calls.fetch_add(1);
            std::lock_guard lock(messages_mutex);
            messages.push_back(message_of(ep));
        });

        for (int i = 0; i < 3; ++i) {
            group.submit(std::async(std::launch::async, []() -> bool {
                throw std::runtime_error("routed");
            }));
        }

        group.drain();

        EXPECT_EQ(calls.load(), 3);
        EXPECT_EQ(messages, (std::vector<std::string> { "routed", "routed", "routed" }));
        EXPECT_TRUE(group.take_errors().empty());
    }

    TEST(AsyncGroupHandlerTest, SuccessfulTasksNeverInvokeTheHandler)
    {
        std::atomic<int> calls { 0 };
        AsyncGroup<bool> group([&calls](std::exception_ptr) { calls.fetch_add(1); });

        group.submit(std::async(std::launch::async, [] { return true; }));
        group.drain();

        EXPECT_EQ(calls.load(), 0);
    }

    TEST(AsyncGroupHandlerTest, ReadyFailureIsReportedByTheSubmitThatSweepsIt)
    {
        std::atomic<int> calls { 0 };
        AsyncGroup<void> group([&calls](std::exception_ptr) { calls.fetch_add(1); });

        group.submit(failed_future("swept"));

        EXPECT_EQ(calls.load(), 1);
        EXPECT_EQ(group.approx_pending(), 0U);
    }

    TEST(AsyncGroupHandlerTest, ReadyFailureWithoutHandlerIsRetainedUntilTaken)
    {
        AsyncGroup<void> group;

        group.submit(failed_future("retained"));

        EXPECT_EQ(group.approx_pending(), 0U);
        const auto errors = group.take_errors();
        ASSERT_EQ(errors.size(), 1U);
        EXPECT_EQ(message_of(errors.front()), "retained");
    }

    TEST(AsyncGroupSweepTest, ReadyTasksAreReclaimedBySubmit)
    {
        AsyncGroup<void> group;

        for (int i = 0; i < 50; ++i) {
            group.submit(ready_future());
        }

        EXPECT_EQ(group.approx_pending(), 0U);
    }

    TEST(AsyncGroupSweepTest, UnfinishedTaskStaysPendingUntilItIsReady)
    {
        AsyncGroup<void> group;
        std::promise<void> promise;

        group.submit(promise.get_future());
        EXPECT_EQ(group.approx_pending(), 1U);

        promise.set_value();
        group.drain();

        EXPECT_EQ(group.approx_pending(), 0U);
    }

    TEST(AsyncGroupSweepTest, SweepKeepsOnlyTheUnfinishedTask)
    {
        AsyncGroup<void> group;
        std::promise<void> pending;

        group.submit(pending.get_future());
        for (int i = 0; i < 10; ++i) {
            group.submit(ready_future());
        }

        EXPECT_EQ(group.approx_pending(), 1U);

        pending.set_value();
        group.drain();
        EXPECT_EQ(group.approx_pending(), 0U);
    }

    TEST(AsyncGroupConcurrencyTest, ConcurrentSubmittersLoseNoTask)
    {
        constexpr int producers = 8;
        constexpr int per_producer = 50;

        AsyncGroup<void> group;
        std::atomic<int> completed { 0 };

        std::vector<std::thread> threads;
        threads.reserve(producers);
        for (int p = 0; p < producers; ++p) {
            threads.emplace_back([&group, &completed] {
                for (int i = 0; i < per_producer; ++i) {
                    group.submit(std::async(std::launch::async, [&completed] {
                        completed.fetch_add(1, std::memory_order_relaxed);
                    }));
                }
            });
        }
        for (auto& t : threads) {
            t.join();
        }

        group.drain();

        EXPECT_EQ(completed.load(), producers * per_producer);
        EXPECT_EQ(group.approx_pending(), 0U);
    }

    TEST(AsyncGroupConcurrencyTest, ConcurrentFailuresAreAllAccountedFor)
    {
        constexpr int producers = 4;
        constexpr int per_producer = 25;

        AsyncGroup<bool> group;

        std::vector<std::thread> threads;
        threads.reserve(producers);
        for (int p = 0; p < producers; ++p) {
            threads.emplace_back([&group] {
                for (int i = 0; i < per_producer; ++i) {
                    group.submit(std::async(std::launch::async, []() -> bool {
                        throw std::runtime_error("concurrent");
                    }));
                }
            });
        }
        for (auto& t : threads) {
            t.join();
        }

        group.drain();

        EXPECT_EQ(group.take_errors().size(), static_cast<size_t>(producers * per_producer));
    }

    TEST(AsyncGroupComputeMatrixTest, ChainFailureMessageReachesGetLastError)
    {
        const auto matrix = Yantra::ComputeMatrix::create();

        matrix->with_async(
            Yantra::Datum<std::vector<double>>(std::vector<double> { 1.0 }),
            [](auto) -> Yantra::Datum<std::vector<double>> {
                throw std::runtime_error("deliberate chain failure");
            },
            [](Yantra::Datum<std::vector<double>>) { });

        matrix->drain_async();

        EXPECT_EQ(matrix->get_last_error(), "deliberate chain failure");
    }

    TEST(AsyncGroupComputeMatrixTest, CompletionRunsBeforeDrainAsyncReturns)
    {
        const auto matrix = Yantra::ComputeMatrix::create();
        std::atomic<bool> completed { false };
        std::vector<double> observed;

        matrix->with_async(
            Yantra::Datum<std::vector<double>>(std::vector<double> { 1.0, 2.0, 3.0 }),
            [](auto chain) { return chain.to_io(); },
            [&](Yantra::Datum<std::vector<double>> result) {
                observed = result.data;
                completed.store(true);
            });

        matrix->drain_async();

        EXPECT_TRUE(completed.load());
        EXPECT_EQ(observed, (std::vector<double> { 1.0, 2.0, 3.0 }));
    }

} // namespace
} // namespace MayaFlux::Test
