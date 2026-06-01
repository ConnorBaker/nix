#include <benchmark/benchmark.h>

#include "nix/store/local-store.hh"
#include "nix/store/store-open.hh"
#include "nix/store/content-address.hh"
#include "nix/util/file-system.hh"
#include "nix/util/hash.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/source-path.hh"

#ifndef _WIN32

#  include <filesystem>
#  include <set>

namespace nix {

/* Base-plus-overlay assembly (item (b) — the dirty-monorepo edit-loop win).
 *
 * Editing one file in an N-file working tree should NOT re-copy the whole
 * tree into the store: `assembleCAPathFromBase` reflinks/hardlinks the
 * N-1 unchanged files from an already-materialised committed base and
 * writes only the 1 changed file. These two benchmarks measure exactly
 * that contrast at the store-primitive level:
 *
 *   BM_FullCopyTree   — what master does on every dirty edit: addToStore
 *                       the whole N-file tree (N full file copies).
 *   BM_AssembleFromBase — what (b) does on edits 2..K: the base is already
 *                       in the store, so assemble (N-1 links + 1 write).
 *                       The base-materialisation cost is paused out (it is
 *                       paid ONCE across an edit session, not per edit).
 *
 * WHERE THIS WINS — read the numbers honestly. The assembler's saving is
 * "BYTES NOT COPIED for the unchanged majority", not raw syscall count:
 *   - On a reflinking store (btrfs/xfs): unchanged files share extents via
 *     `tryCloneFile`, so assembly is O(changed bytes), independent of tree
 *     size — the dramatic win.
 *   - On a real disk (HDD/SSD): the N-1 unchanged files become hardlinks
 *     (metadata only) instead of N full byte writes — assembly avoids the
 *     bulk write+read I/O, the practical monorepo win.
 *   - On tmpfs (a typical CI store): a "copy" is just memcpy in RAM, and
 *     assembly STILL walks the whole tree once for the re-hash guard, so
 *     wall-clock can be ~parity or slightly WORSE for tiny files — the
 *     copy was already free. We therefore use a moderately LARGE per-file
 *     size here so the byte-copy cost is visible even on tmpfs; with tiny
 *     files the two are indistinguishable and that is the honest result.
 * The unconditional, store-independent win is disk footprint: the
 * assembled path shares inodes/extents with the base instead of
 * duplicating the unchanged bytes (asserted in register-assembled-ca-path.cc
 * and the dirty-tree-assemble.sh functional test). */

namespace {

std::shared_ptr<LocalStore> freshStore(const std::filesystem::path & root)
{
    createDirs(root);
    std::shared_ptr<Store> store = openStore(fmt("local?root=%s", root.string()));
    auto local = std::dynamic_pointer_cast<LocalStore>(store);
    if (!local)
        throw Error("expected a LocalStore");
    return local;
}

/* Write an N-file tree (each file `fileBytes` of 'x') under `dir`, with one
   file (`changedIdx`) holding distinct content so it is the "edited" file. */
void writeTree(const std::filesystem::path & dir, size_t n, uint64_t fileBytes, std::optional<size_t> changedIdx)
{
    createDirs(dir / "pkgs");
    for (size_t i = 0; i < n; ++i) {
        std::string contents =
            (changedIdx && *changedIdx == i) ? std::string(fileBytes, 'y') : std::string(fileBytes, 'x');
        writeFile(dir / "pkgs" / fmt("f%d.txt", i), contents);
    }
}

} // namespace

/* FULL COPY: ingest the whole N-file tree via addToStore (master's
   per-dirty-edit cost). */
static void BM_FullCopyTree(benchmark::State & state)
{
    const size_t n = (size_t) state.range(0);
    /* Moderately large per file so the byte-copy cost dominates and the
       link-vs-copy divergence is visible even on a tmpfs store (see the
       header note); with tiny files a tmpfs copy is free and the two are
       indistinguishable. */
    const uint64_t fileBytes = 256 * 1024;

    for (auto _ : state) {
        state.PauseTiming();
        auto root = createTempDir();
        auto store = freshStore(root / "nix/store");
        auto treeDir = createTempDir();
        writeTree(treeDir, n, fileBytes, std::nullopt);
        auto accessor = makeFSSourceAccessor(treeDir);
        state.ResumeTiming();

        auto path = static_cast<Store &>(*store).addToStore(
            "source", SourcePath{accessor, CanonPath::root}, ContentAddressMethod::Raw::NixArchive);
        benchmark::DoNotOptimize(path);

        state.PauseTiming();
        store.reset();
        deletePath(root);
        deletePath(treeDir);
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * (int64_t) n);
}
BENCHMARK(BM_FullCopyTree)->Arg(16)->Arg(64)->Arg(256);

/* ASSEMBLE: base already in the store; one file edited; assemble the dirty
   path (N-1 links + 1 write). Base materialisation is paused out (it is
   amortised across an edit session). */
static void BM_AssembleFromBase(benchmark::State & state)
{
    const size_t n = (size_t) state.range(0);
    /* Moderately large per file so the byte-copy cost dominates and the
       link-vs-copy divergence is visible even on a tmpfs store (see the
       header note); with tiny files a tmpfs copy is free and the two are
       indistinguishable. */
    const uint64_t fileBytes = 256 * 1024;

    for (auto _ : state) {
        state.PauseTiming();
        auto root = createTempDir();
        auto store = freshStore(root / "nix/store");

        /* The committed base (all 'x'), materialised once. */
        auto baseDir = createTempDir();
        writeTree(baseDir, n, fileBytes, std::nullopt);
        auto base = static_cast<Store &>(*store).addToStore(
            "source", SourcePath{makeFSSourceAccessor(baseDir), CanonPath::root}, ContentAddressMethod::Raw::NixArchive);

        /* The dirty working tree: file 0 edited. */
        auto dirtyDir = createTempDir();
        writeTree(dirtyDir, n, fileBytes, /*changedIdx=*/0);
        auto dirtyAccessor = makeFSSourceAccessor(dirtyDir);
        std::set<CanonPath> changed{CanonPath("pkgs/f0.txt")};
        std::set<CanonPath> deleted{};
        state.ResumeTiming();

        auto path = store->assembleCAPathFromBase(base, dirtyAccessor, changed, deleted, "source", std::nullopt);
        benchmark::DoNotOptimize(path);

        state.PauseTiming();
        store.reset();
        deletePath(root);
        deletePath(baseDir);
        deletePath(dirtyDir);
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * (int64_t) n);
}
BENCHMARK(BM_AssembleFromBase)->Arg(16)->Arg(64)->Arg(256);

/* tryCloneFile vs a plain byte copy of one file (the per-unchanged-file
   primitive; on a CoW store this is O(1) regardless of size). */
static void BM_TryCloneFile(benchmark::State & state)
{
    const uint64_t fileBytes = (uint64_t) state.range(0);
    for (auto _ : state) {
        state.PauseTiming();
        auto dir = createTempDir();
        auto src = dir / "src";
        auto dst = dir / "dst";
        writeFile(src, std::string(fileBytes, 'x'));
        state.ResumeTiming();

        bool cloned = tryCloneFile(src, dst);
        benchmark::DoNotOptimize(cloned);

        state.PauseTiming();
        deletePath(dir);
        state.ResumeTiming();
    }
}
BENCHMARK(BM_TryCloneFile)->Arg(4 * 1024)->Arg(1024 * 1024);

} // namespace nix

#endif
