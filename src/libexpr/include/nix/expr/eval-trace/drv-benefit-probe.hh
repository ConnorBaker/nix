#pragma once
///@file
/// MEASUREMENT-ONLY probe for RFC §8 step 3, benefit-magnitude question
/// (`plans/derivation-producer-partition-rfc.md` §0.5 / follow-up #11 correction):
/// what FRACTION of a derivation's flattened dep closure would a producer-edge
/// remove? An edge removes the DERIVATION-CLOSURE deps (the input-reads captured
/// while evaluating `derivationStrict`); it does NOT remove the consumer's own
/// reads. The open question (35–90%, unpinned) is which dep KINDS dominate the
/// flattened range — in particular whether the plurality (StructProj, ~48% in the
/// arch-doc sample) is derivation-closure or consumer-intrinsic.
///
/// This probe answers it by tallying, per `derivationStrict` call, the dep-KIND
/// breakdown of the epoch-log range that grew during that call (the deps that
/// flatten into the consumer today and that an edge would replace). It does NOT
/// restructure the hot path — it reads the range the conservative hook already
/// snapshots. Env-gated on `NIX_MEASURE_DRV_BENEFIT=1`; no-op + ~zero cost
/// otherwise. Validate against a controlled case before trusting (the recurring
/// lesson — three prior probe confounds).

#include "nix/expr/eval-trace/deps/types.hh"

#include <cstdint>
#include <vector>

namespace nix::eval_trace::drv_benefit_probe {

// `Dep` lives in namespace `nix` (types.hh), included above.
using ::nix::Dep;

/// True iff `NIX_MEASURE_DRV_BENEFIT=1`. Cached. All other entry points no-op
/// when false.
bool enabled();

/// Tally the dep-kind breakdown of one derivation's flattened range. `deps` is
/// the epoch-log slice that grew during a `derivationStrict` call (what flattens
/// into the consumer and what a producer-edge would replace). Accumulates a
/// process-global per-kind histogram + per-derivation total. No-op when disabled.
void recordDerivationRange(const std::vector<Dep> & deps);

/// Dump the aggregate breakdown (per-kind dep counts + the implied
/// edge-removable fraction) at process exit. No-op when disabled.
void dump();

} // namespace nix::eval_trace::drv_benefit_probe
