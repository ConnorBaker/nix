#!/usr/bin/env bash

source common.sh

# The object store is unconditional (doc/lazy-store/01-specification.md,
# section 9.10): a built output's regular files are hard links into the
# store's object store as soon as the build registers it, so two outputs
# with the same file share one inode without any setting or pass, and
# `nix-store --optimise` afterwards has nothing left to link.

TODO_NixOS # runs `nix-store --optimise` against the store directly (NIX_REMOTE="") and inspects $NIX_STORE_DIR/.links

# The `auto-optimise-store` setting went with the pass it switched on; a
# command line or a configuration that still gives it is accepted, ignored,
# and told why.  Before: the flag was an error and the
# configuration line an "unknown setting" warning.
# shellcheck disable=SC2016
outPath0=$(echo 'with import '"${config_nix}"'; mkDerivation { name = "foo0"; builder = builtins.toFile "builder" "mkdir $out; echo hello0 > $out/foo"; }' | nix-build - --no-out-link --auto-optimise-store 2> "$TEST_ROOT/auto-optimise.log")
[[ $(cat "$outPath0/foo") == hello0 ]]
# Once: the build hook's child gets the parent's settings by their
# rendering, and `RemovedSetting` stores no value, so it is handed `false`.
[[ $(grep -c "setting 'auto-optimise-store' is ignored" "$TEST_ROOT/auto-optimise.log") == 1 ]]
NIX_CONFIG="auto-optimise-store = true" nix-store --version 2> "$TEST_ROOT/auto-optimise-conf.log"
grepQuiet "setting 'auto-optimise-store' is ignored" "$TEST_ROOT/auto-optimise-conf.log"
# Once per top-level process when the configuration itself says `true`
# (the NixOS case): the build hook's child re-reads that configuration
# and does not warn again (before, twice per `nix-build`).
# shellcheck disable=SC2016
outPath00=$(echo 'with import '"${config_nix}"'; mkDerivation { name = "foo00"; builder = builtins.toFile "builder" "mkdir $out; echo hello00 > $out/foo"; }' | NIX_CONFIG="auto-optimise-store = true" nix-build - --no-out-link 2> "$TEST_ROOT/auto-optimise-hook.log")
[[ $(cat "$outPath00/foo") == hello00 ]]
[[ $(grep -c "setting 'auto-optimise-store' is ignored" "$TEST_ROOT/auto-optimise-hook.log") == 1 ]]
# `false` asks for nothing and is what the build hook hands its child for
# every setting, so it is silent (else every build would print the warning:
# the cli-characterisation goldens on Linux).
NIX_CONFIG="auto-optimise-store = false" nix-store --version 2> "$TEST_ROOT/auto-optimise-false.log"
grepQuietInverse "auto-optimise-store" "$TEST_ROOT/auto-optimise-false.log"
grepQuietInverse "unknown setting" "$TEST_ROOT/auto-optimise-conf.log"
# Where the removed setting appears (the convention of
# `DeprecatedWarnSetting`, `excludedFromFullSerialisation`): not in the
# key=value dump, which is the effective configuration; in the JSON, which
# is the registry the manual's settings page is generated from, with the
# description saying it is removed; and by name.
nix config show > "$TEST_ROOT/config-show.log"
grepQuietInverse "^auto-optimise-store" "$TEST_ROOT/config-show.log"
nix config show --json | jq -e '."auto-optimise-store".description | startswith("Removed; accepted and ignored")'
[[ $(nix config show auto-optimise-store) == false ]]

# shellcheck disable=SC2016
outPath1=$(echo 'with import '"${config_nix}"'; mkDerivation { name = "foo1"; builder = builtins.toFile "builder" "mkdir $out; echo hello > $out/foo"; }' | nix-build - --no-out-link)
# shellcheck disable=SC2016
outPath2=$(echo 'with import '"${config_nix}"'; mkDerivation { name = "foo2"; builder = builtins.toFile "builder" "mkdir $out; echo hello > $out/foo"; }' | nix-build - --no-out-link)

inode1="$(stat --format=%i "$outPath1"/foo)"
inode2="$(stat --format=%i "$outPath2"/foo)"
if [ "$inode1" != "$inode2" ]; then
    fail "inodes do not match"
fi

# The two outputs' files and the blob's.
nlink="$(stat --format=%h "$outPath1"/foo)"
if [ "$nlink" != 3 ]; then
    fail "link count incorrect"
fi

# A third build shares the inode as soon as it is registered.
# shellcheck disable=SC2016
outPath3=$(echo 'with import '"${config_nix}"'; mkDerivation { name = "foo3"; builder = builtins.toFile "builder" "mkdir $out; echo hello > $out/foo"; }' | nix-build - --no-out-link)

inode3="$(stat --format=%i "$outPath3"/foo)"
if [ "$inode1" != "$inode3" ]; then
    fail "inodes do not match"
fi

nlink="$(stat --format=%h "$outPath1"/foo)"
if [ "$nlink" != 4 ]; then
    fail "link count incorrect after the third build"
fi

# The optimise pass finds nothing left to link, and changes nothing.
# XXX: This should work through the daemon too
NIX_REMOTE="" nix-store --optimise 2> "$TEST_ROOT/optimise.log"
grepQuiet "hard-linking 0 files" "$TEST_ROOT/optimise.log"

inode1="$(stat --format=%i "$outPath1"/foo)"
inode3="$(stat --format=%i "$outPath3"/foo)"
if [ "$inode1" != "$inode3" ]; then
    fail "inodes do not match"
fi

nlink="$(stat --format=%h "$outPath1"/foo)"
if [ "$nlink" != 4 ]; then
    fail "link count changed by the optimise pass"
fi

# shellcheck disable=SC2016
outPath4=$(echo 'with import '"${config_nix}"'; mkDerivation { name = "foo4"; builder = builtins.toFile "builder" "mkdir $out; echo hello > $out/foo"; }' | nix-build - --no-out-link)

inode4="$(stat --format=%i "$outPath4"/foo)"
if [ "$inode1" != "$inode4" ]; then
    fail "inodes do not match"
fi

NIX_REMOTE="" nix store optimise 2> "$TEST_ROOT/optimise.log"
grepQuiet "hard-linking 0 files" "$TEST_ROOT/optimise.log"

nlink="$(stat --format=%h "$outPath1"/foo)"
if [ "$nlink" != 5 ]; then
    fail "link count incorrect after the fourth build"
fi

# alias of optimise
if ! NIX_REMOTE="" nix store optimize; then
    fail "nix store optimize alias is not present"
fi

nix-store --gc

# The legacy `.links` table is not made; `nix-store --optimise` removes one.
if [ -e "$NIX_STORE_DIR"/.links ]; then
    fail ".links directory present after optimise and GC"
fi
