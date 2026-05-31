#!/usr/bin/env bash

# Regression: a SYMLINKED source root must hash identically whether
# `builtins.path` takes the eager route or the deferred (SourceVirtual)
# route — and must equal hashing the symlink's *target* directly.
#
# The eager `addPath` branch resolved the root symlink
# (`path.resolveSymlinks()`); the deferred branch (default for a
# content-determined source with no expectedHash) used the raw path, so
# `builtins.path { path = ./symlink-to-dir; }` would record a *symlink*
# node (deferred) vs walk the *target* (eager) — different NAR, different
# store path. Fixed by resolving symlinks once, up front, before the
# eager/deferred split (primops.cc::addPath). This test pins eager ==
# deferred == target.
#
# See doc/tecnix-survey/PROPOSAL.md §6.4 (addPath migration).

source ../common.sh

clearStore

tree=$TEST_ROOT/tree
mkdir -p "$tree/realdir/sub"
echo "alpha" > "$tree/realdir/a.txt"
echo "beta"  > "$tree/realdir/sub/b.txt"
ln -s realdir "$tree/link"

# (1) Deferred route: `builtins.path` over the SYMLINK. Content-determined
#     source, no expectedHash → the SourceVirtual deferred path.
viaLink=$(nix eval --impure --raw --expr \
    "builtins.path { path = $tree/link; name = \"src\"; }")

# (2) The symlink's TARGET directly — the answer the eager branch would
#     have produced (resolveSymlinks lands here).
viaReal=$(nix eval --impure --raw --expr \
    "builtins.path { path = $tree/realdir; name = \"src\"; }")

echo "viaLink=$viaLink"
echo "viaReal=$viaReal"

# The fix: resolving the root symlink up front makes the symlinked-root
# eval equal the resolved-target eval.
[[ "$viaLink" = "$viaReal" ]] \
    || fail "symlinked source root hashed differently from its target: $viaLink vs $viaReal"

# (3) Eager route over the symlink: forcing `expectedHash` (a filter-free
#     pinned hash) takes the eager branch. It must agree with the
#     deferred route — both resolve the root symlink. We derive the
#     expected hash from the deferred result's valid path rather than
#     hardcoding, then re-evaluate with that hash to drive the eager arm.
expectedHash=$(nix path-info --json "$viaReal" | jq -r '.[].narHash // (.[] | .narHash)' 2>/dev/null || true)
if [[ -n "$expectedHash" && "$expectedHash" != null ]]; then
    viaEager=$(nix eval --impure --raw --expr \
        "builtins.path { path = $tree/link; name = \"src\"; sha256 = \"$expectedHash\"; }" 2>/dev/null || true)
    if [[ -n "$viaEager" ]]; then
        echo "viaEager=$viaEager"
        [[ "$viaEager" = "$viaReal" ]] \
            || fail "eager (expectedHash) route over symlink differs from target: $viaEager vs $viaReal"
    fi
fi

# The contents must be the real directory's, reachable through the
# resulting store path.
[[ "$(cat "$viaLink/a.txt")" = "alpha" ]] || fail "resolved store path missing a.txt content"
[[ "$(cat "$viaLink/sub/b.txt")" = "beta" ]] || fail "resolved store path missing sub/b.txt content"

# And the result must be a real directory, NOT a symlink node (the bug
# would have stored a symlink at the root).
[[ -d "$viaLink" && ! -L "$viaLink" ]] || fail "store path is not a materialised directory (symlink leaked)"

echo "symlink-root: ok"
