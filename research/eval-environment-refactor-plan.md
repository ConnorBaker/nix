# EvalEnvironment Refactor Plan

## 1. Goal

Create a single shared libexpr-side authority object, `EvalEnvironment`, as the
only place where evaluator code, shared flake/loading code, and verifier-side
effect observation interact with:

- the filesystem
- the Nix store
- network-backed fetch/materialization
- environment variables
- runtime root publication
- eval-trace dependency recording for effectful observations

The main objective is to make dependency recording structurally hard to bypass.
The secondary objective is to reduce `src/libexpr` size and complexity relative
to the current branch by collapsing indirection and moving behavior to clearer
ownership boundaries.

This is a rewrite plan. Backwards compatibility, bridges, compatibility layers,
and legacy shims are explicitly out of scope.

## 2. Hard Constraints

1. Keep the current compact in-memory `Value` representation.
2. Do not preserve the current spread of effectful helpers across:
   - `EvalState`
   - `primops.cc`
   - `primops/fetchTree.cc`
   - eval-trace dep-resolution helpers
3. Do not preserve in-process protocol/service shells merely because they exist
   today.
4. Prefer direct typed calls over message/protocol indirection when the callee
   is in-process.
5. The boundary cannot be evaluator-only if important dependency-producing
   effect flows live elsewhere in `libexpr` / `libflake`.

## 3. Current Problems

### 3.1 Side effects are structurally scattered

Today, effectful evaluator operations are open-coded across multiple layers:

- `EvalState` performs path coercion, path whitelisting, closure whitelisting,
  mounting, source reading, and environment access.
- `primops.cc` directly performs effectful reads and then separately records
  deps.
- `fetchTree` uses its own path/materialization flow and then publishes
  runtime-root state.
- eval-trace verification and dep resolution effectively define a second effect
  model.

Representative current seams:

- [eval.hh](/home/connorbaker/nix/src/libexpr/include/nix/expr/eval.hh)
- [eval.cc](/home/connorbaker/nix/src/libexpr/eval.cc)
- [primops.cc](/home/connorbaker/nix/src/libexpr/primops.cc)
- [fetchTree.cc](/home/connorbaker/nix/src/libexpr/primops/fetchTree.cc)
- [dep-resolution-service.cc](/home/connorbaker/nix/src/libexpr/eval-trace/store/dep-resolution-service.cc)

This guarantees drift, because the "effect happened" boundary and the
"dependency was recorded" boundary are not the same API boundary.

### 3.2 The branch adds too much in-process eval-trace indirection

The current branch contains a large amount of wrapper/protocol/service code in
addition to the actual semantic logic:

- `TraceBackend`
- `VerificationOrchestrator`
- `verification-protocol.hh`
- session-typed protocol layers around in-process calls

Those layers add code volume and make the runtime harder to reason about.

Representative files:

- [trace-backend.hh](/home/connorbaker/nix/src/libexpr/include/nix/expr/eval-trace/cache/trace-backend.hh)
- [verification-orchestrator.hh](/home/connorbaker/nix/src/libexpr/eval-trace/store/verification-orchestrator.hh)
- [verification-protocol.hh](/home/connorbaker/nix/src/libexpr/include/nix/expr/eval-trace/store/verification-protocol.hh)

### 3.3 `TraceRuntime` had correctness-damaging side channels

Before the Bundle 3 cleanup, `TraceRuntime` had evaluator-local mutable side
channels such as:

- remembered session state
- registry mount publication
- runtime-root recording paths
- identity maps that are adjacent to, but not identical with, the bound session

Representative file:

- [context.hh](/home/connorbaker/nix/src/libexpr/include/nix/expr/eval-trace/context.hh)

The rewrite should leave exactly one active bound trace/effect session, not
several side channels that can drift apart.

Current status:

- `activeRegistry_`, `activeBackend_`, and the old `TraceRuntime` session cache
  have been removed
- session reuse now lives in an internal `TraceSessionFactory` helper adjacent
  to the `EvalEnvironmentAuthority` boundary instead of `TraceRuntime`
- runtime-root publication and registry mutation now route through the bound
  session / `EvalEnvironment`

### 3.4 Value behavior is too free-floating

Even though the compact `Value` layout should stay, most of the behavior around
values is still spread across:

- `EvalState`
- coercion helpers
- free functions
- monolithic primop implementations

Representative files:

- [value.hh](/home/connorbaker/nix/src/libexpr/include/nix/expr/value.hh)
- [eval.cc](/home/connorbaker/nix/src/libexpr/eval.cc)
- [primops.cc](/home/connorbaker/nix/src/libexpr/primops.cc)

## 4. Target Architecture

The rewrite should reduce to six main pieces:

1. `EvalState`
2. evaluator-side request-builder layer
3. `EvalEnvironment`
4. `TraceStore`
5. `Value` view/wrapper layer
6. domain-split primop files

### 4.1 `EvalState`

`EvalState` should own:

- evaluator semantics
- forcing / thunk logic
- lexical scope / env handling
- allocation
- non-effectful value operations

`EvalState` should not directly own effectful operations like:

- `getEnv`
- raw filesystem reads
- raw directory reads
- raw store queries
- fetch materialization
- runtime-root publication
- direct dependency-recording calls for effects

### 4.2 `EvalEnvironment`

`EvalEnvironment` becomes the single shared `libexpr` effect boundary.

It should own:

- environment lookup
- source/path realization
- filesystem reads
- directory reads
- store-path availability and store interaction
- input fetching/materialization/mounting
- runtime-root publication
- effect-origin/dependency-recording hooks
- the active bound eval-trace session, if eval-trace is enabled
- the detached non-session effect scope for shared libexpr callers such as
  flake graph resolution and metadata flows

`EvalEnvironment` should be the only place allowed to translate a libexpr
effect into:

- a concrete result
- provenance / identity data
- dependency recording

### 4.2.1 Explicit authority objects

The rewritten boundary should not recover most of its behavior from ambient
`EvalState` fields.
The end-state goal is explicit authority throughout; any remaining ambient
recording or helper state is a transition cost, not the intended API shape.

Two explicit authority objects are required:

1. `EvalEnvironmentAuthority`
   - stores
   - fetch/input-cache ownership
   - root/store/internal accessors
   - evaluation policy inputs that materially affect side effects
2. `EvalTraceSessionAuthority`
   - root-loader capability
   - owned semantic-registry seed
   - input-accessor and mounted-input bindings
   - exact current session-config constituents:
     - `policyDigest`
     - `graphDigest`
     - `sourceIdentity`
     - `externalRoots`
     - `stableRecoveryKey`

The exact session-config constituents should not be open-coded at the call
sites. Two typed builders are required:

1. flake session-config builder
2. file-eval session-config builder

Session reuse should also not remain open-coded. Two typed reuse-key builders
are required:

1. flake session reuse-key builder
2. file-eval session reuse-key builder

If those stay ambient, the draft still has a nicer call surface but not a real
authority boundary.

Typed session and recovery identities are already a real improvement, and the
same is now largely true for the structured dep-key and runtime-root paths in
the live tree: they stay typed through the in-memory and trace-store layers
and only collapse at the explicit SQLite binding boundary. The remaining gap
is the shared interned-blob substrate, the last `EvalState` bypasses, and any
identity aliases that are not threaded through the real API surface.

Publication sealing is only genuinely done if public construction of
`PublishedStorePathString` remains impossible. If that constructor or an
equivalent public escape hatch still exists anywhere, the publication boundary
is still open and the plan should treat it as such.

The session assembly surface has also been narrowed further: file-eval git
identity now flows as a direct optional snapshot instead of a one-field config
wrapper, and the redundant `FileEvalGitSourceIdentity` alias has been collapsed
to `CurrentGitIdentityHash`.

Considered:

1. keep vague stable identity hints
2. reuse `eval_trace::SessionConfig` directly at the public boundary
3. define an environment-boundary input struct with the exact current
   `SessionConfig` fields

Decided:

- choose `3`

Why:

- `1` is too weak and recreates the hidden-session-input bug
- `2` couples the shared libexpr boundary directly to the store-core type
- `3` keeps the boundary explicit and semantically exact without forcing the
  environment header to depend on a store-internal representation decision

The registry seed should also be owned by value rather than shared through a
`shared_ptr<const ...>` alias.

Why:

- the live `TraceSession` path opens from an owned `SemanticRegistry` value and
  then extends the session-local copy with mount points and verified runtime
  roots
- an alias-shaped registry seed obscures that ownership model and invites
  accidental shared-state assumptions

The root loader also needs an explicit capability contract rather than a raw
capturing callback type.

Considered:

1. leave `rootLoader` as a raw `std::function<Value *()>`
2. describe the callback contract only in prose
3. model it as an explicit `RootLoaderCapability`
4. keep the capability but make it hold an owned explicit holder object

Decided:

- choose `4`

Why:

- `1` hides evaluator re-entry behind an unstructured callback
- `2` is too easy to weaken during implementation
- `3` is better than a raw callback but still leaves the ownership model mostly
  implicit
- `4` lets the draft say exactly what the live root-loader paths require while
  also forcing an owned holder object into the type surface:
  - evaluator-thread affinity
  - captured GC-root lifetime
  - permission to allocate/force through the owning evaluator state
  - explicit ownership of the callback-like object rather than arbitrary
    lambda storage

The holder-based capability should also surface the two live preconditions that
were previously only contract text:

1. evaluator-thread affinity
2. captured-root lifetime

Considered:

1. document those as comments only
2. hide them inside the holder implementation
3. surface them as explicit minted resources consumed by
   `buildRootLoaderCapability(...)`

Decided:

- choose `3`

Why:

- `1` and `2` both keep the most fragile part of root loading implicit
- `3` does not fully prove the invariant, but it at least forces the draft API
  to acknowledge the resources the live code is really depending on

Typed session-config builders should also be explicit about the currently
intentional divergence between flake and file-eval construction.

Considered:

1. keep session-config construction open-coded in `flake.cc` and
   `installable-attr-path.cc`
2. use one generic builder that takes mostly precomputed fields and little
   semantic guidance
3. use two typed builders mirroring the two live construction modes

Decided:

- choose `3`

Why:

- `1` preserves the current drift surface
- `2` is only cosmetically better if the hard semantic choices still live at
  every call site
- `3` captures the intentional divergence directly:
  - flake: has `graphDigest`, omits `externalRoots`, uses flake recovery key
  - file-eval: omits `graphDigest`, sets `externalRoots = {repoRoot}`, derives
    stable recovery from policy unless the design later changes

Typed session reuse-key builders should also mirror the live divergence.

Considered:

1. derive reuse keys ad hoc at each call site
2. derive reuse keys generically from session config alone
3. keep dedicated reuse-key builders for the live flake and file-eval paths

Decided:

- choose `3`

Why:

- `1` preserves the current drift surface
- `2` is false for the current file-eval path, whose reuse key includes command
  identity, auto-args, lookup-path identity, store dir, and current system
- `3` matches the live code:
  - flake reuse keys derive from the semantic session key
  - file-eval reuse keys derive from the broader command/session identity

The draft also needs one higher-level assembly layer above the narrower config,
reuse, and authority builders.

Considered:

1. require flake and file-eval call sites to invoke the config, reuse, open,
   and authority builders separately
2. collapse everything into one monolithic `openEvalSession(...)` helper and
   lose the smaller reusable builders
3. add one higher-level assembler per live call-path family and keep the
   narrower builders underneath it

Decided:

- choose `3`

Why:

- `1` still leaves the last assembly step open-coded at the call sites, which
  is exactly where live drift has been happening
- `2` hides too much useful structure and makes testing smaller pieces harder
- `3` keeps the smaller builders available, but makes the actual flake and
  file-eval session-open packages a single typed product:
  - lookup-path snapshot
  - exact session config
  - session reuse identity
  - root-loader capability
  - registry/input/mount authority

The flake path also needs a stronger authority input than the generic common
authority bundle.

Considered:

1. keep passing preassembled registry/input/mount bindings into the assembler
2. collapse the whole flake resolved-graph model into opaque caller-built
   helper state
3. pass typed flake graph node authority inputs and let the assembler derive
   the node-key-based bindings

Decided:

- choose `3`

Why:

- `1` leaves the most correctness-sensitive flake graph transformation
  open-coded at the call site
- `2` hides too much and makes review harder
- `3` lets the assembler own the exact live transformation from flake graph
  nodes into:
  - registry forward entries
  - mount-point bindings
  - input-accessor bindings

The file-eval reuse-key builder should also consume only lookup-path identity,
not the richer per-entry provenance/context structure used by effectful path
resolution.

Considered:

1. reuse the full lookup-path entry type in file-eval reuse-key requests
2. reuse the full type but ignore provenance/context fields by convention
3. define a smaller lookup-path identity type for reuse-key construction

Decided:

- choose `3`

Why:

- `1` invites accidental hashing drift once provenance/context fields evolve
- `2` keeps the distinction informal
- `3` matches the live `computeFileEvalIdentity(...)` path, which hashes only
  prefix/path identity, not origin or realized string context

File-eval reuse-key building should also expose the current uncacheable cases
explicitly rather than collapsing them to a bare `nullopt`.

Considered:

1. return `std::optional<Hash>`
2. throw for uncacheable inputs
3. return a typed result carrying either a key or an uncacheable reason

Decided:

- choose `3`

Why:

- `1` loses why the input was not cacheable
- `2` is the wrong abstraction because “uncacheable” is ordinary control flow
  in the current implementation
- `3` matches the live distinctions:
  - stdin input
  - missing/uncacheable auto-args identity

### 4.2.2 Explicit call modes

The boundary exposes three public overload families per operation:

1. `method(request)` — **auto-dispatch** (primary public API)
   - tries `tryBindCurrentEvalSession()` first (session → Bound → recording)
   - falls back to `openDetachedEffectScope()` (no recording)
   - most call sites use this exclusively
2. `method(ObserveOnlyTag, request)` — read-only observation
   - verifier/recovery dep-resolution and doc-string loading
3. `method(DetachedEffectScope &, request)` — explicit detached
   - shared libexpr/libflake side effects where the caller holds a scope
     across multiple operations (flake graph resolution, fetch pipelines)

Two additional overload families are **private** implementation details:

4. `method(BoundEffectScope &, request)` — session-recording
   - delegates to `withRecordingAccess` → TraceAccess overload
5. `method(const eval_trace::TraceAccess &, request)` — direct recording
   - the core recording implementation; Bound overloads delegate here
   - also used internally by auto-dispatch via the Bound path

The 3-tier call-site dispatch pattern (check TraceAccess → session →
detached) that previously appeared at ~25 call sites is eliminated.
`tryBindCurrentEvalSession()` succeeds whenever `TraceAccess::current()`
would (both require an active `currentTraceSession()`), and
`withRecordingAccess` inside the Bound path finds and reuses the active
TraceAccess automatically. The auto-dispatch method therefore only needs
2-tier dispatch (session → detached) internally.

All Bound overloads delegate through their TraceAccess sibling (where one
exists) to ensure enrichment logic (e.g., `resolveProvenanceViaRegistry`
in `readFile(TraceAccess)`) is always applied.

Legality rule:

1. `ObserveOnlyTag`
   - only for verifier/recovery-style read-only observation
   - never for operations that may materialize fetches, mutate allowlists, or
     publish runtime roots
2. `DetachedEffectScope`
   - for shared libexpr side effects when no bound recording session is active
   - may mutate store/access-control/mount state
   - must not publish trace-session runtime-root state
3. `BoundEffectScope`
   - required for ordinary evaluator effects whenever eval-trace recording is
     active
   - required for any trace-bound publication path

### 4.2.3 Ownership And Lifetime

The environment must also have one explicit lifetime/ownership rule.

Considered:

1. one long-lived `EvalEnvironment` owned per `EvalState`
2. separate `EvalEnvironment` objects constructed from explicit authority,
   with persistent shared effect/session state living behind the authority
   boundary rather than on `EvalState`
3. evaluator-only environment, with separate shared-owner logic for flake code

Decided:

- choose `2`

Why:

- flake graph resolution, `fetchTree`, and evaluator code all need to share the
  same access-control, mount, store, and input-cache authority
- the rewrite must not reintroduce `EvalEnvironment` as an `EvalState` member
  after explicitly rejecting that architecture
- the shared state still has to persist somewhere, but it should persist behind
  explicit authority/factory objects, not as a new ambient subsystem on
  `EvalState`
- `3` fails the main architectural requirement that dependency-producing shared
  `libexpr` / `libflake` effect flows must pass through the same boundary

Operational rule:

- `EvalEnvironment` is not an `EvalState` member
- callers construct `EvalEnvironment` from explicit `EvalEnvironmentAuthority`
- persistent shared effect/session state lives behind the authority boundary:
  - `lookupPathResolved`
  - `srcToStore`
  - `importResolutionCache`
  - trace-session reuse / detached mount-session bookkeeping
- if a persistent shared cache or session-reuse map is still needed during the
  rewrite, it must live behind the environment/factory boundary rather than as
  direct `EvalState` methods or fields
- current bound-session rebinding during traced evaluation should come from the
  active traced-evaluation context, not from `EvalState::lookupTraceSession(...)`

Why:

- these caches track effectful resolution, store publication, and session reuse
- leaving them ambient on `EvalState` would recreate a second hidden effect
  owner beside the explicit environment boundary

### 4.2.5 Detached Mutation Promotion Rule

Detached shared-caller mutation needs one explicit semantic boundary.

Rule:

1. detached mutation may update shared environment state:
   - access-control / allowlist state
   - storeFS mounts
   - materialized-fetch caches
2. detached mutation must not silently mutate active bound trace-session
   semantic state:
   - no implicit registry updates on the active session
   - no implicit runtime-root publication
   - no implicit session-cache replacement
3. promotion into trace-session-visible state is explicit:
   - flake graph work promotes through `EvalTraceSessionAuthority` at
     `openEvalSession(...)`
   - detached graph completion stays detached via `completeGraphFetch(...)`
   - bound runtime-fetch work promotes through
     `completeLockedRuntimeFetch(BoundEffectScope &&, ...)` or
     `completeUnlockedRuntimeFetch(BoundEffectScope &&, ...)`

Why:

- current phase-1 flake graph work and phase-2 evaluator work both need access
  to the same mount/store authority
- but only the bound session should mutate semantic trace-session state
- this prevents detached shared work from becoming a hidden second session
  mutation channel

Concrete current `fetchTree` mapping:

1. detached flake graph / shared loading path
   - `resolveFetchIdentity(DetachedEffectScope &, ...)`
   - `materializeFetch(DetachedEffectScope &, ResolvedFetchIdentity &&)`
   - may `mountGraphFetchedInput(DetachedEffectScope &, ...)`
   - must `completeGraphFetch(DetachedEffectScope &, ...)` before the mounted
     result is eligible for later session-authority promotion
   - may update shared environment allowlist/mount state
   - uses flake-graph/node-key identity when later promoted into
     `EvalTraceSessionAuthority`
   - must not:
     - publish runtime roots
     - mutate active session registry
     - record `RuntimeFetchIdentity`
   - promotion into session-visible graph state happens only later through
     `EvalTraceSessionAuthority` at `openEvalSession(...)`

1a. detached standalone mount path
   - `resolveFetchIdentity(DetachedEffectScope &, ...)`
   - `materializeFetch(DetachedEffectScope &, ResolvedFetchIdentity &&)`
   - may `mountFetchedInput(DetachedEffectScope &, ...)`
   - the detached standalone mounted result is terminal
   - it must not flow into `completeGraphFetch(...)`
   - it must not be treated as session-authority input later unless rebuilt
     through the graph-specific path

2. bound locked `fetchTree`
   - `resolveFetchIdentity(BoundEffectScope &, ...)`
   - `materializeFetch(BoundEffectScope &, ResolvedFetchIdentity &&)`
   - `mountFetchedInput(BoundEffectScope &, ...)`
   - `completeLockedRuntimeFetch(BoundEffectScope &&, ...)` owns the post-mount flow:
     - mount-point provenance registration
     - runtime-root candidate construction
     - runtime-root validation/publication when a NAR hash exists
     - runtime store-path-availability observation/dependency
   - returns a replacement bound-session capability because runtime-root
     publication may mutate session-visible state
   - uses runtime-root/locked-URL identity, not flake node-key identity
   - do not record `RuntimeFetchIdentity`

3. bound unlocked `fetchTree`
   - `resolveFetchIdentity(BoundEffectScope &, ...)`
   - `materializeFetch(BoundEffectScope &, ResolvedFetchIdentity &&)`
   - `mountFetchedInput(BoundEffectScope &, ...)`
   - `completeUnlockedRuntimeFetch(BoundEffectScope &&, ...)` owns the post-mount flow:
     - mount-point provenance registration if needed by the session model
     - `RuntimeFetchIdentity` recording
     - no runtime-root publication
   - returns the replacement/preserved bound-session capability explicitly

Design rule:

- callers should not manually sequence:
  - `mountInput`
  - registry-mount publication
  - runtime-root publication
  - runtime store-dep recording
  - `RuntimeFetchIdentity` recording
- post-mount runtime-fetch handling belongs to one environment-owned completion
  operation
- detached graph completion and bound runtime completion are different
  operations because only the bound path is allowed to produce:
  - runtime-root candidates
  - runtime store-path deps
  - `RuntimeFetchIdentity`
  - replacement bound-session capabilities

The mounted-input carriers should also stay distinct by effect mode.

Considered:

1. keep one `MountedInput` carrier for all detached and bound callers
2. keep one detached carrier plus one bound carrier and rely on the later
   completion API to police graph-vs-standalone semantics
3. split detached standalone, detached graph, and bound mounted inputs in the
   type surface

Decided:

- choose `3`

Why:

- `1` and `2` both preserve an easy bypass where detached callers can wander
  into bound-only completion semantics by mistake
- `3` makes the split structural:
  - detached graph mount output feeds `completeGraphFetch(...)`
  - detached standalone mount output is terminal
 - bound mount output feeds either `completeLockedRuntimeFetch(...)` or
   `completeUnlockedRuntimeFetch(...)`

### 4.2.4 Residual `EvalState` Usage

The design must also state whether `EvalEnvironment` itself is allowed to keep
reaching back into `EvalState`.

Considered:

1. keep `EvalState &` inside `EvalEnvironment` and rely on discipline
2. allow a narrow residual allowlist of direct `EvalState` reach-throughs
3. make `EvalEnvironment` own no persistent general-purpose `EvalState`
   reference at all

Decided:

- choose `3`

Why:

- `1` recreates the ambient-authority problem under a nicer name
- `2` is easy to weaken during implementation and hard to police in review
- `3` forces evaluation-specific semantics to enter through:
  - request-builder outputs
  - explicit authority objects
  - caller-supplied callbacks such as `rootLoader`

Residual-usage rule:

- `EvalRequestBuilder` may depend on `EvalState` for value forcing/coercion and
  evaluator-local interpretation
- `EvalEnvironment` itself should not store or consult a general-purpose
  `EvalState &`
- if an implementation step seems to require direct `EvalState` access inside
  `EvalEnvironment`, that is a design bug until the needed input is moved into:
  - `EvalEnvironmentAuthority`
  - `EvalTraceSessionAuthority`
  - the typed request surface
  - or an explicit callback capability

### 4.3 Request-builder layer

Between `EvalState` and `EvalEnvironment`, keep a thin evaluator-side request
compiler.

It should:

- interpret `Value` inputs
- perform pure or evaluator-local coercion
- preserve path/string/provenance semantics that occur before the first real
  side effect
- build typed effect requests for the environment

It should not:

- read from disk
- touch the store
- fetch from the network
- record deps
- publish runtime roots

This layer exists specifically so the environment boundary does not start too
late.

### 4.4 `TraceStore`

`TraceStore` should own only persistent cache semantics:

- verification
- recovery
- trace/result/dep-key-set storage
- runtime-root persistence/loading

It should not be wrapped in a message-protocol shell just because it is
currently in-process.

### 4.5 Value behavior layer

Keep `Value` compact, but move kind-specific behavior into typed wrappers such
as:

- `StringValueRef`
- `PathValueRef`
- `ListValueRef`
- `AttrsValueRef`
- `CallableValueRef`
- `ExternalValueRef`

These wrappers should:

- validate tag/layout once
- expose kind-specific APIs
- host coercion and observation helpers
- avoid repeating value-kind branching in primops

### 4.6 Primop organization

Split `primops.cc` by domain, with `primops.cc` reduced to registration/wiring.

Suggested domain files:

- `primops-env.cc`
- `primops-path.cc`
- `primops-store.cc`
- `primops-fetch.cc`
- `primops-json.cc`
- `primops-string.cc`
- `primops-attrs.cc`
- `primops-list.cc`

## 5. `EvalEnvironment` Public Shape

The public API should be capability- and observation-oriented.

### 5.1 Public responsibilities

`EvalEnvironment` should provide grouped operations for:

1. environment
2. path realization / coercion follow-through
3. filesystem reads
4. directory reads
5. store observation / path availability / closure allowlisting
6. fetch/materialize/mount
7. runtime-root publish/load
8. dependency recording for effectful observations
9. shared non-evaluator libexpr effect flows that currently bypass the
   evaluator path, especially flake graph resolution / mount reuse

### 5.2 Return typed observations, not raw values

Each effectful API should return a typed observation object, not just a raw
string/path/blob.

Examples:

- `EnvVarObservation`
- `FileReadObservation`
- `DirectoryReadObservation`
- `StorePathObservation`
- `FetchedInput`
- `DetachedStandaloneMountedInput`
- `DetachedGraphMountedInput`
- `BoundLockedMountedInput`
- `BoundUnlockedMountedInput`
- `GraphFetchCompletion`
- `PublishedRuntimeFetch`

These types should carry both:

- the concrete result needed by the caller
- the semantic/provenance information required for recording or replay

This makes it much harder for callers to “do the effect but forget the dep”.

### 5.3 Suggested internal segmentation

To avoid a god object, `EvalEnvironment` should be one public owner with small
internal facets, for example:

- `EnvFacet`
- `PathFacet`
- `StoreFacet`
- `FetchFacet`
- `TraceFacet`

But those facets should remain implementation details. Public callers in
`libexpr` / `libflake` should see one boundary.

### 5.4 Concrete API draft

The authoritative API surface is the draft header family, not this plan text:

- [eval-environment.hh](/home/connorbaker/nix/src/libexpr/include/nix/expr/eval-environment.hh)
- [environment.hh](/home/connorbaker/nix/src/libexpr/include/nix/expr/eval-environment/environment.hh)
- [capabilities.hh](/home/connorbaker/nix/src/libexpr/include/nix/expr/eval-environment/capabilities.hh)
- [domains.hh](/home/connorbaker/nix/src/libexpr/include/nix/expr/eval-environment/domains.hh)
- [authority-internal.hh](/home/connorbaker/nix/src/libexpr/include/nix/expr/eval-environment/authority-internal.hh)
- [session-types.hh](/home/connorbaker/nix/src/libexpr/include/nix/expr/eval-environment/session-types.hh)
- [request-types.hh](/home/connorbaker/nix/src/libexpr/include/nix/expr/eval-environment/request-types.hh)
- [observation-types.hh](/home/connorbaker/nix/src/libexpr/include/nix/expr/eval-environment/observation-types.hh)
- [request-builder.hh](/home/connorbaker/nix/src/libexpr/include/nix/expr/eval-environment/request-builder.hh)
- flake-side adapter: [eval-trace-session-open-adapter.hh](/home/connorbaker/nix/src/libflake/include/nix/flake/eval-trace-session-open-adapter.hh)

Section 5.4 therefore records only the contract for that header family:

- pure value interpretation stays in `EvalState` and value-view helpers
- effect execution starts in `EvalEnvironment`
- `EvalEnvironment` must not own `force*`, pure `Value` classification, or
  pure path/string interpretation that does not perform I/O
- request construction is builder-owned rather than caller-forged
- session-open authority and reuse assembly are builder / assembler owned
- detailed type definitions live in the headers above, not duplicated here

If the headers and this plan disagree, the headers are the source of truth and
the plan should be updated to reference the new reality rather than carrying a
second pseudo-definition.

### 5.5 Ownership rule for current call sites

The current code implies the following split.

Keep in `EvalState` / value views / request-builder:

- `coerceToPath`
- `coerceToCoercedPath`
- `coerceToStorePath`
- string/path/context coercion
- value forcing

Move to `EvalEnvironment`:

- `getEnv`
- `findFile`
- `resolveLookupPathPath`
- `checkURI`
- `allowPath`
- `allowClosure`
- `allowAndSetStorePathString`
- `mountInput`
- `readFile`
- `readDirectory`
- path existence / stat-style observation
- generic `realiseContext`
- runtime-root publication
- fetch materialization / mounted-input realization
- flake graph mount/reopen flows that currently call `mountInput` directly

Ownership/lifetime rule:

- `EvalEnvironment` is constructed from explicit authority, not retained as an
  `EvalState` subsystem
- shared flake/loading code and evaluator code may construct separate
  `EvalEnvironment` facade objects over the same explicit authority/session
  state
- there is no second hidden effect owner on `EvalState`

Deletion / relocation rule for the remaining side channels:

Delete or relocate the following out of `EvalState` / `TraceRuntime`.
Items 7–10 below have been removed from the tree; the remaining six
are still open.

1. `EvalState::lookupTraceSession`
   - move to environment-owned session reuse
2. `EvalState::rememberTraceSession`
   - move to environment-owned session reuse
3. `EvalState::recordRuntimeRoot`
  - replace with environment-owned `completeLockedRuntimeFetch(...)` /
    `completeUnlockedRuntimeFetch(...)`
4. `EvalState::addRegistryMountPoint`
   - replace with detached-to-session promotion through
     `EvalTraceSessionAuthority` or explicit bound-session publication
5. `TraceRuntime::lookupSession`
6. `TraceRuntime::rememberSession`
7. ~~`TraceRuntime::addRegistryMountPoint`~~ (removed)
8. ~~`TraceRuntime::recordRuntimeRoot`~~ (removed; runtime-root
   publication is now `TraceSession::traceBackend()->recordRuntimeRoot`)
9. ~~`TraceRuntime::activeBackend_`~~ (removed)
10. ~~`TraceRuntime::activeRegistry_`~~ (removed)

Why (for the surviving items):

- these methods and fields currently form a second mutable authority path
- leaving them in place would directly violate the single-owner rule

### 5.6 Required typed relationships

The API should encode these relationships explicitly:

1. request types are minted by `EvalRequestBuilder`, not by arbitrary callers.
   - the boundary should be structurally hard to bypass, not convention-backed

2. `CoercedPathRequest` and `StorePathPublishRequest` are the only normal
   inputs to path/store-path publication effects.
   - no raw `SourcePath + optional<PathObject>` pairs repeated across call sites

3. `LookupPathRequest`, `UriPolicyRequest`, and `RealiseContextRequest`
   are first-class.
   - lookup-path resolution, URI checks, and context realization must not
     remain ambient `EvalState` behavior
   - `RealiseStringRequest` was removed (dead code — `EvalState::realiseString`
     composes `realiseContext` + `rewriteStrings` directly)
   - `LookupPathRequest` must be able to represent both:
     - the session/default lookup path snapshot
     - an explicit `builtins.findFile` search-path list with per-entry origin
     - per-entry string context that may need realization
   - `LookupPathObservation` must carry enough information to drive dependency
     recording from the returned observation:
     - environment lookups such as `NIX_PATH`
     - per-entry resolution effects
     - access-control initialization side effects
     - whether `corepkgs` fallback was used

4. `FetchedInput`, `DetachedStandaloneMountedInput`,
   `DetachedGraphMountedInput`, `BoundLockedMountedInput`, and
   `BoundUnlockedMountedInput` must be distinct types.
   - fetching is not mounting
   - detached mounting is not bound mounting
   - locked bound mounting is not unlocked bound mounting
   - detached graph mounting is not detached standalone mounting
   - mounting is not runtime-root publication

5. bound runtime-fetch completion must return a replacement session capability
   when it mutates session-visible state.
   - session mutation is single-owner
   - success returns the replacement bound state

6. fetch identity observation, fetch materialization, and mount must be
   separate typed phases
   - read-only fetch identity is not materialization
   - materialization is not mount
   - mount is not runtime-root publication
   - fetch identity observation must not return mounted/store-path state

7. post-mount runtime-fetch handling is one environment-owned completion phase.
   - callers must not manually sequence registry publication, runtime-root
     publication, runtime store-dep recording, and `RuntimeFetchIdentity`
     recording

7a. detached mount reuse that mounts an already-known store path must be a
    first-class environment operation.
   - current shared flake/reopen flows cannot be forced through
     fetch-to-store-shaped APIs when they only need:
     - allowlist/update access control
     - mount an accessor at an existing store path

7b. session open must consume one assembled package by value.
   - the move-only root-loader capability and session reuse key must flow
     through the same typed package
   - callers must not pass request/authority pieces separately and then
     re-derive reuse/session ownership out of band

7c. file-eval pre-session Git identity observation must be legal in detached
    mode.
   - current file-eval session construction observes Git identity before the
     bound session exists
   - the boundary must represent that directly rather than forcing verifier-only
     or post-session misuse

8. store authorization is split by operation.
   - `authorizeStorePath` is not `authorizeStoreClosure`
   - `publishStorePath` models `builtins.storePath` end to end
   - `renderAuthorizedStorePath` is not a generic policy call

9. effect mode is explicit in the type system.
   - `ObserveOnlyTag` is only for verifier/recovery-style read-only observation
   - `DetachedEffectScope` is for shared libexpr mutation outside a bound trace
     session
   - `BoundEffectScope` is for recording/publication paths

10. `EvalEnvironment` does not retain a general-purpose `EvalState &`.
    - evaluator-specific semantics enter through the request builder
    - session-specific evaluator re-entry enters through `rootLoader`
    - no ambient trace/runtime helper reach-through remains

### 5.7 Where dependency recording happens

The environment boundary should centralize effect observation, but it must not
hide semantic mode distinctions.

The required rule is:

- effect methods always produce typed observations
- if a bound recording session is active, the environment may publish the
  corresponding dep internally
- verification/recovery may use the same effect surface to compute current
  observations without going through evaluator-side recording

This avoids the bad design where “calling the method” and “recording a dep” are
silently the same thing in all modes.

### 5.8 Non-goals for the first API draft

The first `EvalEnvironment` rewrite should not try to absorb:

- pure `Value` forcing
- attrset/list semantics
- all of `SemanticHandle` publication
- every trace-store algorithm entry

Those are separate follow-on bundles.

## 6. Type-Level Enforcement

The rewrite should keep the type-level machinery that prevents real drift and
drop the machinery that only preserves an in-process protocol fiction.

### 6.1 Keep

- nominal IDs and hash types
- GDP/capability patterns for scoped or high-blast-radius invariants
- ordinary move-only wrappers for non-copyable reusable capabilities
- linear consumption where ownership replacement matters
- opaque bound-session capabilities

### 6.2 Drop or collapse

- session-typed in-process protocol layers if replaced by direct typed calls
- message-protocol wrappers whose only purpose is dispatching to in-process code
- stateless builder classes whose only purpose is reaching private constructors
- fake proof-like tokens that carry no scoped evidence or non-forgeable
  authority

### 6.3 New capability boundaries

Important opaque capability types should likely include:

- `DetachedEffectScope`
- `BoundEffectScope`
- `RootLoaderCapability`
- `PublishedRuntimeFetch`
- `DetachedStandaloneMountedInput`
- `DetachedGraphMountedInput`
- `BoundLockedMountedInput`
- `BoundUnlockedMountedInput`
- `VerifiedObservation` or equivalent where necessary

Rules:

1. `EvalState` cannot forge them.
2. Primops and shared `libflake` callers cannot bypass them.
3. Detached mutation, recording/publication, and session replacement remain
   distinct typed paths.
4. Linear capability tokens must be minted as late as possible and consumed in
   separate statements, not threaded through throw-prone argument evaluation.

### 6.4 Where semantics must be captured

The draft must distinguish between:

1. semantics already carried by the type surface
2. semantics still only described by comments or loose field conventions

Semantics that should be carried directly by types:

- ownership / authority:
  - `DetachedEffectScope`
  - `BoundEffectScope`
  - `RootLoaderCapability`
  - assembled session-open products
- lifecycle / phase order:
  - `ResolvedFetchIdentity -> FetchedInput -> MountedInput -> completion`
  - session replacement after runtime-root publication
- namespace separation:
  - flake node-key identity
  - runtime-root URL identity
  - session reuse keys vs recovery keys vs policy digests
- publication semantics:
  - preserve provenance
  - detach provenance
  - plain synthesized output
- result-shape distinctions:
  - detached standalone mount vs detached graph mount vs bound mount
  - locked runtime fetch vs unlocked runtime fetch
  - lookup-path existing-path vs downloaded-pseudo-url vs hook-resolved-path

Semantics that must not remain only as raw strings / booleans / comments:

- `nodeKey`
- `sourceIdentity`
- `stableRecoveryKey`
- `lockedUrl`
- `narHash`
- lookup-path entry prefixes and raw values when used for identity / reuse-key
  construction
- runtime-root `sourceId`
- file-eval `expressionHash`
- file-eval `autoArgsIdentity`
- `storeDir`
- `currentSystem`
- any boolean or enum whose value materially changes allowed follow-on
  operations rather than only describing data

Required strengthening rules:

1. Distinct semantic string/hash domains should become nominal `Tagged<Tag, T>`
   aliases rather than plain `std::string` or untagged hash values.
1a. Source-identity namespaces should use named factories and disjoint nominal
    types rather than one generic “registered identity” string domain.
2. Multi-step effect flows whose states must be consumed exactly once should use
   `Linear` typestate rather than only move-only DTOs.
2a. One-shot session-open assembly/open carriers should also be `Linear`
    typestate when dropping them would hide a real bug.
3. Precondition-style facts that matter only inside a scoped continuation should
   use GDP proofs.
4. High-blast-radius facts that escape the proving scope should use opaque
   capability objects with private constructors, not public proof tags.
5. Observation/result records with mutually exclusive shapes should use
   `std::variant` or distinct carrier types rather than “one struct plus many
   optional fields” where possible.
6. Variant-driven semantics should prefer exhaustive enum/variant dispatch over
   comment-based conventions.
7. Publication/coercion modes that change provenance semantics should use sealed
   result or capability types rather than plain booleans or render-mode enums.
8. If the environment grows async internals, blocking must stay inside the
   existing typed `syncAwait` discipline; no raw `future.get()`-style escape
   hatches.

Current draft consequence:

- nominal semantic domains belong in `eval-environment/domains.hh`
- publication results should use sealed typed carriers such as
  `PublishedStorePathString`, with direct construction staying internal to
  `EvalEnvironment` and the implementation kept to one sealed record shape
- must-consume fetch/mount/session-open states should be `Linear`-backed
  classes, not plain move-only structs
- `CapturedSessionOpenInputs` should be consumed at assembly time; the final
  assembled session-open product should carry only the authority/reuse token
  needed by `openEvalSession(...)`, not a second copy of already-consumed
  session-open inputs
- linear intermediate carriers should avoid public raw-state accessors when the
  intended use is only the next typed transition; otherwise they recreate a
  bypass around the environment boundary
- if a publication result is still directly constructible, it is not a sealed
  publication boundary yet, only a convenience wrapper around a typed result
- reusable handles/scopes that may be dropped normally should be plain
  move-only wrappers rather than `Linear`
- one-shot capability tokens that participate in a required assembly/open
  transition, such as `RootLoaderCapability`, should be `Linear` rather than
  plain `MoveOnly`
- locked versus unlocked runtime completion and lookup-path resolution variants
  should use distinct carriers or `std::variant`, not “enum + optionals”
- behavior-changing request booleans should become named modes such as
  `LookupPathAccessControlMode` and `StringRealisationMode`
- captured evaluator policy flags should become named modes such as
  `EvalPurityMode`, `EvalRestrictionMode`, and `ImportFromDerivationMode`

## 7. Concrete File Ownership

### 7.1 New / primary files

Suggested new files:

- `src/libexpr/include/nix/expr/eval-environment.hh`
- `src/libexpr/include/nix/expr/eval-environment/fwd.hh`
- `src/libexpr/include/nix/expr/eval-environment/capabilities.hh`
- `src/libexpr/include/nix/expr/eval-environment/domains.hh`
- `src/libexpr/include/nix/expr/eval-environment/authority-internal.hh`
- `src/libexpr/include/nix/expr/eval-environment/session-types.hh`
- `src/libexpr/include/nix/expr/eval-environment/request-types.hh`
- `src/libexpr/include/nix/expr/eval-environment/observation-types.hh`
- `src/libexpr/include/nix/expr/eval-environment/request-builder.hh`
- `src/libexpr/include/nix/expr/eval-environment/environment.hh`
- `src/libexpr/eval-environment.cc`
- `src/libflake/include/nix/flake/eval-trace-session-open-adapter.hh`
- `src/libexpr/include/nix/expr/value-views.hh`
- `src/libexpr/value-views.cc`

Suggested shared-caller integration touch points:

- [src/libflake/flake.cc](/home/connorbaker/nix/src/libflake/flake.cc)
- [src/libflake/include/nix/flake/flake.hh](/home/connorbaker/nix/src/libflake/include/nix/flake/flake.hh)

### 7.2 Existing files to shrink

Primary shrink targets:

- [eval.hh](/home/connorbaker/nix/src/libexpr/include/nix/expr/eval.hh)
- [eval.cc](/home/connorbaker/nix/src/libexpr/eval.cc)
- [primops.cc](/home/connorbaker/nix/src/libexpr/primops.cc)
- [context.hh](/home/connorbaker/nix/src/libexpr/include/nix/expr/eval-trace/context.hh)
- [context.cc](/home/connorbaker/nix/src/libexpr/eval-trace/context.cc)

### 7.3 Existing files to keep as semantic core

Keep as primary eval-trace semantic core:

- [trace-store.hh](/home/connorbaker/nix/src/libexpr/include/nix/expr/eval-trace/store/trace-store.hh)
- [trace-store.cc](/home/connorbaker/nix/src/libexpr/eval-trace/store/trace-store.cc)
- [trace-store-verify.cc](/home/connorbaker/nix/src/libexpr/eval-trace/store/trace-store-verify.cc)

## 8. Explicit Deletion / Collapse Targets

Unless a real out-of-process boundary emerges, the rewrite should delete or
collapse the following layers:

- `VerificationOrchestrator`
- `verification-protocol.hh`
- ~~`trace-store-protocol.hh`~~ (already removed)
- `async-runtime.hh` if it no longer owns real async work

`TraceBackend` should either:

- shrink to a tiny null/direct adapter seam, or
- be removed if only one real backend remains

`TraceRuntime` should be reduced to evaluator-local state that genuinely cannot
live in `EvalEnvironment` or the bound session.

## 9. Rewrite Sequence

### Bundle 1: Introduce `EvalEnvironment`

Goal:

- add `EvalEnvironment`
- add `EvalEnvironmentAuthority` and `DetachedEffectScope`
- route a first narrow set of effectful calls through it
- do not yet rewrite everything

Initial operations to move:

- env lookup
- path allowlisting / closure allowlisting
- input mounting
- runtime-root publication entry points
- constructor/setup authority that currently lives implicitly on `EvalState`

Exit condition:

- new effectful code in `EvalState`, primops, or shared flake-loading code is no
  longer added directly
- no new ambient authority inputs are added to the environment/session setup path

### Bundle 2: Move filesystem/store/fetch effects behind the environment

Move:

- file reads
- directory reads
- store-path checks
- fetchTree materialization flow
- provenance-bearing path realization
- shared flake mount/reopen flows in [src/libflake/flake.cc](/home/connorbaker/nix/src/libflake/flake.cc)

Concrete migration obligations:

1. rewrite [fetchTree.cc](/home/connorbaker/nix/src/libexpr/primops/fetchTree.cc)
   so the live sequence becomes:
   - `checkURI` through `EvalEnvironment`
   - `resolveFetchIdentity(...)`
   - `materializeFetch(...)`
   - `mountFetchedInput(...)`
  - `completeLockedRuntimeFetch(...)` / `completeUnlockedRuntimeFetch(...)`
2. no direct `inputCache->getAccessor(...)` remains in
   [fetchTree.cc](/home/connorbaker/nix/src/libexpr/primops/fetchTree.cc)
3. no direct `mountInput(...)`, `addRegistryMountPoint(...)`,
   `recordRuntimeRoot(...)`, or `recordRuntimeStoreDep(...)` remains in
   [fetchTree.cc](/home/connorbaker/nix/src/libexpr/primops/fetchTree.cc)
4. shared flake session-open code in
   [flake.cc](/home/connorbaker/nix/src/libflake/flake.cc) stops building
   `mountToInput`, `inputAccessors`, and `registryEntries` directly and instead
   routes `LockedFlake` through the flake-local adapter
5. detached shared flake mount/reopen paths use either:
   - `ensureMountedStorePath(...)`, or
   - `resolveFetchIdentity(...) -> materializeFetch(...) -> mountGraphFetchedInput(...)`
6. `resolveFetchIdentity(...)` implementations are invalid if they:
   - call `fetchToStore`
   - mount `storeFS`
   - mutate path/closure allowlists
   - mutate shared cached accessor fingerprints in place
   - publish runtime-root or runtime-store dependency side effects
7. flake loading preserves a dual-ref invariant:
   - `lockedRef` is the live evaluation ref/source view used to evaluate the
     current flake tree
   - `lockFileRef` is the ref persisted into `flake.lock`
   - those may differ for top-level local working trees with `inputs.self`
     attrs, because evaluation must stay on the live working tree while lock
     file persistence must stay tied to the last stable locked source
8. flake loading preserves a dual-root invariant:
   - `logicalRoot` is the live logical source root used for relative-path
     resolution and lock-file location
   - `carrierRoot` is the source root that bounds relative escapes and backs
     `sourceInfo.outPath`
   - `evaluationRoot` is the exact phase-2 import root used to evaluate
     `flake.nix`
   - store-backed rereads must not collapse those roles back into one
     `SourcePath`

Exit condition:

- `primops.cc`, `fetchTree.cc`, `EvalState`, and shared flake-loading code no
  longer perform raw effectful reads or mounts without going through
  `EvalEnvironment`

Current implementation status:

- completed:
  - `fetchTree.cc` now uses `EvalEnvironment` for URI authorization, fetch
    resolution/materialization/mount, and runtime completion
  - detached flake mount/reopen helpers now use `EvalEnvironment`
    (`resolveFetchIdentity(...)`, `materializeFetch(...)`,
    `mountFetchedInput(...)`, `ensureMountedStorePath(...)`) instead of raw
    `inputCache->getAccessor(...)`, `mountInput(...)`, `allowPath(...)`, and
    `storeFS->mount(...)`
  - migrated path/file/store-path operations now realize path context through
    `EvalEnvironment` rather than mixing environment calls with direct
    `EvalState::realisePath(...)` semantics at call sites
  - `builtins.findFile`, `builtins.pathExists`, `builtins.readFile`,
    `builtins.hashFile`, `builtins.readFileType`, `builtins.getEnv`,
    `builtins.currentSystem`, `builtins.storePath`, `builtins.filterSource`,
    and `builtins.path` now route their environment/store/fetch work through
    `EvalEnvironment`
  - `builtins.exec` now realizes string context through `EvalEnvironment`
    rather than calling `EvalState::realiseContext(...)` directly
  - `builtins.readDir` now resolves and reads directories through
    `EvalEnvironment` rather than open-coding path realization and directory IO
    in `primops.cc`
  - `import` path realization now again follows ancestor symlink resolution and
    `default.nix` fallback before `evalFile` / `scopedImport`, and the
    realization step still goes through `EvalEnvironment`
  - installable single-path coercion now copies paths through
    `EvalEnvironment::copyPathToStore(...)` rather than calling `fetchToStore`
    directly
  - detached trace-aware primop paths (`pathExists`, `readFile`, `hashFile`)
    no longer record deps directly in `primops.cc`; they use explicit
    `TraceAccess` recording targets so the environment remains the owner of
    observation-to-recording translation
  - bound `EvalEnvironment` operations that record observations now do so inside
    an explicit dep-capture scope derived from the supplied
    `BoundEffectScope`
  - detached `copyPathToStore(...)` no longer performs hidden ambient dep
    recording; only the bound and explicit recording entry points may turn
    copy-path observations into recorded deps
  - flake loading no longer open-codes store publication for `nixConfig` path
    values; those path-valued settings now publish through `EvalEnvironment`
  - fetch materialization no longer mutates shared cached accessor
    fingerprints in place; any locked fingerprint needed for store-copy
    caching is carried on the environment-owned fetch token and consumed at
    materialization time
  - verifier-side `EnvironmentLookup`, `SessionSystemValue`,
    `DerivedStorePath`, and `RuntimeFetchIdentity` now resolve through
    `EvalEnvironment`
  - `SessionConfig`, `SemanticSessionKey`, and `stableRecoveryKey` stay typed
    through the in-memory trace-store layer and only become `BLOB` at SQLite
    binding; flake `sourceIdentity` is now only the stable logical identity,
    while version sensitivity is carried by `graphDigest` / verification rather
    than being folded into `sourceIdentity` itself
  - runtime-root/session identities are typed through the same in-memory layer
    and the first persistence boundary is now the trace-store/SQLite layer,
    not ad hoc string conversion at call sites; runtime roots now persist
    canonical runtime-fetch identities instead of locked-URL strings
  - structured dep keys now record through typed key objects
    (`DerivedStorePathDepKey`, `StorePathAvailabilityDepKey`,
    `RuntimeFetchIdentityDepKey`) and only collapse to canonical bytes inside
    dep recording / SQLite binding
  - interned dep/source atoms now persist as opaque `BLOB`s in `Strings.value`
    rather than `TEXT`; `DepSource` internings and runtime-root `source_id`
    use canonical encoded blobs instead of ad hoc sentinel strings
  - runtime-root persistence now stays typed through `TraceBackend` and
    `TraceStore`; `DepSource`, `RuntimeFetchIdentityDepKey`, `Hash`, and
    `StorePath` remain structured until the SQLite binding boundary, with
    `SessionRuntimeRoots.source_id`, `fetch_identity`, and `nar_hash`
    persisted as `BLOB`
  - structured hash preimages touched in this rewrite are now framed
    canonically rather than delimiter-joined
  - bound `EvalEnvironment` operations now record through an explicit
    `TraceAccess` created from the supplied `BoundEffectScope`
  - lookup-path resolution, source-to-store, import-resolution, file-trace,
    and fetcher-input caches now live in `EvalEnvironmentSharedState` rather
    than as separate effect-adjacent `EvalState` members
  - the current tree still needs the final dep-key substrate cleanup and the
    last flake graph/root typing pass; typed persistence rows alone are not
    proof that every public/internal semantic API has been narrowed enough
  - publication sealing is a current implementation fact if the direct
    construction escape hatch is gone; the remaining plan work is representational
    cleanup, not sealing itself
  - import-content dep recording and `importNative` path realization now route
    through `EvalEnvironment`
  - transient runtime dep-key wrappers no longer leak into the public
    `eval-environment` surface
  - session/recovery identity hashing now stays typed through `TraceStore` and
    only becomes `BLOB` at SQLite binding
- remaining:
  - the `fetch()` helper in
    [fetchTree.cc](/home/connorbaker/nix/src/libexpr/primops/fetchTree.cc)
    (backing `builtins.fetchurl` and `builtins.fetchTarball`) still performs
    raw `store->ensurePath`, `fetchToStore`, `downloadFile`, and
    `store->queryPathInfo` without going through `EvalEnvironment`. This is
    an undocumented gap relative to the Bundle 2 exit condition (which
    covers all of `fetchTree.cc`). The concrete migration obligations
    (items 1-8) are met — they target `prim_fetchTree` and shared flake
    flows, not the `fetch()` helper. Decision needed: either migrate
    `fetch()` or explicitly scope it out with rationale.
  - `prim_readFile` reference scanning is now inside
    `EvalEnvironment::readFile` — `FileReadObservation::context` is
    populated with store-path references when the file lives in the store
  - `prim_storePath` `ensurePath` is now reordered after `EvalEnvironment`
    construction
  - verifier-side file/directory/stat-hash computation remains local by design;
    only the effectful resolution/oracle portion moved to the environment
  - session-open assembly helpers have been simplified, but the builder
    surface is still wider than the eventual end state because a few
    compatibility factories remain for out-of-scope callers; it remains an
    internal cleanup target
  - the dep-key/runtime-root API still shares the generic interned-byte
    substrate before final persistence, so the strongest boundary is semantic
    typing plus canonical serialization, not physically separate tables yet
  - verification/session caches still use raw strings for repo roots and mount
    subdirs in a few adapter paths; those are live cleanup seams, not finished
    typing
  - the flake graph/root dual-phase typing is live but still needs the final
    carrier-root / evaluation-root normalization pass at the libflake edge;
    the broad `/flake.nix` collapse is already fixed, so the remaining
    frontier is narrower and centered on local-source handling plus
    `fetchGit`/CA-build cases

### Bundle 3: Collapse `TraceRuntime` side channels

Move to `EvalEnvironment` / bound session:

- active trace session ownership
- runtime-root publication
- registry mount updates that are really session/environment state
- detached effect scope bookkeeping for non-session mount/publish flows

Exit condition:

- no split `activeRegistry_` / `activeBackend_`-style mutable authority remains
- no hidden detached side channel exists beside `DetachedEffectScope`

Current implementation status:

- completed:
  - removed `TraceRuntime::activeRegistry_`
  - removed `TraceRuntime::activeBackend_`
  - removed the old `TraceRuntime` session cache and helper methods
  - moved reusable trace-session factory ownership into the internal
    `TraceSessionFactory` helper adjacent to `EvalEnvironmentAuthority`
  - `EvalState` destructor now explicitly releases per-state session-factory
    state
- remaining:
  - run-time validation that no caller still depends on the old split
    ownership assumptions
  - environment-adjacent caches are now consolidated in
    `EvalEnvironmentSharedState`, but the final long-term ownership of that
    shared state relative to `EvalState` still needs an explicit decision

### Bundle 4: Collapse the in-process eval-trace protocol stack

Delete or shrink:

- service wrappers
- orchestrator wrappers
- session-typed in-process message protocols

Replace with:

- direct typed store/session APIs

Exit condition:

- cache verification and record/recovery calls are normal typed calls in-process

Current implementation status:

- completed:
  - removed `TraceStoreService::serve(...)`
  - removed `TraceStoreService` as a separate wrapper
  - removed `trace-store-protocol.hh`
  - removed `verify-pipeline.hh`
  - removed the old `VerifyPipeline` / `VerifyOrRecover` session-type layer
  - removed `OrchestratorHandle<Unbound/Bound>`
  - reduced `verification-protocol.hh` to shared runtime outcome/stage
    definitions
  - removed protocol-only tests that exercised the deleted channel shell
  - removed unused `io_context` plumbing from `VerificationOrchestrator`
  - verifier-side environment/system/derived-store-path/runtime-fetch
    observation now routes through `EvalEnvironment` instead of open-coded
    direct observation
- remaining:
  - re-evaluate whether `VerificationOrchestrator` still needs to be a
    separate wrapper once the remaining prefetch/session state is isolated
  - local stat-hash / structured-content computation remains in dep resolution
    by design; only the effectful observation and path-resolution layer moved
  - the current tree still has an open invalidation frontier in the
    eval-trace impure/core cluster, so protocol collapse is not proof that the
    verification semantics are fully closed

### Bundle 5: Introduce `Value` views

Add typed non-owning wrappers and move:

- coercion helpers
- kind-specific access
- value-family operations

Exit condition:

- large free-floating value-manipulation helpers are substantially reduced

### Bundle 6: Split primops by domain

Reduce `primops.cc` to registration/wiring and move implementations into domain
files.

Exit condition:

- effectful primop code is domain-owned and routes through `EvalEnvironment`

## 10. Validation Requirements

The rewrite is only done if it is structurally hard to bypass the new
environment boundary.

These are end-state acceptance criteria only, not claims about the current tree.
Where the current implementation still falls short, the Bundle status sections
above and the current implementation notes below are the authoritative progress
report.

Current implementation snapshot:

- session/recovery identities are typed through the trace-store in-memory
  layer and SQLite binding
- dep-key and runtime-root persistence are typed farther out than before, but
  the current tree still uses the shared interned-byte substrate between
  typed dep/runtime-root recording and final persistence
- publication sealing now uses a sealed `PublishedStorePathString` boundary:
  callers consume preserve/detach/plain results through accessors, but do not
  synthesize those variants directly
- `EvalEnvironment` is the strongest effect boundary, but `EvalState` still
  retains bypass-capable helpers and cache/path-construction surfaces
  - libflake phase wrappers exist, and the live split is now explicit:
    phase-1 local Git/Mercurial and registry-resolved local inputs must carry a
    rooted `SourcePath` through fetch resolution, not just a bare
    `SourceAccessor`; `logicalRoot` is the live source root for relative-path
    resolution and lockfile location, `carrierRoot` bounds relative escapes and
    backs `sourceInfo.outPath`, and `evaluationRoot` is the phase-2 import root
    used to evaluate `flake.nix`; the broad `/flake.nix` collapse is fixed, but
    the remaining frontier is the graph-edge, lockfile/root-path, and
    carrier-root/evaluation-root normalization pass
- the current authoritative build frontier after the latest structural pass is
  behavioral rather than compile-time: `nix build -L -j0 .` is compile-clean
  through `nix-expr-tests` and package builds; all 15 eval-trace functional
  tests pass (OK: 222 total functional, Fail: 0 as of 2026-04-14);
  tests 2, 3, 7 in `eval-trace-graph.sh` deliberately omit warm-verify
  assertions due to `FileBytes` accessor-type instability on `path:`
  relative inputs (GitSourceAccessor vs FSSourceAccessor); test 8 omits
  warm-verify due to runtime-root persistence timing; these are coverage
  gaps, not regressions; unit tests: 1757 passed, 9 `GTEST_SKIP()`,
  3 `DISABLED_`, all with documented reasons

Validation targets:

1. No new raw effectful evaluator or shared flake-loading calls outside `EvalEnvironment`.
2. No separate side channel for runtime-root publication.
3. No parallel in-process protocol shell retained for compatibility only.
4. No change to compact `Value` layout.
5. Value-kind behavior is measurably reduced in `eval.cc` and `primops.cc`.
6. Dependency recording for environment/filesystem/store/fetch effects is driven
   by `EvalEnvironment` return types, not by caller discipline.
   - this is end-state acceptance, not current-tree status
   - the current implementation routes most bound recording through explicit
     `TraceAccess` derived from the supplied `BoundEffectScope`, but some
     helper paths still admit ambient fallback wiring and remain cleanup
     targets
7. `EvalEnvironmentAuthority` and `EvalTraceSessionAuthority` replace ambient
   constructor/session authority capture.
8. `ObserveOnlyTag`, `DetachedEffectScope`, and `BoundEffectScope` have
   non-overlapping documented legality rules.
   - explicit `TraceAccess` is the intended no-session recording mode; any
     remaining ambient fallback wiring is transitional and should disappear
9. Shared flake mount/reopen flows no longer call `mountInput` directly.
10. `EvalTraceSessionAuthority` carries the exact live session-config fields,
    not vague stable-identity placeholders.
11. The final `EvalEnvironment` implementation does not retain a persistent
    general-purpose `EvalState &`.
12. `EvalEnvironment` is not owned by `EvalState`; separate environment facade
    objects may be constructed from explicit authority without reintroducing an
    `EvalState` subsystem.
13. Effect-adjacent caches such as lookup-path resolution, source-to-store
    mapping, import-resolution caching, and trace-session reuse no longer live
    as separate ambient members directly on `EvalState`.
   - this is end-state acceptance, not a claim that the current tree already
     satisfies it
   - lookup-path resolution, source-to-store, import-resolution, file-trace,
     and fetcher-input caching are still part of the remaining `EvalState`
     boundary/caches cleanup
   - the final long-term ownership of that shared state relative to
     `EvalState` remains a separate cleanup/architecture decision
   - runtime-root/session publication state should stay out of `EvalState`
   - `EvalEnvironment` is the strongest effect boundary in the current tree,
     but not yet the exclusive one: `EvalState` still retains bypass-capable
     helpers and root/store path construction surfaces
   - libflake now carries nominal wrappers for original/resolved/eval-locked/
     persisted refs and for logical-root/carrier-root paths; the remaining
     frontier is graph-edge and lockfile/root-path normalization, and any raw
     string or `SourcePath` erasure there means the phase typing is still too
     weak
14. Flake and file-eval session-config construction no longer happens as
    open-coded field assembly at their call sites.
15. Flake and file-eval session-open package assembly no longer happens as an
    open-coded join step at their call sites.
16. Detached shared-caller mutation cannot mutate trace-session semantic state
    except through the explicit promotion paths.
17. File-eval session reuse-key construction exposes typed uncacheable reasons.
   - reversible dep keys must stay on typed encode/decode helpers with
     canonical framing and persist as opaque bytes, not ad hoc delimiter
     strings
   - the remaining work is to ensure the recording and storage APIs themselves
     preserve those typed dep keys through the final serialization boundary
   - runtime-root publication state must likewise stay typed to the SQLite
     binding boundary
   - flake phase roles still need their own nominal wrappers if we want the
     same discipline in libflake rather than only at serialization boundaries
18. Detached graph completion and bound runtime-fetch completion are separate
    APIs with non-overlapping semantic authority.
19. Post-mount runtime-fetch side effects are owned by explicit environment
    completion flows rather than manually sequenced at call sites.
20. Root loading enters through an explicit capability holding an owned loader
    object, not a raw callback or forgeable proof-like token.
21. Bound runtime-fetch completion returns a replacement session capability
    when it may mutate session-visible state.
22. Flake graph authority assembly no longer happens as an open-coded
    transformation from resolved-graph nodes into registry/mount/accessor
    bindings at the call site.
23. File-eval reuse-key lookup-path identity is built from a canonical reduced
    snapshot, not from ad hoc per-call-site conversion.
24. Detached graph mounts and detached standalone mounts use different carrier
    types and cannot flow into each other’s completion paths accidentally.
25. Session open consumes assembled flake/file-eval packages by value, including
    move-only authority and any session reuse key.
26. Policy-digest construction consumes an explicit captured policy snapshot and
    does not read `NIX_PATH` ambiently during session-config building.
27. Fetch identity observation does not return mounted/store-path state.
28. Lookup-path observations carry enough resolution/access-control detail to
    drive dependency recording from the returned observation.
29. Detached shared flake/reopen flows have a first-class environment operation
    for mounting an already-known store path with its accessor.
30. Detached Git-identity observation is legal for file-eval pre-session work.
31. Session-open snapshot capture is a single environment-owned operation that
    captures policy and lookup-path inputs together.
32. Assembled session-open products and trace-session authority are minted by
    builders/assemblers rather than caller-written aggregate literals.
33. Fetch identity observation uses detached/bound effect modes, not
    `ObserveOnlyTag`.
34. Common trace-session authority inputs and flake-graph authority inputs are
    builder-minted rather than caller-assembled aggregate literals.
35. `resolveFetchIdentity(...)` may consult shared input-cache/fetch identity
    state but must not perform materialization, mounting, or store-path
    publication.
36. `EvalPolicySnapshot` is environment-minted rather than a caller-written
    aggregate.
37. Flake graph extraction outside `libexpr` is limited to one shallow
    flake-local adapter that converts resolved-graph nodes into
    builder-minted `FlakeGraphAuthorityNodeSpec` inputs; the semantic
    derivation into registry/mount/accessor authority remains centralized in
    the session-open assembler.
38. Any concrete `resolveFetchIdentity(...)` implementation is rejected if it
    calls `fetchToStore`, mutates `storeFS`, or publishes allowlist/store-path
    side effects.
39. Flake callers do not hand-assemble session-open authority directly from
    `LockedFlake`; a single flake-local adapter owns that extraction.
40. `resolveFetchIdentity(...)` returns public resolution data separately from
    the environment-minted `ResolvedFetchIdentity` materialization token, and
    `materializeFetch(...)` consumes that token rather than a second raw-input
    materialization request type.
41. Flake-specific builder/assembler entry points are adapter-only rather than
    remaining general public construction hooks.
42. `openTraceCache(...)` no longer directly builds `mountToInput`,
    `inputAccessors`, or `registryEntries`; that normalization lives behind the
    flake-local adapter.
43. No raw-input `FetchMaterializationRequest`-style API remains in the public
    environment surface.
44. Libflake stored phase roles are nominally separated:
    `originalRef`, `resolvedRef`, `lockedRef`, `lockFileRef`, `logicalRoot`,
    `carrierRoot`, and `evaluationRoot` no longer share the same underlying `FlakeRef` /
    `SourcePath` representation in public flake state.

## 11. Major Risks

1. `EvalEnvironment` becomes a god object.
   - Mitigation: single public owner, small internal facets.

2. Collapsing async/protocol layers drops useful scheduling behavior.
   - Mitigation: keep async only where work is truly async, such as fetch or
     download, not around in-process verification.

3. Value wrappers become another helper sprawl layer.
   - Mitigation: organize by value family, not by operation.

4. Centralizing effects forces hard decisions on dependency granularity.
   - This is expected and desirable, but it will surface unresolved edge cases.

## 12. Immediate Next Steps

1. Draft the concrete `EvalEnvironment` header:
   - public methods
   - capability types
   - observation return types
2. Identify the first call sites to move:
   - `getEnv`
   - `mountInput`
   - `readFile`
   - `readDir`
   - `pathExists`
   - runtime-root publication
3. Make `EvalEnvironmentAuthority` / `EvalTraceSessionAuthority` concrete enough
   to remove ambient setup dependencies.
4. Decide whether `TraceBackend` survives as a tiny seam or disappears.
5. Draft the first `Value` view types and move one value-family operation out of
   `EvalState` / `primops` as the pattern.

## 13–29. Adversarial Review History (resolved)

Sections 13 through 29 document the iterative adversarial review of the API
draft. Each section identified weaknesses, proposed corrections, and recorded
design decisions. All issues raised in these sections have been addressed in
the current implementation — they are retained as design history showing why
the current shape exists, not as open work items.

The current adversarial status is in §31 (2026-04-13 review) and §33
(2026-04-14 boundary hardening). For open work items, see §31.11.

---

## 13. Adversarial Review Of The API Draft

This section records the main adversarial objections to the API draft and the
current answer.

### 13.1 Risk: `EvalEnvironment` becomes a giant bag of unrelated helpers

Problem:

- if the public API just mirrors every current `EvalState` helper, complexity is
  merely relocated

Answer:

- keep one public owner
- keep narrow typed request/observation objects
- segment implementation internally by facet
- refuse to move pure value logic into the environment

### 13.2 Risk: effect execution and dep recording are fused too tightly

Problem:

- verifier/recovery code also performs observations
- if every environment call implicitly records, the boundary becomes wrong for
  non-recording consumers

Answer:

- environment methods return typed observations first
- recording is conditional on active bound recording state
- verification uses the same observation surface without evaluator-side replay

### 13.3 Risk: path coercion is accidentally swallowed into the effect layer

Problem:

- current APIs like `coerceToPath` mix value logic and downstream effectful use
- if those are moved wholesale, `EvalEnvironment` becomes half evaluator and
  half environment

Answer:

- split pure interpretation from effect execution
- `EvalState` / value-view layer produces `PathRequest`
- `EvalEnvironment` consumes `PathRequest`

### 13.4 Risk: `mountInput` is still too coarse

Problem:

- current `fetchTree` semantics include:
  - fetch/input-cache lookup
  - mount
  - provenance publication
  - runtime-root persistence in locked cases
- one giant `mountInput(...)` API would preserve that ambiguity

Answer:

- keep `FetchedInput`, detached/bound mounted-input carriers, and runtime-root
  publication as separate
  typed phases
- do not equate fetch, mount, and publish

### 13.5 Risk: global hidden session state survives under a new name

Problem:

- if `EvalEnvironment` owns one mutable implicit session and all methods depend
  on thread-local state, correctness drift remains possible

Answer:

- bound-session capabilities must be explicit at mutation points
- runtime-root publication and session replacement must consume and return typed
  session capabilities
- avoid recreating `TraceRuntime` side channels under a different owner

### 13.6 Risk: size reduction is overstated if the async/protocol stack stays

Problem:

- the branch’s size increase is not just effect drift
- a large part is wrapper/protocol machinery

Answer:

- the rewrite only pays off if the in-process service/protocol shell is deleted
  or collapsed
- `EvalEnvironment` alone is not enough

### 13.7 Risk: typed observations could become too expensive for hot paths

Problem:

- wrapping every file/stat/dir result in rich objects may add churn on hot paths

Answer:

- observation types should be small moveable aggregates
- they should own only what current callers already materialize
- no virtual hierarchy is needed
- hot-path fields should stay by-value or by-small-aggregate, not heap-wrapped

### 13.8 Risk: the API still does not cover current edge semantics

Current edge semantics that the implementation draft must preserve:

- `pathExists` must distinguish missing from wrong-type-for-trailing-slash
- `readFile` must propagate string-context/store-reference information
- locked and unlocked `fetchTree` must remain semantically distinct
- runtime-root publication must stay session-bound
- allowlist/closure authorization remains policy, not semantic data observation

These should be treated as acceptance checks for the real header draft.

## 14. Second Adversarial Pass: Header Skeleton

After drafting [eval-environment.hh](/home/connorbaker/nix/src/libexpr/include/nix/expr/eval-environment.hh),
the main adversarial findings were:

### 14.1 The boundary still starts too late

Current draft problem:

- `PathRequest` assumes the caller already completed path realization and
  provenance threading
- current implementation does critical semantic work during
  `realisePath()` / path coercion / provenance capture

Consequence:

- if left unchanged, the most important path-sensitive semantics remain outside
  `EvalEnvironment`

Required correction:

- split the interface into:
  - a value-facing request builder layer
  - the actual environment execution layer
- or pull the realisation boundary itself into the environment contract

Status:

- addressed in the revised header skeleton by adding an explicit
  `EvalRequestBuilder`

### 14.2 `ObserveOnlyTag` is too weak for fetch/materialize phases

Current draft problem:

- `fetchInput()` and `mountInput()` were given both observe-only and
  session-bound variants
- current fetch semantics distinguish read-only cached resolution from
  cache-populating / store-writing behavior

Consequence:

- a simple mode tag is not a strong enough phase boundary for fetch

Required correction:

- replace mode-tag overloading here with explicit fetch-phase types or
  distinct APIs for:
  - read-only cached lookup
  - materializing fetch
  - mount/publication

Status:

- addressed in the revised header skeleton by splitting:
  - `FetchIdentityRequest`
  - environment-minted `ResolvedFetchIdentity`
  - `FetchedInput`
  - detached/bound mounted-input carriers

### 14.3 `EvalSessionSpec` is in the wrong abstraction layer

Current draft problem:

- it exposes trace-store/session-identity mechanics too directly
- but still does not fully model current session-open needs

Consequence:

- it leaks internals without being sufficient as a true session-open contract

Required correction:

- replace it with a higher-level evaluator/session-open spec
- keep semantic session-key derivation internal to the environment/session layer

Status:

- partially addressed in the revised header skeleton by replacing the low-level
  key-shaped spec with `EvalSessionOpenRequest`

### 14.4 `FetchedInput` is not a safe carrier yet

Current draft problem:

- it used raw `fetchers::Input *` pointers
- current fetch/cache flows return owned values whose lifetime must survive
  later phases

Consequence:

- the carrier is not robust even as a draft

Required correction:

- make `FetchedInput` an owning carrier
- include the fields needed for the later mount/publication/runtime-root phases

Status:

- addressed in the revised header skeleton by making `FetchedInput` carry owned
  shared input objects rather than raw pointers

### 14.5 Runtime-root handling is still too narrow

Current draft problem:

- the skeleton only models validation + publish
- current locked/unlocked `fetchTree` semantics also involve:
  - mount-point registration
  - runtime store-path availability deps
  - unlocked `RuntimeFetchIdentity`

Consequence:

- parts of current `fetchTree` semantics would still leak outside the boundary

Required correction:

- expand the fetch/runtime-root contract so both locked and unlocked flows are
  first-class and explicit

Status:

- partially addressed in the revised header skeleton; still requires a fuller
  contract for mount-point registration and runtime store-path availability
  observation

### 14.6 The single boundary is still incomplete for verifier effects

Current draft problem:

- the skeleton does not yet cover several current effectful verification paths:
  - Git identity
  - derived store-path resolution
  - structured projections
  - session-system observations

Consequence:

- `dep-resolution-service` would otherwise remain a second effect boundary

Required correction:

- either:
  - explicitly include verifier-side observation APIs in the environment, or
  - define a smaller internal observation substrate beneath both evaluator and
    verifier code

Status:

- partially addressed in the revised header skeleton by adding:
  - `observeGitIdentity`
  - `observeDerivedStorePath`
  - `observeSessionSystem`

## 15. Third Adversarial Pass: Revised Skeleton

After adding `EvalRequestBuilder` and the fetch-phase split, the next main
findings were:

### 15.1 Request types are still forgeable

Current problem:

- request types remain public aggregates
- callers could bypass the request-builder and hand-construct fake requests

Consequence:

- the new boundary is still convention-backed rather than structurally enforced

Required correction:

- make request construction builder-owned or capability-gated
- do not leave `CoercedPathRequest`, `ReadFileRequest`, `Fetch*Request`, and
  similar types freely forgeable

### 15.2 Lookup-path and URI policy are still outside the model

Current problem:

- the revised API still does not model:
  - `findFile`
  - `resolveLookupPathPath`
  - URI policy checking
  - lookup-path driven fetch/resolve semantics

Consequence:

- a major evaluator effect path would still remain outside `EvalEnvironment`

Required correction:

- add explicit lookup-path and URI-resolution requests/observations
- or consciously define a subordinate environment facet that owns them

### 15.3 Generic string-context realization — resolved

`EvalEnvironment::realiseContext()` now handles context realization through
the environment boundary. `EvalEnvironment::realiseString` was removed as
dead code — `EvalState::realiseString` composes `realiseContext` +
`rewriteStrings` directly.

### 15.4 Fetch semantics are improved but still incomplete

Current problem:

- the split between fetch identity and materialization is better
- but the draft still omits some state needed by current fetch flows and does
  not yet give first-class shape to unlocked materialized fetch recording

Required correction:

- make the fetch carrier types complete enough for:
  - locked fetch runtime-root handling
  - unlocked `RuntimeFetchIdentity`
  - later provenance/mount publication

### 15.5 Store authorization remains too collapsed

Current problem:

- `authorizeStorePath()` still compresses several materially different current
  operations into one underspecified shape

Required correction:

- split policy/allowlist/store-path publication operations into narrower typed
  requests or observations

### 15.6 Session-open inputs still risk hidden ambient state

Current problem:

- `EvalSessionOpenRequest` no longer leaks low-level key fields directly
- but it still does not make all non-ambient session-open inputs explicit

Required correction:

- define the real session-open authority inputs explicitly enough that the
  environment does not silently depend on hidden evaluator/global state

## 16. Fourth Adversarial Pass: Revised Request Boundary

After making request construction builder-owned, adding explicit lookup-path and
string-context realization requests, splitting store authorization, and adding
`StorePathPublishRequest`, the remaining material findings are:

### 16.1 Session/environment authority is still under-specified

Current problem:

- the revised request layer now covers the major evaluator effect families
- but `EvalEnvironment(EvalState &, Store &)` plus `EvalSessionOpenRequest` still
  leaves major authority inputs implicit
- current live code still depends on additional construction/session state such
  as:
  - trace root-loader wiring
  - semantic registry / locked mount state
  - input accessor maps
  - build-store and fetch/input-cache ownership
  - settings that materially change effect semantics, such as `readOnlyMode`
    and import-from-derivation posture

Consequence:

- the draft has a much better call boundary, but not yet a fully explicit
  environment/session authority boundary

Required correction:

- either:
  - add an explicit environment-construction authority object, or
  - make the session/environment ownership split explicit enough that these
    inputs are not just ambient `EvalState`/`TraceRuntime` capture

### 16.2 Non-recording and session-bound effect entry points still need a
clearer contract

Current problem:

- lookup-path resolution and generic string realization are no longer mislabeled
  as observe-only
- but the draft still has both unbound and session-bound variants for some
  effectful operations without yet spelling out when each is legal

Consequence:

- the API is structurally better than before, but callers could still pick the
  wrong variant unless the rewrite defines:
  - which operations are evaluator-only and may run without a bound trace
    session
  - which operations must be session-bound whenever eval-trace is active

Required correction:

- make the allowed call modes explicit in the implementation bundle plan and
  corresponding validation targets

### 16.3 Fetch/mount caller scope still needs one explicit decision

Current problem:

- the fetch split is now materially better:
  - fetch identity
  - materialization
  - mount
  - runtime-root publication
- but the draft still needs one architectural choice:
  - is `EvalEnvironment` only the boundary for `EvalState` and primops, or
  - does it also become the shared boundary for existing non-evaluator callers
    of `mountInput`

Consequence:

- without that choice, the `BoundEffectScope` requirement on
  `materializeFetch` / `mountFetchedInput` remains slightly ambiguous

Required correction:

- choose one of:
  - evaluator-only boundary, leaving non-evaluator fetch/mount call sites to a
    separate owner
  - shared boundary with an additional non-session materialization path

### 16.4 `builtins.storePath` is now modeled, but the implementation bundle
must prove it replaces the old helper completely

Current improvement:

- `StorePathPublishRequest` and `publishStorePath(...)` now cover the missing
  pre-`StorePath` half of `builtins.storePath`
- the observation now carries rendered path plus string context

Remaining requirement:

- the actual rewrite must prove that call sites stop open-coding:
  - path coercion
  - symlink canonicalization
  - store membership checks
  - `ensurePath`
  - string-context construction

Validation addition:

- bundle completion should explicitly fail while `primops.cc` or `eval.cc` still
  preserves an end-to-end `builtins.storePath` flow outside `EvalEnvironment`

## 17. Fifth Adversarial Pass: Shared Boundary And Explicit Authority

After broadening the draft to a shared `libexpr` boundary, adding
`EvalEnvironmentAuthority`, `EvalTraceSessionAuthority`, and
`DetachedEffectScope`, the remaining material findings are:

### 17.1 Trace-session authority is still only partially explicit

Current problem:

- `EvalTraceSessionAuthority` now makes the major hidden categories visible:
  - root loader
  - input-accessor bindings
  - mounted-input bindings
  - initial semantic registry
  - stable identity hints
- but it still stops short of the exact concrete session-policy/config inputs
  that the live code uses to distinguish sessions

Consequence:

- the draft is much better than before, but session open still risks a final
  round of ambient capture unless the exact session-policy fields land in this
  authority object

Required correction:

- derive and add the exact remaining session-policy/config fields from the live
  `TraceSession` / `SessionConfig` construction path rather than leaving only
  identity hints

### 17.2 Environment authority is explicit, but constructor ownership is still
split between `EvalState` and `EvalEnvironmentAuthority`

Current problem:

- the constructor now takes an explicit authority object
- but it still also takes `EvalState &`
- that means the final rewrite still needs a precise rule for what remains legal
  to depend on through `EvalState`:
  - allocator / symbols / positions
  - effect-external helper state
  - trace/publication hooks that have not yet moved

Consequence:

- without that rule, ambient behavior can still creep back through the retained
  `EvalState` reference even though the main stores/accessors/settings are now
  explicit

Required correction:

- document and enforce the allowed residual `EvalState` dependency set for the
  environment implementation bundle

### 17.3 Shared caller integration is now chosen, but the ownership path still
needs one concrete implementation rule

Current improvement:

- the plan now chooses the shared-boundary direction:
  non-evaluator callers like flake graph resolution should also use
  `EvalEnvironment`
- `DetachedEffectScope` is the type-level hook for that choice

Remaining requirement:

- the implementation plan still needs to say whether:
  - `EvalState` owns one long-lived `EvalEnvironment` instance reused by
    `libflake`, or
  - shared callers construct short-lived environment views/scopes from a common
    authority object

Why it matters:

- lifetime/ownership determines whether detached scopes can safely interact with
  the same mount/access-control/session-adjacent state as evaluator code

### 17.4 `builtins.findFile` semantics are now representable, but this must be
validated explicitly

Current improvement:

- `LookupPathRequest` now supports explicit search-path entries with:
  - prefix
  - raw value
  - per-entry string context
  - per-entry origin

Remaining requirement:

- bundle validation should explicitly fail while `builtins.findFile` still
  performs per-entry realization or origin threading outside the request builder

## 18. Sixth Adversarial Pass: Current Shared-Boundary Draft

After adding explicit authority objects, `DetachedEffectScope`, and per-entry
lookup-path context, the remaining material findings are:

### 18.1 `EvalTraceSessionAuthority` still needs the exact `SessionConfig`
fields, not just identity hints

Current problem:

- the draft now exposes the right categories of hidden session-open inputs
- but the live cache/session machinery still keys behavior off a concrete
  `SessionConfig` shape containing:
  - `policyDigest`
  - `graphDigest`
  - `sourceIdentity`
  - `externalRoots`
  - `stableRecoveryKey`

Consequence:

- if the final authority object stops at `stableSessionIdentity` /
  `stableRecoveryIdentity` hints, the last part of session semantics still
  remains implicit

Required correction:

- replace the identity-hint placeholders with the exact remaining
  session-config constituents derived from the current
  `SessionConfig`/`computePolicyDigest`/flake-session construction code

### 18.2 The constructor boundary is better, but `EvalState &` still needs a
strict residual-usage rule

Current problem:

- `EvalEnvironmentAuthority` now captures the major stores/accessors/settings
- but the environment still takes `EvalState &`
- current `EvalState` still owns trace-runtime side channels and helper entry
  points such as:
  - trace-session lookup/remember
  - runtime-root recording
  - registry mount publication
  - some provenance/value-publication helpers

Consequence:

- unless the rewrite names exactly what remains legal to use through
  `EvalState`, ambient behavior can still leak back in through that retained
  reference

Required correction:

- add an explicit residual-usage allowlist for `EvalState` in the
  implementation bundles and fail the rewrite if environment code keeps reaching
  through to old side-channel helpers

### 18.3 Shared flake integration is now chosen, but lifetime/ownership still
needs one concrete answer

Current problem:

- the draft now clearly chooses the shared-boundary direction for flake mount
  and reopen flows
- but it still does not say whether:
  - one `EvalEnvironment` instance is owned and reused through `EvalState`, or
  - shared callers construct short-lived environment views from the same
    authority object

Why it matters:

- this determines how detached scopes share mount/access-control/session-adjacent
  state with evaluator code, and whether duplicate caches/locks can appear

Required correction:

- make the ownership/lifetime rule explicit before implementation starts

## 19. Seventh Adversarial Pass: Exact Session Config And Long-Lived Ownership

After replacing the session-identity placeholders with the exact current
`SessionConfig` constituents, removing the persistent `EvalState &` from the
environment draft, and choosing one long-lived environment per `EvalState`, the
remaining material findings are:

### 19.1 The exact session-config fields are now present, but the call-site
builders still need to stop open-coding flake vs file-eval policy construction

Current improvement:

- the authority shape now names the exact distinguishing fields:
  - `policyDigest`
  - `graphDigest`
  - `sourceIdentity`
  - `externalRoots`
  - `stableRecoveryKey`

Remaining risk:

- the draft still leaves the construction policy at the call sites
- current flake and file-eval paths intentionally diverge on:
  - `graphDigest`
  - `externalRoots`
  - `stableRecoveryKey`

Required follow-up:

- add explicit constructor helpers or builders for:
  - flake session config
  - file-eval session config
- otherwise the call sites can still drift even though the environment
  authority now carries the right fields

### 19.2 One long-lived environment per `EvalState` is the right ownership
shape, and the effect-adjacent caches should move with it

Current improvement:

- the design no longer permits short-lived ad hoc environment instances for
  flake/shared callers

Decision:

- move the effect-adjacent caches and helper ownership into
  `EvalEnvironment`

Why:

- they correspond to effectful resolution, publication, and session reuse
- leaving them on `EvalState` would undercut the single-owner rule

Concrete candidates from the current tree:

- `lookupPathResolved`
- `srcToStore`
- `importResolutionCache`
- trace-session reuse cache currently hidden behind `TraceRuntime`

### 19.3 The zero-reach-through rule is better, but verifier-side and shared
caller entry points still need one explicit enforcement check

Current improvement:

- the environment no longer keeps a general-purpose `EvalState &`

Remaining risk:

- some shared callers or verifier helpers may still be tempted to bypass the
  request builder and call raw helper code directly because they already have
  values, paths, or inputs in hand

Required follow-up:

- bundle validation should fail while:
  - `builtins.findFile` still threads per-entry origin/context outside the
    request builder
  - shared flake mount/reopen flows still call old helpers directly
  - verifier effect observations still bypass the environment surface

The same boundary discipline should apply to evaluator re-entry via root
loading.

Rule:

- session-opening code must not pass raw capturing lambdas directly once the
  root-loader capability exists
- the root loader must be minted as an explicit capability with documented
  lifetime/thread rules

### 19.4 Registry-seed ownership needed one final correction

Current problem:

- the earlier shared-boundary draft carried the semantic-registry seed as an
  alias-shaped shared pointer

Why that was wrong:

- the live `TraceSession` path takes an owned `SemanticRegistry` value and then
  mutates the session-local copy by adding mount points and verified runtime
  roots

Decision:

- carry `registrySeed` as an owned value in `EvalTraceSessionAuthority`

Remaining requirement:

- the implementation should keep the ownership model obvious and avoid
  reintroducing shared mutable registry state through helper indirection

## 20. Eighth Adversarial Pass: Current Post-Cleanup Draft

After replacing the session-identity placeholders with the exact current
session-config fields, removing the persistent `EvalState &`, moving the
effect-adjacent caches under the long-lived environment owner, and clarifying
`NIX_PATH` as lookup-path snapshot rather than session identity, the remaining
material findings are:

### 20.1 Session-config construction can still drift unless it gets its own
typed builders

Current improvement:

- the authority object now carries the exact current distinguishing fields

Remaining risk:

- the live flake and file-eval call sites still intentionally construct those
  fields differently:
  - flake path in [flake.cc](/home/connorbaker/nix/src/libflake/flake.cc)
  - file-eval path in
    [installable-attr-path.cc](/home/connorbaker/nix/src/libcmd/installable-attr-path.cc)
- if those call sites keep open-coding the construction logic, the environment
  boundary will still inherit drift at the input-building layer

Required follow-up:

- add explicit helpers/builders for:
  - flake session config
  - file-eval session config

### 20.2 The single-owner rule is stronger now, but the split between
environment-owned effect caches and evaluator/tracing state still needs one
explicit cleanup pass

Current improvement:

- effect-adjacent caches like lookup-path resolution, source-to-store caching,
  import-resolution caching, and trace-session reuse are now assigned to the
  long-lived environment owner

Remaining risk:

- `TraceRuntime` and `EvalState` still currently host other state that is not
  purely effect-external, including runtime-root side channels and trace-session
  helper entry points
- unless bundle planning keeps pushing those out, the implementation can still
  recreate a second partial owner beside `EvalEnvironment`

Required follow-up:

- explicitly delete or relocate:
  - `EvalState::lookupTraceSession`
  - `EvalState::rememberTraceSession`
  - `EvalState::recordRuntimeRoot`
  - `EvalState::addRegistryMountPoint`
  - the corresponding `TraceRuntime` side channels

### 20.3 Detached shared-caller mutation still needs one explicit semantic
boundary rule

Current improvement:

- `DetachedEffectScope` now clearly owns shared libexpr mutation outside a bound
  recording session

Remaining risk:

- detached fetch/materialize/mount flows must be allowed to mutate store and
  access-control state, but they must not silently become a second way to
  mutate bound trace-session semantic state
- this matters directly for the current phase-1 flake graph flows that mount
  inputs before ordinary bound evaluation

Required follow-up:

- specify and enforce that detached mutation may update shared environment
  mount/access-control state, but trace-session registry/runtime-root mutation
  remains a separate explicit promotion path

## 21. Ninth Adversarial Pass: Typed Builders And Promotion Rule

After adding typed flake/file-eval session-config builders, a concrete
side-channel deletion/relocation rule, and an explicit detached mutation
promotion rule, the remaining material findings are:

### 21.1 Session-config builders need one landing/ownership choice

Current improvement:

- the design now says flake and file-eval session-config construction must use
  typed builders rather than open-coded field assembly

Remaining risk:

- the draft still needs to decide where those builders live:
  - beside `EvalEnvironment`
  - in a smaller shared session-config helper
  - or in the request-builder layer

Current best fit:

- keep them adjacent to the environment authority layer, not in the
  value/request-builder layer

Why:

- they depend on effect-policy inputs (`EvalSettings`) and shared session
  semantics, not on `Value` interpretation

### 21.2 The side-channel deletion rule is concrete now, but replay/value
identity state still needs one explicit non-goal boundary

Current improvement:

- the plan now names the exact session/runtime helpers and fields that must be
  deleted or relocated out of `EvalState` / `TraceRuntime`

Remaining risk:

- not all `TraceRuntime` state is session-side-channel state
- replay/value-identity machinery should not be accidentally swept into the
  environment just because it lives nearby today

Required follow-up:

- keep the deletion target specific to:
  - session reuse
  - runtime-root publication
  - registry/mount side channels
- leave replay/value-identity state to a separate cleanup bundle unless it
  truly becomes environment-owned for an independent reason

### 21.3 Detached promotion is explicit now, but `fetchTree` locked/unlocked
flows still need one implementation mapping

Current improvement:

- the draft now clearly says detached mutation may update shared environment
  state but not bound session semantic state
- it also names the only promotion paths

Remaining risk:

- the live `fetchTree` paths still differ semantically:
  - locked materialized inputs may later contribute runtime-root publication
  - unlocked flows record `RuntimeFetchIdentity`
- the implementation plan still needs one explicit mapping from those live
  cases onto:
  - detached mutation only
  - immediate bound-session mutation
  - detached then explicit promotion

## 22. Tenth Adversarial Pass: Current Builder And Promotion Draft

After adding typed session-config builders, the side-channel deletion map, and
the detached promotion rule, the remaining material findings are:

### 22.1 Session reuse-key derivation is now first-class, but still needs one
final placement decision

Current improvement:

- flake and file-eval session-reuse-key construction is now required to go
  through typed builders

Remaining risk:

- the draft still needs one explicit placement rule for the reuse-key builders:
  - next to the session-config builders
  - inside the environment
  - or in a separate session-authority helper

Current best fit:

- keep them adjacent to the session-config builders / authority layer

Why:

- they depend on session identity semantics and effective lookup-path snapshot
  capture, not on value interpretation or low-level store execution

### 22.1.1 File-eval uncacheable results are now explicit

Current improvement:

- file-eval reuse-key construction is now intended to return a typed result with
  explicit uncacheable reasons

Remaining risk:

- the concrete reason set must stay aligned with the live code’s ordinary
  control flow, especially:
  - stdin input
  - missing source identity
  - missing/uncacheable auto-args identity

Current best fit:

- keep the reason set narrow and live-code-driven rather than speculative

### 22.2 `NIX_PATH` snapshot consistency is now much tighter, but the draft
still needs one capture rule

Current improvement:

- the draft no longer exposes raw `nixPath` on `EvalSessionOpenRequest`
- it now requires session-open lookup-path state to come from an effective
  snapshot builder tied to the same impure snapshot that fed `policyDigest`

Remaining risk:

- the live `computePolicyDigest` path includes the effective impure-mode
  `NIX_PATH` / `settings.nixPath`
- the implementation still needs to choose where that effective lookup-path
  snapshot is captured:
  - by an `EvalSessionOpenRequestBuilder`
  - or by `EvalEnvironment` itself at session-open time

Current best fit:

- use `EvalSessionOpenRequestBuilder`, not caller-assembled session-open input

### 22.3 The locked/unlocked `fetchTree` mapping is now explicit, but one
remaining integration seam still needs attention

Current improvement:

- the draft now explicitly maps:
  - detached flake/shared loading flows
  - bound locked `fetchTree`
  - bound unlocked `fetchTree`

Remaining risk:

- runtime-root-related side effects are now split between:
  - `mountInput`
  - `TraceSession::registerRuntimeRootMount` (the session-scoped
    replacement for the former `addRegistryMountPoint`)
  - `TraceBackend::recordRuntimeRoot` (session-scoped)
  - `recordRuntimeStoreDep`
- the implementation must ensure those become one coherent environment-owned
  completion flow (e.g., a single `publishRuntimeRoot` returning a replacement
  `EvalSessionHandle`) rather than merely being renamed into several new
  helpers

### 22.4 Root-loader capability is now explicit, but the environment boundary
still needs one enforcement rule

Current improvement:

- the draft now treats root loading as an explicit capability rather than a raw
  capturing callback

Remaining risk:

- if session-open call sites can still hand arbitrary lambdas directly to the
  implementation, the explicit capability buys little

Current best fit:

- require root loaders to be minted by the evaluator/request-builder side and
  forbid raw callback construction at the environment/session boundary

## 23. Thirteenth Adversarial Pass: Assembler And Completion Split

After tightening the root-loader capability into an owned holder object,
splitting detached graph completion from bound runtime-fetch completion, and
adding higher-level session-open assemblers, the remaining material findings
are:

### 23.1 Flake graph authority assembly is still partly open-coded

Current improvement:

- the draft now has a higher-level session-open assembler for flake and
  file-eval callers

Remaining risk:

- the live flake path in
  [flake.cc](/home/connorbaker/nix/src/libflake/flake.cc)
  still has a correctness-sensitive transformation from resolved-graph data
  into:
  - registry forward entries
  - mount-point bindings
  - input-accessor bindings
- if that transformation remains open-coded outside the assembler layer, the
  biggest flake-specific identity/provenance seam is still exposed

Why this matters:

- the current code relies on node-key-based `DepSource` identity staying
  exactly aligned between forward registry entries and reverse mount-point
  publication
- drifting those two views breaks warm verification even if the rest of the
  session-open package is typed cleanly

### 23.2 File-eval reuse-key lookup-path identity still needs one canonical
builder boundary

Current improvement:

- the draft now distinguishes lookup-path identity from the richer
  provenance/context-bearing entry type

Remaining risk:

- if file-eval callers still assemble `UnrealizedLookupPathIdentity` vectors by
  hand, the draft has only moved the drift surface one step out

Current best fit:

- make the higher-level assembler or a dedicated identity-snapshot builder own
  the conversion from live `LookupPath` to `UnrealizedLookupPathIdentity`

Why:

- current `computeFileEvalIdentity(...)` in
  [installable-attr-path.cc](/home/connorbaker/nix/src/libcmd/installable-attr-path.cc)
  hashes a very specific prefix/path snapshot
- the draft should not leave that snapshot shape open to per-call-site
  reinterpretation

### 23.3 Detached mount flows still need one explicit applicability rule for
`completeGraphFetch(...)`

Current improvement:

- detached and bound mounted inputs are now distinct carriers
- detached graph completion is now a separate API from bound runtime-fetch
  completion

Remaining risk:

- current shared callers in
  [flake.cc](/home/connorbaker/nix/src/libflake/flake.cc)
  use `mountInput(...)` in more than one semantic role:
  - graph-building/promotion
  - one-off detached mount-to-store-path use
- the implementation still needs one rule for when detached mount output must
  flow through `completeGraphFetch(...)` and when a detached mount is complete
  without further promotion

Why:

- otherwise the split exists in the type surface but detached callers can still
  apply it inconsistently

### 23.4 Root-loader ownership is stronger, but lifetime/thread guarantees are
still contract-backed

Current improvement:

- the draft no longer hides root loading in a raw `std::function`
- it now uses an owned holder object minted by the session-authority layer

Remaining risk:

- evaluator-thread affinity and captured-GC-root lifetime are still documented
  semantic requirements, not statically proven resources

Current best fit:

- accept the holder-based draft as a major improvement, but treat these as
  explicit implementation-review invariants when the concrete holder types land

## 24. Fourteenth Adversarial Pass: Post-Fix Residual Risks

After adding flake graph authority requests, canonical lookup-path identity
builders, detached graph-vs-standalone mount splits, and explicit root-loader
thread/lifetime resources, the main remaining risks are:

### 24.1 Flake authority input normalization is narrower, but still not fully
opaque

Current improvement:

- the assembler now owns the node-key-based transformation into registry, mount,
  and accessor bindings

Remaining risk:

- callers still have to populate `FlakeGraphAuthorityNodeSpec` values from the
  live resolved graph
- that remaining step should stay a shallow normalization step, not grow new
  semantics of its own

Why this is acceptable for now:

- the correctness-sensitive derivation from node-key authority into session
  bindings is now centralized
- the remaining caller work is a much smaller and more obvious data-copy seam

### 24.2 Root-loader thread/lifetime typing is stronger, but still not a full
proof

Current improvement:

- the capability now consumes an owned holder object plus explicit
  evaluator-thread and lifetime resources

Remaining risk:

- those resources are still opaque capabilities, not a statically checked proof
  that the holder never escapes or runs on the wrong thread

Why this is acceptable for now:

- the draft no longer hides the dependency at all
- implementation review can now demand that concrete holder types respect the
  explicit token/lease model rather than discovering the requirement late

### 24.3 Detached standalone mounts still need disciplined use-site pruning

Current improvement:

- detached graph mounts and detached standalone mounts are now different types

Remaining risk:

- existing detached callers that only need a mounted store path may continue to
  overuse the graph-specific path unless the rewrite actively prunes them back
  to the standalone carrier

Why this is acceptable for now:

- the misuse is now visible in the type choice, not hidden behind one generic
  mounted-input carrier

## 25. Fifteenth Adversarial Pass: Post-Authority/Input Tightening

After making common/flake authority inputs builder-minted and sharpening
fetch-identity resolution into a non-materializing shared-effect step, the main
remaining risks are:

### 25.1 Flake authority normalization still has one shallow caller seam

Current improvement:

- `CommonTraceSessionAuthorityInputs`
- `FlakeGraphAuthorityNodeSpec`
- `FlakeGraphTraceSessionAuthorityRequest`

are now builder-minted types rather than caller-forgeable aggregate structs.

Remaining risk:

- the live flake caller still has to iterate the resolved graph and feed the
  raw node fields into the authority builder
- that step is still outside `EvalEnvironment` / the assembler because the
  resolved graph lives in flake-specific code

Why this is acceptable for now:

- the semantic derivation from node-key authority into registry/mount/accessor
  bindings remains centralized in the assembler
- the remaining caller seam is now a shallow field-extraction/normalization
  step, not an alternate authority-construction path

### 25.2 Fetch identity is better separated, but the implementation contract
still matters

Current improvement:

- the draft no longer models fetch identity as `ObserveOnlyTag`
- the draft no longer returns `StorePath` from fetch-identity resolution
- `resolveFetchIdentity(...)` is explicitly a detached/bound shared-effect
  operation

Remaining risk:

- the live implementation still begins from `inputCache->getAccessor(...)`
- the concrete implementation must preserve the stated contract:
  - may consult shared fetch/input-cache state
  - must not materialize into the store
  - must not mount `storeFS`
  - must not publish allowlist/store-path side effects

Why this is acceptable for now:

- the contract is now explicit in the API/plan rather than hidden behind an
  “observation” name
- implementation review can now reject any `resolveFetchIdentity(...)`
  implementation that drifts into `fetchToStore` / mount behavior

## 26. Sixteenth Adversarial Pass: Post-Captured-Inputs Iteration

After tightening session-open snapshot capture and non-forgeable session-open
packages, the remaining issues are narrower and mostly about keeping the
implementation honest against the type surface.

### 26.1 Flake graph extraction remains one shallow non-libexpr seam

Current improvement:

- `CapturedSessionOpenInputs`
- `EvalTraceSessionAuthority`
- `AssembledFlakeTraceSessionOpen`

are now non-forgeable and builder/assembler-minted, and flake callers no
longer construct final registry/mount/accessor authority directly.

Remaining risk:

- flake-specific code still has to walk `ResolvedFlakeGraph` and pass the raw
  node fields into the authority builder
- this extraction remains outside `libexpr` because the graph type is
  flake-specific

Decision:

- accept that one shallow caller-local extraction step
- require the semantic mapping from node-key graph identity into
  registry/mount/accessor authority to stay inside the assembler

Why:

- forcing `libexpr` to depend on flake graph types would worsen layering
- the remaining seam is now field extraction, not alternate semantic assembly

### 26.2 Fetch identity is now typed correctly, but implementation discipline is
still mandatory

Current improvement:

- `resolveFetchIdentity(...)` is detached/bound only, not `ObserveOnlyTag`
- the return type no longer contains store-path or mount/accessor state
- the API contract now explicitly forbids materialization and mount side
  effects

Remaining risk:

- the live fetch flow still begins from `inputCache->getAccessor(...)`
- a sloppy implementation could still drift into `fetchToStore` or mount work
  despite the type surface

Decision:

- keep `resolveFetchIdentity(...)` as the shared-effect identity-resolution step
- add explicit validation that any implementation calling `fetchToStore`,
  mutating `storeFS`, or publishing allowlist/store-path effects is invalid

Why:

- the current tree does require some shared-effect interaction for fetch
  identity resolution
- the important design boundary is not “zero side effects”, it is “no
  materialization or publication semantics”

## 27. Seventeenth Adversarial Pass: Adapter + Enforced Fetch-Phase Split

After adding a flake-local session-open adapter and tightening the fetch API so
materialization consumes an environment-minted resolved identity, the remaining
surface is closer to the intended architecture.

### 27.1 Flake graph extraction is now centralized in the right layer

Current improvement:

- `libexpr` continues to own the generic builder/assembler vocabulary
- `libflake` now owns a dedicated adapter from `LockedFlake` /
  `ResolvedFlakeGraph` into those builder/assembler inputs

Decision:

- keep graph extraction in `libflake`
- keep semantic authority derivation in the generic session-open assembler

Why:

- `LockedFlake` and `ResolvedFlakeGraph` are flake-owned types
- centralizing the extraction in one flake-local adapter removes duplicated
  call-site normalization without introducing a backwards dependency from
  `libexpr` into flake graph types

### 27.2 Fetch identity and materialization are now structurally separated

Current improvement:

- `ResolvedFetchIdentity` is environment-minted and non-forgeable
- `materializeFetch(...)` consumes `ResolvedFetchIdentity &&`
- there is no longer a second raw-input materialization request type

Decision:

- keep identity resolution and materialization as two separate public phases
- make materialization impossible to reach without first obtaining the resolved
  identity from the environment

Why:

- the old dual-request shape let callers bypass the intended phase boundary
- consuming the minted identity does not by itself prove the implementation is
  correct, but it does make reviewable misuse much narrower

## 28. Eighteenth Adversarial Pass: Where Semantics Are Still Too Weakly Typed

After tightening the adapter and fetch-phase split, the next remaining problems
are not “missing APIs” so much as “semantics still encoded too weakly.”

### 28.1 Too many distinct semantic domains are still plain strings

Current problem:

- the draft still uses raw `std::string` for semantically distinct values such
  as:
  - flake node keys
  - source identities
  - stable recovery keys
  - locked runtime-root URLs
  - NAR hash SRI strings
  - runtime-root source IDs
  - file-eval expression identities
  - auto-args identities
  - store-dir identity
  - current-system identity

Why this is dangerous:

- eval-trace already has a concrete history of bugs caused by namespace drift
  and structurally identical values from different semantic domains being
  confused
- comments and field names do not prevent callers from passing the right shape
  but the wrong meaning

Required correction:

- convert these domains to nominal tagged types (`Tagged<Tag, T>` or equivalent)
- use named factories where the namespace matters, the same way eval-trace uses
  `DepSource::fromNodeKey(...)` vs `DepSource::fromRuntimeRoot(...)`

### 28.2 Some multi-step flows still need real typestate, not only move-only DTOs

Current problem:

- the fetch flow is now better separated, but the draft still models
  `ResolvedFetchIdentity -> FetchedInput -> mounted input -> completion`
  mostly as move-only data transfer rather than `Linear` typestate
- the same is true for some session-open and completion paths

Why this is dangerous:

- move-only prevents aliasing, but not silent drop of an intermediate state
- eval-trace’s existing `Linear` patterns exist specifically to catch
  “constructed but never consumed” bugs

Required correction:

- use `Linear`-backed typestate for the multi-step flows whose transitions must
  be consumed exactly once
- reserve plain move-only DTOs for inert data, not lifecycle-critical states

### 28.3 Some result shapes are still one struct plus optionals instead of a sum

Current problem:

- `LookupPathObservation` still combines several distinct resolution outcomes
  using one struct plus `resolutionKind` and many optional fields

`RuntimeFetchCompletion` has been resolved: replaced by
`PublishedRuntimeFetch<RuntimeFetchLockMode>` with `ConditionalBase` for
locked/unlocked fields (see §32).

Remaining for `LookupPathObservation`:

- prefer `std::variant` or distinct carrier types for materially different
  result shapes
- keep enums only when the data shape is uniform across variants

### 28.4 Publication/provenance semantics are not yet captured strongly enough

Current problem:

- the draft still treats some string/path publication behavior as ordinary
  request/observation fields rather than the intended typed publication
  boundary
- eval-trace’s live design already distinguishes:
  - preserve provenance
  - detach provenance
  - plain synthesized output

Why this is dangerous:

- this subsystem already has a known bug class where callers do the coercion
  and then locally decide whether provenance should survive
- a loose “mode enum + optional provenance” shape recreates that bug surface

Required correction:

- make publication/coercion semantics a sealed typed boundary in the draft, in
  the same spirit as the current `ContextObject` / `CoercedPath` boundary
- avoid leaving provenance-preserving versus provenance-detaching behavior to
  per-call-site interpretation of an observation record

### 28.5 Source-identity namespace separation should be made explicit in the
draft types

Current problem:

- the full eval-trace design already documents a concrete bug caused by mixing
  node-key identity and runtime-root URL identity in one generic string domain
- the current draft still uses raw strings for several of these identities

Why this is dangerous:

- the exact bug class that motivated `DepSource::fromNodeKey(...)` versus
  `DepSource::fromRuntimeRoot(...)` can be reintroduced in the new boundary if
  the environment uses one generic string-shaped identity API

Required correction:

- define disjoint nominal identity types for:
  - flake graph node keys
  - runtime-root locked URLs
  - runtime-root source IDs
  - stable recovery keys / source identities / reuse keys where those are
    semantically distinct
- prefer named factories and typed wrappers over generic string constructors

## 29. Nineteenth Adversarial Pass: Post-strengthening Review

This pass assumes the draft has been tightened to:

- introduce `eval-environment/domains.hh` for nominal semantic string/hash
  domains
- replace plain publication records with `PublishedStorePathString`
- make fetch/mount/completion carriers `Linear` where they must be consumed
- split lookup-path entry outcomes into a `std::variant`
- split locked versus unlocked runtime completion into distinct completion
  shapes and distinct bound mounted types

### 29.1 What this pass resolved

The earlier weak points are materially reduced, but not closed:

- source-identity drift is narrower because the draft now distinguishes:
  - flake graph node keys
  - runtime-root locked URLs
  - stable recovery keys
  - file-eval source and reuse identities
  via nominal tagged domains, while the flake `sourceIdentity` itself remains
  the stable logical identity and does not carry the locked fingerprint;
  version sensitivity is pushed to `graphDigest` / verification
- publication semantics are now modeled as an internal record shape, but the
  boundary is only actually sealed if `PublishedStorePathString` cannot be
  constructed directly outside `EvalEnvironment`:
  - `PublishedStorePathString` construction stays internal to
    `EvalEnvironment`
  - preserve/detach/plain combinations cannot be synthesized arbitrarily by
    callers while that constructor remains internal
  - if a direct construction escape hatch still exists, the publication
    boundary is still open rather than merely a representational cleanup
- critical fetch/mount phases are now `Linear`-backed rather than plain
  move-only structs
- locked versus unlocked runtime completion is no longer one carrier with
  optional fields

### 29.2 Remaining risks

1. The typed surfaces are stronger, but the eventual `.cc` implementation still
   needs to preserve the same invariants:
   - `resolveFetchIdentity(...)` must stay non-materializing
   - `materializeFetch(...)` must consume the linear identity and mint the next
     phase exactly once
   - `mountFetchedInput(...)` must choose the locked/unlocked bound mounted
     type based on actual fetch state, not caller preference

2. Some top-level observations still intentionally carry ordinary data fields,
   not nominal wrappers, because the semantic hazard is lower there:
   - environment variable names/values
   - placeholder strings
   - realised strings / file bytes
   This is acceptable so long as those fields do not become cross-domain
   identity carriers later.

3. The flake adapter remains the place where graph extraction is normalized.
   The new typed domains prevent many mix-ups, but they do not eliminate the
   need to keep that extraction logic single-owned in `libflake`.

4. The current plan still has explicit closure work left in three adjacent
   areas:
   - dep keys now stay typed through recording, interning, and decode entry
     points for the structured key families, but they still share the
     underlying `DepKeyId` / interned-blob substrate before final
     serialization/persistence
   - runtime-root persistence is typed through `TraceBackend`,
     `TraceStore`, and SQLite binding, but the overall dep/runtime-root story
     still relies on the generic interned-blob substrate before the final
     serialized form
   - remaining `EvalState` bypasses and shared-state ownership cleanup still
     need to land
   - explicit recording-target plumbing for detached recording call sites still
     has ambient fallback wiring in helper paths

5. Flake graph typing is still only partially pushed out.
   - nominal phase/domain wrappers now exist in `libflake`
   - `ResolvedFlakeGraph` now keeps `FlakeGraphNodeKey`, carrier/evaluation
     roots, and logical relative paths typed through more of the internal graph
     build and adapter flow
   - `lockedVersionIdentity` is now a canonical typed hash, not a fallback
    string bag; any remaining raw strings or `SourcePath` erasure in the graph
    build or adapter flow still mark incomplete typing pushout
   - the remaining erasures should be confined to explicit serialization
     boundaries such as lockfile JSON and graph-digest hashing, not general
     internal helper flow; if carrier-root and evaluation-root collapse again,
     the phase split is no longer working as intended

6. Session-open assembly is materially better, but still wider and more
   boilerplate-heavy than the intended end state.
   - the dead “assembled open” wrapper types are gone
   - the remaining work is to keep the helper/assembler surface narrow, leave
     compatibility factories only where out-of-scope callers still depend on
     them, and remove any remaining shuttle wrappers

7. Some nominal identity/domain types still look underthreaded relative to the
   intended boundary.
   - if types such as `ExternalRootIdentity` remain only lightly threaded or are
     still convertible back to generic string/blob carriers too early, the draft
     should treat them as unfinished rather than done; if they are dead aliases
     with no real API thread, delete them instead of carrying them as nominal
     scaffolding

### 29.3 Current bar before implementation

The following invariants are preserved by the current implementation. This is
a current readiness bar, not the final validation section; section 10 holds
the acceptance criteria.

1. no public raw string API for node-key, recovery-key, or runtime-root
   identity domains
2. no drop-without-consume path for `ResolvedFetchIdentity`, `FetchedInput`,
   `DetachedGraphMountedInput`, `BoundLockedMountedInput`, or
   `BoundUnlockedMountedInput`
3. no generic bound mounted carrier that lets callers pick locked versus
   unlocked completion after the fact
4. no caller may synthesize preserve/detach/plain publication results outside
   `EvalEnvironment`
5. if a single publication wrapper remains, it must be sealed at the
   construction boundary; if callers can still build
   `PublishedStorePathString` directly, the publication boundary is not sealed
   yet
   - in the current tree the desired shape is present, but this bullet stays
     live until the direct-construction escape hatch is provably absent

## 30. Current Implementation Notes

The current implementation now follows these concrete rules. This section is
current-state reporting, not target-state language; items 15+ are the current
work DAG and status notes, not acceptance criteria:

1. File-eval uses a two-key model.
   - reuse slots are keyed by a broad logical file-eval identity
   - backend/session namespaces are keyed by the semantic session digest
   - if a reuse-slot hit produces a different semantic session digest, the old
     cached session is not reused in place; it is replaced

2. File-eval `stableRecoveryKey` is source-specific rather than policy-only.
   - it is derived from the same logical file-eval identity family as the
     reuse-slot key
   - it must remain stable across Git-state changes for the same logical file
     or expression source

3. File-eval root-load dependency observations are carried in session-open
   authority and recorded by `TraceSession`.
   - root loaders must not call `TraceAccess::current()` directly to record
     `GitRevisionIdentity`

4. The request builder must not perform lookup-path context realization.
   - per-entry `NixStringContext` realization for lookup-path entries now
     belongs to `EvalEnvironment::resolveLookupPath(...)`

5. Typed reuse-slot keys must survive through the session-factory cache
   boundary.
   - the cache boundary must not erase flake vs file-eval slot identity back to
     raw `Hash`

6. `:reload`-style callers must clear file/import parse caches before reopening
   files or flakes.
   - this includes lookup-path resolution caches, not just parsed/imported
     expression caches
   - otherwise the evaluator can re-enter through the right root loader while
     still serving stale parsed/imported or path-resolved state

7. Flake backend reuse must not imply in-memory `TraceSession` reuse.
   - the flake semantic session key intentionally stays stable across certain
     local path content changes so the backend namespace can still be reused
   - but each flake open must get a fresh `TraceSession` with the current root
     loader, graph bindings, and registry seed
   - correctness for `description`-only changes vs output changes must come
     from `callFlake`-recorded deps, not a coarse synthetic `FileBytes(flake.nix)`

8. Typed session identities must survive through `TraceStore`.
  - `SessionConfig`, `SemanticSessionKey`, and `stableRecoveryKey` stay typed
    as `Blake3Hash` domains through the in-memory store/runtime layers
  - the first serialization boundary for those identities is SQLite binding
  - `Sessions.session_key`, `History.recovery_key`, and
    `SessionRuntimeRoots.session_key` therefore persist canonical BLOB digests,
    not ad hoc or hex-text strings

9. Dep-source and dep-key atoms must remain opaque canonical bytes until
   interning / SQLite binding.
- `DepSourceId` interning and reversible dep-key encoders must not rely on
  sentinel strings or delimiter-joined textual packing
- the remaining implementation gap is to keep the dep/runtime-root APIs typed
  all the way to the serialization boundary rather than collapsing to generic
  string views earlier in the call chain
- dep keys and runtime-root identities still share the generic
  `DepKeyId`/interned-blob substrate before final persistence; that substrate
  is the remaining gap, not the typed boundary itself
- `Strings.value` is therefore a `BLOB` table, not a `TEXT` table
- `DepSource` namespace separation is now complete: the type is
  `std::variant<AbsoluteDepSource, GraphNodeDepSourceKey, RuntimeRootSourceKey>`
  with `GraphNodeDepSourceKey = Tagged<_, std::string>` and
  `RuntimeRootSourceKey = Tagged<_, Blake3Hash>` — structurally disjoint.
  Factory methods `fromNodeKey`/`fromRuntimeRoot` are the intended
  construction paths. Serialization discriminates on prefix tags.
  (Prior text here said DepSource still erased identity into a single string
  payload — verified closed as of 2026-04-13 adversarial review, §31.3.)

10. Structured hash preimages must be framed canonically, not delimiter-joined.
   - this includes file-eval logical identity, flake source/recovery identity,
     flake graph digest components, directory-entry dep hashes, and current Git
     identity hashing
   - if the system needs a digest, build it from typed/framed fields; do not
     flatten multi-field state into bespoke strings first

11. Preserved store-path publication is represented as an internal record
    shape, but the boundary is only sealed if direct construction remains
    impossible.
   - `StorePathPublicationMode::Preserve` is not a best-effort rendering mode
   - callers must provide explicit `PathObject` provenance up front
   - `PublishedStorePathString` keeps preserve/detach/plain construction
     internal to `EvalEnvironment`
   - if callers can still build `PublishedStorePathString` directly, the
     publication boundary is still open and should be treated as an active gap
   - the remaining simplification question is representational only after that
     boundary is proven sealed

12. Locked runtime completion must consume a token that already proves `narHash`.
   - `completeLockedRuntimeFetch(...)` must not rediscover or synthesize
     runtime-root identity late
   - the mounted locked token carries `RuntimeRootNarHash` forward from the
     materialization/mount phase
   - completion is therefore publication/persistence only, matching the
     `Linear` typestate split

13. Local Git workdirs with `submodules = true` must fail explicitly if a
    declared submodule is missing or uninitialized.
   - this is not merely an uncacheable state
   - `submodules = true` means submodule content is part of the requested tree
   - dirty-workdir fingerprinting and workdir accessor assembly must therefore
     share the same “required initialized submodule” rule

14. Traced installable values must carry session keepalive per returned value,
   not via a mutable slot on the installable.
   - multiple values returned from repeated evaluation of the same installable
     must not invalidate each other
   - command-side holders such as REPL loaded values keep the returned wrapper,
     not a parallel mutable owner field

15. Dep-key/runtime-root typed-boundary closure now reaches the persistence
    boundary with explicit erasure points.
   - runtime-root persistence is typed through `TraceBackend`,
     `TraceStore`, and SQLite binding, using canonical runtime-fetch identity
     payloads rather than locked-URL strings
   - dep-key recording and decode paths distinguish the canonical encoded blob
     domains for derived-store-path, store-path-availability, and
     runtime-fetch-identity keys
   - those structured dep-key families now keep typed key IDs through
     `Dep::Key`, recording, decode, and verification instead of dropping
     immediately to a raw `DepKeyId`
   - the raw dep-key blob escape hatch is now private to dep-key
     encode/decode internals; hash/persistence code feeds typed key material
     rather than reaching back into a generic public blob resolver
   - generic dep-key storage IDs now appear only through explicit
     erase-for-persistence steps; they are no longer the semantic API surface,
     but they still form the shared interned-byte substrate that sits between
     typed recording and final persistence
   - runtime-root records now keep the fetched store path in a nominal
     `RuntimeRootStorePath` domain instead of decaying immediately to a bare
     `StorePath` at the trace-store boundary
   - libflake phase roles now stay nominally typed through more of the
     resolved-graph and session-adapter flow; remaining erasures should be
     serialization-only
   - if raw strings or `SourcePath` still leak through graph internals, the
     phase typing is still too weak
   - the known frontier is the typed-root/path regressions at graph, lockfile,
     and submodule boundaries

16. Recording dispatch is internalized by auto-dispatch overloads.
   - `BoundEffectScope` and `TraceAccess` overloads are private
   - auto-dispatch (`method(request)`) tries the session first, falls back
     to detached; Bound overloads delegate to TraceAccess internally
   - no call site outside `eval-environment.cc` passes a `TraceAccess` or
     `BoundEffectScope` to an `EvalEnvironment` method
   - direct `TraceAccess::current()` uses in primops and `TraceSession`
     internals are trace-runtime implementation details (shape deps,
     structured projections, etc.), not `EvalEnvironment` boundary APIs

17. `EvalState` now owns one consolidated `EvalEnvironmentSharedState`
    instead of scattering environment-adjacent caches as direct fields.
   - direct `EvalState` bypasses for path/store publication and imported-file
     recording now route through `EvalEnvironment`
   - environment-adjacent caches and session reuse are grouped behind the
     shared environment state instead of being spread across unrelated
     `EvalState` members
   - the final ownership split for `EvalEnvironmentSharedState` still needs to
     be made explicit in implementation planning
   - more broadly, `EvalEnvironment` is the strongest effect boundary in the
     current tree, but not yet an exclusive one: `EvalState` still retains
     bypass-capable helpers and root/store path construction surfaces
   - the current tree therefore has an explicit boundary but not a single
     exclusive effect owner yet

18. libflake phase typing has moved from “separate work stream” into live
    implementation, but the final edge cleanup is not done.
   - keep the nominal phase wrappers and adapters in `libflake`
   - preserve the current rule that remaining raw string/path collapse is
     confined to explicit serialization/digest boundaries
   - finish the last graph-edge, lockfile, and CLI/debug adapter cleanup
     without reintroducing generic `FlakeRef` / `SourcePath` confusion in
     internal helper flow
   - the current flake load/lock failures are evidence that this is still
     open work, not a closed boundary

19. Session-open assembly surface has already been simplified once.
   - `AssembledFlakeTraceSessionOpen` and
     `AssembledFileEvalTraceSessionOpen` are gone
   - `CapturedSessionOpenInputs` remains the linear capture boundary
   - the remaining work is to keep the helper declarations narrow, keep the
     public assembly helpers as thin compatibility shims, and avoid
     reintroducing shuttle wrappers

20. Underthreaded nominal identity types should remain on the cleanup list.
   - if types such as `ExternalRootIdentity` remain present but do not yet carry
     their weight through the live request/authority/persistence flow, they are
     still design debt, not proof that the boundary is closed

## 31. Adversarial Review (2026-04-13)

Four rounds of adversarial verification against the code. Each claim below
was checked by reading the relevant implementation, not by trusting the
plan text alone.

### 31.1 Bundle completion

| Bundle | Exit condition | Status | Evidence |
|--------|---------------|--------|----------|
| 1 | No new raw effectful code added directly | Complete (process gate) | EvalEnvironment exists, is the obvious path |
| 2 | fetchTree.cc, primops.cc, EvalState, shared flake code no longer perform raw effectful reads/mounts without EvalEnvironment | **Substantially complete** | All 8 concrete obligations met; `fetch()` helper gap (see below) |
| 3 | No split activeRegistry\_/activeBackend\_-style mutable authority; no hidden side channel beside DetachedEffectScope | Complete | Both fields deleted; TraceRuntime survives intentionally (§21.2) |
| 4 | Cache verification and record/recovery calls are normal typed calls in-process | **Substantially complete** | Protocol shell deleted (TraceStoreService, trace-store-protocol.hh, verify-pipeline.hh, OrchestratorHandle). VerificationOrchestrator remains as async scheduling wrapper (289 lines) owning VerificationSession + PrefetchPool + 7-step sequencing. Collapse path: move session-state custody to TraceSession, convert sequencing to a free function. async-runtime.hh (138 lines) owns io_context + worker pool. |
| 5 | Value-kind behavior substantially reduced | Not started | Explicit non-goal (§5.8) |
| 6 | Effectful primop code domain-owned | Not started | Explicit non-goal (§5.8) |

### 31.2 Bundle 2 gap: `fetch()` helper

The `fetch()` function in
[fetchTree.cc](/home/connorbaker/nix/src/libexpr/primops/fetchTree.cc)
(backing `builtins.fetchurl` and `builtins.fetchTarball`) performs raw
`store->ensurePath`, `fetchToStore`, `downloadFile`, and
`store->queryPathInfo` without going through `EvalEnvironment`.

The concrete migration obligations (items 1-8) are met — they target
`prim_fetchTree` and shared flake flows, not the `fetch()` helper. But the
exit condition says "fetchTree.cc no longer performs raw effectful reads,"
which covers the entire file.

This gap is documented in the Bundle 2 remaining list (above). Decision
needed: either migrate `fetch()` or explicitly scope it out with rationale.

### 31.3 Type-level enforcement (verified)

**Sealed boundaries:**
- `PublishedStorePathString` — private constructor, private
  preserve/detach/plain factories, `EvalEnvironment`-only friend.
  `appendSubpath` derives from existing valid instances only.
- `DetachedEffectScope` / `BoundEffectScope` — private constructors,
  `EvalEnvironment`-only friend. No external construction found.

**Linear typestate (all 10 verified, all abort if dropped unconsumed):**

| Type | Consume methods |
|------|-----------------|
| `ResolvedFetchIdentity` | `consumeForMaterialization` |
| `FetchedInput` | `consumeForMount` |
| `DetachedGraphMountedInput` | `consumeForGraphCompletion` |
| `BoundLockedMountedInput` | `consumeForRuntimeCompletion` |
| `BoundUnlockedMountedInput` | `consumeForRuntimeCompletion` |
| `RootLoaderCapability` | `discardUnused`, `intoRootLoader` |
| `CapturedSessionOpenInputs` | `consumeForAssembly` |
| `CommonTraceSessionAuthorityInputs` | `consumeForAssembly` |
| `FlakeGraphTraceSessionAuthorityRequest` | `consumeForAssembly` |
| `EvalTraceSessionAuthority` | `discardForSessionReuse`, `consumeForTraceSessionOpen` |

Three types (`RootLoaderCapability`, `CommonTraceSessionAuthorityInputs`,
`FlakeGraphTraceSessionAuthorityRequest`) have public `create()` factories —
intentional, as they are assembly inputs constructed by callers; linearity
still forces consumption.

**DepSource namespace separation:**
`DepSource` is `std::variant<AbsoluteDepSource, GraphNodeDepSourceKey,
RuntimeRootSourceKey>`. `GraphNodeDepSourceKey` is `Tagged<_, std::string>`,
`RuntimeRootSourceKey` is `Tagged<_, Blake3Hash>`. Structurally impossible to
confuse. Factory methods `fromNodeKey`/`fromRuntimeRoot` are the intended
construction paths. Serialization correctly discriminates on prefix tags.
Prior plan text saying "DepSource still erases graph-node vs runtime-root
identity into a single string payload" was incorrect — that gap has been
closed.

**Nominal Tagged domains:** 17 types in `domains.hh` (including `ResolvedLockedInput`
and `FinalizedLockedInput` for the finalization typestate), 12 in `phase-types.hh`.

**Construction authority vs linearity:** The three types with `public create()`
factories (`RootLoaderCapability`, `CommonTraceSessionAuthorityInputs`,
`FlakeGraphTraceSessionAuthorityRequest`) intentionally leave construction open.
The enforcement mechanism is linearity (must consume), not construction authority
(only blessed code may create). All callers in `libcmd` and `libflake` are trusted
code. If untrusted plugin code ever becomes a concern, introduce a general-purpose
`SealedKey<T>` passkey template in libutil rather than per-type friend enumeration.

**`Tagged<>` provides type-confusion prevention, not forgery prevention.** The
fetch-input lifecycle types (`OriginalFetchInput`, `ResolvedLockedInput`,
`FinalizedLockedInput`) are intentionally aggregate-constructible. Pipeline
enforcement comes from the transition functions (`resolveFetchIdentity` returns
`ResolvedLockedInput`, `mountInput` returns `FinalizedLockedInput`), not from
construction sealing. `Tagged<>` prevents passing a `ResolvedLockedInput` where
a `FinalizedLockedInput` is required — which is the actual bug class.

### 31.4 Structural limitation

`EvalState` retains public `ref<Store> store` and `ref<Store> buildStore`
for evaluator-internal operations (derivation assembly, value rendering).
Removing `store` from `EvalState` is not proposed.

The boundary is structurally enforced for the operations that previously
had shim methods on `EvalState`: `allowPath`, `allowClosure`,
`allowAndSetStorePathString`, `checkURI`, `findFile`, `copyPathToStore`,
`realiseContext`, `realiseString`, and `flushTraceContext` have all been
deleted. Calling any of them is a hard compile error. The shared
`EvalEnvironmentSharedState` is private on `EvalState` with explicit
`friend` declarations for the four authority functions.

New code must construct an `EvalEnvironment` and use the typed request
API directly. For the two shared helpers that multiple call sites need,
`copyPathToStoreViaEvalEnvironment` and `realiseStringViaEvalEnvironment`
are declared in `authority-internal.hh` as non-member functions.

The remaining convention-enforced frontier is direct `state.store->`
calls for operations not yet covered by `EvalEnvironment` methods (see
§31.5 out-of-scope table).

### 31.5 Store calls outside EvalEnvironment

**In-scope per Bundle 2 exit condition** (fetchTree.cc, documented above):

| Call site | Operation |
|-----------|-----------|
| `fetch()` line 504 | `store->ensurePath` (triggers substitution) |
| `fetch()` line 518 | `fetchToStore` (network fetch + store write) |
| `fetch()` line 524 | `downloadFile` (network fetch) |
| `fetch()` line 541 | `store->queryPathInfo` (store metadata) |

**In-scope (additional, migrated):**

| Call site | Operation | Migration | Status |
|-----------|-----------|-----------|--------|
| `storePath` primops.cc | `ensurePath` | Reordered after EvalEnvironment construction | Done |
| `readFile` primops.cc | `queryPathInfo` + `PathRefScanSink` | Moved into `EvalEnvironment::readFile` — `FileReadObservation::context` now populated for store paths | Done |

**Out-of-scope** (with rationale):

| Call site | Operation | Rationale |
|-----------|-----------|-----------|
| `derivationStrict` primops.cc:1904 | `writeDerivation` | Derivation assembly: `.drv` is the primary artifact. Coupling EvalEnvironment to `Derivation` types violates the abstraction. Bundle 6 territory. |
| `derivationStrict` primops.cc:1796,1800 | `computeFSClosure`, `readDerivation` | Derivation assembly: store metadata reads to enumerate transitive `DrvDeep` input derivations. Evaluator-internal. |
| `toFile` primops.cc:3083 | `addToStoreFromDump` | Text-addressed store write with pre-computed reference set. Structurally different from `copyPathToStore`. Low leverage. |
| `fetchMercurial` fetchMercurial.cc:87 | `fetchToStore` | Legacy builtin. Could migrate using existing `resolveFetchIdentity` → `materializeFetch` pipeline (identical pattern to `prim_fetchTree` migration). Low priority. |
| `fetchClosure` fetchClosure.cc:54,82,111 | `queryPathInfo` | Validation predicates (`isContentAddressed`) before `allowClosure`, which already routes through EvalEnvironment. Not dep-recorded observations. |
| `prim_import` primops.cc:279-285 | `isStorePath`, `parseStorePath`, `isValidPath` | Pure predicates determining code path (derivation import vs expression eval). No environmental observation significance. |
| `context.cc:327` | `queryPathInfo` | Verifier-side observation (legitimate) |

### 31.6 Validation items

Section 10 states: "These are end-state acceptance criteria only, not
claims about the current tree."

Checked items:

| # | Criterion | Status |
|---|-----------|--------|
| 1 | No new raw effectful calls outside EvalEnvironment | Process gate — met as policy |
| 2 | No separate runtime-root publication side channel | Met |
| 3 | No parallel in-process protocol shell | Met (Bundle 4) |
| 4 | No change to compact Value layout | Met |
| 5 | Value-kind behavior measurably reduced | Not met (Bundle 5 deferred) |
| 6 | Dep recording driven by EvalEnvironment return types | Substantially met |
| 7 | Explicit authority objects replace ambient capture | Met |
| 8 | Three effect modes with documented legality rules | Met |
| 9 | Shared flake flows no longer call mountInput directly | Met |
| 10 | EvalTraceSessionAuthority carries exact SessionConfig fields | Met |
| 11 | No persistent general-purpose EvalState & | **Not met** — authority holds `EvalState *` |
| 12 | EvalEnvironment not owned by EvalState | Met |
| 13 | Effect-adjacent caches not direct EvalState members | **Partially met** — consolidated in `EvalEnvironmentSharedState`; `EvalState` holds `shared_ptr` but it is now private with friend access only for the four authority functions |

Items 11 and 13 are acknowledged as deferred in the plan's own status notes.
Items 14-44 were not individually audited.

### 31.7 EvalState bypass surface (updated 2026-04-14)

All effectful shim methods have been deleted from `EvalState`:
`allowPath`, `allowClosure`, `allowAndSetStorePathString`, `checkURI`,
`findFile` (both overloads), `copyPathToStore`, `realiseContext`,
`realiseString`, `flushTraceContext`, and the `FoundLookupPath` struct.

Former callers across 11 files now construct `EvalEnvironment` directly.
Two shared helpers are declared in `authority-internal.hh` for call sites
that need the full coercion/realization pipeline:
- `copyPathToStoreViaEvalEnvironment` — derivation-name validation,
  already-in-store fast path, context merging
- `realiseStringViaEvalEnvironment` — `coerceToString` + `realiseContext`
  + `rewriteStrings`

`mountInput`, `recordRuntimeRoot`, `addRegistryMountPoint` do not exist
on `EvalState`. `callPathFilter` remains — it is the actual filter
evaluator callback, not an EvalEnvironment shim.

### 31.8 TraceAccess instrumentation

The `TraceAccess::current()` call sites in primops.cc (~14) and eval.cc (~3)
are the standard dep-recording instrumentation API for non-EvalEnvironment
deps (shape deps, structured projections, volatile exec, etc.). All are
properly guarded by `traceActiveDepth`. These are trace-runtime
implementation details, not `EvalEnvironment` boundary APIs.

The previous 3-tier EvalEnvironment dispatch sites (4 in primops.cc) and
the manual `prim_currentSystem_thunk` recording site have been replaced by
auto-dispatch overloads.

### 31.9 Flake root frontier

The plan (§30.18) characterizes the remaining frontier as "final edge
cleanup" on a "live implementation." The broad `/flake.nix` collapse is
fixed. Remaining frontier is "narrower and centered on local-source handling
plus fetchGit/CA-build cases." Unit tests pass (1481/1481 on pre-squash
tree); build succeeds.

### 31.10 Completed since last review

- **Auto-dispatch overloads**: 13 auto-dispatch methods added; TraceAccess
  and BoundEffectScope overloads privatized. ~25 call sites simplified from
  2-tier/3-tier dispatch boilerplate to single-line calls.
- **Bound→TraceAccess delegation fix**: 4 Bound overloads (`observePath`,
  `readDirectory`, `realiseContext`, `publishStorePath`) now delegate through
  their TraceAccess sibling, preventing a latent enrichment-bypass bug class.
- **Dead code removal**: `EvalEnvironment::realiseString` (3 overloads),
  `recordStringRealisationObservation`, `StringRealisationObservation`,
  `RealiseStringRequest`, `buildRealiseStringRequest` — zero external callers.
- **Recording helper cleanup**: removed unused `const Store *` parameter from
  `recordContextRealisationObservation`.
- **Auto-dispatch standalone fallback**: all 13 auto-dispatch overloads now
  check `TraceAccess::current()` as middle-priority fallback between
  `tryBindCurrentEvalSession()` and detached/observeOnly. 5 new
  `TraceAccess` overloads added for Group B functions (`readEnvVar`,
  `resolveLookupPath`, `authorizeStorePath`, `authorizeStoreClosure`,
  `renderAuthorizedStorePath`); Bound overloads refactored to delegate
  through them. Fixes 209 dep-precision unit test failures (§33.8).
- **`FileReadObservation::context` removed**: ref-scan reverted to
  `prim_readFile`; `context` field removed from `FileReadObservation`.
  Fixes `readfile-context` and `eval-trace-graph` test 7 regressions
  (§33.4).
- **Unlocked runtime root persistence**: `completeUnlockedRuntimeFetch`
  now calls `recordRuntimeRoot` (querying narHash from the store),
  matching `completeLockedRuntimeFetch`. Fixes eval-trace-graph test 8
  warm-verify failure (0 runtime roots in DB).
- **`path:` warm-verify enabled**: tests 2, 3, 7 in eval-trace-graph.sh
  now assert `NIX_ALLOW_EVAL=0`. Accessor-type instability was already
  resolved by prior session-key normalization work.
- **Stale test comments fixed**: F-1 KNOWN LIMITATION removed (dirty-tree
  invalidation works); tests 2/3/7 accessor-instability comments removed.

### 31.11 Remaining work (prioritized, updated 2026-04-14)

**Done (boundary hardening pass, §33):**
- ~~Run tests against current tree~~ — build clean, zero warnings
- ~~Migrate `fetch()` helper or scope it out~~ — scoped out with rationale (§31.5)
- ~~Move `prim_storePath`'s `ensurePath` after EvalEnvironment construction~~ — done
- Delete EvalState shim methods — done (§31.7)
- Make `evalEnvironmentSharedState` private — done
- Seal `GraphFetchCompletion` and `EvalPolicySnapshot` — done
- ~~Move `prim_readFile` ref-scan into `EvalEnvironment::readFile`~~ — reverted
  (§33.4); ref-scan stays in `prim_readFile` where it is co-located with
  its only consumer; `FileReadObservation` has no `context` field
- Delete `TraceRuntime::flush()` and `EvalState::flushTraceContext()` — done
- Fix stale plan text and documentation — done
- Auto-dispatch standalone `TraceAccess` fallback — done (§33.8)
- ~~Fix eval-trace-deps test 13b~~ — all 222 functional tests pass (OK: 222,
  Fail: 0 as of 2026-04-14)
- Enable warm-verify for `path:` relative input tests — done; tests 2, 3, 7
  in eval-trace-graph.sh now assert `NIX_ALLOW_EVAL=0` (accessor-type
  instability was already resolved by prior session-key normalization)
- Fix `completeUnlockedRuntimeFetch` missing `recordRuntimeRoot` — done;
  unlocked impure `builtins.fetchTree` now persists runtime root to
  `SessionRuntimeRoots` DB so warm-verify can load it; test 8 in
  eval-trace-graph.sh now asserts `NIX_ALLOW_EVAL=0`
- Fix stale test F-1 comment — done; KNOWN LIMITATION header removed
  (dirty-tree session invalidation works correctly)

**Short-term:**
1. Type `parseFlakeInput`/`parseFlakeInputs` carrier-root parameter
   (`const SourcePath &` → `CarrierRootPath` in `flake.cc:323, 371, 455`)

**Medium-term (deferred validation items):**
2. Remove `EvalState *` from `EvalEnvironmentAuthority` (item 11)
3. Migrate `fetch()` helper to `fetchers::Input` pipeline (requires
   framework support for `expectedHash` pinning and flat-file mode)

**Long-term (explicit non-goals for first draft):**
4. Bundles 5 and 6 (Value views, primop domain split)
5. Legacy fetch builtins (fetchMercurial, fetchClosure store-query paths)
6. `VerificationOrchestrator` collapse into `TraceSession` (§31.1)

## 32. Parameterized Type Infrastructure

This section documents the compile-time infrastructure used to build
parameterized types in the eval-environment boundary. It is a design
reference, not an acceptance criterion.

### ConditionalBase / Absent<T>

`conditional-base.hh` in libutil provides two primitives:

- `Absent<T>` — an empty placeholder struct keyed on T for EBO uniqueness.
  Using it as a public base adds no bytes but reserves a distinct base-class
  identity so that two `Absent<T>` bases with different T remain distinct.
- `ConditionalBase<Cond, T>` — selects `T` when `Cond` is `true`, or
  `Absent<T>` when `Cond` is `false`.

These are used as `public` bases of parameterized class templates. Fields
from `T` are physically present when the corresponding condition is `true`,
and absent when it is `false`. Attempting to access a field on `Absent<T>`
is a compile error.

### Parameterized type families

Four parameterized families use this infrastructure:

**1. `EffectScope<EffectMode>`**

Axis: `EffectMode` enum with enumerators `Detached` and `Bound`.

- `Detached` — no trace session field present.
- `Bound` — has a trace session field.

Named aliases:
- `DetachedEffectScope = EffectScope<EffectMode::Detached>`
- `BoundEffectScope = EffectScope<EffectMode::Bound>`

**2. `MountedInput<MountMode>`**

Axis: `MountMode` enum with enumerators `DetachedStandalone`, `DetachedGraph`,
`BoundLocked`, and `BoundUnlocked`.

Always-present base: `MountedStorePath{storePath, provenance}`.

Conditional fields:
- `FinalizedLockedInput` — present on all modes except `DetachedGraph`.
- `promotedGraphSource` — present only on `DetachedGraph`.
- `narHash` — present only on `BoundLocked`.

Pruned fields (not present on any instantiation):
- `accessor` — dead after mount; not carried forward.
- `originalInput` — passed separately where needed rather than stored on
  the carrier.

Conditional linearity:
- `DetachedStandalone` is `MoveOnly`.
- `DetachedGraph`, `BoundLocked`, and `BoundUnlocked` are `Linear`.

Named aliases:
- `DetachedStandaloneMountedInput = MountedInput<MountMode::DetachedStandalone>`
- `DetachedGraphMountedInput = MountedInput<MountMode::DetachedGraph>`
- `BoundLockedMountedInput = MountedInput<MountMode::BoundLocked>`
- `BoundUnlockedMountedInput = MountedInput<MountMode::BoundUnlocked>`

**3. `LookupPathResolution<LookupPathOrigin>`**

Axis: `LookupPathOrigin` enum with enumerators `Existing`, `Downloaded`,
`HookResolved`, and `Missing`.

Conditional fields:
- Resolved root fields — absent on `Missing`; present on all others.
- `materializedStorePath` — present only on `Downloaded`.

**4. `LookupPathEntry<LookupPathEntryDetail, LookupPathRealization>`**

Two independent axes:

- `LookupPathEntryDetail`: `Full` (carries context and origin) vs `Identity`
  (stripped to prefix/path identity only).
- `LookupPathRealization`: `Unrealized` (pre-context-realization) vs
  `Realized` (post-context-realization).

Construction is sealed:
- Identity entries are only obtainable via `toIdentity()`.
- Full entries are only obtainable via `buildLookupPathEntrySpec()`.

Named aliases include:
- `UnrealizedLookupPathIdentity = LookupPathEntry<LookupPathEntryDetail::Identity, LookupPathRealization::Unrealized>`

### Input identity typestate

Three `Tagged<>` nominal types track the fetch input through pipeline stages:

- `OriginalFetchInput = Tagged<_, shared_ptr<const Input>>` — the caller-provided
  input before resolution. Used to compute runtime fetch identity keys for
  unlocked inputs. Carried on `FetchIdentityRequest` and
  `FetchedInput::MountPayload`.
- `ResolvedLockedInput = Tagged<_, shared_ptr<const Input>>` — after
  cache/accessor resolution, before finalization. No `narHash` or `__final`
  attrs.
- `FinalizedLockedInput = Tagged<_, shared_ptr<const Input>>` — after
  finalization by `mountInput`. Has `narHash` and `__final` attrs.

All three are non-interconvertible via `Tagged` nominal typing.

`mountInput` is a pure function: it takes a `const ResolvedLockedInput &` and
produces a `FinalizedLockedInput`. It does not mutate its argument in place.

### MountedStorePath shared base

`MountedStorePath{storePath, provenance}` is the common base for all
post-mount carrier types. It is inherited (publicly) by:

- `MountedInput` (all four `MountMode` instantiations)
- `GraphFetchCompletion`
- `PublishedRuntimeFetch` (both locked and unlocked instantiations)

`DetachedMountedStorePath` is a plain alias for `MountedStorePath` used at
sites that hold only the base without a mode-specific carrier.

### BoundMountedInput and RuntimeFetchCompletion variants eliminated

The old `BoundMountedInput = variant<BoundLockedMountedInput,
BoundUnlockedMountedInput>` has been removed, along with
`LockedRuntimeFetchCompletion`, `UnlockedRuntimeFetchCompletion`,
`RuntimeFetchCompletion` (variant), and the old `PublishedRuntimeFetch` struct.

These have been replaced by:

- `PublishedRuntimeFetch<RuntimeFetchLockMode>` — parameterized completion type
  inheriting `MountedStorePath`. Conditional fields: `runtimeRootCandidate`
  (Locked only), `promotedSource` (Unlocked only). Provides `runtimeSource()`
  accessor for the matching `DepSource`. Aliases:
  `LockedPublishedRuntimeFetch`, `UnlockedPublishedRuntimeFetch`.
- `RuntimeFetchResult = variant<LockedPublishedRuntimeFetch,
  UnlockedPublishedRuntimeFetch>` — returned by `mountAndCompleteRuntimeFetch`.
- `mountAndCompleteRuntimeFetch(BoundEffectScope &&, FetchedInput &&)
  -> RuntimeFetchResult` — internalizes the locked/unlocked fork.

The `runtimeSource()` accessor fixes a pre-existing provenance mismatch in
the unlocked path: the old code passed `nullopt` to `emitTreeAttrs`, which
recomputed the source from the finalized input's attrs (including narHash and
__final). But `registerRuntimeRootMount` had registered the mount under a key
derived from the original input's attrs (without narHash/__final). These
produce different BLAKE3 hashes, causing provenance recorded on the `outPath`
string to not match the registered runtime root mount. The new code passes
`runtimeSource()` explicitly, aligning provenance with the mount registration
for both locked and unlocked paths.

### Naming convention

All `using` aliases for parameterized template instantiations follow the
pattern `<Tag><Template>`, where the tag matches the enum enumerator name
exactly.

Examples:
- `BoundEffectScope = EffectScope<EffectMode::Bound>`
- `DetachedEffectScope = EffectScope<EffectMode::Detached>`
- `BoundLockedMountedInput = MountedInput<MountMode::BoundLocked>`
- `BoundUnlockedMountedInput = MountedInput<MountMode::BoundUnlocked>`
- `DetachedGraphMountedInput = MountedInput<MountMode::DetachedGraph>`
- `DetachedStandaloneMountedInput = MountedInput<MountMode::DetachedStandalone>`

## 33. Boundary Hardening Pass (2026-04-14)

Adversarial review of the refactor plan (§31) and the codebase identified
11 concrete gaps. A second adversarial pass over proposed fixes rejected
ad-hoc per-type patches in favor of structural changes that fix entire
categories. This section documents the changes.

### 33.1 `EvalEnvironmentSharedState` made private

`EvalState::evalEnvironmentSharedState` moved from public to private.
Four `friend` declarations in `eval.hh` grant access to the authority
functions declared in `authority-internal.hh`. Internal helpers that
previously reached through the public field (`evalFileImpl` in `eval.cc`,
`makeBaseEvalEnvironmentAuthority` in `authority.cc`) now receive the
shared state as a parameter from the friended callers.

### 33.2 EvalState shim methods deleted

All effectful shim methods removed from `EvalState`:

| Deleted method | Callers migrated |
|----------------|-----------------|
| `allowPath` | `common-eval-args.cc` (×2), `fetchMercurial.cc`, `context.cc`, `profile.cc` (×2), `eval.cc` test |
| `allowClosure` | `fetchClosure.cc` (×3) |
| `allowAndSetStorePathString` | `fetchTree.cc` (×2), `primops.cc` |
| `checkURI` | `fetchMercurial.cc`, `fetchTree.cc` |
| `findFile` (2 overloads) | `common-eval-args.cc`, `nix-instantiate.cc` |
| `copyPathToStore` | `value-to-json.cc` |
| `realiseContext` | `primops.cc` |
| `realiseString` | `nix_api_value.cc` |
| `flushTraceContext` | `develop.cc` (×2), `run.cc`, `formatter.cc`, `env.cc` |

Also deleted: `FoundLookupPath` struct, `TraceRuntime::flush()` stub.

Two shared helpers declared in `authority-internal.hh` for call sites
needing the full coercion/realization pipeline:
- `copyPathToStoreViaEvalEnvironment` — promoted from file-static in
  `eval.cc`; includes derivation-name validation and already-in-store
  fast path
- `realiseStringViaEvalEnvironment` — new; combines `coerceToString` +
  `realiseContext` + `rewriteStrings`

Migration preserved error behavior: `findFile` callers wrap
`resolveLookupPath` in try/catch converting `Error` to `ThrownError`,
matching the old shim's behavior.

### 33.3 Sealed aggregate result types

`GraphFetchCompletion` and `EvalPolicySnapshot` converted from plain
aggregates to non-aggregate types with private constructors:

- `GraphFetchCompletion` — `friend` of `detail::MountedInput<>` (the
  sole constructor is `consumeForGraphCompletion`). Moved below `MountMode`
  enum and `detail::MountedInput` forward declaration to satisfy ordering.
- `EvalPolicySnapshot` — `friend class EvalEnvironment`. Construction
  site uses positional arguments with `/* fieldName */` comments.

Both have `static_assert(!std::is_aggregate_v<...>)` guards.

### 33.4 Store-call frontier tightened

- `prim_storePath`: `ensurePath` reordered after `EvalEnvironment`
  construction with explanatory comment.
- `prim_readFile`: reference-scan logic (`queryPathInfo` + `PathRefScanSink`)
  stays in `prim_readFile` (primops.cc), co-located with its only consumer.
  An earlier attempt to move it into `EvalEnvironment::readFile`'s
  `ObserveOnlyTag` overload was reverted because the `ObserveOnlyTag`
  overload initialized `FileReadObservation::context` from
  `request.coercedPath.context` (the input path's string context) rather
  than building a fresh ref-scanned context, causing `readfile-context`
  and `eval-trace-graph` test 7 to fail. The fix removed `context` from
  `FileReadObservation` entirely — the struct now carries only pure
  observations (`request`, `observedPath`, `bytes`, `textObject`). The
  ref-scan is a `prim_readFile`-specific concern: it builds a fresh
  `NixStringContext` from store-path references found in the file bytes
  and passes it to `mkString`.
- `fetch()` helper: scoped out with rationale in §31.5. The trace
  recording at lines 526-537 (via `TraceAccess`) already happens; the
  remaining raw store calls are materialization (`fetchToStore`,
  `downloadFile`) and hash verification (`queryPathInfo`).

### 33.5 Documentation and plan consistency

- `eval-environment.hh` stale "design skeleton" comment replaced
- §29.3 "before wiring into the build" preamble corrected
- §31.1 Bundle 4 changed to "Substantially complete"
- §31.3 added design intent for `create()` factories and `Tagged<>` types
- §31.5 added explicit in-scope/out-of-scope tables with rationale
- §30 functional test status corrected (all 15 pass; coverage gaps, not
  failures)
- `src/libexpr-tests/eval-trace/CLAUDE.md` fixed stale property test claim
- `lookupPathResolved` documented as intentionally non-concurrent

### 33.6 Design decisions recorded

**Construction authority vs linearity:** Public `create()` factories on
`RootLoaderCapability`, `CommonTraceSessionAuthorityInputs`, and
`FlakeGraphTraceSessionAuthorityRequest` are intentional. Linearity
(must consume) is the enforcement mechanism, not construction authority.

**`Tagged<>` provides confusion prevention, not forgery prevention.**
Fetch-input lifecycle types are intentionally aggregate-constructible.
Pipeline enforcement comes from transition functions.

**`readFile` ref-scan stays in `prim_readFile`:** The `ObserveOnlyTag`
overload does not call `queryPathInfo` or perform ref-scanning. An
earlier attempt to move ref-scanning into the `ObserveOnlyTag` overload
caused a regression (§33.4): it initialized `FileReadObservation::context`
from `request.coercedPath.context` instead of building a fresh context.
The fix removed `context` from `FileReadObservation` entirely. The
ref-scan (`queryPathInfo` + `PathRefScanSink`) remains in `prim_readFile`
(primops.cc), co-located with its only consumer.

### 33.7 Files changed

26 files, 306 insertions, 298 deletions. Net -8 lines excluding plan/doc.

**Headers modified:**
- `eval.hh` — private shared state, deleted 10 method declarations, friend
  declarations
- `authority-internal.hh` — 2 shared helper declarations
- `observation-types.hh` — sealed `GraphFetchCompletion`, `static_assert`;
  removed `context` field from `FileReadObservation`
- `session-types.hh` — sealed `EvalPolicySnapshot`, `static_assert`
- `context.hh` — deleted `flush()` declaration
- `eval-environment.hh` — fixed stale comment

**Implementations modified:**
- `eval.cc` — deleted 9 shim implementations, promoted
  `copyPathToStoreViaEvalEnvironment`, added
  `realiseStringViaEvalEnvironment`, parameter-threaded `evalFileImpl`
- `primops.cc` — deleted `realiseContext`/`realiseString`, migrated
  `prim_toFile`/`import`, restored `readFile` ref-scan (reverted from
  `eval-environment.cc`), reordered `prim_storePath`
- `eval-environment.cc` — removed `readFile` ref-scan and `context`
  field from `FileReadObservation`; sealed `EvalPolicySnapshot`
  construction; added 5 `TraceAccess` overloads for Group B functions;
  added `TraceAccess::current()` fallback to all 13 auto-dispatch
  overloads (§33.8)
- `authority.cc` — parameter-threaded `makeBaseEvalEnvironmentAuthority`
- `context.cc` — deleted `flush()`, migrated `allowPath`

**Caller migrations:**
- `fetchClosure.cc`, `fetchMercurial.cc`, `fetchTree.cc` — EvalEnvironment
  direct calls
- `common-eval-args.cc`, `nix-instantiate.cc` — `resolveLookupPath` with
  error handling
- `value-to-json.cc` — `copyPathToStoreViaEvalEnvironment`
- `nix_api_value.cc` — `realiseStringViaEvalEnvironment`
- `profile.cc`, `eval.cc` test — `authorizeStorePath`
- `develop.cc`, `run.cc`, `formatter.cc`, `env.cc` — deleted
  `flushTraceContext` calls

### 33.8 Auto-dispatch standalone TraceAccess fallback (2026-04-14)

The 13 auto-dispatch overloads previously checked only
`tryBindCurrentEvalSession()` (which queries `currentTraceSession()`).
This missed the standalone `DepCaptureScope` path used by dep-precision
tests: `DepCaptureScope` writes to `currentStandaloneDepCtx()`, which
`TraceAccess::current()` checks but `tryBindCurrentEvalSession()` does
not. All oracle deps (readFile, readDir, getEnv, pathExists, etc.)
recorded zero deps in the test fixture, causing 209 unit test failures.

**Fix:** All 13 auto-dispatch overloads now check three recording
contexts in priority order:

1. `tryBindCurrentEvalSession()` — bound TraceSession (production path)
2. `eval_trace::TraceAccess::current()` — standalone recording context
   (DepCaptureScope in tests, non-fiber production paths)
3. detached / observeOnly fallback — no recording

This matches how shape deps in `primops.cc`/`shape-deps.cc` already
record via `TraceAccess::current()`.

**Group A** (7 functions with existing `TraceAccess` overloads):
`observePath`, `readFile`, `readDirectory`, `realiseContext`,
`publishStorePath`, `copyPathToStore`, `observeSessionSystem`. The
auto-dispatch calls the `TraceAccess` overload directly — same code
path as `BoundEffectScope` (which delegates via `withRecordingAccess`).
No duplication.

**Group B** (5 functions without prior `TraceAccess` overloads):
`readEnvVar`, `resolveLookupPath`, `authorizeStorePath`,
`authorizeStoreClosure`, `renderAuthorizedStorePath`. New private
`TraceAccess` overloads added; `BoundEffectScope` overloads refactored
to delegate through them via `withRecordingAccess`. Both session-based
and standalone paths now converge on the same implementation.

**Group C** (1 function with no recording): `authorizeUri`. No
`TraceAccess` fallback needed — Bound and Detached overloads are
identical (pure policy check, no dep recording).

**`environment.hh`**: 5 new private `TraceAccess` overload declarations
added to the TraceAccess overloads section.
