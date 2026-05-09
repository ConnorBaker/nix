#!/usr/bin/env bash

# End-to-end integration smoke test for `nix eval-info`. Deeper coverage of
# the TraceStore::queryEvalInfoExclusive read path lives in the unit tests at
# src/libexpr-tests/eval-trace/store/eval-info.cc. This file only exercises
# things that can only be observed at the CLI level: flag plumbing, attr-path
# tokenization on the command line, text output format, and the
# --include-history → allowHistoryFallback wiring.

source common.sh

testDir="$TEST_ROOT/eval-trace-info"
mkdir -p "$testDir"

###############################################################################
# Test 1: Populate the cache via `nix eval`, query via `nix eval-info`.
# Covers:
#   - JSON envelope fully present (all required fields)
#   - cached=true, source=session, failure=null, hasVolatileDeps=false
#   - traceId and resultId > 0; traceHash is 64 hex chars
#   - At least one fileBytes dep whose resolved key IS the data.txt file
###############################################################################
clearStoreIndex

cat >"$testDir/t1.nix" <<'EOF'
{ out = builtins.readFile ./data.txt; }
EOF
echo "payload" >"$testDir/data.txt"

nix eval --impure -f "$testDir/t1.nix" out > /dev/null

info_json="$testDir/t1.json"
nix eval-info --json --impure -f "$testDir/t1.nix" out > "$info_json"

[[ "$(jq -r .version "$info_json")" == "1" ]]         || { echo "T1 wrong version"; exit 1; }
[[ "$(jq -r .cached "$info_json")" == "true" ]]       || { echo "T1 expected cache hit"; exit 1; }
[[ "$(jq -r .source "$info_json")" == "session" ]]    || { echo "T1 expected source=session"; exit 1; }
[[ "$(jq -r .failure "$info_json")" == "null" ]]      || { echo "T1 failure should be null"; exit 1; }
[[ "$(jq -r .hasVolatileDeps "$info_json")" == "false" ]] || { echo "T1 hasVolatileDeps should be false"; exit 1; }
[[ "$(jq -r .traceId "$info_json")" -gt 0 ]]          || { echo "T1 traceId not positive"; exit 1; }
[[ "$(jq -r .resultId "$info_json")" -gt 0 ]]         || { echo "T1 resultId not positive"; exit 1; }
[[ "$(jq -r '.traceHash | length' "$info_json")" == "64" ]] \
  || { echo "T1 traceHash wrong length"; exit 1; }
[[ "$(jq -r '.sessionKey | length' "$info_json")" == "64" ]] \
  || { echo "T1 sessionKey wrong length"; exit 1; }
[[ "$(jq -r '.deps | type' "$info_json")" == "array" ]] \
  || { echo "T1 deps must be array"; exit 1; }
[[ "$(jq -r '.runtimeRoots | type' "$info_json")" == "array" ]] \
  || { echo "T1 runtimeRoots must be array"; exit 1; }

# Resolved key must exactly end with the data.txt basename, not substring-match.
fb_count="$(jq '[.deps[] | select(.type == "fileBytes" and (.key | endswith("/data.txt")))] | length' "$info_json")"
[[ "$fb_count" -ge 1 ]] || { echo "T1 missing fileBytes dep on data.txt"; cat "$info_json"; exit 1; }

# resultKind must match the CachedResult variant. `out = builtins.readFile`
# stores a String.  A regression in cachedResultKindName's variant→label
# dispatch (e.g., swapping two cases) would surface here but not in the unit
# tests, which only assert on the variant itself.
[[ "$(jq -r .resultKind "$info_json")" == "String" ]] \
  || { echo "T1 expected resultKind=String, got '$(jq -r .resultKind "$info_json")'"; exit 1; }
# value.kind mirrors resultKind for structured variants.
[[ "$(jq -r '.value.kind' "$info_json")" == "String" ]] \
  || { echo "T1 expected value.kind=String"; exit 1; }

echo "T1 passed: JSON envelope, fileBytes dep, resultKind all verified"

###############################################################################
# Test 2: Cache miss on an attr that was never evaluated.
###############################################################################
clearStoreIndex

cat >"$testDir/t2.nix" <<'EOF'
{ seen = 1; unseen = 2; }
EOF
nix eval --impure -f "$testDir/t2.nix" seen > /dev/null

miss_json="$testDir/t2-miss.json"
nix eval-info --json --impure -f "$testDir/t2.nix" unseen > "$miss_json"
[[ "$(jq -r .cached "$miss_json")" == "false" ]] \
  || { echo "T2 expected cache miss for unseen attr"; exit 1; }
[[ "$(jq -r .source "$miss_json")" == "none" ]] \
  || { echo "T2 expected source=none on miss"; exit 1; }

echo "T2 passed: CLI correctly surfaces cache miss"

###############################################################################
# Test 3: --no-eval-trace refuses (usage error, non-zero exit, helpful text).
###############################################################################
clearStoreIndex
cat >"$testDir/t3.nix" <<'EOF'
{ v = 3; }
EOF

if nix eval-info --no-eval-trace --impure -f "$testDir/t3.nix" v 2>"$testDir/t3.stderr"; then
    echo "T3 expected non-zero exit"; exit 1
fi
grepQuiet "eval-trace is disabled" "$testDir/t3.stderr" \
  || { echo "T3 stderr missing expected hint"; cat "$testDir/t3.stderr"; exit 1; }

echo "T3 passed: --no-eval-trace produces a clear usage error"

###############################################################################
# Test 4: Text output format (non-JSON) has the expected structure.
# Covers: column headers `attr path:`, `session key:`, `cached:`,
#         `trace hash:`, `result:`, `dependencies:`, `runtime fetch roots:`.
# Also: the text format differs between cache-hit and cache-miss cases.
###############################################################################
clearStoreIndex
cat >"$testDir/t4.nix" <<'EOF'
{ v = 42; }
EOF
nix eval --impure -f "$testDir/t4.nix" v > /dev/null

hit_txt="$(nix eval-info --impure -f "$testDir/t4.nix" v 2>/dev/null)"
for header in "^attr path:" "^session key:" "^cached:" "^trace hash:" \
              "^result:" "^dependencies:" "^runtime fetch roots:"; do
    echo "$hit_txt" | grep -q "$header" \
      || { echo "T4 missing header '$header' in text output"; echo "$hit_txt"; exit 1; }
done
# Cache-hit text must say "yes", not "no".
echo "$hit_txt" | grep -q "^cached:.*yes" \
  || { echo "T4 cache-hit text must include 'cached:.*yes'"; exit 1; }
# Int value must render in the result line (printValue visitor dispatch).
echo "$hit_txt" | grep -q "^result:.*Int 42" \
  || { echo "T4 expected 'result: Int 42' line"; echo "$hit_txt"; exit 1; }

# Miss text differs: no trace hash, no result, no dependencies lines.
miss_txt="$(nix eval-info --impure -f "$testDir/t4.nix" missing 2>/dev/null)"
echo "$miss_txt" | grep -q "^cached:.*no" \
  || { echo "T4 cache-miss text must say 'cached:.*no'"; exit 1; }
echo "$miss_txt" | grep -q "No cached trace" \
  || { echo "T4 cache-miss text should explain the miss"; exit 1; }
# A miss should NOT print a trace hash or result line.
echo "$miss_txt" | grep -q "^trace hash:" \
  && { echo "T4 cache-miss text should not print 'trace hash:'"; exit 1; }
echo "$miss_txt" | grep -q "^result:" \
  && { echo "T4 cache-miss text should not print 'result:'"; exit 1; }

echo "T4 passed: text output format verified for hit + miss cases"

###############################################################################
# Test 5: --include-history flag is plumbed through to queryEvalInfoExclusive.
# On a fresh hit, both with and without --include-history should source=session
# (session layer always preferred). We cannot construct a "history-only" hit
# without driving two distinct session keys, which belongs to the unit tests.
###############################################################################
clearStoreIndex
cat >"$testDir/t5.nix" <<'EOF'
{ v = 5; }
EOF
nix eval --impure -f "$testDir/t5.nix" v > /dev/null

plain="$testDir/t5-plain.json"
with_hist="$testDir/t5-hist.json"
nix eval-info --json                  --impure -f "$testDir/t5.nix" v > "$plain"
nix eval-info --json --include-history --impure -f "$testDir/t5.nix" v > "$with_hist"

[[ "$(jq -r .source "$plain")"      == "session" ]] \
  || { echo "T5 plain expected source=session"; exit 1; }
[[ "$(jq -r .source "$with_hist")"  == "session" ]] \
  || { echo "T5 --include-history should still prefer session on a fresh hit"; exit 1; }
# Same attr, same flags apart from --include-history ⇒ same sessionKey.
sk1="$(jq -r .sessionKey "$plain")"
sk2="$(jq -r .sessionKey "$with_hist")"
[[ "$sk1" == "$sk2" ]] || { echo "T5 sessionKey must match across invocations"; exit 1; }

echo "T5 passed: --include-history plumbed (session preferred)"

echo ""
echo "All eval-info integration smoke tests passed."
