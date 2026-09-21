#include "nix/util/thread-pool.hh"
#include "nix/util/signals.hh"
#include "nix/util/util.hh"

#include <algorithm>
#include <cassert>
#include <optional>

namespace nix {

CopyPool::CopyPool(size_t workers)
    : capacity(workers - 1)
{
    assert(workers >= 1);
    threads.reserve(capacity);
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
        std::packaged_task<void()> job;
        {
            std::unique_lock lock(mutex);
            wake.wait(lock, [&] { return stopping || !pending.empty(); });
            if (pending.empty())
                return;
            job = std::move(pending.front().run);
            pending.pop_front();
        }
        job();
    }
}

std::future<void> CopyPool::tryAsync(const void * group, std::function<void()> run)
{
    std::lock_guard lock(mutex);
    if (pending.size() == capacity)
        return {};
    std::packaged_task<void()> job(std::move(run));
    auto result = job.get_future();
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
    pending.push_back({group, std::move(job)});
    wake.notify_one();
    return result;
}

void CopyPool::wait(std::future<void> & result, const void * group)
{
    while (result.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        std::packaged_task<void()> job;
        {
            std::lock_guard lock(mutex);
            auto i = std::find_if(pending.begin(), pending.end(), [&](auto & job) { return job.group == group; });
            if (i != pending.end()) {
                job = std::move(i->run);
                pending.erase(i);
            }
        }
        if (!job.valid()) {
            result.wait();
            return;
        }
        job();
    }
}

void CopyPool::ordered(size_t count, fun<bool(size_t)> canFork, fun<void(size_t, bool)> run, fun<void(size_t)> consume)
{
    struct Child
    {
        std::future<void> running;
        std::exception_ptr error;
    };

    std::vector<Child> children(count);
    std::exception_ptr failure;
    try {
        for (size_t i = 0; i < count; ++i) {
            auto & child = children[i];
            if (count > 1 && canFork(i))
                child.running = tryAsync(&children, [&, i] { run(i, true); });
            if (!child.running.valid())
                try {
                    run(i, false);
                } catch (...) {
                    child.error = std::current_exception();
                    /* No later work is needed. Earlier jobs still determine
                       which error the ordered fold reports. */
                    count = i + 1;
                    break;
                }
        }
        for (size_t i = 0; i < count; ++i) {
            auto & child = children[i];
            if (child.running.valid()) {
                wait(child.running, &children);
                child.running.get();
            }
            if (child.error)
                std::rethrow_exception(child.error);
            consume(i);
        }
    } catch (...) {
        failure = std::current_exception();
    }
    /* Join outside exception unwinding: helped jobs must still observe
       interrupts, which are suppressed while a destructor is unwinding. */
    for (auto & child : children)
        if (child.running.valid())
            wait(child.running, &children);
    if (failure)
        std::rethrow_exception(failure);
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
