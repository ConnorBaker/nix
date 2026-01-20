#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <utility>

#include <boost/container_hash/hash.hpp>

namespace nix {

/**
 * An atomic counter aligned on a cache line to prevent false sharing.
 * The counter is only enabled when the `NIX_SHOW_STATS` environment
 * variable is set. This is to prevent contention on these counters
 * when multi-threaded evaluation is enabled.
 */
struct alignas(64) Counter
{
    using value_type = uint64_t;

    std::atomic<value_type> inner{0};

    static bool enabled;

    Counter() {}

    operator value_type() const noexcept
    {
        return inner;
    }

    void operator=(value_type n) noexcept
    {
        inner = n;
    }

    value_type load() const noexcept
    {
        return inner;
    }

    value_type operator++() noexcept
    {
        return enabled ? ++inner : 0;
    }

    value_type operator++(int) noexcept
    {
        return enabled ? inner++ : 0;
    }

    value_type operator--() noexcept
    {
        return enabled ? --inner : 0;
    }

    value_type operator--(int) noexcept
    {
        return enabled ? inner-- : 0;
    }

    value_type operator+=(value_type n) noexcept
    {
        return enabled ? inner += n : 0;
    }

    value_type operator-=(value_type n) noexcept
    {
        return enabled ? inner -= n : 0;
    }
};

/**
 * Histogram with exact counts for every size value.
 * Uses a mutex-protected sparse map for thread-safe recording.
 */
struct SizeHistogram
{
    mutable std::mutex mutex;
    mutable std::unordered_map<size_t, uint64_t> counts;

    void record(size_t size) noexcept
    {
        if (!Counter::enabled)
            return;

        std::lock_guard<std::mutex> lock(mutex);
        counts[size]++;
    }

    template<typename F>
    void forEach(F && fn) const
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto & [size, count] : counts) {
            fn(size, count);
        }
    }
};

/**
 * 2D histogram for tracking pairs of sizes (e.g., left/right operands).
 * Uses (size1, size2) as key to capture correlations between operands.
 */
struct SizePairHistogram
{
    mutable std::mutex mutex;
    mutable std::unordered_map<std::pair<size_t, size_t>, uint64_t, boost::hash<std::pair<size_t, size_t>>> counts;

    void record(size_t left, size_t right) noexcept
    {
        if (!Counter::enabled)
            return;

        std::lock_guard<std::mutex> lock(mutex);
        counts[std::make_pair(left, right)]++;
    }

    template<typename F>
    void forEach(F && fn) const
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto & [pair, count] : counts) {
            fn(pair.first, pair.second, count);
        }
    }
};

} // namespace nix
