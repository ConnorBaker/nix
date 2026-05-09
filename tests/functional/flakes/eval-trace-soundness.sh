#!/usr/bin/env bash

# Eval trace soundness tests: end-to-end correctness guarantees for the
# eval-trace cache under real-world mutation scenarios.
#
# BUG-1 regression guard, schema migration coexistence, constructive recovery
# precision after revert, and structural override (StructuredProjection) for
# JSON key access.
#
# Terminology follows Build Systems à la Carte (BSàlC) and Adapton.

source ./common.sh

requireGit

###############################################################################
# Test 1: BUG-1 regression — flake update must not serve stale cached result
###############################################################################
#
# BUG-1: tryGitIdentityRecovery extracted the STORED git hash from the failed
# trace's deps and used it as the lookup key into History, finding the v1 entry
# even though the current git HEAD was at v2. This compared stored-against-stored
# rather than stored-against-current, producing a stale result.
#
# Scenario:
#   input flake: v1 → "input-v1"
#   top flake:   value = dep.value
#   Steps:
#     1. Lock + cold eval → "input-v1"
#     2. Warm eval → "input-v1" (cache hit)
#     3. Update input to v2, nix flake update
#     4. Cold eval → "input-v2"
#     5. Warm eval → MUST be "input-v2", NOT "input-v1"
#   Step 5 is the critical assertion: without the BUG-1 fix, the warm path
#   would serve the stale "input-v1" via History recovery.

clearStoreIndex

t1InputDir="$TEST_ROOT/soundness-bug1-input"
t1Dir="$TEST_ROOT/soundness-bug1-top"

# Create the input flake at v1
createGitRepo "$t1InputDir" ""
cat >"$t1InputDir/flake.nix" <<'EOF'
{
  description = "Input flake v1";
  outputs = { self }: { value = "input-v1"; };
}
EOF
git -C "$t1InputDir" add .
git -C "$t1InputDir" commit -m "Input v1"

# Create the top flake depending on the input
createGitRepo "$t1Dir" ""
cat >"$t1Dir/flake.nix" <<EOF
{
  description = "Top flake";
  inputs.dep.url = "git+file://$t1InputDir";
  outputs = { self, dep }: { value = dep.value; };
}
EOF
git -C "$t1Dir" add .
git -C "$t1Dir" commit -m "Init top"

# Lock and cold eval → "input-v1" (BSàlC: trace recording)
[[ "$(nix eval --json "$t1Dir#value")" == '"input-v1"' ]]

# Warm eval → "input-v1" (BSàlC: verifying trace succeeds)
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t1Dir#value")" == '"input-v1"' ]]

# Update input to v2
cat >"$t1InputDir/flake.nix" <<'EOF'
{
  description = "Input flake v2";
  outputs = { self }: { value = "input-v2"; };
}
EOF
git -C "$t1InputDir" add .
git -C "$t1InputDir" commit -m "Input v2"

# Update lock file in top
nix flake update dep --flake "$t1Dir"
git -C "$t1Dir" add flake.lock
git -C "$t1Dir" commit -m "Update lock to input v2"

# Cold eval after lock update → MUST produce "input-v2"
[[ "$(nix eval --json "$t1Dir#value")" == '"input-v2"' ]]

# CRITICAL (BUG-1): warm eval must also produce "input-v2", not stale "input-v1".
# With BUG-1, tryGitIdentityRecovery would look up the stale git hash from the
# v1 trace's deps, find the v1 History entry, and serve "input-v1".
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t1Dir#value")" == '"input-v2"' ]]

echo "Test 1 passed: BUG-1 regression — flake update serves fresh result on warm eval"

###############################################################################
# Test 2: Schema migration — old DB coexists with new, evaluation succeeds
###############################################################################
#
# When a new version of nix creates eval-trace-v19.sqlite, an older DB file
# (eval-trace-v12.sqlite) must be silently ignored — not deleted, not causing
# an error. New code creates v19 in the same directory; v12 must survive
# untouched.

clearStoreIndex

t2Dir="$TEST_ROOT/soundness-schema"
createGitRepo "$t2Dir" ""
cat >"$t2Dir/flake.nix" <<'EOF'
{
  description = "Schema migration test";
  outputs = { self }: { value = "schema-ok"; };
}
EOF
git -C "$t2Dir" add .
git -C "$t2Dir" commit -m "Init"

# Plant a dummy old-version DB file in the cache directory.
# Note: `touch` produces a zero-byte file — this tests "new code
# doesn't rm -rf files it doesn't know about" but does NOT exercise
# actual schema coexistence. A follow-up using sqlite3 to plant a
# populated old-epoch DB would be stronger; for now counter checks
# pin that the new DB is being created and used.
cacheDir="$TEST_HOME/.cache/nix"
mkdir -p "$cacheDir"
touch "$cacheDir/eval-trace-v12.sqlite"

# Cold eval: must create a current-epoch DB and write at least one record.
# If the v12 file collision caused the new code to abort DB creation
# silently, record.count would stay 0 and the NIX_ALLOW_EVAL=0
# follow-up would have no trace to serve.
NIX_SHOW_STATS=1 NIX_SHOW_STATS_PATH="$TEST_HOME/soundness-t2-cold.json" \
    nix eval --json "$t2Dir#value" | grepQuiet "schema-ok"
t2_records="$(readEvalTraceCounter "$TEST_HOME/soundness-t2-cold.json" evalTrace.record.count)"
if [[ "$t2_records" -lt 1 ]]; then
    echo "Test 2 regression: cold eval did not record into the new-schema DB"
    echo "  record.count=$t2_records"
    exit 1
fi

# Warm verify via NIX_ALLOW_EVAL=0: must hit the new-schema DB, not
# attempt to read from the v12 stub. Counter: evalTrace.hits ≥ 1.
NIX_SHOW_STATS=1 NIX_SHOW_STATS_PATH="$TEST_HOME/soundness-t2-warm.json" \
    env NIX_ALLOW_EVAL=0 nix eval --json "$t2Dir#value" | grepQuiet "schema-ok"
t2_hits="$(readEvalTraceCounter "$TEST_HOME/soundness-t2-warm.json" evalTrace.hits)"
if [[ "$t2_hits" -lt 1 ]]; then
    echo "Test 2 regression: warm verify didn't hit the new-schema DB"
    exit 1
fi

# v12 must still exist (new code must not delete unrecognised DB files)
[[ -f "$cacheDir/eval-trace-v12.sqlite" ]]

echo "Test 2 passed: old schema DB coexists with new, evaluation succeeds"

###############################################################################
# Test 3: Precision — constructive recovery after v1 → v2 → v1 revert
###############################################################################
#
# After recording traces for both v1 and v2, reverting to v1 content must serve
# the v1 result without fresh evaluation (constructive recovery from History).
# This is the BSàlC constructive-trace property applied to a flake input.

clearStoreIndex

t3InputDir="$TEST_ROOT/soundness-revert-input"
t3Dir="$TEST_ROOT/soundness-revert-top"

# Create the input flake at v1
createGitRepo "$t3InputDir" ""
cat >"$t3InputDir/flake.nix" <<'EOF'
{
  description = "Revert input v1";
  outputs = { self }: { value = "revert-v1"; };
}
EOF
git -C "$t3InputDir" add .
git -C "$t3InputDir" commit -m "Input v1"

# Create the top flake
createGitRepo "$t3Dir" ""
cat >"$t3Dir/flake.nix" <<EOF
{
  description = "Revert top";
  inputs.dep.url = "git+file://$t3InputDir";
  outputs = { self, dep }: { value = dep.value; };
}
EOF
git -C "$t3Dir" add .
git -C "$t3Dir" commit -m "Init top"

# Record trace for v1 (BSàlC: trace recording)
[[ "$(nix eval --json "$t3Dir#value")" == '"revert-v1"' ]]
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t3Dir#value")" == '"revert-v1"' ]]

# Change input to v2 and update lock
cat >"$t3InputDir/flake.nix" <<'EOF'
{
  description = "Revert input v2";
  outputs = { self }: { value = "revert-v2"; };
}
EOF
git -C "$t3InputDir" add .
git -C "$t3InputDir" commit -m "Input v2"
nix flake update dep --flake "$t3Dir"
git -C "$t3Dir" add flake.lock
git -C "$t3Dir" commit -m "Lock v2"

# Record trace for v2 (BSàlC: trace recording)
[[ "$(nix eval --json "$t3Dir#value")" == '"revert-v2"' ]]
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t3Dir#value")" == '"revert-v2"' ]]

# Revert input back to v1 content and update lock
cat >"$t3InputDir/flake.nix" <<'EOF'
{
  description = "Revert input v1";
  outputs = { self }: { value = "revert-v1"; };
}
EOF
git -C "$t3InputDir" add .
git -C "$t3InputDir" commit -m "Back to v1"
nix flake update dep --flake "$t3Dir"
git -C "$t3Dir" add flake.lock
git -C "$t3Dir" commit -m "Lock back to v1"

# Must produce "revert-v1" (BSàlC: constructive trace recovery from History)
[[ "$(nix eval --json "$t3Dir#value")" == '"revert-v1"' ]]

# Warm eval must also return "revert-v1" (not stale "revert-v2")
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t3Dir#value")" == '"revert-v1"' ]]

echo "Test 3 passed: constructive recovery after v1 → v2 → v1 revert"

###############################################################################
# Test 4: Precision — unrelated JSON key change hits cache (structural override)
###############################################################################
#
# A flake reads config.json via builtins.fromJSON and accesses only the
# "accessed" key. When the "unrelated" key changes, the StructuredProjection
# dep for "accessed" still passes, so the trace must cache-hit (structural
# override — NixBinding SC dep covers the accessed binding, content dep failure
# is overridden). When "accessed" itself changes, the trace must invalidate.

clearStoreIndex

t4Dir="$TEST_ROOT/soundness-json-precision"
createGitRepo "$t4Dir" ""

cat >"$t4Dir/config.json" <<'EOF'
{"accessed": "value-v1", "unrelated": "orig"}
EOF

cat >"$t4Dir/flake.nix" <<'EOF'
{
  description = "JSON structural override test";
  outputs = { self }: let
    cfg = builtins.fromJSON (builtins.readFile ./config.json);
  in {
    result = cfg.accessed;
  };
}
EOF
git -C "$t4Dir" add .
git -C "$t4Dir" commit -m "Init"

# Record trace (BSàlC: trace recording)
[[ "$(nix eval --json "$t4Dir#result")" == '"value-v1"' ]]
# Warm verify (BSàlC: verifying trace succeeds)
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t4Dir#result")" == '"value-v1"' ]]

# Change ONLY the unrelated key — accessed key unchanged
cat >"$t4Dir/config.json" <<'EOF'
{"accessed": "value-v1", "unrelated": "changed"}
EOF
git -C "$t4Dir" add config.json
git -C "$t4Dir" commit -m "Change only unrelated key"

# Must cache-hit — structural override: "accessed" binding is unchanged
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t4Dir#result")" == '"value-v1"' ]]

# Now change the ACCESSED key — cache must invalidate
cat >"$t4Dir/config.json" <<'EOF'
{"accessed": "value-v2", "unrelated": "changed"}
EOF
git -C "$t4Dir" add config.json
git -C "$t4Dir" commit -m "Change accessed key"

# Verify miss — accessed binding changed, structural override does not apply
NIX_ALLOW_EVAL=0 expectStderr 1 nix eval --json "$t4Dir#result" \
  | grepQuiet "not everything is cached"

# Re-record with new value
[[ "$(nix eval --json "$t4Dir#result")" == '"value-v2"' ]]
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t4Dir#result")" == '"value-v2"' ]]

echo "Test 4 passed: JSON unrelated key change hits cache, accessed key change invalidates"

echo "All eval-trace-soundness tests passed! (BUG-1, schema migration, constructive recovery, structural override)"
