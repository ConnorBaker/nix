#!/usr/bin/env bash

# ON-vs-OFF byte-identity on a self-contained PURE workload, run under
# `--pure-eval` so the byte-identity gate is deterministic, across unforce
# batch sizes {0 (a revert after every element), a small one, a large one that
# fits the whole result in one batch}.
#
# NON-VACUITY (this is the point): the earlier version of this gate forced a
# broad workload whose reverted cells were internal throwaway — their JSON was
# serialised before their batch ended, so nothing ever re-read a reverted
# cell. Skipping the end-of-batch revert-to-pre-image (a real use-after-free,
# `src/libexpr/unforce.cc` line 2281) then left the output unchanged and the
# gate PASSED while reclamation was broken.
#
# This workload instead re-materialises a REVERTED cell into the compared
# output. `R` and `P.a` are the SAME old cell reached two ways. A batch per
# element forces `R` in early elements (the warm-up `seq`s drive the
# promote-after-two-forces policy and record a cross-element consumer pair),
# then `R` is reverted to a thunk at a batch boundary and LATER elements read
# it again: `(R == P.a)` rebuilds it and checks identity, and `R` / `R.xs` /
# `R.deep` serialise the rebuilt value itself. If the batch-end revert is
# skipped, `R` keeps a forced value pointing into the freed job region, so the
# later reads diverge from base (in practice: deterministic `attribute 'a'
# missing`, the corrupted region re-read). The `--json`, `--xml` and
# `nix eval --json` serialisations must all be `cmp`-clean ON-vs-OFF at every
# batch size.
#
# The non-vacuity assertion is `ownerConsumerPairsPeak >= 1` (a cross-element
# consumer of a promoted target was recorded — the sharing machinery the
# revert/promotion protects actually engaged), NOT merely `reverted >= 1`.
# The companion UAF gate is `eval-unforce-identity.sh`.

source common.sh

# Unforcing never runs in a build with Boehm GC, whatever `eval-unforce` says.
# Such a build reports its collector's heap in NIX_SHOW_STATS.
if [[ $(NIX_SHOW_STATS=1 NIX_SHOW_STATS_PATH=/dev/stdout nix eval --expr 1 2>/dev/null) == *'"heapSize"'* ]]; then
    skipTest "unforcing is disabled in a build with Boehm GC"
fi

# A pure, deterministic workload whose output re-materialises a reverted cell.
read -r -d '' expr <<'NIX' || true
let
  # A shared OLD cell (allocated before any print batch) whose structure's
  # bytes land in the compared output. `R` and `P.a` are the same cell reached
  # two ways; identity must survive a revert-and-rebuild across a batch end.
  P = { a = { tag = "payload"; xs = [ 10 20 30 ]; deep = { k = "v"; n = 7; }; }; };
  R = P.a;
in [
  # Warm-up forces: the promote-after-two-forces policy engages on P and a
  # cross-element consumer pair is recorded (the non-vacuity signal below).
  (builtins.seq R true)
  (builtins.seq R true)
  (builtins.seq R true)
  (P ? a)
  (builtins.seq R true)
  # The reverted-then-rebuilt cell, re-materialised into the compared output:
  (R == P.a)   # identity of the rebuilt cell against its other reach
  R            # the rebuilt cell's whole value
  R.xs         # ... and sub-values
  R.deep
]
NIX

pure=(--option pure-eval true)
off=(--option eval-unforce false)
# {batch per element (reverts between elements, so a reverted cell is re-read
# in a later element), small, one big batch}.
batchSizes=(0 4096 134217728)

# References, with unforcing off.
nix-instantiate "${pure[@]}" "${off[@]}" --eval --strict --json -E "$expr" > "$TEST_ROOT/pure-off.json" \
    || fail "reference --json (unforce off) failed"
nix-instantiate "${pure[@]}" "${off[@]}" --eval --strict --xml -E "$expr" > "$TEST_ROOT/pure-off.xml" \
    || fail "reference --xml (unforce off) failed"
nix eval "${pure[@]}" "${off[@]}" --json --expr "$expr" > "$TEST_ROOT/pure-off.eval.json" \
    || fail "reference nix eval --json (unforce off) failed"

# The identity observation is genuinely present-and-true in the reference: the
# 6th element (index 5) is `(R == P.a)`.
grepQuiet 'true,true,true,true,true,true,' "$TEST_ROOT/pure-off.json" \
    || fail "the identity observation is not present-and-true in the reference: $(< "$TEST_ROOT/pure-off.json")"

for bs in "${batchSizes[@]}"; do
    on=(--option eval-unforce true --option eval-unforce-batch-size "$bs")

    nix-instantiate "${pure[@]}" "${on[@]}" --eval --strict --json -E "$expr" > "$TEST_ROOT/pure-on.json" \
        || fail "--json (unforce on, batch $bs) failed"
    cmp -s "$TEST_ROOT/pure-off.json" "$TEST_ROOT/pure-on.json" \
        || fail "--json differs ON-vs-OFF at batch size $bs (reverted cell re-read into output): $(< "$TEST_ROOT/pure-on.json")"

    nix-instantiate "${pure[@]}" "${on[@]}" --eval --strict --xml -E "$expr" > "$TEST_ROOT/pure-on.xml" \
        || fail "--xml (unforce on, batch $bs) failed"
    cmp -s "$TEST_ROOT/pure-off.xml" "$TEST_ROOT/pure-on.xml" \
        || fail "--xml differs ON-vs-OFF at batch size $bs"

    nix eval "${pure[@]}" "${on[@]}" --json --expr "$expr" > "$TEST_ROOT/pure-on.eval.json" \
        || fail "nix eval --json (unforce on, batch $bs) failed"
    cmp -s "$TEST_ROOT/pure-off.eval.json" "$TEST_ROOT/pure-on.eval.json" \
        || fail "nix eval --json differs ON-vs-OFF at batch size $bs"
done

# The gate is not vacuous: unforcing really runs on this workload (and off
# really does not), and the specific machinery the revert/promotion protects
# — a cross-element consumer of a promoted target — actually engaged.
unforceStat() { # FIELD OPTION...
    local field=$1
    shift
    NIX_SHOW_STATS=1 NIX_SHOW_STATS_PATH="$TEST_ROOT/pure-stats.json" \
        nix-instantiate "${pure[@]}" "$@" --eval --strict --json -E "$expr" >/dev/null 2>/dev/null \
        || fail "statistics run failed ($*)"
    jq ".unforce.$field // 0" "$TEST_ROOT/pure-stats.json"
}
[[ $(unforceStat scopes "${off[@]}") == 0 ]] \
    || fail "unforcing ran with eval-unforce = false"
(( $(unforceStat scopes --option eval-unforce true --option eval-unforce-batch-size 0) >= 1 )) \
    || fail "unforcing did not run with eval-unforce = true, batch size 0"
# The guarded path: a reverted cell is re-forced, AND a cross-element consumer
# pair was recorded (the promotion heuristic's own signal). This is strictly
# stronger than `reverted >= 1`: it proves the shared-cell machinery engaged,
# not just that some throwaway cell was reverted.
(( $(unforceStat reverted --option eval-unforce true --option eval-unforce-batch-size 0) >= 1 )) \
    || fail "no cell was reverted with eval-unforce = true, batch size 0"
(( $(unforceStat ownerConsumerPairsPeak --option eval-unforce true --option eval-unforce-batch-size 0) >= 1 )) \
    || fail "no cross-element consumer pair was recorded: the shared-cell promotion path was not exercised"
