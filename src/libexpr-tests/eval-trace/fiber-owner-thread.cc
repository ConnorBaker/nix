/**
 * Thread-affinity tests for the Suspendable thread-local bridge.
 *
 * Four tests cover the enforcement model for the OR-9 worker-thread
 * scenario:
 *   - Innermost_TracksScope exercises the Tracking invariant: the
 *     private static thread_local SuspendableCtxScope::current_ is
 *     correctly updated by scope ctor/dtor.  Observed via the
 *     public innermost() accessor.
 *   - TracedExpr_OffOwnerThread_Aborts: calling into the colored
 *     pipeline from a worker thread aborts at TracedExpr::eval's
 *     outermost-entry check.
 *   - SuspendableCtxScope_OffOwnerThread_Aborts: direct scope
 *     construction on a worker thread aborts at the scope ctor's
 *     thread-affinity check.
 *   - SyncAwait_OffOwnerThread_Aborts: smuggling an
 *     EvalContext<Suspendable> reference to a worker thread and
 *     calling syncAwait() aborts at the ctx's thread-affinity
 *     check.
 *
 * The death tests match on distinct substrings of the violation
 * messages so a silent deletion of any single check would fail the
 * corresponding test.  A primitive test for
 * FiberScheduler::onOwnerThread() directly is covered transitively
 * by TracedExpr_OffOwnerThread_Aborts.
 */
#include "eval-trace/helpers.hh"

#include "nix/expr/eval-trace/eval-context.hh"
#include "nix/expr/eval-trace/cache/trace-session.hh"
#include "nix/expr/eval-trace/cache/trace-backend.hh"

#include <atomic>
#include <thread>

#include <boost/asio/awaitable.hpp>

#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

class TracedExprThreadAffinityTest : public TraceCacheFixture {};

TEST_F(TracedExprThreadAffinityTest, Innermost_TracksScope)
{
    auto session = makeCache("42");
    auto * sched = session->traceBackend()->getScheduler();
    ASSERT_NE(sched, nullptr);

    EXPECT_EQ(SuspendableCtxScope::innermost(), nullptr);
    {
        SuspendableCtxScope scope(state, *sched);
        EXPECT_EQ(SuspendableCtxScope::innermost(), &scope);
    }
    EXPECT_EQ(SuspendableCtxScope::innermost(), nullptr);
}

TEST_F(TracedExprThreadAffinityTest, TracedExpr_OffOwnerThread_Aborts)
{
    auto session = makeCache("42");
    EXPECT_DEATH(
        {
            std::thread worker([&] { forceRoot(*session); });
            worker.join();
        },
        "TracedExpr evaluated off the scheduler");
}

TEST_F(TracedExprThreadAffinityTest, SuspendableCtxScope_OffOwnerThread_Aborts)
{
    auto session = makeCache("42");
    auto * sched = session->traceBackend()->getScheduler();
    ASSERT_NE(sched, nullptr);
    EXPECT_DEATH(
        {
            std::thread worker([&] {
                SuspendableCtxScope scope(state, *sched);
            });
            worker.join();
        },
        "SuspendableCtxScope constructed off");
}

// Construct scope on the test (owner) thread BEFORE EXPECT_DEATH so
// its ctor affinity check passes.  The forked/re-executed child
// inherits the scope (fast mode COW) or reconstructs on re-exec
// (threadsafe mode); either way, the worker spawned inside the
// closure has an id that differs from ctx.ownerThreadId_, so
// syncAwait aborts with a message distinct from the scope ctor's.
TEST_F(TracedExprThreadAffinityTest, SyncAwait_OffOwnerThread_Aborts)
{
    auto session = makeCache("42");
    auto * sched = session->traceBackend()->getScheduler();
    ASSERT_NE(sched, nullptr);
    SuspendableCtxScope scope(state, *sched);
    auto & ctx = scope.ctx();
    EXPECT_DEATH(
        {
            std::thread worker([&] {
                ctx.syncAwait([]() -> boost::asio::awaitable<void> {
                    co_return;
                }());
            });
            worker.join();
        },
        "syncAwait called from non-owner thread");
}

} // namespace nix::eval_trace
