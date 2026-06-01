#!/usr/bin/env bash

# Perf (item (c)): a LOCKED input whose narHash is already known must NOT
# pay a redundant dryRun walk just to learn its store-path NAME.
#
# `mountInput`'s known-narHash fast path derives the CA store path from
# the recorded narHash (no walk). If that path is neither valid locally
# nor substitutable (the cold, local-only case — e.g. first build on a
# fresh checkout of a locked flake), master/upstream FELL THROUGH to the
# slow path's eager dryRun, walking the whole tree just to recompute the
# narHash it already held. Item (c) instead mints the real CA path,
# mounts the live accessor under it, and DEFERS the byte-copy to the
# first hard demand. A consumer that only instantiates (reads `.drvPath`)
# then never dryRun-walks the locked input at all.
#
# Non-vacuity: we count `hashing '` (DryRun walk) markers under -vvvv.
# Before (c): the locked dep contributes a `hashing` walk. After (c): it
# does not. We assert the dep's walk is gone (total <= 1: only the
# flake-self, which has no pre-known narHash, may still walk — that is
# the fundamental floor, not addressed by (c)), AND that the (c) defer
# marker fired.
#
# Soundness of the deferred copy is exercised by instantiating a
# derivation over the locked dep and asserting its inputSrc materialises
# to a VALID store path (the deferred copy runs + is checked at the
# derivation boundary). The complementary "unlocked + wrong narHash must
# still throw" contract is covered by lazy-narhash.sh — (c) deliberately
# does NOT defer there (the `isLocked` gate).

source ../common.sh

requireGit
clearStore
clearCache

count_matches() {
    local pattern=$1 file=$2 n
    n=$(command grep -cE "$pattern" "$file" 2>/dev/null || true)
    echo "${n:-0}"
}

# Real system, injected into the flake so it evaluates under PURE eval
# (a locked flake fetched by rev is pure; `builtins.currentSystem` is
# unavailable there).
system=$(nix eval --impure --raw --expr "builtins.currentSystem")

# A dependency flake with a non-trivial tree (so a redundant walk is
# clearly visible / would be expensive on a real monorepo).
dep=$TEST_ROOT/dep
createGitRepo "$dep"
mkdir -p "$dep/a"
for i in $(seq 1 20); do echo "content $i" > "$dep/a/f$i.txt"; done
echo '{ outputs = { self }: { src = self.outPath; }; }' > "$dep/flake.nix"
git -C "$dep" add -A
git -C "$dep" commit -qm init

# A consumer flake that builds a derivation consuming the dep's tree.
cons=$TEST_ROOT/cons
createGitRepo "$cons"
cat > "$cons/flake.nix" <<EOF
{
  inputs.dep.url = "git+file://$dep";
  outputs = { self, dep }: {
    drv = derivation {
      name = "uses-dep";
      system = "$system";
      builder = "/bin/sh";
      args = [ "-c" "echo hi > \$out" ];
      src = dep.src;
    };
  };
}
EOF
git -C "$cons" add -A
git -C "$cons" commit -qm init

# Lock the consumer (records the dep's rev + narHash in flake.lock) and
# commit the lock so the flake is fully locked + pure-evaluable by rev.
nix flake lock "git+file://$cons" >/dev/null 2>&1
git -C "$cons" add -A
git -C "$cons" commit -qm lock >/dev/null 2>&1
rev=$(git -C "$cons" rev-parse HEAD)

# Confirm the dep really is locked with a narHash (the precondition (c) keys on).
grep -q '"narHash"' "$cons/flake.lock" || fail "expected dep narHash in flake.lock"

# COLD store + cache: the dep's CA path is neither valid nor substitutable,
# so master would fall through to the eager dryRun.
clearStore
clearCache

log=$TEST_ROOT/eval.log
# Instantiate ONLY (read .drvPath): no build, no output-path display, so
# the dep need never be copied — pure "name the input" work.
nix eval "git+file://$cons?rev=$rev#drv.drvPath" --raw -vvvv 2> "$log" > /dev/null

hashings=$(count_matches "hashing '" "$log")
deferred=$(count_matches "deferring copy" "$log")
echo "locked-dep cold instantiate: hashing=$hashings deferring-copy=$deferred"

# The (c) defer must have fired for the locked dep.
[[ "$deferred" -ge 1 ]] \
    || fail "item (c) did not defer the locked input: no 'deferring copy' marker. Log:
$(cat "$log")"

# And the redundant dep walk must be gone: only the flake-self (no
# pre-known narHash) may walk. Master walked twice (self + dep dryRun).
[[ "$hashings" -le 1 ]] \
    || fail "DEFER REGRESSION: locked-dep cold instantiate produced $hashings 'hashing' walks; expected <=1 (the locked dep should be named from its narHash without a dryRun walk). Log:
$(cat "$log")"

# Soundness: instantiate the drv and assert its inputSrc materialises to a
# VALID store path. `nix derivation show` over the .drvPath demands the
# input source's real path, which forces the deferred copy AND checks it
# (a fake/dangling path would not be a valid store object). srcs are base
# names; prepend the store dir.
clearStore
clearCache
drvPath=$(nix eval "git+file://$cons?rev=$rev#drv.drvPath" --raw)
storeDir=$(nix eval --impure --raw --expr "builtins.storeDir")
nix derivation show "$drvPath" > "$TEST_ROOT/drv.json"
srcs=$(jq -r '.derivations[].inputs.srcs[]' "$TEST_ROOT/drv.json")
[[ -n "$srcs" ]] || { cat "$TEST_ROOT/drv.json" >&2; fail "drv has no input srcs (expected the dep src)"; }
for src in $srcs; do
    nix path-info "$storeDir/$src" > /dev/null 2>&1 \
        || fail "drv inputSrc '$src' is not a valid store path (the deferred locked-input copy did not materialise correctly)"
done

# --- the `immutableRev` gate: a `path:` input is MUTABLE even if it carries
# a user-supplied "fake" `rev` attr, so it must NOT take the known-narHash
# defer (which would move the narHash check past a metadata-only read).
# Assert the defer marker does NOT fire for a `path:?rev=…` input. -------
pathdir=$TEST_ROOT/pathdir
mkdir -p "$pathdir"
echo original > "$pathdir/data.txt"
# Learn its real narHash, then read it back WITH a fake rev. The defer must
# not engage for a path: input regardless of the rev attribute.
pnar=$(nix eval --impure --raw --expr "(builtins.fetchTree { type = \"path\"; path = \"$pathdir\"; }).narHash")
clearStore
clearCache
plog=$TEST_ROOT/path.log
nix eval --impure --raw -vvvv \
    --expr "(builtins.fetchTree { type = \"path\"; path = \"$pathdir\"; rev = \"0000000000000000000000000000000000000001\"; narHash = \"$pnar\"; }).outPath" \
    2> "$plog" > /dev/null || true
pdefer=$(count_matches "deferring copy" "$plog")
echo "path:?rev= input: deferring-copy markers = $pdefer (must be 0 — path: is mutable, never deferred)"
[[ "$pdefer" -eq 0 ]] \
    || fail "GATE REGRESSION: a path: input with a fake rev took the known-narHash defer ($pdefer markers); path: content is mutable and must keep eager verification (the immutableRev gate excludes getType()==\"path\"). Log:
$(cat "$plog")"

echo "known-narhash-defer: ok"
