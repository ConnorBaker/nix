#include "nix/expr/eval-trace/deps/hash.hh"
#include "nix/expr/eval-trace/deps/interning-pools.hh"

#include <algorithm>

namespace nix::eval_trace {

// ── Canonical dep ordering (deterministic trace fingerprints) ────────

std::vector<Dep> sortAndDedupDeps(const std::vector<Dep> & deps)
{
    auto sorted = deps;
    std::sort(sorted.begin(), sorted.end(),
        [](const Dep & a, const Dep & b) {
            if (auto cmp = a.type <=> b.type; cmp != 0) return cmp < 0;
            if (a.sourceId != b.sourceId) return a.sourceId < b.sourceId;
            return a.keyId < b.keyId;
        });
    sorted.erase(std::unique(sorted.begin(), sorted.end(),
        [](const Dep & a, const Dep & b) {
            return a.type == b.type && a.sourceId == b.sourceId && a.keyId == b.keyId;
        }), sorted.end());
    return sorted;
}

// ── Trace hashing (BLAKE3 via HashSink, BSàlC §3.2 verifying traces) ──

static void feedDepToSink(HashSink & sink, InterningPools & pools, const Dep & dep, bool includeHash)
{
    auto typeStr = std::to_string(static_cast<int>(dep.type));
    sink(std::string_view("T", 1));
    sink(typeStr);
    sink(std::string_view("S", 1));
    sink(pools.resolve(dep.sourceId));
    sink(std::string_view("K", 1));
    sink(pools.resolve(dep.keyId));
    if (includeHash) {
        sink(std::string_view("H", 1));
        hashDepValue(sink, dep.expectedHash);
    }
}

Hash computeTraceHashFromSorted(InterningPools & pools, const std::vector<Dep> & sortedDeps)
{
    HashSink sink(HashAlgorithm::BLAKE3);
    for (auto & dep : sortedDeps)
        feedDepToSink(sink, pools, dep, true);
    return sink.finish().hash;
}

Hash computeTraceHash(InterningPools & pools, const std::vector<Dep> & deps)
{
    return computeTraceHashFromSorted(pools, sortAndDedupDeps(deps));
}

Hash computeTraceStructHashFromSorted(InterningPools & pools, const std::vector<Dep> & sortedDeps)
{
    HashSink sink(HashAlgorithm::BLAKE3);
    for (auto & dep : sortedDeps)
        feedDepToSink(sink, pools, dep, false);
    return sink.finish().hash;
}

Hash computeTraceStructHash(InterningPools & pools, const std::vector<Dep> & deps)
{
    return computeTraceStructHashFromSorted(pools, sortAndDedupDeps(deps));
}


} // namespace nix::eval_trace
