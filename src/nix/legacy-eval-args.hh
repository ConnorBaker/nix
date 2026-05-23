#pragma once
///@file

#include "nix/cmd/common-eval-args.hh"
#include "nix/main/shared.hh"

namespace nix {

/**
 * Argument-parser base shared by the legacy-CLI entry points
 * (`nix-build`, `nix-instantiate`, `nix-env`, `nix-prefetch-url`).
 *
 * Each of those tools previously declared an identical local
 * `struct MyArgs : LegacyArgs, MixEvalArgs { using LegacyArgs::LegacyArgs; }`.
 * Lift the boilerplate into one place. Tools that need extra hooks
 * (e.g. `nix-build` adds `setBaseDir` for shebang-relative
 * resolution) extend this class with their own members.
 */
struct LegacyEvalArgs : LegacyArgs, MixEvalArgs
{
    using LegacyArgs::LegacyArgs;
};

} // namespace nix
