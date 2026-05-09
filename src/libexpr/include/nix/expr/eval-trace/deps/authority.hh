#pragma once
/// deps/authority.hh — AuthorityGate + AuthorityState.
///
/// Split from trace-store.hh so consumers that only need to mint the phase-1
/// authority proof (libflake's lockFlake implementation) don't pull in the
/// full storage interface.

#include "nix/util/gdp/proof.hh"

#include <utility>

namespace nix::eval_trace {

/// Phase 1 authority state: fetcher caches, registry state, store mutation
/// authority. NOT part of semantic cache identity.
struct AuthorityState {};

/// GDP certifier for phase-1 authority. Only lockFlake's implementation
/// should call withAuthority().
class AuthorityGate : private gdp::Certifier<AuthorityState>
{
public:
    template<typename F>
    static auto withAuthority(F && f)
    {
        return Certifier::withProof(std::forward<F>(f));
    }
};

} // namespace nix::eval_trace
