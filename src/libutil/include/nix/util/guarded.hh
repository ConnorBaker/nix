#pragma once
///@file
///
/// Continuation-based synchronized data wrapper.
///
/// Guarded<T, Mutex> provides thread-safe access through withLock(f).
/// The lock is held exactly for the duration of f — the body cannot
/// accidentally hold the lock across suspension points or I/O.
///
/// Complements Sync<T> (RAII-handle-based, sync.hh). Use Guarded when
/// scoped continuation prevents lock-escape bugs; use Sync when you need
/// condvar waits or RAII lock handles.

#include <mutex>
#include <utility>

namespace nix {

template<typename T, typename Mutex = std::mutex>
class Guarded {
    mutable Mutex mutex_;
    T data_;

public:
    Guarded() = default;

    template<typename... Args>
        requires (sizeof...(Args) > 0)
    explicit Guarded(Args &&... args)
        : data_(std::forward<Args>(args)...) {}

    Guarded(const Guarded &) = delete;
    Guarded & operator=(const Guarded &) = delete;

    /// Execute body with exclusive access to the data.
    /// Non-const: body receives T &.
    template<typename F>
    decltype(auto) withLock(F && body)
    {
        std::lock_guard guard(mutex_);
        return std::forward<F>(body)(data_);
    }

    /// Const overload: body receives const T &.
    template<typename F>
    decltype(auto) withLock(F && body) const
    {
        std::lock_guard guard(mutex_);
        return std::forward<F>(body)(data_);
    }

    /// Execute body WITHOUT locking. For single-threaded shutdown/teardown
    /// paths where no concurrent access is possible and locking would
    /// deadlock (e.g., GC finalizer thread holds the mutex).
    template<typename F>
    decltype(auto) withLockUnsafe(F && body)
    {
        return std::forward<F>(body)(data_);
    }
};

} // namespace nix
