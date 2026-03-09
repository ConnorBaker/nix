#pragma once

#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/util/hash.hh"

#include <concepts>
#include <string>
#include <vector>

namespace nix {
struct InterningPools;
}

namespace nix::eval_trace {

/**
 * Concept for dep key feeders: a callable that feeds a dep ID field into a HashSink.
 * For source: always resolves as a string (use a non-ParentContext type).
 * For key: resolves as string for most types, via vocab trie for ParentContext.
 * Replaces std::function<void(HashSink&, DepType, uint32_t)> for zero-cost abstraction.
 */
template<typename F>
concept DepKeyFeeder = requires(F f, HashSink & s, DepType t, uint32_t id) {
    { f(s, t, id) } -> std::same_as<void>;
};

/**
 * Sort deps by key (type, sourceId, keyId) and deduplicate by the same triple.
 * Returns a sorted+deduped copy. Produces a canonical dep ordering so that
 * trace hashes are deterministic regardless of the order in which deps were
 * collected during evaluation.
 */
std::vector<Dep> sortAndDedupDeps(const std::vector<Dep> & deps);

// ── Template hash computation (zero-cost KeyFeeder dispatch) ─────────

namespace detail {

inline void feedDepToSink(HashSink & sink, const Dep & dep, bool includeHash, auto && feedKey)
{
    auto typeStr = std::to_string(static_cast<int>(dep.key.type));
    sink(std::string_view("T", 1));
    sink(typeStr);
    sink(std::string_view("S", 1));
    feedKey(sink, DepType::Content, dep.key.sourceId.value);
    sink(std::string_view("K", 1));
    feedKey(sink, dep.key.type, dep.key.keyId.value);
    if (includeHash) {
        sink(std::string_view("H", 1));
        hashDepValue(sink, dep.hash);
    }
}

} // namespace detail

/**
 * Pre-sorted trace hash with KeyFeeder callback (primary API).
 * Computes BLAKE3 hash of all deps INCLUDING hash values.
 * ImplicitStructural deps (ImplicitShape, GitIdentity) are excluded.
 */
template<DepKeyFeeder F>
TraceHash computeTraceHashFromSorted(const std::vector<Dep> & sortedDeps, F && feedKey)
{
    HashSink sink(HashAlgorithm::BLAKE3);
    for (auto & dep : sortedDeps) {
        if (depKind(dep.key.type) == DepKind::ImplicitStructural)
            continue;
        detail::feedDepToSink(sink, dep, true, feedKey);
    }
    return TraceHash::fromSink(sink);
}

/**
 * Pre-sorted structural hash with KeyFeeder callback (primary API).
 * Computes BLAKE3 hash of all deps EXCLUDING hash values (structure only).
 * ImplicitStructural deps are excluded.
 */
template<DepKeyFeeder F>
StructHash computeTraceStructHashFromSorted(const std::vector<Dep> & sortedDeps, F && feedKey)
{
    HashSink sink(HashAlgorithm::BLAKE3);
    for (auto & dep : sortedDeps) {
        if (depKind(dep.key.type) == DepKind::ImplicitStructural)
            continue;
        detail::feedDepToSink(sink, dep, false, feedKey);
    }
    return StructHash::fromSink(sink);
}

/**
 * Convenience overloads: resolve all IDs as strings via InterningPools.
 * Used by tests and simple callers that don't need ParentContext vocab hashing.
 */
TraceHash computeTraceHash(InterningPools & pools, const std::vector<Dep> & deps);
StructHash computeTraceStructHash(InterningPools & pools, const std::vector<Dep> & deps);
TraceHash computeTraceHashFromSorted(InterningPools & pools, const std::vector<Dep> & sortedDeps);
StructHash computeTraceStructHashFromSorted(InterningPools & pools, const std::vector<Dep> & sortedDeps);

/**
 * Canonical #keys hash: sort keys, join with \0, BLAKE3.
 * Used for StructuredContent/ImplicitShape #keys deps.
 */
Blake3Hash canonicalKeysHash(std::vector<std::string> keys);

} // namespace nix::eval_trace
