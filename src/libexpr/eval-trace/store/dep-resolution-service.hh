#pragma once
/// dep-resolution-service.hh — Dep hash resolution.
///
/// resolveDepHash<TaggedDepType> computes the current hash for a single dep.
/// It is the shared leaf function for all dep resolution paths (typestate L3,
/// pre-population in verifyTrace, any future path).
///
/// Subsumption is enforced inside resolveDepHash.  Origin dispatch:
///   - CurrentTraceDep: when isFileVerified(traceId, F) holds and the kind is
///     Structural/ImplicitStructural, returns dep.hash directly AND writes
///     L1 as VerifiedHash (via the VerifiedSubsumption capability minted
///     here). The trace id is part of the proof, so a file verified by one
///     trace cannot authorize stored hashes from another trace.
///   - CandidateDep: the subsumption shortcut is NOT compiled; every call
///     falls through to the compute path, producing hash(op(current F)).
///     This keeps tryStructuralVariantRecovery honest — a candidate matches
///     history iff its deps resolve to the candidate's recorded trace hash
///     under the current filesystem, not via a trivial stored-vs-stored
///     comparison.  The capability factory is also SFINAE-restricted to
///     CurrentTrace origin, so historical-hash L1 poisoning is a compile
///     error.
///
/// L1 caching is internal: computed hashes are cached as ComputedHash,
/// subsumed hashes as VerifiedHash.  VerificationSession's write methods are
/// private — only resolveDepHash can write to L1.

#include "nix/expr/eval-trace/store/verification-session.hh"
#include "nix/expr/eval-trace/store/semantic-registry.hh"
#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/expr/eval-trace/deps/interning-pools.hh"
#include "nix/util/source-path.hh"

#include <boost/unordered/unordered_flat_map.hpp>

#include <optional>
#include <string>

namespace nix {
class EvalState;
}

namespace nix::eval_trace {

/// Compute the current hash for a single dep.
///
/// For CurrentTrace origin: checks trace-scoped subsumption first — if this
/// trace's file Content dep was verified unchanged and the kind is
/// Structural/ImplicitStructural, returns dep.hash without computation and
/// writes L1 as VerifiedHash.
///
/// For Candidate origin: the subsumption shortcut is compiled out; every
/// call runs the compute path and writes ComputedHash to L1 (sound: pure
/// function of current state).
///
/// Caches results in L1 internally: ComputedHash (compute path, any origin)
/// or VerifiedHash (subsumption path, CurrentTrace origin only).
///
/// TaggedDepType is CurrentTraceDep or CandidateDep.  Passing a raw Dep is
/// a compile error — caller must choose an origin.
template<typename TaggedDepType>
std::optional<DepHashValue> resolveDepHash(
    EvalState & state, VerificationSession & session, const TaggedDepType & dep,
    const SemanticRegistry & registry,
    const InterningPools & pools,
    ParseCaches & caches,
    const StrandToken<FileStrandTag> & tok);

} // namespace nix::eval_trace
