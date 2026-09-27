#include "nix/util/thread-pool.hh"
#include "nix/util/signals.hh"
#include "nix/util/util.hh"

#include <algorithm>
#include <cassert>
#include <optional>
#include <utility>

namespace nix {

CopyPool::CopyPool(size_t workers)
    : capacity(workers - 1)
{
    assert(workers >= 1);
    threads.reserve(capacity);
    pending.reserve(capacity);
}

CopyPool::~CopyPool()
{
    {
        std::lock_guard lock(mutex);
        stopping = true;
    }
    wake.notify_all();
    for (auto & thread : threads)
        thread.join();
}

size_t CopyPool::defaultWorkers()
{
    return std::min<size_t>(std::max<unsigned int>(std::thread::hardware_concurrency(), 1), 8);
}

void CopyPool::work(std::promise<void> ready)
{
    /* Registration allocates. Report failure before accepting any work
       on this thread, rather than letting it escape the thread entry. */
    std::optional<ReceiveInterrupts> receiveInterrupts;
    try {
        receiveInterrupts.emplace();
    } catch (...) {
        ready.set_exception(std::current_exception());
        return;
    }
    ready.set_value();
    while (true) {
        Job job;
        {
            std::unique_lock lock(mutex);
            wake.wait(lock, [&] { return stopping || !pending.empty(); });
            if (pending.empty())
                return;
            job = pending.back();
            pending.pop_back();
        }
        execute(job);
    }
}

void CopyPool::execute(const Job & job)
{
    std::exception_ptr failure;
    try {
        job.group->run(job.index, true);
    } catch (...) {
        failure = std::current_exception();
    }
    std::unique_lock lock(mutex);
    if (failure && job.index < job.group->failedIndex) {
        job.group->failedIndex = job.index;
        job.group->failure.swap(failure);
    }
    if (failure) {
        /* A discarded exception's destructor may reenter the pool. Release
           it outside the mutex, but before this job is observed as joined. */
        lock.unlock();
        failure = nullptr;
        lock.lock();
    }
    /* Notify before unlocking: the waiter may destroy this group as soon
       as it observes completion under the same mutex. */
    if (!--job.group->remaining)
        job.group->done.notify_one();
}

bool CopyPool::tryAsync(Group & group, size_t i)
{
    std::unique_lock lock(mutex);
    if (pending.size() == capacity)
        return false;
    if (threads.size() < capacity) {
        std::promise<void> ready;
        auto started = ready.get_future();
        threads.emplace_back([this, ready = std::move(ready)]() mutable { work(std::move(ready)); });
        try {
            started.get();
        } catch (...) {
            threads.back().join();
            threads.pop_back();
            throw;
        }
    }
    pending.push_back({&group, i});
    ++group.remaining;
    lock.unlock();
    wake.notify_one();
    return true;
}

void CopyPool::wait(Group & group)
{
    std::unique_lock lock(mutex);
    while (group.remaining) {
        auto i = std::find_if(pending.begin(), pending.end(), [&](auto & job) { return job.group == &group; });
        if (i == pending.end()) {
            group.done.wait(lock, [&] { return !group.remaining; });
        } else {
            auto job = std::exchange(*i, pending.back());
            pending.pop_back();
            lock.unlock();
            execute(job);
            lock.lock();
        }
    }
}

void CopyPool::ordered(size_t count, fun<bool(size_t)> canFork, fun<void(size_t, bool)> run, fun<void(size_t)> consume)
{
    if (count < 2) {
        if (count) {
            run(0, false);
            consume(0);
        }
        return;
    }

    Group group{run, count, {}, 0, {}};
    std::exception_ptr failure;
    try {
        for (size_t i = 0; i < count; ++i) {
            if (canFork(i) && tryAsync(group, i))
                continue;
            try {
                run(i, false);
            } catch (...) {
                auto childFailure = std::current_exception();
                {
                    std::lock_guard lock(mutex);
                    if (i < group.failedIndex) {
                        group.failedIndex = i;
                        group.failure.swap(childFailure);
                    }
                }
                /* Stop launching, but let the ordered fold choose between
                   this visit's failure and the preceding prefix. */
                break;
            }
        }
    } catch (...) {
        failure = std::current_exception();
    }
    /* Join outside exception unwinding: helped jobs must still observe
       interrupts, which are suppressed while a destructor is unwinding. */
    wait(group);
    if (failure)
        std::rethrow_exception(failure);
    for (size_t i = 0; i < group.failedIndex; ++i)
        consume(i);
    if (group.failure)
        std::rethrow_exception(group.failure);
}

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

    debug("reaping %d worker threads", workers.size());

    work.notify_all();

    for (auto & thr : workers)
        thr.join();
}

void ThreadPool::enqueue(work_t t)
{
    auto state(state_.lock());
    if (quit)
        throw ThreadPoolShutDown("cannot enqueue a work item while the thread pool is shutting down");
    state->pending.push(std::move(t));
    /* Note: process() also executes items, so count it as a worker. */
    if (state->pending.size() > state->workers.size() + 1 && state->workers.size() + 1 < maxThreads)
        state->workers.emplace_back(&ThreadPool::doWork, this, false);
    work.notify_one();
}

void ThreadPool::process()
{
    state_.lock()->draining = true;

    /* Do work until no more work is pending or active. */
    try {
        doWork(true);

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

void ThreadPool::doWork(bool mainThread)
{
    ReceiveInterrupts receiveInterrupts;

#ifndef _WIN32 // Does Windows need anything similar for async exit handling?
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
