#pragma once
///@file

#include "nix/cmd/command.hh"

namespace nix {

/**
 * Append `cmd.SourceExprCommand::getDefaultFlakeAttrPaths()` after the
 * caller's `prefixed` entries, returning the combined list.
 *
 * Used by the modern flake-installable commands whose default
 * attribute-path search shape is "look at my command-specific
 * `<output>.<system>.default` / `default<Output>.<system>` entries
 * first, then fall back to the base flake outputs": `nix run`
 * (`apps`/`defaultApp`), `nix bundle` (same), and the `nix develop`
 * family (`devShells`/`devShell`).
 *
 * Commands whose default-attr-path list is *not* "mine then base"
 * (e.g. `nix search`, `nix formatter`, and `nix repl`, which
 * deliberately replace the base list rather than augment it) keep
 * their direct overrides; the asymmetry is intentional.
 *
 * This is a free helper rather than a mixin because the concrete
 * command classes inherit `SourceExprCommand` non-virtually (via
 * `InstallableCommand` / `InstallableValueCommand`); a mixin that
 * also inherited `SourceExprCommand` would create a duplicate
 * subobject and ambiguous member access.
 */
inline Strings prependFlakeAttrPaths(Strings prefixed, SourceExprCommand & cmd)
{
    for (auto & s : cmd.SourceExprCommand::getDefaultFlakeAttrPaths())
        prefixed.push_back(s);
    return prefixed;
}

/**
 * Same as `prependFlakeAttrPaths` but for
 * `getDefaultFlakeAttrPathPrefixes`. Kept as a separate helper so each
 * call site reads symmetrically with its `getDefaultFlakeAttrPaths`
 * sibling.
 */
inline Strings prependFlakeAttrPathPrefixes(Strings prefixed, SourceExprCommand & cmd)
{
    for (auto & s : cmd.SourceExprCommand::getDefaultFlakeAttrPathPrefixes())
        prefixed.push_back(s);
    return prefixed;
}

} // namespace nix
