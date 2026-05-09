#pragma once
/// blocking-scope.hh — GDP token for blocking I/O capability.
///
/// gdp::Proof<BlockingTag> is a proof that the current code is running on
/// a dedicated blocking thread, NOT on the io_context event loop.
/// Any function that performs blocking I/O (SQLite, filesystem, git,
/// store daemon IPC) must require `const gdp::Proof<BlockingTag> &`.
///
/// The token can only be constructed by:
///   - coroBlock(): dispatches to BlockingThreadPool (Certifier<BlockingTag>)
///   - Classes that privately inherit Certifier<BlockingTag> (TraceStore, etc.)
///
/// Calling a blocking function without the proof is a compile error.
/// This prevents blocking I/O on io_context worker threads, which would
/// cause shutdown deadlocks (ioc_.stop() can't interrupt blocked handlers).

#include "nix/util/gdp/proof.hh"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <vector>

namespace nix::eval_trace {

/// Tag type for blocking-context GDP proofs.
struct BlockingTag {};

class BlockingThreadPool;

/// Dedicated thread pool for blocking I/O. Separate from the io_context
/// event loop. ioc_.stop() does NOT affect these threads — they run
/// blocking work to completion, then wait for more.
class BlockingThreadPool : private gdp::Certifier<BlockingTag> {
    template<typename F>
    friend auto coroBlock(BlockingThreadPool &, const F &)
        -> boost::asio::awaitable<std::invoke_result_t<F, const gdp::Proof<BlockingTag> &>>;

public:
    /// Takes io_context & as a structural dependency: the io_context
    /// (and its worker threads) must be declared BEFORE this pool.
    /// C++ destroys members in reverse declaration order, so this pool
    /// is destroyed first — its stop() joins blocking threads, which
    /// post timer cancellations to the io_context. The io_context
    /// workers are still alive to process them. Declaring this pool
    /// before the io_context triggers -Wreorder (compile error with
    /// -Werror) because the constructor references a not-yet-initialized member.
    BlockingThreadPool(boost::asio::io_context & ioc, uint32_t numThreads = 2);
    ~BlockingThreadPool();

    BlockingThreadPool(const BlockingThreadPool &) = delete;
    BlockingThreadPool & operator=(const BlockingThreadPool &) = delete;

    /// Post work to the blocking pool. The function runs on a pool thread.
    /// Accepts move-only callables (needed for asio completion handlers).
    template<typename F>
    void post(F && f)
    {
        {
            std::lock_guard lock(mutex_);
            work_.push_back(std::make_unique<Work<std::decay_t<F>>>(std::forward<F>(f)));
        }
        cv_.notify_one();
    }

    void stop();

private:
    /// Type-erased move-only callable wrapper.
    struct WorkBase {
        virtual ~WorkBase() = default;
        virtual void operator()() = 0;
    };
    template<typename F>
    struct Work final : WorkBase {
        F f;
        explicit Work(F && f) : f(std::move(f)) {}
        void operator()() override { f(); }
    };

    std::vector<std::thread> workers_;
    std::vector<std::unique_ptr<WorkBase>> work_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stopping_ = false;
};

/// Offload blocking work from a coroutine to the blocking pool.
///
/// Uses async_initiate (the asio-idiomatic custom async operation
/// pattern) instead of timers. The blocking thread signals completion
/// by posting the result to the coroutine's executor. No shared
/// mutable state, no timer queue races.
///
/// The previous implementation used steady_timer::cancel() from the
/// blocking thread to signal the coroutine. This had a data race:
/// concurrent coroBlock calls shared the io_context's timer queue,
/// and cancel() from the blocking thread raced with async_wait()
/// setup on ioc worker threads (TSAN: deadline_timer_service::cancel
/// vs timer_queue::enqueue_timer). async_initiate eliminates the
/// timer entirely — the type system prevents the race because there
/// is no shared mutable state to race on.
///
/// No io_context parameter. The coroutine's executor (from
/// this_coro::executor) determines where the completion runs.
/// The previous signature took io_context & — this was removed
/// because it invited creating timers and other unserialized
/// objects on the raw io_context, which caused data races. With
/// no io_context in scope, the only way to interact with the
/// event loop is through the executor, which is thread-safe.
template<typename F>
auto coroBlock(BlockingThreadPool & pool, const F & f)
    -> boost::asio::awaitable<std::invoke_result_t<F, const gdp::Proof<BlockingTag> &>>
{
    using R = std::invoke_result_t<F, const gdp::Proof<BlockingTag> &>;

    auto executor = co_await boost::asio::this_coro::executor;

    if constexpr (std::is_void_v<R>) {
        co_await boost::asio::async_initiate<
            decltype(boost::asio::use_awaitable), void(std::exception_ptr)>(
            [&pool, &f, executor](auto handler) {
                pool.post([h = std::move(handler), &f, executor]() mutable {
                    std::exception_ptr ep;
                    try {
                        BlockingThreadPool::withProof([&](const auto & scope) {
                            f(scope);
                        });
                    } catch (...) {
                        ep = std::current_exception();
                    }
                    boost::asio::post(executor,
                        [h = std::move(h), ep]() mutable {
                            std::move(h)(ep);
                        });
                });
            },
            boost::asio::use_awaitable);
    } else {
        co_return co_await boost::asio::async_initiate<
            decltype(boost::asio::use_awaitable), void(std::exception_ptr, R)>(
            [&pool, &f, executor](auto handler) {
                pool.post([h = std::move(handler), &f, executor]() mutable {
                    std::exception_ptr ep;
                    std::optional<R> val;
                    try {
                        BlockingThreadPool::withProof([&](const auto & scope) {
                            val.emplace(f(scope));
                        });
                    } catch (...) {
                        ep = std::current_exception();
                    }
                    boost::asio::post(executor,
                        [h = std::move(h), ep, v = std::move(val)]() mutable {
                            std::move(h)(ep, v ? std::move(*v) : R{});
                        });
                });
            },
            boost::asio::use_awaitable);
    }
}

/// Like Sync<T> but lock() requires a blocking-context proof.
///
/// Forces all callers through coroBlock or a Certifier<BlockingTag>.
/// Calling lock() without a gdp::Proof<BlockingTag> is a compile error —
/// prevents accidentally locking from the io_context event loop thread.
template<typename T>
class BlockingSync {
    mutable std::mutex mutex_;
    T data_;

public:
    BlockingSync() = default;

    template<typename... Args>
    explicit BlockingSync(Args &&... args)
        : data_(std::forward<Args>(args)...) {}

    class [[nodiscard]] WriteLock {
        std::unique_lock<std::mutex> lk_;
        T & data_;

        friend class BlockingSync;
        WriteLock(std::mutex & m, T & d) : lk_(m), data_(d) {}

    public:
        WriteLock(const WriteLock &) = delete;
        WriteLock & operator=(const WriteLock &) = delete;
        WriteLock(WriteLock &&) = default;

        T * operator->() { return &data_; }
        T & operator*() { return data_; }
    };

    /// Lock requires proof of blocking context.
    [[nodiscard]] WriteLock lock(const gdp::Proof<BlockingTag> &) {
        return WriteLock(mutex_, data_);
    }
};

} // namespace nix::eval_trace
