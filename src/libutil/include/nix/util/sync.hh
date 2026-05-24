#pragma once
///@file

#include <cstdlib>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <cassert>

#include "nix/util/error.hh"

namespace nix {

/**
 * `Sync<T>` ensures synchronized access to a value of type `T` via a
 * `std::mutex`. It is used as follows:
 *
 *   struct Data { int x; ... };
 *
 *   Sync<Data> data;
 *
 *   {
 *     auto data_(data.lock());
 *     data_->x = 123;
 *   }
 *
 * Here, "data" is automatically unlocked when "data_" goes out of
 * scope.
 *
 * `readLock()` is provided as a documentation device. A plain
 * `std::mutex` offers no shared-locking distinction, so `readLock()`
 * still acquires the same exclusive lock as `lock()`. The returned
 * type (`ConstLock`) exposes only `const`-qualified access so that
 * read-only intent at the call site is enforced by the type system.
 * Use `SharedSync` if you need a true shared (reader) lock.
 *
 * The lock object exposes condition-variable wait helpers
 * (`wait`/`wait_for`/`wait_until`) that take a
 * `std::condition_variable` reference. These are not provided on
 * `SharedSync` because `std::condition_variable` does not bind to
 * `std::shared_lock`.
 */
template<class T>
class Sync
{
private:
    mutable std::mutex mutex;
    T data;

public:

    using element_type = T;

    Sync() {}

    Sync(const T & data)
        : data(data)
    {
    }

    Sync(T && data) noexcept
        : data(std::move(data))
    {
    }

    template<typename... Ts>
    Sync(Ts &&... args)
        requires requires { T{std::forward<Ts>(args)...}; }
        : data(std::forward<Ts>(args)...)
    {
    }

    Sync(Sync && other) noexcept
        : data(std::move(*other.lock()))
    {
    }

    class Lock
    {
    private:
        Sync * s;
        std::unique_lock<std::mutex> lk;
        friend Sync;

        Lock(Sync * s)
            : s(s)
            , lk(s->mutex)
        {
        }

    public:
        Lock(Lock && l) = delete;
        Lock(const Lock & l) = delete;
        Lock & operator=(Lock && l) = delete;
        Lock & operator=(const Lock & l) = delete;

        ~Lock() {}

        T * operator->()
        {
            return &s->data;
        }

        T & operator*()
        {
            return s->data;
        }

        void wait(std::condition_variable & cv)
        {
            cv.wait(lk);
        }

        template<class Predicate>
        void wait(std::condition_variable & cv, Predicate pred)
        {
            cv.wait(lk, std::move(pred));
        }

        template<class Rep, class Period>
        std::cv_status wait_for(std::condition_variable & cv, const std::chrono::duration<Rep, Period> & duration)
        {
            return cv.wait_for(lk, duration);
        }

        template<class Rep, class Period, class Predicate>
        bool
        wait_for(std::condition_variable & cv, const std::chrono::duration<Rep, Period> & duration, Predicate pred)
        {
            return cv.wait_for(lk, duration, pred);
        }

        template<class Clock, class Duration>
        std::cv_status
        wait_until(std::condition_variable & cv, const std::chrono::time_point<Clock, Duration> & duration)
        {
            return cv.wait_until(lk, duration);
        }
    };

    /**
     * Read-only view on the locked inner value. Returned by
     * `readLock()`; otherwise structurally identical to `Lock` but
     * exposes only `const`-qualified access so callers can't mutate
     * through a lock acquired with read-only intent. The underlying
     * mutex is still held exclusively (a `std::mutex` has no shared
     * mode); use `SharedSync` for genuine reader/writer parallelism.
     */
    class ConstLock
    {
    private:
        const Sync * s;
        std::unique_lock<std::mutex> lk;
        friend Sync;

        ConstLock(const Sync * s)
            : s(s)
            , lk(s->mutex)
        {
        }

    public:
        ConstLock(ConstLock && l) = delete;
        ConstLock(const ConstLock & l) = delete;
        ConstLock & operator=(ConstLock && l) = delete;
        ConstLock & operator=(const ConstLock & l) = delete;

        ~ConstLock() {}

        const T * operator->() const
        {
            return &s->data;
        }

        const T & operator*() const
        {
            return s->data;
        }
    };

    /**
     * Acquire (exclusive) access to the inner value.
     */
    Lock lock()
    {
        return Lock(this);
    }

    /**
     * Acquire read-only (still exclusive) access to the inner value.
     * The returned `ConstLock` exposes only `const T*` / `const T&`,
     * preserving the historical documented contract that read-side
     * lock holders cannot mutate through them. (For `std::mutex`
     * there is no shared mode, so the lock itself is still exclusive
     * — only the value-level access is constrained.)
     */
    ConstLock readLock() const
    {
        return ConstLock(this);
    }
};

/**
 * `SharedSync<T>` is like `Sync<T>` but built on `std::shared_mutex`
 * so multiple readers can hold the read lock concurrently. Acquire
 * write (exclusive) access with `lock()` and read (shared) access
 * with `readLock()`. Condition-variable waits are intentionally NOT
 * exposed because `std::condition_variable` does not bind to
 * `std::shared_lock`; if you need cv waits, use `Sync`.
 */
template<class T>
class SharedSync
{
private:
    mutable std::shared_mutex mutex;
    T data;

public:

    using element_type = T;

    SharedSync() {}

    SharedSync(const T & data)
        : data(data)
    {
    }

    SharedSync(T && data) noexcept
        : data(std::move(data))
    {
    }

    template<typename... Ts>
    SharedSync(Ts &&... args)
        requires requires { T{std::forward<Ts>(args)...}; }
        : data(std::forward<Ts>(args)...)
    {
    }

    SharedSync(SharedSync && other) noexcept
        : data(std::move(*other.lock()))
    {
    }

    class WriteLock
    {
    private:
        SharedSync * s;
        std::unique_lock<std::shared_mutex> lk;
        friend SharedSync;

        WriteLock(SharedSync * s)
            : s(s)
            , lk(s->mutex)
        {
        }

    public:
        WriteLock(WriteLock && l) = delete;
        WriteLock(const WriteLock & l) = delete;
        WriteLock & operator=(WriteLock && l) = delete;
        WriteLock & operator=(const WriteLock & l) = delete;

        ~WriteLock() {}

        T * operator->()
        {
            return &s->data;
        }

        T & operator*()
        {
            return s->data;
        }
    };

    class ReadLock
    {
    private:
        const SharedSync * s;
        std::shared_lock<std::shared_mutex> lk;
        friend SharedSync;

        ReadLock(const SharedSync * s)
            : s(s)
            , lk(s->mutex)
        {
        }

    public:
        ReadLock(ReadLock && l) = delete;
        ReadLock(const ReadLock & l) = delete;
        ReadLock & operator=(ReadLock && l) = delete;
        ReadLock & operator=(const ReadLock & l) = delete;

        ~ReadLock() {}

        const T * operator->()
        {
            return &s->data;
        }

        const T & operator*()
        {
            return s->data;
        }
    };

    /**
     * Acquire write (exclusive) access to the inner value.
     */
    WriteLock lock()
    {
        return WriteLock(this);
    }

    /**
     * Acquire read (shared) access to the inner value.
     */
    ReadLock readLock() const
    {
        return ReadLock(this);
    }
};

} // namespace nix
