#!/usr/bin/env bash

# PROTOTYPE (unforce): promotion must not split value identity. Each program
# in eval-unforce-identity/ compares a value kept by a promoted cell with the
# same value reached through a cell that a job reverted (see the force
# markers in src/libexpr/unforce.cc). With a job per element, under every
# promotion threshold and backend, the output must match the output without
# unforcing.

source common.sh

# Unforcing never runs in a build with Boehm GC, whatever `eval-unforce` says.
# Such a build reports its collector's heap in NIX_SHOW_STATS.
if [[ $(NIX_SHOW_STATS=1 NIX_SHOW_STATS_PATH=/dev/stdout nix eval --expr 1 2>/dev/null) == *'"heapSize"'* ]]; then
    skipTest "unforcing is disabled in a build with Boehm GC"
fi

cd eval-unforce-identity

backends=(bump)
allocatorStats=$(NIX_SHOW_STATS=1 NIX_SHOW_STATS_PATH=/dev/stdout nix eval --expr 1 2>/dev/null || true)
if [[ $allocatorStats == *'"mimallocVersion"'* ]]; then
    backends+=(mimalloc)
fi

run() {
    local f=$1
    shift
    # nested.nix is printed by `nix eval`, whose nested containers are
    # scopes of their own with the mimalloc backend.
    # The exit status is part of the output.
    if [[ $f == nested.nix ]]; then
        nix eval "$@" --impure --file "$f" 2>&1 || echo "exit status $?"
    else
        nix-instantiate "$@" --eval --strict "$f" 2>&1 || echo "exit status $?"
    fi
}

for f in *.nix; do
    [[ $f == set.nix ]] && continue
    expected=$(run "$f" --option eval-unforce false)
    for backend in "${backends[@]}"; do
        for k in 0 1 2 3 4 5; do
            out=$(NIX_UNFORCE_BACKEND=$backend NIX_UNFORCE_PROMOTE=$k \
                run "$f" --option eval-unforce true --option eval-unforce-batch-size 0)
            [[ $out == "$expected" ]] \
                || fail "$f (backend $backend, promotion after $k forces): got '$out', expected '$expected'"
        done
    done
done

# A job whose producer closure runs out of its step budget falls back to
# promoting, or reverting, every target it forced. Both keep identity: with
# a budget of 0 steps every closure falls back.
for f in *.nix; do
    [[ $f == set.nix || $f == nested.nix ]] && continue
    expected=$(run "$f" --option eval-unforce false)
    for fallback in promote revert; do
        for k in 2 3; do
            out=$(NIX_UNFORCE_IDENTITY=markers NIX_UNFORCE_WALK_BUDGET_BASE=0 NIX_UNFORCE_WALK_BUDGET_PER_KIB=0 \
                NIX_UNFORCE_WALK_FALLBACK=$fallback NIX_UNFORCE_PROMOTE=$k \
                run "$f" --option eval-unforce true --option eval-unforce-batch-size 0)
            [[ $out == "$expected" ]] \
                || fail "$f (budget 0, fallback $fallback, promotion after $k forces): got '$out', expected '$expected'"
        done
    done
done

# The fallback happens, and is counted.
stats=$(NIX_UNFORCE_IDENTITY=markers NIX_UNFORCE_STATS=1 NIX_UNFORCE_WALK_BUDGET_BASE=0 NIX_UNFORCE_WALK_BUDGET_PER_KIB=0 \
    nix-instantiate --option eval-unforce true --option eval-unforce-batch-size 0 --eval --strict follows.nix 2>&1 >/dev/null)
[[ $stats =~ over\ budget\ ([0-9]+) ]] || fail "no unforce stats: $stats"
(( BASH_REMATCH[1] >= 1 )) || fail "the closure never ran out of budget: $stats"

# Forgetting force counts only delays promotions: with the count map capped
# at one entry it is dropped after almost every job, and identity holds.
for f in *.nix; do
    [[ $f == set.nix || $f == nested.nix ]] && continue
    expected=$(run "$f" --option eval-unforce false)
    out=$(NIX_UNFORCE_COUNTS_CAP=1 run "$f" --option eval-unforce true --option eval-unforce-batch-size 0)
    [[ $out == "$expected" ]] || fail "$f (counts capped at 1): got '$out', expected '$expected'"
done
stats=$(NIX_UNFORCE_STATS=1 NIX_UNFORCE_COUNTS_CAP=100 nix-instantiate --option eval-unforce true \
    --option eval-unforce-batch-size 0 --eval --strict --json -E 'builtins.genList (i: { inherit i; }) 1000' 2>&1 >/dev/null)
[[ $stats =~ counts\ peak\ ([0-9]+),\ counts\ resets\ ([0-9]+) ]] || fail "no unforce stats: $stats"
(( BASH_REMATCH[1] <= 101 && BASH_REMATCH[2] >= 1 )) || fail "the force counts outgrew their cap: $stats"

# Generated schedules (from the epoch branch): each makes a count-only policy
# diverge at exactly one K. K-1 jobs force R (and P), one job forces P only,
# so P is promoted with P.a copied unforced, then one job forces R alone,
# which reaches K and is promoted holding a copy of P.a's value; P.a is
# reverted and rebuilt by the comparison.
schedule() {
    local k=$1 warm="" i
    for ((i = 1; i < k; i++)); do warm+=' (builtins.seq R true)'; done
    echo "let P = { a = { f = x: x; }; }; R = P.a; in [$warm (P ? a) (builtins.seq R true) (R == P.a) ]"
}
for k in 2 3 4 5; do
    expr=$(schedule "$k")
    expected=$(nix-instantiate --option eval-unforce false --eval --strict -E "$expr")
    for backend in "${backends[@]}"; do
        for promote in 0 1 2 3 4 5; do
            out=$(NIX_UNFORCE_BACKEND=$backend NIX_UNFORCE_PROMOTE=$promote nix-instantiate \
                --option eval-unforce true --option eval-unforce-batch-size 0 --eval --strict -E "$expr")
            [[ $out == "$expected" ]] \
                || fail "schedule for K=$k (backend $backend, NIX_UNFORCE_PROMOTE=$promote): got '$out', expected '$expected'"
        done
    done
done

# The closure's shortcuts (memos, `reached`, copying as it decides) must
# reach the same promotion set as a plain recomputation, which
# NIX_UNFORCE_CLOSURE_CHECK runs after every job and reports on stderr.
for f in *.nix; do
    [[ $f == set.nix || $f == nested.nix ]] && continue
    for k in 2 3; do
        out=$(NIX_UNFORCE_IDENTITY=markers NIX_UNFORCE_CLOSURE_CHECK=1 NIX_UNFORCE_PROMOTE=$k \
            run "$f" --option eval-unforce true --option eval-unforce-batch-size 0)
        [[ $out != *"unforce check"* ]] || fail "$f (promotion after $k forces): $out"
    done
done

# The owner closure (the default) promotes nothing that markers' plain
# closure would not: markers' candidates include every true owner, so an
# extra promotion would mean a misattributed owner. NIX_UNFORCE_OWNER_CHECK
# computes both after every job and reports on stderr.
for f in *.nix; do
    [[ $f == set.nix || $f == nested.nix ]] && continue
    for k in 1 2 3; do
        for consumers in 0 1; do
            out=$(NIX_UNFORCE_OWNER_CHECK=1 NIX_UNFORCE_OWNER_CONSUMERS=$consumers NIX_UNFORCE_PROMOTE=$k \
                run "$f" --option eval-unforce true --option eval-unforce-batch-size 0)
            [[ $out != *"unforce owner check"* ]] || fail "$f (owner check, promotion after $k forces): $out"
            expected=$(run "$f" --option eval-unforce false)
            [[ $out == "$expected" ]] \
                || fail "$f (consumers $consumers, promotion after $k forces): got '$out', expected '$expected'"
        done
    done
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
# Owners are off in the A/B mode.
changes=$(NIX_UNFORCE_IDENTITY=markers ownerStat ownerChanges "$expr")
(( changes == 0 )) || fail "NIX_UNFORCE_IDENTITY=markers still tracks owners: $changes owner changes"

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

# The timeline's budget (review identity-owners-v1, SF1). With 4 KiB, the
# same job runs out: it stops tracking owners and is kept whole, and the
# output is still base's.
overflowStats=$(NIX_UNFORCE_OWNER_BUDGET=4096 NIX_SHOW_STATS=1 NIX_SHOW_STATS_PATH=/dev/stdout nix-instantiate \
    --option eval-unforce true --option eval-unforce-batch-size 0 --eval --strict --json -E "$expr" 2> /dev/null \
    | tr -d ' \n')
[[ $overflowStats =~ \"ownerOverflowJobs\":([0-9]+) ]] && (( BASH_REMATCH[1] >= 1 )) \
    || fail "a 4 KiB owner budget did not overflow"
[[ $overflowStats =~ \"keptJobs\":([0-9]+) ]] && (( BASH_REMATCH[1] >= 1 )) \
    || fail "the job whose timeline overflowed was not kept whole"
NIX_UNFORCE_OWNER_BUDGET=4096 nix-instantiate --option eval-unforce true --option eval-unforce-batch-size 0 \
    --eval --strict --json -E "$expr" > "$TEST_ROOT/alternate-budget.json"
cmp -s "$TEST_ROOT/alternate-budget.json" "$TEST_ROOT/alternate-off.json" \
    || fail "the job whose timeline overflowed differs from base"

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

# The copy limit (print-meta-200: one job copied 1 101 MiB while its 1 679 MiB
# region was still mapped). A job whose Evac(S) exceeds the limit is kept
# whole, with base's output.
expr='let
  xs = builtins.genList (i: { v = [ i ]; }) 20000;
  s = { inherit xs; n = builtins.length xs; };
in [ s.n s.n (builtins.length s.xs) s.xs ]'
limitStats=$(NIX_UNFORCE_COPY_LIMIT=4096 NIX_UNFORCE_PROMOTE=1 NIX_SHOW_STATS=1 NIX_SHOW_STATS_PATH=/dev/stdout \
    nix-instantiate --option eval-unforce true --eval --strict --json -E "$expr" 2> /dev/null | tr -d ' \n')
[[ $limitStats =~ \"ownerCopyLimitJobs\":([0-9]+) ]] && (( BASH_REMATCH[1] >= 1 )) \
    || fail "a 4 KiB copy limit kept no job whole"
NIX_UNFORCE_COPY_LIMIT=4096 NIX_UNFORCE_PROMOTE=1 nix-instantiate --option eval-unforce true --eval --strict --json \
    -E "$expr" > "$TEST_ROOT/copylimit-on.json"
nix-instantiate --option eval-unforce false --eval --strict --json -E "$expr" > "$TEST_ROOT/copylimit-off.json"
cmp -s "$TEST_ROOT/copylimit-on.json" "$TEST_ROOT/copylimit-off.json" || fail "a job kept whole by the copy limit differs from base"
