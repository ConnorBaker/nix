# Property Testing Framework — Implementation Notes

**Status:** Phase 1 and Phase 2 are fully landed. All property test
files exist in `src/libexpr-tests/eval-trace/property/invariant/`
(`soundness.cc`, `invalidation.cc`, `precision.cc`, `recovery.cc`,
`idempotence.cc`, `determinism.cc`, `trace-hash-determinism.cc`,
`structural-override.cc`, and additional invariants). `makeNixExprGen`
includes `FromJSONGen`, `AttrAccessGen`, `PathExistsGen`, and the nine
nixpkgs-fidelity generators. W1–W4 from the original plan are all
merged.

For remaining work, see `plans/property-testing-framework.md`:

- Suspected production soundness gaps (six `GTEST_SKIP` tests pending
  runtime investigation).
- `ListLength.RemoveElement_Invalidates` shared-DB contamination.
- Cross-session Track 2 tests (S2 invalidation, S3 recovery, S4
  fingerprint isolation).
- Remaining CQK coverage gaps (GitRevisionIdentity, NarIdentity,
  DirectoryEntries, DummyStore-backed CQKs, EnvironmentLookup,
  TraceValueContext precision).
- BUG-10 float regression property (C3).

## Shared-EvalState cross-session limitation

`simulateWarmRestart()` (renamed from `simulateNewSession()`) does not
clear `InterningPools` and cannot invalidate the `SourceAccessor` cache
without `invalidateFileCache(path)` per mutated path.  The stronger
`simulateColdProcess()` helper resets `cacheFileContentHash` via
`resetFileCache()` but is not safe to call mid-RC-iteration.  Tests that
depend on genuine cross-process isolation cannot be written without a
new `EvalState` per session, which is out of scope for RapidCheck's
same-process iteration model.  Document this when authoring new
cross-session tests and prefer `cross-session-soundness.cc`'s pattern
(explicit `invalidateFileCache` per mutated path).
