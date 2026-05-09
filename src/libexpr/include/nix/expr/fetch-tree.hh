#pragma once

#include "nix/expr/eval.hh"
#include "nix/expr/eval-environment/observation-types.hh"

namespace nix {

/**
 * Convert a libfetchers `Input` to libexpr `Value`.
 */
void emitTreeAttrs(
    EvalState & state,
    const StorePath & storePath,
    const fetchers::Input & input,
    Value & v,
    std::optional<DepSource> originSource = std::nullopt,
    bool emptyRevFallback = false,
    bool forceDirty = false);

} // namespace nix
