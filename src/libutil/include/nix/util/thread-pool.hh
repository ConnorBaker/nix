#pragma once
///@file

#include "nix/util/error.hh"
#include "nix/util/sync.hh"
#include "nix/util/fun.hh"
#include "nix/util/signals.hh"

#include <boost/compat/move_only_function.hpp>

#include <queue>
#include <functional>
#include <thread>
#include <map>
#include <atomic>
#include <vector>
#include <optional>
#include <span>
#include <utility>

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
     * Move-only, so that a work item can own what it operates on.
     * `std::move_only_function` would do, but libc++ does not implement
     * it yet.
     */
    using work_t = boost::compat::move_only_function<void()>;

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

    /** Drain with a registration owned before admission. Each invocation owns
     * its registration, including when separate callers process concurrently. */
    void process(ReceiveInterrupts receiveInterrupts);

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

    void doWork(bool mainThread, ReceiveInterrupts receiveInterrupts);
};

/** Evaluate independent, materialized child records, then fold in order.
 * The bool member is an advisory scheduling hint, not an effectful predicate.
 * Width is positive; the member pointer is valid, and its hint members remain
 * unchanged until return. A const span does not freeze aliases to the records.
 * Accepted work and exception cleanup finish before borrowed callbacks return.
 * A caller choosing a frontier must deny nested scheduling to all its children. */
template<typename Child>
void orderedFrontier(
    std::span<const Child> children,
    bool Child::* hint,
    size_t workers,
    function_ref<void(size_t)> run,
    function_ref<void(size_t)> consume)
{
    auto count = children.size();
    if (count < 2) {
        if (count) {
            run(0);
            consume(0);
        }
        return;
    }
    struct Group
    {
        function_ref<void(size_t)> run;
        size_t failedIndex;
        std::exception_ptr failure;
        std::mutex mutex;
        void recordFailure(size_t index, std::exception_ptr & childFailure)
        {
            std::lock_guard lock(mutex);
            if (index < failedIndex) {
                failedIndex = index;
                failure.swap(childFailure);
            }
        }
    } group{run, count, {}, {}};
    // Registration precedes pool construction and every accepted job.
    std::optional<std::pair<ReceiveInterrupts, ThreadPool>> frontier;
    std::exception_ptr failure;
    try {
        for (size_t i = 0; i < count; ++i) {
            if (workers > 1 && children[i].*hint) {
                if (!frontier)
                    frontier.emplace(std::nullopt, workers);
                frontier->second.enqueue([&group, i] {
                    std::exception_ptr failure;
                    try {
                        group.run(i);
                    } catch (...) {
                        failure = std::current_exception();
                    }
                    if (failure)
                        group.recordFailure(i, failure);
                    // Destroy a losing error outside the lock, before this job joins.
                });
                continue;
            }
            try {
                run(i);
            } catch (...) {
                auto childFailure = std::current_exception();
                group.recordFailure(i, childFailure);
                break;
            }
        }
    } catch (...) {
        failure = std::current_exception();
    }
    // Join outside exception unwinding, so child visits still observe interrupts.
    if (frontier)
        frontier->second.process(std::move(frontier->first));
    if (failure)
        std::rethrow_exception(failure);
    for (size_t i = 0, end = group.failedIndex; i < end; ++i)
        consume(i);
    if (group.failure)
        std::rethrow_exception(group.failure);
}

/**
 * Process in parallel a set of items of type T that have a partial
 * ordering between them. Thus, any item is only processed after all
 * its dependencies have been processed.
 */
template<typename T>
void processGraph(
    const std::set<T> & nodes,
    fun<std::set<T>(const T &)> getEdges,
    fun<void(const T &)> processNode,
    size_t maxThreads = 0)
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
    ThreadPool pool{maxThreads};

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
