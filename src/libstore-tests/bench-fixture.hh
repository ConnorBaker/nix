#pragma once

#ifndef _WIN32

#include "nix/store/derivations.hh"
#include "nix/store/local-store.hh"
#include "nix/store/store-open.hh"
#include "nix/util/file-descriptor.hh"
#include "nix/util/file-system.hh"
#include "nix/util/hash.hh"

#include <oneapi/tbb/parallel_for_each.h>
#include <oneapi/tbb/task_arena.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace nix::bench {

/* ---------------------------------------------------------------------------
   Fixture model — see optimise-bench.cc for the full rationale of why
   this synthetic store is shaped the way it is. The summary:

     1. Heavy-tailed in-degree distribution. A small "platform" head
        (the first `platformSize()` paths, with no references) plays the
        role of glibc / bash / coreutils. Application paths are added in
        topological order using Barabási–Albert preferential attachment.

     2. Variable file count and size. Per-path file count is geometric
        with mean `avgFilesPerPath`. Per-file size is log-normal
        (median ~1 KiB, p99 ~10 KiB), clamped to [32 B, 256 KiB].

     3. Realistic content sharing. A pool of 256 deterministic blobs
        supplies dedup-eligible content. Each file picks "from the pool"
        with probability 0.6 (weighted by sqrt-Zipf), or "fresh random"
        with probability 0.4.

   GC roots: ceil(sqrt(nPaths)) randomly selected non-platform paths
   (Barabasi) or `liveClusters` cluster tops (Clusters) are registered
   as permanent roots.

   Determinism: every random draw is seeded from `(nPaths,
   avgFilesPerPath, topology, clusterSize)` via FNV-1a in
   `FixtureSpec::seed()`. Same args ⇒ byte-identical fixture.
   --------------------------------------------------------------------------- */

struct FixtureSpec
{
    size_t nPaths;
    size_t avgFilesPerPath;
    /* Optional explicit root count. nullopt → default sqrt(nPaths). */
    std::optional<size_t> nRootsOverride = std::nullopt;

    /* Reference-graph topology. See optimise-bench.cc for rationale. */
    enum class Topology { Barabasi, Clusters };
    Topology topology = Topology::Barabasi;

    /* Geometric mean cluster size. Only used when topology == Clusters. */
    size_t clusterSize = 50;

    /* Number of clusters to root. 0 = auto = max(2, totalClusters/100). */
    size_t liveClusters = 0;

    size_t platformSize() const
    {
        return std::clamp<size_t>(nPaths / 100, 10, 200);
    }

    size_t nRoots() const
    {
        if (nRootsOverride)
            return std::min(*nRootsOverride, nPaths);
        return std::max<size_t>(1, static_cast<size_t>(std::sqrt(double(nPaths))));
    }

    uint64_t seed() const
    {
        uint64_t h = 0xcbf29ce484222325ull;
        for (uint64_t v : {
            uint64_t(nPaths),
            uint64_t(avgFilesPerPath),
            uint64_t(topology),
            uint64_t(clusterSize),
        }) {
            h ^= v;
            h *= 0x100000001b3ull;
        }
        return h;
    }
};

struct ContentPool
{
    static constexpr size_t kPoolSize = 256;

    /* Per-blob ceiling enforced by `pickIdx`. Safety valve against
       pathological fixture inputs (1M-path stores saturating ext4's
       per-inode hardlink ceiling). At supported sizes (≤50k paths)
       rotation never fires; replica spillover is exercised separately
       via the `NIX_TEST_LINK_MAX_OVERRIDE` env var. */
    static constexpr size_t kHardlinkCap = 200000;

    std::vector<std::string> blobs;
    std::vector<size_t> usage;
    std::discrete_distribution<size_t> picker;

    explicit ContentPool(std::mt19937_64 & rng)
        : usage(kPoolSize, 0)
    {
        std::lognormal_distribution<double> sizeDist(std::log(4096.0), 1.0);
        std::vector<double> weights(kPoolSize);
        blobs.reserve(kPoolSize);
        for (size_t i = 0; i < kPoolSize; ++i) {
            size_t sz = std::clamp<size_t>(
                static_cast<size_t>(sizeDist(rng)), 64, 256 * 1024);
            /* Zero-fill body with an 8-byte unique header keyed off
               the blob's index. Why this shape:
                 - Each blob's NAR hash must be distinct from the
                   other 255, otherwise optimise would dedup blobs
                   that should stay as separate canonicals. The
                   8-byte header (`fnv1a-style mix of blob index`)
                   gives 2^64 distinct headers; collision probability
                   among 256 blobs is vanishing.
                 - The rest is zeros so the binary cache (which
                   stores each blob as a NAR + zstd) compresses
                   massively. Random bytes (the previous design)
                   are incompressible — a 556 MB cache at 10k paths.
                   With the zero body, the same fixture compresses
                   to ~5 MB.
                 - Bench timing is unchanged: SHA256 is
                   constant-time-per-byte regardless of content, the
                   dedup count is identical, the file sizes are
                   identical. The only observable difference is
                   on-disk byte distribution, which optimise/GC
                   don't measure. */
            std::string blob(sz, '\0');
            if (sz >= 8) {
                uint64_t header = 0xcbf29ce484222325ull;
                header ^= static_cast<uint64_t>(i);
                header *= 0x100000001b3ull;
                std::memcpy(blob.data(), &header, 8);
            }
            blobs.push_back(std::move(blob));
            weights[i] = 1.0 / std::sqrt(double(i + 1));
        }
        picker = std::discrete_distribution<size_t>(weights.begin(), weights.end());
    }

    /* Phase-1-only — must be called sequentially. */
    size_t pickIdx(std::mt19937_64 & rng)
    {
        for (int tries = 0; tries < 16; ++tries) {
            size_t i = picker(rng);
            if (usage[i] < kHardlinkCap) {
                ++usage[i];
                return i;
            }
        }
        for (size_t i = 0; i < blobs.size(); ++i) {
            if (usage[i] < kHardlinkCap) {
                ++usage[i];
                return i;
            }
        }
        return 0;
    }
};

struct FileSpec
{
    int32_t blobIdx;     /* >=0: index into ContentPool::blobs; -1: unique */
    uint32_t uniqueSize; /* only used if blobIdx == -1 */
};

struct PathPlan
{
    StorePath storePath;
    std::vector<FileSpec> files;
    uint64_t fileSeedBase;

    PathPlan(StorePath sp, uint64_t seed)
        : storePath(std::move(sp))
        , fileSeedBase(seed)
    {
    }
};

/* Write `sz` bytes consisting of an 8-byte unique header keyed off
   `seed` followed by zeros. Why this shape:
     - The 8-byte header makes every `(seed, sz)` combination produce
       a distinct file content → distinct NAR hash → no spurious
       dedup between paths that should remain unique under optimise.
     - The zero tail compresses to near-zero in the cache derivation's
       zstd-encoded binary cache. Real-byte content (the previous
       design) was incompressible and dominated cache size + import
       wall-clock.
     - The bench measurement is unchanged: optimise/GC hash files
       byte-by-byte, but SHA256's per-byte cost is content-independent.
*/
inline void writeUniqueBytes(int fd, size_t sz, uint64_t seed)
{
    constexpr size_t bufBytes = 4096;
    /* Static-init the zero buffer once and reuse it. */
    static const char zeros[bufBytes] = {};

    if (sz == 0)
        return;

    /* 8-byte header. Smaller files (< 8 bytes) get a truncated header
       — currently no caller can hit this since blob sizes are clamped
       to ≥ 64 and unique-file sizes to ≥ 32. */
    char header[8];
    std::memcpy(header, &seed, 8);
    size_t headerLen = std::min<size_t>(sz, 8);
    writeFull(fd, std::string_view(header, headerLen));

    size_t written = headerLen;
    while (written < sz) {
        size_t toWrite = std::min(bufBytes, sz - written);
        writeFull(fd, std::string_view(zeros, toWrite));
        written += toWrite;
    }
}

/* Build a populated `LocalStore` containing a synthetic fixture
   matching `FixtureSpec`. Two modes:

     * Owning mode (default): creates a fresh tmp dir, populates it,
       and `remove_all`s it on destruction. Used by the in-process
       benchmark calls in `optimise-bench.cc`.

     * Borrowed mode (NIX_BENCH_STORE_ROOT set): uses the directory
       in `$NIX_BENCH_STORE_ROOT` as the store root, does NOT delete
       it on destruction. Used by the multi-process bench scenario
       where a sibling `nix-store --optimise` loop is run against
       the same on-disk store concurrently. The bench test harness
       owns cleanup. */
struct BenchFixture
{
    std::filesystem::path root;
    bool ownsRoot;
    ref<Store> store;

    static std::pair<std::filesystem::path, bool> pickRoot()
    {
        if (auto * env = std::getenv("NIX_BENCH_STORE_ROOT"); env && *env) {
            std::filesystem::path r(env);
            std::filesystem::create_directories(r);
            return {r, /*ownsRoot=*/false};
        }
        return {std::filesystem::path(createTempDir()), /*ownsRoot=*/true};
    }

    /* Materialise `nix/store` under `root` and open a `LocalStore`
       there. Factored out of the mem-init list because the previous
       comma-operator form — `store((create_directories(...),
       openStore(...)))` — is a known anti-pattern: readers parse it
       as a 2-arg function call. */
    static ref<Store> openAtRoot(const std::filesystem::path & root)
    {
        std::filesystem::create_directories(root / "nix/store");
        return openStore(fmt("local?root=%s", root.string()));
    }

    BenchFixture(
        size_t nPaths,
        size_t avgFilesPerPath,
        std::optional<size_t> nRootsOverride = std::nullopt,
        FixtureSpec::Topology topology = FixtureSpec::Topology::Barabasi,
        size_t clusterSize = 50)
        : BenchFixture(
              pickRoot(), nPaths, avgFilesPerPath,
              std::move(nRootsOverride), topology, clusterSize)
    {}

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
        /* When `NIX_BENCH_STORE_ROOT` was honoured, the orchestrator
           owns the store directory's lifetime. */
        if (!ownsRoot)
            return;

        /* `optimisePath_` set subdirs read-only via MakeReadOnly;
           a `chmod -R u+w` is the simplest way to make the tree
           removable. Best-effort cleanup — `(void)`-casting the rc
           doesn't suppress GCC's `warn_unused_result` attribute on
           `system`, so capture into a discarded variable instead. */
        std::string cmd = "chmod -R u+w " + shellEscape(root.string());
        [[maybe_unused]] int rc = std::system(cmd.c_str());
        std::error_code rmEc;
        std::filesystem::remove_all(root, rmEc);
    }

private:
    static std::string shellEscape(std::string_view s)
    {
        std::string out;
        out.reserve(s.size() + 2);
        out.push_back('\'');
        for (char c : s) {
            if (c == '\'')
                out.append("'\\''");
            else
                out.push_back(c);
        }
        out.push_back('\'');
        return out;
    }

    BenchFixture(
        std::pair<std::filesystem::path, bool> rootSpec,
        size_t nPaths,
        size_t avgFilesPerPath,
        std::optional<size_t> nRootsOverride,
        FixtureSpec::Topology topology,
        size_t clusterSize)
        : root(std::move(rootSpec.first))
        , ownsRoot(rootSpec.second)
        , store(openAtRoot(root))
    {
        auto & local = this->local();
        FixtureSpec spec{
            .nPaths = nPaths,
            .avgFilesPerPath = avgFilesPerPath,
            .nRootsOverride = nRootsOverride,
            .topology = topology,
            .clusterSize = clusterSize,
        };
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

        const size_t platform = spec.platformSize();
        const std::string storeDir = (root / "nix/store").string();

        auto generateFileSpecs = [&](PathPlan & plan) -> uint64_t {
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
            return totalBytes;
        };

        auto emitPath = [&](size_t i, StorePathSet refs) {
            auto storePath = StorePath::random(fmt("optimise-bench-%zu", i));
            pathByIndex.push_back(storePath);
            PathPlan plan(storePath, spec.seed() ^ (uint64_t(i) * 0x9e3779b97f4a7c15ull));
            uint64_t totalBytes = generateFileSpecs(plan);
            ValidPathInfo info{plan.storePath, UnkeyedValidPathInfo(local, Hash::dummy)};
            info.narSize = totalBytes;
            info.references = std::move(refs);
            infos.emplace(plan.storePath, std::move(info));
            plans.push_back(std::move(plan));
        };

        std::vector<std::vector<size_t>> clusters;

        if (spec.topology == FixtureSpec::Topology::Barabasi) {
            std::vector<size_t> targets;
            targets.reserve(size_t(nPaths * 6 * 1.3));

            for (size_t i = 0; i < nPaths; ++i) {
                StorePathSet refs;

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
                    for (size_t idx : chosen) {
                        refs.insert(pathByIndex[idx]);
                        targets.push_back(idx);
                    }
                }

                emitPath(i, std::move(refs));

                if (i < platform)
                    targets.push_back(i);
            }
        } else {
            std::geometric_distribution<size_t> clusterSizeDist(
                1.0 / std::max<double>(2.0, double(spec.clusterSize)));
            std::poisson_distribution<size_t> baseRefDist(2.0);
            std::poisson_distribution<size_t> intraRefDist(1.0);

            for (size_t i = 0; i < platform; ++i)
                emitPath(i, {});

            std::uniform_int_distribution<size_t> baseRefPick(0, platform - 1);

            size_t i = platform;
            while (i < nPaths) {
                size_t want = 1 + clusterSizeDist(rng);
                want = std::min(want, nPaths - i);

                std::vector<size_t> body;
                body.reserve(want);

                for (size_t j = 0; j + 1 < want; ++j, ++i) {
                    StorePathSet refs;

                    size_t nBaseRefs = std::max<size_t>(1, baseRefDist(rng));
                    std::set<size_t> picked;
                    while (picked.size() < nBaseRefs && picked.size() < platform)
                        picked.insert(baseRefPick(rng));
                    for (auto idx : picked)
                        refs.insert(pathByIndex[idx]);

                    if (j > 0) {
                        size_t nIntra = std::min<size_t>(intraRefDist(rng), j);
                        if (nIntra > 0) {
                            std::uniform_int_distribution<size_t> intraPick(0, j - 1);
                            std::set<size_t> intraIdx;
                            while (intraIdx.size() < nIntra && intraIdx.size() < j)
                                intraIdx.insert(intraPick(rng));
                            for (auto idx : intraIdx)
                                refs.insert(pathByIndex[body[idx]]);
                        }
                    }

                    emitPath(i, std::move(refs));
                    body.push_back(i);
                }

                if (i < nPaths) {
                    StorePathSet refs;
                    for (size_t bodyIdx : body)
                        refs.insert(pathByIndex[bodyIdx]);
                    size_t nBaseRefs = std::max<size_t>(1, baseRefDist(rng));
                    std::set<size_t> picked;
                    while (picked.size() < nBaseRefs && picked.size() < platform)
                        picked.insert(baseRefPick(rng));
                    for (auto idx : picked)
                        refs.insert(pathByIndex[idx]);

                    emitPath(i, std::move(refs));
                    body.push_back(i);
                    ++i;
                }

                clusters.push_back(std::move(body));
            }
        }

        /* Phase 2: parallel file I/O */
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
                /* Mode 0444 at create-time avoids a follow-up chmod call. */
                AutoCloseFD fd = toDescriptor(::open(fp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0444));
                if (!fd)
                    throw SysError("open %s", fp);
                const auto & fs = plan.files[f];
                if (fs.blobIdx >= 0) {
                    const auto & blob = pool.blobs[fs.blobIdx];
                    writeFull(fd.get(), std::string_view(blob.data(), blob.size()));
                } else {
                    writeUniqueBytes(
                        fd.get(), fs.uniqueSize,
                        plan.fileSeedBase ^ (uint64_t(f) * 0xbf58476d1ce4e5b9ull));
                }
            }
        });
        });

        /* Phase 3: register paths and roots */
        local.registerValidPaths(infos);

        if (nPaths > platform) {
            auto rootsDir = root / "gcroots";
            std::filesystem::create_directories(rootsDir);
            std::set<size_t> rootIndices;

            if (spec.topology == FixtureSpec::Topology::Barabasi) {
                std::uniform_int_distribution<size_t> rootPick(platform, nPaths - 1);
                size_t want = std::min(spec.nRoots(), nPaths - platform);
                while (rootIndices.size() < want)
                    rootIndices.insert(rootPick(rng));
            } else if (!clusters.empty()) {
                size_t want = spec.liveClusters > 0
                    ? std::min(spec.liveClusters, clusters.size())
                    : std::max<size_t>(2, clusters.size() / 100);
                want = std::min(want, clusters.size());
                std::uniform_int_distribution<size_t> clusterPick(0, clusters.size() - 1);
                std::set<size_t> rootedClusters;
                while (rootedClusters.size() < want)
                    rootedClusters.insert(clusterPick(rng));
                for (size_t cIdx : rootedClusters)
                    rootIndices.insert(clusters[cIdx].back());
            }

            for (size_t idx : rootIndices) {
                auto link = rootsDir / fmt("r-%zu", idx);
                local.addPermRoot(pathByIndex[idx], link);
            }
        }
    }
};

} // namespace nix::bench

#endif // _WIN32
