#include "nix/expr/eval-trace/deps/hash.hh"
#include "nix/expr/eval-trace/canonical-hash.hh"
#include "nix/expr/eval-trace/deps/input-resolution.hh"
#include "nix/expr/eval-trace/deps/interning-pools.hh"
#include "nix/expr/eval-trace/deps/dep-hash-fns.hh"

#include <algorithm>
#include <ranges>
#include <tuple>

namespace nix::eval_trace {

// ── Canonical dep ordering (deterministic trace fingerprints) ────────

std::vector<Dep> sortAndDedupDeps(const std::vector<Dep> & deps)
{
    auto sorted = deps;
    std::ranges::sort(sorted, [](const Dep & lhs, const Dep & rhs) {
        return std::tie(lhs.key, lhs.hash) < std::tie(rhs.key, rhs.hash);
    });
    auto [first, last] = std::ranges::unique(sorted, {}, [](const Dep & dep) {
        return std::tie(dep.key, dep.hash);
    });
    sorted.erase(first, last);
    return sorted;
}

// ── Convenience overloads (resolve all IDs as strings) ───────────────

// These InterningPools-based convenience overloads are test-only. They cannot
// encode TraceContext deps correctly (the production encoding routes through
// TraceStore::feedKey + vocab.feedPath for AttrPathId material; vocab is not
// threaded through these overloads). feedCanonicalDepKeyMaterial() will
// unreachable() on TraceContext keys, which is the desired behavior — it
// surfaces the unsupported combination at the feeder instead of silently
// producing wrong bytes. Tests that need to hash TraceContext deps must use
// the closure-form overloads directly and feed via TraceStore::feedKey.

TraceHash computeTraceHashFromSorted(InterningPools & pools, const std::vector<Dep> & sortedDeps)
{
    return computeTraceHashFromSorted(sortedDeps, [&pools](CanonicalHashBuilder & builder, const Dep::Key & key) {
        feedCanonicalDepKeyMaterial(builder, pools, key);
    });
}

TraceHash computeTraceHash(InterningPools & pools, const std::vector<Dep> & deps)
{
    return computeTraceHashFromSorted(pools, sortAndDedupDeps(deps));
}

StructHash computeTraceStructHashFromSorted(InterningPools & pools, const std::vector<Dep> & sortedDeps)
{
    return computeTraceStructHashFromSorted(sortedDeps, [&pools](CanonicalHashBuilder & builder, const Dep::Key & key) {
        feedCanonicalDepKeyMaterial(builder, pools, key);
    });
}

StructHash computeTraceStructHash(InterningPools & pools, const std::vector<Dep> & deps)
{
    return computeTraceStructHashFromSorted(pools, sortAndDedupDeps(deps));
}

FullTraceHash computeFullTraceHashFromSorted(InterningPools & pools, const std::vector<Dep> & sortedDeps)
{
    return computeFullTraceHashFromSorted(sortedDeps, [&pools](CanonicalHashBuilder & builder, const Dep::Key & key) {
        feedCanonicalDepKeyMaterial(builder, pools, key);
    });
}

FullTraceHash computeFullTraceHash(InterningPools & pools, const std::vector<Dep> & deps)
{
    return computeFullTraceHashFromSorted(pools, sortAndDedupDeps(deps));
}

DepKeySetHash computeDepKeySetHashFromSorted(InterningPools & pools, const std::vector<Dep> & sortedDeps)
{
    return computeDepKeySetHashFromSorted(sortedDeps, [&pools](CanonicalHashBuilder & builder, const Dep::Key & key) {
        feedCanonicalDepKeyMaterial(builder, pools, key);
    });
}

DepKeySetHash computeDepKeySetHash(InterningPools & pools, const std::vector<Dep> & deps)
{
    return computeDepKeySetHashFromSorted(pools, sortAndDedupDeps(deps));
}


// ── Canonical keys hash ──────────────────────────────────────────────

DepHash canonicalKeysHash(std::vector<std::string> keys)
{
    std::ranges::sort(keys);
    auto builder = makeDomainBuilder<hash_domain::CanonicalKeysHash>();
    builder.field("key-count", static_cast<uint64_t>(keys.size()));
    for (const auto & key : keys)
        builder.field("key", key);
    return DepHash{builder.finish()};
}

} // namespace nix::eval_trace
