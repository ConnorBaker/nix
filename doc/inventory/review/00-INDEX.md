# Review reports — index

Every investigation that has run against the catalog has a report
here. Reports are grouped by pass; each entry records the question
the report answered and any follow-ups it spawned.

The conventions:
- **E1-E8**: eight follow-up agents queued from the consolidation
  pass to address deferred items in the four-pass review.
- **F1-F3**: "follow-up" findings flagged *during* E1-E8 that needed
  their own investigation.
- **B1-B3**: structural decisions deferred from the consolidation
  pass (Category B in the consolidation log).
- **C1**: marginal observations deferred from the consolidation pass
  (Category C).
- **V1-V5**: verification pass — pure source-evidence verification of
  weak/mixed-evidence claims from the E/F/B/C reports.
- **01-05**: the original four-pass review (two adversarial passes,
  two pattern-discovery passes) plus the consolidation log.

## Original four-pass review

| Report | Question | Spawned |
| ------ | -------- | ------- |
| [01-adversarial-existing.md](01-adversarial-existing.md) | Pass 1 of adversarial review of the existing catalog: verdict-table contradictions, identifier drift, mis-categorisations | 03 (deeper) |
| [02-overlooked-patterns.md](02-overlooked-patterns.md) | Pass 1 of pattern discovery: cross-shard patterns the per-shard catalogers missed; produced 23 N-candidates plus clusters A-D, uplift series U1-U10, architectural themes A1-A7, redundant abstractions R1-R10 | 04 (deeper) |
| [03-adversarial-existing-deeper.md](03-adversarial-existing-deeper.md) | Pass 2 of adversarial review: verifies pass-1's claims; finds new hazards; substantive disagreements | 05 (consolidation) |
| [04-overlooked-patterns-deeper.md](04-overlooked-patterns-deeper.md) | Pass 2 of pattern discovery: verifies pass-1's N1-N23; proposes N24-N44; adds clusters E/F/G | 05 (consolidation) |
| [05-consolidation-log.md](05-consolidation-log.md) | Consolidation of all four prior reports into the catalog. 89 applies / 13 defers / 4 rejects. Records every decision per finding | E1-E8 |

## E series — eight follow-up agents

These were queued from the consolidation log's deferred follow-ups.

| Report | Question | Verdict | Spawned | Verified by |
| ------ | -------- | ------- | ------- | ----------- |
| [E1-impureOutputHash.md](E1-impureOutputHash.md) | Is `impureOutputHash` (libstore/derivations.cc) genuinely dead and safe to delete? Internal linkage; no downstream consumers; lix already removed it | Safe to delete as a standalone PR | none | none needed (binary-level evidence via `nm -m`) |
| [E2-c-api-settings.md](E2-c-api-settings.md) | Audit C-API `*Settings` field exposure for #134/#138/#144 | Source-compat for #134 and #144 (with caveat); #138 source-only break | F1 (eval-state-builder dead-storage) | V1 |
| [E3-goal-co-property-test.md](E3-goal-co-property-test.md) | Specify a test that pins `Goal::Co`'s O(1) frame-depth invariant before any partial #160 migration | Heap-allocation count via member operator new on Goal::promise_type; threshold `peakLive <= 8` while `cumulative >= 1000`; sanity-check inverse case | none | none needed (specification) |
| [E4-daemon-client-error-matrix.md](E4-daemon-client-error-matrix.md) | Build a daemon-client error-message preservation matrix for #125's `boost::format → std::format` migration | 6 protocol versions; bidirectional matrix; ANSI-strip canonical normalisation; both text-grep contracts must be preserved | F2 (`"is not valid"` second contract; `binary-cache-store.cc` suspected bug) | V2 (in flight) |
| [E5-branch-verification.md](E5-branch-verification.md) | Specify periodic verification so catalog body counts don't drift silently | `doc/inventory/counts.toml` sidecar; weekly cron + manual pre-PR run; tolerance shapes; 8 unautomatable claims flagged | none | none needed (specification) |
| [E6-test-support-consolidation.md](E6-test-support-consolidation.md) | Validate Cluster F (N28, N39, N41) consolidation direction by walking every fixture | N28 refined to two layered dicts; N39 deferred until U10; N41 fits 16/22 with hazards (corrected by V5 to 27/28) | none | V5 |
| [E7-forward-decl-audit.md](E7-forward-decl-audit.md) | Audit forward-decl consolidation (#166, N9) for source-compat and ABI impact | Ship `serialise-fwd.hh`, `store-fwd.hh`, AND `eval-fwd.hh` together; zero C-API impact (global namespace); add CI guard | none | none needed (verified end-to-end on read) |
| [E8-vtrue-vfalse-invariant.md](E8-vtrue-vfalse-invariant.md) | Recommend a coherent invariant for `Value::vTrue`/`vFalse`/`vNull`/`vEmptyList` | Direction A (rename, drop singleton implication) — recommended on structural grounds; perf claim was unsupported | none | V4 (downgraded perf claim) |

## F series — follow-ups from E-pass findings

| Report | Question | Verdict | Verified by |
| ------ | -------- | ------- | ----------- |
| [F1-eval-state-builder.md](F1-eval-state-builder.md) | Verify E2's flagged dead-storage in `nix_eval_state_builder_load`; classify dead / bug / defensive | Latent UAF (not just dead storage). Recommend folding the fix into #138 | V3 |
| [F2-is-not-valid-contract.md](F2-is-not-valid-contract.md) | Verify E4's second wire-crossing text-grep contract (`"is not valid"`) and the suspected `binary-cache-store.cc` bug | Contract mostly dead code today (bypassed by `MINIMUM_PROTOCOL_VERSION = 1.18`); `BadStorePathName` is a surviving fire path; `binary-cache-store.cc` not-bug | none needed (verified from source + git history) |
| [F3-proto-version-drift.md](F3-proto-version-drift.md) | Verify E4's flagged `PROTOCOL_VERSION = 1.39 vs. catalog's 1.38` | Both numbers exist; `WorkerProto::latest` struct = 1.38, `PROTOCOL_VERSION` macro = 1.39; catalog updated to document both | none needed (direct file read) |

## B series — structural decisions

| Report | Question | Verdict |
| ------ | -------- | ------- |
| [B1-134-135-144.md](B1-134-135-144.md) | Should #134/#135 be demoted into #144? | Keep cross-referenced. **#135 does NOT actually depend on #136** (catalog claim is wrong about that link); #134 does require #136. Disagreed with consolidator's rationale, agreed with outcome |
| [B2-65-211-merge.md](B2-65-211-merge.md) | Should #65 (primop arg-validation) and #211 (`force*` family) be merged? | Keep cross-referenced. **`forceAttrs`/`forceList` use `withTrace` rather than `addTrace`+try/catch** — different shape from the rest. #211 is 7 bodies, not 10. #65 is 79 canonical sites + ~135 bespoke. Different risk profiles, different consumer scopes |
| [B3-18-split.md](B3-18-split.md) | Should #18 (`MakeError`/`CloneableError`) be split into 18a/18b/18c? | Keep umbrella, rewrite body. **22 hand-written derivations (not 14)**; 18a is not a real refactor target (`MakeError` already is CRTP sugar); 18b is per-class because most carry intrinsic payload data; 18c is too small for top-level status |

## C series — marginal observations

| Report | Question | Verdict |
| ------ | -------- | ------- |
| [C1-marginal-observations.md](C1-marginal-observations.md) | Investigate four/five marginal observations the consolidator deferred (`SourceAccessor::number`, `Bindings` k-way merge, `Counter::enabled`, `SymbolTable` append-only, `dummy`/sentinel) | All five legitimately deferred. **One adjacent finding worth a new candidate**: `Hash::dummy` / `StorePath::dummy` placeholder-then-overwrite across 5 sites; codebase already signals via two `// FIXME: hack` tags |

## V series — verification pass

The user required that weak or mixed evidence be replaced by semantic
source evidence. Each V-report verifies (or downgrades) a specific
prior claim.

| Report | Question | Verdict |
| ------ | -------- | ------- |
| [V1-e2-c-api-exposure.md](V1-e2-c-api-exposure.md) | Verify E2's "no settings struct type is exposed across the C ABI" | **Refuted with caveats**: 5 of 6 C-API libraries `install_headers` with their `*_internal.h(h)` files; opacity holds for C consumers but layout is exposed to C++ consumers. Adds a release-note ABI caveat to #138 |
| V2-e4-reachability.md | Per-site reachability of E4's 61 positional-format wire-crossing sites | **In flight** |
| [V3-f1-trace.md](V3-f1-trace.md) | Code-trace verify F1's UAF claim | **Confirmed**: source unambiguously establishes the dangling pointer. `nix::ref<bool>` is sole owner; not transferred in `_build`; `_free` destroys it; `EvalState` reads through dangling `bool *` in `EvalState::ensureLazyPathCopied` and `addToStore`/`fetchToStore` paths |
| [V4-e8-eqvalues-perf.md](V4-e8-eqvalues-perf.md) | Verify E8's "negligible performance benefit" claim for Direction B | **Inconclusive on perf — downgrade**. Direction A confirmed on four non-perf grounds. **Critical finding**: `ExprOpEq::eval`'s stack-local-copy semantics make the highest-frequency `eqValues` caller immune to canonical pointer-equality regardless of direction |
| [V5-e6-fixture-recheck.md](V5-e6-fixture-recheck.md) | Verify E6's per-fixture compatibility classifications | All classifications hold; aggregate corrected from "16/22" to "27/28 with (subdir, suffix)". `CaDerivationAdvancedAttrsTest` is the one true structural hazard; needs common abstract base because of `TYPED_TEST_SUITE` parameterisation |

## How a new agent should pick a report to read

By question type:
- *"What's the next refactor to land?"* → [`../STATUS.md`](../STATUS.md)
- *"Has this candidate been investigated?"* → check the candidate's
  `**Branch:**` and `**Validation:**` lines, then this index for
  any associated E/F/B/C/V report.
- *"Is this claim semantically grounded?"* → check the candidate
  body for "predicted" / "structural reasoning, unmeasured" / "verified
  in V-N" labels.
- *"What did the prior reviewer assume?"* → 01-05 cover the original
  passes; the V-pass downgrades any unsupported claim.

## Adding a new report

When you spawn an agent for a new investigation:
1. Pick a letter (E/F/B/C/V/N) per the conventions above.
2. Pick the next number in that series.
3. Reference [`AGENT-CHARTER.md`](AGENT-CHARTER.md) in the spawn prompt.
4. Have the agent stream to `review/<letter><n>-<topic>.md`.
5. Add a row to this index when the report lands.
6. Update [`../STATUS.md`](../STATUS.md) if the report changes the
   pending/queued/blocked state.
