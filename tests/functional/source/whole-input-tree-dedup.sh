#!/usr/bin/env bash

# Track Z.gap1 (whole-input root-tree-OID bridge) end-to-end test.
#
# A whole git input's fetcher-cache row keys on the COMMIT REV, not the
# root tree OID (git.cc getFingerprint returns rev.gitRev()). So two
# commits with an IDENTICAL root tree historically re-walked the tree
# once each. gap1 surfaces the root tree OID via
# SourceAccessor::getRootTreeHash() and bridges through the cross-pipeline
# `treeHashToNarHash` projection, so the second commit's whole-input NAR
# walk is avoided.
#
# Non-vacuity: with gap1 reverted, the second eval re-walks (a
# "copying '...'" line appears for the second rev). This test asserts it
# does NOT.
#
# See doc/tecnix-survey/PROPOSAL.md Track Z / §6.4.9.

source ../common.sh

requireGit

clearStore
clearCache

repo=$TEST_ROOT/repo
createGitRepo "$repo"

# Commit 1: some content.
mkdir -p "$repo/sub"
echo "tree-dedup content" > "$repo/sub/file.txt"
echo "root file" > "$repo/root.txt"
git -C "$repo" add -A
git -C "$repo" commit -m 'rev1'
rev1=$(git -C "$repo" rev-parse HEAD)
tree1=$(git -C "$repo" rev-parse 'HEAD^{tree}')

# Commit 2: change a file then change it back, producing a NEW commit
# whose ROOT TREE is byte-identical to rev1's (same tree OID) but whose
# commit OID differs.
echo "scratch" > "$repo/root.txt"
git -C "$repo" commit -a -m 'scratch'
echo "root file" > "$repo/root.txt"   # restore exact original bytes
git -C "$repo" commit -a -m 'rev2-same-tree'
rev2=$(git -C "$repo" rev-parse HEAD)
tree2=$(git -C "$repo" rev-parse 'HEAD^{tree}')

echo "rev1=$rev1 tree1=$tree1"
echo "rev2=$rev2 tree2=$tree2"

# Setup sanity: distinct commits, identical root tree.
[[ "$rev1" != "$rev2" ]] || fail "test setup invalid: revs are identical"
[[ "$tree1" = "$tree2" ]] \
    || fail "test setup invalid: root trees differ ($tree1 vs $tree2); cannot exercise the tree-OID bridge"

# (1) Materialise the WHOLE input at rev1 (copies the tree into the
#     store, populating treeHashToNarHash on the root tree OID).
log1=$TEST_ROOT/eval1.log
out1=$(nix eval --impure --raw --expr \
    "(builtins.fetchTree { type = \"git\"; url = \"file://$repo\"; rev = \"$rev1\"; allRefs = true; }).outPath" \
    -vvvv 2> "$log1")
echo "out1=$out1"

# (2) Materialise the WHOLE input at rev2 (different commit rev, same
#     root tree) in a FRESH process so the in-process memo is empty and
#     only the persistent treeHashToNarHash bridge can save the walk.
log2=$TEST_ROOT/eval2.log
out2=$(nix eval --impure --raw --expr \
    "(builtins.fetchTree { type = \"git\"; url = \"file://$repo\"; rev = \"$rev2\"; allRefs = true; }).outPath" \
    -vvvv 2> "$log2")
echo "out2=$out2"

# Same root tree ⇒ same NAR ⇒ same content-addressed store path.
[[ "$out1" = "$out2" ]] \
    || fail "whole-input store paths differ across same-tree revs: $out1 vs $out2"

# The gap1 bridge: the second eval must NOT re-walk (copy) the tree. With
# gap1 active, rev2 hits treeHashToNarHash on the shared root tree OID.
if command grep -E "(hashing|copying) '" "$log2"; then
    fail "second eval (rev2, same root tree) re-walked the tree; root-tree-OID bridge did not fire. Log:
$(cat "$log2")"
fi

# Belt-and-braces: the second log should show the cross-pipeline hit on
# the root tree OID (the debug line gap1 emits) OR a plain warm cache hit.
command grep -qE "root-tree-OID|using cache entry|cross-pipeline cache hit" "$log2" \
    || fail "second eval shows no cache hit at all; bridge silent. Log:
$(cat "$log2")"

echo "whole-input-tree-dedup: ok"
