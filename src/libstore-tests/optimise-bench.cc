#include <benchmark/benchmark.h>

#include "nix/store/derivations.hh"
#include "nix/store/globals.hh"
#include "nix/store/local-store.hh"
#include "nix/store/store-open.hh"
#include "nix/util/experimental-features.hh"
#include "nix/util/file-system.hh"
#include "nix/util/hash.hh"

#ifndef _WIN32

#  include "nix/util/file-descriptor.hh"

#  include <oneapi/tbb/parallel_for_each.h>
#  include <oneapi/tbb/task_arena.h>

#  include <fcntl.h>
#  include <sys/stat.h>
#  include <unistd.h>

#  include <algorithm>
#  include <cmath>
#  include <cstdint>
#  include <filesystem>
#  include <mutex>
#  include <random>
#  include <string>
#  include <thread>
#  include <vector>

using namespace nix;

namespace {

/* ---------------------------------------------------------------------------
   Fixture model

   The fixture builds a synthetic Nix store with three properties chosen
   to stress the same code paths a real store does:

     1. Heavy-tailed in-degree distribution. A small "platform" head
        (the first `platformSize()` paths, with no references) plays the
        role of glibc / bash / coreutils. Application paths are added in
        topological order using Barabási–Albert preferential attachment:
        each new path's references are sampled from `targets`, where
        each existing path appears once per incoming edge. The result is
        a power-law in-degree distribution dominated by a few hubs —
        which is what the snapshot-driven GC's hot path is amortising
        across.

     2. Variable file count and size. Per-path file count is geometric
        with mean `avgFilesPerPath`. Per-file size is log-normal
        (median ~1 KiB, p99 ~10 KiB), clamped to [32 B, 256 KiB]. Yields
        the long-tailed "many small files, occasional bigger blob" shape
        of real packages.

     3. Realistic content sharing. A pool of 256 deterministic blobs
        (log-normal sized, median ~4 KiB) supplies dedup-eligible
        content. Each file picks "from the pool" with probability 0.6
        (weighted by sqrt-Zipf so a few blobs are very popular), or
        "fresh random" with probability 0.4. Per-blob hardlink usage is
        capped at 4096 to stay well below ext4's EMLINK ceiling at any
        fixture size.

   GC roots: ceil(sqrt(nPaths)) randomly selected non-platform paths
   are registered as permanent roots, so `collectGarbage(gcDeleteDead)`
   actually exercises the live-closure walk instead of deleting
   everything.

   Determinism: every random draw is seeded from `(nPaths,
   avgFilesPerPath)` via the FNV-style mixer in `FixtureSpec::seed()`.
   Same args ⇒ byte-identical fixture across runs and machines. No
   files are checked into the tree.
   --------------------------------------------------------------------------- */

struct FixtureSpec
{
    size_t nPaths;
    size_t avgFilesPerPath;

    size_t platformSize() const
    {
        return std::clamp<size_t>(nPaths / 100, 10, 200);
    }

    size_t nRoots() const
    {
        return std::max<size_t>(1, static_cast<size_t>(std::sqrt(double(nPaths))));
    }

    uint64_t seed() const
    {
        uint64_t h = 0xcbf29ce484222325ull;
        for (uint64_t v : {uint64_t(nPaths), uint64_t(avgFilesPerPath)}) {
            h ^= v;
            h *= 0x100000001b3ull;
        }
        return h;
    }
};

struct ContentPool
{
    static constexpr size_t kPoolSize = 256;

    std::vector<std::string> blobs;
    std::discrete_distribution<size_t> picker;

    explicit ContentPool(std::mt19937_64 & rng)
    {
        std::lognormal_distribution<double> sizeDist(std::log(4096.0), 1.0);
        std::uniform_int_distribution<int> byteDist(0, 255);
        std::vector<double> weights(kPoolSize);
        blobs.reserve(kPoolSize);
        for (size_t i = 0; i < kPoolSize; ++i) {
            size_t sz = std::clamp<size_t>(
                static_cast<size_t>(sizeDist(rng)), 64, 256 * 1024);
            std::string blob(sz, '\0');
            for (auto & c : blob)
                c = static_cast<char>(byteDist(rng));
            blobs.push_back(std::move(blob));
            weights[i] = 1.0 / std::sqrt(double(i + 1));
        }
        picker = std::discrete_distribution<size_t>(weights.begin(), weights.end());
    }

    size_t pickIdx(std::mt19937_64 & rng)
    {
        return picker(rng);
    }
};

/* Per-file plan written by phase 1, consumed by phase 2. */
struct FileSpec
{
    int32_t blobIdx;     /* >=0: index into ContentPool::blobs; -1: unique */
    uint32_t uniqueSize; /* only used if blobIdx == -1 */
};

struct PathPlan
{
    StorePath storePath;
    std::vector<FileSpec> files;
    uint64_t fileSeedBase; /* per-path seed for unique-content RNG */

    PathPlan(StorePath sp, uint64_t seed)
        : storePath(std::move(sp))
        , fileSeedBase(seed)
    {
    }
};

/* Generate `sz` deterministic-but-unique bytes from `seed` directly
   into `fd`. 8 bytes per RNG call (vs 1 byte/call when sampling
   uniform_int<0,255>); ~5× faster end-to-end. */
static void writeUniqueBytes(int fd, size_t sz, uint64_t seed)
{
    std::mt19937_64 rng(seed);
    constexpr size_t bufBytes = 4096;
    uint64_t buf[bufBytes / sizeof(uint64_t)];
    size_t written = 0;
    while (written < sz) {
        for (auto & w : buf)
            w = rng();
        size_t toWrite = std::min(bufBytes, sz - written);
        writeFull(fd, std::string_view(reinterpret_cast<const char *>(buf), toWrite));
        written += toWrite;
    }
}

/* Create a fresh LocalStore backed by a tmp dir, populate it with
   `nPaths` synthetic store entries shaped per the fixture model
   above. `avgFilesPerPath` sets the geometric-distribution mean for
   per-path file count; the legacy second argument is repurposed
   without changing call sites.

   Returns a RAII bundle that deletes the tree on destruction. */
struct BenchFixture
{
    std::filesystem::path root;
    ref<Store> store;

    BenchFixture(size_t nPaths, size_t avgFilesPerPath)
        : root(createTempDir())
        , store(
              (std::filesystem::create_directories(root / "nix/store"),
               openStore(fmt("local?root=%s", root.string()))))
    {
        auto & local = this->local();
        FixtureSpec spec{nPaths, avgFilesPerPath};
        std::mt19937_64 rng(spec.seed());

        ContentPool pool(rng);

        std::geometric_distribution<size_t> fileCountDist(
            1.0 / std::max<double>(1.0, double(avgFilesPerPath)));
        std::poisson_distribution<size_t> outDegreeDist(6.0);
        std::bernoulli_distribution dedupCoin(0.6);
        std::bernoulli_distribution uniformRefCoin(0.3);
        std::lognormal_distribution<double> uniqueSizeDist(std::log(1024.0), 1.0);

        ValidPathInfos infos;
        std::vector<StorePath> pathByIndex;
        pathByIndex.reserve(nPaths);
        std::vector<PathPlan> plans;
        plans.reserve(nPaths);

        /* Barabási–Albert "targets" pool: each existing path appears
           once per incoming edge. Sampling uniformly from this list
           gives selection probability proportional to (in-degree),
           yielding the heavy-tailed distribution. Mixed with uniform
           sampling at probability 0.3 so brand-new paths still get
           occasional references. */
        std::vector<size_t> targets;
        targets.reserve(size_t(nPaths * 6 * 1.3));

        const size_t platform = spec.platformSize();
        const std::string storeDir = (root / "nix/store").string();

        /* ----- Phase 1: sequential plan construction (CPU-only) ----- */
        for (size_t i = 0; i < nPaths; ++i) {
            auto storePath = StorePath::random(fmt("optimise-bench-%zu", i));
            pathByIndex.push_back(storePath);
            PathPlan plan(storePath, spec.seed() ^ (uint64_t(i) * 0x9e3779b97f4a7c15ull));

            size_t fileCount = 1 + fileCountDist(rng);
            uint64_t totalBytes = 0;
            plan.files.reserve(fileCount);
            for (size_t f = 0; f < fileCount; ++f) {
                FileSpec fs{};
                if (dedupCoin(rng)) {
                    fs.blobIdx = static_cast<int32_t>(pool.pickIdx(rng));
                    totalBytes += pool.blobs[fs.blobIdx].size();
                } else {
                    fs.blobIdx = -1;
                    fs.uniqueSize = std::clamp<uint32_t>(
                        static_cast<uint32_t>(uniqueSizeDist(rng)), 32, 256 * 1024);
                    totalBytes += fs.uniqueSize;
                }
                plan.files.push_back(fs);
            }

            ValidPathInfo info{plan.storePath, UnkeyedValidPathInfo(local, Hash::dummy)};
            info.narSize = totalBytes;

            /* Platform paths (i < platform) have no references — they
               are the leaves of the dependency DAG, like glibc. */
            if (i >= platform && i > 0) {
                size_t k = std::min(outDegreeDist(rng), i);
                std::set<size_t> chosen;
                size_t attempts = 0;
                while (chosen.size() < k && attempts < k * 4) {
                    ++attempts;
                    size_t pick;
                    if (targets.empty() || uniformRefCoin(rng)) {
                        std::uniform_int_distribution<size_t> u(0, i - 1);
                        pick = u(rng);
                    } else {
                        std::uniform_int_distribution<size_t> u(0, targets.size() - 1);
                        pick = targets[u(rng)];
                    }
                    chosen.insert(pick);
                }
                StorePathSet refs;
                for (size_t idx : chosen) {
                    refs.insert(pathByIndex[idx]);
                    targets.push_back(idx);
                }
                info.references = std::move(refs);
            }

            /* Seed the platform layer in `targets` once each so the
               first application paths can pick them. */
            if (i < platform)
                targets.push_back(i);

            infos.emplace(plan.storePath, std::move(info));
            plans.push_back(std::move(plan));
        }

        /* ----- Phase 2: parallel file I/O ----- */
        size_t writeThreads = std::clamp<size_t>(
            std::thread::hardware_concurrency(), 1, 16);
        tbb::task_arena arena(static_cast<int>(writeThreads));
        arena.execute([&] {
        tbb::parallel_for_each(plans, [&](const PathPlan & plan) {
            std::string pathDir = storeDir + "/" + std::string(plan.storePath.to_string());
            if (::mkdir(pathDir.c_str(), 0755) != 0)
                throw SysError("mkdir %s", pathDir);
            for (size_t f = 0; f < plan.files.size(); ++f) {
                std::string fp = pathDir + "/f-" + std::to_string(f);
                /* Mode 0444 at create-time avoids a follow-up chmod
                   call. With normal umask 022 the resulting mode is
                   0444; with restrictive umasks (077) it's 0400 —
                   either way it's read-only, which is what
                   `optimisePath_` requires. */
                int fd = ::open(fp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0444);
                if (fd < 0)
                    throw SysError("open %s", fp);
                const auto & fs = plan.files[f];
                try {
                    if (fs.blobIdx >= 0) {
                        const auto & blob = pool.blobs[fs.blobIdx];
                        writeFull(fd, std::string_view(blob.data(), blob.size()));
                    } else {
                        writeUniqueBytes(
                            fd, fs.uniqueSize,
                            plan.fileSeedBase ^ (uint64_t(f) * 0xbf58476d1ce4e5b9ull));
                    }
                } catch (...) {
                    ::close(fd);
                    throw;
                }
                ::close(fd);
            }
        });
        });

        /* ----- Phase 3: register paths and roots ----- */
        local.registerValidPaths(infos);

        /* Register sqrt(nPaths) random non-platform paths as permanent
           GC roots. Without this, `collectGarbage(gcDeleteDead)` has
           an empty live set and the liveness traversal does no work. */
        if (nPaths > platform) {
            auto rootsDir = root / "gcroots";
            std::filesystem::create_directories(rootsDir);
            std::uniform_int_distribution<size_t> rootPick(platform, nPaths - 1);
            std::set<size_t> rootIndices;
            size_t want = std::min(spec.nRoots(), nPaths - platform);
            while (rootIndices.size() < want)
                rootIndices.insert(rootPick(rng));
            for (size_t idx : rootIndices) {
                auto link = rootsDir / fmt("r-%zu", idx);
                local.addPermRoot(pathByIndex[idx], link);
            }
        }
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

    SettingsGuard()
        : savedOptimiseThreads(settings.getLocalSettings().optimiseThreads.get())
        , savedGcLinksThreads(settings.getLocalSettings().getGCSettings().gcLinksThreads.get())
    {
    }

    ~SettingsGuard()
    {
        settings.getLocalSettings().optimiseThreads.override(savedOptimiseThreads);
        settings.getLocalSettings().getGCSettings().gcLinksThreads.override(savedGcLinksThreads);
    }

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

    for (auto _ : state) {
        state.PauseTiming();
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
    /* (nPaths, optimise-threads). */
    ->Args({200, 1})
    ->Args({200, 4})
    ->Args({200, 16})
    ->Args({2000, 1})
    ->Args({2000, 4})
    ->Args({2000, 16})
    ->Args({10000, 1})
    ->Args({10000, 4})
    ->Args({10000, 16})
    /* Large fixtures: only the high-thread cases — single-threaded
       runs at this size are dominated by setup, not signal. */
    ->Args({50000, 4})
    ->Args({50000, 16})
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

    for (auto _ : state) {
        state.PauseTiming();
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
    /* (nPaths, optimise-threads, gc-links-threads). */
    ->Args({200, 1, 1})
    ->Args({200, 4, 4})
    ->Args({200, 16, 16})
    ->Args({2000, 1, 1})
    ->Args({2000, 4, 4})
    ->Args({2000, 16, 16})
    ->Args({2000, 16, 1})
    ->Args({2000, 1, 16})
    /* Larger fixtures — exercise contention at realistic scale. */
    ->Args({10000, 4, 4})
    ->Args({10000, 16, 16})
    ->Args({50000, 4, 4})
    ->Args({50000, 16, 16})
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

    for (auto _ : state) {
        state.PauseTiming();
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
    /* (nPaths, gc-links-threads). */
    ->Args({200, 1})
    ->Args({200, 16})
    ->Args({2000, 1})
    ->Args({2000, 4})
    ->Args({2000, 16})
    ->Args({10000, 1})
    ->Args({10000, 4})
    ->Args({10000, 16})
    ->Args({50000, 4})
    ->Args({50000, 16})
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
    ->Arg(50000)
    ->Unit(benchmark::kMillisecond);

#endif
