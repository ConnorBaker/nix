#pragma once
///@file

#include <chrono>
#include <cstdint>

namespace nix {

/* Per-stage wall-clock timer. `record(field)` charges the interval
   since the cursor was last advanced to `field` and resets the
   cursor to "now". Single-threaded — used by `optimiseStore` and
   `collectGarbage` for their `Timings` struct counters. */
struct StageTimer
{
    std::chrono::steady_clock::time_point cursor;

    StageTimer()
        : cursor(std::chrono::steady_clock::now())
    {
    }

    void record(uint64_t & field)
    {
        auto now = std::chrono::steady_clock::now();
        field += std::chrono::duration_cast<std::chrono::nanoseconds>(now - cursor).count();
        cursor = now;
    }
};

} // namespace nix
