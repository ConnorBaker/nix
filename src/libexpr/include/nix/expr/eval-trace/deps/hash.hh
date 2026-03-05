#pragma once

#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/util/hash.hh"

#include <functional>
#include <string>
#include <vector>

namespace nix {
struct InterningPools;
}

namespace nix::eval_trace {

/**
 * Callback to feed a dep ID field into a HashSink.
 * For source: always resolves as a string (use a non-ParentContext type).
 * For key: resolves as string for most types, via vocab trie for ParentContext.
 * The uint32_t is the raw value of a DepSourceId or DepKeyId.
 */
using KeyFeeder = std::function<void(HashSink & sink, DepType type, uint32_t idValue)>;

/**
 * Sort deps by key (type, sourceId, keyId) and deduplicate by the same triple.
 * Returns a sorted+deduped copy. Produces a canonical dep ordering so that
 * trace hashes are deterministic regardless of the order in which deps were
 * collected during evaluation.
 */
std::vector<Dep> sortAndDedupDeps(const std::vector<Dep> & deps);

/**
 * Pre-sorted trace hash with KeyFeeder callback (primary API).
 * Computes BLAKE3 hash of all deps INCLUDING hash values.
 * ImplicitStructural deps (ImplicitShape, GitIdentity) are excluded from
 * the hash — they don't affect evaluation results and excluding them keeps
 * trace_hash stable across benign changes (e.g. different git commits
 * that don't change any evaluated files), which prevents ParentContext
 * chain breakage.
 */
Hash computeTraceHashFromSorted(const std::vector<Dep> & sortedDeps, const KeyFeeder & feedKey);

/**
 * Pre-sorted structural hash with KeyFeeder callback (primary API).
 * Computes BLAKE3 hash of all deps EXCLUDING hash values (structure only).
 * ImplicitStructural deps are excluded (same rationale as computeTraceHashFromSorted).
 */
Hash computeTraceStructHashFromSorted(const std::vector<Dep> & sortedDeps, const KeyFeeder & feedKey);

/**
 * Convenience overloads: resolve all IDs as strings via InterningPools.
 * Used by tests and simple callers that don't need ParentContext vocab hashing.
 */
Hash computeTraceHash(InterningPools & pools, const std::vector<Dep> & deps);
Hash computeTraceStructHash(InterningPools & pools, const std::vector<Dep> & deps);
Hash computeTraceHashFromSorted(InterningPools & pools, const std::vector<Dep> & sortedDeps);
Hash computeTraceStructHashFromSorted(InterningPools & pools, const std::vector<Dep> & sortedDeps);

} // namespace nix::eval_trace
