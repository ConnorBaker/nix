#include "nix/expr/eval-result-serialise.hh"

#include <algorithm>

namespace nix::eval_cache {

// ── Shared dep sort+dedup ────────────────────────────────────────────

std::vector<Dep> sortAndDedupDeps(const std::vector<Dep> & deps)
{
    auto sorted = deps;
    std::sort(sorted.begin(), sorted.end(),
        [](const Dep & a, const Dep & b) {
            if (a.type != b.type) return a.type < b.type;
            if (a.source != b.source) return a.source < b.source;
            return a.key < b.key;
        });
    sorted.erase(std::unique(sorted.begin(), sorted.end(),
        [](const Dep & a, const Dep & b) {
            return a.type == b.type && a.source == b.source && a.key == b.key;
        }), sorted.end());
    return sorted;
}

// ── Dep content hash (HashSink-based) ───────────────────────────────

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

Hash computeDepContentHashFromSorted(const std::vector<Dep> & sortedDeps)
{
    HashSink sink(HashAlgorithm::SHA256);
    for (auto & dep : sortedDeps)
        feedDepToSink(sink, dep, true);
    return sink.finish().hash;
}

Hash computeDepContentHash(const std::vector<Dep> & deps)
{
    return computeDepContentHashFromSorted(sortAndDedupDeps(deps));
}

Hash computeDepStructHashFromSorted(const std::vector<Dep> & sortedDeps)
{
    HashSink sink(HashAlgorithm::SHA256);
    for (auto & dep : sortedDeps)
        feedDepToSink(sink, dep, false);
    return sink.finish().hash;
}

Hash computeDepStructHash(const std::vector<Dep> & deps)
{
    return computeDepStructHashFromSorted(sortAndDedupDeps(deps));
}

Hash computeDepContentHashWithParentFromSorted(
    const std::vector<Dep> & sortedDeps,
    const Hash & parentDepContentHash)
{
    HashSink sink(HashAlgorithm::SHA256);
    for (auto & dep : sortedDeps)
        feedDepToSink(sink, dep, true);
    // Domain-separated parent identity (parent's dep content hash)
    sink(std::string_view("P", 1));
    auto parentHex = parentDepContentHash.to_string(HashFormat::Base16, false);
    sink(parentHex);
    return sink.finish().hash;
}

Hash computeDepContentHashWithParent(
    const std::vector<Dep> & deps,
    const Hash & parentDepContentHash)
{
    return computeDepContentHashWithParentFromSorted(sortAndDedupDeps(deps), parentDepContentHash);
}

} // namespace nix::eval_cache
