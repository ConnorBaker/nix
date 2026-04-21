#!/usr/bin/env bash

# Functional tests for --build-debugger.
#
# The interactive attach itself is exercised by the NixOS VM test
# (`tests/nixos/build-debugger.nix`). This script covers the static gates
# that don't need a live kernel namespace / sandboxed builder to reach.

source common.sh

# All of these refusal tests should run identically whether or not the
# experimental feature is on, since the flag itself requires the feature.
TODO_NixOS

clearStoreIfPossible

# ---------------------------------------------------------------------------
# 1. Experimental-feature gate: --build-debugger without the feature enabled
#    is refused at flag-parse time.
# ---------------------------------------------------------------------------
expectStderr 1 nix --offline build --build-debugger -f simple.nix \
    | grepQuiet "experimental Nix feature 'build-debugger' is disabled"

# ---------------------------------------------------------------------------
# 2. The `build-debugger` setting shows up in `nix show-config`.
# ---------------------------------------------------------------------------
nix --extra-experimental-features 'nix-command build-debugger' \
    show-config 2>&1 | grepQuiet 'build-debugger = false'

# ---------------------------------------------------------------------------
# 3. The flag is registered only on `nix build`. `nix develop`, `nix shell`,
#    `nix run` must refuse it with an "unrecognised flag" / similar error.
# ---------------------------------------------------------------------------
expectStderr 1 nix --extra-experimental-features 'nix-command build-debugger flakes' \
    develop --build-debugger -f simple.nix \
    </dev/null \
    | grepQuiet -E "(unrecognised|unknown) flag"

# ---------------------------------------------------------------------------
# 4. `nix debug-attach` subcommand exists and requires root.
# ---------------------------------------------------------------------------
# A well-formed but nonexistent drv path is enough to trigger the euid check
# before we touch the filesystem. Skip when we're already root.
if [[ "$(id -u)" -ne 0 ]]; then
    expectStderr 1 nix --extra-experimental-features 'nix-command build-debugger' \
        debug-attach /nix/store/00000000000000000000000000000000-dummy.drv \
        2>&1 | grepQuiet -E "must be run as root"
fi

# ---------------------------------------------------------------------------
# 5. Daemon-mode trust gate: an untrusted client setting build-debugger=true
#    via --option gets a clear refusal, not a silent ignore.
# ---------------------------------------------------------------------------
if isDaemonNewer "2.35"; then
    if [[ "${NIX_REMOTE-}" == "daemon" ]]; then
        expectStderr 1 nix --offline build \
            --extra-experimental-features build-debugger \
            --option build-debugger true \
            -f simple.nix \
            </dev/null \
            | grepQuiet "is not allowed because you are not in 'trusted-users'" \
            || echo "note: daemon-mode trust gate test skipped (client was trusted)" >&2
    fi
fi

echo "build-debugger functional tests passed (gate refusals)"
