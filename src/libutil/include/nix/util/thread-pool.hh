#pragma once
///@file

#include "nix/util/error.hh"
#include "nix/util/fun.hh"
#include "nix/util/signals.hh"
#include "nix/util/sync.hh"

#include <algorithm>
#include <queue>
#include <functional>
#include <thread>
#include <map>
#include <atomic>
#include <vector>

namespace nix {

MakeError(ThreadPoolShutDown, Error);

/**
 * A simple thread pool that executes a queue of work items
 * (lambdas).
 */
class ThreadPool
{
public:

    ThreadPool(size_t maxThreads = 0);

    ~ThreadPool();

    /**
     * An individual work item.
     *
     * \todo use std::packaged_task?
     */
    typedef fun<void()> work_t;

    /**
     * Enqueue a function to be executed by the thread pool.
     */
    void enqueue(work_t t);

    /**
     * Execute work items until the queue is empty.
     *
     * \note Note that work items are allowed to add new items to the
     * queue; this is handled correctly.
     *
     * Queue processing stops prematurely if any work item throws an
     * exception. This exception is propagated to the calling thread. If
     * multiple work items throw an exception concurrently, only one
     * item is propagated; the others are printed on stderr and
     * otherwise ignored.
     */
    void process();

    /**
     * Shut down all worker threads and wait until they've exited.
     * Active work items are finished, but any pending work items are discarded.
     */
    void shutdown();

private:

    size_t maxThreads;

    struct State
    {
        std::queue<work_t> pending;
        size_t active = 0;
        std::exception_ptr exception;
        std::vector<std::thread> workers;
        bool draining = false;
    };

    std::atomic_bool quit{false};

    Sync<State> state_;

    std::condition_variable work;

    void doWork(bool mainThread);
};

/**
 * Iterate `items` in parallel via `ThreadPool`, invoking `body(item)`
 * for each element. Work is partitioned into chunks sized to give
 * each worker ~4 chunks (rough work-stealing headroom), clamped to
 * `maxChunkSize` to bound per-task overhead and queue memory.
 *
 * `body` runs on worker threads; shared state must be synchronised
 * by the caller (typically `std::atomic` or `Sync<T>`). Each
 * per-item iteration calls `checkInterrupt()` so the pool shuts
 * down promptly on SIGINT.
 *
 * Empty input is a no-op. If `nThreads == 1`, the caller's thread
 * does all the work via `ThreadPool::process()`.
 */
template<typename Item, typename Body>
void parallelForEachChunked(std::vector<Item> & items, size_t nThreads, size_t maxChunkSize, Body && body)
{
    if (items.empty())
        return;

    size_t chunkSize = std::clamp<size_t>(items.size() / (nThreads * 4), 1, maxChunkSize);

    ThreadPool pool(nThreads);
    for (size_t start = 0; start < items.size(); start += chunkSize) {
        size_t end = std::min(start + chunkSize, items.size());
        pool.enqueue([&, start, end] {
            for (size_t i = start; i < end; ++i) {
                checkInterrupt();
                body(items[i]);
            }
        });
    }
    pool.process();
}

/**
 * Process in parallel a set of items of type T that have a partial
 * ordering between them. Thus, any item is only processed after all
 * its dependencies have been processed.
 */
template<typename T>
void processGraph(const std::set<T> & nodes, fun<std::set<T>(const T &)> getEdges, fun<void(const T &)> processNode)
{
    struct Graph
    {
        std::set<T> left;
        std::map<T, std::set<T>> refs, rrefs;
    };

    Sync<Graph> graph_(Graph{nodes, {}, {}});

    std::function<void(const T &)> worker;

    /* Create pool last to ensure threads are stopped before other destructors
     * run */
    ThreadPool pool;

    worker = [&](const T & node) {
        {
            auto graph(graph_.lock());
            auto i = graph->refs.find(node);
            if (i == graph->refs.end())
                goto getRefs;
            goto doWork;
        }

    getRefs: {
        auto refs = getEdges(node);
        refs.erase(node);

        {
            auto graph(graph_.lock());
            for (auto & ref : refs)
                if (graph->left.count(ref)) {
                    graph->refs[node].insert(ref);
                    graph->rrefs[ref].insert(node);
                }
            if (graph->refs[node].empty())
                goto doWork;
        }
    }

        return;

    doWork:
        processNode(node);

        /* Enqueue work for all nodes that were waiting on this one
           and have no unprocessed dependencies. */
        {
            auto graph(graph_.lock());
            for (auto & rref : graph->rrefs[node]) {
                auto & refs(graph->refs[rref]);
                auto i = refs.find(node);
                assert(i != refs.end());
                refs.erase(i);
                if (refs.empty())
                    pool.enqueue(std::bind(worker, rref));
            }
            graph->left.erase(node);
            graph->refs.erase(node);
            graph->rrefs.erase(node);
        }
    };

    for (auto & node : nodes) {
        try {
            pool.enqueue(std::bind(worker, std::ref(node)));
        } catch (ThreadPoolShutDown &) {
            /* Stop if the thread pool is shutting down. It means a
               previous work item threw an exception, so process()
               below will rethrow it. */
            break;
        }
    }

    pool.process();

    if (!graph_.lock()->left.empty())
        throw Error("graph processing incomplete (cyclic reference?)");
}

} // namespace nix
