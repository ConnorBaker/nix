/* Microbenchmark for the operator-stack READ path (PROPOSAL.md §1.2–§1.4).
 *
 * Every read of a virtualised source walks the wrapper stack the factory built
 * (e.g. a filtered Subset = DirectorySynthesizer ∘ Restrict ∘ Translate). The
 * proposal's claim is that this is cheap — "pure forwards to a readImpl field,
 * no per-recipe dispatch". These benchmarks measure the per-`readFile`/`lstat`
 * overhead the operator stack adds over a bare accessor, so a regression that
 * makes a wrapper allocate or re-dispatch per read shows up.
 *
 *   BM_Read_BareMemory     — baseline: readFile straight through MemorySourceAccessor
 *   BM_Read_SubsetStack    — readFile through a sourceViewSubset (the
 *                            DirectorySynthesizer∘Restrict∘Translate stack)
 *   BM_Lstat_SubsetStack   — maybeLstat through the same (the synthesis path)
 *
 * Single-tree regression guard.
 */

#include <benchmark/benchmark.h>

#include "nix/util/memory-source-accessor.hh"
#include "nix/util/source-view.hh"
#include "nix/util/hash.hh"

namespace nix {

namespace {

/* A tree of `n` files under /sub, plus a deeper /sub/nested/* — enough paths
   that the accepted-set membership check in Restrict does real work. */
ref<MemorySourceAccessor> makeTree(size_t n)
{
    auto m = make_ref<MemorySourceAccessor>();
    for (size_t i = 0; i < n; ++i)
        m->addFile(CanonPath("sub") / ("f" + std::to_string(i) + ".txt"), "contents");
    return m;
}

std::set<CanonPath> acceptedUnder(size_t n)
{
    std::set<CanonPath> s;
    for (size_t i = 0; i < n; ++i)
        s.insert(CanonPath("/sub") / ("f" + std::to_string(i) + ".txt"));
    return s;
}

} // namespace

static void BM_Read_BareMemory(benchmark::State & state)
{
    const size_t n = (size_t) state.range(0);
    auto m = makeTree(n);
    auto path = CanonPath("sub") / ("f" + std::to_string(n / 2) + ".txt");
    for (auto _ : state) {
        auto s = m->readFile(path);
        benchmark::DoNotOptimize(s);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Read_BareMemory)->Arg(16)->Arg(256);

static void BM_Read_SubsetStack(benchmark::State & state)
{
    const size_t n = (size_t) state.range(0);
    auto m = makeTree(n);
    auto shape = hashString(HashAlgorithm::SHA256, "bench-shape");
    /* Subset over the whole /sub subtree → DirectorySynthesizer∘Restrict∘Translate. */
    auto view = sourceViewSubset(m, shape, acceptedUnder(n), CanonPath::root);
    /* The accepted set is /-rooted; read via the same namespace. */
    auto path = CanonPath("/sub") / ("f" + std::to_string(n / 2) + ".txt");
    for (auto _ : state) {
        auto s = view->readFile(path);
        benchmark::DoNotOptimize(s);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Read_SubsetStack)->Arg(16)->Arg(256);

static void BM_Lstat_SubsetStack(benchmark::State & state)
{
    const size_t n = (size_t) state.range(0);
    auto m = makeTree(n);
    auto shape = hashString(HashAlgorithm::SHA256, "bench-shape");
    auto view = sourceViewSubset(m, shape, acceptedUnder(n), CanonPath::root);
    /* lstat an intermediate dir — exercises DirectorySynthesizer's ancestor
       synthesis (the path that is NOT a pure forward). */
    auto dir = CanonPath("/sub");
    for (auto _ : state) {
        auto st = view->maybeLstat(dir);
        benchmark::DoNotOptimize(st);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Lstat_SubsetStack)->Arg(16)->Arg(256);

} // namespace nix
