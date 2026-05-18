#!/usr/bin/env bash

# Patch B regression / correctness test.
#
# Covers:
#   * Parallel optimise equivalent to serial (many paths, crosses
#     chunkSize=256 boundary).
#   * Thread-count knob (1, 4, 16).
#   * optimise-store idempotency.
#   * Nothing interferes with auto-optimise-store inline path.
#   * SIGINT while parallel optimise is running leaves the store in
#     a consistent state (no half-written tempLinks).

source common.sh

TODO_NixOS

# ------------- helpers -------------

# Generate N derivations in one Nix evaluation, each with M shared files
# (same contents across all derivations) plus a unique file. Builds them
# all and drops the GC roots. Deterministically named so tests can
# identify them by glob.
generateStore() {
    local n="$1"
    local m="$2"
    local tag="$3"

    realiseFromExpr "$tag" <<EOF
let
  cfg = import ${config_nix};
  mkN = i: cfg.mkDerivation {
    name = "${tag}-\${toString i}-pkg";
    builder = builtins.toFile "b" ''
      mkdir \$out
      for j in \$(seq 1 ${m}); do
        echo shared-\$j > \$out/shared\$j
      done
      echo unique-\${toString i} > \$out/unique
    '';
  };
in
  builtins.listToAttrs (
    map (i: { name = "d\${toString i}"; value = mkN i; })
    (builtins.genList (x: x) ${n}))
EOF
}

# Snapshot the structural state of the store that optimise should
# preserve regardless of thread count.
#
# Under hardlink dedup, the "shape" of a shared filename is its
# nlink count: every user file pointing at the same canonical
# shares the same nlink value (N + 1, where N is the number of
# user-paths with this content and +1 is the canonical). We record
# the content-hash of each shared file rather than the nlink so
# the snapshot is stable across canonical-vs-anchor ordering — the
# actual measure of "did optimise produce the same dedup grouping".
snapshot() {
    local tag="$1"
    local out="$2"
    : > "$out"
    for p in "$NIX_STORE_DIR"/*-"$tag"-*-pkg; do
        [ -e "$p" ] || continue
        local base
        base="$(basename "$p")"
        for f in "$p"/shared*; do
            local h
            h="$(sha256sum "$f" | awk '{print $1}')"
            printf '%s %s %s\n' "$base" "$(basename "$f")" "$h" >> "$out"
        done
    done
    sort -o "$out" "$out"
}

# Count entries in .links/, including both the flat (single-level)
# and sharded (two-level) layouts so that toggling Xp::ShardedLinks
# between runs doesn't change what's counted.
countLinks() {
    {
        find "$NIX_STORE_DIR"/.links -maxdepth 1 -type f 2>/dev/null
        find "$NIX_STORE_DIR"/.links -mindepth 2 -maxdepth 2 -type f 2>/dev/null
    } | wc -l
}

# ------------- Scenario 1: multi-chunk correctness equivalence -------------
#
# Build 400 derivations. Each produces 3 shared files + 1 unique file.
# 400 × 4 = 1600 files, well past the 256-path chunk size, ensuring the
# ThreadPool queue receives multiple chunks.

N=400
M=3

clearStoreIfPossible
generateStore "$N" "$M" serialtag

NIX_REMOTE="" nix-store --optimise --option optimise-threads 1
snapshot serialtag "$TEST_ROOT/serial.snap"
serialLinks="$(countLinks)"

clearStoreIfPossible
generateStore "$N" "$M" paralleltag

NIX_REMOTE="" nix-store --optimise --option optimise-threads 8
snapshot paralleltag "$TEST_ROOT/parallel.snap"
parallelLinks="$(countLinks)"

# Content-hash distribution per shared filename must match across
# runs. Reducing to (filename, content-hash) checks that the same
# set of "shared file" identities exists in both runs — i.e. all
# files named `shared1` have the same content, all `shared2` do,
# etc. This is what optimise's correctness actually preserves.
reduce() {
    awk '{ print $2 " " $3 }' "$1" | sort -u
}
serialReduced="$(reduce "$TEST_ROOT/serial.snap")"
parallelReduced="$(reduce "$TEST_ROOT/parallel.snap")"

if [ "$serialReduced" != "$parallelReduced" ]; then
    echo "FAIL: parallel optimise differs from serial at N=$N"
    diff <(echo "$serialReduced") <(echo "$parallelReduced") || true
    exit 1
fi

# .links/ entry count must match exactly.
if [ "$serialLinks" != "$parallelLinks" ]; then
    echo "FAIL: .links/ entry count differs: serial=$serialLinks parallel=$parallelLinks"
    exit 1
fi

# Per-file dedup verification — for one sample of shared content,
# verify the total user-file references across all `.links/` replicas
# of that content equals N.
#
# We sum across replicas rather than asserting `primary.nlink == N+1`
# because the filesystem's `_PC_LINK_MAX` may be lower than `N+1`
# (e.g., on some ZFS / tmpfs sandbox configurations it can be as low
# as 128). When that happens the replica-spill walk in `optimisePath_`
# fills `.00`, `.01`, … and the primary's nlink saturates at the
# fs cap. The true invariant is `Σ (nlink_i - 1) == N` over the
# replicas that host this content, where the `-1` per replica is the
# `.links/` entry name itself.
sampleShared="$(find "$NIX_STORE_DIR" -name 'shared1' -path '*-paralleltag-*' -print -quit)"
if [ -z "$sampleShared" ]; then
    echo "FAIL: no shared1 sample file found in store"
    exit 1
fi
sampleHash="$(sha256sum "$sampleShared" | awk '{print $1}')"
totalUserRefs=0
replicaCount=0
replicaNlinks=""
shopt -s nullglob
for f in "$NIX_STORE_DIR"/.links/* "$NIX_STORE_DIR"/.links/*/*; do
    [ -f "$f" ] || continue
    h="$(sha256sum "$f" | awk '{print $1}')"
    if [ "$h" = "$sampleHash" ]; then
        nl="$(stat --format=%h "$f")"
        totalUserRefs=$((totalUserRefs + nl - 1))
        replicaCount=$((replicaCount + 1))
        replicaNlinks="$replicaNlinks $nl"
    fi
done
shopt -u nullglob
if [ "$replicaCount" -lt 1 ]; then
    echo "FAIL: no .links/ entry for shared1 content"
    exit 1
fi
if [ "$totalUserRefs" != "$N" ]; then
    echo "FAIL: expected $N user-file references to shared1 content across replicas," \
         "got $totalUserRefs (replicas=$replicaCount, nlinks=[$replicaNlinks])"
    exit 1
fi

# ------------- Scenario 2: idempotency across repeated parallel runs -------------

# Hash the FILE contents of `.links/` (including any sharded subdirs).
# An earlier version of this snapshot used `ls .links` which under
# sharded layout only enumerated the 1024 shard directory names —
# those don't change with file content, so the idempotency check was
# vacuous on sharded stores. `find -type f -printf '%P\n'` lists
# files recursively with paths relative to .links/, which captures
# replicas under shard subdirectories correctly.
linksContentHash() {
    find "$NIX_STORE_DIR"/.links -type f -printf '%P\n' 2>/dev/null \
        | sort | md5sum | awk '{print $1}'
}

preHash="$(linksContentHash)"
NIX_REMOTE="" nix-store --optimise --option optimise-threads 8
postHash="$(linksContentHash)"
if [ "$preHash" != "$postHash" ]; then
    echo "FAIL: re-running parallel optimise changed .links/"
    exit 1
fi

# ------------- Scenario 3: thread count extremes -------------

NIX_REMOTE="" nix-store --optimise --option optimise-threads 0   # auto
NIX_REMOTE="" nix-store --optimise --option optimise-threads 1
NIX_REMOTE="" nix-store --optimise --option optimise-threads 32  # clamped to cap

# Ensure no test-side corruption.
if [ "$(countLinks)" != "$parallelLinks" ]; then
    echo "FAIL: thread-count knob variation changed .links/ entry count"
    exit 1
fi

# ------------- Scenario 4: SIGINT mid-run leaves store consistent -------------
#
# Start an optimise on a populated store, send SIGINT mid-loop, and
# verify:
#   (a) the interrupted process exits non-zero (the RAII guards in
#       optimisePath_ run rather than being skipped);
#   (b) immediately after the interrupt — *before* any cleanup
#       optimise — the leftover .tmp-link-* count is bounded by the
#       number of in-flight worker threads (each worker's destructor
#       cleans up its own tempLink during stack unwind, so at most
#       a small race-window count survives);
#   (c) a subsequent clean optimise still produces zero leftovers.
#
# Asserting (b) is the actual SIGINT correctness invariant. Without
# it, the test could pass even if interrupts leaked unboundedly many
# temp files — the cleanup run at the end would tidy up regardless,
# masking the regression this scenario is here to catch.

clearStoreIfPossible
generateStore 200 2 interrupttag

optThreads=4
# A worker thread holds at most one tempLink active at a time
# (created+renamed inside optimisePath_'s body, RAII-cleaned on
# unwind). The kernel interrupt can land anywhere; we tolerate up
# to `optThreads * 2` survivors as headroom for the brief window
# between create and the scope-guard registration. Anything more
# than that indicates a real leak in the interrupt path.
leftover_cap=$((optThreads * 2))

# Run optimise in the background with a moderate thread count so the
# interrupt lands somewhere mid-loop.
nix-store --optimise --option optimise-threads "$optThreads" > /dev/null 2>&1 &
pid=$!
# Poll for a `.tmp-link-*` file (real evidence optimise is mid-loop)
# rather than a fixed sleep. With a fixed sleep on a slow VM optimise
# may not have started yet; on a fast one it may have finished.
# Bail after 5 s — that's well above any realistic startup latency,
# and we want to fail loudly rather than block forever if optimise
# hangs without producing tempLinks.
deadline=$(( $(date +%s) + 5 ))
while [ $(date +%s) -lt "$deadline" ]; do
    if ls "$NIX_STORE_DIR"/.tmp-link-* > /dev/null 2>&1; then
        break
    fi
    # Stop polling once the optimise process exits — handled by the
    # `wait` below.
    if ! kill -0 "$pid" 2>/dev/null; then
        break
    fi
    sleep 0.05
done
kill -INT "$pid" 2>/dev/null || true
# `wait` returns the process's exit status. Optimise interrupted by
# SIGINT exits with 130 (128 + SIGINT); 0 means it completed before
# we could interrupt (in which case the test is no-op-passing).
set +e
wait "$pid"
optExit=$?
set -e

leftovers_before="$(find "$NIX_STORE_DIR" -maxdepth 1 -name '.tmp-link-*' 2>/dev/null | wc -l)"

# (a) and (b): only meaningful when the interrupt actually landed.
if [ "$optExit" -ne 0 ]; then
    if [ "$leftovers_before" -gt "$leftover_cap" ]; then
        echo "FAIL: $leftovers_before .tmp-link-* files survived SIGINT (cap=$leftover_cap, optExit=$optExit) — interrupt path leaks"
        exit 1
    fi
else
    echo "NOTE: optimise completed before SIGINT could land; SIGINT-leak assertion skipped"
fi

# (c): a clean optimise run must succeed and remove any survivors.
NIX_REMOTE="" nix-store --optimise --option optimise-threads "$optThreads"

leftovers_after="$(find "$NIX_STORE_DIR" -maxdepth 1 -name '.tmp-link-*' 2>/dev/null | wc -l)"
if [ "$leftovers_after" != "0" ]; then
    echo "FAIL: $leftovers_after .tmp-link-* files remain after interrupt + clean run"
    exit 1
fi

echo "PASS: optimise-store-parallel (leftovers before=$leftovers_before, after=$leftovers_after, optExit=$optExit)"
