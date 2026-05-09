# Functional Tests

## Eval-Trace Functional Tests

Shell-based end-to-end tests for the eval-trace cache subsystem. These tests
run `nix eval` against real files and flakes and verify cache behavior using
`NIX_ALLOW_EVAL=0` guards, stderr assertions, and output comparison.

For unit test documentation, see `src/libexpr-tests/eval-trace/CLAUDE.md`.

### Test Files

```
tests/functional/
  eval-trace-impure-core.sh        File-based trace basics (7 tests)
  eval-trace-impure-deps.sh        Oracle deps: env, system, time, paths (12 tests)
  eval-trace-impure-advanced.sh    Nested traces, shared deps, FullAttrs (7 tests)
  eval-trace-impure-output.sh      Scalar types, derivations, constructive recovery (14 tests)
  eval-trace-impure-regression.sh  Specific bug regressions (6 tests)
  eval-trace-impure-soundness.sh   BUG-3/5/8 soundness, precision, OR-2 FileBytes backstop (8 tests)

tests/functional/flakes/
  eval-trace-core.sh               Flake trace basics, sibling sharing (8 tests)
  eval-trace-deps.sh               Flake deps, fetchTree, lock precision (28 tests, 1-25 with 13b/13c sub-tests)
  eval-trace-graph.sh              Resolved flake graph, subflakes, path: inputs (9 tests)
  eval-trace-output.sh             Scalar types, derivations (5 tests)
  eval-trace-recovery.sh           Constructive recovery (7 tests)
  eval-trace-semantic.sh           Semantic replay, identity equality (5 tests)
  eval-trace-volatile.sh           Volatile deps (4 tests)
  eval-trace-soundness.sh          BUG-1 regression, schema migration (4 tests)
  eval-trace-flake-inputs.sh       Real-world flake input mutation scenarios (7 tests)
```

### File Descriptions

#### tests/functional/ (impure mode)

**eval-trace-impure-core.sh** — Basic `--file` and `--expr` trace recording and
verification; file modification invalidation; selective trace verification; auto-called
function traces; `--argstr` context isolation (separate trace keys); `--expr` file
import trace dependencies.

**eval-trace-impure-deps.sh** — Oracle dependency tracking: `builtins.getEnv`,
`builtins.currentSystem`, `builtins.currentTime` (volatile), `builtins.pathExists`,
`builtins.readDir`, file deletion, directory removal, `readFileType`, `hashFile`,
`addPath`, `filterSource`. Each verifies the dep is correctly recorded and that
changing the oracle value invalidates the trace.

**eval-trace-impure-advanced.sh** — Partial trace invalidation via thunk mutation
(Adapton selective dirtying); deep nested trace chain; FullAttrs shared trace;
nested FullAttrs with shared dep; no infinite recursion; deep trace chain O(2^K);
dep suspension (fat parent trace).

**eval-trace-impure-output.sh** — Full output path tests: cursor-based verification;
trace invalidation on file modification; `--write-to` from traced results; `--json`
for drv/list/outPath; constructive recovery (revert A→B→A); three-way cycling
recovery; Phase 2/3 constructive recovery.

**eval-trace-impure-regression.sh** — Regression tests for specific bugs:
- Test 1: `readFile` dep inside derivation `buildCommand` tracked at drv level
- Test 2: `recordSiblingTrace` does not overwrite first-eval sibling's trace
- Test 3: `TracedExpr` wrapper from `navigateToReal` does not steal trace deps
- Test 4: derivation with multiple env-level `readFile` trace dependencies
- Tests 5-6: additional regression guards

**eval-trace-impure-soundness.sh** — Soundness guards:
- Test 1: BUG-3 — coerceToString sub-thunk dep survives `PublicationWarmupScope`
- Test 2: BUG-5 — untracked file change invalidates trace in impure mode
- Test 3: Precision — independent sibling invalidation (unrelated change = cache hit)
- Test 4: Precision — comment-only imported `.nix` change is subsumed; value change re-records
- Test 5: BUG-8 — failed eval does not corrupt subsequent cache verification
- Test 6: OR-2 fix — bare-import `attrNames` + key-set edit triggers warm-verify miss (positive soundness pin)
- Test 7: OR-2 coarse-conservative behaviour — comment-only edit invalidates warm verify (FileBytes is byte-granular) while preserving observable value
- Test 8: OR-2 scope — unrelated-file edits warm-hit (FileBytes backstop is per-file, not per-working-tree)

#### tests/functional/flakes/ (flake mode)

**eval-trace-core.sh** — Basic flake trace recording and verification; fine-grained
trace invalidation (Adapton demand-driven dirtying); sibling trace sharing via
`navigateToReal` wrapping; `--attr`-path cursor behavior; same-flake different-attr isolation.

**eval-trace-deps.sh** — Flake dependency tracking: `pathExists`, `readDir`,
flake input updates (irrelevant input = cache hit), `fetchTree` locking,
lock file precision, input narHash sensitivity, runtime fetchTree deps, mounted
input recording. 28 tests (1-25 with 13b/13c sub-tests) across input types and precision/soundness scenarios.
Test 24 is the BUG-11 regression guard (nixPath in policy digest); Test 25 is
the allowed-uris parity test.

**eval-trace-graph.sh** — Resolved flake graph behavior: follows-collapse correctness;
dir subflake with relative path; relative path recursion with nested relative inputs;
subflake output isolation; `path:` input session key stability (notes accessor-type
limitation — see Known Limitations in `src/libexpr/eval-trace/CLAUDE.md`).

**eval-trace-output.sh** — Scalar type recording and verification via cursor path;
derivation trace (`--json` returns traced store path); `--write-to` from traced
results; list output; `null`/`bool` types.

**eval-trace-recovery.sh** — Constructive recovery: dependency revert (A→B→A);
alternating versions; three-way cycling; stable runtime fetchTree input; session
invalidation on graph change; primary-session warm path; structural-variant
recovery gated by `--no-eval-trace-structural-recovery`.

**eval-trace-semantic.sh** — Semantic replay: `--reference-lock-file` with
warm verify; override-input session isolation; path/text/context semantic replay
with selective invalidation; identity equality across cache hits.

**eval-trace-volatile.sh** — Volatile dep behavior: `builtins.currentTime` always
fails verification; mixed volatile/non-volatile deps; non-volatile sibling cache hit
despite volatile sibling; volatile exec dep.

**eval-trace-soundness.sh** — Critical soundness regressions:
- Test 1: BUG-1 — flake update must not serve stale cached result
- Test 2: Schema migration — old DB coexists with new schema, evaluation succeeds
- Test 3: Precision — constructive recovery after v1→v2→v1 revert
- Test 4: Precision — unrelated JSON key change hits cache via structural override

**eval-trace-flake-inputs.sh** — Real-world flake input mutation scenarios (F-1 through F-7):
- F-1: `path:` input with dirty git working tree — session invalidation
- F-2: `builtins.fetchTarball` warm hit + tarball content change invalidation
- F-3: `nix flake update` end-to-end — no stale result + constructive recovery after revert
- F-4: `--override-input` type switch (`path:` → `git:`) does not reuse stale trace
- F-5: Deep follows chain / diamond pattern — soundness and no infinite loop
- F-6: Unused flake input change — cache-hit precision on the consuming output
- F-7: Partial flake update — one input advanced, sibling input's trace still warm-hits

### Common Patterns

#### clearStoreIndex()

Defined once in `tests/functional/common/functions.sh` and sourced by all
eval-trace functional test files via `common.sh`:

```bash
clearStoreIndex() {
    rm -rf "$TEST_HOME/.cache/nix/eval-trace"* "$TEST_HOME/.cache/nix/attr-vocab.sqlite"*
}
```

Call `clearStoreIndex` between test cases to prevent one test's cached traces
from interfering with another. Always call it at the start of a test that
needs a cold (no-trace) first evaluation.

#### NIX_ALLOW_EVAL=0

Used to assert that a result is served entirely from cache without calling
the evaluator. If the trace is not found or is invalid, the command fails.

```bash
NIX_ALLOW_EVAL=0 nix eval --file expr.nix --json  # must hit cache or fail
```

This is the primary assertion for "warm verify": if it succeeds, the trace
was valid and the result was served from cache.

#### NIX_SHOW_STATS + readEvalTraceCounter (path-kind discrimination)

`NIX_ALLOW_EVAL=0` proves "served from cache" but does NOT distinguish
WHICH cache path fired: primary session lookup, `scanHistory` bootstrap,
or 3-strategy recovery (GitIdentity / DirectHash / StructVariant). When
the invariant depends on the specific path, capture `NIX_SHOW_STATS` JSON
and assert on counter deltas via `readEvalTraceCounter`:

```bash
NIX_SHOW_STATS=1 NIX_SHOW_STATS_PATH="$TEST_HOME/mytest.json" \
    env NIX_ALLOW_EVAL=0 nix eval --json "$flake#attr"
records="$(readEvalTraceCounter "$TEST_HOME/mytest.json" evalTrace.record.count)"
hits="$(readEvalTraceCounter "$TEST_HOME/mytest.json" evalTrace.hits)"
bootstraps="$(readEvalTraceCounter "$TEST_HOME/mytest.json" evalTrace.recovery.historyBootstraps)"
attempts="$(readEvalTraceCounter "$TEST_HOME/mytest.json" evalTrace.recovery.attempts)"
```

Common assertion patterns:
- **Primary-session-served-only** (§N.4 Case A in the unit-test guide):
  `hits >= 1 && attempts == 0 && bootstraps == 0`.
- **Warm-subsumption (no re-record)**: `records == 0`.
- **Cold re-record (intended miss)**: `records >= 1`.
- **History-bootstrap served**: `bootstraps >= 1`.

The helper is defined in `tests/functional/common/functions.sh` and
takes two args: a path to the stats JSON and a dotted key path into it.
It exits non-zero on missing keys, so a regression that renamed or
removed a counter surfaces as a test failure rather than a false pass.

Example in production: `eval-trace-deps.sh` Test 25 (policy-digest
regression guard), the Tests 7/8/19/20 strengthenings, and
`eval-trace-recovery.sh` Test 6.

#### expectStderr 1

Used for expected failures (e.g., `NIX_ALLOW_EVAL=0` with no valid trace):

```bash
expectStderr 1 nix eval ... 2>&1 | grepQuiet "eval-trace"
```

`expectStderr N cmd...` asserts `cmd` exits with status `N`.

#### Test numbering convention

Tests within each file are numbered sequentially with comments:

```bash
# Test 1: description
...
# Test 2: description
...
```

The number is a stable identifier for referencing tests in bug reports and
commit messages (e.g., "Test 3 in eval-trace-impure-soundness.sh").

#### Test isolation

Each test case should:
1. Call `clearStoreIndex` before cold evaluation
2. Create fresh temporary files/directories
3. Clean up temporary resources at the end

Use `NIX_ALLOW_EVAL=0` for warm-verify assertions. Use `expectStderr 1` for
intentional failures (no trace found, volatile dep, schema mismatch).
