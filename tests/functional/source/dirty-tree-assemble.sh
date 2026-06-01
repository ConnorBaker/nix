#!/usr/bin/env bash

# Perf (item (b)): editing one file in a large dirty git working tree must
# NOT re-copy the whole tree into the store. `ensureLazyPathCopied` detects
# a dirty workdir overlay (a `SourceViewAccessor` with a `recipe::Overlay`
# over the committed-tree base), materialises that committed base ONCE
# (content-addressed, reused across edits), and ASSEMBLES the dirty store
# path from it: unchanged files are reflinked/hardlinked from the base,
# only changed files are written fresh.
#
# Non-vacuity: we assert the assembled source path SHARES INODES with the
# materialised base for unchanged files (the hardlink fallback on a
# non-reflinking store; on a CoW store the data extents are shared instead
# but inodes differ — we run on the test store which is extN/tmpfs, so the
# hardlink path is what fires) and that the changed file does NOT. A
# regression to a full copy would make every file a distinct inode.
#
# Soundness is enforced in-process by `assembleCAPathFromBase`'s re-hash
# guard (unit-tested in register-assembled-ca-path.cc, incl. the negative
# "mis-assembly is caught" case); here we additionally assert the
# assembled source path is a VALID store path with the CORRECT contents.

source ../common.sh

requireGit
clearStore
clearCache

count_matches() {
    local pattern=$1 file=$2 n
    n=$(command grep -cE "$pattern" "$file" 2>/dev/null || true)
    echo "${n:-0}"
}

system=$(nix eval --impure --raw --expr "builtins.currentSystem")

repo=$TEST_ROOT/repo
createGitRepo "$repo"
mkdir -p "$repo/pkgs"
# A tree big enough that "copy all" vs "link all but one" is a clear
# difference, small enough to stay fast.
n=40
for i in $(seq 1 $n); do echo "file $i stable content" > "$repo/pkgs/f$i.txt"; done
cat > "$repo/flake.nix" <<EOF
{
  outputs = { self }: {
    drv = derivation {
      name = "uses-src";
      system = "$system";
      builder = "/bin/sh";
      args = [ "-c" ":" ];
      src = self;
    };
  };
}
EOF
git -C "$repo" add -A
git -C "$repo" commit -qm init

# Make the working tree DIRTY by editing exactly one file, then
# instantiate the derivation (impure → dirty workdir overlay). Reading
# .drvPath forces the source to materialise at the derivation boundary,
# which is where the assembler runs.
echo "EDITED" > "$repo/pkgs/f1.txt"

log=$TEST_ROOT/eval.log
nix eval "git+file://$repo#drv.drvPath" --impure --raw -vvvv 2> "$log" > /dev/null

# The assembler must have fired (not the full-copy fallback).
grep -q "assembled '" "$log" \
    || fail "item (b) did not assemble the dirty tree from a base — no 'assembled' marker (fell back to full copy?). Log tail:
$(tail -20 "$log")"

# Locate the assembled source path and the materialised base in the store.
storeDir=$(nix eval --impure --raw --expr "builtins.storeDir")
src=$(nix derivation show "git+file://$repo#drv" --impure | jq -r '.derivations[].inputs.srcs[]')
[[ -n "$src" ]] || fail "drv has no input src"
srcPath="$storeDir/$src"
basePath=$(echo "$storeDir"/*-source-base)

[[ -d "$srcPath" ]] || fail "assembled source path '$srcPath' is not a directory"
[[ -d "$basePath" ]] || fail "materialised base '$basePath' not found"

# Contents correct: the edited file is the new content; an unchanged file
# is the original.
[[ "$(cat "$srcPath/pkgs/f1.txt")" == "EDITED" ]] || fail "changed file has wrong content"
[[ "$(cat "$srcPath/pkgs/f20.txt")" == "file 20 stable content" ]] || fail "unchanged file has wrong content"

# The assembled path is a first-class VALID store path (it passed the
# re-hash guard and was registered).
nix path-info "$srcPath" > /dev/null 2>&1 || fail "assembled source is not a valid store path"

# Inode sharing: count how many of the unchanged files share an inode with
# the base (hardlinked, not copied). On the test store (extN/tmpfs, no
# reflink) this should be all-but-the-changed-file.
shared=0
distinct=0
for i in $(seq 1 $n); do
    si=$(stat -c %i "$srcPath/pkgs/f$i.txt")
    bi=$(stat -c %i "$basePath/pkgs/f$i.txt")
    if [[ "$si" == "$bi" ]]; then
        shared=$((shared + 1))
    else
        distinct=$((distinct + 1))
    fi
done
echo "dirty-tree assemble: $shared/$n files hardlinked from base, $distinct rewritten"

# The changed file must be rewritten (distinct inode); the vast majority
# must be shared. We require at least n-2 shared (allow a little slack for
# any incidental store dedup), and the changed file specifically distinct.
f1Src=$(stat -c %i "$srcPath/pkgs/f1.txt")
f1Base=$(stat -c %i "$basePath/pkgs/f1.txt")
[[ "$f1Src" != "$f1Base" ]] || fail "the CHANGED file shares the base inode — it was not rewritten"

[[ "$shared" -ge $((n - 2)) ]] \
    || fail "ASSEMBLE REGRESSION: only $shared/$n unchanged files were hardlinked from the base; expected >= $((n - 2)). A full re-copy would make all files distinct inodes."

# --- per-EvalState base memo: two derivations over the SAME dirty source,
# materialised in ONE eval process, must materialise the committed base
# only ONCE. The base accessor carries no fingerprint, so without the memo
# `fetchToStore` re-walks + re-copies it for each consumer; the memo keys
# it on the committed tree OID (stable across the edit) and reuses it. ---
twocons=$TEST_ROOT/twocons
createGitRepo "$twocons"
mkdir -p "$twocons/pkgs"
for i in $(seq 1 $n); do echo "file $i stable content" > "$twocons/pkgs/f$i.txt"; done
cat > "$twocons/flake.nix" <<EOF
{
  outputs = { self }: {
    a = derivation { name = "a"; system = "$system"; builder = "/bin/sh"; args = [ "-c" ":" ]; src = self; };
    b = derivation { name = "b"; system = "$system"; builder = "/bin/sh"; args = [ "-c" ":" ]; src = self; };
  };
}
EOF
git -C "$twocons" add -A
git -C "$twocons" commit -qm init
echo "EDITED" > "$twocons/pkgs/f1.txt" # dirty

clearStore
clearCache
tlog=$TEST_ROOT/two.log
# A single eval process that instantiates BOTH a and b (both consume the
# same dirty `self` → the same committed base). The base is materialised
# by a `fetchToStore` Copy logged as `copying '«…»'` of the git accessor;
# the per-EvalState memo must keep that to exactly ONE across both.
nix eval --impure --raw -vvvv --expr "
  let f = builtins.getFlake \"git+file://$twocons\";
  in f.outputs.a.drvPath + f.outputs.b.drvPath
" 2> "$tlog" > /dev/null

# Count base materialisations. The ONLY `copying '…' to the store` line in
# this eval is the committed base's `fetchToStore` Copy (the assembled
# source itself is hashed in a scratch dir, not via a `copying` line). The
# assembler runs for BOTH a and b (asserted via the `assembled` marker),
# but the per-EvalState memo must keep the base Copy to exactly ONE.
assembledCount=$(count_matches "assembled '" "$tlog")
baseCopies=$(count_matches "copying '" "$tlog")
echo "dirty-tree assemble (2 drvs, 1 process): assembled=$assembledCount, base copied=$baseCopies time(s)"
[[ "$assembledCount" -ge 2 ]] \
    || fail "expected the assembler to run for both derivations (assembled=$assembledCount). Log:
$(cat "$tlog")"
[[ "$baseCopies" -le 1 ]] \
    || fail "BASE-MEMO REGRESSION: two derivations sharing one dirty source materialised the committed base $baseCopies times in one eval; the per-EvalState memo should keep it to 1. Log:
$(cat "$tlog")"

echo "dirty-tree-assemble: ok"
