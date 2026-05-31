#!/usr/bin/env bash

# Tests the `git-lazy-fetch` setting's SAFE-FALLTHROUGH behaviour.
#
# `git-lazy-fetch` creates partial (blob:none) clones so blobs are
# backfilled on demand — but ONLY for http(s) remotes whose server
# advertises protocol-v2 `filter` (because the on-demand backfill,
# `GitPromisorProvider`, is HTTP-only; marking a non-fetchable repo
# partial would omit blobs nothing could ever fetch back). For every
# other remote it must transparently fall through to a normal full
# fetch.
#
# The full create→fetch→backfill loop needs a real HTTP git server
# advertising `uploadpack.allowFilter`, which this functional harness
# doesn't provide; that path is covered by the unit tests in
# `git-promisor-wiring.cc` (creation/config) and verified end-to-end
# manually (PROPOSAL.md §6.2.2). Here we lock in the property that
# matters for safety: turning the setting ON must never break or slow
# down a `file://` git input.

# shellcheck source=common.sh
source common.sh

requireGit

repo="$TEST_ROOT/lazy-fallthrough"
createGitRepo "$repo"

mkdir -p "$repo/sub"
echo "wanted contents" > "$repo/sub/wanted.txt"
echo "{ outputs = _: {}; }" > "$repo/flake.nix"
git -C "$repo" add flake.nix sub/wanted.txt
git -C "$repo" commit -m "c1"
rev=$(git -C "$repo" rev-parse HEAD)

# With git-lazy-fetch ENABLED, a file:// input must still fetch and
# read normally — the safety gate skips partial-clone marking for
# non-http(s) transports.
path=$(nix eval --impure --raw --option git-lazy-fetch true \
    --expr "(builtins.fetchGit { url = \"file://$repo\"; rev = \"$rev\"; }).outPath")
[[ -d "$path" ]]
[[ "$(cat "$path/sub/wanted.txt")" == "wanted contents" ]]

# Reading a file out of the tree must work (would throw GIT_ENOTFOUND
# if we had wrongly marked this file:// repo partial without a usable
# backfill provider).
content=$(nix eval --impure --raw --option git-lazy-fetch true \
    --expr "builtins.readFile (builtins.fetchGit { url = \"file://$repo\"; rev = \"$rev\"; } + \"/sub/wanted.txt\")")
[[ "$content" == "wanted contents" ]]

# The setting must also be a no-op on the result identity: the outPath
# with the setting on must equal the outPath with it off (laziness is
# transparent — same content, same store path).
pathOff=$(nix eval --impure --raw --option git-lazy-fetch false \
    --expr "(builtins.fetchGit { url = \"file://$repo\"; rev = \"$rev\"; }).outPath")
[[ "$path" == "$pathOff" ]]

# The env-var default must be honoured too (NIX_GIT_LAZY_FETCH=1 ⇒ on),
# again falling through safely for file://.
path2=$(NIX_GIT_LAZY_FETCH=1 nix eval --impure --raw \
    --expr "(builtins.fetchGit { url = \"file://$repo\"; rev = \"$rev\"; }).outPath")
[[ "$path2" == "$path" ]]

# Pin the env-var → setting wiring NON-VACUOUSLY: over file:// the
# outPath is identical whether lazy is on or off (the safety gate skips
# partial-clone creation for non-http(s)/ssh), so the equality above
# proves transparency but NOT that the env var toggled anything. The
# setting value reported by `nix config show <key>` (which prints just
# the value) is the direct observable and does flip:
[[ "$(nix config show git-lazy-fetch)" == "false" ]]
[[ "$(NIX_GIT_LAZY_FETCH=1 nix config show git-lazy-fetch)" == "true" ]]
# An explicit config value must win over the env var.
[[ "$(NIX_GIT_LAZY_FETCH=1 nix config show --option git-lazy-fetch false git-lazy-fetch)" == "false" ]]

echo "git-lazy-fetch safe-fallthrough tests passed"
