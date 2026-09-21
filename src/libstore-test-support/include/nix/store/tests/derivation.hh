#pragma once
///@file

#include <rapidcheck/gen/Arbitrary.h>

#include "nix/store/derivations.hh"

#include "nix/store/tests/path.hh"
#include "nix/store/tests/derived-path.hh"

namespace rc {

/**
 * Generates an input-addressed derivation output, the kind that needs no
 * experimental feature.  Content-addressed, deferred and impure outputs are
 * not generated here; a test that needs them gates them behind the fixtures
 * that enable the features.
 */
template<>
struct Arbitrary<nix::derivation::Output>
{
    static Gen<nix::derivation::Output> arbitrary();
};

/**
 * Generates a structurally well-formed input-addressed `Derivation`: a
 * non-empty set of input-addressed outputs, arbitrary platform, builder,
 * arguments and environment (never containing the ATerm-reserved `__json`
 * key), input sources as opaque derived paths, no derivation inputs, and
 * no structured attributes.  Every such derivation is valid for `unparse`.
 */
template<>
struct Arbitrary<nix::Derivation>
{
    static Gen<nix::Derivation> arbitrary();
};

} // namespace rc
