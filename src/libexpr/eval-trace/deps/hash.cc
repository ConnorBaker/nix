#include "nix/expr/eval-trace/deps/hash.hh"

#include <algorithm>

namespace nix::eval_trace {

// ── Canonical dep ordering (deterministic trace fingerprints) ────────

std::vector<Dep> sortAndDedupDeps(const std::vector<Dep> & deps)
{
    auto sorted = deps;
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    return sorted;
}

// ── Trace hashing (BLAKE3 via HashSink, BSàlC §3.2 verifying traces) ──

static void feedDepToSink(HashSink & sink, const Dep & dep, bool includeHash)
{
    auto typeStr = std::to_string(static_cast<int>(dep.type));
    sink(std::string_view("T", 1));
    sink(typeStr);
    sink(std::string_view("S", 1));
    sink(dep.source);
    sink(std::string_view("K", 1));
    sink(dep.key);
    if (includeHash) {
        sink(std::string_view("H", 1));
        hashDepValue(sink, dep.expectedHash);
    }
}

Hash computeTraceHashFromSorted(const std::vector<Dep> & sortedDeps)
{
    HashSink sink(HashAlgorithm::BLAKE3);
    for (auto & dep : sortedDeps)
        feedDepToSink(sink, dep, true);
    return sink.finish().hash;
}

Hash computeTraceHash(const std::vector<Dep> & deps)
{
    return computeTraceHashFromSorted(sortAndDedupDeps(deps));
}

Hash computeTraceStructHashFromSorted(const std::vector<Dep> & sortedDeps)
{
    HashSink sink(HashAlgorithm::BLAKE3);
    for (auto & dep : sortedDeps)
        feedDepToSink(sink, dep, false);
    return sink.finish().hash;
}

Hash computeTraceStructHash(const std::vector<Dep> & deps)
{
    return computeTraceStructHashFromSorted(sortAndDedupDeps(deps));
}


} // namespace nix::eval_trace
