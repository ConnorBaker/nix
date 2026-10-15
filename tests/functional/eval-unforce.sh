#!/usr/bin/env bash

# PROTOTYPE (unforce): output of traversals that unforce what they visit
# (`eval-unforce`) must match the output without unforcing.

source common.sh

# Unforcing never runs in a build with Boehm GC, whatever `eval-unforce` says:
# no job ends, so no statistics are printed.
if [[ $(NIX_UNFORCE_STATS=1 nix-instantiate --option eval-unforce true --option eval-unforce-batch-size 0 \
    --eval --strict -E '[ 1 ]' 2>&1 >/dev/null) != *'unforce: '* ]]; then
    skipTest "unforcing is disabled in this build"
fi

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
traceExpr='let s = builtins.trace "forced" { v = 1; }; in [ s.v s.v ]'
# Base's stderr is exactly the one trace line; the count is what is printed
# if it is not.
countTraces() {
    local err=$TEST_ROOT/trace.err
    nix-instantiate "$@" --eval --strict -E "$traceExpr" 2> "$err" >/dev/null
    printf 'trace: forced\n' | cmp -s - "$err" || fail "nix-instantiate --strict $* (trace) stderr: $(< "$err")"
    grep -c 'trace: forced' "$err" || true
}
[[ $(countTraces "${off[@]}") == 1 ]] || fail "trace printed more than once without unforcing"
[[ $(countTraces "${on[@]}") == 1 ]] || fail "trace printed more than once with the default batch size"

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

# Running out of memory while deciding what to keep at the end of a batch is
# fatal: a clear message, then abort(), before any value has changed.
rc=0
out=$(NIX_UNFORCE_FAIL_DECIDE=1 nix-instantiate "${batch0[@]}" --eval --strict \
    -E 'let s = { a = 1; }; in [ s.a s.a s.a ]' 2>&1) || rc=$?
(( rc == 134 )) || fail "expected abort() (status 134), got $rc: $out"
[[ $out == *"out of memory while deciding"*"--option eval-unforce false"* ]] \
    || fail "no fatal message: $out"

# The closure's step budget bounds the whole decision, not only its walks
# (review of markers v2, S1): here a chain t1..t600 is found one target per
# round (promoting t(i+1) puts t(i)'s list in the kept set), and every round
# also tests 300 000 targets forced to integers, whose walks pop nothing.
# Before rounds and targets were charged this took 600 rounds and about
# 90 times base's instructions without reaching the budget.
{
    echo 'let'
    echo '  t1 = { v = [ 1 2 3 ]; w = null; };'
    for ((i = 2; i <= 600; i++)); do echo "  t$i = { v = [ 1 2 3 ]; w = t$((i - 1)).v; };"; done
    echo '  zs = builtins.genList (builtins.add 0) 300000;'
    echo -n '  ts = [ '
    for ((i = 1; i <= 600; i++)); do echo -n "t$i "; done
    echo '];'
    echo 'in builtins.seq zs [ (builtins.seq t600 true) (builtins.deepSeq ts (builtins.deepSeq zs true)) ]'
} > "$TEST_ROOT/rounds.nix"
stats=$(NIX_UNFORCE_STATS=1 NIX_UNFORCE_PROMOTE=2 nix-instantiate "${batch0[@]}" --eval --strict "$TEST_ROOT/rounds.nix" 2>&1 >/dev/null)
[[ $stats =~ rounds\ ([0-9]+).*over\ budget\ ([0-9]+) ]] || fail "no unforce stats: $stats"
(( BASH_REMATCH[1] <= 20 )) || fail "the closure took ${BASH_REMATCH[1]} rounds in one job: $stats"
(( BASH_REMATCH[2] == 1 )) || fail "the closure did not run out of budget: $stats"
[[ $(nix-instantiate "${batch0[@]}" --eval --strict "$TEST_ROOT/rounds.nix") == '[ true true ]' ]] \
    || fail "rounds.nix: wrong output"

# Rebuilding the force counts when they outgrow their cap keeps this job's
# counts (review of markers v2, S2): each job here forces 3 000 fresh cells,
# more than the cap of 1 000, and P. Dropping the whole map after every job
# would keep P's count at 1, so P would never be promoted.
expr='let
  fib = n: if n < 2 then n else fib (n - 1) + fib (n - 2);
  P = fib 15;
  bigs = builtins.genList (k: builtins.genList (i: i) 3000) 10;
  el = k: builtins.seq P (builtins.foldl'"'"' (a: x: builtins.seq x a) true (builtins.elemAt bigs k));
in builtins.deepSeq (map builtins.length bigs) (builtins.genList el 10)'
expected=$(nix-instantiate "${off[@]}" --eval --strict -E "$expr")
stats=$(NIX_UNFORCE_STATS=1 NIX_UNFORCE_COUNTS_CAP=1000 nix-instantiate "${batch0[@]}" --eval --strict -E "$expr" 2>&1 >/dev/null)
[[ $stats =~ jobs\ [0-9]+,\ logged\ [0-9]+,\ reverted\ [0-9]+,\ stubbed\ [0-9]+,\ promoted\ ([0-9]+).*counts\ resets\ ([0-9]+) ]] \
    || fail "no unforce stats: $stats"
(( BASH_REMATCH[2] >= 5 )) || fail "the force counts never outgrew their cap: $stats"
(( BASH_REMATCH[1] >= 1 )) || fail "a cell forced in every job was never promoted: $stats"
[[ $(NIX_UNFORCE_COUNTS_CAP=1000 nix-instantiate "${batch0[@]}" --eval --strict -E "$expr") == "$expected" ]] \
    || fail "counts rebuilt: wrong output"

# A walk is also charged for the width of what it scans (review of markers
# v3, S1'): beside the chain of rounds.nix, one target holds a young
# Bindings of 1 000 000 attributes pointing to old cells, which every round
# scans. Before Bindings widths were charged, this took 600 rounds without
# reaching the budget.
{
    echo 'let'
    echo '  old = builtins.listToAttrs (builtins.genList (i: { name = "a${toString i}"; value = i; }) 1000000);'
    echo '  big = removeAttrs old [ "nonexistent" ];'
    echo '  t1 = { v = [ 1 2 3 ]; w = null; };'
    for ((i = 2; i <= 600; i++)); do echo "  t$i = { v = [ 1 2 3 ]; w = t$((i - 1)).v; };"; done
    echo -n '  ts = [ '
    for ((i = 1; i <= 600; i++)); do echo -n "t$i "; done
    echo '];'
    echo 'in builtins.seq old [ (builtins.seq t600 true) (builtins.seq big (builtins.deepSeq ts true)) ]'
} > "$TEST_ROOT/wide.nix"
stats=$(NIX_UNFORCE_STATS=1 NIX_UNFORCE_PROMOTE=2 nix-instantiate "${batch0[@]}" --eval --strict "$TEST_ROOT/wide.nix" 2>&1 >/dev/null)
[[ $stats =~ rounds\ ([0-9]+).*over\ budget\ ([0-9]+) ]] || fail "no unforce stats: $stats"
(( BASH_REMATCH[1] <= 200 )) || fail "the closure took ${BASH_REMATCH[1]} rounds in one job: $stats"
(( BASH_REMATCH[2] == 1 )) || fail "the closure did not run out of budget: $stats"
[[ $(nix-instantiate "${batch0[@]}" --eval --strict "$TEST_ROOT/wide.nix") == '[ true true ]' ]] \
    || fail "wide.nix: wrong output"

# Output equals base: the trace above is printed exactly once with a batch
# per element too. Fails until replays are silent (effects design R); last,
# so that it does not hide the checks above.
[[ $(countTraces "${batch0[@]}") == 1 ]] || fail "trace printed more than once with eval-unforce-batch-size = 0"
