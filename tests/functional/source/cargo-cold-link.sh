#!/usr/bin/env bash

# Perf #1 (copy-once-link-N): on a COLD store, N packages that share one
# SourceContentId but have distinct `name`s produce N distinct CA store
# paths. Their NARs are byte-identical (the NAR doesn't encode the store
# path name), so the scheduler Copies the FIRST sibling's bytes once and
# HARDLINKS the rest into their name-stamped CA paths — 1 copy, not N.
#
# This is the dominant cold cargo-workspace cost (PROPOSAL.md §6.10.2).
# The walk-count mechanism mirrors single-walk-fusion.sh: the Copy walk
# logs `copying '<path>'` (fetch-to-store.cc), the narHash DryRun walk
# logs `hashing '<path>'`.
#
# PF-1: a single shared contentId observed by N placeholders must cost
# exactly ONE cold walk of the SUBTREE — the first sibling's fused Copy
# (which yields the hash), with the N−1 siblings hardlinked. Earlier
# this path ran a narHash DryRun *prelude* over the subtree AND then a
# Copy = TWO cold subtree walks; the prelude is now skipped for the
# single-contentId case so the first `outPathOf` fuses.
#
# So we assert, SCOPED TO THE `/sub` SUBTREE (the scheduler's walk
# target): `copying '…/sub'`==1 (copy-once-link-N) AND `hashing '…/sub'`
# ==0 (no redundant DryRun prelude). NB the whole git INPUT's own
# narHash walk (`hashing '«git+file://…»/'`, from mountInput forcing
# t.outPath — the locked-input walk, unrelated to the cargo group) is
# deliberately NOT counted here; scoping to `/sub` isolates the
# scheduler's behaviour from the input materialisation.
#
# Non-vacuity: with #1 reverted each sibling Copies independently → N
# `copying '…/sub'` lines; with the PF-1 prelude restored there is an
# extra `hashing '…/sub'` line.

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
git -C "$repo" add -A
git -C "$repo" commit -qm init
rev=$(git -C "$repo" rev-parse HEAD)

# N builtins.path calls over the SAME git subtree (same content+filter ⇒
# same SourceContentId) with DISTINCT names ⇒ N distinct CA store paths,
# one shared contentId. This is the cargo-workspace shape: the batched
# resolveSourceVirtualContext → outPathsOf groups them by contentId.
# We force a single batched demand by putting all N in one derivation's
# args (string context), so the whole group resolves together.
expr="
let
  t = builtins.fetchGit { url = \"file://$repo\"; rev = \"$rev\"; };
  mk = name: builtins.path { path = t.outPath + \"/sub\"; inherit name; };
in
  builtins.toJSON [
    (mk \"pkg-a\") (mk \"pkg-b\") (mk \"pkg-c\") (mk \"pkg-d\")
  ]
"

log=$TEST_ROOT/eval.log
out=$(nix eval --impure --raw --expr "$expr" -vvvv 2> "$log")
echo "out=$out"

# Extract the four store paths.
paths=$(echo "$out" | jq -r '.[]')
echo "paths:"
echo "$paths"
nPaths=$(echo "$paths" | wc -l)
[[ "$nPaths" -eq 4 ]] || fail "expected 4 store paths, got $nPaths"

# All four must be distinct (name-stamped) and valid.
nDistinct=$(echo "$paths" | sort -u | wc -l)
[[ "$nDistinct" -eq 4 ]] || fail "expected 4 distinct paths, got $nDistinct"
for p in $paths; do
    nix path-info "$p" > /dev/null 2>&1 || fail "path not valid: $p"
done

# Content equality: all four must have the same narHash (same content,
# different name).
h0=""
for p in $paths; do
    h=$(nix path-info --json "$p" | jq -r '.[].narHash // (.[] | .narHash)')
    if [[ -z "$h0" ]]; then h0=$h; else
        [[ "$h" = "$h0" ]] || fail "narHash differs across siblings: $h vs $h0"
    fi
done

# THE #1 assertion: exactly ONE byte-Copy of the subtree for the whole
# group. The other three siblings are hardlinked, not copied. Scope to
# `/sub` so the git input's own materialisation can't confound the count.
copyings=$(count_matches "copying '[^']*/sub'" "$log")
echo "subtree copying lines: $copyings"
[[ "$copyings" -eq 1 ]] \
    || fail "copy-once-link-N REGRESSION: $copyings 'copying …/sub' lines for a 4-package shared-content group; expected exactly 1 (the rest hardlinked). Log:
$(cat "$log")"

# THE PF-1 assertion: ZERO narHash DryRun walks OF THE SUBTREE. The
# single-contentId prelude is skipped, so the first sibling's fused Copy
# (logged as `copying …/sub`, above) is the only subtree walk. A
# `hashing …/sub` line would mean the redundant prelude DryRun came back.
# (The whole-input `hashing '«git+file://…»/'` line is expected and not
# counted — it's mountInput forcing t.outPath, not the scheduler.)
hashings=$(count_matches "hashing '[^']*/sub'" "$log")
echo "subtree hashing lines: $hashings"
[[ "$hashings" -eq 0 ]] \
    || fail "PF-1 REGRESSION: $hashings 'hashing …/sub' (redundant DryRun prelude) lines for a single-contentId group; expected 0 — the prelude should be skipped so the first outPathOf fuses. Log:
$(cat "$log")"

echo "cargo-cold-link: ok"
