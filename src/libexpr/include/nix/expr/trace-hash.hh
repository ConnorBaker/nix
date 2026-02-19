#pragma once

#include "nix/expr/eval-trace-deps.hh"
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
 * Used for structural variant recovery: traces are grouped by structural
 * hash so that when a trace hash miss occurs, we can scan only
 * structurally-equivalent traces and recompute their trace hashes against
 * current dep values (BSàlC §3.4, constructive traces with structural
 * equivalence classes).
 */
Hash computeTraceStructHash(const std::vector<Dep> & deps);

/**
 * Sort deps by (type, source, key) and deduplicate by the same triple.
 * Produces a canonical dep ordering so that trace hashes are deterministic
 * regardless of the order in which deps were collected during evaluation.
 */
std::vector<Dep> sortAndDedupDeps(const std::vector<Dep> & deps);

/**
 * Pre-sorted variants: skip internal sort+dedup (caller must provide
 * canonically sorted deps). Used by trace recording (BSàlC: "record"),
 * which sorts once and computes both trace hash and structural hash.
 */
Hash computeTraceHashFromSorted(const std::vector<Dep> & sortedDeps);
Hash computeTraceStructHashFromSorted(const std::vector<Dep> & sortedDeps);

} // namespace nix::eval_trace
