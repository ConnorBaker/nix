#!/usr/bin/env bash

# One address (doc/lazy-store/01-specification.md, section 9.11;
# 04-derivation.md, section 1.9, the fetchers' and the language's representation): evaluation
# names every source it copies or mounts by its SHA-256 git tree hash, which
# is git's own name for the tree and the store's object hash.  There is no
# setting and no second name.  A `sha256` -- a NAR hash -- is an older
# assertion: verified by the shim after the fetch, and answered from the
# memo when the tree it names is already present.  A `treeHash` is the
# assertion that costs nothing.  The lock file carries `treeHash` (version
# 8); `narHash` on a fetched tree is the lazy attribute the shim computes
# when a program forces it.

source common.sh

requireGit
# SHA-256 git objects in the store
requireDaemonNewerThan 2.31pre20250724

enableFeatures flakes

src=$TEST_ROOT/src
rm -rf "$src"
mkdir -p "$src/sub"
echo hello > "$src/a"
echo world > "$src/sub/b"
chmod +x "$src/sub/b"
ln -s a "$src/link"

# git's own name for the tree, from a SHA-256 repository
repo=$TEST_ROOT/scratch
createGitRepo "$repo" "--object-format=sha256"
cp -r "$src" "$repo/src"
git -C "$repo" add src
git -C "$repo" commit -q -m x
treeHash=$(nix hash convert --from base16 --to sri --hash-algo sha256 "$(git -C "$repo" rev-parse HEAD:src)")
# The NAR hash: the serialisation's, a function of the tree.
narHash=$(nix hash path --mode nar --format sri "$src")
# A hash that names no object in the store.
unknownHash=sha256-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=

caOf() { nix path-info --json --json-format 2 "$1" | jq -r '.info | to_entries[0].value.ca | "\(.method):\(.hash)"'; }
narHashOf() { nix path-info --json --json-format 2 "$1" | jq -r '.info | to_entries[0].value.narHash'; }

# builtins.path: git's name for the tree, the hashing door's name for the
# source and for the object; the object's NAR hash is the serialisation's.
p=$(nix eval --raw --impure --expr "builtins.path { path = $src; name = \"src\"; }")
[[ $(caOf "$p") == "git:$treeHash" ]]
[[ $(narHashOf "$p") == "$narHash" ]]
[[ $(nix hash path --mode git --algo sha256 --format sri "$src") == "$treeHash" ]]
[[ $(nix hash path --mode git --algo sha256 --format sri "$p") == "$treeHash" ]]

# The same object from every door evaluation names a source by.
[[ $(nix eval --raw --impure --expr "\"\${$src}\"") == "$p" ]]
t=$(nix eval --raw --impure --expr "(builtins.fetchTree { type = \"path\"; path = \"$src\"; }).outPath")
[[ $(caOf "$t") == "git:$treeHash" ]]
[[ $(narHashOf "$t") == "$narHash" ]]
# The tree's attributes: `treeHash` is the name's hash; `narHash` is still
# there, computed by a walk when forced.
[[ $(nix eval --raw --impure --expr "(builtins.fetchTree { type = \"path\"; path = \"$src\"; }).treeHash") == "$treeHash" ]]
[[ $(nix eval --raw --impure --expr "(builtins.fetchTree { type = \"path\"; path = \"$src\"; }).narHash") == "$narHash" ]]

# filterSource: the filtered tree's own git name.
filtered=$TEST_ROOT/src-filtered
rm -rf "$filtered"
cp -r "$src" "$filtered"
rm "$filtered/sub/b"
filteredTreeHash=$(nix hash path --mode git --algo sha256 --format sri "$filtered")
filteredNarHash=$(nix hash path --mode nar --format sri "$filtered")
f=$(nix eval --raw --impure --expr "builtins.filterSource (p: t: baseNameOf p != \"b\") $src")
[[ $(caOf "$f") == "git:$filteredTreeHash" ]]
# A named tree walked whole and then filtered: the filtered walk is not
# answered from the memo of the whole.
f2=$(nix eval --raw --impure --expr "builtins.filterSource (p: t: baseNameOf p != \"b\") (builtins.fetchTree { type = \"path\"; path = \"$src\"; }).outPath")
[[ $(caOf "$f2") == $(caOf "$f") ]]

# A `sha256` asserts the NAR hash, verified by the shim after the fetch:
# accepted when right, the path the same; refused when wrong.  (A plain
# directory has no name, so its pair is not recorded in the memo; the memo's
# evidence is the tarball's below, whose tree is named.)
[[ $(nix eval --raw --impure --expr "builtins.path { path = $src; name = \"src\"; sha256 = \"$narHash\"; }") == "$p" ]]
# API-GUESS: exit 102 -- addPath verifies through `assertNarHash`, whose
# error is Error(102, "NAR hash mismatch in input '%s' ...").
expectStderr 102 nix eval --raw --impure --expr "builtins.path { path = $src; name = \"src\"; sha256 = \"$unknownHash\"; }" | grepQuiet "NAR hash mismatch"
# Under a filter the assertion is on the filtered NAR.
[[ $(nix eval --raw --impure --expr "builtins.path { path = $src; filter = (p: t: baseNameOf p != \"b\"); sha256 = \"$filteredNarHash\"; }") == "$f" ]]
expectStderr 102 nix eval --raw --impure --expr "builtins.path { path = $src; filter = (p: t: baseNameOf p != \"b\"); sha256 = \"$unknownHash\"; }" | grepQuiet "NAR hash mismatch"

# A `treeHash` asserts the name itself.  A present tree is found by its
# name without reading the path -- the path need not exist -- and a wrong
# tree hash on a real path is refused after the walk.
# API-GUESS: the fast path runs before the path is read, so a path that does
# not exist is fine when the tree is present.
[[ $(nix eval --raw --impure --expr "builtins.path { path = /does-not-exist/must-remain-unused/src; treeHash = \"$treeHash\"; }") == "$p" ]]
expectStderr 102 nix eval --raw --impure --expr "builtins.path { path = $src; name = \"src\"; treeHash = \"$unknownHash\"; }" | grepQuiet "tree hash mismatch"

# fetchTarball: the unpacked tree under its git name.  Pinned by the NAR
# hash afterwards, the tree is found through the memo without touching the
# URL (the tarball's tree is named, so the shim recorded the pair); pinned
# by the tree hash, it is found by its name; a wrong NAR hash is refused.
tar -cf "$TEST_ROOT/src.tar" -C "$TEST_ROOT" src
tOn=$(nix eval --raw --impure --expr "fetchTarball \"file://$TEST_ROOT/src.tar\"")
[[ $(caOf "$tOn") == "git:$treeHash" ]]
[[ $(nix eval --raw --impure --expr "fetchTarball { url = \"file:///does-not-exist/must-remain-unused/src.tar\"; sha256 = \"$narHash\"; }") == "$tOn" ]]
# API-GUESS: `fetchTarball { treeHash }`, the new argument of 04 section 1.9.
[[ $(nix eval --raw --impure --expr "fetchTarball { url = \"file:///does-not-exist/must-remain-unused/src.tar\"; treeHash = \"$treeHash\"; }") == "$tOn" ]]
expectStderr 102 nix eval --raw --impure --expr "fetchTarball { url = \"file://$TEST_ROOT/src.tar\"; sha256 = \"$unknownHash\"; }" | grepQuiet "NAR hash mismatch"

# Named as before: a flat file is flat, a source with references keeps the
# flat name, text is text.  These are the format's content-address methods,
# asked for by name, not a second naming of sources.
[[ $(caOf "$(nix eval --raw --impure --expr "builtins.path { path = $src/a; recursive = false; }")") == flat:* ]]
# shellcheck disable=SC2016 # the ${} is Nix string interpolation, not shell
[[ $(caOf "$(nix eval --raw --impure --expr 'let b = builtins.toFile "b" "b"; a = builtins.toFile "a" "${b}"; in builtins.path { path = a; name = "a-copy"; }')") == nar:* ]]
[[ $(caOf "$(nix eval --raw --expr 'builtins.toFile "t" "t"')") == text:* ]]

# A flake input: the lock file is version 8 and carries the tree hash and
# no NAR hash; the input's path is git's name; `narHash` in the flake's
# outputs still evaluates, through the shim; a second lock changes nothing.
flake=$TEST_ROOT/flake
rm -rf "$flake"
mkdir -p "$flake"
cat > "$flake/flake.nix" <<NIX
{ inputs.src = { url = "path:$src"; flake = false; }; outputs = { self, src }: { p = src.outPath; n = src.narHash; t = src.treeHash; }; }
NIX
nix flake lock "$flake"
lock=$flake/flake.lock
[[ $(jq -r .version "$lock") == 8 ]]
[[ $(jq -r .nodes.src.locked.treeHash "$lock") == "$treeHash" ]]
[[ $(jq -r '.nodes.src.locked | has("narHash")' "$lock") == false ]]
[[ $(nix flake metadata --json "$flake" | jq -r .locks.nodes.src.locked.treeHash) == "$treeHash" ]]
[[ $(nix eval --raw "$flake#t") == "$treeHash" ]]
[[ $(nix eval --raw "$flake#n") == "$narHash" ]]
[[ $(caOf "$(nix eval --raw "$flake#p")") == "git:$treeHash" ]]
cp "$lock" "$TEST_ROOT/lock-v8"
nix flake lock "$flake"
cmp "$lock" "$TEST_ROOT/lock-v8"

# A version-7 lock, written by hand from the version-8 one: the node asserts
# the NAR hash.  Evaluation succeeds (the shim verifies it against the
# tree); a wrong NAR hash is refused; `nix flake update` refetches the
# input and the lock written is version 8 with the node's `treeHash`.
jq --arg narHash "$narHash" '.version = 7 | .nodes.src.locked |= (del(.treeHash) | .narHash = $narHash)' "$TEST_ROOT/lock-v8" > "$lock"
[[ $(jq -r .version "$lock") == 7 ]]
[[ $(nix eval --raw --no-write-lock-file "$flake#p") == "$t" ]]
[[ $(nix eval --raw --no-write-lock-file "$flake#n") == "$narHash" ]]
bad=$TEST_ROOT/flake-bad
rm -rf "$bad"
mkdir -p "$bad"
cp "$flake/flake.nix" "$bad/"
jq --arg narHash "$unknownHash" '.nodes.src.locked.narHash = $narHash' "$lock" > "$bad/flake.lock"
expectStderr 102 nix eval --raw --no-write-lock-file "$bad#p" | grepQuiet "NAR hash mismatch"
nix flake update src --flake "$flake"
[[ $(jq -r .version "$lock") == 8 ]]
[[ $(jq -r .nodes.src.locked.treeHash "$lock") == "$treeHash" ]]
[[ $(jq -r '.nodes.src.locked | has("narHash")' "$lock") == false ]]
cmp "$lock" "$TEST_ROOT/lock-v8"

# A binary cache holds and serves the git-named objects, with their address.
cache=$TEST_ROOT/cache
rm -rf "$cache"
nix copy --to "file://$cache" "$p" "$t" "$tOn"
[[ $(nix path-info --store "file://$cache" --json --json-format 2 "$tOn" | jq -r '.info | to_entries[0].value.ca.method') == git ]]

# With the objects gone locally, a pinned fetchTarball substitutes the tree
# under its name -- through the memo for a NAR hash, directly for a tree
# hash -- and the locked flake input, named `source` too, is substituted
# under that name rather than copied again: the source directory is gone by
# then, so substitution is the only way it can succeed.
# `substitute` is set explicitly: without Internet the CLI turns
# substitution off unless the option was given on the command line
# (src/nix/main.cc, the `useNet` block), and the sandboxed test run has none.
nix store delete "$p" "$t" "$tOn" 2> /dev/null || nix-store --delete --ignore-liveness "$p" "$t" "$tOn"
[[ ! -e $tOn ]]
[[ $(nix eval --raw --impure --option substitute true --substituters "file://$cache" --expr "fetchTarball { url = \"file:///does-not-exist/must-remain-unused/src.tar\"; sha256 = \"$narHash\"; }") == "$tOn" ]]
[[ $(caOf "$tOn") == "git:$treeHash" ]]
nix store delete "$tOn" 2> /dev/null || nix-store --delete --ignore-liveness "$tOn"
[[ $(nix eval --raw --impure --option substitute true --substituters "file://$cache" --expr "fetchTarball { url = \"file:///does-not-exist/must-remain-unused/src.tar\"; treeHash = \"$treeHash\"; }") == "$tOn" ]]
nix store delete "$t" "$tOn" 2> /dev/null || nix-store --delete --ignore-liveness "$t" "$tOn"
rm -rf "$src"
[[ $(nix eval --raw --option substitute true --substituters "file://$cache" "$flake#p") == "$t" ]]
[[ $(caOf "$t") == "git:$treeHash" ]]
