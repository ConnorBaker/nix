#!/usr/bin/env bash

# Promotion must not split value identity. Each program
# in eval-unforce-identity/ compares a value kept by a promoted cell with the
# same value reached through a cell that a job reverted (see the owner
# timeline in src/libexpr/unforce.cc). With a job per element, the output
# must match the output without unforcing.

source common.sh

# Unforcing never runs in a build with Boehm GC, whatever `eval-unforce` says.
# Such a build reports its collector's heap in NIX_SHOW_STATS.
if [[ $(NIX_SHOW_STATS=1 NIX_SHOW_STATS_PATH=/dev/stdout nix eval --expr 1 2>/dev/null) == *'"heapSize"'* ]]; then
    skipTest "unforcing is disabled in a build with Boehm GC"
fi

cd eval-unforce-identity

run() {
    local f=$1
    shift
    # The exit status is part of the output.
    if [[ $f == nested.nix ]]; then
        nix eval "$@" --impure --file "$f" 2>&1 || echo "exit status $?"
    else
        nix-instantiate "$@" --eval --strict "$f" 2>&1 || echo "exit status $?"
    fi
}

# The shipping default (owner timeline, promote after 2 forces) must keep
# identity on every program, with a job per element.
for f in *.nix; do
    [[ $f == set.nix ]] && continue
    expected=$(run "$f" --option eval-unforce false)
    out=$(run "$f" --option eval-unforce true --option eval-unforce-batch-size 0)
    [[ $out == "$expected" ]] \
        || fail "$f: got '$out', expected '$expected'"
done

# Generated schedules: each makes a count-only policy
# diverge at exactly one K. K-1 jobs force R (and P), one job forces P only,
# so P is promoted with P.a copied unforced, then one job forces R alone,
# which reaches K and is promoted holding a copy of P.a's value; P.a is
# reverted and rebuilt by the comparison. The owner timeline keeps identity
# on all of these at the default.
schedule() {
    local k=$1 warm="" i
    for ((i = 1; i < k; i++)); do warm+=' (builtins.seq R true)'; done
    echo "let P = { a = { f = x: x; }; }; R = P.a; in [$warm (P ? a) (builtins.seq R true) (R == P.a) ]"
}
for k in 2 3 4 5; do
    expr=$(schedule "$k")
    expected=$(nix-instantiate --option eval-unforce false --eval --strict -E "$expr")
    out=$(nix-instantiate \
        --option eval-unforce true --option eval-unforce-batch-size 0 --eval --strict -E "$expr")
    [[ $out == "$expected" ]] \
        || fail "schedule for K=$k: got '$out', expected '$expected'"
done

# A young cell that a target owns, forced by the driver (here the JSON
# printer, forcing the elements of a list the job's target built), runs under
# that target as owner: every such force changes the owner and back. The
# segments of a force that allocates nothing are overwritten and those of
# the driver merged, so the timeline stays short however many there are.
ownerStat() { # FIELD EXPR
    NIX_SHOW_STATS=1 NIX_SHOW_STATS_PATH=/dev/stdout nix-instantiate --option eval-unforce true \
        --option eval-unforce-batch-size 0 --eval --strict --json -E "$2" 2>/dev/null \
        | sed -n 's/.*"'"$1"'": *\([0-9]*\).*/\1/p' | head -n 1
}
expr='let L = builtins.genList (i: { v = i * 2; }) 2000; in [ L ]'
changes=$(ownerStat ownerChanges "$expr")
segs=$(ownerStat ownerSegsPeak "$expr")
[[ -n $changes && -n $segs ]] || fail "no owner statistics"
(( changes >= 2000 )) || fail "driver forces of a target's cells changed the owner $changes times, not >= 2000"
(( segs <= 16 )) || fail "the owner timeline grew to $segs segments for $changes owner changes"

# A job with a long timeline (160 001 segments, 1.2 MiB, within the 4 MiB
# budget of batch size 0): the driver alternates between young cells of two
# targets, and every force allocates. The output is base's.
expr='let
  n = 40000;
  a = builtins.listToAttrs [ { name = "x"; value = builtins.genList (i: { v = [ i ]; }) n; } ];
  b = builtins.listToAttrs [ { name = "x"; value = builtins.genList (i: { v = [ (i + 1) ]; }) n; } ];
in [ a b (builtins.genList (i: builtins.head (builtins.elemAt a.x i).v + builtins.head (builtins.elemAt b.x i).v) n) a.x b.x ]'
segs=$(ownerStat ownerSegsPeak "$expr")
(( segs > 65536 )) || fail "the alternating job made only $segs owner segments"
nix-instantiate --option eval-unforce true --option eval-unforce-batch-size 0 --eval --strict --json -E "$expr" \
    > "$TEST_ROOT/alternate-on.json"
nix-instantiate --option eval-unforce false --eval --strict --json -E "$expr" > "$TEST_ROOT/alternate-off.json"
cmp -s "$TEST_ROOT/alternate-on.json" "$TEST_ROOT/alternate-off.json" || fail "the alternating job's output differs from base"

# Consumer pairs are recorded once each: a target that alternates between
# young cells of two others, 100 000 times, records two pairs.
expr='let
  n = 100000;
  xs = builtins.genList (i: i) n;
  a = { x = map (builtins.mul 2) xs; };
  b = { x = map (builtins.mul 3) xs; };
  c = builtins.foldl'"'"' (acc: i: acc + builtins.elemAt a.x i + builtins.elemAt b.x i) 0 xs;
in [ c ]'
pairs=$(ownerStat ownerConsumerPairsPeak "$expr")
[[ -n $pairs ]] && (( pairs <= 4 )) || fail "an alternating consumer recorded ${pairs:-no} consumer pairs"

# The two "keep the whole job" fallbacks are exercised and counted, each still
# output-identical to base. A promoted job whose young closure to copy exceeds
# the copy limit (a quarter of the batch, at least 4 MiB) is kept whole
# instead of copied (ownerCopyLimitJobs); a job whose owner timeline exceeds
# the same budget stops tracking owners and is kept whole (ownerOverflowJobs).
cmpBase() { # EXPR
    nix-instantiate --option eval-unforce true --option eval-unforce-batch-size 0 \
        --eval --strict --json -E "$1" > "$TEST_ROOT/keep-on.json" 2>/dev/null
    nix-instantiate --option eval-unforce false --eval --strict --json -E "$1" > "$TEST_ROOT/keep-off.json" 2>/dev/null
    cmp -s "$TEST_ROOT/keep-on.json" "$TEST_ROOT/keep-off.json"
}
expr='let big = builtins.genList (i: { v = i; }) 500000; in [ big big ]'
copyKept=$(ownerStat ownerCopyLimitJobs "$expr")
[[ -n $copyKept ]] && (( copyKept >= 1 )) || fail "a promoted job over the copy limit was not kept whole (ownerCopyLimitJobs=${copyKept:-none})"
cmpBase "$expr" || fail "the copy-limit kept-whole job's output differs from base"

expr='let
  n = 160000;
  a = builtins.listToAttrs [ { name = "x"; value = builtins.genList (i: { v = [ i ]; }) n; } ];
  b = builtins.listToAttrs [ { name = "x"; value = builtins.genList (i: { v = [ (i + 1) ]; }) n; } ];
in [ a b (builtins.genList (i: builtins.head (builtins.elemAt a.x i).v + builtins.head (builtins.elemAt b.x i).v) n) a.x b.x ]'
overflowKept=$(ownerStat ownerOverflowJobs "$expr")
[[ -n $overflowKept ]] && (( overflowKept >= 1 )) || fail "a job over the owner budget did not overflow and keep whole (ownerOverflowJobs=${overflowKept:-none})"
cmpBase "$expr" || fail "the owner-overflow kept-whole job's output differs from base"
