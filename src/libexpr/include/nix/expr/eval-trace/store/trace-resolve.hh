#pragma once
/// store/trace-resolve.hh — Resolve a single Dep to display-friendly strings.
///
/// `resolveDep(pools, vocab, dep)` is a pure function over its inputs.
/// Lifted out of `TraceStore::resolveDep` (step 5 of
/// rearchitecture-proposal.md §14) so tests and future non-TraceStore
/// callers can use it without a live store.

#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/expr/eval-trace/deps/interning-pools.hh"
#include "nix/expr/eval-trace/store/attr-vocab-store.hh"
#include "nix/expr/eval-trace/store/trace-value-types.hh"

namespace nix::eval_trace {

/// Resolve a single Dep into a display-friendly `ResolvedDep` carrying
/// owned strings. `pools` and `vocab` provide the interning pools /
/// attr-path vocabulary used to decode the dep's typed-ID payload.
[[nodiscard]] ResolvedDep resolveDep(const InterningPools & pools, AttrVocabStore & vocab, const Dep & dep);

} // namespace nix::eval_trace
