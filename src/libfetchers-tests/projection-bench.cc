/* Microbenchmark for projection key encoding (PROPOSAL.md §0 / §2.A).
 *
 * Every content-keyed cache in the system is a `Projection<>` instance,
 * and every lookup/upsert first runs `Derived::toKey(from)` to build the
 * `(domain, Attrs)` key. `toKey` is therefore on the hot path of the
 * fetcher cache. These benchmarks measure the key-construction cost for
 * the two projections most exercised by the source-materialisation work:
 *
 *   - FilteredSourcePathToHash: keyed on
 *     (sourceFingerprint, method, subpath, shapeHash) — the filtered
 *     source cache (the `;shape` dedup key, PROPOSAL.md §6.2).
 *   - SourcePathToHash: the most general one,
 *     (fingerprint, method, subpath) — every addPath/fetchToStore.
 *
 * Single-tree regression guard.
 */

#include <benchmark/benchmark.h>

#include "nix/fetchers/filtered-shape.hh"
#include "nix/fetchers/projection.hh"
#include "nix/util/hash.hh"

namespace nix::fetchers {

static void BM_FilteredSourceKey_ToKey(benchmark::State & state)
{
    FilteredSourceKey k{
        .sourceFingerprint = "git:0123456789abcdef0123456789abcdef01234567",
        .method = "nar",
        .subpath = "/some/nested/subpath",
        .shapeHash = "sha256-3b8f1c2d4e6a8b0c2d4e6f8a0b2c4d6e8f0a2b4c6d8e0f1a2b3c4d5e6f7a8b9c0",
    };
    for (auto _ : state) {
        auto key = FilteredSourcePathToHash::toKey(k);
        benchmark::DoNotOptimize(key);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FilteredSourceKey_ToKey);

/* Round-trip the value codec (Hash → Attrs → Hash): what an upsert + a
   subsequent read pay on the value side. */
static void BM_FilteredSourceValue_RoundTrip(benchmark::State & state)
{
    auto h = hashString(HashAlgorithm::SHA256, "filtered-result-bytes");
    for (auto _ : state) {
        auto attrs = FilteredSourcePathToHash::toValue(h);
        auto back = FilteredSourcePathToHash::fromValue(attrs);
        benchmark::DoNotOptimize(back);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FilteredSourceValue_RoundTrip);

} // namespace nix::fetchers
