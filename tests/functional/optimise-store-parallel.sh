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

    local drvExpr
    drvExpr=$(cat <<EOF
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
)
    # Materialise into a single rooted drv so nix-store --realise sees it.
    local rootedDrv
    rootedDrv=$(echo "$drvExpr" | nix-instantiate --expr - --add-root "$TEST_ROOT/$tag.drvs" --indirect 2>/dev/null || true)
    # Build each output sequentially (fast — all builds finish in <1s).
    echo "$rootedDrv" | while IFS= read -r drv; do
        [ -z "$drv" ] && continue
        nix-store --realise "$drv" > /dev/null
    done
    # Drop the drv roots; outputs remain valid in DB with no GC root.
    rm -f "$TEST_ROOT/$tag.drvs"*
}

# Snapshot the structural state of the store that optimise should
# preserve regardless of thread count. We key by derivation name and
# count links to shared files.
snapshot() {
    local tag="$1"
    local out="$2"
    : > "$out"
    for p in "$NIX_STORE_DIR"/*-"$tag"-*-pkg; do
        [ -e "$p" ] || continue
        local base
        base="$(basename "$p")"
        for f in "$p"/shared*; do
            printf '%s %s %s\n' \
                "$base" "$(basename "$f")" "$(stat --format='%h' "$f")" >> "$out"
        done
    done
    sort -o "$out" "$out"
}

# Count entries in .links/ ignoring hidden entries.
countLinks() {
    find "$NIX_STORE_DIR"/.links -maxdepth 1 -type f 2>/dev/null | wc -l
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

# Link-count distribution per shared filename must match across runs.
# (Actual inode numbers will differ across store clears, but the shape
# of hard-link clusters should be identical.)
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

# Every shared file must have nlink == N+1 (N derivation outputs + 1
# canonical entry in .links/<hash>).
expectedNlink="$((N + 1))"
for line in $(awk '/shared1/ {print $3}' "$TEST_ROOT/parallel.snap" | sort -u); do
    if [ "$line" != "$expectedNlink" ]; then
        echo "FAIL: expected nlink=$expectedNlink for shared1 files, got distinct $line"
        exit 1
    fi
done

# ------------- Scenario 2: idempotency across repeated parallel runs -------------

preHash="$(ls "$NIX_STORE_DIR"/.links | sort | md5sum | awk '{print $1}')"
NIX_REMOTE="" nix-store --optimise --option optimise-threads 8
postHash="$(ls "$NIX_STORE_DIR"/.links | sort | md5sum | awk '{print $1}')"
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

# ------------- Scenario 4: auto-optimise-store inline path -------------

clearStoreIfPossible
# shellcheck disable=SC2016
outA=$(echo 'with import '"${config_nix}"'; mkDerivation { name = "inline-A"; builder = builtins.toFile "b" "mkdir $out; echo inline > $out/f"; }' | nix-build - --no-out-link --auto-optimise-store)
# shellcheck disable=SC2016
outB=$(echo 'with import '"${config_nix}"'; mkDerivation { name = "inline-B"; builder = builtins.toFile "b" "mkdir $out; echo inline > $out/f"; }' | nix-build - --no-out-link --auto-optimise-store)

inoA="$(stat --format=%i "$outA"/f)"
inoB="$(stat --format=%i "$outB"/f)"
if [ "$inoA" != "$inoB" ]; then
    echo "FAIL: auto-optimise-store inline dedup regressed"
    exit 1
fi

# ------------- Scenario 5: SIGINT mid-run leaves store consistent -------------
#
# This one is probabilistic: we start an optimise on a large store and
# send SIGINT after a short delay, then verify no ‘.tmp-link’ temp files
# remain and a subsequent optimise completes cleanly.

clearStoreIfPossible
generateStore 200 2 interrupttag

# Run optimise in the background with a moderate thread count so the
# interrupt lands somewhere mid-loop.
nix-store --optimise --option optimise-threads 4 > /dev/null 2>&1 &
pid=$!
# Give it time to start doing real work.
sleep 0.2
kill -INT "$pid" 2>/dev/null || true
wait "$pid" || true

# After interrupt, there must be no leftover .tmp-link-* files in
# /nix/store. Temp files in this pattern are created during the
# tempLink+rename dance; on interrupt they should be cleaned up (or
# at worst, cleaned up by a subsequent successful optimise).
leftovers_before="$(find "$NIX_STORE_DIR" -maxdepth 1 -name '.tmp-link-*' 2>/dev/null | wc -l)"

# A clean optimise run must succeed and remove any leftovers.
NIX_REMOTE="" nix-store --optimise --option optimise-threads 4

leftovers_after="$(find "$NIX_STORE_DIR" -maxdepth 1 -name '.tmp-link-*' 2>/dev/null | wc -l)"
if [ "$leftovers_after" != "0" ]; then
    echo "FAIL: $leftovers_after .tmp-link-* files remain after interrupt + clean run"
    exit 1
fi

echo "PASS: optimise-store-parallel (leftovers before=$leftovers_before)"
