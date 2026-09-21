#!/usr/bin/env bash

source common.sh

touch "$TEST_ROOT/foo" -t 202211111111
# We only check whether 2022-11-1* **:**:** is the last modified date since
# `lastModified` is transformed into UTC in `builtins.fetchTarball`.
[[ "$(nix eval --impure --raw --expr "(builtins.fetchTree \"path://$TEST_ROOT/foo\").lastModifiedDate")" =~ 2022111.* ]]

# Check that we can override lastModified for "path:" inputs.
[[ "$(nix eval --impure --expr "(builtins.fetchTree { type = \"path\"; path = \"$TEST_ROOT/foo\"; lastModified = 123; }).lastModified")" = 123 ]]

# Both the in-memory observation and its spill retain the complete tree,
# including executable files, empty directories and symlink targets.
for mib in 1 65; do
    input="$TEST_ROOT/prepared-$mib"
    mkdir -p "$input/empty" "$input/nested"
    for i in $(seq 1 16); do
        mkdir -p "$input/nested/$i"
        printf '%s\n' "$i" > "$input/nested/$i/value"
    done
    truncate -s "$((mib * 1024 * 1024))" "$input/nested/large"
    printf 'executable\n' > "$input/run"
    chmod +x "$input/run"
    ln -s nested/large "$input/link"
    expected=$(nix hash path --mode git "$input")
    output=$(nix eval --impure --raw --expr "(builtins.fetchTree \"path:$input\").outPath")
    [[ "$(nix hash path --mode git "$output")" = "$expected" ]]
    test -d "$output/empty"
    test -x "$output/run"
    [[ "$(readlink "$output/link")" = nested/large ]]
    printf changed > "$input/nested/large"
    changed=$(nix eval --impure --raw --expr "(builtins.fetchTree \"path:$input\").outPath")
    [[ "$changed" != "$output" ]]
    [[ "$(nix hash path --mode git "$output")" = "$expected" ]]
done
