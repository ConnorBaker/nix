#include "nix/expr/eval-trace/deps/hash.hh"
#include "nix/expr/eval-trace/deps/interning-pools.hh"
#include "nix/expr/eval-trace/deps/recording.hh"

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

static void feedDepToSink(HashSink & sink, const Dep & dep, bool includeHash,
                          const KeyFeeder & feedKey)
{
    auto typeStr = std::to_string(static_cast<int>(dep.key.type));
    sink(std::string_view("T", 1));
    sink(typeStr);
    sink(std::string_view("S", 1));
    // Source is always a string (even for ParentContext it's empty/"").
    // Feed via feedKey with a non-ParentContext type to ensure string resolution.
    feedKey(sink, DepType::Content, dep.key.sourceId.value);
    sink(std::string_view("K", 1));
    feedKey(sink, dep.key.type, dep.key.keyId.value);
    if (includeHash) {
        sink(std::string_view("H", 1));
        hashDepValue(sink, dep.hash);
    }
}

Hash computeTraceHashFromSorted(const std::vector<Dep> & sortedDeps, const KeyFeeder & feedKey)
{
    HashSink sink(HashAlgorithm::BLAKE3);
    for (auto & dep : sortedDeps) {
        if (depKind(dep.key.type) == DepKind::ImplicitStructural)
            continue;
        feedDepToSink(sink, dep, true, feedKey);
    }
    return sink.finish().hash;
}

Hash computeTraceStructHashFromSorted(const std::vector<Dep> & sortedDeps, const KeyFeeder & feedKey)
{
    HashSink sink(HashAlgorithm::BLAKE3);
    for (auto & dep : sortedDeps) {
        if (depKind(dep.key.type) == DepKind::ImplicitStructural)
            continue;
        feedDepToSink(sink, dep, false, feedKey);
    }
    return sink.finish().hash;
}

// ── Convenience overloads (resolve all IDs as strings) ───────────────

static KeyFeeder makePoolFeeder(InterningPools & pools)
{
    return [&pools](HashSink & s, DepType, uint32_t idValue) {
        s(pools.resolve(DepKeyId(idValue)));
    };
}

Hash computeTraceHashFromSorted(InterningPools & pools, const std::vector<Dep> & sortedDeps)
{
    return computeTraceHashFromSorted(sortedDeps, makePoolFeeder(pools));
}

Hash computeTraceHash(InterningPools & pools, const std::vector<Dep> & deps)
{
    return computeTraceHashFromSorted(pools, sortAndDedupDeps(deps));
}

Hash computeTraceStructHashFromSorted(InterningPools & pools, const std::vector<Dep> & sortedDeps)
{
    return computeTraceStructHashFromSorted(sortedDeps, makePoolFeeder(pools));
}

Hash computeTraceStructHash(InterningPools & pools, const std::vector<Dep> & deps)
{
    return computeTraceStructHashFromSorted(pools, sortAndDedupDeps(deps));
}


// ── Canonical keys hash ──────────────────────────────────────────────

Blake3Hash canonicalKeysHash(std::vector<std::string> keys)
{
    std::sort(keys.begin(), keys.end());
    std::string canonical;
    for (size_t i = 0; i < keys.size(); i++) {
        if (i > 0) canonical += '\0';
        canonical += keys[i];
    }
    return depHash(canonical);
}

} // namespace nix::eval_trace
