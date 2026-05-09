#pragma once
///@file
/// Private helper for libflake: hash a `fetchers::Attrs` map into a
/// `CanonicalHashBuilder` with stable length-framed, domain-separated
/// framing. Used by both `LockedFlake::getFingerprint` (in `flake.cc`)
/// and `computeResolvedGraphDigest` (in
/// `eval-trace-session-open-adapter.cc`); both are canonical-hash inputs
/// that must stay byte-identical across call sites. Not installed.

#include "nix/expr/eval-trace/canonical-hash.hh"
#include "nix/fetchers/attrs.hh"
#include "nix/util/util.hh"

#include <variant>

namespace nix::flake {

inline void appendFetcherAttrs(
    eval_trace::CanonicalHashBuilder & builder, const fetchers::Attrs & attrs)
{
    builder.field("attr-count", static_cast<uint64_t>(attrs.size()));
    for (const auto & [name, attr] : attrs) {
        builder.field("attr-name", name);
        std::visit(
            overloaded{
                [&](const std::string & value) {
                    builder.field("attr-type", "string");
                    builder.field("attr-value", value);
                },
                [&](uint64_t value) {
                    builder.field("attr-type", "uint64");
                    builder.field("attr-value", value);
                },
                [&](Explicit<bool> value) {
                    builder.field("attr-type", "bool");
                    builder.field("attr-value", value.t);
                },
                [&](const fetchers::LazyAttr &) {
                    // A lazy attr's value is functionally determined by
                    // other attrs (typically `rev`) that are already in
                    // the hash. Record only its presence — forcing the
                    // computation during fingerprinting would defeat
                    // the point of laziness. This means lazy and
                    // concrete attrs produce distinct fingerprints.
                    builder.field("attr-type", "lazy");
                },
            },
            attr);
    }
}

} // namespace nix::flake
