#!/usr/bin/env bash

# Output of traversals that unforce what they visit
# (`eval-unforce`) must match the output without unforcing.

source common.sh

# Unforcing never runs in a build with Boehm GC, whatever `eval-unforce` says.
# Such a build reports its collector's heap in NIX_SHOW_STATS.
if [[ $(NIX_SHOW_STATS=1 NIX_SHOW_STATS_PATH=/dev/stdout nix eval --expr 1 2>/dev/null) == *'"heapSize"'* ]]; then
    skipTest "unforcing is disabled in a build with Boehm GC"
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

# Unforcing is active exactly when enabled, in each traversal compared above
# (ported from unforce-master b0135129cc / fd1364a8cf: "check unforcing in
# every traversal"). The evaluator's statistics report unforce scopes (one
# per batch) only once a batch has ended, so without this check the
# comparisons above would also pass with unforcing silently off, since each
# is against the same traversal with unforcing off. `unforceStats TRAVERSAL
# OPTION...` evaluates statsExpr with the options and TRAVERSAL (strict,
# json, xml, eval or eval-json), which must succeed with the expected
# output, and sets `scopes` and `reverted` (0 when there is no `unforce`
# entry in the NIX_SHOW_STATS JSON).
#
# The "unforcing ran" signal here is `scopes` (a batch boundary reached under
# unforcing), not master's `reverted`: with the owner timeline a print job
# that fits one batch is kept whole (`decideJob`), so an on run can report
# `scopes >= 1` with `reverted == 0` (observed for `--strict` and for a batch
# per element). Only off runs must have no scope and nothing reverted.
statsExpr='[ (1 + 1) (2 + 2) ]'
statsXml="<?xml version='1.0' encoding='utf-8'?>
<expr>
  <list>
    <int value=\"2\" />
    <int value=\"4\" />
  </list>
</expr>"
unforceStats() {
    local traversal=$1 stats=$TEST_ROOT/show-stats.json out=$TEST_ROOT/show-stats.out rc=0
    local cmd expected
    shift
    case $traversal in
        strict) cmd=(nix-instantiate "$@" --eval --strict) expected='[ 2 4 ]' ;;
        json) cmd=(nix-instantiate "$@" --eval --strict --json) expected='[2,4]' ;;
        xml) cmd=(nix-instantiate "$@" --eval --strict --xml) expected=$statsXml ;;
        eval) cmd=(nix eval "$@") expected='[ 2 4 ]' ;;
        eval-json) cmd=(nix eval "$@" --json) expected='[2,4]' ;;
        *) fail "unforceStats: unknown traversal: $traversal" ;;
    esac
    cmd+=(--expr "$statsExpr")
    rm -f "$stats"
    NIX_SHOW_STATS=1 NIX_SHOW_STATS_PATH="$stats" "${cmd[@]}" > "$out" || rc=$?
    [[ $rc == 0 ]] || fail "${cmd[*]} (statistics) exited with $rc"
    printf '%s\n' "$expected" | cmp -s - "$out" || fail "${cmd[*]} (statistics) printed: $(< "$out")"
    [[ -s $stats ]] || fail "${cmd[*]} wrote no statistics to $stats (NIX_SHOW_STATS_PATH)"
    scopes=$(jq '.unforce.scopes // 0' "$stats")
    reverted=$(jq '.unforce.reverted // 0' "$stats")
}
for traversal in strict json xml eval eval-json; do
    unforceStats "$traversal" "${off[@]}"
    [[ $scopes == 0 && $reverted == 0 ]] \
        || fail "$traversal, eval-unforce = false: unforcing ran ($scopes scopes, $reverted reverted)"
    unforceStats "$traversal" "${on[@]}"
    [[ $scopes -ge 1 ]] \
        || fail "$traversal, eval-unforce = true: unforcing did not run ($scopes scopes, $reverted reverted)"
done
# A batch per element: one scope per element.
unforceStats strict "${batch0[@]}"
[[ $scopes -ge 2 ]] \
    || fail "eval-unforce-batch-size = 0: no batch per element ($scopes scopes, $reverted reverted)"
# Without options, unforcing follows the effective setting, which a NIX_CONFIG
# in the environment may change.
default=$(nix config show eval-unforce)
unforceStats strict
case $default in
    true) [[ $scopes -ge 1 ]] || fail "eval-unforce is true by default but unforcing did not run" ;;
    false) [[ $scopes == 0 ]] || fail "eval-unforce is false by default but unforcing ran" ;;
    *) fail "unexpected eval-unforce value: $default" ;;
esac

# A trace in a value shared between elements is printed once.
traceExpr='let s = builtins.trace "forced" { v = 1; }; in [ s.v s.v ]'
# Base's stderr is exactly the one trace line; the count is what is printed
# if it is not.
countTraces() {
    local err=$TEST_ROOT/trace.err out=$TEST_ROOT/trace.out rc=0
    # Also require exit 0 and the full output (ported from unforce-master
    # fd1364a8cf: a crash after the trace was printed must not pass).
    nix-instantiate "$@" --eval --strict -E "$traceExpr" > "$out" 2> "$err" || rc=$?
    [[ $rc == 0 ]] || fail "nix-instantiate --strict $* (trace) exited with $rc: $(< "$err")"
    printf '[ 1 1 ]\n' | cmp -s - "$out" || fail "nix-instantiate --strict $* (trace) stdout: $(< "$out")"
    printf 'trace: forced\n' | cmp -s - "$err" || fail "nix-instantiate --strict $* (trace) stderr: $(< "$err")"
    grep -c 'trace: forced' "$err" || true
}
[[ $(countTraces "${off[@]}") == 1 ]] || fail "trace printed more than once without unforcing"
[[ $(countTraces "${on[@]}") == 1 ]] || fail "trace printed more than once with the default batch size"

# Nothing is printed to stdout when forcing fails part-way, as without
# unforcing (which forces deeply before printing). The failure must be the
# evaluation error (exit code 1), not a crash (ported from unforce-master
# fd1364a8cf, which also runs it with unforcing off).
for opts in off on batch0; do
    declare -n o=$opts
    out=$TEST_ROOT/partial.out err=$TEST_ROOT/partial.err rc=0
    nix-instantiate "${o[@]}" --eval --strict -E 'builtins.genList (x: x + "foo") 2' > "$out" 2> "$err" || rc=$?
    [[ $rc == 1 ]] || fail "nix-instantiate --strict ($opts) exited with $rc, not 1: $(< "$err")"
    grepQuiet 'cannot add a string to an integer' "$err" \
        || fail "nix-instantiate --strict ($opts) did not report the evaluation error: $(< "$err")"
    [[ ! -s $out ]] || fail "nix-instantiate --strict ($opts) printed partial output: $(< "$out")"
done

# Running out of memory while deciding what to keep at the end of a batch is
# fatal: a clear message, then abort(), before any value has changed.
rc=0
out=$(NIX_UNFORCE_DEBUG=fail-decide nix-instantiate "${batch0[@]}" --eval --strict \
    -E 'let s = { a = 1; }; in [ s.a s.a s.a ]' 2>&1) || rc=$?
(( rc == 134 )) || fail "expected abort() (status 134), got $rc: $out"
[[ $out == *"out of memory while deciding"*"--option eval-unforce false"* ]] \
    || fail "no fatal message: $out"

# The owner closure keeps identity on deep producer chains with no walk,
# however deep: here a chain t1..t600 (promoting t(i+1) puts t(i)'s list in
# the kept set), beside 300 000 targets forced to integers. The owner
# timeline decides this in one pass per copied object; the output equals
# base.
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
[[ $(nix-instantiate "${batch0[@]}" --eval --strict "$TEST_ROOT/rounds.nix") == '[ true true ]' ]] \
    || fail "rounds.nix: wrong output"

# The same, beside one target holding a young Bindings of 1 000 000
# attributes pointing to old cells.
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
[[ $(nix-instantiate "${batch0[@]}" --eval --strict "$TEST_ROOT/wide.nix") == '[ true true ]' ]] \
    || fail "wide.nix: wrong output"

# The same job as wide.nix, plus a 32 MiB string built by doubling (64 MiB
# of string data in all): string data is never walked for identity.
sed 's/^in builtins.seq old \[ (builtins.seq t600 true) (builtins.seq big /  dbl = s: n: if n == 0 then s else dbl (s + s) (n - 1);\n  pad = builtins.stringLength (dbl "x" 25);\n&(builtins.seq pad /; s/true)) ]$/true))) ]/' \
    "$TEST_ROOT/wide.nix" > "$TEST_ROOT/wide-pad.nix"
grep -q 'seq pad' "$TEST_ROOT/wide-pad.nix" || fail "wide-pad.nix: no padding"
[[ $(nix-instantiate "${batch0[@]}" --eval --strict "$TEST_ROOT/wide-pad.nix") == '[ true true ]' ]] \
    || fail "wide-pad.nix: wrong output"

# Output equals base: the trace above is printed exactly once with a batch
# per element too. Effects design R makes replayed diagnostics silent: when a
# reverted cell is re-forced in a later batch, a `builtins.trace`/`warn` line
# identical to one already emitted this evaluation is suppressed, so unforcing
# never prints a diagnostic base does not. (This was an xfail until that
# landed.) Kept last so it does not hide the checks above.
[[ $(countTraces "${batch0[@]}") == 1 ]] || fail "trace printed more than once with eval-unforce-batch-size = 0"
