#include <benchmark/benchmark.h>

#include "nix/store/derivations.hh"
#include "nix/store/globals.hh"
#include "nix/store/local-store.hh"
#include "nix/store/store-open.hh"
#include "nix/util/experimental-features.hh"
#include "nix/util/file-system.hh"
#include "nix/util/hash.hh"

#ifndef _WIN32

#  include <algorithm>
#  include <filesystem>
#  include <fstream>
#  include <mutex>
#  include <string>
#  include <thread>

using namespace nix;

namespace {

/* Create a fresh LocalStore backed by a tmp dir, register `nPaths`
   each containing `nShared` files with identical content plus a few
   unique files. This is a tight approximation of a typical Nix store
   layout post-auto-optimise.

   Returns a RAII bundle that deletes the tree on destruction. */
struct BenchFixture
{
    std::filesystem::path root;
    ref<Store> store;

    BenchFixture(size_t nPaths, size_t nShared)
        : root(createTempDir())
        , store(
              (std::filesystem::create_directories(root / "nix/store"),
               openStore(fmt("local?root=%s", root.string()))))
    {
        auto & local = this->local();

        /* Write N fake store paths, each containing `nShared` files
           and one unique file. Shared-file contents are parameterised
           over a "group" id derived from the path index so that:
             (a) dedup still exercises `.links/` (paths in the same
                 group get identical shared files), and
             (b) no single `.links/<hash>` entry exceeds the ext4
                 65 000-hardlink ceiling even for large fixtures.

           Group size = 4096 means `.links/` accumulates ~nPaths*nShared
           / 4096 distinct hashes. For 10k paths × 10 files that's
           ~24 entries — more than enough to exercise directory-level
           contention without hitting EMLINK. */
        constexpr size_t groupSize = 4096;
        ValidPathInfos infos;
        auto makeReadOnly = [](const std::filesystem::path & p) {
            /* Store files must be read-only or `optimisePath_` skips
               them as "suspicious writable files". */
            std::filesystem::permissions(
                p,
                std::filesystem::perms::owner_read
                | std::filesystem::perms::group_read
                | std::filesystem::perms::others_read);
        };
        for (size_t i = 0; i < nPaths; ++i) {
            auto drvPath = StorePath::random(fmt("optimise-bench-%d", i));
            auto pathDir = root / "nix/store" / std::string(drvPath.to_string());
            std::filesystem::create_directory(pathDir);

            /* Group id varies every `groupSize` paths so the shared
               content rotates. */
            auto group = i / groupSize;
            std::string sharedContent(4096, static_cast<char>('A' + (group % 26)));

            for (size_t j = 0; j < nShared; ++j) {
                auto p = pathDir / fmt("shared-%d", j);
                std::ofstream out(p, std::ios::binary);
                /* Mix group id + file index so each (group, j) pair
                   hashes distinctly. */
                out.write(sharedContent.data(), sharedContent.size());
                out.put(static_cast<char>('a' + j));
                out.close();
                makeReadOnly(p);
            }

            std::string unique = fmt("unique-%d", i);
            auto up = pathDir / "unique";
            std::ofstream uf(up, std::ios::binary);
            uf.write(unique.data(), unique.size());
            uf.close();
            makeReadOnly(up);

            ValidPathInfo info{drvPath, UnkeyedValidPathInfo(local, Hash::dummy)};
            info.narSize = (sharedContent.size() + 1) * nShared + unique.size();
            infos.emplace(drvPath, std::move(info));
        }
        local.registerValidPaths(infos);
    }

    LocalStore & local()
    {
        auto sp = std::shared_ptr<Store>(store.get_ptr());
        auto lp = std::dynamic_pointer_cast<LocalStore>(sp);
        if (!lp)
            throw Error("expected LocalStore for local?root=...");
        return *lp;
    }

    BenchFixture(const BenchFixture &) = delete;
    BenchFixture & operator=(const BenchFixture &) = delete;

    ~BenchFixture()
    {
        /* optimisePath_ sets subdirectories read-only via
           MakeReadOnly; make every directory writable again so
           remove_all can traverse and unlink. */
        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
             !ec && it != std::filesystem::recursive_directory_iterator();
             it.increment(ec)) {
            if (it->is_directory(ec))
                std::filesystem::permissions(
                    it->path(), std::filesystem::perms::owner_all, ec);
        }
        std::filesystem::remove_all(root, ec);
    }
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
    size_t savedOptimiseThreads;
    size_t savedGcLinksThreads;
    std::set<ExperimentalFeature> savedExperimentalFeatures;

    SettingsGuard()
        : savedOptimiseThreads(settings.getLocalSettings().optimiseThreads.get())
        , savedGcLinksThreads(settings.getLocalSettings().getGCSettings().gcLinksThreads.get())
        , savedExperimentalFeatures(experimentalFeatureSettings.experimentalFeatures.get())
    {
    }

    ~SettingsGuard()
    {
        settings.getLocalSettings().optimiseThreads.override(savedOptimiseThreads);
        settings.getLocalSettings().getGCSettings().gcLinksThreads.override(savedGcLinksThreads);
        experimentalFeatureSettings.experimentalFeatures.override(savedExperimentalFeatures);
    }

    SettingsGuard(const SettingsGuard &) = delete;
    SettingsGuard & operator=(const SettingsGuard &) = delete;
};

/* Toggle the `sharded-links` experimental feature for the duration of
   the current benchmark iteration. */
static void setShardedLinks(bool enabled)
{
    auto features = experimentalFeatureSettings.experimentalFeatures.get();
    if (enabled)
        features.insert(Xp::ShardedLinks);
    else
        features.erase(Xp::ShardedLinks);
    experimentalFeatureSettings.experimentalFeatures.override(features);
}

/* Serialise all benchmarks in this file against any future parallel
   benchmark runner. google-benchmark's current behaviour is fully
   serial, so this mutex is contention-free today — but it provides a
   durable guarantee so adding `->Threads(N)` on a bench later cannot
   silently introduce global-settings races. */
static std::mutex benchSerialiseMutex;

} // namespace

/* ========================================================================
 *   A note on what these benchmarks measure, and what they don't
 * ========================================================================
 *
 * The fixture uses `$TMPDIR` for the store root. On typical NixOS /
 * Linux hosts `$TMPDIR` resolves to `/tmp`, which is almost always a
 * tmpfs (RAM-backed). Benchmarks on tmpfs show the CPU- and
 * kernel-VFS-bound ceiling of the operations, NOT the real-world
 * wall-clock on a large store backed by a block device.
 *
 * Three concrete differences on tmpfs vs ext4 on an IOPS-limited
 * device (e.g. EBS):
 *
 *   1. `link(2)` and `rename(2)` on tmpfs are ~20–100 µs; on ext4
 *      they are 100 µs – several ms (depends on IOPS budget).
 *   2. tmpfs serialises concurrent directory mutations on a kernel
 *      rwsem. At high thread counts (N=16) the `.links/` directory's
 *      rwsem becomes the bottleneck; per-syscall latency rises 3–4×
 *      over N=4. On a real ext4 store the IOPS ceiling kicks in
 *      before the VFS lock does, so the shape of the scaling curve
 *      is different.
 *   3. tmpfs has no journal, no fsync cost. The batched-SQLite win
 *      in Patch G2 is much more visible on ext4 with `data=ordered`
 *      than on tmpfs.
 *
 * Concretely, measurements on an Intel 32-thread host, Linux 6.18,
 * tmpfs `/tmp`:
 *
 *   BM_OptimiseStore/10000/1   ~5.5 s
 *   BM_OptimiseStore/10000/4   ~2.2 s   (2.5× speedup)
 *   BM_OptimiseStore/10000/16  ~2.5 s   (regression vs N=4)
 *
 * The regression at N=16 is VFS-lock contention, not a Nix-side
 * lock. `perf stat` shows 23× more context switches and 3.85× more
 * system CPU at N=16 vs N=4, and `strace -c` shows link/rename
 * per-call latency grows from 14/25 µs to 56/88 µs.
 *
 * On the target production workload (20 TB EBS, ext4, 16k IOPS
 * budget), parallelism is bounded by IOPS, not by tmpfs VFS
 * locks, and we expect the designed 4–8× speedup at N=8–16.
 * ======================================================================== */

/* ---------- BM_OptimiseStore ------------------------------------------
 *
 * Measures the wall-clock of a single `optimiseStore()` run over a
 * store of `nPaths` × `nShared` files. Varies the `optimise-threads`
 * setting to expose parallel speedup.
 *
 * Expected shape: 2–3× speedup at N=4 on tmpfs; flat or regression
 * beyond N=4 due to `.links/` directory rwsem contention (see
 * top-of-file note). On real ext4 with IOPS headroom, scaling
 * continues further.
 */
static void BM_OptimiseStore(benchmark::State & state)
{
    std::lock_guard benchLock(benchSerialiseMutex);
    SettingsGuard settingsGuard;

    const size_t nPaths = state.range(0);
    const size_t nShared = 10;
    const size_t threads = state.range(1);
    const bool sharded = state.range(2) != 0;

    for (auto _ : state) {
        state.PauseTiming();
        /* Toggle the experimental feature BEFORE opening the store,
           since `LocalStore::shardedLinks` is captured in the ctor. */
        setShardedLinks(sharded);
        BenchFixture fixture(nPaths, nShared);
        settings.getLocalSettings().optimiseThreads.override(threads);
        state.ResumeTiming();

        OptimiseStats stats;
        fixture.local().optimiseStore(stats);

        state.PauseTiming();
        benchmark::DoNotOptimize(stats.bytesFreed.load());
        benchmark::DoNotOptimize(stats.filesLinked.load());
    }

    state.SetItemsProcessed(state.iterations() * nPaths * nShared);
}

BENCHMARK(BM_OptimiseStore)
    /* (nPaths, threads, sharded). Sharded=1 enables the
       `sharded-links` experimental feature so `.links/` splits across
       1024 subdirectories, reducing directory rwsem contention. */
    ->Args({200, 1, 0})
    ->Args({200, 4, 0})
    ->Args({200, 16, 0})
    ->Args({2000, 1, 0})
    ->Args({2000, 4, 0})
    ->Args({2000, 16, 0})
    ->Args({10000, 1, 0})
    ->Args({10000, 4, 0})
    ->Args({10000, 16, 0})
    ->Args({200, 1, 1})
    ->Args({200, 4, 1})
    ->Args({200, 16, 1})
    ->Args({2000, 1, 1})
    ->Args({2000, 4, 1})
    ->Args({2000, 16, 1})
    ->Args({10000, 1, 1})
    ->Args({10000, 4, 1})
    ->Args({10000, 16, 1})
    ->Unit(benchmark::kMillisecond);

/* ---------- BM_OptimiseConcurrentGC -----------------------------------
 *
 * Runs `optimiseStore()` and `collectGarbage()` simultaneously in the
 * same process. Each operation is pinned to its own thread count via
 * Args({nPaths, optThreads, gcThreads}) — asymmetric configurations
 * let us see how contention behaves when one side is oversubscribed.
 *
 * Historical note: before the per-thread gc-socket change in
 * `addTempRoot` (each worker thread owns its own connection keyed by
 * `std::thread::id` in `LocalStore::_fdRootsSockets`), this
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
static void BM_OptimiseConcurrentGC(benchmark::State & state)
{
    std::lock_guard benchLock(benchSerialiseMutex);
    SettingsGuard settingsGuard;

    const size_t nPaths = state.range(0);
    const size_t optThreads = state.range(1);
    const size_t gcThreads = state.range(2);
    const bool sharded = state.range(3) != 0;

    for (auto _ : state) {
        state.PauseTiming();
        setShardedLinks(sharded);
        BenchFixture fixture(nPaths, /*nShared=*/8);
        settings.getLocalSettings().optimiseThreads.override(optThreads);
        settings.getLocalSettings().getGCSettings().gcLinksThreads.override(gcThreads);

        /* Pre-optimise so that the concurrent GC phase has something
           to sweep. We time the _second_ optimise + a concurrent GC. */
        {
            OptimiseStats warm;
            fixture.local().optimiseStore(warm);
        }
        state.ResumeTiming();

        std::thread gcThread([&] {
            GCOptions opts;
            opts.action = GCOptions::gcDeleteDead;
            GCResults res;
            try {
                fixture.local().collectGarbage(opts, res);
            } catch (...) {
                /* GC may fail cleanly if the store is simultaneously
                   being modified; for this benchmark we tolerate
                   that — the point is to measure contention, not
                   correctness (which is covered by the functional
                   tests). */
            }
        });

        OptimiseStats stats;
        try {
            fixture.local().optimiseStore(stats);
        } catch (...) {
            /* Same tolerance as above. */
        }

        gcThread.join();

        state.PauseTiming();
        benchmark::DoNotOptimize(stats.bytesFreed.load());
    }

    state.SetItemsProcessed(state.iterations() * nPaths);
}

BENCHMARK(BM_OptimiseConcurrentGC)
    /* (nPaths, optThreads, gcThreads, sharded). */
    ->Args({200, 1, 1, 0})
    ->Args({200, 4, 4, 0})
    ->Args({200, 16, 16, 0})
    ->Args({2000, 1, 1, 0})
    ->Args({2000, 4, 4, 0})
    ->Args({2000, 16, 16, 0})
    ->Args({2000, 16, 1, 0})
    ->Args({2000, 1, 16, 0})
    ->Args({200, 1, 1, 1})
    ->Args({200, 4, 4, 1})
    ->Args({200, 16, 16, 1})
    ->Args({2000, 1, 1, 1})
    ->Args({2000, 4, 4, 1})
    ->Args({2000, 16, 16, 1})
    ->Args({2000, 16, 1, 1})
    ->Args({2000, 1, 16, 1})
    ->Unit(benchmark::kMillisecond);

/* ---------- BM_CollectGarbage -----------------------------------------
 *
 * End-to-end GC on a fixture of `nPaths` store entries with auto-
 * optimise pre-run so `.links/` is fully populated. Varies
 * `gc-links-threads` to expose the parallel-links speedup introduced
 * in Patch G1.
 *
 * The deletion phase (Patch G2: batched SQLite invalidation) runs
 * unconditionally — it doesn't have a thread knob — but this bench
 * covers its end-to-end contribution at the same time.
 *
 * Expected shape: near-linear speedup on the links phase, flat on
 * the deletion phase, so overall scaling flattens as deletion
 * dominates.
 */
static void BM_CollectGarbage(benchmark::State & state)
{
    std::lock_guard benchLock(benchSerialiseMutex);
    SettingsGuard settingsGuard;

    const size_t nPaths = state.range(0);
    const size_t threads = state.range(1);
    const bool sharded = state.range(2) != 0;

    for (auto _ : state) {
        state.PauseTiming();
        setShardedLinks(sharded);
        BenchFixture fixture(nPaths, /*nShared=*/10);
        settings.getLocalSettings().optimiseThreads.override(std::min<size_t>(threads, 8));
        settings.getLocalSettings().getGCSettings().gcLinksThreads.override(threads);
        /* Pre-optimise so `.links/` has entries for GC to sweep. */
        {
            OptimiseStats warm;
            fixture.local().optimiseStore(warm);
        }
        state.ResumeTiming();

        GCOptions opts;
        opts.action = GCOptions::gcDeleteDead;
        GCResults res;
        fixture.local().collectGarbage(opts, res);

        state.PauseTiming();
        benchmark::DoNotOptimize(res.bytesFreed);
    }

    state.SetItemsProcessed(state.iterations() * nPaths);
}

BENCHMARK(BM_CollectGarbage)
    /* (nPaths, gcLinksThreads, sharded). */
    ->Args({200, 1, 0})
    ->Args({200, 16, 0})
    ->Args({2000, 1, 0})
    ->Args({2000, 4, 0})
    ->Args({2000, 16, 0})
    ->Args({10000, 1, 0})
    ->Args({10000, 4, 0})
    ->Args({10000, 16, 0})
    ->Args({200, 1, 1})
    ->Args({200, 16, 1})
    ->Args({2000, 1, 1})
    ->Args({2000, 4, 1})
    ->Args({2000, 16, 1})
    ->Args({10000, 1, 1})
    ->Args({10000, 4, 1})
    ->Args({10000, 16, 1})
    ->Unit(benchmark::kMillisecond);

/* ---------- BM_InvalidatePathsChecked ---------------------------------
 *
 * Isolates Patch G2 (batched SQLite invalidation) from the rest of GC.
 * Registers `nPaths` paths, times the call to invalidate all of them
 * in one batch. Compare against the `BM_RegisterValidPathsDerivations`
 * wall-clock to get a rough sense of the per-path cost.
 */
static void BM_InvalidatePathsChecked(benchmark::State & state)
{
    std::lock_guard benchLock(benchSerialiseMutex);
    SettingsGuard settingsGuard;

    const size_t nPaths = state.range(0);

    for (auto _ : state) {
        state.PauseTiming();
        BenchFixture fixture(nPaths, /*nShared=*/2);
        /* Collect the registered paths. */
        auto pathSet = fixture.store->queryAllValidPaths();
        std::vector<StorePath> paths{pathSet.begin(), pathSet.end()};
        state.ResumeTiming();

        fixture.local().invalidatePathsChecked(paths);
    }

    state.SetItemsProcessed(state.iterations() * nPaths);
}

BENCHMARK(BM_InvalidatePathsChecked)
    ->Arg(100)
    ->Arg(500)
    ->Arg(2000)
    ->Arg(10000)
    ->Unit(benchmark::kMillisecond);

#endif
