#!/usr/bin/env bash

source common.sh

# Tests for absolute path literal linting. Use TEST_ROOT so the literal names
# an existing, writable path without relying on host-specific roots.

clearStoreIfPossible

absPathLiteral="$TEST_ROOT"

# Test: By default, absolute path literals are allowed
nix-instantiate --eval --strict -E "$absPathLiteral" > "$TEST_ROOT/default.out"
[[ "$(cat "$TEST_ROOT/default.out")" == "$absPathLiteral" ]]

# Test: warn mode warns but still evaluates
nix-instantiate --lint-absolute-path-literals warn --eval --strict -E "$absPathLiteral" \
    > "$TEST_ROOT/warn.out" \
    2> "$TEST_ROOT/warn.err"
[[ "$(cat "$TEST_ROOT/warn.out")" == "$absPathLiteral" ]]
grepQuiet "absolute path literals are not portable" "$TEST_ROOT/warn.err"
grepQuiet -F "path literal '$absPathLiteral'" "$TEST_ROOT/warn.err"

# Test: fatal mode rejects absolute path literals
expectStderr 1 nix-instantiate --lint-absolute-path-literals fatal --eval --strict -E "$absPathLiteral" \
    > "$TEST_ROOT/fatal.err"
grepQuiet "absolute path literals are not portable" "$TEST_ROOT/fatal.err"
grepQuiet -F "path literal '$absPathLiteral'" "$TEST_ROOT/fatal.err"

# Test: Setting via NIX_CONFIG
NIX_CONFIG='lint-absolute-path-literals = warn' nix eval --expr "$absPathLiteral" 2>"$TEST_ROOT"/stderr
grepQuiet "absolute path literals are not portable" "$TEST_ROOT/stderr"

# Test: Command line overrides config
NIX_CONFIG='lint-absolute-path-literals = warn' nix eval --lint-absolute-path-literals ignore --expr "$absPathLiteral" 2>"$TEST_ROOT"/stderr
grepQuietInverse "absolute path literal" "$TEST_ROOT/stderr"

echo "absolute-path-literals test passed!"
