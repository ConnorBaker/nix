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
    if [[ $f == nested.nix ]]; then
        nix eval "$@" --impure --file "$f" 2>&1
    else
        nix-instantiate "$@" --eval --strict "$f" 2>&1
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
