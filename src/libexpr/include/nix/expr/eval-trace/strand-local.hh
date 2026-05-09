#pragma once
/// strand-local.hh — GDP token proving strand affinity.
///
/// StrandLocal<T, Tag> wraps data that should only be accessed from a
/// specific strand (or strand-equivalent execution context like a
/// blocking pool thread). Access requires a StrandToken<Tag>, which
/// can only be created by a Certifier<Tag> (the class that owns the strand).
///
/// WHAT BECOMES A COMPILE ERROR:
///   parseCaches.jsonDomCache.access();         // missing StrandToken
///   parseCaches.jsonDomCache.access(gitToken); // wrong tag type
///   prefetchPool_.access();                     // missing StrandToken

#include "nix/util/gdp/proof.hh"
#include "nix/util/gdp/proof-guarded.hh"

namespace nix::eval_trace {

/// GDP proof that code is running on a strand with the given Tag.
template<typename Tag>
using StrandToken = gdp::Proof<Tag>;

/// Data that can only be accessed with the matching StrandToken<Tag>.
template<typename T, typename Tag>
using StrandLocal = gdp::ProofGuarded<T, Tag>;

// Tag types for specific strands
struct FileStrandTag {};
struct VerificationAccessTag {};

} // namespace nix::eval_trace
