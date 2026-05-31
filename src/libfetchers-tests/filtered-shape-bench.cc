/* Microbenchmark for the filtered-shape walk (PROPOSAL.md §2.D).
 *
 * `collectFilteredShape` mirrors dumpPath's structural traversal but
 * accumulates a content-free shape digest + accepted-path set instead of
 * NAR bytes. It is the per-eval floor of a filtered source
 * (`lib.cleanSource ./.`): the structure must be walked and the user
 * filter run once to compute the shapeHash, even on a warm cache (only
 * the blob bytes are deferred). This measures that floor as a function of
 * tree size, so a regression in the walk (e.g. accidental re-walk, or a
 * per-node allocation) shows up.
 *
 * Single-tree regression guard; the cross-tree filtered-source story is
 * the `filtered` workload in benchmarks/.
 */

#include <benchmark/benchmark.h>

#include "nix/fetchers/filtered-shape.hh"
#include "nix/util/memory-source-accessor.hh"

namespace nix {

namespace {

/* Build an in-memory tree of `dirs` directories each holding `filesPerDir`
   small files — a stand-in for a source tree of size dirs*filesPerDir. */
ref<MemorySourceAccessor> makeTree(size_t dirs, size_t filesPerDir)
{
    auto m = make_ref<MemorySourceAccessor>();
    for (size_t d = 0; d < dirs; ++d) {
        for (size_t f = 0; f < filesPerDir; ++f) {
            auto p = CanonPath("d" + std::to_string(d)) / ("f" + std::to_string(f) + ".txt");
            m->addFile(p, "contents of file");
        }
    }
    return m;
}

} // namespace

/* Walk an accept-all filter over a tree of state.range(0) total entries.
   Reports items/sec so the per-entry cost is directly comparable across
   sizes. */
static void BM_CollectFilteredShape_AcceptAll(benchmark::State & state)
{
    const size_t total = (size_t) state.range(0);
    const size_t filesPerDir = 16;
    const size_t dirs = std::max<size_t>(1, total / filesPerDir);
    auto fs = makeTree(dirs, filesPerDir);
    PathFilter acceptAll = [](const std::string &) { return true; };

    for (auto _ : state) {
        auto shape = collectFilteredShape(*fs, CanonPath::root, acceptAll);
        benchmark::DoNotOptimize(shape.accepted);
        benchmark::DoNotOptimize(shape.shapeHash);
    }
    state.SetItemsProcessed(state.iterations() * (int64_t) (dirs * filesPerDir));
}
BENCHMARK(BM_CollectFilteredShape_AcceptAll)->Arg(64)->Arg(1'024)->Arg(8'192);

/* A realistic cleanSource-style filter (reject a dotfile prefix). The
   filter runs once per child; this captures the predicate-call overhead
   on top of the walk. */
static void BM_CollectFilteredShape_CleanSourceShape(benchmark::State & state)
{
    const size_t total = (size_t) state.range(0);
    const size_t filesPerDir = 16;
    const size_t dirs = std::max<size_t>(1, total / filesPerDir);
    auto fs = makeTree(dirs, filesPerDir);
    /* Reject paths whose basename starts with '.', the cleanSource idiom. */
    PathFilter clean = [](const std::string & p) {
        auto slash = p.rfind('/');
        auto base = slash == std::string::npos ? p : p.substr(slash + 1);
        return !base.empty() && base[0] != '.';
    };

    for (auto _ : state) {
        auto shape = collectFilteredShape(*fs, CanonPath::root, clean);
        benchmark::DoNotOptimize(shape.accepted);
    }
    state.SetItemsProcessed(state.iterations() * (int64_t) (dirs * filesPerDir));
}
BENCHMARK(BM_CollectFilteredShape_CleanSourceShape)->Arg(64)->Arg(1'024)->Arg(8'192);

} // namespace nix
