#include <benchmark/benchmark.h>

#include "nix/store/derivations.hh"
#include "nix/store/globals.hh"
#include "nix/store/local-store.hh"
#include "nix/store/store-open.hh"
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

/* ========================================================================
 *   Benchmarks for `optimiseStore` and `collectGarbage`
 * ========================================================================
 *
 * Points the fixture at a tmpdir (inherited from `$TMPDIR`). On
 * typical hosts this resolves to `/tmp`, which is usually a tmpfs
 * (RAM-backed). Override via `TMPDIR=/some/other/path` to measure
 * real-filesystem latency and IOPS effects (e.g. an ext4 or xfs
 * volume representative of your production store).
 *
 * Usage:
 *
 *     /path/to/nix-store-benchmarks --benchmark_filter='BM_OptimiseStore'
 *
 * These benchmarks are designed so you can run the same binary against
 * different Nix builds (upstream vs. a patched tree) and compare.
 * ======================================================================== */

namespace {

/* Create a fresh LocalStore backed by a tmp dir. Register `nPaths`,
   each containing `nShared` files (rotating content by group id so
   `.links/` gets populated but no single hash exceeds the 65 000
   hardlink ceiling on ext4), plus one unique file per path. */
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

            /* Group id rotates every `groupSize` paths so `.links/`
               accumulates distinct hashes without any single hash
               exhausting the 65 000-hardlink ext4 ceiling. */
            auto group = i / groupSize;
            std::string sharedContent(4096, static_cast<char>('A' + (group % 26)));

            for (size_t j = 0; j < nShared; ++j) {
                auto p = pathDir / fmt("shared-%d", j);
                std::ofstream out(p, std::ios::binary);
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
        /* `optimisePath_` sets subdirectories read-only; make them
           writable again so `remove_all` can traverse and unlink. */
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

/* Serialise all benchmarks in this file so that (a) they don't step
   on each other's fixture state, and (b) adding `->Threads(N)` on
   any bench later cannot silently race on the global `settings`
   object. */
static std::mutex benchSerialiseMutex;

} // namespace

/* ---------- BM_OptimiseStore ------------------------------------------
 *
 * Measures the wall-clock of a single `optimiseStore()` run over a
 * fixture of `nPaths` × `nShared` files.
 */
static void BM_OptimiseStore(benchmark::State & state)
{
    std::lock_guard benchLock(benchSerialiseMutex);

    const size_t nPaths = state.range(0);
    const size_t nShared = 10;

    for (auto _ : state) {
        state.PauseTiming();
        BenchFixture fixture(nPaths, nShared);
        state.ResumeTiming();

        OptimiseStats stats;
        fixture.local().optimiseStore(stats);

        state.PauseTiming();
        benchmark::DoNotOptimize(stats.bytesFreed);
        benchmark::DoNotOptimize(stats.filesLinked);
    }

    state.SetItemsProcessed(state.iterations() * nPaths * nShared);
}

BENCHMARK(BM_OptimiseStore)
    ->Arg(200)
    ->Arg(2000)
    ->Arg(10000)
    ->Unit(benchmark::kMillisecond);

/* ---------- BM_CollectGarbage -----------------------------------------
 *
 * End-to-end GC on a fixture of `nPaths` store entries, with auto-
 * optimise pre-run so `.links/` is populated before GC starts.
 */
static void BM_CollectGarbage(benchmark::State & state)
{
    std::lock_guard benchLock(benchSerialiseMutex);

    const size_t nPaths = state.range(0);

    for (auto _ : state) {
        state.PauseTiming();
        BenchFixture fixture(nPaths, /*nShared=*/10);
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
    ->Arg(200)
    ->Arg(2000)
    ->Arg(10000)
    ->Unit(benchmark::kMillisecond);

/* ---------- BM_OptimiseConcurrentGC -----------------------------------
 *
 * Runs `optimiseStore()` and `collectGarbage()` simultaneously in the
 * same process. Captures total wall-clock when the two operations
 * race against each other — the common case on a Hydra master.
 */
static void BM_OptimiseConcurrentGC(benchmark::State & state)
{
    std::lock_guard benchLock(benchSerialiseMutex);

    const size_t nPaths = state.range(0);

    for (auto _ : state) {
        state.PauseTiming();
        BenchFixture fixture(nPaths, /*nShared=*/8);
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
                   being modified; we're measuring contention, not
                   correctness. */
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
        benchmark::DoNotOptimize(stats.bytesFreed);
    }

    state.SetItemsProcessed(state.iterations() * nPaths);
}

BENCHMARK(BM_OptimiseConcurrentGC)
    ->Arg(200)
    ->Arg(2000)
    ->Unit(benchmark::kMillisecond);

#endif
