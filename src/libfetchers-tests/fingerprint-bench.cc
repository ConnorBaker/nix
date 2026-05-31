/* Microbenchmarks for the fingerprint layer (PROPOSAL.md §1.1 / §2.B).
 *
 * `getFingerprint` is the single content-identity surface: one virtual
 * call produces a cache key, and everything downstream (the
 * sourcePathToHash / treeHashToNarHash projection lookups) keys on its
 * rendered string. These benchmarks measure the per-call cost of the
 * string machinery on that hot path:
 *
 *   - mergeFingerprintSuffix: the alphabetical-insert merge that makes
 *     wrapper-stack order irrelevant (`;e`/`;l`/`;s` flag composition).
 *   - blob/treeFingerprint: the structured-fingerprint constructors.
 *   - bareTreeOid: the canonical bare-`tree:` decoder used as the
 *     cross-pipeline bridge-key gate (must be total + cheap; it runs on
 *     every sourcePathToHash miss).
 *
 * Single-tree: these guard OUR mechanisms against regression; the
 * cross-tree comparison is the CLI harness in benchmarks/.
 */

#include <benchmark/benchmark.h>

#include "nix/util/fingerprint.hh"

namespace nix {

/* Merge one suffix into a bare fingerprint — the common case (a git
   subtree picking up an export-ignore flag). */
static void BM_MergeFingerprintSuffix_One(benchmark::State & state)
{
    std::string base = "tree:0123456789abcdef0123456789abcdef01234567";
    for (auto _ : state) {
        auto r = mergeFingerprintSuffix(base, ";e");
        benchmark::DoNotOptimize(r);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MergeFingerprintSuffix_One);

/* Merge several suffixes in REVERSE alphabetical order, so each insert
   hits the ordering path. This is the wrapper-stack-order-independence
   guarantee under load (PROPOSAL.md §2.B: "wrapper-stack-order
   independence falls out of the alphabetical merge"). */
static void BM_MergeFingerprintSuffix_Many(benchmark::State & state)
{
    for (auto _ : state) {
        std::string f = "tree:0123456789abcdef0123456789abcdef01234567";
        f = mergeFingerprintSuffix(f, ";s");
        f = mergeFingerprintSuffix(f, ";l=1");
        f = mergeFingerprintSuffix(f, ";e");
        benchmark::DoNotOptimize(f);
    }
    state.SetItemsProcessed(state.iterations() * 3);
}
BENCHMARK(BM_MergeFingerprintSuffix_Many);

static void BM_BlobFingerprint(benchmark::State & state)
{
    std::string oid = "89e5a3e0f7c1b2a4d6e8f0a2c4e6b8d0f2a4c6e8";
    for (auto _ : state) {
        auto r = blobFingerprint(oid, "100644");
        benchmark::DoNotOptimize(r);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_BlobFingerprint);

static void BM_TreeFingerprint(benchmark::State & state)
{
    std::string oid = "0123456789abcdef0123456789abcdef01234567";
    for (auto _ : state) {
        auto r = treeFingerprint(oid);
        benchmark::DoNotOptimize(r);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_TreeFingerprint);

/* The bridge-key gate: decode a bare `tree:<hex>` (hit) and reject a
   suffixed one (miss). Both must be cheap and total — this runs on every
   sourcePathToHash miss to decide whether the cross-pipeline
   treeHashToNarHash bridge is eligible (PROPOSAL.md §2.C / L-BridgeKeyCanonical). */
static void BM_BareTreeOid_Accept(benchmark::State & state)
{
    std::string bare = "tree:0123456789abcdef0123456789abcdef01234567";
    for (auto _ : state) {
        auto r = bareTreeOid(bare);
        benchmark::DoNotOptimize(r);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_BareTreeOid_Accept);

static void BM_BareTreeOid_Reject(benchmark::State & state)
{
    /* A suffixed fingerprint must decode to nullopt (not a bridge key). */
    std::string suffixed = "tree:0123456789abcdef0123456789abcdef01234567;e";
    for (auto _ : state) {
        auto r = bareTreeOid(suffixed);
        benchmark::DoNotOptimize(r);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_BareTreeOid_Reject);

} // namespace nix
