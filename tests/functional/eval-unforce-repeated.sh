#!/usr/bin/env bash

# PROTOTYPE (unforce): a container printed in one job and reached again in a
# later one prints as «repeated», as without unforcing: the printers keep
# their `seen` sets exact across jobs (`UnforceSeen` in
# src/libexpr/unforce.cc). With a job per top-level element, under every
# promotion threshold, `nix-instantiate --eval --strict` and `nix eval` must
# print what they print without unforcing. The e-*.nix programs are a sample
# of the printing corpus of the review of markers v2 (§2.2, e2q), on which
# every program where base prints «repeated» used to differ. The attr*.nix
# programs and ae-*.nix (the same sample with the top-level list turned into
# an attribute set named z3..z0, from the review of markers v3, §1.2) are
# top-level attribute sets whose Bindings order, in which
# `nix-instantiate --strict` forces them, is not the printed order.
#
# Bump backend only: the mimalloc backend's nested scopes revert what every
# printed container forced, so there shared values still print in full.

source common.sh

# Unforcing never runs in a build with Boehm GC, whatever `eval-unforce` says.
# Such a build reports its collector's heap in NIX_SHOW_STATS.
if [[ $(NIX_SHOW_STATS=1 NIX_SHOW_STATS_PATH=/dev/stdout nix eval --expr 1 2>/dev/null) == *'"heapSize"'* ]]; then
    skipTest "unforcing is disabled in a build with Boehm GC"
fi

cd eval-unforce-repeated

run() {
    local mode=$1 f=$2
    shift 2
    if [[ $mode == eval ]]; then
        nix eval "$@" --impure --file "$f" 2>&1 || echo "exit status $?"
    else
        nix-instantiate "$@" --eval --strict "$f" 2>&1 || echo "exit status $?"
    fi
}

on=(--option eval-unforce true --option eval-unforce-batch-size 0)
for f in rep*.nix e-*.nix attr*.nix ae-*.nix; do
    for mode in instantiate eval; do
        expected=$(run "$mode" "$f" --option eval-unforce false)
        if [[ $mode == instantiate && $expected != *'«repeated»'* ]]; then
            fail "$f: prints no «repeated» without unforcing: $expected"
        fi
        out=$(NIX_UNFORCE_BACKEND=bump run "$mode" "$f" "${on[@]}")
        [[ $out == "$expected" ]] \
            || fail "$f ($mode): got '$out', expected '$expected'"
        # The owner closure's cross-check: it is the runtime backstop for
        # ownerOf/candidate exactness (NIX_UNFORCE_DEBUG=owner-check).
        out=$(NIX_UNFORCE_BACKEND=bump NIX_UNFORCE_DEBUG=owner-check run "$mode" "$f" "${on[@]}")
        [[ $out != *"unforce owner check"* ]] || fail "$f ($mode): $out"
    done
done

# In the default configuration too (attr-desc-batch.nix's zz takes more than
# a batch).
for mode in instantiate eval; do
    expected=$(run "$mode" attr-desc-batch.nix --option eval-unforce false)
    out=$(NIX_UNFORCE_BACKEND=bump run "$mode" attr-desc-batch.nix --option eval-unforce true)
    [[ $out == "$expected" ]] || fail "attr-desc-batch.nix ($mode, defaults): got '$out', expected '$expected'"
done

# What was kept is counted.
stats=$(NIX_UNFORCE_BACKEND=bump NIX_SHOW_STATS=1 NIX_SHOW_STATS_PATH=/dev/null nix-instantiate "${on[@]}" --eval --strict rep.nix 2>&1 >/dev/null)
[[ $stats =~ seen\ nominated\ ([0-9]+),\ kept\ ([0-9]+)\;\ jobs\ kept\ whole\ ([0-9]+)\ \(([0-9]+)\ KiB\)$ ]] \
    || fail "no unforce stats: $stats"
(( BASH_REMATCH[1] >= 1 )) || fail "rep.nix: nothing nominated: $stats"
