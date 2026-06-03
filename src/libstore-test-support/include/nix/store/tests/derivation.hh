#pragma once
///@file

#include <rapidcheck/gen/Arbitrary.h>

#include "nix/store/derivations.hh"

#include "nix/store/tests/path.hh"

namespace rc {

/**
 * Generates an `InputAddressed` derivation output (the default kind, which
 * needs no experimental feature). The `CAFixed`/`CAFloating`/`Deferred`/
 * `Impure` arms are intentionally *not* generated here: gate those behind
 * the `CaDerivationTest`/`DynDerivationTest` fixtures in a test that needs
 * them (see PROPOSAL-LAZY-DERIVATIONS.md §11).
 */
template<>
struct Arbitrary<nix::DerivationOutput>
{
    static Gen<nix::DerivationOutput> arbitrary();
};

/**
 * Generates a structurally well-formed input-addressed `Derivation`:
 * a non-empty set of `InputAddressed` outputs, arbitrary
 * platform/builder/args/env (env never contains the ATerm-reserved
 * `"__json"` key), arbitrary `inputSrcs`, and empty `inputDrvs`.
 *
 * This is sufficient for the content-addressing **projection laws**
 * (LD-P*), which exercise `computeStorePath`/`unparse` — i.e. the *hash of
 * the in-memory struct* — not buildability. A generator that produced
 * realisable derivations (correct input-addressed output paths, a valid
 * `inputDrvs` closure) is deliberately out of scope here.
 */
template<>
struct Arbitrary<nix::Derivation>
{
    static Gen<nix::Derivation> arbitrary();
};

} // namespace rc
