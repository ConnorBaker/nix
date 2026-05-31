#!/usr/bin/env bash

# Perf: a COLD, content-fingerprinted builtins.path must materialise its
# source in ONE tree walk, not two.
#
# When a `builtins.path` source has a content fingerprint (a git-backed
# subtree — the monorepo pattern), addPath takes the DEFERRED path:
# register a SourceView + emit a SourceVirtual placeholder. Resolving the
# placeholder used to walk the source TWICE on a cold store:
# MaterializationScheduler::narHashOf→fetchToStore2(DryRun) to hash (bytes
# discarded), then outPathOf→fetchToStore2(Copy) to ingest. The DryRun is
# redundant — the Copy already yields the narHash. outPathOf now fuses
# hash+copy into ONE Copy walk for the cache-miss case.
#
# (A plain non-git `builtins.path` on a /tmp dir has NO fingerprint, so it
# takes the EAGER fetchToStore path — single walk, never the scheduler.
# This test therefore uses a GIT-backed subtree, which is the only shape
# that reaches the deferred/scheduler double-walk. Verified: unfixed walks
# the subtree source twice (2 `hashing` lines), fixed once.)
#
# The suite previously asserted store paths / cache-key strings but NEVER
# walk counts, so the redundant walk was invisible. This closes that gap.
# Debug-log markers (fetch-to-store.cc): DryRun→`hashing '<path>'`,
# Copy→`copying '<path>'`.

source ../common.sh

requireGit
clearStore
clearCache

count_matches() {
    local pattern=$1 file=$2 n
    n=$(command grep -cE "$pattern" "$file" 2>/dev/null || true)
    echo "${n:-0}"
}

repo=$TEST_ROOT/repo
createGitRepo "$repo"
mkdir -p "$repo/sub"
echo "alpha" > "$repo/sub/a.txt"
echo "beta" > "$repo/sub/b.txt"
echo "top" > "$repo/top.txt"
git -C "$repo" add -A
git -C "$repo" commit -qm init
rev=$(git -C "$repo" rev-parse HEAD)

# builtins.path over a git subtree → content-fingerprinted (tree:<sha>) →
# deferred scheduler path → the place the double-walk lived.
expr="
let
  t = builtins.fetchGit { url = \"file://$repo\"; rev = \"$rev\"; };
in
  builtins.path { path = t.outPath + \"/sub\"; name = \"gsub\"; }
"

log=$TEST_ROOT/eval.log
nix eval --impure --raw --expr "$expr" -vvvv 2> "$log" > /dev/null

# Count walks of the BUILTINS.PATH subtree source specifically. The
# scheduler walk renders the SourceView accessor path; the fetchGit input
# walk renders the «git+file://…» URL. We isolate the subtree-source walk
# by matching the materialisation of the `gsub`-named source: its DryRun
# would log a `hashing` line for the SourceView path. The robust signal is
# the TOTAL `hashing` count — unfixed = 2 (fetchGit input + subtree
# DryRun), fixed = 1 (input only; subtree's DryRun fused into its Copy).
hashings=$(count_matches "hashing '" "$log")
echo "git-subtree builtins.path cold: hashing=$hashings"

# Fused: the subtree source is NOT separately DryRun-walked. With the
# fetchGit input contributing at most one `hashing`, the fused path keeps
# the total at 1; the unfused double-walk pushed it to 2.
[[ "$hashings" -le 1 ]] \
    || fail "FUSION REGRESSION: cold git-subtree builtins.path produced $hashings 'hashing' (DryRun) walks; expected <=1 (the redundant source DryRun should be fused into the Copy). Log:
$(cat "$log")"

echo "single-walk-fusion: ok"
