#pragma once
///@file

#include "nix/util/signals.hh"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

namespace nix {

/* A bounded pool for nested directory scopes. A full queue makes the
   caller visit inline. While joining, a caller helps only its own child
   jobs: nested helping follows the tree's depth, never an unbounded chain
   of siblings, and waiting parents cannot exhaust all execution slots.
   The caller counts as one additional worker. */
class NestedWorkPool
{
    std::mutex mutex;
    std::condition_variable wake;

    struct Job
    {
        const void * group;
        std::packaged_task<void()> run;
    };

    std::deque<Job> pending;
    std::vector<std::thread> threads;
    size_t capacity;
    bool stopping = false;

    void work()
    {
        ReceiveInterrupts receiveInterrupts;
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
            job(); // The future carries the exception back to its directory.
        }
    }

public:
    explicit NestedWorkPool(size_t workers)
        : capacity(workers - 1)
    {
        threads.reserve(capacity);
    }

    ~NestedWorkPool()
    {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        wake.notify_all();
        for (auto & thread : threads)
            thread.join();
    }

    std::future<void> tryAsync(const void * group, std::function<void()> run)
    {
        std::lock_guard lock(mutex);
        if (pending.size() == capacity)
            return {};
        std::packaged_task<void()> job(std::move(run));
        auto result = job.get_future();
        if (threads.size() < capacity)
            threads.emplace_back([this] { work(); });
        pending.push_back({group, std::move(job)});
        wake.notify_one();
        return result;
    }

    void wait(std::future<void> & result, const void * group)
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
};

} // namespace nix
