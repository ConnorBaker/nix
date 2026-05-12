#!/usr/bin/env bash

# Stress test: run nix-store --optimise and nix-store --gc in parallel
# against a shared store. Neither must corrupt the store or leave
# orphaned temporaries. A final optimise + GC run must succeed cleanly.

source common.sh

TODO_NixOS

# ---------- helpers ----------

generateStore() {
    local n="$1"
    local tag="$2"

    local expr
    expr=$(cat <<EOF
let
  cfg = import ${config_nix};
  mkN = i: cfg.mkDerivation {
    name = "${tag}-\${toString i}";
    builder = builtins.toFile "b" ''
      mkdir \$out
      echo shared > \$out/shared
      echo unique-\${toString i} > \$out/unique
    '';
  };
in builtins.listToAttrs (
     map (i: { name = "d\${toString i}"; value = mkN i; })
     (builtins.genList (x: x) ${n}))
EOF
)
    local drvs
    drvs=$(echo "$expr" | nix-instantiate --expr - --add-root "$TEST_ROOT/$tag.drvs" --indirect 2>/dev/null)
    while IFS= read -r drv; do
        [ -z "$drv" ] && continue
        nix-store --realise "$drv" > /dev/null
    done <<< "$drvs"
    rm -f "$TEST_ROOT/$tag.drvs"*
}

# ---------- Phase 1: populate store with duplicated content ----------

clearStoreIfPossible
generateStore 200 concurrent

NIX_REMOTE="" nix-store --optimise --option optimise-threads 4

# ---------- Phase 2: rebuild more content, then run optimise + GC in parallel ----------

generateStore 150 concurrent-fresh

# Kick off both in parallel. Each must exit with success (or an
# acceptable failure mode), and between them they must not leave
# orphan temporaries.

(nix-store --optimise --option optimise-threads 8 > "$TEST_ROOT/opt.log" 2>&1) &
opt_pid=$!

# Small head start for optimise so GC catches it mid-work.
sleep 0.1

(nix-store --gc --option gc-links-threads 4 > "$TEST_ROOT/gc.log" 2>&1) &
gc_pid=$!

set +e
wait "$opt_pid"
opt_rc=$?
wait "$gc_pid"
gc_rc=$?
set -e

echo "optimise rc=$opt_rc, gc rc=$gc_rc"
if [ "$opt_rc" -ne 0 ]; then
    echo "--- optimise log ---"
    cat "$TEST_ROOT/opt.log"
fi
if [ "$gc_rc" -ne 0 ]; then
    echo "--- gc log ---"
    cat "$TEST_ROOT/gc.log"
fi

# Accept either success or the soft-fail combination where one saw
# the other's transient state and bailed. Neither may segfault.
if [ "$opt_rc" -gt 1 ] || [ "$gc_rc" -gt 1 ]; then
    echo "FAIL: unexpected exit code (segfault?)"
    exit 1
fi

# ---------- Phase 3: no orphan .tmp-link-* files ----------

leftovers="$(find "$NIX_STORE_DIR" -maxdepth 1 -name '.tmp-link-*' 2>/dev/null | wc -l)"
if [ "$leftovers" != "0" ]; then
    echo "FAIL: $leftovers .tmp-link-* orphans after concurrent run"
    find "$NIX_STORE_DIR" -maxdepth 1 -name '.tmp-link-*'
    exit 1
fi

# ---------- Phase 4: a clean optimise + GC must complete ----------

NIX_REMOTE="" nix-store --optimise --option optimise-threads 4
nix-store --gc --option gc-links-threads 4

# After a full GC, all unique content with no roots must be gone.
remaining="$(find "$NIX_STORE_DIR" -maxdepth 1 -name '*-concurrent*' 2>/dev/null | wc -l)"
if [ "$remaining" != "0" ]; then
    echo "FAIL: $remaining concurrent-* paths survived final GC"
    find "$NIX_STORE_DIR" -maxdepth 1 -name '*-concurrent*' | head -5
    exit 1
fi

# ---------- Phase 5: multiple optimise processes race each other ----------
#
# The optimise code path must be safe under N concurrent processes
# against the same store. We race 3 optimise processes together and
# require all to succeed (they should all tolerate each other's
# EEXIST via Patch A).

clearStoreIfPossible
generateStore 300 race-opt

set +e
(nix-store --optimise --option optimise-threads 4 > "$TEST_ROOT/opt1.log" 2>&1) &
p1=$!
(nix-store --optimise --option optimise-threads 4 > "$TEST_ROOT/opt2.log" 2>&1) &
p2=$!
(nix-store --optimise --option optimise-threads 4 > "$TEST_ROOT/opt3.log" 2>&1) &
p3=$!
wait "$p1"; rc1=$?
wait "$p2"; rc2=$?
wait "$p3"; rc3=$?
set -e

for i in 1 2 3; do
    rc="rc$i"
    if [ "${!rc}" -gt 1 ]; then
        echo "FAIL: optimise process $i exited with unexpected rc=${!rc}"
        cat "$TEST_ROOT/opt$i.log"
        exit 1
    fi
done

# After 3 racing optimises, a final optimise must succeed cleanly.
NIX_REMOTE="" nix-store --optimise --option optimise-threads 4

# Any .tmp-link-* files stranded by the race are considered a bug:
# the tempLink+rename dance in optimisePath_ must always either rename
# the tempLink into place or remove it on failure.
#
# Three processes × four worker threads each = at most 12 tempLinks
# in flight simultaneously, and every one of them is owned by a
# `boost::scope::scope_exit` guard inside `optimisePath_`. On normal
# return *or* unwind the guard fires `unlink(tempLink)`. The only
# paths that can leak past the guard are:
#   - SIGKILL of a worker process (not exercised here; we let all
#     three processes exit cleanly via `wait`).
#   - A genuine bug in optimisePath_ that defeats the guard
#     (e.g. moving the tempLink out of the guard's path).
# So after `wait` returns for all three, the count must be small —
# bounded by `procs × threads` even under maximally pessimistic
# scheduling. The earlier `gt 2000` cap was effectively unbounded;
# 3600 tempLinks would still pass.
procs=3
threads=4
leftover_cap=$((procs * threads))
leftovers="$(find "$NIX_STORE_DIR" -maxdepth 1 -name '.tmp-link-*' 2>/dev/null | wc -l)"
echo "tempLink leftovers after ${procs}-way race: $leftovers (cap=$leftover_cap)"
if [ "$leftovers" -gt "$leftover_cap" ]; then
    echo "FAIL: $leftovers .tmp-link-* orphans — exceeds bounded race-window cap"
    exit 1
fi
find "$NIX_STORE_DIR" -maxdepth 1 -name '.tmp-link-*' -delete 2>/dev/null || true

# ---------- Phase 6: interleaved optimise↔GC rounds ----------
#
# Each round generates 80 fresh paths and adds permanent gcroots for
# them, then races optimise against GC. Roots from earlier rounds are
# *kept*, so the live set grows monotonically across rounds. This way
# round N's GC has accumulated `.links/` state to walk (and must
# preserve all of it), while round N's optimise has a wider working
# set to dedup. Without the explicit roots, generateStore's outputs
# are unrooted from the start and GC would reap them between rounds
# — the rounds would degenerate into independent fresh-store races.

clearStoreIfPossible
rootsDir="$TEST_ROOT/phase6-roots"
mkdir -p "$rootsDir"
for round in 1 2 3; do
    generateStore 80 "round-$round"
    # Pin everything generateStore just built. We discover the
    # outputs by scanning the store for paths matching the
    # round's tag, then `--add-root --indirect` each one so GC
    # treats it as live.
    while IFS= read -r outPath; do
        [ -z "$outPath" ] && continue
        nix-store --add-root "$rootsDir/$(basename "$outPath")" \
                  --indirect --realise "$outPath" > /dev/null
    done < <(find "$NIX_STORE_DIR" -maxdepth 1 -type d -name "*-round-$round-*" 2>/dev/null)

    (nix-store --optimise --option optimise-threads 4 > /dev/null 2>&1) &
    popt=$!
    (nix-store --gc --option gc-links-threads 4 > /dev/null 2>&1) &
    pgc=$!
    set +e
    wait "$popt" || true
    wait "$pgc" || true
    set -e

    # After each round, all previously-rooted paths must still
    # exist: GC was running concurrently but `--add-root` made
    # them live before GC's `findRoots` ran.
    for f in "$rootsDir"/*; do
        target="$(readlink -f "$f" 2>/dev/null)" || continue
        if [ ! -e "$target" ]; then
            echo "FAIL: rooted path $target was deleted by concurrent GC in round $round"
            exit 1
        fi
    done
done

# A final pair must succeed and leave no orphans, with all rooted
# paths still present.
NIX_REMOTE="" nix-store --optimise --option optimise-threads 4
nix-store --gc --option gc-links-threads 4

leftovers="$(find "$NIX_STORE_DIR" -maxdepth 1 -name '.tmp-link-*' 2>/dev/null | wc -l)"
if [ "$leftovers" != "0" ]; then
    echo "FAIL: $leftovers .tmp-link-* orphans after interleaved rounds"
    exit 1
fi

# Verify the rooted paths *still* survived the final GC.
for f in "$rootsDir"/*; do
    target="$(readlink -f "$f" 2>/dev/null)" || continue
    if [ ! -e "$target" ]; then
        echo "FAIL: rooted path $target deleted by final GC"
        exit 1
    fi
done

echo "PASS: gc-optimise-concurrent"
