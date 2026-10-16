#!/usr/bin/env bash

source common.sh

nix eval --expr 'assert 1 + 2 == 3; true'

[[ $(nix eval --impure --expr 'builtins.readFile ./pure-eval.sh') =~ clearStore ]]

missingImpureErrorMsg=$(expectStderr 1 nix eval --expr 'builtins.readFile ./pure-eval.sh')

# shellcheck disable=SC1111
echo "$missingImpureErrorMsg" | grepQuiet -- --impure || \
    fail "The error message should mention the “--impure” flag to unblock users"

[[ $(nix eval --expr 'builtins.pathExists ./pure-eval.sh') == false ]] || \
    fail "Calling 'pathExists' on a non-authorised path should return false"

# A forbidden access while evaluating the argument is an error, not a missing path.
for arg in \
    '(builtins.readFile ./pure-eval.sh)' \
    '(builtins.seq (builtins.readFile ./pure-eval.sh) 1)' \
    '{ outPath = builtins.readFile ./pure-eval.sh; }' \
    '{ __toString = _: builtins.readFile ./pure-eval.sh; }'; do
    expectStderr 1 nix eval --expr "builtins.pathExists $arg" | grepQuiet "is forbidden in pure evaluation mode"
done

expect 1 nix eval --expr builtins.currentTime
expect 1 nix eval --expr builtins.currentSystem

expect 1 nix-instantiate --pure-eval ./simple.nix

[[ $(nix eval --impure --expr "(import (builtins.fetchurl { url = \"file://$(pwd)/pure-eval.nix\"; })).x") == 123 ]]
expect 1 nix eval --expr "(import (builtins.fetchurl { url = \"file://$(pwd)/pure-eval.nix\"; })).x"
nix eval --expr "(import (builtins.fetchurl { url = \"file://$(pwd)/pure-eval.nix\"; sha256 = \"$(nix hash file pure-eval.nix --type sha256)\"; })).x"

# shellcheck disable=SC2088
expect 1 nix eval --expr '~/foo'

expectStderr 0 nix eval --expr "/some/absolute/path" \
  | grepQuiet "/some/absolute/path"

expectStderr 0 nix eval --expr "/some/absolute/path" --impure \
  | grepQuiet "/some/absolute/path"

expectStderr 0 nix eval --expr "some/relative/path" \
  | grepQuiet "$PWD/some/relative/path"

expectStderr 0 nix eval --expr "some/relative/path" --impure \
  | grepQuiet "$PWD/some/relative/path"

# A URL in a lookup path passed to `builtins.findFile` can't be locked, so it
# must not be downloaded in pure mode.
expectStderr 1 nix eval --expr 'builtins.findFile [ { prefix = "foo"; path = "https://example.invalid/foo.tar.gz"; } ] "foo"' \
  | grepQuiet "cannot download Nix search path entry 'https://example.invalid/foo.tar.gz' in pure evaluation mode"

# The same goes for unlocked `flake:` entries.
expectStderr 1 nix eval --expr "builtins.findFile [ { prefix = \"foo\"; path = \"flake:path:$TEST_ROOT/no-such-path\"; } ] \"foo\"" \
  | grepQuiet "cannot fetch unlocked Nix search path entry 'flake:path:$TEST_ROOT/no-such-path' in pure evaluation mode"

# The NAR hash of a `flake:` entry is verified.
mkdir -p "$TEST_ROOT/find-file-flake/a" "$TEST_ROOT/find-file-flake/b"
echo 456 > "$TEST_ROOT/find-file-flake/a/default.nix"
echo 789 > "$TEST_ROOT/find-file-flake/b/default.nix"
expectStderr 102 nix eval --expr "import (builtins.findFile [ { prefix = \"foo\"; path = \"flake:path:$TEST_ROOT/find-file-flake/a?narHash=sha256-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=\"; } ] \"foo\")" \
  | grepQuiet "NAR hash mismatch in Nix search path entry"

# A locked `flake:` entry can be used in pure mode, without the flake registries.
flakeRef="path:$TEST_ROOT/find-file-flake/a?narHash=$(nix hash path "$TEST_ROOT/find-file-flake/a")"
nix registry add "$flakeRef" "path:$TEST_ROOT/find-file-flake/b"
[[ $(nix eval --expr "import (builtins.findFile [ { prefix = \"foo\"; path = \"flake:$flakeRef\"; } ] \"foo\")") == 456 ]]
