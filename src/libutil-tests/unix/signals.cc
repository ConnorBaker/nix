#include "nix/util/signals.hh"
#include "nix/util/finally.hh"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cerrno>
#include <future>
#include <optional>
#include <stdexcept>
#include <utility>

namespace nix {

#ifdef __linux__ // sigtimedwait makes every signal observation nonblocking.

TEST(ReceiveInterrupts, copiedDeliveriesCannotOutliveTheirRegistration)
{
    sigset_t signal, previous;
    sigemptyset(&signal);
    sigaddset(&signal, NIX_SIG_MULTI_INT);
    ASSERT_EQ(pthread_sigmask(SIG_BLOCK, &signal, &previous), 0);
    auto take = [&] {
        timespec immediate{};
        return sigtimedwait(&signal, nullptr, &immediate);
    };
    Finally restoreMask{[&] {
        while (take() == NIX_SIG_MULTI_INT) {
        }
        pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    }};
    Finally restoreInterrupted{[old = getInterrupted()] { setInterrupted(old); }};

    unsigned cases = 0;
    for (auto [initiallyActive, activate] :
         {std::pair{true, true}, std::pair{false, true}, std::pair{false, false}})
        for (unsigned moves = 0; moves != 4; ++moves) {
            SCOPED_TRACE(::testing::Message()
                         << "initiallyActive=" << initiallyActive << "activate=" << activate << "moves=" << moves);
            std::array<std::optional<ReceiveInterrupts>, 4> scopes;
            if (initiallyActive)
                scopes[0].emplace();
            else
                scopes[0].emplace(std::nullopt);
            auto target = scopes[0]->target;
            auto registration = scopes[0]->callback.get();
            auto queued = scopes[0]->makeCallback();
            auto copied = queued;

            if (!initiallyActive) {
                unix::triggerInterrupt();
                queued();
                copied();
                EXPECT_EQ(take(), -1);
                EXPECT_EQ(errno, EAGAIN);
            }
            for (unsigned i = 0; i != moves; ++i) {
                scopes[i + 1].emplace(std::move(*scopes[i]));
                EXPECT_FALSE(scopes[i]->target);
                EXPECT_FALSE(scopes[i]->callback);
                scopes[i]->makeCallback()();
                ReceiveInterrupts empty{std::move(*scopes[i])};
                EXPECT_FALSE(empty.target);
                EXPECT_FALSE(empty.callback);
                empty.makeCallback()();
                scopes[i].reset();
                EXPECT_EQ(scopes[i + 1]->target, target);
                EXPECT_EQ(scopes[i + 1]->callback.get(), registration);
                EXPECT_EQ(take(), -1);
                EXPECT_EQ(errno, EAGAIN);
            }
            auto & receiving = scopes[moves];
            if (!initiallyActive && activate)
                *receiving->target->lock() = pthread_self();

            if (activate) {
                unix::triggerInterrupt(); // The original registered production callback survived every move.
                EXPECT_EQ(take(), NIX_SIG_MULTI_INT);
                for (auto * deliver : {&queued, &copied}) {
                    (*deliver)();
                    EXPECT_EQ(take(), NIX_SIG_MULTI_INT);
                }
                std::async(std::launch::async, copied).get();
                EXPECT_EQ(take(), NIX_SIG_MULTI_INT); // Delivery runs elsewhere but targets this thread.
            }

            receiving.reset();
            EXPECT_FALSE(*target->lock());
            /* Unregistration cannot recall copies already queued for delivery. */
            for (auto * deliver : {&queued, &copied}) {
                (*deliver)();
                EXPECT_EQ(take(), -1);
                EXPECT_EQ(errno, EAGAIN);
            }
            std::async(std::launch::async, copied).get();
            EXPECT_EQ(take(), -1);
            EXPECT_EQ(errno, EAGAIN);

            /* A new scope on the very same pthread_t does not revive old copies. */
            receiving.emplace();
            queued();
            copied();
            EXPECT_EQ(take(), -1);
            EXPECT_EQ(errno, EAGAIN);
            receiving->makeCallback()();
            EXPECT_EQ(take(), NIX_SIG_MULTI_INT);
            ++cases;
        }
    EXPECT_EQ(cases, 12u);
    RecordProperty("ownership_configurations", cases);
}

#endif

TEST(ReceiveInterrupts, targetThreadCannotExitDuringADeliveryBorrow)
{
    using namespace std::chrono_literals;
    using Target = decltype(ReceiveInterrupts::target);
    unsigned cases = 0;
    for (bool initiallyActive : {false, true})
        for (unsigned moves = 0; moves != 4; ++moves) {
            SCOPED_TRACE(::testing::Message() << "active=" << initiallyActive << " moves=" << moves);
            std::promise<Target> targetReady;
            auto targetFuture = targetReady.get_future();
            std::promise<fun<void()>> callbackReady;
            auto callbackFuture = callbackReady.get_future();
            std::promise<void> leave, cancelling;
            auto leaveFuture = leave.get_future().share();
            auto cancellingFuture = cancelling.get_future();
            auto owner = std::async(std::launch::async, [&] {
                std::array<std::optional<ReceiveInterrupts>, 4> scopes;
                if (initiallyActive)
                    scopes[0].emplace();
                else
                    scopes[0].emplace(std::nullopt);
                auto queued = scopes[0]->makeCallback();
                for (unsigned i = 0; i != moves; ++i) {
                    scopes[i + 1].emplace(std::move(*scopes[i]));
                    scopes[i].reset();
                }
                auto & receiving = scopes[moves];
                if (!initiallyActive)
                    *receiving->target->lock() = pthread_self();
                targetReady.set_value(receiving->target);
                callbackReady.set_value(std::move(queued));
                if (leaveFuture.wait_for(5s) != std::future_status::ready)
                    throw std::runtime_error("interrupt owner did not receive its exit request");
                cancelling.set_value();
            });
            bool released = false;
            auto release = [&] {
                if (!released) {
                    released = true;
                    leave.set_value();
                }
            };
            Finally unblock{release}; // Runs before the async future joins on any assertion failure.
            ASSERT_EQ(targetFuture.wait_for(5s), std::future_status::ready);
            auto target = targetFuture.get();
            ASSERT_EQ(callbackFuture.wait_for(5s), std::future_status::ready);
            auto queued = callbackFuture.get();
            {
                /* Delivery holds this exact lock through pthread_kill. Holding its
                   borrow here fixes the in-flight schedule without a global hook. */
                auto delivery = target->lock();
                ASSERT_TRUE(*delivery);
                release();
                ASSERT_EQ(cancellingFuture.wait_for(5s), std::future_status::ready);
                EXPECT_EQ(owner.wait_for(20ms), std::future_status::timeout);
            }
            ASSERT_EQ(owner.wait_for(5s), std::future_status::ready);
            owner.get();
            /* The owner has exited and joined; even a queued delivery is now inert. */
            ASSERT_FALSE(*target->lock());
            queued();
            ++cases;
        }
    EXPECT_EQ(cases, 8u);
    RecordProperty("ownership_configurations", cases);
}

} // namespace nix
