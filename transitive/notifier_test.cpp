#include "MayaFlux/Transitive/Parallel/Notifier.hpp"

#include <gtest/gtest.h>
#include "../support/test_metadata.hpp"

MAYAFLUX_TEST_FILE((::MayaFlux::Test::Meta::Annotation { ::MayaFlux::Test::Meta::Kind::contract, ::MayaFlux::Test::Meta::requirements<>() }))

namespace MayaFlux::Test {
namespace {

    using Parallel::Notifier;
    using namespace std::chrono_literals;

    TEST(NotifierWaitTest, ANotificationBeforeTheWaitMakesItReturnAtOnce)
    {
        Notifier notifier;

        notifier.notify();

        EXPECT_TRUE(notifier.wait_for(0ms));
    }

    TEST(NotifierWaitTest, NotificationsBeforeTheWaitCollapseIntoOne)
    {
        Notifier notifier;

        notifier.notify();
        notifier.notify();
        notifier.notify();

        EXPECT_TRUE(notifier.wait_for(0ms));
        EXPECT_FALSE(notifier.wait_for(0ms));
    }

    TEST(NotifierWaitTest, ATimedWaitWithNoNotificationReturnsFalseAfterTheTimeout)
    {
        Notifier notifier;
        const auto start = std::chrono::steady_clock::now();

        EXPECT_FALSE(notifier.wait_for(20ms));

        EXPECT_GE(std::chrono::steady_clock::now() - start, 20ms);
    }

    TEST(NotifierWaitTest, AWaitUntilAPastDeadlineReturnsFalseAtOnce)
    {
        Notifier notifier;

        EXPECT_FALSE(notifier.wait_until(std::chrono::steady_clock::now() - 1s));
    }

    TEST(NotifierWaitTest, AWaitUntilAFutureDeadlineIsEndedByANotification)
    {
        Notifier notifier;
        std::jthread signaller([&notifier]() {
            std::this_thread::sleep_for(10ms);
            notifier.notify();
        });

        EXPECT_TRUE(notifier.wait_until(std::chrono::steady_clock::now() + 5s));
    }

    TEST(NotifierWaitTest, ABlockedWaitIsReleasedByAnotherThread)
    {
        Notifier notifier;
        std::atomic<bool> woke { false };

        std::jthread waiter([&notifier, &woke]() {
            notifier.wait();
            woke.store(true);
        });

        std::this_thread::sleep_for(20ms);
        EXPECT_FALSE(woke.load());

        notifier.notify();
        waiter.join();

        EXPECT_TRUE(woke.load());
    }

    TEST(NotifierWaitTest, ANotificationAfterAWaitIsNotLost)
    {
        Notifier notifier;

        notifier.notify();
        notifier.wait();

        notifier.notify();
        EXPECT_TRUE(notifier.wait_for(0ms));
    }

    TEST(NotifierStopTest, AStopCallbackReleasesAWaitingThread)
    {
        Notifier notifier;
        std::atomic<bool> finished { false };

        std::jthread waiter([&notifier, &finished](const std::stop_token& stop) {
            const std::stop_callback on_stop(stop, [&notifier] { notifier.notify(); });
            while (!stop.stop_requested()) {
                notifier.wait();
            }
            finished.store(true);
        });

        std::this_thread::sleep_for(10ms);
        waiter.request_stop();
        waiter.join();

        EXPECT_TRUE(finished.load());
    }

    TEST(NotifierConcurrencyTest, ManyNotifiersNeverStrandTheWaiter)
    {
        Notifier notifier;
        std::atomic<uint64_t> posted { 0 };
        std::atomic<uint64_t> seen { 0 };
        constexpr uint64_t per_thread = 20000;
        constexpr size_t threads = 4;

        std::jthread waiter([&](const std::stop_token& stop) {
            const std::stop_callback on_stop(stop, [&notifier] { notifier.notify(); });
            while (!stop.stop_requested()) {
                notifier.wait();
                seen.store(posted.load(std::memory_order_acquire), std::memory_order_release);
            }
        });

        {
            std::vector<std::jthread> signallers;
            for (size_t i = 0; i < threads; ++i) {
                signallers.emplace_back([&]() {
                    for (uint64_t n = 0; n < per_thread; ++n) {
                        posted.fetch_add(1, std::memory_order_acq_rel);
                        notifier.notify();
                    }
                });
            }
        }

        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (seen.load(std::memory_order_acquire) != per_thread * threads
            && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(1ms);
        }

        EXPECT_EQ(seen.load(), per_thread * threads);
    }

}
}
