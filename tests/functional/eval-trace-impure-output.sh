#!/usr/bin/env bash

# Impure eval trace output tests: nix eval cursor verification, trace
# invalidation, --write-to from traced results, --json traced result replay
# for drv/list/outPath, constructive recovery via modify-then-revert,
# three-way cycling constructive recovery, Phase 2/3 constructive recovery.
# Terminology follows Build Systems à la Carte (BSàlC), Adapton, and Shake.

source common.sh

clearStoreIndex() {
    rm -rf "$TEST_HOME/.cache/nix/eval-trace"* "$TEST_HOME/.cache/nix/attr-vocab.sqlite"* "$TEST_HOME/.cache/nix/stat-hash-cache"*
}

testDir="$TEST_ROOT/eval-trace-impure"
mkdir -p "$testDir"

cp "${config_nix}" "$testDir/config.nix"

###############################################################################
# Test 1: nix eval --file trace recording and cursor-based verification
###############################################################################
clearStoreIndex

cat >"$testDir/test-eval-cursor.nix" <<'EOF'
{
  myString = "hello from eval";
  myBool = false;
  myInt = 99;
  myPath = ./test-eval-cursor.nix;
}
EOF

# Fresh evaluation — records traces via cursor-based evaluation (BSàlC: trace recording)
nix eval --impure -f "$testDir/test-eval-cursor.nix" myString | grepQuiet "hello from eval"
[[ "$(nix eval --json --impure -f "$testDir/test-eval-cursor.nix" myBool)" == 'false' ]]
[[ "$(nix eval --json --impure -f "$testDir/test-eval-cursor.nix" myInt)" == '99' ]]
nix eval --raw --impure -f "$testDir/test-eval-cursor.nix" myPath | grepQuiet "test-eval-cursor.nix"

# Verification hits — traced results served without re-evaluation (BSàlC: verifying trace succeeds)
NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/test-eval-cursor.nix" myString | grepQuiet "hello from eval"
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-eval-cursor.nix" myBool)" == 'false' ]]
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-eval-cursor.nix" myInt)" == '99' ]]

echo "Test 1 passed: nix eval --file trace recording + cursor verification"

###############################################################################
# Test 2: nix eval --file — trace verification fails on file modification
###############################################################################
clearStoreIndex

echo "dep-data-v1" > "$testDir/dep-data.txt"

cat >"$testDir/test-eval-dep.nix" <<'EOF'
{
  depVal = builtins.readFile ./dep-data.txt;
  constVal = "constant";
}
EOF

# Record traces for both attributes
nix eval --impure -f "$testDir/test-eval-dep.nix" depVal | grepQuiet "dep-data-v1"
nix eval --impure -f "$testDir/test-eval-dep.nix" constVal | grepQuiet "constant"

# Verification hits — traced results valid
NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/test-eval-dep.nix" depVal | grepQuiet "dep-data-v1"
NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/test-eval-dep.nix" constVal | grepQuiet "constant"

# Modify dep-data.txt — dirties depVal's trace dependency
echo "dep-data-v2" > "$testDir/dep-data.txt"

# depVal: verify miss — trace verification fails (content dependency changed)
NIX_ALLOW_EVAL=0 expectStderr 1 nix eval --impure -f "$testDir/test-eval-dep.nix" depVal \
  | grepQuiet "not everything is cached"

# constVal: verification hit — its trace has no dependency on dep-data.txt (Adapton: selective dirtying)
NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/test-eval-dep.nix" constVal | grepQuiet "constant"

# Fresh evaluation re-records depVal's trace
nix eval --impure -f "$testDir/test-eval-dep.nix" depVal | grepQuiet "dep-data-v2"

# Verification hit — new trace valid
NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/test-eval-dep.nix" depVal | grepQuiet "dep-data-v2"

echo "Test 2 passed: nix eval --file trace verification fails on modification"

###############################################################################
# Test 3: --write-to from traced results via --file cursor path
###############################################################################
clearStoreIndex

cat >"$testDir/test-write-to.nix" <<'EOF'
{
  fileData = {
    greeting = "hello from file";
    farewell = "goodbye from file";
  };
}
EOF

# Record trace (BSàlC: trace recording)
nix eval --impure -f "$testDir/test-write-to.nix" fileData --json > /dev/null

# Use --write-to from traced result (verification hit)
writeOutDir="$testDir/write-to-out"
nix eval --impure -f "$testDir/test-write-to.nix" fileData --write-to "$writeOutDir"
[[ "$(cat "$writeOutDir/greeting")" == "hello from file" ]]
[[ "$(cat "$writeOutDir/farewell")" == "goodbye from file" ]]

rm -rf "$writeOutDir"

# Use --write-to entirely from traced results (no evaluation permitted)
NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/test-write-to.nix" fileData --write-to "$writeOutDir"
[[ "$(cat "$writeOutDir/greeting")" == "hello from file" ]]
[[ "$(cat "$writeOutDir/farewell")" == "goodbye from file" ]]

rm -rf "$writeOutDir"

echo "Test 3 passed: --write-to from traced results"

###############################################################################
# Test 4: Derivation trace recording — --json replays traced store path (impure)
###############################################################################
clearStoreIndex

cat >"$testDir/test-json-drv.nix" <<'EOF'
let inherit (import ./config.nix) mkDerivation; in
{
  drv = mkDerivation {
    name = "json-drv-impure";
    buildCommand = "echo hello > $out";
  };
}
EOF

# Fresh evaluation — traces derivation, --json returns store path
drvJson="$(nix eval --json --impure -f "$testDir/test-json-drv.nix" drv)"
[[ "$drvJson" == '"'* ]]  # Must be a JSON string

# Verification hit — traced derivation result replayed without evaluation
drvJsonCached="$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-json-drv.nix" drv)"
[[ "$drvJsonCached" == "$drvJson" ]]

echo "Test 4 passed: derivation trace recording and verification (impure)"

###############################################################################
# Test 5: List of strings trace recording (ListOfStrings traced result type)
###############################################################################
clearStoreIndex

cat >"$testDir/test-json-list.nix" <<'EOF'
{
  myList = [ "a" "b" "c" ];
}
EOF

# Record trace with list result
listJson="$(nix eval --json --impure -f "$testDir/test-json-list.nix" myList)"
[[ "$listJson" == '["a","b","c"]' ]]

# Re-evaluation verifies trace correctness
listJson2="$(nix eval --json --impure -f "$testDir/test-json-list.nix" myList)"
[[ "$listJson2" == '["a","b","c"]' ]]

# Verification hit — traced list served without evaluation
listJson3="$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-json-list.nix" myList)"
[[ "$listJson3" == '["a","b","c"]' ]]

echo "Test 5 passed: list of strings trace recording and replay"

###############################################################################
# Test 6: Non-derivation outPath trace recording and replay
###############################################################################
clearStoreIndex

cat >"$testDir/test-json-outpath.nix" <<'EOF'
{
  withOutPath = { outPath = "/nix/store/fake-path"; foo = 42; };
}
EOF

# Fresh evaluation — traces outPath as recorded result
outPathJson="$(nix eval --json --impure -f "$testDir/test-json-outpath.nix" withOutPath)"
[[ "$outPathJson" == '"/nix/store/fake-path"' ]]

# Verification hit — traced outPath replayed without evaluation
outPathJson2="$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-json-outpath.nix" withOutPath)"
[[ "$outPathJson2" == '"/nix/store/fake-path"' ]]

echo "Test 6 passed: non-derivation outPath trace replay"

###############################################################################
# Test 7: Modify-then-revert constructive recovery (BSàlC: constructive trace)
###############################################################################
clearStoreIndex

echo '"original"' > "$testDir/revert-dep.txt"

cat >"$testDir/test-revert.nix" <<'EOF'
{
  result = builtins.readFile ./revert-dep.txt;
}
EOF

# Record trace for "original" (BSàlC: trace recording)
[[ "$(nix eval --json --impure -f "$testDir/test-revert.nix" result)" == '"\"original\"\n"' ]]

# Verification hit — traced result valid
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-revert.nix" result)" == '"\"original\"\n"' ]]

# Modify the dependency — dirties the trace
echo '"modified"' > "$testDir/revert-dep.txt"

# Verify miss — trace verification fails (dependency changed)
NIX_ALLOW_EVAL=0 expectStderr 1 nix eval --json --impure -f "$testDir/test-revert.nix" result \
  | grepQuiet "not everything is cached"

# Fresh evaluation records trace for "modified"
[[ "$(nix eval --json --impure -f "$testDir/test-revert.nix" result)" == '"\"modified\"\n"' ]]

# Verification hit — "modified" trace valid
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-revert.nix" result)" == '"\"modified\"\n"' ]]

# Revert the dependency back to original
echo '"original"' > "$testDir/revert-dep.txt"

# Constructive recovery — "original" traced result found via matching dependency fingerprint (BSàlC: constructive trace)
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-revert.nix" result)" == '"\"original\"\n"' ]]

echo "Test 7 passed: modify-then-revert constructive recovery"

###############################################################################
# Test 8: Three-way version cycling — constructive recovery for all versions
###############################################################################
clearStoreIndex

echo '"v1"' > "$testDir/cycle-dep.txt"

cat >"$testDir/test-cycle.nix" <<'EOF'
{
  result = builtins.readFile ./cycle-dep.txt;
}
EOF

# Record trace for v1
nix eval --json --impure -f "$testDir/test-cycle.nix" result
NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-cycle.nix" result

# Record trace for v2
echo '"v2"' > "$testDir/cycle-dep.txt"
nix eval --json --impure -f "$testDir/test-cycle.nix" result

# Record trace for v3
echo '"v3"' > "$testDir/cycle-dep.txt"
nix eval --json --impure -f "$testDir/test-cycle.nix" result

# Revert to v1 — constructive recovery from previously recorded trace
echo '"v1"' > "$testDir/cycle-dep.txt"
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-cycle.nix" result)" == '"\"v1\"\n"' ]]

# Switch to v2 — also constructively recovered from recorded trace
echo '"v2"' > "$testDir/cycle-dep.txt"
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-cycle.nix" result)" == '"\"v2\"\n"' ]]

echo "Test 8 passed: three-way version cycling constructive recovery"

###############################################################################
# Test 9: Multi-attr constructive recovery across multiple attributes
###############################################################################
clearStoreIndex

echo '"version-A"' > "$testDir/hash-recovery-dep.txt"

cat >"$testDir/test-hash-recovery.nix" <<'EOF'
let data = builtins.readFile ./hash-recovery-dep.txt; in
{
  attr1 = "first-${data}";
  attr2 = "second-${data}";
  attr3 = "third-${data}";
}
EOF

# Record traces for all attributes (BSàlC: trace recording)
nix eval --json --impure -f "$testDir/test-hash-recovery.nix" attr1
nix eval --json --impure -f "$testDir/test-hash-recovery.nix" attr2
nix eval --json --impure -f "$testDir/test-hash-recovery.nix" attr3

# Verification hits — all traced results valid
NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-hash-recovery.nix" attr1
NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-hash-recovery.nix" attr2
NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-hash-recovery.nix" attr3

# Modify file -> fresh evaluation records new traces
echo '"version-B"' > "$testDir/hash-recovery-dep.txt"
nix eval --json --impure -f "$testDir/test-hash-recovery.nix" attr1
nix eval --json --impure -f "$testDir/test-hash-recovery.nix" attr2
nix eval --json --impure -f "$testDir/test-hash-recovery.nix" attr3

# Revert to version-A -> constructive recovery finds original traced results
echo '"version-A"' > "$testDir/hash-recovery-dep.txt"

# All attrs constructively recovered (no fresh evaluation needed)
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-hash-recovery.nix" attr1)" == '"first-\"version-A\"\n"' ]]
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-hash-recovery.nix" attr2)" == '"second-\"version-A\"\n"' ]]
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-hash-recovery.nix" attr3)" == '"third-\"version-A\"\n"' ]]

echo "Test 9 passed: multi-attr constructive recovery"

###############################################################################
# Test 10: Three-way cycling constructive recovery with single attribute
###############################################################################
clearStoreIndex

cat >"$testDir/diff-dep.txt" <<'EOF'
version-A
EOF

cat >"$testDir/test-diff-recovery.nix" <<'EOF'
{
  result = builtins.readFile ./diff-dep.txt;
}
EOF

# Version A: fresh evaluation records trace
result_a1="$(nix eval --json --impure -f "$testDir/test-diff-recovery.nix" result)"
[[ "$result_a1" == '"version-A\n"' ]]

# Version A: verification hit
result_a2="$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-diff-recovery.nix" result)"
[[ "$result_a2" == '"version-A\n"' ]]

# Version B: fresh evaluation records trace
echo "version-B" > "$testDir/diff-dep.txt"
result_b="$(nix eval --json --impure -f "$testDir/test-diff-recovery.nix" result)"
[[ "$result_b" == '"version-B\n"' ]]

# Version C: fresh evaluation records trace
echo "version-C" > "$testDir/diff-dep.txt"
result_c="$(nix eval --json --impure -f "$testDir/test-diff-recovery.nix" result)"
[[ "$result_c" == '"version-C\n"' ]]

# Revert to version A: constructive recovery finds original traced result
echo "version-A" > "$testDir/diff-dep.txt"
result_recover="$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-diff-recovery.nix" result)"
[[ "$result_recover" == '"version-A\n"' ]]

# Also verify version B can be constructively recovered
echo "version-B" > "$testDir/diff-dep.txt"
result_recover_b="$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-diff-recovery.nix" result)"
[[ "$result_recover_b" == '"version-B\n"' ]]

echo "Test 10 passed: three-way cycling constructive recovery"

###############################################################################
# Test 11: Phase 2 constructive recovery — dep-less child with parent identity
# Parent changes value, child has no direct trace dependencies but its value
# changes. Phase 1 (direct hash recovery) is SKIPPED for dep-less children
# with parent hint (hash([]) is ambiguous across all dep-less nodes).
# Phase 2 uses parent's Merkle identity hash to discriminate
# (BSàlC: constructive trace with structural key).
###############################################################################
clearStoreIndex

echo "v1" > "$testDir/parent-dep.txt"

cat >"$testDir/test-phase2.nix" <<'EOF'
let
  parentData = builtins.readFile ./parent-dep.txt;
in {
  child = parentData;
}
EOF

# Fresh evaluation: record trace for v1
[[ "$(nix eval --json --impure -f "$testDir/test-phase2.nix" child)" == '"v1\n"' ]]

# Verification hit — traced result valid
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-phase2.nix" child)" == '"v1\n"' ]]

# Switch to v2 (different size to avoid stat-hash-cache false positives)
echo "version2" > "$testDir/parent-dep.txt"

# Fresh evaluation: record trace for v2
[[ "$(nix eval --json --impure -f "$testDir/test-phase2.nix" child)" == '"version2\n"' ]]

# Verification hit — traced result valid
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-phase2.nix" child)" == '"version2\n"' ]]

# Revert to v1 — Phase 2 constructive recovery finds child="v1\n" via parent's Merkle identity
echo "v1" > "$testDir/parent-dep.txt"

[[ "$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-phase2.nix" child)" == '"v1\n"' ]]

echo "Test 11 passed: Phase 2 constructive recovery (dep-less child with parent identity)"

###############################################################################
# Test 12: Phase 3 constructive recovery — structural variant scanning
# Attribute reads different files depending on mode. Two recorded traces have
# different dependency structures. When current trace verification fails,
# Phase 3 scans structural equivalence classes and finds the trace version
# with matching structure (BSàlC: constructive trace with structural matching).
###############################################################################
clearStoreIndex

echo "simple" > "$testDir/p3-mode.txt"
echo "base-data" > "$testDir/p3-base.txt"
echo "extra-data" > "$testDir/p3-extra.txt"

cat >"$testDir/test-phase3.nix" <<'NIXEOF'
let
  mode = builtins.readFile ./p3-mode.txt;
in {
  result = if mode == "simple\n"
    then builtins.readFile ./p3-base.txt
    else builtins.readFile ./p3-base.txt + builtins.readFile ./p3-extra.txt;
}
NIXEOF

# Record trace v1: mode="simple" -> reads only base
# Trace structure: [Content(test-phase3.nix), Content(p3-mode.txt), Content(p3-base.txt)]
[[ "$(nix eval --json --impure -f "$testDir/test-phase3.nix" result)" == '"base-data\n"' ]]

# Verification hit — traced result valid
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-phase3.nix" result)" == '"base-data\n"' ]]

# Switch to complex mode -> reads base + extra
# Trace structure: [Content(test-phase3.nix), Content(p3-mode.txt), Content(p3-base.txt), Content(p3-extra.txt)]
echo "complex" > "$testDir/p3-mode.txt"

# Record trace v2 (fresh evaluation after verify miss)
[[ "$(nix eval --json --impure -f "$testDir/test-phase3.nix" result)" == '"base-data\nextra-data\n"' ]]

# Verification hit — traced result valid
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-phase3.nix" result)" == '"base-data\nextra-data\n"' ]]

# Now modify extra AND revert mode to simple:
# - Current trace has v2's structure [test.nix, mode, base, extra]
# - mode.txt changed ("complex" -> "simple") -> v2's trace verification fails
# - Phase 1: recompute v2's structure with new hashes -> no matching trace
# - Phase 3: scan structural equivalence classes, find v1's class [test.nix, mode, base]
#   All of v1's trace dependencies validate (test.nix unchanged, mode="simple" matches v1, base unchanged)
#   -> constructive recovery yields v1's recorded result
echo "simple" > "$testDir/p3-mode.txt"
echo "extra-data-modified!!" > "$testDir/p3-extra.txt"

[[ "$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-phase3.nix" result)" == '"base-data\n"' ]]

echo "Test 12 passed: Phase 3 constructive recovery (structural equivalence class scanning)"

###############################################################################
# Test 13: Stat-hash-cache — same-size file modification detected
# Verifies that modifying a file to same-length content is detected by the
# stat-hash-cache (nanosecond mtime or inode change), preventing stale
# traced results from passing verification.
###############################################################################
clearStoreIndex

echo "AAAA" > "$testDir/stat-dep.txt"

cat >"$testDir/test-stat-cache.nix" <<'EOF'
{
  result = builtins.readFile ./stat-dep.txt;
}
EOF

# Fresh evaluation: record trace for "AAAA"
[[ "$(nix eval --json --impure -f "$testDir/test-stat-cache.nix" result)" == '"AAAA\n"' ]]

# Verification hit — traced result valid
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-stat-cache.nix" result)" == '"AAAA\n"' ]]

# Modify to same-size content (4 chars + newline = 5 bytes, same as original)
echo "BBBB" > "$testDir/stat-dep.txt"

# Should detect the change (stat-hash-cache uses nanosecond mtime)
# If stat-hash-cache is broken, this would incorrectly pass trace verification
NIX_ALLOW_EVAL=0 expectStderr 1 nix eval --json --impure -f "$testDir/test-stat-cache.nix" result \
  | grepQuiet "not everything is cached"

# Fresh evaluation records new trace for "BBBB"
[[ "$(nix eval --json --impure -f "$testDir/test-stat-cache.nix" result)" == '"BBBB\n"' ]]

# Verification hit — traced result valid
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-stat-cache.nix" result)" == '"BBBB\n"' ]]

echo "Test 13 passed: stat-hash-cache detects same-size modification for trace verification"

echo "All eval-trace-impure-output tests passed! (BSàlC: verifying traces, constructive traces)"
