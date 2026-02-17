#pragma once

#include "nix/expr/trace-cache.hh"
#include "nix/expr/dep-tracker.hh"
#include "nix/util/hash.hh"

#include <string>
#include <vector>

namespace nix::eval_trace {

/**
 * Compute a trace hash over sorted+deduped deps (excluding result, parent, and context).
 *
 * In Build Systems à la Carte (BSàlC §3.2), a verifying trace stores the key
 * together with hashes of its dependencies and result. This function computes
 * the dependency portion of that trace fingerprint: a single hash summarising
 * all (type, source, key, expectedHash) tuples. During verification (warm
 * path), the current trace hash is compared against the recorded one to decide
 * whether the cached result is still valid. During constructive recovery
 * (BSàlC §3.4), the trace hash is recomputed from current dep hashes and used
 * to locate a matching trace whose result can be reused.
 *
 * The hash is order-independent: two traces with identical dep tuples produce
 * the same trace hash regardless of collection order or duplicates, since deps
 * are sorted and deduped before hashing.
 */
Hash computeTraceHash(const std::vector<Dep> & deps);

/**
 * Compute a structural hash of a trace's deps (type, source, key only — no
 * expectedHash). Two traces with the same dep keys but different dep values
 * produce the same structural hash. This captures the "shape" of the
 * dependency graph without its content (Adapton: the structure of a
 * demanded computation graph (DCG) node, ignoring computed values).
 *
 * Used for structural-group constructive recovery (Phase 3): traces are
 * grouped by structural hash so that when a trace hash miss occurs, we can
 * scan only structurally-equivalent traces and recompute their trace hashes
 * against current dep values (BSàlC §3.4, constructive traces with
 * structural equivalence classes).
 */
Hash computeTraceStructHash(const std::vector<Dep> & deps);

/**
 * Compute a trace hash that includes the parent's trace_hash, forming a
 * Merkle chain (analogous to Salsa's versioned query with context). The
 * parent hash is domain-separated ("P" prefix + 32-byte hash) and appended
 * to the dep-hash stream.
 *
 * Used for parent-aware constructive recovery within Phase 1: when a plain
 * trace hash lookup is ambiguous (e.g., multiple traces share an empty dep
 * list), incorporating the parent's identity disambiguates which historical
 * trace corresponds to the current evaluation context.
 */
Hash computeTraceHashWithParent(
    const std::vector<Dep> & deps,
    const Hash & parentDepContentHash);

/**
 * Sort deps by (type, source, key) and deduplicate by the same triple.
 * Produces a canonical dep ordering so that trace hashes are deterministic
 * regardless of the order in which deps were collected during evaluation.
 */
std::vector<Dep> sortAndDedupDeps(const std::vector<Dep> & deps);

/**
 * Pre-sorted variants: skip internal sort+dedup (caller must provide
 * canonically sorted deps). Used by trace recording (BSàlC: "record"),
 * which sorts once and computes multiple hash functions — trace hash,
 * structural hash, and parent-aware trace hash — in a single pass.
 */
Hash computeTraceHashFromSorted(const std::vector<Dep> & sortedDeps);
Hash computeTraceStructHashFromSorted(const std::vector<Dep> & sortedDeps);
Hash computeTraceHashWithParentFromSorted(
    const std::vector<Dep> & sortedDeps,
    const Hash & parentDepContentHash);

} // namespace nix::eval_trace
