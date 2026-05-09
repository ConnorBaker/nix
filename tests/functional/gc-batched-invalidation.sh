#!/usr/bin/env bash

# Patch G2 regression test: batched SQLite invalidation during GC.
#
# Covers:
#   * Multiple-batch path (>256 paths so batchSize is exceeded).
#   * Idempotent GC.
#   * PathInUse fallback still works (referrers outside the batch).
#   * GC with a GC root keeps the protected path alive; releasing the
#     root lets it be collected.
#   * Cache consistency after GC: isValidPath returns false for deleted
#     paths.

source common.sh

TODO_NixOS

# ---------- helpers ----------

# Generate N disjoint derivations with unique content (so no
# cross-references), build them, release their drv roots.
generateDisjoint() {
    local n="$1"
    local tag="$2"

    local expr
    expr=$(cat <<EOF
let
  cfg = import ${config_nix};
  mkN = i: cfg.mkDerivation {
    name = "${tag}-\${toString i}-unique";
    builder = builtins.toFile "b" ''
      mkdir \$out
      echo \${toString i} > \$out/f
    '';
  };
in builtins.listToAttrs (
     map (i: { name = "d\${toString i}"; value = mkN i; })
     (builtins.genList (x: x) ${n}))
EOF
)
    local drvs
    drvs=$(echo "$expr" | nix-instantiate --expr - --add-root "$TEST_ROOT/$tag.drvs" --indirect 2>/dev/null)
    while IFS= read -r drv; do
        [ -z "$drv" ] && continue
        nix-store --realise "$drv" > /dev/null
    done <<< "$drvs"
    rm -f "$TEST_ROOT/$tag.drvs"*
}

hasGlob() { find "$NIX_STORE_DIR" -maxdepth 1 -name "$1" -print -quit 2>/dev/null | grep -q .; }

countGlob() { find "$NIX_STORE_DIR" -maxdepth 1 -name "$1" 2>/dev/null | wc -l; }

# ---------- Scenario 1: multi-batch happy path ----------
#
# batchSize is 256. Build 600 derivations so GC's invalidation must
# process at least three batches.

N=600

clearStoreIfPossible
generateDisjoint "$N" multibatch

before="$(countGlob "*-multibatch-*-unique")"
if [ "$before" != "$N" ]; then
    echo "SETUP FAIL: expected $N multibatch paths, found $before"
    exit 1
fi

nix-store --gc

after="$(countGlob "*-multibatch-*-unique")"
if [ "$after" != "0" ]; then
    echo "FAIL: $after multibatch paths survived multi-batch GC"
    nix-store --gc --print-roots
    exit 1
fi

# ---------- Scenario 2: idempotent GC on clean store ----------

nix-store --gc    # Must be a no-op and not crash.

# ---------- Scenario 3: protected path kept alive, then released ----------

clearStoreIfPossible

# shellcheck disable=SC2016
echo 'with import '"${config_nix}"'; mkDerivation { name = "pinned-target"; builder = builtins.toFile "b" "mkdir $out; echo pinned > $out/f"; }' \
    | nix-instantiate --add-root "$TEST_ROOT/leaf.drv" --indirect - \
    | xargs -I{} nix-store --realise {} --add-root "$TEST_ROOT/leaf.link" --indirect > /dev/null
rm -f "$TEST_ROOT/leaf.drv"

# With root present, GC must keep the target alive.
nix-store --gc
if ! hasGlob "*-pinned-target"; then
    echo "FAIL: pinned-target was removed despite GC root"
    exit 1
fi

# Release root.
rm -f "$TEST_ROOT/leaf.link"

# Now GC must remove it.
nix-store --gc
if hasGlob "*-pinned-target"; then
    echo "FAIL: pinned-target not removed after releasing root"
    nix-store --gc --print-roots
    exit 1
fi

# ---------- Scenario 4: isValidPath cache consistency after batched GC ----------
#
# Build some paths, grab their store-path strings into a file (so they
# are NOT captured in shell environ / runtime roots), then GC. A
# subsequent isValidPath query must reflect reality (not stale cache).

clearStoreIfPossible
generateDisjoint 300 cachecheck

# Collect path names via filesystem glob into a file; do NOT leave
# store paths in shell variables.
find "$NIX_STORE_DIR" -maxdepth 1 -name "*-cachecheck-*-unique" > "$TEST_ROOT/cachecheck.paths"
pathsBefore="$(wc -l < "$TEST_ROOT/cachecheck.paths")"
if [ "$pathsBefore" != "300" ]; then
    echo "SETUP FAIL: expected 300 cachecheck paths, have $pathsBefore"
    exit 1
fi

nix-store --gc

# isValidPath must return false for all of them now.
while IFS= read -r p; do
    if nix-store --check-validity "$p" 2>/dev/null; then
        echo "FAIL: cached isValidPath stale: $p reports valid after GC"
        exit 1
    fi
done < "$TEST_ROOT/cachecheck.paths"

# ---------- Scenario 5: PathInUse fallback via referrer chain ----------
#
# Build A, then build B which references A via a fixed-output or
# runtime dep. Trying to delete only A must fail / refuse. Releasing
# all roots and doing a full GC must remove both via the fallback.

clearStoreIfPossible
# Build A.
# shellcheck disable=SC2016
echo 'with import '"${config_nix}"'; mkDerivation { name = "leaf-A"; builder = builtins.toFile "b" "mkdir $out; echo A > $out/f"; }' \
    | nix-instantiate --add-root "$TEST_ROOT/A.drv" --indirect - \
    | xargs -I{} nix-store --realise {} --add-root "$TEST_ROOT/A.link" --indirect > /dev/null

# Build B which references A.
# shellcheck disable=SC2016
echo '
  let a = builtins.storePath (builtins.head (builtins.filter (p: builtins.match ".*leaf-A.*" p != null) (builtins.attrNames (builtins.readDir "'"$NIX_STORE_DIR"'"))));
  in throw "ignored"' > /dev/null 2>&1 || true
# Simpler: use a derivation that copies from A's path.
aPath="$(readlink "$TEST_ROOT/A.link")"
# shellcheck disable=SC2016
echo 'with import '"${config_nix}"'; mkDerivation { name = "parent-B"; a = builtins.storePath "'"$aPath"'"; builder = builtins.toFile "b" "mkdir $out; ln -s $a $out/ref"; }' \
    | nix-instantiate --add-root "$TEST_ROOT/B.drv" --indirect - \
    | xargs -I{} nix-store --realise {} --add-root "$TEST_ROOT/B.link" --indirect > /dev/null

# With both roots, try to delete A. Must fail (B references it).
# "2>&1 | grep -qi" handles nix-store variations in error phrasing.
if nix-store --delete "$aPath" 2>&1 | grep -qi "in use\|referenced\|alive\|cannot delete"; then
    : # expected
elif nix-store -q --hash "$aPath" 2>/dev/null; then
    : # path still valid = delete was refused
else
    echo "FAIL: deletion of A succeeded despite B referencing it"
    exit 1
fi

# Release both roots. GC should remove both via the batched path with
# fallback to per-path if referrer ordering matters.
rm -f "$TEST_ROOT/A.link" "$TEST_ROOT/B.link" "$TEST_ROOT/A.drv" "$TEST_ROOT/B.drv"
nix-store --gc

if hasGlob "*-leaf-A"; then
    echo "FAIL: leaf-A survived after all roots released"
    exit 1
fi
if hasGlob "*-parent-B"; then
    echo "FAIL: parent-B survived after all roots released"
    exit 1
fi

echo "PASS: gc-batched-invalidation"
