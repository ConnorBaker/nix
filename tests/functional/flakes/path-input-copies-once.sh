#!/usr/bin/env bash

# A `path:` flake reference outside the store is named first and copied
# only when the store lacks the tree (doc/lazy-store/01-specification.md,
# section 9.9, the `path` fetcher's door; section 10): a second evaluation
# of an unchanged tree copies nothing, and an edited tree is copied again.
# Before, every evaluation dumped and restored the tree (`path.cc`,
# `addToStoreFromDump` whenever the path was not a valid `-source` store
# path); this test failed at the second evaluation's `copying`.

source ./common.sh

flakeDir=$TEST_ROOT/path-input
mkdir -p "$flakeDir"
cat > "$flakeDir/flake.nix" <<EOF
{ outputs = { self }: { x = builtins.readFile (self + "/data"); }; }
EOF
echo hello > "$flakeDir/data"

# The first evaluation copies the tree: the `copying` activity at -vv.
[[ $(nix eval --raw -vv --no-write-lock-file "path:$flakeDir#x" 2> "$TEST_ROOT/path-input-1.err") = hello ]]
grepQuiet "copying .*path-input.* to the store" "$TEST_ROOT/path-input-1.err"

# The second evaluation of the unchanged tree names it, finds it valid,
# and copies nothing.
[[ $(nix eval --raw -vv --no-write-lock-file "path:$flakeDir#x" 2> "$TEST_ROOT/path-input-2.err") = hello ]]
grepQuietInverse "copying .* to the store" "$TEST_ROOT/path-input-2.err"

# An edited tree has another name and is copied.
echo changed > "$flakeDir/data"
[[ $(nix eval --raw -vv --no-write-lock-file "path:$flakeDir#x" 2> "$TEST_ROOT/path-input-3.err") = changed ]]
grepQuiet "copying .*path-input.* to the store" "$TEST_ROOT/path-input-3.err"
