/* Baseline-compatible slice of the full optimise-bench rig that
   lives on the `vibe-coding/optimise-and-gc-throughput` branch.
   Deliberately stripped to the APIs available at commit
   616df9797 (prior to the parallel-optimise / sharded-links /
   io_uring-GC / per-thread-gc-socket / batched-invalidate work).

   What's kept:
     - ThrottleGate (VM-side dynamic throttle handshake)
     - timedCall (manual-time wall-clock around the measured call)
     - truncateBenchTempRoots (so pre-optimise warm-up doesn't pin
       every path as a live temp root before the GC bench runs)
     - optimise  — measures LocalStore::optimiseStore(OptimiseStats&)
     - gc_barabasi / gc_clusters — measure LocalStore::collectGarbage
       over the BA and Clusters fixture topologies respectively
     - optimise_with_concurrent_gc — runs optimise + GC simultaneously
       on the same store. Included deliberately: the per-thread
       gc-socket fix on the comparison branch was written *for*
       this workload, and at the baseline commit the shared
       Sync<AutoCloseFD> in `addTempRoot` is the dominant cost.
       Excluding it would understate the improvement on a real
       user-visible workflow (concurrent build+GC). Threads are
       inert here (no optimise-threads / gc-links-threads settings
       at this commit), so the bench-side concurrency is what we
       can express: two std::threads, single-worker each.

   What's dropped vs the full rig:
     - Variant enum (no sharded layout / replica cap at baseline)
     - Dispatch enum (no io_uring unlink path at baseline)
     - applyVariant (no matching settings)
     - SettingsGuard body (nothing to save/restore)
     - Per-stage Timings counters (OptimiseStats / GCResults have no
       Timings substructs at baseline)
     - optimise_migrate (sharded layout absent — measures a
       migration step that doesn't exist at this commit)
     - invalidate_paths (invalidatePathsChecked not exposed at
       baseline)

   Capture-name shape: plain `BENCHMARK(foo)->Args({N, T})` produces
   `foo/N/T/manual_time`. The rig's `bench.py` filter builder on the
   baseline worktree must match this exact shape — no
   `<dispatch>_<variant>` prefix, since there is no variant axis. */

#include <benchmark/benchmark.h>

#include "nix/store/derivations.hh"
#include "nix/store/globals.hh"
#include "nix/store/local-store.hh"
#include "nix/store/store-open.hh"
#include "nix/util/file-system.hh"
#include "nix/util/hash.hh"

#include <chrono>

#ifndef _WIN32

#  include "bench-fixture.hh"
#  include "nix/util/file-descriptor.hh"

#  include <fcntl.h>
#  include <sys/stat.h>
#  include <unistd.h>

#  include <algorithm>
#  include <atomic>
#  include <cstdint>
#  include <cstdlib>
#  include <filesystem>
#  include <mutex>
#  include <string>
#  include <string_view>
#  include <thread>

using namespace nix;
using namespace nix::bench;

namespace {

/* Serialise all benchmarks in this file. google-benchmark runs
   serially by default; this guards against a future parallel runner
   racing the shared BenchFixture tmpdir / global settings. */
std::mutex benchSerialiseMutex;

} // namespace

/* RAII guard for the dynamic throttle-gating protocol. Construction
   asks the VM-side `bench-throttle-daemon` (see
   `tests/nixos/nix-store-bench/throttle-daemon.sh`) to apply the
   configured cgroup I/O limits + dm-delay, blocking until the daemon
   acks. Destruction asks the daemon to clear them. No-op when the
   gate is disabled (`NIX_BENCH_THROTTLE_GATE` unset or "0"), so
   non-throttle cells pay zero handshake cost. */
class ThrottleGate
{
    static constexpr const char * kOnReq = "/tmp/.bench_throttle_on";
    static constexpr const char * kOnAck = "/tmp/.bench_throttle_ack_on";
    static constexpr const char * kOffReq = "/tmp/.bench_throttle_off";
    static constexpr const char * kOffAck = "/tmp/.bench_throttle_ack_off";

    bool gated;

    static bool envEnabled()
    {
        static const bool v = [] {
            auto * env = std::getenv("NIX_BENCH_THROTTLE_GATE");
            return env && *env && std::string_view(env) != "0";
        }();
        return v;
    }

    static void touch(const char * path)
    {
        int fd = ::open(path, O_WRONLY | O_CREAT, 0644);
        if (fd >= 0)
            ::close(fd);
    }

    static void waitAndConsumeAck(const char * path)
    {
        constexpr auto timeout = std::chrono::seconds(30);
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (::access(path, F_OK) != 0) {
            if (std::chrono::steady_clock::now() > deadline)
                throw Error(
                    "throttle gate: marker %s not seen within 30 s — is "
                    "`bench-throttle-daemon` running?",
                    path);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ::unlink(path);
    }

public:
    ThrottleGate() : gated(envEnabled())
    {
        if (gated) {
            touch(kOnReq);
            waitAndConsumeAck(kOnAck);
            ::unlink(kOnReq);
        }
    }

    ~ThrottleGate()
    {
        if (!gated)
            return;
        touch(kOffReq);
        try {
            waitAndConsumeAck(kOffAck);
        } catch (...) {
            /* Dtor mustn't throw. */
        }
        ::unlink(kOffReq);
    }

    ThrottleGate(const ThrottleGate &) = delete;
    ThrottleGate & operator=(const ThrottleGate &) = delete;
};

/* Time the measured call with `steady_clock`, install the elapsed
   nanos on `state` via `SetIterationTime` (required under
   `UseManualTime`), and return the elapsed nanos. The ThrottleGate
   RAII is deliberately outside the timing window. */
template <typename F>
static uint64_t timedCall(benchmark::State & state, F && f)
{
    ThrottleGate gate;

    auto t0 = std::chrono::steady_clock::now();
    f();
    auto t1 = std::chrono::steady_clock::now();

    uint64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    state.SetIterationTime(static_cast<double>(ns) / 1.0e9);
    return ns;
}

static void reportTotalMs(benchmark::State & state, uint64_t totalCallSumNs)
{
    const double iters = static_cast<double>(std::max<int64_t>(state.iterations(), 1));
    state.counters["t_total_ms"] = (totalCallSumNs / iters) / 1.0e6;
}

/* `optimiseStore` writes every visited path into `temproots/<pid>`,
   so a warm-up `optimiseStore` pins every path against a later GC
   in the same process. Truncate between warm-up and timed GC. */
static void truncateBenchTempRoots(const std::filesystem::path & root)
{
    auto tempRootsFile = root / "nix" / "var" / "nix"
                         / "temproots" / std::to_string(::getpid());
    if (!std::filesystem::exists(tempRootsFile))
        return;
    int fd = ::open(tempRootsFile.c_str(), O_WRONLY);
    if (fd < 0)
        return;
    if (::ftruncate(fd, 0) == -1) {
        int e = errno;
        ::close(fd);
        throw SysError(e, "truncating bench temproots file");
    }
    ::close(fd);
}

/* ---------- optimise ---------------------------------------------
 *
 * Single `optimiseStore()` run over a store of `nPaths` × 10 files.
 * Baseline: serial; `threads` arg is carried in the capture name
 * purely so result JSONs are comparable to the current-branch
 * runs on the rig. */
static void optimise(benchmark::State & state)
{
    std::lock_guard benchLock(benchSerialiseMutex);

    const size_t nPaths = state.range(0);
    const size_t nShared = 10;
    /* threads (state.range(1)) is recorded in the capture name for
       host-side A/B parity — baseline has no optimise-threads knob
       to apply it to. */

    uint64_t totalCallSumNs = 0;
    for (auto _ : state) {
        state.PauseTiming();
        BenchFixture fixture(nPaths, nShared);
        state.ResumeTiming();

        OptimiseStats stats;
        totalCallSumNs += timedCall(state, [&] {
            fixture.local().optimiseStore(stats);
        });

        state.PauseTiming();
        benchmark::DoNotOptimize(stats.filesLinked);
        benchmark::DoNotOptimize(stats.bytesFreed);
    }

    reportTotalMs(state, totalCallSumNs);
    state.SetItemsProcessed(state.iterations() * nPaths * nShared);
}

BENCHMARK(optimise)
    ->Args({2000, 4})
    ->Args({10000, 4})
    ->Args({50000, 4})
    ->Unit(benchmark::kMillisecond)
    ->UseManualTime();

/* ---------- optimise_with_concurrent_gc -------------------------
 *
 * Runs `optimiseStore()` and `collectGarbage()` simultaneously in
 * the same process, two `std::thread`s, single-worker each. At
 * 616df9797 there are no optimise-threads / gc-links-threads
 * settings, so the (threads, threads2) cell axes only survive in
 * the BENCHMARK arg list and the result-name suffix for host-side
 * A/B parity with the comparison branch.
 *
 * Why this is in the baseline at all: every `addTempRoot` call from
 * the optimise side blocks on a single `Sync<AutoCloseFD>` to the
 * GC server, and at this commit the GC server is the same process
 * doing the deletion. Concurrent optimise+GC therefore serialises
 * on the temp-root socket — the exact contention the comparison
 * branch's per-thread-gc-socket rewrite eliminates. Measuring it
 * here gives the A/B a fair "before" number for the workload that
 * change was written to fix. (Cross-process optimise vs in-process
 * GC pays the same round-trip and is also slow at this commit, but
 * isn't exercised by this bench.)
 *
 * `gc_threw` / `opt_threw` counters: at baseline the
 * concurrent-modification race can throw on either side (GC sees
 * a path disappear mid-walk; optimise sees a link target vanish).
 * Tolerating the throw and surfacing the count keeps a fast row
 * caused by an exception-shortened inner loop from masquerading as
 * a measured speed-up — `bench.py`'s `_throws_marker` flags any
 * non-zero count with `!`.
 */
static void optimise_with_concurrent_gc(benchmark::State & state)
{
    std::lock_guard benchLock(benchSerialiseMutex);

    const size_t nPaths = state.range(0);
    /* threads (state.range(1)) and threads2 (state.range(2)) are
       recorded in the capture-name only — see header note. */

    uint64_t totalCallSumNs = 0;
    uint64_t gcThrowCount = 0;
    uint64_t optThrowCount = 0;
    for (auto _ : state) {
        state.PauseTiming();
        BenchFixture fixture(nPaths, /*nShared=*/8);

        /* Pre-optimise so the timed concurrent GC has dead paths to
           sweep — without this the GC's findRoots already covers
           every path and Phase-2 deletion is a no-op, defeating the
           contention story we're measuring. */
        {
            OptimiseStats warm;
            fixture.local().optimiseStore(warm);
        }
        truncateBenchTempRoots(fixture.root);

        std::atomic<bool> gcThrew{false};
        std::atomic<bool> optThrew{false};
        state.ResumeTiming();

        OptimiseStats stats;
        GCResults gcRes;
        totalCallSumNs += timedCall(state, [&] {
            std::thread gcThread([&] {
                GCOptions opts;
                opts.action = GCOptions::gcDeleteDead;
                try {
                    fixture.local().collectGarbage(opts, gcRes);
                } catch (...) {
                    /* See header comment on the throw counters. */
                    gcThrew.store(true, std::memory_order_relaxed);
                }
            });

            try {
                fixture.local().optimiseStore(stats);
            } catch (...) {
                optThrew.store(true, std::memory_order_relaxed);
            }

            gcThread.join();
        });

        state.PauseTiming();
        benchmark::DoNotOptimize(stats.bytesFreed);
        benchmark::DoNotOptimize(gcRes.bytesFreed);
        if (gcThrew.load(std::memory_order_relaxed))
            ++gcThrowCount;
        if (optThrew.load(std::memory_order_relaxed))
            ++optThrowCount;
    }

    reportTotalMs(state, totalCallSumNs);
    /* Always emit so 0 is a "clean run" signal rather than "bench
       forgot to populate the counter". */
    state.counters["gc_threw"] = static_cast<double>(gcThrowCount);
    state.counters["opt_threw"] = static_cast<double>(optThrowCount);
    state.SetItemsProcessed(state.iterations() * nPaths);
}

/* Args = (nPaths, threads, threads2). Symmetric (T, T) cells only
   at baseline — the asymmetric (16, 1) / (1, 16) cells from the
   full rig measure thread-count interactions that don't exist at
   this commit anyway. */
BENCHMARK(optimise_with_concurrent_gc)
    ->Args({2000, 4, 4})
    ->Args({10000, 4, 4})
    ->Args({50000, 4, 4})
    ->Unit(benchmark::kMillisecond)
    ->UseManualTime();

/* ---------- gc_barabasi / gc_clusters ----------------------------
 *
 * End-to-end GC on a fixture of `nPaths` store entries with an
 * auto-optimise warm-up so `.links/` is fully populated. Baseline
 * GC is serial; `threads` is carried in the capture name only. */
static void gc_collect(benchmark::State & state, FixtureSpec::Topology topology,
                       size_t clusterSize = 50)
{
    std::lock_guard benchLock(benchSerialiseMutex);

    const size_t nPaths = state.range(0);
    /* threads = state.range(1) — recorded but inert at baseline. */

    uint64_t totalCallSumNs = 0;
    for (auto _ : state) {
        state.PauseTiming();
        BenchFixture fixture(nPaths, /*avgFilesPerPath=*/10,
                             /*nRootsOverride=*/std::nullopt,
                             topology, clusterSize);
        {
            OptimiseStats warm;
            fixture.local().optimiseStore(warm);
        }
        truncateBenchTempRoots(fixture.root);
        state.ResumeTiming();

        /* bpftrace marker: see test_script.py's HAS_DISPATCH block.
           The syscalls are expected to fail with ENOENT — they exist
           solely as kernel-visible brackets around the timed call. */
        (void) ::open("/tmp/.bench_gc_start", O_RDONLY);

        GCOptions opts;
        opts.action = GCOptions::gcDeleteDead;
        GCResults res;
        totalCallSumNs += timedCall(state, [&] {
            fixture.local().collectGarbage(opts, res);
        });

        (void) ::open("/tmp/.bench_gc_end", O_RDONLY);

        state.PauseTiming();
        benchmark::DoNotOptimize(res.bytesFreed);
    }

    reportTotalMs(state, totalCallSumNs);
    state.SetItemsProcessed(state.iterations() * nPaths);
}

static void gc_barabasi(benchmark::State & state)
{
    gc_collect(state, FixtureSpec::Topology::Barabasi);
}

BENCHMARK(gc_barabasi)
    ->Args({2000, 4})
    ->Args({10000, 4})
    ->Args({50000, 4})
    ->Unit(benchmark::kMillisecond)
    ->UseManualTime();

static void gc_clusters(benchmark::State & state)
{
    gc_collect(state, FixtureSpec::Topology::Clusters, /*clusterSize=*/50);
}

BENCHMARK(gc_clusters)
    ->Args({2000, 4})
    ->Args({10000, 4})
    ->Args({50000, 4})
    ->Unit(benchmark::kMillisecond)
    ->UseManualTime();

#endif
