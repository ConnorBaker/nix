#!/usr/bin/env bash

# PROTOTYPE (unforce): output of traversals that unforce what they visit
# (`eval-unforce`) must match the output without unforcing.

source common.sh

off=(--option eval-unforce false)
# The default batch size, given explicitly so that a NIX_CONFIG in the
# environment does not change it.
on=(--option eval-unforce true --option eval-unforce-batch-size 67108864)
batch0=(--option eval-unforce true --option eval-unforce-batch-size 0)

# A value referring back to the top level prints as «repeated», as without
# unforcing: `nix-instantiate --strict` must mark the top level as seen.
expr='let x = { a = x; }; in x'
expected='{ a = «repeated»; }'
for opts in off on batch0; do
    declare -n o=$opts
    [[ $(nix-instantiate "${o[@]}" --eval --strict -E "$expr") == "$expected" ]] \
        || fail "nix-instantiate --strict ($opts): self-reference not printed as «repeated»"
    [[ $(nix eval "${o[@]}" --expr "$expr") == "$expected" ]] \
        || fail "nix eval ($opts): self-reference not printed as «repeated»"
done

expr='let x = [ x ]; in x'
expected=$(nix-instantiate "${off[@]}" --eval --strict -E "$expr")
for opts in on batch0; do
    declare -n o=$opts
    [[ $(nix-instantiate "${o[@]}" --eval --strict -E "$expr") == "$expected" ]] \
        || fail "nix-instantiate --strict ($opts): list self-reference differs"
done

# Serialisations agree with and without unforcing.
expr='let s = { v = 1; w = [ 1 2 { z = "a"; } ]; }; in { a = s; b = [ s s ]; c = { d = s.w; }; }'
for fmt in --json --xml; do
    expected=$(nix-instantiate "${off[@]}" --eval --strict $fmt -E "$expr")
    for opts in on batch0; do
        declare -n o=$opts
        [[ $(nix-instantiate "${o[@]}" --eval --strict $fmt -E "$expr") == "$expected" ]] \
            || fail "nix-instantiate --strict $fmt ($opts) differs"
    done
done
expected=$(nix eval "${off[@]}" --json --expr "$expr")
for opts in on batch0; do
    declare -n o=$opts
    [[ $(nix eval "${o[@]}" --json --expr "$expr") == "$expected" ]] || fail "nix eval --json ($opts) differs"
done

# A trace in a value shared between elements is printed once.
expr='let s = builtins.trace "forced" { v = 1; }; in [ s.v s.v ]'
countTraces() {
    nix-instantiate "$@" --eval --strict -E "$expr" 2>&1 >/dev/null | grep -c 'trace: forced' || true
}
[[ $(countTraces "${off[@]}") == 1 ]] || fail "trace printed more than once without unforcing"
[[ $(countTraces "${on[@]}") == 1 ]] || fail "trace printed more than once with the default batch size"
# KNOWN BUG (see notes/port/PORT.md): with a batch per element, the end of
# the first element's batch unforces `s`, which the second element forces
# again, so the trace is printed twice. Flip this to `== 1` once fixed.
[[ $(countTraces "${batch0[@]}") == 2 ]] \
    || fail "trace count with eval-unforce-batch-size = 0 changed: update this test (known bug fixed?)"

# Nothing is printed to stdout when forcing fails part-way, as without
# unforcing (which forces deeply before printing).
for opts in on batch0; do
    declare -n o=$opts
    out=$(nix-instantiate "${o[@]}" --eval --strict -E 'builtins.genList (x: x + "foo") 2' 2>/dev/null) && fail "should fail"
    [[ -z $out ]] || fail "nix-instantiate --strict ($opts) printed partial output: $out"
done

# Promotion copies a list's element array once, however many promoted cells
# hold that list: here 200 cells share one 50 000-element array, which is
# 0.4 MB copied once and 80 MB copied per cell.
expr='let
  S = { big = builtins.genList (i: i) 50000; };
  ts = builtins.genList (i: { y = S.big; }) 200;
in [ (builtins.seq ts true) (builtins.foldl'"'"' (a: t: builtins.seq (builtins.length t.y) a) true ts) ]'
stats=$(NIX_UNFORCE_STATS=1 NIX_UNFORCE_PROMOTE=1 nix-instantiate "${batch0[@]}" --eval --strict -E "$expr" 2>&1 >/dev/null)
[[ $stats =~ promoted\ ([0-9]+).*copied\ ([0-9]+)\ MiB ]] || fail "no unforce stats: $stats"
(( BASH_REMATCH[1] >= 200 )) || fail "expected at least 200 promotions: $stats"
(( BASH_REMATCH[2] < 16 )) || fail "a shared list array was copied per cell: $stats"
