#!/usr/bin/env bash

# A Git input's subtrees are named by their tree objects, so a subtree that
# is unchanged between two revisions is hashed and copied once: the second
# revision finds the store path in the source memo.  On the reference this
# subtree is copied again under the second revision, so this test states a
# cost property of the branch, not an agreement with the reference
# (doc/lazy-store/04-derivation.md, section 1.4).
#
# The inputs are fetched with exportIgnore = false, as flake inputs and
# fetchTree git inputs are.  builtins.fetchGit defaults to exportIgnore = true,
# and an export-ignore filter's subtrees carry no name (their contents depend
# on attribute files above them and possibly outside the tree), so its
# subtrees are cached per revision as before.

source common.sh

requireGit

clearStoreIfPossible

repo=$TEST_ROOT/subtree-names
rm -rf "$repo" "$TEST_HOME/.cache/nix"

git init -q "$repo"
git -C "$repo" config user.email "nix@example.org"
git -C "$repo" config user.name "Nix"

mkdir -p "$repo/shared"
echo a > "$repo/shared/a"
echo b > "$repo/shared/b"
echo 1 > "$repo/top"
git -C "$repo" add -A
git -C "$repo" commit -q -m one
rev1=$(git -C "$repo" rev-parse HEAD)

echo 2 > "$repo/top"
git -C "$repo" commit -q -am two
rev2=$(git -C "$repo" rev-parse HEAD)

subtree() {
    nix eval --impure --raw -vv --expr "
      let src = builtins.fetchGit { url = \"file://$repo\"; rev = \"$1\"; exportIgnore = false; };
      in builtins.path { path = src.outPath + \"/shared\"; name = \"shared\"; }
    " 2> "$TEST_ROOT/subtree-names-$1.log"
}

# The first revision: the subtree is copied.
path1=$(subtree "$rev1")
grepQuiet "copying '.*/shared' to the store" "$TEST_ROOT/subtree-names-$rev1.log"

# The second revision, same subtree, other tree elsewhere: the memo answers
# under the subtree's own name, and nothing is hashed or copied again.
path2=$(subtree "$rev2")
[[ $path1 == "$path2" ]]
grepQuietInverse "copying '.*/shared' to the store" "$TEST_ROOT/subtree-names-$rev2.log"
grepQuietInverse "hashing '.*/shared'" "$TEST_ROOT/subtree-names-$rev2.log"

# A filtered view of the subtree is named too: the same filter on the same
# tree is answered from the memo the second time round.
filtered() {
    nix eval --impure --raw -vv --expr "
      let src = builtins.fetchGit { url = \"file://$repo\"; rev = \"$1\"; exportIgnore = false; };
      in builtins.path {
        path = src.outPath + \"/shared\";
        name = \"shared-a\";
        filter = path: type: baseNameOf path != \"b\";
      }
    " 2> "$TEST_ROOT/subtree-names-filtered-$1.log"
}

filtered1=$(filtered "$rev1")
grepQuiet "copying '.*/shared' to the store" "$TEST_ROOT/subtree-names-filtered-$rev1.log"
filtered2=$(filtered "$rev2")
[[ $filtered1 == "$filtered2" ]]
[[ $filtered1 != "$path1" ]]
grepQuietInverse "copying '.*/shared' to the store" "$TEST_ROOT/subtree-names-filtered-$rev2.log"
grepQuietInverse "hashing '.*/shared'" "$TEST_ROOT/subtree-names-filtered-$rev2.log"

# The copy of an edited checkout is made from the store's own objects
# where the store holds them (doc/lazy-store/01-specification.md, section
# 9.10, "One route", the second route): `shared/` is clean and its tree
# was entered by the committed revision's copy, `top` is edited.  The
# copied path dumps to the checkout's bytes, each regular file is the
# blob's link -- `shared/a` shared with the committed path -- and
# verification finds every object of it.  Through a daemon the copy is
# the one route, and the same holds.
echo 3 > "$repo/top"
dirty=$(nix eval --impure --raw --expr "(builtins.fetchGit { url = \"file://$repo\"; exportIgnore = false; }).outPath")
committed=$(nix eval --impure --raw --expr "(builtins.fetchGit { url = \"file://$repo\"; rev = \"$rev2\"; exportIgnore = false; }).outPath")
[[ $(cat "$dirty/top") == 3 ]]
[[ $(cat "$committed/top") == 2 ]]
[[ $(stat --format=%i "$dirty/shared/a") == $(stat --format=%i "$committed/shared/a") ]]
[[ $(stat --format=%h "$dirty/shared/b") -ge 3 ]]
plain=$TEST_ROOT/subtree-names-plain
rm -rf "$plain"
cp -R "$repo" "$plain"
rm -rf "$plain/.git"
nix nar pack "$plain" > "$TEST_ROOT/subtree-names-plain.nar"
nix nar dump-path "$dirty" > "$TEST_ROOT/subtree-names-dirty.nar"
cmp "$TEST_ROOT/subtree-names-plain.nar" "$TEST_ROOT/subtree-names-dirty.nar"
if [[ -z ${NIX_REMOTE:-} ]]; then
    nix-store --verify --check-contents
fi
git -C "$repo" checkout -q -- top
