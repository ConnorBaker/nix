#pragma once

#include "nix/expr/eval-cache.hh"
#include "nix/expr/file-load-tracker.hh"
#include "nix/util/hash.hh"

#include <string>
#include <vector>

namespace nix::eval_cache {

/**
 * Compute a content hash of sorted+deduped deps (without result/parent/context).
 *
 * Used by the dep hash recovery index: coldStore records this hash,
 * recovery recomputes it from current dep hashes to find matching traces.
 * Two dep sets with identical (type, source, key, hash) entries produce
 * the same content hash regardless of input order or duplicates.
 */
Hash computeDepContentHash(const std::vector<Dep> & deps);

/**
 * Compute a structural hash of deps (type, source, key only — no expectedHash).
 * Two dep sets with the same keys but different values produce the same struct hash.
 * Used for struct-group recovery (Phase 3).
 */
Hash computeDepStructHash(const std::vector<Dep> & deps);

/**
 * Compute a dep content hash that includes parent identity (parent dep content hash).
 * Appends "P" + 32-byte parent hash to the hash input.
 * Used for parent-aware recovery (Phase 2).
 */
Hash computeDepContentHashWithParent(
    const std::vector<Dep> & deps,
    const Hash & parentDepContentHash);

/**
 * Sort deps by (type, source, key) and deduplicate by the same triple.
 * Deterministic output regardless of dep collection order.
 */
std::vector<Dep> sortAndDedupDeps(const std::vector<Dep> & deps);

/**
 * Pre-sorted variants: skip internal sort+dedup (caller must provide sorted deps).
 * Used by coldStore() which sorts once and calls multiple hash functions.
 */
Hash computeDepContentHashFromSorted(const std::vector<Dep> & sortedDeps);
Hash computeDepStructHashFromSorted(const std::vector<Dep> & sortedDeps);
Hash computeDepContentHashWithParentFromSorted(
    const std::vector<Dep> & sortedDeps,
    const Hash & parentDepContentHash);

} // namespace nix::eval_cache
