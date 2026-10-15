#!/usr/bin/env bash

# PROTOTYPE (unforce): promotion must not split value identity. Each program
# in eval-unforce-identity/ compares a value kept by a promoted cell with the
# same value reached through a cell that a job reverted (see the force
# markers in src/libexpr/unforce.cc). With a job per element, under every
# promotion threshold and backend, the output must match the output without
# unforcing.

source common.sh

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
    # The exit status is part of the output (bind3.nix fails, as in base).
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
            out=$(NIX_UNFORCE_WALK_BUDGET_BASE=0 NIX_UNFORCE_WALK_BUDGET_PER_KIB=0 \
                NIX_UNFORCE_WALK_FALLBACK=$fallback NIX_UNFORCE_PROMOTE=$k \
                run "$f" --option eval-unforce true --option eval-unforce-batch-size 0)
            [[ $out == "$expected" ]] \
                || fail "$f (budget 0, fallback $fallback, promotion after $k forces): got '$out', expected '$expected'"
        done
    done
done

# The fallback happens, and is counted.
stats=$(NIX_UNFORCE_STATS=1 NIX_UNFORCE_WALK_BUDGET_BASE=0 NIX_UNFORCE_WALK_BUDGET_PER_KIB=0 \
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
    --option eval-unforce-batch-size 0 --eval --strict -E 'builtins.genList (i: { inherit i; }) 1000' 2>&1 >/dev/null)
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
