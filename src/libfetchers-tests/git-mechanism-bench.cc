/* Microbenchmarks for the two git mechanisms the coverage matrix flagged as
 * uncovered (PROPOSAL.md §2.G readBlob, §2.H synthesiseTree).
 *
 *   BM_SynthesiseTreeOid  — build a synthetic Git tree containing only an
 *     accepted subset, reading NO blob bytes (the cache-key-normalisation
 *     primitive). Swept over accepted-set size.
 *   BM_ReadBlob_Concurrent — N threads reading distinct blobs through one
 *     GitSourceAccessor, exercising the Phase-1 (locked tree walk) /
 *     Phase-2 (lock-free git_odb_read) split. Reports throughput; a
 *     regression that holds the State lock across the read would crater it.
 *
 * Single-tree regression guard. Builds a real on-disk repo via libgit2 (the
 * production path, no mocks), mirroring the git-fingerprint.cc test fixture.
 */

#include <benchmark/benchmark.h>

#include "nix/fetchers/git-utils.hh"
#include "nix/util/file-system.hh"
#include "nix/util/canon-path.hh"
#include "nix/util/hash.hh"

#include <git2/commit.h>
#include <git2/global.h>
#include <git2/index.h>
#include <git2/repository.h>
#include <git2/signature.h>
#include <git2/tree.h>

#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

namespace nix::fetchers {

namespace {

struct Repo
{
    std::filesystem::path dir;
    git_repository * repo = nullptr;
    Hash rootTree{HashAlgorithm::SHA1};
    Hash commit{HashAlgorithm::SHA1};

    /* Build a repo with `nFiles` files of `fileBytes` each under /sub, commit
       it, and record the root tree + commit OIDs. */
    Repo(size_t nFiles, size_t fileBytes)
    {
        dir = createTempDir();
        git_libgit2_init();
        if (git_repository_init(&repo, dir.string().c_str(), 0) != 0)
            throw Error("git_repository_init failed");
        std::string body(fileBytes, 'x');
        for (size_t i = 0; i < nFiles; ++i) {
            auto p = dir / "sub" / ("f" + std::to_string(i) + ".txt");
            std::filesystem::create_directories(p.parent_path());
            std::ofstream(p) << body << i; // +i so blobs are distinct
        }

        git_index * idx = nullptr;
        git_repository_index(&idx, repo);
        git_index_add_all(idx, nullptr, 0, nullptr, nullptr);
        git_oid treeOid;
        git_index_write_tree(&treeOid, idx);
        git_index_free(idx);

        git_tree * tree = nullptr;
        git_tree_lookup(&tree, repo, &treeOid);
        git_signature * sig = nullptr;
        git_signature_now(&sig, "Bench", "bench@example.com");
        git_oid commitOid;
        git_commit_create(&commitOid, repo, "HEAD", sig, sig, "UTF-8", "c", tree, 0, nullptr);
        git_signature_free(sig);
        git_tree_free(tree);

        rootTree = toHash(treeOid);
        commit = toHash(commitOid);
    }

    ~Repo()
    {
        if (repo)
            git_repository_free(repo);
        try {
            std::filesystem::remove_all(dir);
        } catch (...) {
        }
    }

    static Hash toHash(const git_oid & oid)
    {
        char buf[GIT_OID_SHA1_HEXSIZE + 1] = {0};
        git_oid_tostr(buf, sizeof(buf), &oid);
        return Hash::parseAny(std::string(buf), HashAlgorithm::SHA1);
    }
};

std::set<CanonPath> acceptedUnder(size_t n)
{
    std::set<CanonPath> s;
    for (size_t i = 0; i < n; ++i)
        s.insert(CanonPath("/sub") / ("f" + std::to_string(i) + ".txt"));
    return s;
}

} // namespace

/* synthesiseTreeOid: build a synthetic tree of an accepted subset, NO blob
   reads. Swept over subset size. (Non-flushing variant — the cache-key path.) */
static void BM_SynthesiseTreeOid(benchmark::State & state)
{
    const size_t n = (size_t) state.range(0);
    Repo r(n, 256);
    auto gitRepo = GitRepo::openRepo(r.dir, {});
    auto accepted = acceptedUnder(n);

    for (auto _ : state) {
        auto oid = gitRepo->synthesiseTreeOid(r.rootTree, accepted);
        benchmark::DoNotOptimize(oid);
    }
    state.SetItemsProcessed(state.iterations() * (int64_t) n);
}
BENCHMARK(BM_SynthesiseTreeOid)->Arg(16)->Arg(128)->Arg(512);

/* readBlob throughput single-threaded — the Phase-1 (locked walk) + Phase-2
   (lock-free odb read) baseline. */
static void BM_ReadBlob_Serial(benchmark::State & state)
{
    const size_t n = 256;
    Repo r(n, 4096);
    auto accessor = GitRepo::openRepo(r.dir, {})->getAccessor(r.commit, {}, "");
    size_t i = 0;
    for (auto _ : state) {
        auto s = accessor->readFile(CanonPath("/sub") / ("f" + std::to_string(i++ % n) + ".txt"));
        benchmark::DoNotOptimize(s);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ReadBlob_Serial);

/* readBlob concurrency: `threads` threads each read distinct blobs through one
   shared accessor. The Phase-1/2 split means only the brief tree walk is
   serialised; if the State lock were held across git_odb_read this would
   collapse toward serial throughput. Manual threads (not gbench's ThreadRange)
   so the shared accessor + fixed work is explicit. */
static void BM_ReadBlob_Concurrent(benchmark::State & state)
{
    const size_t n = 512;
    const size_t threads = (size_t) state.range(0);
    Repo r(n, 4096);
    auto accessor = GitRepo::openRepo(r.dir, {})->getAccessor(r.commit, {}, "");

    for (auto _ : state) {
        std::vector<std::thread> pool;
        const size_t perThread = 64;
        for (size_t t = 0; t < threads; ++t)
            pool.emplace_back([&, t] {
                for (size_t k = 0; k < perThread; ++k) {
                    auto idx = (t * perThread + k) % n;
                    auto s = accessor->readFile(CanonPath("/sub") / ("f" + std::to_string(idx) + ".txt"));
                    benchmark::DoNotOptimize(s);
                }
            });
        for (auto & th : pool)
            th.join();
    }
    state.SetItemsProcessed(state.iterations() * (int64_t) (threads * 64));
}
BENCHMARK(BM_ReadBlob_Concurrent)->Arg(1)->Arg(4)->Arg(8);

} // namespace nix::fetchers
