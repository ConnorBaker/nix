#pragma once

#include <array>
#include <atomic>
#include <cstdint>

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
 * Histogram with fixed buckets for argument size statistics.
 * Buckets: 0, 1, 2-5, 6-10, 11-50, 51+
 */
struct SizeHistogram
{
    static constexpr size_t NUM_BUCKETS = 6;
    Counter buckets[NUM_BUCKETS];

    static constexpr size_t bucketIndex(size_t size) noexcept
    {
        if (size == 0) return 0;
        if (size == 1) return 1;
        if (size <= 5) return 2;
        if (size <= 10) return 3;
        if (size <= 50) return 4;
        return 5;
    }

    void record(size_t size) noexcept
    {
        buckets[bucketIndex(size)]++;
    }

    static constexpr std::array<const char *, NUM_BUCKETS> bucketLabels() noexcept
    {
        return {"0", "1", "2-5", "6-10", "11-50", "51+"};
    }
};

} // namespace nix
