#pragma once

#include "nix/expr/eval-trace/canonical-hash.hh"
#include "nix/expr/eval-trace/deps/types.hh"

#include <algorithm>
#include <concepts>
#include <ranges>
#include <string>
#include <vector>

namespace nix {
struct InterningPools;
}

namespace nix::eval_trace {

/**
 * Concept for dep key feeders: a callable that feeds a dep key into a framed hash builder.
 * For source: always resolves as a string (use a non-trace-context type).
 * For key: resolves as string for most types, via vocab trie for trace-context deps.
 * Replaces std::function<void(CanonicalHashBuilder&, Dep::Key)> for zero-cost abstraction.
 */
template<typename F>
concept DepKeyFeeder = requires(F f, CanonicalHashBuilder & builder, const Dep::Key & key) {
    { f(builder, key) } -> std::same_as<void>;
};

/**
 * Sort deps by key + observed value and drop only exact duplicates.
 * Returns a canonical dep ordering so trace hashes are deterministic even when
 * conflicting observations of the same dep key are present.
 */
std::vector<Dep> sortAndDedupDeps(const std::vector<Dep> & deps);

// ── Template hash computation (zero-cost KeyFeeder dispatch) ─────────

namespace detail {

inline size_t hashableDepCount(const std::vector<Dep> & sortedDeps)
{
    return std::ranges::count_if(sortedDeps, [](const Dep & dep) {
        return contributesToTraceHash(dep.key.kind);
    });
}

inline void feedDepValue(CanonicalHashBuilder & builder, const DepHashValue & value)
{
    std::visit(overloaded{
        [&](const DepHash & hash) {
            builder.field("dep.value.type", std::string_view("digest"));
            builder.field("dep.value.digest", hash);
        },
        [&](const std::string & string) {
            builder.field("dep.value.type", std::string_view("string"));
            builder.field("dep.value.string", std::string_view(string));
        },
    }, value);
}

inline void feedDep(
    CanonicalHashBuilder & builder,
    uint64_t ordinal,
    const Dep & dep,
    bool includeHash,
    const auto & feedKey)
{
    builder.field("dep.ordinal", ordinal);
    feedKey(builder, dep.key);
    if (includeHash)
        feedDepValue(builder, dep.hash);
}

} // namespace detail

/**
 * Pre-sorted trace hash with KeyFeeder callback (primary API).
 * Computes the canonical framed active-backend hash of all deps INCLUDING hash values.
 * ImplicitStructural deps (ImplicitShape, GitIdentity) are excluded.
 */
template<DepKeyFeeder F>
TraceHash computeTraceHashFromSorted(const std::vector<Dep> & sortedDeps, const F & feedKey)
{
    auto builder = makeDomainBuilder<hash_domain::TraceHashV2>();
    builder.field("dep-count", static_cast<uint64_t>(detail::hashableDepCount(sortedDeps)));
    uint64_t ordinal = 0;
    for (auto & dep : sortedDeps) {
        if (!contributesToTraceHash(dep.key.kind))
            continue;
        detail::feedDep(builder, ordinal++, dep, true, feedKey);
    }
    return TraceHash{builder.finish()};
}

/**
 * Pre-sorted full trace storage hash with KeyFeeder callback.
 * Computes the framed active-backend hash of every dep key and value,
 * including ImplicitStructural guard deps. This is the trace-row storage
 * identity; canonical TraceHash remains the recovery/result-matching hash.
 */
template<DepKeyFeeder F>
FullTraceHash computeFullTraceHashFromSorted(const std::vector<Dep> & sortedDeps, const F & feedKey)
{
    auto builder = makeDomainBuilder<hash_domain::FullTraceHashV1>();
    builder.field("dep-count", static_cast<uint64_t>(sortedDeps.size()));
    uint64_t ordinal = 0;
    for (auto & dep : sortedDeps)
        detail::feedDep(builder, ordinal++, dep, true, feedKey);
    return FullTraceHash{builder.finish()};
}

/**
 * Pre-sorted structural hash with KeyFeeder callback (primary API).
 * Computes the canonical framed active-backend hash of all deps EXCLUDING hash
 * values (structure only).
 * ImplicitStructural deps are excluded.
 */
template<DepKeyFeeder F>
StructHash computeTraceStructHashFromSorted(const std::vector<Dep> & sortedDeps, const F & feedKey)
{
    auto builder = makeDomainBuilder<hash_domain::StructHashV2>();
    builder.field("dep-count", static_cast<uint64_t>(detail::hashableDepCount(sortedDeps)));
    uint64_t ordinal = 0;
    for (auto & dep : sortedDeps) {
        if (!contributesToTraceHash(dep.key.kind))
            continue;
        detail::feedDep(builder, ordinal++, dep, false, feedKey);
    }
    return StructHash{builder.finish()};
}

/**
 * Pre-sorted exact dep-key-set storage hash with KeyFeeder callback.
 * Computes the framed active-backend hash of every dep key, including
 * ImplicitStructural guard deps. DepKeySetId is exact only when keyed by this
 * hash; canonical StructHash intentionally excludes implicit guards.
 */
template<DepKeyFeeder F>
DepKeySetHash computeDepKeySetHashFromSorted(const std::vector<Dep> & sortedDeps, const F & feedKey)
{
    auto builder = makeDomainBuilder<hash_domain::DepKeySetHashV1>();
    builder.field("dep-count", static_cast<uint64_t>(sortedDeps.size()));
    uint64_t ordinal = 0;
    for (auto & dep : sortedDeps)
        detail::feedDep(builder, ordinal++, dep, false, feedKey);
    return DepKeySetHash{builder.finish()};
}

/**
 * Convenience overloads: resolve all IDs as strings via InterningPools.
 * Used by tests and simple callers that don't need trace-context vocab hashing.
 */
TraceHash computeTraceHash(InterningPools & pools, const std::vector<Dep> & deps);
StructHash computeTraceStructHash(InterningPools & pools, const std::vector<Dep> & deps);
TraceHash computeTraceHashFromSorted(InterningPools & pools, const std::vector<Dep> & sortedDeps);
StructHash computeTraceStructHashFromSorted(InterningPools & pools, const std::vector<Dep> & sortedDeps);
FullTraceHash computeFullTraceHash(InterningPools & pools, const std::vector<Dep> & deps);
FullTraceHash computeFullTraceHashFromSorted(InterningPools & pools, const std::vector<Dep> & sortedDeps);
DepKeySetHash computeDepKeySetHash(InterningPools & pools, const std::vector<Dep> & deps);
DepKeySetHash computeDepKeySetHashFromSorted(InterningPools & pools, const std::vector<Dep> & sortedDeps);

/**
 * Canonical #keys hash: sort keys and feed them into the active eval-trace hash backend.
 * Used for StructuredContent/ImplicitShape #keys deps.
 */
DepHash canonicalKeysHash(std::vector<std::string> keys);

} // namespace nix::eval_trace
