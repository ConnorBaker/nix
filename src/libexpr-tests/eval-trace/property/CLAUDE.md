# Property-Based Tests for Eval-Trace

Property tests use [RapidCheck](https://github.com/emil-e/rapidcheck) to verify
universal invariants of the eval-trace cache across randomly generated Nix
expressions.

## Directory Layout

```
property/
  expr-gen.hh              Types: TestExpr, DepSlot, showValue, assertValuesEqual
  expr-gen.cc              DepSlot methods, shared helpers, makeNixExprGen()
  value-assert.hh          Standalone assertValuesEqual
  generator-test.cc        Unit tests for generators
  CLAUDE.md                This file

  gen/                     One file per expression generator (40+ files)
    scalar.cc              Literal int/string/bool/null/float
    read-file.cc           builtins.readFile <tmpfile>
    attr-access.cc         (fromJSON readFile)."key"
    overlay.cc             nixpkgs overlay: self: super: { ... }
    import-tree.cc         Tree-shaped import chain (3 files)
    call-package.cc        callPackage: import + formals + defaults
    ...

  invariant/               Core cache invariants (P1–P11)
    soundness.cc           P1:  eval(E) == cached_eval(E)
    invalidation.cc        P2:  mutate(dep) → re-eval
    precision.cc           P3:  unrelated mutation → cache hit
    idempotence.cc         P4:  eval twice → same result
    recovery.cc            P5:  record(v1), record(v2), revert → recover(v1)
    determinism.cc         P6:  independent recordings → same result
    commutativity.cc       P7:  eval order independence
    monotonicity.cc        P8:  A's mutation → B still hits
    trace-hash-determinism.cc  P9:  trace hash stable across sessions
    cross-session-soundness.cc P10: cross-session warm hit
    structural-override.cc P11: SC override precision

  builtin/                 Per-builtin dep tracking
    attrset/               Attrset operations (P12–P18)
      update-operator.cc     (A // B).key
      intersect-attrs.cc     (intersectAttrs A B).key
      remove-attrs.cc        (removeAttrs A ["k"]).key
      map-attrs.cc           (mapAttrs f A).key
      attr-names.cc          length (attrNames A) — #keys dep
      has-attr.cc            A ? key — #has:key dep
      cat-attrs.cc           elemAt (catAttrs "k" [A B]) 0
    list/                  List operations (P19–P25)
      map.cc                 length (map f list)
      filter.cc              length (filter pred list)
      length.cc              length list — #len dep
      elem-at.cc             elemAt list 0
      concat-lists.cc        length (concatLists [A B])
      sort.cc                length (sort lessThan list)
      foldl.cc               foldl' (+) 0 list
    string/                String operations (P26–P30)
      replace-strings.cc     replaceStrings
      substring.cc           substring
      string-length.cc       stringLength
      concat-strings-sep.cc  concatStringsSep
      tostring-provenance.cc toString provenance
    fs/                    Filesystem / import
      path-exists.cc         pathExists
      pathexists-if.cc       pathExists + if/then/else
      dir-of.cc              dirOf
      import-file.cc         import <file.nix>
      read-dir.cc            readDir + mapAttrs composition
    comparison/            Comparison operations
      equality.cc            (fromJSON A) == (fromJSON B)
      less-than.cc           lessThan with #len deps
    formals/               Function formal parameters
      ellipsis.cc            { x, ... }: x
      no-ellipsis.cc         { a, b }: a
    trace/                 Cross-scope dep kinds (deterministic)
      parent-slot.cc         TraceParentSlot
      value-context.cc       TraceValueContext

  composition/             Multi-layer nixpkgs-fidelity patterns
    attrset-pipeline.cc    readFile → fromJSON → mapAttrs → removeAttrs → .key
    list-pipeline.cc       readFile → fromJSON → map → length
    multi-source-merge.cc  (a // b // c).key with 3 JSON sources
    import-tree.cc         Tree-shaped 3-file import chain
    call-package.cc        import + formals + defaults
    overlay.cc             Overlay function over structured data
    conditional-dep.cc     if pathExists then readFile else "default"
    sibling-trace.cc       Shared dep source, mkDerivation-style
    multi-binding-let.cc   5-dep-slot let with all bindings forced
    rec-attrset.cc         rec { val=readFile; name="pkg-${val}"; ... }
    try-eval.cc            tryEval success-path (RapidCheck)
    try-eval-error.cc      tryEval error-path (deterministic)

  coverage/                Builtins-batch coverage tests
    arithmetic-ops.cc      +, -, *, / on traced integers
    boolean-ops.cc         &&, ||, ! on traced booleans
    type-checks.cc         typeOf, isInt, isString, etc.
    list-concat-op.cc      ++ operator
    string-concat-op.cc    + operator on strings
    string-builtins.cc     Various string builtins
    toxml.cc               builtins.toXML
    fromtoml.cc            builtins.fromTOML
    list-transforms.cc     all, any, genList, etc.
    container-transforms.cc mapAttrs, filter on containers
    attrvalues-ops.cc      attrValues, attrNames operations
    eval-control.cc        tryEval, seq, deepSeq
    compositions.cc        Multi-builtin compositions
    readdir-attr-access.cc readDir + attribute access

  special/                 Error handling, concurrency
    error-path.cc          Error during eval → no cache corruption
    concurrent-eval.cc     Interleaved eval → no contamination
```

## Conventions

### File naming

- **gen/**: `<concept>.cc` — one generator function per file
- **invariant/**: named after the invariant (soundness, precision, etc.)
- **builtin/<category>/**: named after the builtin or operation
- **composition/**: named after the nixpkgs pattern being modeled
- **coverage/**: named after the builtin category being covered
- **special/**: named after the edge case

### Test naming

```cpp
TEST_F(FixtureName, Scenario_ExpectedOutcome)
```

Fixture classes use `EvalTraceProperty_<Name>` prefix.  Common suffixes:
- `*_Invalidates` — soundness: mutation causes cache miss
- `*_CacheHit` / `*_StillHits` — precision: mutation doesn't invalidate
- `*_CorrectlyInvalidates` — formerly a precision test, now soundness
- `*_WarmHit` — cold→warm cycle hits cache
- `*_ValueCorrect` — deterministic value verification

### Include paths

Files at different depths need different relative paths to `expr-gen.hh`:
- `property/*.cc` → `#include "expr-gen.hh"`
- `property/invariant/*.cc`, `property/composition/*.cc`, etc. → `#include "../expr-gen.hh"`
- `property/builtin/attrset/*.cc`, etc. → `#include "../../expr-gen.hh"`

`#include "eval-trace/helpers.hh"` is resolved via meson include paths and
works from any depth — do not change it.

### Writing a new generator

1. Create `gen/<name>.cc` with the generator function
2. Declare it in `expr-gen.hh`
3. Wire it into `makeNixExprGen()` in `expr-gen.cc` (if appropriate)
4. Include `"../expr-gen.hh"` and only the headers the generator needs

### Writing a new property test

1. Choose the right subdirectory based on what the test exercises
2. Create the test file with a unique `EvalTraceProperty_<Name>` fixture
3. Set a unique `testFingerprint = hashString(HashAlgorithm::SHA256, "prop-<name>")`
4. Use `rc::detail::checkGTestWith` + lambda for RapidCheck tests
5. Use `TEST_F` for deterministic tests
6. End every RC mutation iteration with `slot.restore(); invalidateFileCache(slot.path);`
7. For composition tests: use `*makeSpecificGen()` inside a no-arg lambda,
   NOT `(TestExpr expr)` from `makeNixExprGen()`. The latter draws from
   37+ generators and `RC_PRE` guards cannot reliably select the intended
   expression shape.
8. Assert path-kind via `PathCountersSnapshot` (helpers.hh) rather than
   `loaderCalls == N`. Three canonical shapes:
   - **Case A, primary-cache hit:** `snap.primaryCacheServedOnly()` —
     forbids recovery() fallback AND scanHistory bootstrap. Use for
     `_CacheHit` tests that genuinely expect the primary session cache
     to serve.
   - **Case B, cache miss:** `snap.deltaTraceCacheMisses() >= 1`. Use
     for `_Invalidates` tests after a dep mutation.
   - **Case D, any-cached-path after a deliberate session boundary**
     (simulateWarmRestart, recovery.cc, cross-session-soundness):
     `snap.deltaTraceCacheHits() >= 1`. Tolerates primary, recovery,
     and history-bootstrap paths. Pair with `assertValuesEqual` for
     correctness. The test is NOT asserting "recovery fires" — it's
     asserting "some cached path served and the loader did not re-run."
   - **Case E, tolerate re-eval OR recovery-then-recompute:**
     `snap.deltaTraceCacheMisses() + snap.deltaRecoveryAttempts() >= 1`.
     See `sort.cc` / `filter.cc` for current usage.
   The §N.4 migration completed 2026-04-20 converted every pre-existing
   `RC_ASSERT(loaderCalls == N)` to the appropriate counter shape.

   **Counter enable state.** `Counter::enabled` is set to `true`
   globally in `src/libexpr-tests/main.cc`, so raw counter reads
   (e.g., `nrRecoveryGitIdentityAccepted.load()`) work outside
   `PathCountersSnapshot`. Use `PathCountersSnapshot` when you want
   scoped baseline/delta; use raw reads when you want a single
   counter observation. Production still defaults to `false` — the
   global-enable change is test-harness-only.

### Session namespace isolation for multi-expression tests

All `makeCache()` calls within a fixture share the same `testFingerprint`
and all root expressions map to `AttrPathId(0)`. When a test evaluates
**two different expressions** (e.g., commutativity: A and B), both target
the same `(session_key, AttrPathId(0))` row in the Sessions table. The
second expression's cold eval finds the first expression's trace and
verifies it instead of recording its own.

**Fix:** Use separate fingerprints for each expression:

```cpp
auto fpA = hashString(HashAlgorithm::SHA256, "prop-mytest-A");
auto fpB = hashString(HashAlgorithm::SHA256, "prop-mytest-B");
auto savedFp = testFingerprint;

testFingerprint = fpA;
auto cA = makeCache(exprA.nixCode);
(void) forceRoot(*cA);

testFingerprint = fpB;
auto cB = makeCache(exprB.nixCode);
(void) forceRoot(*cB);

testFingerprint = savedFp;  // restore at end
```

Tests that need this: any test with `(TestExpr exprA, TestExpr exprB)`
or that evaluates multiple distinct expressions in one RC iteration.
Current examples: `commutativity.cc`, `monotonicity.cc`.

## Fixture Pattern

All property tests extend `TraceCacheFixture` (from `helpers.hh`):

```cpp
class EvalTraceProperty_Example : public TraceCacheFixture {
public:
    EvalTraceProperty_Example() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-example");
    }
};
```

Use `rc::detail::checkGTestWith` (NOT `RC_GTEST_FIXTURE_PROP` — that
recreates the fixture per iteration, destroying the SQLite DB).

## P2/P8 Guard: Kind::File Only

P2 (`invalidation.cc`) and P8 (`monotonicity.cc`) restrict mutation to
`Kind::File` slots. Excluded kinds:
- `Kind::JsonFile`/`Kind::JsonArray` — SC override correctly serves cache
- `Kind::FileExistence` — tested in dedicated `builtin/fs/path-exists.cc` and `pathexists-if.cc`
- `Kind::EnvVar` — flaky in shared-DB RC iteration model
- `Kind::DirectoryEntries` — tested in dedicated `builtin/fs/read-dir.cc`

## Known Limitations

1. **SC override and JsonFile/JsonArray in P2** — SC dep granularity may not
   invalidate on value changes at unaccessed keys. P2 scoped to FileBytes.
2. **EnvVar in shared-DB model** — stale trace hash across RC iterations.
3. **FileExistence deletion** — tested deterministically in `pathexists-if.cc`
   (Soundness and Toggle tests) and via RC in `path-exists.cc`.
4. **EvalState reuse** — `cacheFileContentHash` persists across RC iterations.
   `simulateWarmRestart()` clears it but individual iterations do not.
   (For a stronger cold-process boundary that also wipes PosTable and
   traceCtx, use `simulateColdProcess()`; not safe mid-iteration.)
5. **`EvalTraceProperty_HasAttr.DISABLED_RemoveKey_Invalidates`** — disabled.
   In the shared-fixture RapidCheck model it can fail the pre-mutation warm-hit
   check even when deterministic `hasattr.cc` coverage passes, including
   quoted-string `?` cases and key-removal cache misses. Prefer deterministic
   coverage unless this property is rewritten to run one generated case per
   fresh fixture / `EvalState`.
6. **Filesystem-name coverage on case-insensitive FSes** — `makeReadDirMapAttrsGen`
   uses `makeNixFilesystemIdentifierGen` (in `expr-gen.cc`) instead of
   `makeNixIdentifierGen` for names that become real directory entries. On
   case-insensitive filesystems (macOS APFS default, Windows NTFS default,
   sandboxed `$TMPDIR` under `/nix/var/nix/builds` on macOS) this restricts
   generated filenames to `[a-z0-9_]` so two distinct generated names cannot
   alias onto the same inode (e.g. `_d` and `_D`). On case-sensitive
   filesystems (most Linux) the generator delegates to `makeNixIdentifierGen`
   and full upper/lower-case coverage is preserved. Because `makeReadDirMapAttrsGen`
   is mixed into `makeNixExprGen()`, every property test that draws the
   general expression pool sees this coverage delta on case-insensitive FSes.
   In-memory attrset/JSON key generators (`makeJsonObjectGen` etc.) are
   unaffected — Nix attrset keys are case-sensitive regardless of host FS.
   Detection lives in `tempFsIsCaseSensitive()` (local to `expr-gen.cc`),
   which probes the temp-dir base once per process.
