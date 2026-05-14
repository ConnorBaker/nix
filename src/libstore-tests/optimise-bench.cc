#include <benchmark/benchmark.h>

#include "nix/store/derivations.hh"
#include "nix/store/globals.hh"
#include "nix/store/local-store.hh"
#include "nix/store/store-open.hh"
#include "nix/util/environment-variables.hh"
#include "nix/util/experimental-features.hh"
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
#  include <cmath>
#  include <cstdint>
#  include <filesystem>
#  include <mutex>
#  include <string>
#  include <string_view>
#  include <thread>
#  include <vector>

using namespace nix;
using namespace nix::bench;

namespace {

/* RAII guard that snapshots a single `Setting<T>` value at
   construction and restores it on destruction. Composed inside
   `SettingsGuard` below to scope per-bench overrides. */
template <typename T>
struct SavedSetting
{
    Setting<T> & ref;
    T saved;

    explicit SavedSetting(Setting<T> & s) : ref(s), saved(s.get()) {}
    ~SavedSetting() { ref.override(saved); }

    SavedSetting(const SavedSetting &) = delete;
    SavedSetting & operator=(const SavedSetting &) = delete;
};

/* RAII guard that snapshots the thread-related `LocalSettings` and
   the `experimental-features` set at construction and restores them
   at destruction. Keeps each benchmark function call self-contained
   so it leaves no state for the next one under google-benchmark's
   default serial execution AND under
   `--benchmark_enable_random_interleaving`, which re-enters the
   function repeatedly.

   All benchmarks in this file mutate global `settings` because
   `LocalStore::config->getLocalSettings()` and the global
   `settings.getLocalSettings()` currently share backing state. The
   cleanest fix would be per-store config, but that is a larger
   refactor and orthogonal to the benchmark itself. */
struct SettingsGuard
{
    SavedSetting<size_t> optimiseThreads{settings.getLocalSettings().optimiseThreads};
    SavedSetting<size_t> gcLinksThreads{settings.getLocalSettings().getGCSettings().gcLinksThreads};
    SavedSetting<bool> gcLinksUseIoUring{settings.getLocalSettings().getGCSettings().gcLinksUseIoUring};
    SavedSetting<size_t> maxLinkReplicas{settings.getLocalSettings().maxLinkReplicas};
    SavedSetting<std::set<ExperimentalFeature>> xpFeatures{experimentalFeatureSettings.experimentalFeatures};

    /* Snapshot+restore of `NIX_TEST_LINK_MAX_OVERRIDE` (read once by
       `LocalStore`'s ctor). Preserves the absent/present distinction
       so a bench that didn't set the var doesn't leak a value into
       the next one. */
    struct SavedEnv
    {
        std::optional<std::string> saved = getEnv("NIX_TEST_LINK_MAX_OVERRIDE");
        ~SavedEnv()
        {
            if (saved)
                setEnv("NIX_TEST_LINK_MAX_OVERRIDE", saved->c_str());
            else
                ::unsetenv("NIX_TEST_LINK_MAX_OVERRIDE");
        }
    } savedLinkMaxOverride;

    /* Helper to toggle an experimental feature in/out of the live
       `experimentalFeatures` set. `Xp::ShardedLinks` is read at
       LocalStore construction time, so callers must invoke this
       *before* constructing the BenchFixture. */
    static void setXpFeature(ExperimentalFeature feature, bool enabled)
    {
        auto features = experimentalFeatureSettings.experimentalFeatures.get();
        if (enabled)
            features.insert(feature);
        else
            features.erase(feature);
        experimentalFeatureSettings.experimentalFeatures.override(features);
    }

    SettingsGuard() = default;
    SettingsGuard(const SettingsGuard &) = delete;
    SettingsGuard & operator=(const SettingsGuard &) = delete;
};

/* Serialise all benchmarks in this file against any future parallel
   benchmark runner. google-benchmark's current behaviour is fully
   serial, so this mutex is contention-free today — but it provides a
   durable guarantee so adding `->Threads(N)` on a bench later cannot
   silently introduce global-settings races. */
static std::mutex benchSerialiseMutex;

} // namespace

/* The fixture uses `$TMPDIR` (typically tmpfs) for the store root,
   so these benchmarks measure the CPU- and kernel-VFS-bound ceiling
   of each operation, not real-world wall-clock on a block-device
   store. Run on a real backing FS to measure that; see the bench
   README's "Findings" section for the tmpfs-vs-ext4 differences and
   the canonical scaling-curve shape. */

/* ---------- optimise ------------------------------------------
 *
 * Measures the wall-clock of a single `optimiseStore()` run over a
 * store of `nPaths` × `nShared` files. Varies the `optimise-threads`
 * setting to expose parallel speedup.
 *
 * This is the bench that most directly exercises the sharded
 * `.links/` and replica-spillover paths — they live inside
 * `optimisePath_`, which is the hot loop here.
 *
 * Expected shape (baseline column, useSharded=0):
 *   - 2–3× speedup at N=4 on tmpfs; flat or regression beyond N=4
 *     due to `.links/` directory rwsem contention.
 *   - On real ext4 with IOPS headroom, scaling continues further.
 *
 * Expected deltas vs the baseline column:
 *   - useSharded=1: pays a one-shot `migrateLinksDirToSharded()`
 *     cost (negligible on a fresh store). Per-file overhead is
 *     within noise. The interesting cell is large + ext4 where
 *     the replica walk absorbs hardlink-ceiling spills that the
 *     flat layout would silently drop. The bench measures *time*,
 *     but the OptimiseStats counters (filesLinked / bytesFreed)
 *     also differ — without sharded-links on ext4, ~85k of the
 *     fixture's 500k files (50k path size) end up undeduped.
 */
/* Per-stage timing descriptors. Each entry pairs the bench counter
   name (rendered alongside `Time` in google-benchmark's output) with
   the matching field in the `Timings` struct. Sums accumulate across
   all iterations in the function invocation; `reportStageCounters`
   divides by `state.iterations()` to emit per-iteration *mean* stage
   times in milliseconds (matching every bench's
   `Unit(kMillisecond)`). With `--benchmark_repetitions=N`, the harness
   further aggregates these into `_mean`/`_median`/`_stddev`/`_cv`. */
template <typename T>
struct StageField
{
    const char * counterName;
    uint64_t T::* field;
};

constexpr StageField<OptimiseStats::Timings> optimiseStages[] = {
    {"t_setup_ms", &OptimiseStats::Timings::setupNs},
    {"t_migrate_ms", &OptimiseStats::Timings::migrateLinksNs},
    {"t_query_ms", &OptimiseStats::Timings::queryAllPathsNs},
    {"t_load_ihash_ms", &OptimiseStats::Timings::loadInodeHashNs},
    {"t_parallel_ms", &OptimiseStats::Timings::parallelOptimiseNs},
};

constexpr StageField<GCResults::Timings> gcStages[] = {
    {"t_roots_ms", &GCResults::Timings::findRootsNs},
    {"t_load_paths_ms", &GCResults::Timings::loadValidPathsNs},
    {"t_traverse_ms", &GCResults::Timings::traverseAndPhase1Ns},
    {"t_phase2_ms", &GCResults::Timings::phase2DeleteNs},
    {"t_cleanup_ms", &GCResults::Timings::cleanupLinksNs},
};

template <typename T, typename Stages>
void accumulateStageTimings(T & sum, const T & t, const Stages & stages)
{
    for (auto & s : stages)
        sum.*(s.field) += t.*(s.field);
}

template <typename T, typename Stages>
void reportStageCounters(benchmark::State & state, const T & sum, const Stages & stages)
{
    const double iters = static_cast<double>(std::max<int64_t>(state.iterations(), 1));
    for (auto & s : stages)
        state.counters[s.counterName] = (sum.*(s.field) / iters) / 1.0e6;
}

/* RAII guard for the dynamic throttle-gating protocol. Construction
   asks the VM-side `bench-throttle-daemon` (see
   `tests/nixos/nix-store-bench/throttle-daemon.sh`) to apply the
   configured cgroup I/O limits + dm-delay, blocking until the daemon
   acks. Destruction asks the daemon to clear them, blocking until ack
   (best-effort; the dtor swallows the timeout exception). The two
   "sides" of the handshake therefore bracket the measured call
   exactly: fixture build / pre-warm / cleanup all run unthrottled.

   No-op when the gate is disabled (`NIX_BENCH_THROTTLE_GATE` unset or
   set to "0"), so non-throttle cells pay zero handshake cost.

   Synchronisation is via touch/poll marker files in `/tmp` (tmpfs):
     - bench creates `<req>` → daemon sees it, applies, creates `<ack>`
     - bench sees `<ack>`, unlinks `<ack>` and `<req>`
   Polled at 1 ms (sub-noise vs the ~s measurements). The 30 s timeout
   surfaces a dead/missing daemon as a loud `Error` rather than an
   indefinite hang — observed during development when the daemon's
   PATH was wrong and it exited with code=127 before the first
   handshake. */
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
            /* Dtor mustn't throw. If the off-ack times out we accept
               a leaked-throttled state; the bench process will exit
               shortly and the throttle dies with the cgroup. */
        }
        ::unlink(kOffReq);
    }

    ThrottleGate(const ThrottleGate &) = delete;
    ThrottleGate & operator=(const ThrottleGate &) = delete;
};

/* Time the measured call with `steady_clock`, install the elapsed
   nanos on `state` via `SetIterationTime` (required under
   `UseManualTime` — `PauseTiming` empirically leaks fixture-build
   wall into the harness-measured `Time` column otherwise), and
   return the elapsed nanos so the caller can sum them across
   iterations for the `t_total_ms` counter.

   The `ThrottleGate` RAII guard brackets the call with the dynamic
   throttle handshake (no-op when `NIX_BENCH_THROTTLE_GATE` is unset).
   It's deliberately constructed *outside* the timing window so the
   handshake cost isn't charged to the measured call. */
template <typename F>
uint64_t timedCall(benchmark::State & state, F && f)
{
    ThrottleGate gate;

    auto t0 = std::chrono::steady_clock::now();
    f();
    auto t1 = std::chrono::steady_clock::now();

    uint64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    state.SetIterationTime(static_cast<double>(ns) / 1.0e9);
    return ns;
}

/* Emit the per-iteration mean of the bench's wall-clock total
   into `state.counters["t_total_ms"]`. Pairs with `timedCall`. */
static void reportTotalMs(benchmark::State & state, uint64_t totalCallSumNs)
{
    const double iters = static_cast<double>(std::max<int64_t>(state.iterations(), 1));
    state.counters["t_total_ms"] = (totalCallSumNs / iters) / 1.0e6;
}

/* `optimiseStore` writes every visited path into `temproots/<pid>`
   via `addTempRoot` (see optimise-store.cc), so any warm-up
   `optimiseStore` call pins every path against a *subsequent* GC
   in the same process. Benches that pre-optimise to populate
   `.links/` and then time a `collectGarbage` need to truncate
   the temproots file in between — otherwise GC sees every path
   as a temp root and Phase-2 deletion becomes a no-op, silently
   zeroing the most important GC signal. The only intentional
   roots are the permanent `addPermRoot` entries written by
   `BenchFixture`'s ctor. */
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

/* Layout × replica variant. Captured by `BENCHMARK_CAPTURE` so
   each combination produces a separately-named benchmark like
   `optimise/sharded_multi_replica_hardlink/2000/4` instead of an
   opaque numeric suffix like `.../2000/4/1`. Makes
   `--benchmark_filter` patterns self-documenting and avoids the
   need for downstream tooling to translate flag tuples back into
   mechanism names.

   The two axes are orthogonal in the implementation, so all four
   combinations are independently expressible:

     layout    replica-cap     tag                                  note
     --------  -------------   -----------------------------------  ----------------
     flat      1 (single)      flat_single_replica_hardlink         pre-`fffca655b` baseline
     flat      100 (multi)     flat_multi_replica_hardlink          replica spill without sharding
     sharded   1 (single)      sharded_single_replica_hardlink      sharding without replica spill
     sharded   100 (multi)     sharded_multi_replica_hardlink       both axes together

   The bench rig does not ship reflink-based dedup — that was
   investigated as a candidate third axis but rejected; see the
   bench README's "Rejected: CoW reflinks" section. All dedup goes
   via `link(2)`. */
enum class Variant {
    FlatSingleReplicaHardlink,
    FlatMultiReplicaHardlink,
    ShardedSingleReplicaHardlink,
    ShardedMultiReplicaHardlink,
};

/* Variant settings table. Each row is the (sharded, replicaCap,
   linkMaxOverride) tuple to install on the matching `Variant`
   enumerator. The `linkMaxOverride` value is exported via
   `NIX_TEST_LINK_MAX_OVERRIDE`; `=100` on the multi-replica cells
   forces the replica-spill path on test filesystems with
   effectively-infinite hardlink ceilings (tmpfs/ZFS). Rows MUST
   follow the `Variant` enum's declaration order; the static_assert
   below catches drift. */
struct VariantDescriptor
{
    bool sharded;
    size_t replicaCap;
    int64_t linkMaxOverride;
};

constexpr VariantDescriptor variantDescriptors[] = {
    /* Variant::FlatSingleReplicaHardlink    */ {false, 1,   0},
    /* Variant::FlatMultiReplicaHardlink     */ {false, 100, 100},
    /* Variant::ShardedSingleReplicaHardlink */ {true,  1,   100},
    /* Variant::ShardedMultiReplicaHardlink  */ {true,  100, 100},
};

static_assert(
    static_cast<size_t>(Variant::ShardedMultiReplicaHardlink) + 1 == std::size(variantDescriptors),
    "variantDescriptors must have one row per Variant enumerator, in declaration order");

static const VariantDescriptor & variantInfo(Variant v)
{
    return variantDescriptors[static_cast<size_t>(v)];
}

/* GC dispatch axis. Independent of the layout × dedup space above
   — used by `gc_barabasi` and `gc_clusters` via a second
   `BENCHMARK_CAPTURE` argument. */
enum class Dispatch {
    Syscall,  // thread-pool, plain blocking syscalls
    IoUring,  // io_uring batched STATX + UNLINKAT
};

/* Apply a `Variant` to the live settings. Toggles
   `Xp::ShardedLinks`, `max-link-replicas`, and
   `NIX_TEST_LINK_MAX_OVERRIDE` in a single call so caller bench
   bodies don't have to remember the coupling rules. Call *before*
   constructing the BenchFixture — `Xp::ShardedLinks` and the env
   var are both read at LocalStore open time. */
static void applyVariant(Variant v)
{
    const auto & d = variantInfo(v);
    SettingsGuard::setXpFeature(Xp::ShardedLinks, d.sharded);
    settings.getLocalSettings().maxLinkReplicas.override(d.replicaCap);
    if (d.linkMaxOverride > 0)
        setEnv("NIX_TEST_LINK_MAX_OVERRIDE", std::to_string(d.linkMaxOverride).c_str());
    else
        unsetenv("NIX_TEST_LINK_MAX_OVERRIDE");
}

static void optimise(benchmark::State & state, Variant variant)
{
    std::lock_guard benchLock(benchSerialiseMutex);
    SettingsGuard settingsGuard;

    const size_t nPaths = state.range(0);
    const size_t nShared = 10;
    const size_t threads = state.range(1);

    /* Accumulate stage timings across iterations so the reported
       counters are per-iteration means rather than the last
       iteration's snapshot. `reportStageCounters` divides by
       `state.iterations()` at the end. */
    OptimiseStats::Timings sumTimings;
    /* Wall-clock around the `optimiseStore` call only. Sums to
       `t_total_ms`. Difference between that and the sum of per-stage
       counters is "uncharged" work inside `optimiseStore` (Activity
       ctor/dtor, parallel-arena teardown, etc.). */
    uint64_t totalCallSumNs = 0;
    for (auto _ : state) {
        state.PauseTiming();
        applyVariant(variant);
        BenchFixture fixture(nPaths, nShared);
        settings.getLocalSettings().optimiseThreads.override(threads);
        state.ResumeTiming();

        OptimiseStats stats;
        totalCallSumNs += timedCall(state, [&] {
            fixture.local().optimiseStore(stats);
        });

        state.PauseTiming();
        benchmark::DoNotOptimize(stats.bytesFreed.load());
        benchmark::DoNotOptimize(stats.filesLinked.load());
        accumulateStageTimings(sumTimings, stats.timings, optimiseStages);
    }

    reportStageCounters(state, sumTimings, optimiseStages);
    reportTotalMs(state, totalCallSumNs);
    state.SetItemsProcessed(state.iterations() * nPaths * nShared);
}

/* (Args = `(nPaths, optimise-threads)`).
 *
 * Two-tier matrix per variant:
 *   - The thread-scaling profile (1/4/16 threads) is run on the
 *     baseline (`flat_single_replica_hardlink`) variant for the
 *     parallel-speedup story.
 *   - The other variants run only at threads=4 (the post-knee
 *     count on tmpfs) across all sizes — they're orthogonal to
 *     thread scaling, and concentrating iterations on fewer cells
 *     beats single-shot runs across many.
 *
 * Filter patterns (with `--benchmark_filter`):
 *   - `optimise/flat_single_replica_hardlink/`          — all baseline
 *   - `optimise/sharded_multi_replica_hardlink/2000/4`  — one cell
 *   - `optimise/sharded_(single|multi)_replica_hardlink/` — sharded × replica axis
 */
/* `UseManualTime()` makes the harness use `SetIterationTime`
   from inside the loop rather than measuring the
   ResumeTiming→PauseTiming wall itself. Critical because the
   bench fixture's construction lives in a `PauseTiming` block
   but empirically still leaks into the harness-measured wall;
   `UseManualTime` cuts it cleanly. */
BENCHMARK_CAPTURE(optimise, flat_single_replica_hardlink, Variant::FlatSingleReplicaHardlink)
    ->Args({200, 1})
    ->Args({200, 4})
    ->Args({200, 16})
    ->Args({2000, 1})
    ->Args({2000, 4})
    ->Args({2000, 16})
    ->Args({10000, 1})
    ->Args({10000, 4})
    ->Args({10000, 16})
    /* 50 k: only the high-thread cases — single-threaded runs at
       this size are dominated by fixture build, not signal. */
    ->Args({50000, 4})
    ->Args({50000, 16})
    ->Unit(benchmark::kMillisecond)
    ->UseManualTime();

BENCHMARK_CAPTURE(optimise, flat_multi_replica_hardlink,
                  Variant::FlatMultiReplicaHardlink)
    ->Args({2000, 4})
    ->Args({10000, 4})
    ->Args({50000, 4})
    ->Unit(benchmark::kMillisecond)
    ->UseManualTime();

BENCHMARK_CAPTURE(optimise, sharded_single_replica_hardlink,
                  Variant::ShardedSingleReplicaHardlink)
    ->Args({2000, 4})
    ->Args({10000, 4})
    ->Args({50000, 4})
    ->Unit(benchmark::kMillisecond)
    ->UseManualTime();

BENCHMARK_CAPTURE(optimise, sharded_multi_replica_hardlink,
                  Variant::ShardedMultiReplicaHardlink)
    ->Args({2000, 4})
    ->Args({10000, 4})
    ->Args({50000, 4})
    ->Unit(benchmark::kMillisecond)
    ->UseManualTime();

/* ---------- optimise_migrate -----------------------------------
 *
 * Exercises the one-shot `migrateLinksDirToSharded()` path inside
 * `optimiseStore()`. None of the other `optimise` cells reach
 * it: they all run on a freshly-created store, so the `.links/`
 * directory is empty (or sharded from inception) and migration finds
 * nothing to move.
 *
 * Pattern:
 *
 *   1. Build the fixture with `Xp::ShardedLinks` *off* (legacy flat
 *      layout). Run `optimiseStore()` to populate `.links/<H>` flat
 *      entries — one per deduplicated content hash. The
 *      `NIX_TEST_LINK_MAX_OVERRIDE` env var is left unset here, so
 *      the flat layout's EMLINK ceiling is the host's pathconf
 *      value (tmpfs ≈ INT_MAX,
 *      ext4 = 65 000); on tmpfs/ZFS the dedup is total, so we get
 *      one flat entry per *distinct* (blob, replica) tuple.
 *   2. Drop the legacy LocalStore handle and re-open the same
 *      directory with `Xp::ShardedLinks` *on*. The new handle's
 *      constructor sees a non-empty flat `.links/` and a freshly
 *      created set of (empty) shard subdirectories.
 *   3. Time `optimiseStore()` on the re-opened handle. The first
 *      stage inside `optimiseStore` is `migrateLinksDirToSharded()`,
 *      which is the work this bench targets. Subsequent stages
 *      (`queryAllPaths`, `loadInodeHash`, `parallelOptimise`) still
 *      run, but with the dedup already done they only verify
 *      canonical-association — far cheaper than the migration itself.
 *
 * The `t_migrate_ms` counter on each row isolates the migration time
 * from the rest of the optimise pipeline. Expect it to dominate the
 * total at sizes where the flat-`.links/` population is large enough
 * (≥ 2 k paths × 10 files × ~60% dedup ≈ 12 k flat entries to
 * rename).
 *
 * Args = (nPaths, optimise-threads). Migration is currently
 * single-threaded (renames are sequential), so the `threads` axis
 * only affects the post-migration verification phase; useful as a
 * sanity check that `t_migrate_ms` does not move with threads while
 * `t_parallel_ms` does.
 */
static void optimise_migrate(benchmark::State & state)
{
    std::lock_guard benchLock(benchSerialiseMutex);
    SettingsGuard settingsGuard;

    const size_t nPaths = state.range(0);
    const size_t threads = state.range(1);

    OptimiseStats::Timings sumTimings;
    uint64_t totalCallSumNs = 0;
    for (auto _ : state) {
        state.PauseTiming();

        /* Phase 1: flat-layout pre-population. */
        applyVariant(Variant::FlatSingleReplicaHardlink);
        BenchFixture fixture(nPaths, /*nShared=*/10);
        settings.getLocalSettings().optimiseThreads.override(threads);
        {
            OptimiseStats warm;
            fixture.local().optimiseStore(warm);
        }

        /* Phase 2: drop the flat-mode LocalStore handle *before*
           opening the sharded one. `fixture.store = openStore(uri)`
           evaluates the RHS first and would have two LocalStore
           instances on the same directory live simultaneously —
           both holding the big-lock shared, both with SQLite open,
           and the old one still owning its `temproots/<pid>` write
           lock. The current code paths don't break under this
           overlap, but it's a trap for any future ctor change that
           opens shared resources eagerly. Step-by-step:
             1. Move the underlying shared_ptr out of `fixture.store`
                into a local `tmp`.
             2. `tmp.reset()` drops the last strong reference,
                running ~LocalStore (releasing locks, closing fds).
             3. Open the new sharded store and assign it back.
           The intermediate moved-from `fixture.store` is not
           touched between (1) and (3), so the broken ref invariant
           is never observable. */
        applyVariant(Variant::ShardedMultiReplicaHardlink);
        auto rootPath = fixture.root;
        {
            std::shared_ptr<Store> tmp = std::move(fixture.store).get_ptr();
            tmp.reset(); /* ~LocalStore runs here */
            fixture.store = openStore(fmt("local?root=%s", rootPath.string()));
        }

        state.ResumeTiming();

        OptimiseStats stats;
        totalCallSumNs += timedCall(state, [&] {
            fixture.local().optimiseStore(stats);
        });

        state.PauseTiming();
        benchmark::DoNotOptimize(stats.bytesFreed.load());
        benchmark::DoNotOptimize(stats.filesLinked.load());
        accumulateStageTimings(sumTimings, stats.timings, optimiseStages);
    }

    reportStageCounters(state, sumTimings, optimiseStages);
    reportTotalMs(state, totalCallSumNs);
    state.SetItemsProcessed(state.iterations() * nPaths);
}

BENCHMARK(optimise_migrate)
    ->Args({2000, 1})
    ->Args({2000, 4})
    ->Args({10000, 4})
    ->Args({50000, 4})
    ->Unit(benchmark::kMillisecond)
    ->UseManualTime();

/* ---------- optimise_with_concurrent_gc -----------------------------------
 *
 * Runs `optimiseStore()` and `collectGarbage()` simultaneously in the
 * same process. Each operation is pinned to its own thread count via
 * Args({nPaths, optThreads, gcThreads}) — asymmetric configurations
 * let us see how contention behaves when one side is oversubscribed.
 *
 * Historical note: before the per-thread gc-socket change in
 * `addTempRoot` (each worker thread owns its own connection via TBB's
 * `enumerable_thread_specific` per-thread slot in
 * `LocalStore::_fdRootsSockets` — an opaque TLS-backed key, not
 * `std::thread::id`), this
 * benchmark showed a 15× slowdown at (16, 16). All 16 optimise
 * workers serialised on a single `Sync<AutoCloseFD>` covering the
 * blocking write+read round-trip to the GC server. After the fix,
 * (16, 16) runs in ~24 ms on the 200-path fixture — a measured
 * ~135× improvement.
 *
 * Expected shape today:
 *   - (1, 1): serialised baseline.
 *   - (4, 4), (8, 8): near-linear progress per side.
 *   - (16, 16): both saturated; kernel VFS locks on `.links/` are
 *     the ceiling, not Nix-side locks.
 *   - (16, 1): whether a heavy optimise starves GC.
 *   - (1, 16): opposite — whether a heavy GC starves optimise.
 *
 * Note: this measures intra-process contention. Cross-process
 * contention is not exercised here; inter-process optimise+GC will
 * still pay the gc-socket connect round-trip once per client-side
 * process, but no longer once per worker.
 */
static void optimise_with_concurrent_gc(benchmark::State & state, Variant variant)
{
    std::lock_guard benchLock(benchSerialiseMutex);
    SettingsGuard settingsGuard;

    const size_t nPaths = state.range(0);
    const size_t optThreads = state.range(1);
    const size_t gcThreads = state.range(2);

    OptimiseStats::Timings sumOpt;
    GCResults::Timings sumGC;
    uint64_t totalCallSumNs = 0;
    uint64_t gcThrowCount = 0;
    uint64_t optThrowCount = 0;
    uint64_t totalBytesFreed = 0;
    for (auto _ : state) {
        state.PauseTiming();
        applyVariant(variant);
        BenchFixture fixture(nPaths, /*nShared=*/8);
        settings.getLocalSettings().optimiseThreads.override(optThreads);
        settings.getLocalSettings().getGCSettings().gcLinksThreads.override(gcThreads);

        /* Pre-optimise so that the concurrent GC phase has something
           to sweep. We time the _second_ optimise + a concurrent GC. */
        {
            OptimiseStats warm;
            fixture.local().optimiseStore(warm);
        }
        /* The warm-up registered every path as a temp root; clear
           that before the timed region so the bench's concurrent GC
           sees only the temp roots the in-flight optimise actually
           adds during contention. Without this, GC's findRoots scan
           starts from "everything live" and Phase-2 deletion is a
           no-op — defeating the contention story. */
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
                    /* GC may fail cleanly if the store is simultaneously
                       being modified; we tolerate that here — the point
                       is to measure contention — but expose the failure
                       via a counter so a regression that makes one side
                       throw cannot masquerade as a speed-up. */
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
        benchmark::DoNotOptimize(stats.bytesFreed.load());
        totalBytesFreed += gcRes.bytesFreed;
        accumulateStageTimings(sumOpt, stats.timings, optimiseStages);
        accumulateStageTimings(sumGC, gcRes.timings, gcStages);
        if (gcThrew.load(std::memory_order_relaxed))
            ++gcThrowCount;
        if (optThrew.load(std::memory_order_relaxed))
            ++optThrowCount;
    }

    /* Report optimise- and GC-side stage timings, prefixed so
       consumers can attribute regressions to one half vs the other.
       The stage-counter names live in `optimiseStages` / `gcStages`
       with a `t_` prefix that we re-target to `t_opt_` / `t_gc_`. */
    const double iters = static_cast<double>(std::max<int64_t>(state.iterations(), 1));
    for (const auto & f : optimiseStages) {
        std::string raw = f.counterName; /* "t_setup_ms" etc. */
        std::string renamed = std::string("t_opt_") + raw.substr(2);
        state.counters[renamed] = (sumOpt.*(f.field) / iters) / 1.0e6;
    }
    for (const auto & f : gcStages) {
        std::string raw = f.counterName;
        std::string renamed = std::string("t_gc_") + raw.substr(2);
        state.counters[renamed] = (sumGC.*(f.field) / iters) / 1.0e6;
    }
    state.counters["gc_bytes_freed_mb"] =
        static_cast<double>(totalBytesFreed) / iters / (1024.0 * 1024.0);
    reportTotalMs(state, totalCallSumNs);
    /* Always emit the throw counters — value 0 on a clean run, > 0
       signals a regression. A missing field would otherwise mean
       "bench-side bug in the counter emission", not "clean run". */
    state.counters["gc_threw"] = static_cast<double>(gcThrowCount);
    state.counters["opt_threw"] = static_cast<double>(optThrowCount);
    state.SetItemsProcessed(state.iterations() * nPaths);
}

/* Args = `(nPaths, optimise-threads, gc-links-threads)`.
 *
 * Variants: same `Variant` enum as `optimise`. Both halves
 * of the concurrent pair (optimise + GC) operate on a store laid
 * out by the variant, so layout/dedup contention shows up
 * symmetrically. */
BENCHMARK_CAPTURE(optimise_with_concurrent_gc, flat_single_replica_hardlink, Variant::FlatSingleReplicaHardlink)
    ->Args({200, 1, 1})
    ->Args({200, 4, 4})
    ->Args({200, 16, 16})
    ->Args({2000, 1, 1})
    ->Args({2000, 4, 4})
    ->Args({2000, 16, 16})
    ->Args({2000, 16, 1})
    ->Args({2000, 1, 16})
    ->Args({10000, 4, 4})
    ->Args({10000, 16, 16})
    ->Args({50000, 4, 4})
    ->Args({50000, 16, 16})
    ->Unit(benchmark::kMillisecond)
    ->UseManualTime();

BENCHMARK_CAPTURE(optimise_with_concurrent_gc, flat_multi_replica_hardlink,
                  Variant::FlatMultiReplicaHardlink)
    ->Args({2000, 4, 4})
    ->Args({10000, 4, 4})
    ->Args({50000, 4, 4})
    ->Unit(benchmark::kMillisecond)
    ->UseManualTime();

BENCHMARK_CAPTURE(optimise_with_concurrent_gc, sharded_single_replica_hardlink,
                  Variant::ShardedSingleReplicaHardlink)
    ->Args({2000, 4, 4})
    ->Args({10000, 4, 4})
    ->Args({50000, 4, 4})
    ->Unit(benchmark::kMillisecond)
    ->UseManualTime();

BENCHMARK_CAPTURE(optimise_with_concurrent_gc, sharded_multi_replica_hardlink,
                  Variant::ShardedMultiReplicaHardlink)
    ->Args({2000, 4, 4})
    ->Args({10000, 4, 4})
    ->Args({50000, 4, 4})
    ->Unit(benchmark::kMillisecond)
    ->UseManualTime();

/* ---------- gc_barabasi / gc_clusters ----------------------------
 *
 * End-to-end GC on a fixture of `nPaths` store entries with auto-
 * optimise pre-run so `.links/` is fully populated. The two bench
 * symbols differ only in fixture topology:
 *
 *   gc_barabasi — Barabási–Albert preferential attachment, sqrt(N)
 *                 random roots. Heavy-tailed in-degree; closure walk
 *                 dominates over Phase-2 deletion (mostly-live
 *                 store, build-farm shape after recent GC).
 *
 *   gc_clusters — many small isolated cluster DAGs (clusterSize=50,
 *                 geometric), ~1% rooted. Bulk of the store is
 *                 genuinely unreachable; Phase-1 rename + Phase-2
 *                 parallel deletePath dominate (Nix store after
 *                 heavy build churn). BA can't model this: 1%
 *                 random roots on a hubby graph still keep almost
 *                 everything live.
 *
 * Both vary `gc-links-threads` to expose the Patch G1 parallel-links
 * speedup. The deletion phase (Patch G2: batched SQLite
 * invalidation) runs unconditionally — no thread knob — but is
 * exercised at the same time.
 *
 * Expected shape: near-linear speedup on the links phase, flat on
 * the deletion phase, so overall scaling flattens as deletion
 * dominates.
 */
static void gc_collect(benchmark::State & state, Variant variant, Dispatch dispatch,
                       FixtureSpec::Topology topology, size_t clusterSize = 50)
{
    std::lock_guard benchLock(benchSerialiseMutex);
    SettingsGuard settingsGuard;

    const size_t nPaths = state.range(0);
    const size_t threads = state.range(1);

    GCResults::Timings sumTimings;
    uint64_t totalCallSumNs = 0;
    for (auto _ : state) {
        state.PauseTiming();
        applyVariant(variant);
        BenchFixture fixture(nPaths, /*avgFilesPerPath=*/10,
                             /*nRootsOverride=*/std::nullopt,
                             topology, clusterSize);
        settings.getLocalSettings().optimiseThreads.override(std::min<size_t>(threads, 8));
        settings.getLocalSettings().getGCSettings().gcLinksThreads.override(threads);
        settings.getLocalSettings().getGCSettings().gcLinksUseIoUring.override(dispatch == Dispatch::IoUring);
        /* Pre-optimise so `.links/` has entries for GC to sweep.
           The warm-up registers every path as a temp root via
           addTempRoot; without the truncate below, the timed GC
           sees every path as live and Phase-2 deletion is a no-op.
           The fixture's permanent `gcroots/` symlinks survive
           truncation. */
        {
            OptimiseStats warm;
            fixture.local().optimiseStore(warm);
        }
        truncateBenchTempRoots(fixture.root);
        state.ResumeTiming();

        /* Phase markers: two unique syscalls that bracket the timed
           region. bpftrace gates its counters on observing these in
           the bench's process so per-mode syscall/VFS tallies
           reflect only the collectGarbage call, not the (excluded)
           fixture build and optimise warm-up. ENOENT on both is the
           expected outcome; the markers are syscall observations,
           not real operations. */
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
        accumulateStageTimings(sumTimings, res.timings, gcStages);
    }

    reportStageCounters(state, sumTimings, gcStages);
    reportTotalMs(state, totalCallSumNs);
    state.SetItemsProcessed(state.iterations() * nPaths);
}

static void gc_barabasi(benchmark::State & state, Variant variant, Dispatch dispatch)
{
    gc_collect(state, variant, dispatch, FixtureSpec::Topology::Barabasi);
}

/* Helper macro to emit `BENCHMARK_CAPTURE` rows for every
   `(variant, dispatch)` pair in the GC matrix. Args = `(nPaths,
   gc-links-threads)`. */
#define GC_CAPTURE(BENCH, VARIANT_TAG, VARIANT_ENUM, DISPATCH_TAG, DISPATCH_ENUM, ...) \
    BENCHMARK_CAPTURE(BENCH, DISPATCH_TAG##_##VARIANT_TAG, VARIANT_ENUM, DISPATCH_ENUM) \
        __VA_ARGS__ \
        ->Unit(benchmark::kMillisecond) \
        ->UseManualTime()

/* `gc_barabasi` uses the BA fixture (mostly-live). The
   thread-scaling profile (1/4/16) only runs on the
   `syscall_flat_single_replica_hardlink` baseline; the layout ×
   dedup × dispatch matrix runs at threads=4 for the rest. */
GC_CAPTURE(gc_barabasi, flat_single_replica_hardlink, Variant::FlatSingleReplicaHardlink,
           syscall, Dispatch::Syscall,
           ->Args({200, 1})
           ->Args({200, 4})
           ->Args({200, 16})
           ->Args({2000, 4})
           ->Args({2000, 16})
           ->Args({10000, 4})
           ->Args({10000, 16})
           ->Args({50000, 4})
           ->Args({50000, 16}));

GC_CAPTURE(gc_barabasi, flat_single_replica_hardlink, Variant::FlatSingleReplicaHardlink,
           iouring, Dispatch::IoUring,
           ->Args({2000, 4})->Args({10000, 4})->Args({50000, 4}));

GC_CAPTURE(gc_barabasi, flat_multi_replica_hardlink, Variant::FlatMultiReplicaHardlink,
           syscall, Dispatch::Syscall,
           ->Args({2000, 4})->Args({10000, 4})->Args({50000, 4}));
GC_CAPTURE(gc_barabasi, flat_multi_replica_hardlink, Variant::FlatMultiReplicaHardlink,
           iouring, Dispatch::IoUring,
           ->Args({2000, 4})->Args({10000, 4})->Args({50000, 4}));

GC_CAPTURE(gc_barabasi, sharded_single_replica_hardlink, Variant::ShardedSingleReplicaHardlink,
           syscall, Dispatch::Syscall,
           ->Args({2000, 4})->Args({10000, 4})->Args({50000, 4}));
GC_CAPTURE(gc_barabasi, sharded_single_replica_hardlink, Variant::ShardedSingleReplicaHardlink,
           iouring, Dispatch::IoUring,
           ->Args({2000, 4})->Args({10000, 4})->Args({50000, 4}));

GC_CAPTURE(gc_barabasi, sharded_multi_replica_hardlink, Variant::ShardedMultiReplicaHardlink,
           syscall, Dispatch::Syscall,
           ->Args({2000, 4})->Args({10000, 4})->Args({50000, 4}));
GC_CAPTURE(gc_barabasi, sharded_multi_replica_hardlink, Variant::ShardedMultiReplicaHardlink,
           iouring, Dispatch::IoUring,
           ->Args({2000, 4})->Args({10000, 4})->Args({50000, 4}));

static void gc_clusters(benchmark::State & state, Variant variant, Dispatch dispatch)
{
    gc_collect(state, variant, dispatch, FixtureSpec::Topology::Clusters,
               /*clusterSize=*/50);
}

/* Matrix uses the same `GC_CAPTURE` helper as `gc_barabasi`,
   with the same 5 variants × 2 dispatch combos. The bench body
   measures only the `collectGarbage` call (not the warm-up
   `optimiseStore`), so dim deltas attribute purely to GC behaviour
   on the variant's resulting `.links/` layout. */
GC_CAPTURE(gc_clusters, flat_single_replica_hardlink, Variant::FlatSingleReplicaHardlink,
           syscall, Dispatch::Syscall,
           ->Args({2000, 4})->Args({10000, 4})->Args({50000, 4}));
GC_CAPTURE(gc_clusters, flat_single_replica_hardlink, Variant::FlatSingleReplicaHardlink,
           iouring, Dispatch::IoUring,
           ->Args({2000, 4})->Args({10000, 4})->Args({50000, 4}));

GC_CAPTURE(gc_clusters, flat_multi_replica_hardlink, Variant::FlatMultiReplicaHardlink,
           syscall, Dispatch::Syscall,
           ->Args({2000, 4})->Args({10000, 4})->Args({50000, 4}));
GC_CAPTURE(gc_clusters, flat_multi_replica_hardlink, Variant::FlatMultiReplicaHardlink,
           iouring, Dispatch::IoUring,
           ->Args({2000, 4})->Args({10000, 4})->Args({50000, 4}));

GC_CAPTURE(gc_clusters, sharded_single_replica_hardlink, Variant::ShardedSingleReplicaHardlink,
           syscall, Dispatch::Syscall,
           ->Args({2000, 4})->Args({10000, 4})->Args({50000, 4}));
GC_CAPTURE(gc_clusters, sharded_single_replica_hardlink, Variant::ShardedSingleReplicaHardlink,
           iouring, Dispatch::IoUring,
           ->Args({2000, 4})->Args({10000, 4})->Args({50000, 4}));

GC_CAPTURE(gc_clusters, sharded_multi_replica_hardlink, Variant::ShardedMultiReplicaHardlink,
           syscall, Dispatch::Syscall,
           ->Args({2000, 4})->Args({10000, 4})->Args({50000, 4}));
GC_CAPTURE(gc_clusters, sharded_multi_replica_hardlink, Variant::ShardedMultiReplicaHardlink,
           iouring, Dispatch::IoUring,
           ->Args({2000, 4})->Args({10000, 4})->Args({50000, 4}));

/* ---------- invalidate_paths ---------------------------------
 *
 * Isolates Patch G2 (batched SQLite invalidation) from the rest of GC.
 * Registers `nPaths` paths, times the call to invalidate all of them
 * in one batch. Compare against the `BM_RegisterValidPathsDerivations`
 * wall-clock to get a rough sense of the per-path cost.
 */
static void invalidate_paths(benchmark::State & state)
{
    std::lock_guard benchLock(benchSerialiseMutex);
    SettingsGuard settingsGuard;

    const size_t nPaths = state.range(0);

    uint64_t totalCallSumNs = 0;
    for (auto _ : state) {
        state.PauseTiming();
        BenchFixture fixture(nPaths, /*nShared=*/2);
        /* Topo-sort so referrers come before references. `Refs.reference`
           has an `on delete restrict` FK, and SQLite evaluates FKs
           immediately (no `DEFERRABLE INITIALLY DEFERRED` in the
           schema), so deleting an inner node before its referrers have
           been deleted fails mid-transaction with
           `FOREIGN KEY constraint failed`. Production GC mirrors this
           by running `topoSort` on its dead-path set in gc.cc's main
           traversal and then calling `invalidatePathsChecked` on
           batches of 256; our single batch is correct under the same
           ordering. `topoSortPaths` gives us "p refers to q ⇒ p
           before q", which is that ordering. Without this, any
           fixture with cross-references (which here is all `nPaths`
           > `platformSize()`, i.e. effectively every registered
           cell) aborts the bench binary with std::terminate. The
           userspace `PathInUse` check inside `invalidatePathsChecked`
           short-circuits only the application-level check — SQLite
           still enforces the FK row-by-row. */
        auto pathSet = fixture.store->queryAllValidPaths();
        auto paths = fixture.store->topoSortPaths(pathSet);
        state.ResumeTiming();

        totalCallSumNs += timedCall(state, [&] {
            fixture.local().invalidatePathsChecked(paths);
        });

        state.PauseTiming();
    }

    reportTotalMs(state, totalCallSumNs);
    state.SetItemsProcessed(state.iterations() * nPaths);
}

/* `UseManualTime()` matches every other bench in this file —
   `PauseTiming` empirically leaks fixture-build wall into the
   reported Time column on this google-benchmark build, so we time
   the actual `invalidatePathsChecked` call with `steady_clock` and
   feed it back via `SetIterationTime`. Without this, this row's
   Time would be incomparable with the rest of the matrix. */
BENCHMARK(invalidate_paths)
    ->Arg(100)
    ->Arg(500)
    ->Arg(2000)
    ->Arg(10000)
    ->Arg(50000)
    ->Unit(benchmark::kMillisecond)
    ->UseManualTime();

#endif
