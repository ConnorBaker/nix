#pragma once
///@file

#include "nix/util/error.hh"
#include "nix/util/sync.hh"
#include "nix/util/fun.hh"

#include <boost/compat/move_only_function.hpp>

#include <queue>
#include <functional>
#include <thread>
#include <map>
#include <atomic>
#include <deque>
#include <future>

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

/** A bounded pool whose nested directory scopes join their own children.
 * A full queue leaves work on the caller; a waiting caller may help only
 * its own group, so waiting parents do not exhaust the pool. */
class CopyPool
{
    std::mutex mutex;
    std::condition_variable wake;

    struct Result
    {
        bool submitted = false; // Only the enclosing ordered() call accesses this.
        bool completed = false; // Completion and failure are published under mutex.
        std::exception_ptr failure;
    };

    struct Group
    {
        const fun<void(size_t, bool)> & run;
        std::vector<Result> children;
        std::condition_variable done;
    };

    struct Job
    {
        Group * group;
        size_t index;
    };

    std::deque<Job> pending;
    std::vector<std::thread> threads;
    size_t capacity;
    bool stopping = false;

    void work(std::promise<void> ready);
    void execute(const Job & job);
    void tryAsync(Group & group, size_t i);
    void wait(Group & group, Result & result);

public:
    explicit CopyPool(size_t workers);
    ~CopyPool();

    static size_t defaultWorkers();

    /** Evaluate a finite sequence, then consume its results in order.
     * `run` receives whether it needs a detached traversal scope. Every
     * submitted job is joined before return, including on failure. Child
     * and consume failures are reported in traversal order, independently
     * of completion order. Callbacks and their captured state must outlive
     * this call; each child owns its result storage. */
    void ordered(size_t count, fun<bool(size_t)> canFork, fun<void(size_t, bool)> run, fun<void(size_t)> consume);
};

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
