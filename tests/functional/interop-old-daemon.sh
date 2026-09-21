#!/usr/bin/env bash

# This client against an OLD daemon (a Nix built from master, given through
# NIX_REFERENCE_BIN), for the three inputs whose store path this Nix names by
# a tree hash the old daemon cannot supply: `fetchTree {type="path"}`,
# `fetchTree {type="file"}` and a flake with a `path:` input.
#
# The old daemon describes a path by its NAR hash alone, so what needs the
# object hash of a path it holds computes it by one walk
# (`Store::queryObjectHash`, src/libstore/store-api.cc), and what this client
# *sends* it carries the NAR hash the old form needs
# (`CommonProto::writePathInfoHashes`).  The sources are git-addressed
# (doc/lazy-store/01-specification.md section 9.9), so the old daemon needs
# its `git-hashing` experimental feature to store them at all
# (doc/manual/rl-next/one-address.md, *Older peers and git-named sources*);
# without it every one of the three is refused at `AddToStore`, which is
# checked first.
#
# `tests/functional/object-hash.sh`'s interop block drives `builtins.path`,
# the two `fetchTree` kinds, the write buffer and the importer against the
# same daemon.  What is added here is the flake `path:` input -- a lock file
# written while the store is the old daemon's -- and the copy out of the old
# daemon into a binary cache and back.

source common.sh

TODO_NixOS

needLocalStore "the reference daemon is given a store root of this test"

if [[ -z "${NIX_REFERENCE_BIN:-}" || ! -x "$NIX_REFERENCE_BIN/nix" ]]; then
    skipTest "NIX_REFERENCE_BIN is not set, so no Nix built from master is in hand"
fi

gitHashingDisabled="experimental Nix feature 'git-hashing' is disabled"

# The three sources.
src=$TEST_ROOT/interop-src
rm -rf "$src"
mkdir -p "$src/d"
echo one > "$src/a"
echo two > "$src/d/b"
echo three > "$src/x"
chmod +x "$src/x"

plain=$TEST_ROOT/interop-plain
echo hello > "$plain"

dep=$TEST_ROOT/interop-dep
rm -rf "$dep"
mkdir -p "$dep"
echo depends > "$dep/content"

flakeDir=$TEST_ROOT/interop-flake
rm -rf "$flakeDir"
mkdir -p "$flakeDir"
cat > "$flakeDir/flake.nix" <<EOF
{
  inputs.dep = {
    url = "path:$dep";
    flake = false;
  };
  outputs = { self, dep }: {
    depPath = dep.outPath;
    depTreeHash = dep.treeHash;
  };
}
EOF

# A second set of the same three, for the without-feature block at the end: a
# path the store already holds is not added again, so the refusal needs sources
# the daemon has never seen.
src2=$TEST_ROOT/interop-src2
rm -rf "$src2"
mkdir -p "$src2"
echo four > "$src2/a"

plain2=$TEST_ROOT/interop-plain2
echo goodbye > "$plain2"

dep2=$TEST_ROOT/interop-dep2
rm -rf "$dep2"
mkdir -p "$dep2"
echo other > "$dep2/content"

flakeDir2=$TEST_ROOT/interop-flake2
rm -rf "$flakeDir2"
mkdir -p "$flakeDir2"
cat > "$flakeDir2/flake.nix" <<EOF
{
  inputs.dep = {
    url = "path:$dep2";
    flake = false;
  };
  outputs = { self, dep }: {
    depPath = dep.outPath;
  };
}
EOF

# The names this Nix gives them, computed here, where the store is this one.
srcTreeHash=$(nix hash path --mode git --algo sha256 --format sri "$src")
plainBlobHash=$(nix hash path --mode git --algo sha256 --format sri "$plain")
depTreeHash=$(nix hash path --mode git --algo sha256 --format sri "$dep")

oldRoot=$TEST_ROOT/interop-old-daemon
rm -rf "$oldRoot"
mkdir -p "$oldRoot/store" "$oldRoot/var/nix" "$oldRoot/var/log/nix"

(
    export NIX_STORE_DIR=$oldRoot/store NIX_STATE_DIR=$oldRoot/var/nix NIX_LOG_DIR=$oldRoot/var/log/nix \
        NIX_DAEMON_SOCKET_PATH=$oldRoot/socket
    oldPid=
    # The reference daemon does not always exit on SIGTERM (it loops on
    # "interrupted by the user" after a refused operation): poll, then kill.
    stopOldDaemon() {
        kill "$oldPid" 2> /dev/null || true
        for ((i = 0; i < 50; i++)); do
            kill -0 "$oldPid" 2> /dev/null || break
            sleep 0.1
        done
        kill -9 "$oldPid" 2> /dev/null || true
        wait "$oldPid" 2> /dev/null || true
        oldPid=
    }
    trap 'if [[ -n $oldPid ]]; then stopOldDaemon; fi' EXIT
    startOldDaemon() {
        rm -f "$NIX_DAEMON_SOCKET_PATH"
        # `NIX_REMOTE` is set to `daemon` below for the client; the daemon
        # itself must open the local store, not a connection to itself.  The
        # harness points the dynamic loader at this build's libraries; the
        # reference binary must find its own.
        NIX_REMOTE='' NIX_CONFIG="extra-experimental-features = $1" \
            env -u DYLD_LIBRARY_PATH -u DYLD_FALLBACK_LIBRARY_PATH -u LD_LIBRARY_PATH \
            "$NIX_REFERENCE_BIN/nix" --extra-experimental-features nix-command daemon &
        oldPid=$!
        for ((i = 0; i < 100; i++)); do
            [[ -S $NIX_DAEMON_SOCKET_PATH ]] && break
            kill -0 "$oldPid" || fail "the reference daemon died"
            sleep 0.1
        done
        [[ -S $NIX_DAEMON_SOCKET_PATH ]] || fail "the reference daemon did not start"
    }
    export NIX_REMOTE=daemon

    startOldDaemon "nix-command git-hashing"

    # `path`: the tree, named by its tree hash, which the walk gives.
    pathOut=$(nix eval --impure --raw --expr "(builtins.fetchTree { type = \"path\"; path = \"$src\"; }).outPath")
    [[ $(cat "$pathOut/a") == one ]]
    [[ $(cat "$pathOut/d/b") == two ]]
    [[ -x $pathOut/x ]]
    [[ $(nix eval --impure --raw --expr "(builtins.fetchTree { type = \"path\"; path = \"$src\"; }).treeHash") \
       == "$srcTreeHash" ]]

    # `file`: a plain file, named by its blob id.
    fileOut=$(nix eval --impure --raw --expr "(builtins.fetchTree { type = \"file\"; url = \"file://$plain\"; }).outPath")
    [[ $(cat "$fileOut") == hello ]]
    [[ $(nix eval --impure --raw --expr "(builtins.fetchTree { type = \"file\"; url = \"file://$plain\"; }).treeHash") \
       == "$plainBlobHash" ]]

    # A flake with a `path:` input: the fetcher names the tree by one walk and
    # copies it into the old daemon's store before evaluation sees it
    # (`PathInputScheme::getAccessor`, src/libfetchers/path.cc; 01
    # section 10, *a `path:` input outside the store is named first*).
    rm -f "$flakeDir/flake.lock"
    depOut=$(nix eval --raw "$flakeDir#depPath")
    [[ $(cat "$depOut/content") == depends ]]
    [[ $(nix eval --raw "$flakeDir#depTreeHash") == "$depTreeHash" ]]
    nix-store --check-validity "$depOut"
    # The lock the client wrote while the store was the old daemon's is this
    # Nix's: version 8, `treeHash`, no `narHash`.
    [[ -e $flakeDir/flake.lock ]]
    jq -e '.version == 8' "$flakeDir/flake.lock" > /dev/null
    jq -e --arg h "$depTreeHash" \
        '[.nodes[] | select(.locked != null) | .locked] | length == 1 and all(.treeHash == $h) and all(has("narHash") | not)' \
        "$flakeDir/flake.lock" > /dev/null

    # The copy out of the old daemon: this client reads the three paths from a
    # daemon that describes them by their NAR hash alone, and writes a cache
    # whose `.narinfo` carries the object hash it computed by the walk.
    cache=$oldRoot/cache
    rm -rf "$cache"
    nix copy --to "file://$cache" "$pathOut" "$fileOut" "$depOut"
    for p in "$pathOut" "$fileOut" "$depOut"; do
        base=$(basename "$p")
        narinfo=$cache/$(echo "$base" | cut -c1-32).narinfo
        [[ -e $narinfo ]]
        grepQuiet "^ObjectHash: git:sha256:$(nix hash path --mode git --algo sha256 --format base16 "$p")\$" "$narinfo"
        grepQuiet "^NarHash: sha256:" "$narinfo"
        grepQuiet "^CA: fixed:git:sha256:" "$narinfo"
    done

    # And back into a store of this Nix, where the same names hold.
    newStore=$oldRoot/new-store
    rm -rf "$newStore"
    nix copy --from "file://$cache" --to "$newStore" --no-check-sigs "$pathOut" "$fileOut" "$depOut"
    for p in "$pathOut" "$fileOut" "$depOut"; do
        base=$(basename "$p")
        [[ $(nix path-info --store "$newStore" --json --json-format 4 "$p" \
             | jq -r --arg b "$base" '.info[$b].objectHash') \
           == "git:sha256:$(nix hash path --mode git --algo sha256 --format base16 "$p")" ]]
    done

    # What the old daemon holds is described by its NAR hash alone: format 2
    # shows it, and format 4 refuses with the message that names the
    # alternative (src/nix/path-info.cc, `toJSON`).
    [[ $(nix path-info --json --json-format 2 "$depOut" | jq -r '.info | to_entries[0].value.narHash') \
       == "$(nix hash path --mode nar --format sri "$depOut")" ]]
    expectStderr 1 nix path-info --json --json-format 4 "$depOut" | grepQuiet "use a lower format"

    # Without the feature the old daemon refuses each of the three at
    # `AddToStore`, the address being `fixed:git:`.  The sources are fresh
    # ones, since a path the store already holds is not added again (the
    # name-first door) and would show nothing.
    stopOldDaemon
    startOldDaemon "nix-command"
    expectStderr 1 nix eval --impure --raw --expr \
        "(builtins.fetchTree { type = \"path\"; path = \"$src2\"; }).outPath" \
        | grepQuiet "$gitHashingDisabled"
    expectStderr 1 nix eval --impure --raw --expr \
        "(builtins.fetchTree { type = \"file\"; url = \"file://$plain2\"; }).outPath" \
        | grepQuiet "$gitHashingDisabled"
    expectStderr 1 nix eval --raw "$flakeDir2#depPath" | grepQuiet "$gitHashingDisabled"
)
