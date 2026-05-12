#!/usr/bin/env bash

# Regression test for the snapshot-based forward-closure walk used
# by `collectGarbage` under the `keep-derivations` and
# `keep-outputs` flags. These flags exercise three code paths that
# the bulk-snapshot rework rewrote:
#
#   1. `markAlive`'s forward closure: when a live path is reached,
#      its forward references (and optionally derivers/outputs) are
#      also marked alive. Tests scenario C.
#
#   2. The main traversal's `keepDerivations` branch: when visiting
#      a derivation, enqueue its outputs via the snapshot's
#      `outputsByDrvId` + `deriverById` filter. Tests scenarios B and D.
#
#   3. The main traversal's `keepOutputs` branch: when visiting an
#      output path, enqueue its deriver via the snapshot's
#      `deriversByOutputId`. Tests scenarios A, C, D.
#
# GC roots are established via `nix-store --add-root ... --indirect`
# which registers the path symlink under `$stateDir/gcroots/auto/`
# so the GC root-finder can discover it.

source common.sh

TODO_NixOS

# ---------- Scenario A: keep-outputs holds a derivation's outputs alive ----------
#
# Build a derivation, root only the drv (indirectly). With
# `keep-outputs=true`, GC must preserve the output because it's an
# output of a live derivation.

clearStoreIfPossible

# shellcheck disable=SC2016
drvA=$(echo 'with import '"${config_nix}"'; mkDerivation {
  name = "keep-outputs-A";
  builder = builtins.toFile "b" "mkdir $out; echo scenarioA > $out/f";
}' | nix-instantiate --add-root "$TEST_ROOT/A.drv.root" --indirect -)

# Realise without rooting the output — only the drv is rooted.
outA=$(nix-store --realise "$drvA")

# Sanity check: drv and output both exist.
[ -e "$drvA" ] || { echo "SETUP FAIL(A): drv not present"; exit 1; }
[ -e "$outA" ] || { echo "SETUP FAIL(A): output not built"; exit 1; }

# Without keep-outputs, GC would delete $outA. With it, $outA survives.
nix-store --gc --option keep-outputs true --option keep-derivations false
if [ ! -e "$outA" ]; then
    echo "FAIL(A): keep-outputs=true GC removed the output despite a live deriver"
    nix-store --gc --print-roots || true
    exit 1
fi
if [ ! -e "$drvA" ]; then
    echo "FAIL(A): rooted drv vanished"
    exit 1
fi

# Release the drv root and GC again. Both should disappear.
rm -f "$TEST_ROOT/A.drv.root"
nix-store --gc --option keep-outputs true --option keep-derivations false
if [ -e "$outA" ]; then
    echo "FAIL(A): output survived after releasing drv root"
    exit 1
fi
if [ -e "$drvA" ]; then
    echo "FAIL(A): drv survived after releasing its root"
    exit 1
fi

# ---------- Scenario B: keep-derivations holds a derivation alive via its output ----------
#
# Reverse of A: root only the output, GC with keep-derivations=true.
# The drv must survive because a live output pulls its deriver alive.

clearStoreIfPossible

# shellcheck disable=SC2016
drvB=$(echo 'with import '"${config_nix}"'; mkDerivation {
  name = "keep-derivs-B";
  builder = builtins.toFile "b" "mkdir $out; echo scenarioB > $out/f";
}' | nix-instantiate -)

# Realise and root only the output (not the drv).
nix-store --realise "$drvB" --add-root "$TEST_ROOT/B.out.root" --indirect > /dev/null
outB="$(readlink "$TEST_ROOT/B.out.root")"

[ -e "$drvB" ] || { echo "SETUP FAIL(B): drv not present"; exit 1; }
[ -e "$outB" ] || { echo "SETUP FAIL(B): output not built"; exit 1; }

# With keep-derivations=true, rooted output keeps drv alive.
nix-store --gc --option keep-derivations true --option keep-outputs false
if [ ! -e "$outB" ]; then
    echo "FAIL(B): GC removed the rooted output"
    exit 1
fi
if [ ! -e "$drvB" ]; then
    echo "FAIL(B): keep-derivations=true GC removed the drv despite a live output"
    nix-store --gc --print-roots || true
    exit 1
fi

# Release output root; both should now vanish.
rm -f "$TEST_ROOT/B.out.root"
nix-store --gc --option keep-derivations true --option keep-outputs false
if [ -e "$outB" ]; then
    echo "FAIL(B): output survived after releasing root"
    exit 1
fi
if [ -e "$drvB" ]; then
    echo "FAIL(B): drv survived after releasing output root"
    exit 1
fi

# ---------- Scenario C: transitive closure under keep-outputs ----------
#
# Build two derivations where B depends on A at runtime. Root only
# B's drv. With `keep-outputs=true`:
#   * B_drv is rooted directly.
#   * B_out is kept alive because its deriver (B_drv) is live
#     (keep-outputs forward edge).
#   * A_out is kept alive because it's a runtime reference of
#     B_out (forward closure walked by markAlive).
#
# This specifically exercises the snapshot-based BFS in markAlive,
# traversing `referencesById` edges from B_out to A_out.

clearStoreIfPossible

# A: source leaf.
# shellcheck disable=SC2016
drvC_A=$(echo 'with import '"${config_nix}"'; mkDerivation {
  name = "transC-A";
  builder = builtins.toFile "b" "mkdir $out; echo leaf > $out/f";
}' | nix-instantiate -)

# Realise A to get its output path string.
outA_str=$(nix-store --realise "$drvC_A")

# B: runtime-references A's output.
# shellcheck disable=SC2016
drvC_B=$(echo 'with import '"${config_nix}"'; mkDerivation {
  name = "transC-B";
  a = builtins.storePath "'"$outA_str"'";
  builder = builtins.toFile "b" "mkdir $out; ln -s $a $out/ref";
}' | nix-instantiate --add-root "$TEST_ROOT/C.drv.root" --indirect -)

outB_str=$(nix-store --realise "$drvC_B")

# Sanity: all four paths exist.
for p in "$drvC_A" "$outA_str" "$drvC_B" "$outB_str"; do
    [ -e "$p" ] || { echo "SETUP FAIL(C): $p missing"; exit 1; }
done

# With keep-outputs=true, we should preserve the whole closure:
#   B_drv (rooted) -> B_out (keep-outputs) -> A_out (ref edge).
# A_drv is NOT preserved (keep-derivations=false, and B_out doesn't
# reference A_drv).
nix-store --gc --option keep-outputs true --option keep-derivations false

for p in "$drvC_B" "$outB_str" "$outA_str"; do
    if [ ! -e "$p" ]; then
        echo "FAIL(C): expected $p to survive keep-outputs GC"
        nix-store --gc --print-roots || true
        exit 1
    fi
done
# A's drv has no keep-outputs path back to anything live.
if [ -e "$drvC_A" ]; then
    echo "FAIL(C): A's drv should have been collected (keep-derivations is false)"
    exit 1
fi

# Release the B drv root; everything B-reachable goes.
rm -f "$TEST_ROOT/C.drv.root"
nix-store --gc --option keep-outputs true --option keep-derivations false
for p in "$drvC_B" "$outB_str" "$outA_str"; do
    if [ -e "$p" ]; then
        echo "FAIL(C): $p survived after releasing root"
        exit 1
    fi
done

# ---------- Scenario D: both flags together ----------
#
# Root only the output. With both flags on:
#   * keep-derivations=true -> D_drv is kept (via D_out).
#   * keep-outputs=true -> all outputs of D_drv are kept (just D_out).
# So both survive.

clearStoreIfPossible

# shellcheck disable=SC2016
drvD=$(echo 'with import '"${config_nix}"'; mkDerivation {
  name = "keep-both-D";
  builder = builtins.toFile "b" "mkdir $out; echo scenarioD > $out/f";
}' | nix-instantiate -)
nix-store --realise "$drvD" --add-root "$TEST_ROOT/D.out.root" --indirect > /dev/null
outD="$(readlink "$TEST_ROOT/D.out.root")"

nix-store --gc --option keep-outputs true --option keep-derivations true
[ -e "$outD" ] || { echo "FAIL(D): output vanished under both-flags GC"; exit 1; }
[ -e "$drvD" ] || { echo "FAIL(D): drv vanished under both-flags GC"; exit 1; }

rm -f "$TEST_ROOT/D.out.root"
nix-store --gc --option keep-outputs true --option keep-derivations true
if [ -e "$outD" ] || [ -e "$drvD" ]; then
    echo "FAIL(D): paths survived after releasing root"
    [ -e "$outD" ] && echo "  outD still present: $outD"
    [ -e "$drvD" ] && echo "  drvD still present: $drvD"
    exit 1
fi

# ---------- Scenario E: keep-outputs does NOT resurrect invalidated outputs ----------
#
# If an output was deleted (invalidated), keep-outputs should not
# resurrect it. The snapshot's `outputsByDrvId` is filtered to
# ValidPaths members (matching the old code's `isValidPath` filter).
# Test that a drv whose output was deleted and which has no other
# roots gets GC'd.

clearStoreIfPossible

# shellcheck disable=SC2016
drvE=$(echo 'with import '"${config_nix}"'; mkDerivation {
  name = "filtered-E";
  builder = builtins.toFile "b" "mkdir $out; echo scenarioE > $out/f";
}' | nix-instantiate --add-root "$TEST_ROOT/E.drv.root" --indirect -)
outE=$(nix-store --realise "$drvE")

# Invalidate the output.
nix-store --delete "$outE" 2>/dev/null || true

# drv is rooted. With keep-outputs, GC shouldn't error out on the
# now-missing output. The drv survives (it's rooted).
nix-store --gc --option keep-outputs true --option keep-derivations false
[ -e "$drvE" ] || { echo "FAIL(E): rooted drv vanished despite still having its GC root"; exit 1; }

# Release the drv root; the drv should go.
rm -f "$TEST_ROOT/E.drv.root"
nix-store --gc --option keep-outputs true --option keep-derivations false
if [ -e "$drvE" ]; then
    echo "FAIL(E): drv survived with no roots and keep-outputs=true"
    exit 1
fi

echo "PASS: gc-keep-flags"
