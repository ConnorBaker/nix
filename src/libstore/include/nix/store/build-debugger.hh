#pragma once
///@file
///
/// Shared constants and helpers for the `--build-debugger` experimental
/// feature. Consumed by the builder (publish attach-info), the hook path
/// in `DerivationBuildingGoal` (publish redirect attach-info), and the
/// `nix debug-attach` CLI (read attach-info).
///
/// Keeping the schema version in a single header avoids the three-file
/// drift that would otherwise make bumping the schema error-prone.

#include <filesystem>
#include <string_view>

#include "nix/store/path.hh"

namespace nix {

class Store;

/**
 * Schema version for files under `<nix-state-dir>/debugger/`.
 *
 * A writer stamps `schemaVersion: N` into every `.attach` file; readers
 * refuse entries with an unrecognised version rather than mis-parse.
 * Bump when making incompatible changes to the on-disk format so that
 * an older `nix debug-attach` binary paired with a newer daemon fails
 * fast instead of silently doing the wrong thing.
 */
constexpr int kDebuggerAttachInfoSchemaVersion = 1;

/**
 * Publish a redirect attach-info file at
 * `<nix-state-dir>/debugger/<hash>.attach` containing `remoteHost` so
 * that `nix debug-attach <drv>` on this host automatically SSHes to
 * `remoteHost` instead of attempting a local attach. Used by:
 *
 *   - `DerivationBuildingGoal::buildWithBuildHook` after the hook
 *     accepts the build on a remote builder.
 *   - `CmdBuild::run` when the user passed `--store ssh-ng://host` and
 *     the paused sandbox will live on `host`.
 *
 * Atomic write + rename; returns the final file's path (empty on
 * failure — failures are non-fatal and logged as warnings, never
 * thrown, since losing auto-follow just forces the user into
 * `--on <host>`).
 *
 * @param store Store to render `drvPath` against. Taking the `Store`
 *     directly (rather than a pre-rendered string) keeps the on-disk
 *     `drvPath` field's shape authoritative — callers can't accidentally
 *     inject newlines or non-path content into the JSON.
 * @param drvPath The derivation being instrumented.
 * @param remoteHost Opaque host identifier the attach CLI forwards to
 *     SSH. Usually a store URI like `ssh://host` or `ssh-ng://host`;
 *     `nix debug-attach` strips the scheme when dispatching.
 */
std::filesystem::path writeDebuggerRedirectAttachInfo(
    Store & store,
    const StorePath & drvPath,
    std::string_view remoteHost);

} // namespace nix
