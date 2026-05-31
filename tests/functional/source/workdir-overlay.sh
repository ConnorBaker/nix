#!/usr/bin/env bash

# Item 4: WorkdirDelta libgit2-backed Overlay end-to-end test.
#
# The WorkdirDelta overlay wraps a dirty git workdir in:
#   SoundnessGuard(E∪W) ∘ DirectorySynthesizer(E) ∘ Layer([Restrict(E)(disk), Mask(W)(base)])
# where E = dirtyFiles, W = deletedFiles.
#
# What the overlay does (and what this test checks):
#   - The NAR walk (fetchGit / fetchToStore) sees dirty bytes for
#     tracked-modified files, nothing for deleted files, and committed
#     bytes for unchanged files.
#   - The fingerprint is dirty-aware: it includes a `;d=<delta>` suffix
#     derived from the modified/deleted delta, so cache rows for the
#     dirty workdir don't collide with the committed-tree rows.
#   - A second eval of the same dirty workdir reuses the persistent
#     sourcePathToHash cache row rather than re-walking.
#
# What the overlay does NOT do:
#   - Serve dirty bytes through flake-input string interpolation.
#     `inputs.repo + "/modify-me"` resolves through `outPath`, which
#     is the committed-tree store path (narHash is the committed NAR).
#     See doc/tecnix-survey/PROPOSAL.md §6.3 (Item 4) for the full explanation.
#
# We use `builtins.fetchGit { url = "..."; }` directly (not a locked
# flake input) so the overlay accessor IS the materialisation source
# and the resulting store path reflects the workdir state.

source ../common.sh

requireGit

clearStore
clearCache

repo=$TEST_ROOT/repo
createGitRepo "$repo"

# Two committed files: one to modify, one to delete.
echo "committed contents" > "$repo/modify-me"
echo "to be deleted" > "$repo/delete-me"
mkdir "$repo/keep"
echo "stable" > "$repo/keep/stable.txt"
git -C "$repo" add modify-me delete-me keep/stable.txt
git -C "$repo" commit -m 'initial commit'
headRev=$(git -C "$repo" rev-parse HEAD)

# Make the workdir dirty:
#   - `modify-me` is changed (tracked-modified)
#   - `delete-me` is removed via `git rm --cached` (un-staged) then rm
#   - `keep/stable.txt` is unchanged
#   - `untracked.txt` is created but NOT added (untracked)
echo "DIRTY OVERRIDE" > "$repo/modify-me"
git -C "$repo" rm --cached delete-me
rm "$repo/delete-me"
echo "this should be invisible" > "$repo/untracked.txt"

# Quick sanity check that libgit2 sees what we expect.
git -C "$repo" status --porcelain | sort

log=$TEST_ROOT/eval.log

# Materialise the dirty workdir into the store via `builtins.fetchGit`.
# This exercises the overlay's NAR walk: dirty bytes for modify-me,
# whiteout for delete-me, committed bytes for keep/stable.txt.
outPath=$(nix eval --impure --raw \
    --expr "(builtins.fetchGit { url = \"file://$repo\"; }).outPath" \
    2> "$log")
[[ -n "$outPath" ]] \
    || fail "builtins.fetchGit returned empty outPath. Log:
$(cat "$log")"

echo "outPath: $outPath"

# 1) Modified file contains the workdir bytes (overlay routed modify-me
#    to the disk accessor, not the committed tree).
modified=$(cat "$outPath/modify-me")
[[ "$modified" = "DIRTY OVERRIDE" ]] \
    || fail "modify-me content mismatch.
expected: 'DIRTY OVERRIDE'
got:      '$modified'"

# 2) Unchanged file contains the committed bytes.
stable=$(cat "$outPath/keep/stable.txt")
[[ "$stable" = "stable" ]] \
    || fail "keep/stable.txt content mismatch.
expected: 'stable'
got:      '$stable'"

# 3) Deleted file is absent (whiteout via Mask(W)).
if [[ -e "$outPath/delete-me" ]]; then
    fail "deleted file 'delete-me' is present in store path — overlay whiteout broken"
fi

# 4) Untracked file is absent (not in wd.files; AllowList denies it).
if [[ -e "$outPath/untracked.txt" ]]; then
    fail "untracked file 'untracked.txt' is present in store path — should be invisible"
fi

# 5) Fingerprint is dirty-aware: the sourcePathToHash cache key must
#    include the `;d=<delta>` dirty-workdir suffix.
log2=$TEST_ROOT/eval2.log
nix eval --impure --raw \
    --expr "(builtins.fetchGit { url = \"file://$repo\"; }).outPath" \
    -vvvv 2> "$log2" > /dev/null

command grep -q "sourcePathToHash.*$headRev.*;d=" "$log2" \
    || fail "dirty-workdir fingerprint missing ';d=<delta>' suffix. Log:
$(cat "$log2")"

# 6) Cache row reuse on second eval: the third invocation should hit
#    the persistent sourcePathToHash row written by the first.
log3=$TEST_ROOT/eval3.log
nix eval --impure --raw \
    --expr "(builtins.fetchGit { url = \"file://$repo\"; }).outPath" \
    -vvvv 2> "$log3" > /dev/null

command grep -q "using cache entry.*sourcePathToHash" "$log3" \
    || fail "third eval did not reuse persistent cache row. Log:
$(cat "$log3")"

echo "workdir-overlay: ok"
