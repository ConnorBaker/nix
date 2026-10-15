#!/usr/bin/env bash

source common.sh

(( $(nix search -f search.nix '' hello | wc -l) > 0 ))

# Check descriptions are searched
(( $(nix search -f search.nix '' broken | wc -l) > 0 ))

# Check search that matches nothing
(( $(nix search -f search.nix '' nosuchpackageexists | wc -l) == 0 ))

# Search for multiple arguments
(( $(nix search -f search.nix '' hello empty | wc -l) == 2 ))

# Multiple arguments will not exist
(( $(nix search -f search.nix '' hello broken | wc -l) == 0 ))

# No regex should return an error
(( $(nix search -f search.nix '' | wc -l) == 0 ))

## Search expressions

# Check that empty search string matches all
nix search -f search.nix '' ^ | grepQuiet foo
nix search -f search.nix '' ^ | grepQuiet bar
nix search -f search.nix '' ^ | grepQuiet hello

## Tests for multiple regex/match highlighting

e=$'\x1b' # grep doesn't support \e, \033 or even \x1b
# Multiple overlapping regexes
(( $(nix search -f search.nix '' 'oo' 'foo' 'oo' | grep -c "$e\[32;1mfoo$e\\[0;1m") == 1 ))
(( $(nix search -f search.nix '' 'broken b' 'en bar' | grep -c "$e\[32;1mbroken bar$e\\[0m") == 1 ))

# Multiple matches
# Searching for 'o' should yield the 'o' in 'broken bar', the 'oo' in foo and 'o' in hello
(( $(nix search -f search.nix '' 'o' | grep -Eoc "$e\[32;1mo{1,2}$e\[(0|0;1)m") == 3 ))
# Searching for 'b' should yield the 'b' in bar and the two 'b's in 'broken bar'
# NOTE: This does not work with `grep -c` because it counts the two 'b's in 'broken bar' as one matched line
(( $(nix search -f search.nix '' 'b' | grep -Eo "$e\[32;1mb$e\[(0|0;1)m" | wc -l) == 3 ))

## Tests for --exclude
(( $(nix search -f search.nix ^ -e hello | grep -c hello) == 0 ))

(( $(nix search -f search.nix foo ^ --exclude 'foo|bar' | grep -Ec 'foo|bar') == 0 ))
(( $(nix search -f search.nix foo ^ -e foo --exclude bar | grep -Ec 'foo|bar') == 0 ))
[[ $(nix search -f search.nix '' ^ -e bar --json | jq -c 'keys') == '["foo","hello"]' ]]

## Evaluation errors under legacyPackages are skipped, however the package set is selected

cat > "$TEST_ROOT/legacy.nix" <<'EOF'
let
  pkg = name: {
    type = "derivation";
    inherit name;
  };
in
{
  legacyPackages.sys = {
    broken = throw "this package fails to evaluate";
    foo = pkg "foo-1";
    bar = pkg "bar-2";
  };
  other = {
    broken = throw "this package fails to evaluate";
    foo = pkg "foo-1";
  };
}
EOF

[[ $(nix search -f "$TEST_ROOT/legacy.nix" '' ^ --json | jq -c 'keys') == '["legacyPackages.sys.bar","legacyPackages.sys.foo"]' ]]
# Attribute paths are shown relative to the selected value.
[[ $(nix search -f "$TEST_ROOT/legacy.nix" legacyPackages.sys ^ --json | jq -c 'keys') == '["bar","foo"]' ]]
[[ $(nix search --impure --expr "import $TEST_ROOT/legacy.nix" legacyPackages.sys ^ --json | jq -c 'keys') == '["bar","foo"]' ]]

# Errors outside legacyPackages are not skipped.
expectStderr 1 nix search -f "$TEST_ROOT/legacy.nix" other ^ --json | grepQuiet "this package fails to evaluate"

# Errors in selecting the value to search are not skipped either.
expectStderr 1 nix search -f "$TEST_ROOT/legacy.nix" legacyPackages.sys.broken ^ --json | grepQuiet "this package fails to evaluate"
echo '{ pkgs }: { legacyPackages.sys = pkgs; }' > "$TEST_ROOT/legacy-args.nix"
expectStderr 1 nix search -f "$TEST_ROOT/legacy-args.nix" legacyPackages.sys ^ --json | grepQuiet "argument without a value"
[[ $(nix search -f "$TEST_ROOT/legacy-args.nix" --arg pkgs "(import $TEST_ROOT/legacy.nix).legacyPackages.sys" legacyPackages.sys ^ --json | jq -c 'keys') == '["bar","foo"]' ]]
