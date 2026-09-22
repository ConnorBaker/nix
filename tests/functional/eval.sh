#!/usr/bin/env bash

source common.sh

testStdinHeredoc=$(nix eval -f - <<EOF
{
  bar = 3 + 1;
  foo = 2 + 2;
}
EOF
)
[[ $testStdinHeredoc == '{ bar = 4; foo = 4; }' ]]

nix eval --expr 'assert 1 + 2 == 3; true'

[[ $(nix eval int -f "./eval.nix") == 123 ]]
[[ $(nix eval str -f "./eval.nix") == '"foo\nbar"' ]]
[[ $(nix eval str --raw -f "./eval.nix") == $'foo\nbar' ]]
[[ "$(nix eval attr -f "./eval.nix")" == '{ foo = "bar"; }' ]]
[[ $(nix eval attr --json -f "./eval.nix") == '{"foo":"bar"}' ]]
[[ $(nix eval int -f - < "./eval.nix") == 123 ]]
[[ "$(nix eval --expr '{"assert"=1;bar=2;}')" == '{ "assert" = 1; bar = 2; }' ]]

# Check if toFile can be utilized during restricted eval
[[ $(nix eval --restrict-eval --expr 'import (builtins.toFile "source" "42")') == 42 ]]

nix-instantiate --eval -E 'assert 1 + 2 == 3; true'
[[ $(nix-instantiate -A int --eval "./eval.nix") == 123 ]]
[[ $(nix-instantiate -A str --eval "./eval.nix") == '"foo\nbar"' ]]
[[ $(nix-instantiate -A str --raw --eval "./eval.nix") == $'foo\nbar' ]]
[[ "$(nix-instantiate -A attr --eval "./eval.nix")" == '{ foo = "bar"; }' ]]
[[ $(nix-instantiate -A attr --eval --json "./eval.nix") == '{"foo":"bar"}' ]]
[[ $(nix-instantiate -A int --eval - < "./eval.nix") == 123 ]]
[[ "$(nix-instantiate --eval -E '{"assert"=1;bar=2;}')" == '{ "assert" = 1; bar = 2; }' ]]

# Check that symlink cycles don't cause a hang.
ln -sfn cycle.nix "$TEST_ROOT/cycle.nix"
(! nix eval --file "$TEST_ROOT/cycle.nix")

# Test that printing deep data structures produces a controlled error.
# The expression creates a non-cyclic but infinitely deep structure:
# f returns immediately with a thunk, so Nix call depth stays at 1,
# but Printer::print recurses on the C++ stack.
expectStderr 1 nix eval --expr 'let f = n: { inner = f (n + 1); }; in f 0' --max-call-depth 100 \
  | grepQuiet "stack overflow; max-call-depth exceeded"

# Same for builtins.toXML
expectStderr 1 nix eval --expr 'builtins.toXML (let f = n: { inner = f (n + 1); }; in f 0)' --max-call-depth 100 \
  | grepQuiet "stack overflow; max-call-depth exceeded"

# Same for equality comparison (n is not observable, so structures are equal)
expectStderr 1 nix eval --expr 'let f = n: { inner = f (n + 1); }; in f 0 == f 1' --max-call-depth 100 \
  | grepQuiet "stack overflow; max-call-depth exceeded"

# Same for assert with equality (uses assertEqValues)
expectStderr 1 nix eval --expr 'let f = n: { inner = f (n + 1); }; in assert f 0 == f 1; true' --max-call-depth 100 \
  | grepQuiet "stack overflow; max-call-depth exceeded"

# Same for string coercion with __toString
# shellcheck disable=SC2016
expectStderr 1 nix eval --expr 'let f = n: { __toString = _: f (n + 1); }; in "${f 0}"' --max-call-depth 100 \
  | grepQuiet "stack overflow; max-call-depth exceeded"

# --file and --pure-eval don't mix.
expectStderr 1 nix eval --pure-eval --file "$TEST_ROOT/cycle.nix" | grepQuiet "not compatible"

# Check that relative symlinks are resolved correctly.
mkdir -p "$TEST_ROOT/xyzzy" "$TEST_ROOT/foo"
ln -sfn ../xyzzy "$TEST_ROOT/foo/bar"
printf 123 > "$TEST_ROOT/xyzzy/default.nix"
[[ $(nix eval --impure --expr "import $TEST_ROOT/foo/bar") = 123 ]]

# Test --arg-from-file.
[[ "$(nix eval --raw --arg-from-file foo "${config_nix}" --expr '{ foo }: { inherit foo; }' foo)" = "$(cat "${config_nix}")" ]]

# Check that special(-ish) files are drained.
if [[ -e /proc/version ]]; then
    [[ "$(nix eval --raw --arg-from-file foo /proc/version --expr '{ foo }: { inherit foo; }' foo)" = "$(cat /proc/version)" ]]
fi

# Test --arg-from-stdin.
[[ "$(echo bla | nix eval --raw --arg-from-stdin foo --expr '{ foo }: { inherit foo; }' foo)" = bla ]]

# Test that unknown settings are warned about
out="$(expectStderr 0 nix eval --option foobar baz --expr '""' --raw)"
[[ "$(echo "$out" | grep -c foobar)" = 1 ]]

# Test flag alias
out="$(nix eval --expr '{}' --build-cores 1)"
[[ "$(echo "$out" | wc -l)" = 1 ]]

# Repeating a completed copy request preserves its result. Changing the
# request, or reading the source itself, must still observe changed bytes.
copyRoot="$TEST_ROOT/copy-requests"
mkdir -p "$copyRoot/original" "$copyRoot/other"
ln -s original "$copyRoot/alias"
ln -s original/input "$copyRoot/link"
cat > "$copyRoot/requests.nix" <<'EOF'
{ source, other, alias, shell, first, second, hash ? "", typeOnly ? false }:
let
  p = /. + source;
  requests = {
    interpolation = p: "${p}";
    json = p: builtins.fromJSON (builtins.unsafeDiscardStringContext (builtins.toJSON p));
    path = p: builtins.path { path = p; };
    before = p: builtins.path { path = p; name = "before"; };
    renamed = p: builtins.path { path = p; name = "after"; };
    flat = p: builtins.path { path = p; recursive = false; };
    filtered = p: builtins.path { path = p; filter = _: _: true; };
    other = _: "${/. + other}";
    ancestor = _: "${/. + alias}";
    raw = p: p;
    nar = p: builtins.path { path = p; sha256 = hash; };
    tree = p: builtins.path { path = p; treeHash = hash; };
    badNar = p: builtins.path { path = p; name = "rejected-${builtins.baseNameOf p}"; sha256 = hash; };
    badTree = p: builtins.path { path = p; name = "rejected-${builtins.baseNameOf p}"; treeHash = hash; };
  };
  firstCopy = requests.${first} p;
  changed = builtins.seq firstCopy (builtins.exec [
    shell "-c" ''printf BBB > "$1"; printf BBB > "$2"; printf null''
    "copy-request" source other
  ]);
  secondCopy = builtins.seq changed (requests.${second} p);
in { result = if typeOnly then builtins.readFileType secondCopy else builtins.readFile secondCopy; }
EOF
copyArgs=(--impure --raw --file "$copyRoot/requests.nix" result
    --option allow-unsafe-native-code-during-evaluation true
    --argstr shell "$SHELL" --argstr other "$copyRoot/other/input"
    --argstr alias "$copyRoot/alias/input")

# All nine entry-point pairs denote the same unfiltered default-name request.
for first in interpolation json path; do
    for second in interpolation json path; do
        printf AAA > "$copyRoot/original/input"
        [[ $(nix eval "${copyArgs[@]}" --argstr source "$copyRoot/original/input" \
            --argstr first "$first" --argstr second "$second") == AAA ]]
    done
done

# Distinct name, method, filter or source, and raw reads, observe the rewrite.
for second in renamed flat filtered other raw; do
    printf AAA > "$copyRoot/original/input"
    printf AAA > "$copyRoot/other/input"
    [[ $(nix eval "${copyArgs[@]}" --argstr source "$copyRoot/original/input" \
        --argstr first interpolation --argstr second "$second") == BBB ]]
done

# Preserve the public counterexample that rejected caching a source's bytes:
# a copy named "before" cannot determine a later copy named "after".
printf AAA > "$copyRoot/original/input"
[[ $(nix eval "${copyArgs[@]}" --argstr source "$copyRoot/original/input" \
    --argstr first before --argstr second renamed) == BBB ]]

# Ancestor aliases resolve to the same request. A final symlink is preserved
# by coercion but followed by builtins.path, so those requests differ.
printf AAA > "$copyRoot/original/input"
[[ $(nix eval "${copyArgs[@]}" --argstr source "$copyRoot/original/input" \
    --argstr first interpolation --argstr second ancestor) == AAA ]]
printf AAA > "$copyRoot/original/input"
[[ $(nix eval "${copyArgs[@]}" --argstr source "$copyRoot/link" \
    --argstr first interpolation --argstr second path --arg typeOnly true) == regular ]]

# Assertions describe the current source, rather than an earlier unasserted
# request. Give each assertion its own name so no valid destination can skip
# the read; the rejected addresses have never been copied either.
for mode in nar tree; do
    printf BBB > "$copyRoot/expected"
    hashMode=$mode
    if [[ $mode == tree ]]; then hashMode=git; fi
    goodHash=$(nix hash path --mode "$hashMode" --algo sha256 --format sri "$copyRoot/expected")
    printf CCC > "$copyRoot/expected"
    badHash=$(nix hash path --mode "$hashMode" --algo sha256 --format sri "$copyRoot/expected")
    assertInput=$(mktemp "$copyRoot/assert-$mode-XXXXXX")
    printf AAA > "$assertInput"
    [[ $(nix eval "${copyArgs[@]}" --argstr source "$assertInput" \
        --argstr first interpolation --argstr second "$mode" --argstr hash "$goodHash") == BBB ]]
    printf AAA > "$assertInput"
    if [[ $mode == nar ]]; then rejected=badNar; else rejected=badTree; fi
    expect 102 nix eval "${copyArgs[@]}" --argstr source "$assertInput" \
        --argstr first interpolation --argstr second "$rejected" --argstr hash "$badHash"
done
