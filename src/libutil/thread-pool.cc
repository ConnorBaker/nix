#include "nix/util/thread-pool.hh"
#include "nix/util/signals.hh"
#include "nix/util/util.hh"

#include <cassert>
#include <optional>
#include <utility>

namespace nix {

void ThreadPoolShutDown::anchor() {}

ThreadPool::ThreadPool(size_t _maxThreads)
    : maxThreads(_maxThreads)
{
    if (!maxThreads) {
        maxThreads = std::thread::hardware_concurrency();
        if (!maxThreads)
            maxThreads = 1;
    }

    debug("starting pool of %d threads", maxThreads - 1);
}

ThreadPool::~ThreadPool()
{
    shutdown();
}

void ThreadPool::shutdown()
{
    std::vector<std::thread> workers;
    {
        auto state(state_.lock());
        quit = true;
        std::swap(workers, state->workers);
    }

    if (workers.empty())
        return;

    if (lvlDebug <= verbosity)
        logger->log(lvlDebug, "reaping worker threads");

    work.notify_all();

    for (auto & thr : workers)
        thr.join();
}

void ThreadPool::enqueue(work_t t)
{
    auto state(state_.lock());
    if (quit)
        throw ThreadPoolShutDown("cannot enqueue a work item while the thread pool is shutting down");
    /* process() also executes items. Start the first background worker on
       first admission, and reject a failed worker start before queuing work. */
    if (state->pending.size() >= state->workers.size() && state->workers.size() + 1 < maxThreads)
        state->workers.emplace_back([this, receiveInterrupts = ReceiveInterrupts{std::nullopt}]() mutable {
            doWork(false, std::move(receiveInterrupts));
        });
    state->pending.push(std::move(t));
    work.notify_one();
}

void ThreadPool::process()
{
    try {
        process(ReceiveInterrupts{});
    } catch (...) {
        shutdown();
        throw;
    }
}

void ThreadPool::process(ReceiveInterrupts receiveInterrupts)
{
    state_.lock()->draining = true;

    /* Do work until no more work is pending or active. */
    try {
        doWork(true, std::move(receiveInterrupts));

        auto state(state_.lock());

        assert(quit);

        if (state->exception)
            std::rethrow_exception(state->exception);

    } catch (...) {
        /* In the exceptional case, some workers may still be
           active. They may be referencing the stack frame of the
           caller. So wait for them to finish. (~ThreadPool also does
           this, but it might be destroyed after objects referenced by
           the work item lambdas.) */
        shutdown();
        throw;
    }
}

void ThreadPool::doWork(bool mainThread, ReceiveInterrupts receiveInterrupts)
{
#ifndef _WIN32 // Does Windows need anything similar for async exit handling?
    if (receiveInterrupts.target)
        *receiveInterrupts.target->lock() = pthread_self();
    if (!mainThread)
        unix::interruptCheck = [&]() { return (bool) quit; };
#endif

    bool didWork = false;
    std::exception_ptr exc;

    while (true) {
        work_t w;
        {
            auto state(state_.lock());

            if (didWork) {
                assert(state->active);
                state->active--;

                if (exc) {

                    if (!state->exception) {
                        state->exception = exc;
                        // Tell the other workers to quit.
                        quit = true;
                        work.notify_all();
                    } else {
                        /* Print the exception, since we can't
                           propagate it. */
                        try {
                            std::rethrow_exception(exc);
                        } catch (const Interrupted &) {
                            // The interrupted state may be picked up by multiple
                            // workers, which is expected, so we should ignore
                            // it silently and let the first one bubble up,
                            // rethrown via the original state->exception.
                        } catch (const ThreadPoolShutDown &) {
                            // Similarly expected.
                        } catch (std::exception & e) {
                            ignoreExceptionExceptInterrupt();
                        }
                    }
                }
            }

            /* Wait until a work item is available or we're asked to
               quit. */
            while (true) {
                if (quit)
                    return;

                if (!state->pending.empty())
                    break;

                /* If there are no active or pending items, and the
                   main thread is running process(), then no new items
                   can be added. So exit. */
                if (!state->active && state->draining) {
                    quit = true;
                    work.notify_all();
                    return;
                }

                state.wait(work);
            }

            w = std::move(state->pending.front());
            state->pending.pop();
            state->active++;
        }

        try {
            w();
        } catch (...) {
            exc = std::current_exception();
        }

        didWork = true;
    }
}

} // namespace nix
