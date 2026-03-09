#include "nix/expr/eval-trace/deps/hash.hh"
#include "nix/expr/eval-trace/deps/interning-pools.hh"
#include "nix/expr/eval-trace/deps/dep-hash-fns.hh"

#include <algorithm>
#include <ranges>

namespace nix::eval_trace {

// ── Canonical dep ordering (deterministic trace fingerprints) ────────

std::vector<Dep> sortAndDedupDeps(const std::vector<Dep> & deps)
{
    auto sorted = deps;
    std::ranges::sort(sorted);
    auto [first, last] = std::ranges::unique(sorted);
    sorted.erase(first, last);
    return sorted;
}

// ── Convenience overloads (resolve all IDs as strings) ───────────────

TraceHash computeTraceHashFromSorted(InterningPools & pools, const std::vector<Dep> & sortedDeps)
{
    return computeTraceHashFromSorted(sortedDeps, [&pools](HashSink & s, DepType, uint32_t idValue) {
        s(pools.resolve(DepKeyId(idValue)));
    });
}

TraceHash computeTraceHash(InterningPools & pools, const std::vector<Dep> & deps)
{
    return computeTraceHashFromSorted(pools, sortAndDedupDeps(deps));
}

StructHash computeTraceStructHashFromSorted(InterningPools & pools, const std::vector<Dep> & sortedDeps)
{
    return computeTraceStructHashFromSorted(sortedDeps, [&pools](HashSink & s, DepType, uint32_t idValue) {
        s(pools.resolve(DepKeyId(idValue)));
    });
}

StructHash computeTraceStructHash(InterningPools & pools, const std::vector<Dep> & deps)
{
    return computeTraceStructHashFromSorted(pools, sortAndDedupDeps(deps));
}


// ── Canonical keys hash ──────────────────────────────────────────────

Blake3Hash canonicalKeysHash(std::vector<std::string> keys)
{
    std::ranges::sort(keys);
    std::string canonical;
    for (size_t i = 0; i < keys.size(); i++) {
        if (i > 0) canonical += '\0';
        canonical += keys[i];
    }
    return depHash(canonical);
}

} // namespace nix::eval_trace
