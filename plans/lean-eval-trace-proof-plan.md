# Lean Eval-Trace Proof Plan

**Date:** 2026-04-10

---

## 1. Requirement

The requirement is not merely:

> "prove that the cache checker only accepts matching dep hashes."

It is:

> "prove that the dependency-tracking implementation records a sufficient set of
> dependencies, so that accepting a trace implies equivalence to fresh
> evaluation."

That requires four separate proof obligations:

1. **Dependency semantics are correct.**
   The abstract meaning of `CanonicalQueryKind`, override/subsumption,
   provenance resolution, replay, and parent/child trace context is sound.

2. **The implementation of tracking matches the semantics.**
   The concrete recorder, replay logic, and verifier implement that abstract
   model.

3. **Instrumentation is complete.**
   The evaluator emits all required dependency observations for the audited
   fragment.

4. **The theorem target is honest.**
   Any known soundness gaps are either fixed or explicitly excluded.

This plan is therefore for a verified dependency-tracking implementation, not
just a verified verification pass.

---

## 2. What Success Looks Like

There are three theorem levels. Only the third fully satisfies the requirement.

### Level A: Verified kernel

> Given a correct stream of dependency events, the Lean kernel records, replays,
> verifies, and recovers traces soundly.

This is necessary, but insufficient. It does **not** yet prove the current Nix
implementation of dependency tracking is correct.

### Level B: Audited implementation theorem

> For a specific audited fragment of the evaluator, the C++ implementation emits
> a complete and correctly classified dependency-event stream, and the Lean
> kernel is sound over that stream.

This is the first theorem that actually proves dependency tracking correctness
for real implementation code.

### Level C: Whole-system theorem

> For all supported eval-trace behaviors, trace acceptance implies equivalence to
> fresh evaluation.

This is not achievable immediately because the current design explicitly admits
known soundness gaps.

**Planned deliverable:** Level B for a materially useful subset, with an
explicit path to Level C after closing listed gaps.

---

## 3. Precise Proof Target

The current draft said "observationally equivalent" but did not define it
precisely enough. This section fixes that.

### 3.1 World model

The proof uses an explicit world `W` containing exactly the state that eval
trace may observe in the audited fragment:

- file bytes
- directory listings
- path existence and file type
- environment variables
- current system value
- runtime-root identity and store-path availability
- structured file projections for JSON/TOML
- `.nix` binding projections
- volatile observations:
  - time
  - `builtins.exec`
- trace-context dependencies:
  - `TraceValueContext`
  - `TraceParentSlot`
- provenance/registry state:
  - graph node mounts
  - runtime-root mounts

Still excluded from the initial theorem:

- arbitrary network effects
- undocumented impurity not represented by a dep kind

Time and `builtins.exec` are modeled in the spec and kernel as volatile
observations that force rejection rather than successful reuse. They are
therefore in scope for the dependency model, but not as reusable effects.

### 3.2 Result relation

For the initial theorem, the result relation is:

- successful values compare by equality of the fully forced,
  evaluator-observable value, including:
  - scalar payloads
  - list and attrset shape
  - string contents plus string context
  - path/provenance-visible identity where user-observable
- evaluation errors compare by same error class plus equivalent observable cause
- divergence is **out of scope** for v1

This means the first theorem is for terminating evaluations only. That should
be stated explicitly in the proof target document and in Lean.

Still excluded from the initial result relation:

- function extensionality
- opaque native values not materialized in the audited subset
- observational statements that require proving arbitrary laziness properties

### 3.3 Agreement relation

For a dependency set `D`, define `AgreeOn(W1, W2, D)` to mean:

- every observation represented by every dep in `D` yields the same abstract
  observation in `W1` and `W2`

The central theorem is then:

> If evaluating expression `e` in world `W1` records dep set `D` and result `r`,
> and `AgreeOn(W1, W2, D)` holds, then fresh evaluation of `e` in `W2` produces
> a result observationally equivalent to `r`.

That is the dependency-tracking theorem. Everything else is in service of it.

---

## 4. Requirement Coverage Gaps in the Current Code

The design document already states that a full theorem is false today:

- [design.md §8.1](/home/connorbaker/nix/doc/eval-trace/design.md#L541)

The initial proof must therefore either:

1. fix those gaps first, or
2. explicitly exclude them from the theorem

The plan must be judged against that fact. A proposal that ignores the listed
gaps is not ambitious; it is incorrect.

### Initial explicit exclusions

Unless fixed first, v1 excludes:

- ParentSlot key-set soundness
- symlink-target soundness
- known parent-mediated value-change gaps
- any evaluator behavior outside the audited instrumentation set
- divergence-sensitive statements

This is the minimum honest theorem envelope.

---

## 5. Current Code Boundary: What Helps and What Hurts

### Good starting surface

The following pieces are already close to proof-friendly:

- dep algebra and descriptors:
  [types.hh](/home/connorbaker/nix/src/libexpr/include/nix/expr/eval-trace/deps/types.hh)
- typed IDs:
  [ids.hh](/home/connorbaker/nix/src/libexpr/include/nix/expr/eval-trace/ids.hh)
- interning pools:
  [interning-pools.hh](/home/connorbaker/nix/src/libexpr/include/nix/expr/eval-trace/deps/interning-pools.hh)
- recording logic:
  [dep-recording-context.cc](/home/connorbaker/nix/src/libexpr/eval-trace/deps/dep-recording-context.cc)
- provenance registry:
  [semantic-registry.hh](/home/connorbaker/nix/src/libexpr/include/nix/expr/eval-trace/store/semantic-registry.hh)

### Bad starting surface

The following are proof-hostile and should remain outside the initial verified
core or be refactored first:

- pointer-keyed runtime state and GC-address reuse:
  [memo-replay-store.hh](/home/connorbaker/nix/src/libexpr/include/nix/expr/eval-trace/deps/memo-replay-store.hh)
- global mutable session side channels:
  [context.hh](/home/connorbaker/nix/src/libexpr/include/nix/expr/eval-trace/context.hh)
  and [context.cc](/home/connorbaker/nix/src/libexpr/eval-trace/context.cc)
- GC/finalizer-sensitive lifetime assumptions:
  [traced-expr.hh](/home/connorbaker/nix/src/libexpr/eval-trace/cache/traced-expr.hh)

The allocator convergence with `mimalloc` helps ABI interoperability, but it
does not remove Boehm-managed object identity from the proof problem.

---

## 6. Refactors Required Before the Lean Kernel Can Be Trusted

These are not optional cleanup tasks. They are prerequisites for a credible
proof story.

### 6.1 Eliminate session-scoped mutation during cold eval

Current problem (updated 2026-04-29):

- The previously-cited `activeRegistry_` and `activeBackend_` process-scoped
  globals have been removed.  `SemanticRegistry` is constructed with its
  mount points at session open, and the `TraceBackend *` is owned by
  `TraceSession`.
- Mutation during cold eval still happens at the session granularity:
  - `TraceSession::registerRuntimeRootMount` (in `cache/trace-session.cc`)
    dynamically adds mount points to the per-session `SemanticRegistry`
    when runtime `fetchTree` inputs resolve.
  - `TraceSession::releaseBackend` swaps the live backend for
    `NullTraceBackend` during session recycling; `TracedExpr` thunks
    created before the swap become "zombies".
- Both are session-scoped, not process-scoped, so the worst hazards of
  ambient globals are gone.  What remains is still not proof-friendly:
  correctness of the reused-session path depends on invariants about
  which session a `TracedExpr` was bound to, and the zombie-thunk
  behavior after `releaseBackend` is not expressed in the type system.

Relevant code:

- `src/libexpr/eval-trace/cache/trace-session.cc` — `registerRuntimeRootMount`,
  `releaseBackend`, constructor body that populates the initial registry.
- `src/libexpr/eval-environment.cc` — `EvalEnvironment::completeLocked/
  UnlockedRuntimeFetch` call sites that invoke `registerRuntimeRootMount`
  and `recordRuntimeRoot`.
- `src/libexpr/eval-environment/authority.cc` — session recycling path
  that invokes `releaseBackend`.

Required change:

- express the "session-bound" precondition on `TracedExpr` and its
  `LazyState` in the type system, not as a runtime invariant on
  `activeSession_`.
- make the zombie-thunk path (between `releaseBackend` and GC) either
  structurally unreachable or explicitly null-typed.
- remove dynamic registry mutation during cold eval, or prove that
  the mutation does not affect prior dep recordings.

Files to change:

- `src/libexpr/include/nix/expr/eval-trace/context.hh`
- `src/libexpr/eval-trace/context.cc`
- `src/libexpr/include/nix/expr/eval.hh`
- `src/libexpr/eval.cc`
- `src/libflake/flake.cc`
- `src/libexpr/primops/fetchTree.cc`
- `src/libexpr/eval-trace/cache/trace-session.cc`
- `src/libexpr/eval-trace/cache/traced-expr.hh`

Acceptance tests:

- reuse-session path writes runtime-root metadata into the reused session
  type-safely, not via a separate mutation call.
- `resetFileCache()` leaves no dangling session-bound `TracedExpr` state.
- long-lived `EvalState` C API and REPL flows remain correct.
- `releaseBackend` zombie path is unreachable at compile time or
  explicitly benign at runtime.

### 6.2 Replace pointer identity at the Lean boundary

Current problem:

- replay and identity logic are keyed by `Value *`
- correctness depends on GC lifetime and address reuse behavior

Required change:

- introduce explicit stable tokens at the boundary to the verified kernel
- keep raw pointers entirely on the C++ side of an adapter layer

Suggested token types:

- `ThunkToken : UInt32`
- `ValueToken : UInt32`
- `ScopeToken : UInt32`
- `TraceNodeToken : UInt32`

Files to change:

- [memo-replay-store.hh](/home/connorbaker/nix/src/libexpr/include/nix/expr/eval-trace/deps/memo-replay-store.hh)
- [context.hh](/home/connorbaker/nix/src/libexpr/include/nix/expr/eval-trace/context.hh)
- [context.cc](/home/connorbaker/nix/src/libexpr/eval-trace/context.cc)
- [trace-session.hh](/home/connorbaker/nix/src/libexpr/include/nix/expr/eval-trace/cache/trace-session.hh)
- [trace-session.cc](/home/connorbaker/nix/src/libexpr/eval-trace/cache/trace-session.cc)
- [traced-expr.hh](/home/connorbaker/nix/src/libexpr/eval-trace/cache/traced-expr.hh)

Acceptance tests:

- no Lean-visible state is keyed by raw addresses
- replay equivalence tests pass under synthetic token reuse scenarios
- existing epoch-bug regression tests still pass

### 6.3 Make the dependency-event language explicit

Current problem:

- the proof boundary is implicit in scattered C++ calls
- instrumentation completeness cannot be stated cleanly

Required change:

- define an explicit event language emitted by the evaluator-side adapter
- make the Lean kernel consume only those events

Core event families:

- `PushScope`
- `FinalizeScope`
- `ObserveDep`
- `ObserveStructuredDep`
- `ReplayRange`
- `RecordThunkRange`
- `RegisterRegistryEntry`
- `RegisterMountPoint`
- `VerifyTrace`
- `RecoverTrace`

Files to add:

- `src/libexpr/include/nix/expr/eval-trace/lean-kernel-api.hh`
- `src/libexpr/eval-trace/lean-kernel-adapter.cc`

Acceptance tests:

- a complete event trace can be captured for audited evaluations
- C++ and Lean kernels agree on finalized dep sets and verification outcomes

### 6.4 Make instrumentation obligations explicit and exhaustive

Current problem:

- the design doc lists recording sites, but there is no machine-checked
  inventory of what each site must observe

Required change:

- write an implementation-facing obligation table mapping each audited builtin
  or evaluator action to the exact abstract observation it must emit
- this table becomes the contract proved in Lean and checked in C++

Files to create:

- `plans/lean-eval-trace-obligation-table.md`

Acceptance tests:

- every audited site has a corresponding obligation entry
- every obligation entry has at least one concrete regression test

### 6.5 Tie the audited subset to concrete implementation files

The proof plan must name the current implementation sites, not just abstract
operations.

Audited site map:

- `evalFile` / import content deps:
  [eval.cc](/home/connorbaker/nix/src/libexpr/eval.cc)
- `.nix` binding registration and recording:
  [nix-binding.cc](/home/connorbaker/nix/src/libexpr/eval-trace/deps/nix-binding.cc)
  and [eval.cc](/home/connorbaker/nix/src/libexpr/eval.cc)
- `getEnv`, `pathExists`, `readFile`, `readFileType`, `readDir`,
  `fromJSON`, `currentSystem`:
  [primops.cc](/home/connorbaker/nix/src/libexpr/primops.cc)
- `fromTOML`:
  [fromTOML.cc](/home/connorbaker/nix/src/libexpr/primops/fromTOML.cc)
- `fetchTree` runtime-root and store-availability tracking:
  [fetchTree.cc](/home/connorbaker/nix/src/libexpr/primops/fetchTree.cc)
- flake/session provenance and graph-backed registry population:
  [flake.cc](/home/connorbaker/nix/src/libflake/flake.cc)
- trace-context production and replay collapse:
  [context.hh](/home/connorbaker/nix/src/libexpr/include/nix/expr/eval-trace/context.hh)
  and [context.cc](/home/connorbaker/nix/src/libexpr/eval-trace/context.cc)
- trace-context-bearing traced expressions:
  [trace-session.cc](/home/connorbaker/nix/src/libexpr/eval-trace/cache/trace-session.cc)

Acceptance tests:

- every audited theorem references one or more concrete implementation files
- every audited file is assigned to at least one instrumentation contract

---

## 7. Lean v1 Scope

The first version must cover more than coarse file deps. In particular,
trace-context and provenance are part of the dependency-tracking claim and
belong in v1.

### 7.1 Pure kernel

Lean owns the semantics of:

- dep kinds and descriptors
- dep values and typed IDs
- provenance registry
- recording and dedup
- instability
- replay and rollback
- pass-1 / pass-2 verification
- recovery
- trace-context deps:
  - `TraceValueContext`
  - `TraceParentSlot`

### 7.2 Audited implementation subset

The initial audited subset must include:

- `evalFile`
- `.nix` binding structured deps
- `readFile`
- `fromJSON`
- `fromTOML`
- `readDir`
- `readFileType`
- `pathExists`
- `getEnv`
- `currentSystem`
- `currentTime`
- `builtins.exec`
- `fetchTree` runtime-root and store-availability tracking
- graph-node and runtime-root provenance resolution
- trace-context emission and verification:
  - `TraceValueContext`
  - `TraceParentSlot`

This is larger than the original draft on purpose. Without trace-context and
provenance in v1, we would mostly be proving a filesystem oracle layer rather
than the eval-trace system.

### 7.3 Explicit non-goals for v1

Still deferred:

- full async orchestration proof
- full evaluator semantics
- unrestricted flake/session whole-system theorem

---

## 8. Concrete Theorems

The proof development should target named theorems, not only a prose story.

### 8.1 Semantic target

- `obs_eq_success`
- `obs_eq_error`
- `agree_on_dep`
- `agree_on_dep_set`

### 8.2 Registry and provenance

- `reverse_forward_agree`
- `runtime_root_namespace_disjoint`
- `node_key_namespace_disjoint`
- `provenance_resolution_sound`

### 8.3 Recording

- `record_preserves_scope_own_dep_invariant`
- `record_preserves_epoch_suffix`
- `record_dedup_sound`
- `record_unstable_only_on_semantic_conflict`
- `parent_child_dep_separation_sound`

### 8.4 Replay

- `rollback_removes_future_ranges`
- `replay_into_active_scope_only`
- `replay_sound`
- `trace_value_context_sound`
- `trace_parent_slot_sound`

### 8.5 Verification and recovery

- `verified_content_subsumes_structural`
- `pass2_structural_override_sound`
- `runtime_root_verification_sound`
- `direct_hash_recovery_sound`
- `structural_variant_recovery_sound`
- `volatile_never_valid`

### 8.6 Instrumentation completeness

One theorem family per audited site:

- `instrument_evalFile_complete`
- `instrument_readFile_complete`
- `instrument_json_complete`
- `instrument_toml_complete`
- `instrument_readDir_complete`
- `instrument_pathExists_complete`
- `instrument_readFileType_complete`
- `instrument_getEnv_complete`
- `instrument_currentSystem_complete`
- `instrument_currentTime_complete`
- `instrument_exec_complete`
- `instrument_fetchTree_complete`
- `instrument_trace_context_complete`

These are the theorems that lift the plan from a verified kernel to a verified
implementation fragment.

### 8.7 End-to-end theorem

- `tracked_eval_sound_audited_subset`

This theorem is parameterized by:

- the explicit world model
- the explicit observation relation
- the audited instrumentation contracts
- the explicit exclusions from [design.md §8.1](/home/connorbaker/nix/doc/eval-trace/design.md#L541)

---

## 9. Lean Module Layout

Suggested package structure:

```text
EvalTrace/
  World.lean
  ObsEq.lean
  Types.lean
  Ids.lean
  Hash.lean
  Registry.lean
  Recording.lean
  Replay.lean
  TraceContext.lean
  Verify.lean
  Recovery.lean
  EventLang.lean
  InstrumentationContracts.lean
  Instrumentation/
    EvalFile.lean
    NixBinding.lean
    ReadFile.lean
    JsonToml.lean
    ReadDir.lean
    PathExists.lean
    EnvSystem.lean
    FetchTree.lean
    TraceContext.lean
  Theorems/
    Registry.lean
    Recording.lean
    Replay.lean
    TraceContext.lean
    Verify.lean
    Recovery.lean
    Instrumentation.lean
    EndToEnd.lean
```

Lean should continue to model Nix values abstractly. Do not begin by modeling
all of Nix evaluation.

---

## 10. C ABI and Performance Contract

The original requirement also asked whether the Lean component can be low-cost
enough for production use. The plan needs an explicit performance contract.

### 10.1 ABI shape

Production ABI requirements:

- POD-only arguments and results
- `uint32_t` IDs and tokens
- fixed-size hashes
- no Lean heap objects crossing the boundary
- no C++ ownership of Lean objects except opaque session handles

Suggested core types:

```c
typedef struct lt_session lt_session;

typedef struct {
  uint8_t kind;
  uint8_t flags;
  uint32_t source_id;
  uint32_t key_id;
  uint32_t aux_id;
  uint8_t hash_tag;
  uint8_t hash_bytes[32];
} lt_dep;
```

### 10.2 Runtime model

The ABI must also specify:

- one-time Lean runtime initialization per process
- session construction/destruction rules
- error transport across the ABI
- ownership of returned buffers

Required rule:

- no panics/exceptions may cross the C ABI boundary

### 10.3 Call-frequency budget

Prototype mode may use per-event calls.

Production mode must not.

Production budget:

- no more than one FFI call for scope finalization
- no more than one FFI call for replay publication of a range
- registry/session setup may batch arbitrarily
- zero per-dep heap allocation on the C++ side of the boundary

This keeps the hot path out of the per-dep FFI regime.

### 10.4 Benchmark gates

The plan needs explicit performance gates:

1. **Shadow mode gate**
   Up to 2x slowdown on targeted test workloads is acceptable while validating
   the model.

2. **Production gate**
   No more than 10% wall-clock regression on trace-enabled evaluator benchmarks
   relative to the native C++ kernel.

3. **Allocation gate**
   No new unbounded allocation proportional to dep count on the hot path beyond
   the existing dep-buffer materialization.

4. **Init gate**
   Lean runtime initialization must be one-time, not per evaluation.

Files to add:

- benchmark harness under `src/libexpr-tests/eval-trace/bench/`

---

## 11. C++ / Lean Split

### Keep in C++

- filesystem access
- store access
- git access
- environment access
- time and process execution
- parsing into host runtime objects
- evaluator control flow
- pointer-keyed GC-sensitive implementation details behind an adapter

### Move to Lean

- dep-key formation
- dep semantics
- provenance semantics
- recording/dedup/instability
- replay semantics
- trace-context semantics
- verification and override logic
- recovery logic
- instrumentation contracts for the audited subset

This keeps Lean on the semantic side of the boundary, where proof value is
highest and FFI friction is lowest.

---

## 12. Execution Plan

### Stage 0: Publish the exact theorem target

Deliverables:

- proof-target note defining:
  - world model
  - observation relation
  - result relation
  - explicit exclusions

Acceptance:

- no theorem statement in the project may use "equivalent" or "same result"
  without referring to that definition

### Stage 1: Refactor the implementation boundary

Tasks:

1. remove global active-session correctness dependence
2. introduce stable Lean-boundary tokens
3. add explicit dependency-event API
4. write the obligation table for audited instrumentation

Acceptance:

- new regression tests for session reuse and reset hazards pass
- every audited site is represented in the obligation table

### Stage 2: Build the Lean reference kernel

Tasks:

1. mirror dep algebra and registry
2. mirror recording and replay
3. mirror verification and recovery
4. prove internal invariants

Acceptance:

- Lean model compiles
- generated event traces match C++ kernel behavior

### Stage 3: Shadow mode

Tasks:

1. feed identical event traces to C++ and Lean kernels
2. compare finalized dep sets
3. compare replay outputs
4. compare verification and recovery outcomes

Acceptance:

- zero-diff shadow mode on audited eval-trace tests

### Stage 4: Prove instrumentation completeness for v1

Tasks:

1. prove one obligation theorem per audited site
2. prove trace-context completeness
3. prove provenance/runtime-root completeness

Acceptance:

- all v1 instrumentation theorems are complete
- every theorem has at least one paired regression test

### Stage 5: Switch verification and recovery to Lean

Acceptance:

- existing functional tests pass
- benchmark gates hold

### Stage 6: Switch recording and replay core to Lean

Acceptance:

- existing functional tests pass
- shadow mode can be disabled because Lean is the production kernel
- benchmark gates still hold

### Stage 7: Expand from Level B to Level C

Tasks:

1. close currently excluded soundness gaps
2. enlarge the audited subset
3. remove exclusions one by one

Acceptance:

- theorem exclusions shrink monotonically

---

## 13. Testing Strategy

Use four classes of tests.

### 13.1 Model equivalence tests

- generated event traces through both kernels

### 13.2 Existing eval-trace tests

Run:

- `src/libexpr-tests/eval-trace/...`
- `tests/functional/flakes/eval-trace-*.sh`
- `tests/functional/eval-trace-impure-*.sh`

### 13.3 Obligation tests

Each audited site gets:

- a proof obligation
- a targeted regression test
- a shadow-mode equivalence test

### 13.4 Performance tests

Benchmark:

- trace-enabled nixpkgs evaluation microbenchmarks
- representative impure builtin workloads
- trace-context-heavy workloads

The existing correctness gate remains useful:

- [implementation.md §8.1](/home/connorbaker/nix/doc/eval-trace/implementation.md#L412)

but it is a supplement to proof, not a substitute.

---

## 14. Adversarial Summary

This plan meets the requirement only if judged by the Level B deliverable:

- verified kernel
- verified implementation obligations for an audited fragment
- explicit exclusions

It does **not** pretend to prove the current whole implementation correct today.
That would be false given the documented gaps.

The plan therefore succeeds if it produces:

1. an exact theorem statement
2. concrete refactors that make the theorem tractable
3. proof of instrumentation completeness for a nontrivial audited subset
4. a production-worthy ABI and performance story

If any of those are missing, the plan does not meet the requirement.

---

## 15. Bottom Line

A credible Lean effort here should target:

1. a verified dependency-tracking kernel over explicit event streams and interned IDs
2. a verified implementation theorem for an audited eval-trace subset that
   includes provenance and trace-context machinery
3. a production ABI with explicit performance gates
4. gradual expansion toward a whole-system theorem as soundness gaps are closed

Do **not** begin by trying to verify:

- Boehm object lifetimes
- raw pointer identity maps
- async orchestration
- all of Nix evaluation

Begin by forcing the implementation to refine a precise dependency-event model.
That is the shortest path to a theorem that is true, useful, and cheap enough
to ship.
