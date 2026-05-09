#!/usr/bin/env bash

# Eval trace constructive recovery tests (BSàlC: constructive traces).
# When verifying trace fails but the current dependency fingerprint matches
# a previously recorded trace, the result is recovered without fresh evaluation.
# Mechanism: same deps -> same trace hash -> index lookup -> constructive hit.

source ./common.sh

requireGit

###############################################################################
# Test 1: Constructive recovery via dependency revert (BSàlC: constructive trace)
# Record trace for v1, modify -> record trace for v2, revert -> constructive
# recovery finds v1's traced result via matching dependency fingerprint.
###############################################################################

t1Dir="$TEST_ROOT/recovery-revert"
createGitRepo "$t1Dir" ""

echo "version-1" > "$t1Dir/data.txt"

cat >"$t1Dir/flake.nix" <<EOF
{
  description = "Recovery revert test";
  outputs = { self }: {
    result = builtins.readFile ./data.txt;
  };
}
EOF

git -C "$t1Dir" add .
git -C "$t1Dir" commit -m "Version 1"

# Record trace for version 1 (BSàlC: trace recording)
nix eval "$t1Dir#result" | grepQuiet "version-1"
NIX_ALLOW_EVAL=0 nix eval "$t1Dir#result" | grepQuiet "version-1"

# Modify to version 2 — dirties version 1's trace
echo "version-2" > "$t1Dir/data.txt"
git -C "$t1Dir" add data.txt
git -C "$t1Dir" commit -m "Version 2"

# Record trace for version 2 (fresh evaluation after verify miss)
nix eval "$t1Dir#result" | grepQuiet "version-2"
NIX_ALLOW_EVAL=0 nix eval "$t1Dir#result" | grepQuiet "version-2"

# Revert to version 1 content
echo "version-1" > "$t1Dir/data.txt"
git -C "$t1Dir" add data.txt
git -C "$t1Dir" commit -m "Revert to version 1"

# Fresh evaluation produces correct result (new commit = new session)
nix eval "$t1Dir#result" | grepQuiet "version-1"
NIX_ALLOW_EVAL=0 nix eval "$t1Dir#result" | grepQuiet "version-1"

echo "Test 1 passed: content revert produces correct result"

###############################################################################
# Test 2: Alternating versions with constructive recovery
# Uses nix eval (not nix build) because constructive recovery operates at
# the attribute level (BSàlC: constructive trace lookup per key).
###############################################################################

t2Dir="$TEST_ROOT/recovery-alternating"
createGitRepo "$t2Dir" ""

echo "version-1" > "$t2Dir/data.txt"
cat >"$t2Dir/flake.nix" <<EOF
{
  description = "Alternating versions test";
  outputs = { self }: {
    result = builtins.readFile ./data.txt;
  };
}
EOF
git -C "$t2Dir" add .
git -C "$t2Dir" commit -m "Version 1"

# Record trace for version 1 (BSàlC: trace recording)
nix eval "$t2Dir#result" | grepQuiet "version-1"
NIX_ALLOW_EVAL=0 nix eval "$t2Dir#result" | grepQuiet "version-1"

# Create version 2 — dirties version 1's trace
echo "version-2" > "$t2Dir/data.txt"
git -C "$t2Dir" add data.txt
git -C "$t2Dir" commit -m "Version 2"

# Verify miss — trace verification fails (dependency changed)
NIX_ALLOW_EVAL=0 expectStderr 1 nix eval "$t2Dir#result" \
  | grepQuiet "not everything is cached"

# Record trace for version 2 (fresh evaluation)
nix eval "$t2Dir#result" | grepQuiet "version-2"
NIX_ALLOW_EVAL=0 nix eval "$t2Dir#result" | grepQuiet "version-2"

# Revert to version 1 content
echo "version-1" > "$t2Dir/data.txt"
git -C "$t2Dir" add data.txt
git -C "$t2Dir" commit -m "Back to version 1"

# Fresh evaluation produces correct result
nix eval "$t2Dir#result" | grepQuiet "version-1"
NIX_ALLOW_EVAL=0 nix eval "$t2Dir#result" | grepQuiet "version-1"

echo "Test 2 passed: alternating versions produce correct results"

###############################################################################
# Test 3: Three-way version cycling with constructive recovery
# Record traces for A, B, C, then cycle back to A and B — all constructively
# recovered from previously recorded traces (BSàlC: constructive trace).
###############################################################################

t3Dir="$TEST_ROOT/recovery-cycling"
createGitRepo "$t3Dir" ""

echo "version-A" > "$t3Dir/data.txt"
cat >"$t3Dir/flake.nix" <<EOF
{
  description = "Three-way cycling test";
  outputs = { self }: {
    result = builtins.readFile ./data.txt;
  };
}
EOF
git -C "$t3Dir" add .
git -C "$t3Dir" commit -m "Version A"

# Record trace for version A
nix eval "$t3Dir#result" | grepQuiet "version-A"
NIX_ALLOW_EVAL=0 nix eval "$t3Dir#result" | grepQuiet "version-A"

# Record trace for version B
echo "version-B" > "$t3Dir/data.txt"
git -C "$t3Dir" add data.txt
git -C "$t3Dir" commit -m "Version B"
nix eval "$t3Dir#result" | grepQuiet "version-B"

# Record trace for version C
echo "version-C" > "$t3Dir/data.txt"
git -C "$t3Dir" add data.txt
git -C "$t3Dir" commit -m "Version C"
nix eval "$t3Dir#result" | grepQuiet "version-C"

# Revert to version A content
echo "version-A" > "$t3Dir/data.txt"
git -C "$t3Dir" add data.txt
git -C "$t3Dir" commit -m "Back to A"
nix eval "$t3Dir#result" | grepQuiet "version-A"
NIX_ALLOW_EVAL=0 nix eval "$t3Dir#result" | grepQuiet "version-A"

# Switch to version B content
echo "version-B" > "$t3Dir/data.txt"
git -C "$t3Dir" add data.txt
git -C "$t3Dir" commit -m "Back to B"
nix eval "$t3Dir#result" | grepQuiet "version-B"
NIX_ALLOW_EVAL=0 nix eval "$t3Dir#result" | grepQuiet "version-B"

echo "Test 3 passed: three-way version cycling produces correct results"

###############################################################################
# Test 4: Constructive recovery with a stable runtime fetchTree input
# The latest trace is invalidated by a local file change, but constructive
# recovery still needs the SemanticRegistry (pre-populated at session open)
# to resolve the runtime input before it can recompute the older trace's
# fine-grained runtime deps.
###############################################################################

clearStoreIndex

t4RuntimeDir="$TEST_ROOT/recovery-runtime-input"
t4Dir="$TEST_ROOT/recovery-runtime-top"

createGitRepo "$t4RuntimeDir" ""
printf "runtime-data" > "$t4RuntimeDir/data.txt"
git -C "$t4RuntimeDir" add .
git -C "$t4RuntimeDir" commit -m "Init runtime input"
t4Rev=$(git -C "$t4RuntimeDir" rev-parse HEAD)

createGitRepo "$t4Dir" ""
printf "version-1" > "$t4Dir/local.txt"
cat >"$t4Dir/flake.nix" <<EOF
{
  description = "Recovery runtime-input test";
  outputs = { self }: let
    src = builtins.fetchTree {
      type = "git";
      url = "file://$t4RuntimeDir";
      rev = "$t4Rev";
    };
  in {
    result = builtins.readFile ./local.txt + "|" + builtins.readFile (src.outPath + "/data.txt");
  };
}
EOF
git -C "$t4Dir" add .
git -C "$t4Dir" commit -m "Version 1"

nix eval --raw "$t4Dir#result" | grepQuiet "version-1|runtime-data"
NIX_ALLOW_EVAL=0 nix eval --raw "$t4Dir#result" | grepQuiet "version-1|runtime-data"

printf "version-2" > "$t4Dir/local.txt"
git -C "$t4Dir" add local.txt
git -C "$t4Dir" commit -m "Version 2"

nix eval --raw "$t4Dir#result" | grepQuiet "version-2|runtime-data"
NIX_ALLOW_EVAL=0 nix eval --raw "$t4Dir#result" | grepQuiet "version-2|runtime-data"

printf "version-1" > "$t4Dir/local.txt"
git -C "$t4Dir" add local.txt
git -C "$t4Dir" commit -m "Back to version 1"

nix eval --raw "$t4Dir#result" | grepQuiet "version-1|runtime-data"
NIX_ALLOW_EVAL=0 nix eval --raw "$t4Dir#result" | grepQuiet "version-1|runtime-data"

echo "Test 4 passed: version revert with stable runtime input"

###############################################################################
# Test 5: Session invalidation when graph changes but flake identity doesn't
# A depends on B and C, where C follows B. Updating B changes the resolution
# graph (C now resolves to a different B) without changing A's own content.
# The semantic session key includes the graph digest, so the old session
# must NOT be reused.
###############################################################################

clearStoreIndex

t5DirB="$TEST_ROOT/recovery-sess-B"
t5DirC="$TEST_ROOT/recovery-sess-C"
t5Dir="$TEST_ROOT/recovery-sess-A"

# Create B v1
createGitRepo "$t5DirB" ""
cat >"$t5DirB/flake.nix" <<'EOF'
{
  description = "Session B";
  outputs = { self }: { value = "B-v1"; };
}
EOF
git -C "$t5DirB" add .
git -C "$t5DirB" commit -m "B v1"

# Create C (depends on B)
createGitRepo "$t5DirC" ""
cat >"$t5DirC/flake.nix" <<EOF
{
  description = "Session C";
  inputs.b.url = "git+file://$t5DirB";
  outputs = { self, b }: { value = "C-\${b.value}"; };
}
EOF
git -C "$t5DirC" add .
git -C "$t5DirC" commit -m "C init"

# Create A (depends on B and C, C.b follows A.b)
createGitRepo "$t5Dir" ""
cat >"$t5Dir/flake.nix" <<EOF
{
  description = "Session A";
  inputs.b.url = "git+file://$t5DirB";
  inputs.c.url = "git+file://$t5DirC";
  inputs.c.inputs.b.follows = "b";
  outputs = { self, b, c }: { value = "\${b.value}-\${c.value}"; };
}
EOF
git -C "$t5Dir" add .
git -C "$t5Dir" commit -m "A init"

# Lock and cold-record
result5a=$(nix eval --json "$t5Dir#value")
[[ "$result5a" == *"B-v1"* ]]
[[ "$result5a" == *"C-B-v1"* ]]

# Warm-verify
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t5Dir#value")" == "$result5a" ]]

# Update B to v2 — A's content is unchanged, but the graph changes
cat >"$t5DirB/flake.nix" <<'EOF'
{
  description = "Session B v2";
  outputs = { self }: { value = "B-v2"; };
}
EOF
git -C "$t5DirB" add .
git -C "$t5DirB" commit -m "B v2"

# Update A's lock (A's flake.nix is unchanged)
nix flake update b --flake "$t5Dir"
git -C "$t5Dir" add flake.lock
git -C "$t5Dir" commit -m "Update B in lock"

# Must see new values — old session key is invalid due to graph digest change
result5b=$(nix eval --json "$t5Dir#value")
[[ "$result5b" == *"B-v2"* ]]
[[ "$result5b" == *"C-B-v2"* ]]

# Warm-verify the new session
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t5Dir#value")" == "$result5b" ]]

echo "Test 5 passed: session invalidation on graph change with unchanged flake identity"

###############################################################################
# Test 6: Warm-path produces correct output with NIX_ALLOW_EVAL=0
# This verifies the warm path serves cached results without evaluation.
# NIX_ALLOW_EVAL=0 would fail if any fresh evaluation were attempted,
# proving the warm path does no evaluation (and by extension no fetches,
# since evaluation is the only path to fetchToStore in phase 2).
###############################################################################

clearStoreIndex

t6Dir="$TEST_ROOT/recovery-zero-fetch"
createGitRepo "$t6Dir" ""

cat >"$t6Dir/flake.nix" <<'EOF'
{
  description = "Zero-fetch warm path test";
  outputs = { self }: {
    a = builtins.readFile ./data.txt;
    b = builtins.toString (builtins.length (builtins.attrNames { x = 1; y = 2; z = 3; }));
    c = if builtins.pathExists ./optional.nix then "yes" else "no";
  };
}
EOF
echo "test-data" >"$t6Dir/data.txt"
git -C "$t6Dir" add .
git -C "$t6Dir" commit -m "Init"

# Cold-record all three attributes
cold_a=$(nix eval --raw "$t6Dir#a")
cold_b=$(nix eval --raw "$t6Dir#b")
cold_c=$(nix eval --raw "$t6Dir#c")

# Warm-verify all three — NIX_ALLOW_EVAL=0 proves no evaluation occurs.
# Capture NIX_SHOW_STATS to pin the path: primary cache hit, not
# recovery and not History-based bootstrap. A regression that served
# these from scanHistory or from the 3-strategy recovery would pass
# the NIX_ALLOW_EVAL=0 guard but would register nonzero deltas on
# recovery.historyBootstraps or recovery.attempts.
warm_a=$(NIX_SHOW_STATS=1 NIX_SHOW_STATS_PATH="$TEST_HOME/rec-t6-a.json" \
         env NIX_ALLOW_EVAL=0 nix eval --raw "$t6Dir#a")
warm_b=$(NIX_SHOW_STATS=1 NIX_SHOW_STATS_PATH="$TEST_HOME/rec-t6-b.json" \
         env NIX_ALLOW_EVAL=0 nix eval --raw "$t6Dir#b")
warm_c=$(NIX_SHOW_STATS=1 NIX_SHOW_STATS_PATH="$TEST_HOME/rec-t6-c.json" \
         env NIX_ALLOW_EVAL=0 nix eval --raw "$t6Dir#c")

[[ "$cold_a" == "$warm_a" ]]
[[ "$cold_b" == "$warm_b" ]]
[[ "$cold_c" == "$warm_c" ]]

# Pin primary-session hit: every warm call must register at least one
# evalTrace.hits and zero recovery/bootstrap activity. If the warm
# path silently reached the History-bootstrap or 3-strategy recovery
# branches to return these results, that would be a regression —
# the test claims "zero evaluation AND primary-session served."
for id in a b c; do
    hits="$(readEvalTraceCounter "$TEST_HOME/rec-t6-$id.json" evalTrace.hits)"
    attempts="$(readEvalTraceCounter "$TEST_HOME/rec-t6-$id.json" evalTrace.recovery.attempts)"
    bootstraps="$(readEvalTraceCounter "$TEST_HOME/rec-t6-$id.json" evalTrace.recovery.historyBootstraps)"
    if [[ "$hits" -lt 1 || "$attempts" -gt 0 || "$bootstraps" -gt 0 ]]; then
        echo "Test 6 regression on attr $id: primary-session-cache-served-only broken"
        echo "  hits=$hits attempts=$attempts historyBootstraps=$bootstraps"
        cat "$TEST_HOME/rec-t6-$id.json" >&2
        exit 1
    fi
done

echo "Test 6 passed: warm-path zero-eval AND primary-session-served"

###############################################################################
# Test 7: --no-eval-trace-structural-recovery gates the structural-variant
# recovery layer end-to-end via the CLI settings pipeline.
#
# Scenario construction.  Source-identity for --file eval is the absolute
# file path (see computeFileEvalLogicalIdentityHash in session-builders.cc),
# so two --impure evaluations of the same file under different environment
# values share a session key and stack up traces in `History` at the same
# (session_key, rootPath) slot.  The expression conditionally reads a
# second env var only on one branch, so the two cold recordings differ in
# dep structure (one vs. two env deps) and therefore land in distinct
# structural-variant groups.  A third invocation then mutates the env to a
# state where:
#   - primary lookup returns the most-recent trace (two-dep),
#   - verify fails (the two-dep trace's env no longer matches current),
#   - DirectHash recomputes the two-dep set (new hash not in History),
#   - SV scans groups: the single-dep group (Trace 1) recomputes to a
#     hash matching its own recorded fullHash -> HIT.
#
# With the flag off the SV step is gated out, `tryStructVariant` returns
# `nullopt`, recovery reports failure, and the orchestrator falls back to
# fresh evaluation — both branches MUST still produce the correct answer
# (soundness is flag-independent), but the flag changes WHICH path
# produced it.  Counter assertions pin the path.  Without the flag
# actually reaching `tryStructVariant`, the disabled-branch counter check
# would fail.
###############################################################################

t7Src="$TEST_ROOT/recovery-svoff.nix"
cat >"$t7Src" <<'EOF'
let
  x = builtins.getEnv "NIX_SVGATE_X";
in
  if x == "simple"
  then "a-only:simple"
  else "both:" + x + ":" + builtins.getEnv "NIX_SVGATE_Y"
EOF

# Helper: lay down Trace 1 (single-dep, "simple") and Trace 2 (two-dep,
# "complex"/"foo") at the same rootPath slot, then invoke a recovery run
# under `NIX_SVGATE_X=simple` with the caller-supplied extra flags and
# stats path.  The recovery run MUST go through the recovery pipeline
# (DirectHash cannot satisfy it) so that `tryStructVariant` is reached.
runSVGateScenario() {
    local extraFlags="$1"
    local statsPath="$2"

    clearStoreIndex

    # Cold record 1: short-circuit branch, single env dep (NIX_SVGATE_X).
    NIX_SVGATE_X=simple NIX_SVGATE_Y= \
        nix eval --impure -f "$t7Src" --raw >/dev/null

    # Cold record 2: full branch, two env deps. Different struct hash →
    # separate SV group. Overwrites CurrentNode to point at Trace 2;
    # Trace 1 stays in History.
    NIX_SVGATE_X=complex NIX_SVGATE_Y=foo \
        nix eval --impure -f "$t7Src" --raw >/dev/null

    # Recovery probe: back to NIX_SVGATE_X=simple (NIX_SVGATE_Y unset).
    # Verify-Trace-2 fails; DirectHash of the two-dep set under current
    # env matches nothing in History; SV must be the layer that (in the
    # default flag state) produces the match via Trace 1's group.
    local out
    # shellcheck disable=SC2086
    out=$(NIX_SVGATE_X=simple NIX_SVGATE_Y= \
        NIX_SHOW_STATS=1 NIX_SHOW_STATS_PATH="$statsPath" \
        nix eval --impure $extraFlags -f "$t7Src" --raw)
    echo "$out"
}

# Reference value under a completely cache-disabled evaluation.  Used
# only to pin the expected string at NIX_SVGATE_X=simple.
clearStoreIndex
ref=$(NIX_SVGATE_X=simple NIX_SVGATE_Y= \
      nix eval --impure --no-eval-trace -f "$t7Src" --raw)
[[ "$ref" == "a-only:simple" ]]

# Run A: SV ENABLED via the explicit positive-form flag (also exercises
# that --eval-trace-structural-recovery is recognised by the CLI argument
# parser — an unrecognised flag would fail with nonzero exit).
on_out=$(runSVGateScenario "--eval-trace-structural-recovery" \
                           "$TEST_ROOT/rec-t7-on.json")
[[ "$on_out" == "$ref" ]]
on_svhits=$(readEvalTraceCounter "$TEST_ROOT/rec-t7-on.json" evalTrace.recovery.structVariant.hits)
on_attempts=$(readEvalTraceCounter "$TEST_ROOT/rec-t7-on.json" evalTrace.recovery.attempts)
if [[ "$on_svhits" -lt 1 ]]; then
    echo "Test 7 regression (SV enabled): structVariant.hits=$on_svhits, expected >= 1"
    echo "  Recovery was attempted ($on_attempts times) but SV did not fire."
    cat "$TEST_ROOT/rec-t7-on.json" >&2
    exit 1
fi
if [[ "$on_attempts" -lt 1 ]]; then
    echo "Test 7 scenario build broken: recovery.attempts=$on_attempts on the SV-enabled probe"
    echo "  The recovery pipeline was never reached; scenario did not exercise SV."
    cat "$TEST_ROOT/rec-t7-on.json" >&2
    exit 1
fi

# Run B: SV DISABLED via --no-eval-trace-structural-recovery.  Same
# scenario; SV hits must be zero, recovery.failures must be nonzero
# (tryStructVariant returns nullopt after the gate check, which the
# orchestrator reports as a failed recovery).  Fresh evaluation still
# produces the correct answer.
off_out=$(runSVGateScenario "--no-eval-trace-structural-recovery" \
                            "$TEST_ROOT/rec-t7-off.json")
[[ "$off_out" == "$ref" ]]
off_svhits=$(readEvalTraceCounter "$TEST_ROOT/rec-t7-off.json" evalTrace.recovery.structVariant.hits)
off_attempts=$(readEvalTraceCounter "$TEST_ROOT/rec-t7-off.json" evalTrace.recovery.attempts)
off_failures=$(readEvalTraceCounter "$TEST_ROOT/rec-t7-off.json" evalTrace.recovery.failures)
if [[ "$off_svhits" -ne 0 ]]; then
    echo "Test 7 regression (SV disabled): structVariant.hits=$off_svhits, expected 0"
    echo "  The gate did not prevent SV from firing."
    cat "$TEST_ROOT/rec-t7-off.json" >&2
    exit 1
fi
if [[ "$off_attempts" -lt 1 ]]; then
    echo "Test 7 scenario build broken: recovery.attempts=$off_attempts on the SV-disabled probe"
    echo "  The recovery pipeline was never reached; the structVariant.hits==0 check is vacuous."
    cat "$TEST_ROOT/rec-t7-off.json" >&2
    exit 1
fi
if [[ "$off_failures" -lt 1 ]]; then
    echo "Test 7 regression (SV disabled): recovery.failures=$off_failures, expected >= 1"
    echo "  If SV was skipped the recovery should have returned nullopt."
    cat "$TEST_ROOT/rec-t7-off.json" >&2
    exit 1
fi

echo "Test 7 passed: --no-eval-trace-structural-recovery gates the SV path end-to-end"

echo "All eval-trace-recovery tests passed! (BSàlC: constructive traces)"
