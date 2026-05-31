#!/usr/bin/env bash

# Track Z.gap4 — unfiltered `builtins.path` subtree bridges via the bare
# `tree:<sha>` cross-pipeline `treeHashToNarHash` projection.
#
# THE CLAIM (gap4). An UNFILTERED `builtins.path { path = input.outPath +
# "/sub"; ... }` over a Git subtree builds a `recipe::Subtree` SourceView.
# `addPath` splices a `;shape=addPath-unfiltered-v1` sentinel onto the
# view's `fingerprint` FIELD so the `;shape`-suffixed `sourcePathToHash`
# dedup row cannot collide with the bare base (Tradeoff 9). But the
# CROSS-PIPELINE bridge key is a SEPARATE channel: `fetchToStore2` asks
# the view's `getFingerprint`, which short-circuits through the Translate
# operator stack to the base and returns the BARE `tree:<sub-sha>` (the
# `;shape` field is only a fallback, never reached when the base yields a
# fingerprint). So `peekTreeHashBridge`/`bareTreeOid` DO accept it, and
# the subtree shares `treeHashToNarHash[<sub-sha>]` with the SAME tree
# reached any other way — e.g. a whole git input whose ROOT tree IS that
# subtree's tree (which populates the row via Track Z.gap1's
# `getRootTreeHash()`), or a tarball with that tree.
#
# This is the two-channels design from PROPOSAL.md §6.11 Seam 3: the
# dedup key STAYS `;shape`-suffixed (no `sourcePathToHash` collision);
# the bridge key is the bare tree OID. The same single `bareTreeOid`
# decoder gates both the consult and the writeback so eligibility cannot
# drift (`;e`/`;l`/`;s`/`;shape=` all disqualify — the NAR then differs
# from a vanilla tree dump).
#
# WHY THIS IS NOT ALREADY COVERED. `subtree-dedup.sh` shares two REVS of
# the same subtree — that is served by the `;shape`-suffixed
# `sourceContentToNarHash`/subtree-fingerprint row alone and is VACUOUS
# w.r.t. the bare-tree-OID bridge. `whole-input-tree-dedup.sh` covers
# WHOLE inputs (gap1) only. Neither pins the cross-MECHANISM bridge
# between an unfiltered subtree `builtins.path` and a bare tree OID that
# a DIFFERENT pipeline populated. That seam — a working-but-untested
# fast-path one regression away from going dormant (PROPOSAL.md §5) — is
# what this test pins.
#
# NON-VACUITY (proven). The bridge populator (process 1) writes ONLY the
# bare `treeHashToNarHash[S]` row plus its own commit-rev
# `sourcePathToHash[git:<rev>]` row — it NEVER writes the subtree's
# `;shape`-suffixed `tree:<S>;shape=...` `sourcePathToHash` row. So in
# process 2 the ONLY thing that can save the subtree walk is the bare
# tree-OID bridge. Empirically: with process 1 OMITTED (the
# `treeHashToNarHash[S]` row absent — equivalently, with the
# `peekTreeHashBridge` consult reverted) process 2 re-walks the subtree
# (`copying '…/sub'`). With it present, process 2 does ZERO subtree
# walks. See PROPOSAL.md §6.4.10 gap4 + §6.11 Seam 3.

source ../common.sh

requireGit

clearStore
clearCache

# `_NIX_TEST_BARF_ON_UNCACHEABLE=1` makes fetch-to-store.cc throw if a
# source path resolves with no fingerprint and no filter — a backstop
# that the subtree-aware bare `tree:<sha>` fingerprint is in play.
export _NIX_TEST_BARF_ON_UNCACHEABLE=1

# main_repo: a /sub subtree (tree OID S) plus an unrelated root file.
main=$TEST_ROOT/main_repo
createGitRepo "$main"
mkdir -p "$main/sub"
echo "stable subtree content" > "$main/sub/file.txt"
echo "root file (main)" > "$main/top.txt"
git -C "$main" add -A
git -C "$main" commit -m 'main rev1'
mrev=$(git -C "$main" rev-parse HEAD)
sub_sha=$(git -C "$main" rev-parse "HEAD:sub")

# sub_repo: a repo whose ROOT TREE is byte-identical to main_repo's /sub
# subtree — same blob OID for file.txt, same single-entry tree, so the
# root tree OID equals `sub_sha`. Materialising this WHOLE input is the
# "same subtree reached another way": it populates treeHashToNarHash on
# the shared tree OID via Track Z.gap1's getRootTreeHash().
sub=$TEST_ROOT/sub_repo
createGitRepo "$sub"
echo "stable subtree content" > "$sub/file.txt"   # exact same bytes as main_repo/sub/file.txt
git -C "$sub" add -A
git -C "$sub" commit -m 'sub rev1'
srev=$(git -C "$sub" rev-parse HEAD)
sub_root_tree=$(git -C "$sub" rev-parse 'HEAD^{tree}')

echo "sub_sha (main:sub)        = $sub_sha"
echo "sub_root_tree (sub root)  = $sub_root_tree"

# Setup sanity: the two trees MUST share an OID, or the bridge cannot
# fire (it keys on the shared tree OID).
[[ "$sub_sha" = "$sub_root_tree" ]] \
    || fail "test setup invalid: main_repo:/sub tree OID ($sub_sha) != sub_repo root tree OID ($sub_root_tree); the two paths do not share a tree OID, so the bridge has nothing to share"

# ---------------------------------------------------------------------
# Process 1 — the bridge WRITER. Materialise the WHOLE sub_repo input.
# Track Z.gap1 surfaces its root tree OID (== sub_sha) and writes
# treeHashToNarHash[sub_sha] -> narHash. The store path name is the
# fetcher default ("source"); we reuse that exact name in process 2 so
# the bridged narHash reconstructs an ALREADY-VALID store path (no Copy
# re-walk on the hit).
# ---------------------------------------------------------------------
log1=$TEST_ROOT/eval1.log
out1=$(nix eval --impure --raw --expr \
    "(builtins.fetchTree { type = \"git\"; url = \"file://$sub\"; rev = \"$srev\"; allRefs = true; }).outPath" \
    -vvvv 2> "$log1")
echo "process1 (whole sub_repo) out1=$out1"

# It must have populated the bridge row on the shared tree OID.
command grep -qE "treeHashToNarHash:.*$sub_sha" "$log1" \
    || fail "process 1 did not touch treeHashToNarHash on the shared tree OID $sub_sha; the gap1 root-tree-OID writeback did not fire. Log:
$(cat "$log1")"

# ---------------------------------------------------------------------
# Process 2 — the bridge CONSUMER, FRESH process (in-process memos
# empty). An UNFILTERED builtins.path over main_repo's /sub. The only
# persistent row that can save the subtree walk is treeHashToNarHash on
# the shared tree OID — process 1 never wrote the `;shape`-suffixed
# subtree sourcePathToHash row. `name = "source"` matches process 1 so
# the bridged narHash reconstructs the same already-valid store path.
# ---------------------------------------------------------------------
log2=$TEST_ROOT/eval2.log
out2=$(nix eval --impure --raw --expr \
    "builtins.path { path = (builtins.fetchTree { type = \"git\"; url = \"file://$main\"; rev = \"$mrev\"; allRefs = true; }).outPath + \"/sub\"; name = \"source\"; }" \
    -vvvv 2> "$log2")
echo "process2 (unfiltered subtree builtins.path) out2=$out2"

# (a) Same NAR (the vanilla dump of tree S, reached two ways) + same name
#     ⇒ same content-addressed store path.
[[ "$out1" = "$out2" ]] \
    || fail "store paths differ across the two ways of reaching tree $sub_sha: $out1 vs $out2"

# (b) THE gap4 ASSERTION: process 2 must NOT walk the SUBTREE. The
#     bare-tree-OID bridge (peekTreeHashBridge on the bare tree:<sub-sha>
#     fingerprint that getFingerprint returns past the ;shape field)
#     supplies the narHash with no walk, and the reconstructed store path
#     is already valid so there is no Copy either.
#
#     We scope the grep to the /sub subpath: evaluating
#     `(fetchTree main_repo).outPath` independently forces the WHOLE
#     main_repo input to be hashed (an orthogonal cost `.outPath`
#     demands, present in subtree-dedup.sh too) — that root-level walk is
#     NOT the subtree walk gap4 is about.
if command grep -E "(hashing|copying) '[^']*/sub'" "$log2"; then
    fail "process 2 WALKED the /sub subtree; the bare-tree-OID bridge did not fire for the unfiltered builtins.path subtree (gap4 regressed). Log:
$(cat "$log2")"
fi

# (c) Belt-and-braces: the precise bridge signal must be present — a
#     cross-pipeline hit on treeHashToNarHash for the shared tree OID.
command grep -qE "cross-pipeline cache hit via treeHashToNarHash|using cache entry 'treeHashToNarHash:.*$sub_sha" "$log2" \
    || fail "process 2 shows no treeHashToNarHash bridge hit on $sub_sha; the subtree did not take the bare-tree-OID bridge. Log:
$(cat "$log2")"

echo "unfiltered-subtree-bridge: ok"
