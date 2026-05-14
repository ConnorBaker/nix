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

   What's dropped vs the full rig:
     - Variant enum (no sharded layout / replica cap at baseline)
     - Dispatch enum (no io_uring unlink path at baseline)
     - applyVariant (no matching settings)
     - SettingsGuard body (nothing to save/restore)
     - Per-stage Timings counters (OptimiseStats / GCResults have no
       Timings substructs at baseline)
     - optimise_migrate (sharded layout absent)
     - optimise_with_concurrent_gc (pre-dates per-thread gc-socket
       fix — reports 15x slowdown that isn't real-world)
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
