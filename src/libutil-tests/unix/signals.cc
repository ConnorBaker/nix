#include "nix/util/signals.hh"
#include "nix/util/finally.hh"

#include <gtest/gtest.h>

#include <chrono>
#include <cerrno>
#include <future>
#include <optional>
#include <stdexcept>

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

    std::optional<ReceiveInterrupts> receiving;
    receiving.emplace();
    auto queued = receiving->makeCallback();
    auto copied = queued;

    unix::triggerInterrupt(); // The registered production callback.
    EXPECT_EQ(take(), NIX_SIG_MULTI_INT);
    for (auto * deliver : {&queued, &copied}) {
        (*deliver)();
        EXPECT_EQ(take(), NIX_SIG_MULTI_INT);
    }

    receiving.reset();
    /* Unregistration cannot recall copies already queued for delivery. */
    for (auto * deliver : {&queued, &copied}) {
        (*deliver)();
        EXPECT_EQ(take(), -1);
        EXPECT_EQ(errno, EAGAIN);
    }

    /* A new scope on the very same pthread_t does not revive old copies. */
    receiving.emplace();
    queued();
    copied();
    EXPECT_EQ(take(), -1);
    EXPECT_EQ(errno, EAGAIN);
    receiving->makeCallback()();
    EXPECT_EQ(take(), NIX_SIG_MULTI_INT);
}

#endif

TEST(ReceiveInterrupts, targetThreadCannotExitDuringADeliveryBorrow)
{
    using namespace std::chrono_literals;
    using Target = decltype(ReceiveInterrupts::target);
    std::promise<Target> targetReady;
    auto targetFuture = targetReady.get_future();
    std::promise<fun<void()>> callbackReady;
    auto callbackFuture = callbackReady.get_future();
    std::promise<void> leave, cancelling;
    auto leaveFuture = leave.get_future().share();
    auto cancellingFuture = cancelling.get_future();
    auto owner = std::async(std::launch::async, [&] {
        ReceiveInterrupts receiving;
        targetReady.set_value(receiving.target);
        callbackReady.set_value(receiving.makeCallback());
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
}

} // namespace nix
