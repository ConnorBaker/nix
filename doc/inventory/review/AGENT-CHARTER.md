# Agent charter — Nix C++ catalog work

This file is the standing charter for any agent spawned to work on the
catalog under `doc/inventory/`. Every prompt that asks an agent to
verify, discover, classify, or recommend should reference this file
("Read first: doc/inventory/review/AGENT-CHARTER.md") and require the
agent to address the rules below.

The rules are derived from failure modes observed in prior passes and
are documented in [`../candidates/README.md`](../candidates/README.md)
(the "Evidentiary standard" section). This file is the operational
form for agents.

## Standing rules

For every claim you make in a report, address each rule below where it
applies. If a rule does not apply, say so. If you cannot satisfy a
rule with semantic evidence, mark the claim "unsupported" and explain
the gap.

### Rule 1 — Walk the call graph

Behaviour claims must trace at least one level of caller. Specifically:

- Do not reason about a function in isolation. Identify at least one
  caller and confirm the function is reachable along the path your
  claim assumes.
- Trace virtual dispatch. If the function is a virtual override, name
  every concrete subclass that ships and what each one does.
- For lifetime claims, trace destruction order. If the call site is
  reachable during static destruction (via `~unique_ptr<T>` chains
  from namespace-scope statics), the lifetime contract is *not* the
  same as during normal execution.
- Reference example: `windowSize()` un-leak (#148, INVALID) — the
  function-local-static accessor was correct in isolation but wrong
  under static destruction order, because `~ProgressBar` runs before
  `windowSize()`'s function-local static is destroyed. The hazard
  could only be seen by walking the destructor chain.

### Rule 2 — Trace what ships, not what is in `src/`

ABI claims must check shipping artefacts:

- `meson.build` `install_headers(...)` rules determine the public
  header set.
- Library symbol visibility (verify with `nm`, not source text alone).
- The install set sometimes includes test-support libraries that are
  themselves consumed by external tooling.
- Reference example: E2 found no `nix::settings.*` field reads in
  `src/*-c/` source. V1 found that 5 of 6 C-API libraries install
  their `*_internal.h(h)` files, exposing layout to C++ consumers
  through the public include path. Source-text inventory was correct;
  ABI conclusion was wrong.

### Rule 3 — Counts require closed enumeration

When citing a count:

- Record the exact command (`grep -rn "..."`, `find ... | wc -l`,
  etc.) that produces the number, and where it was run.
- Record what is excluded and why (test files, generated headers,
  comments, etc.).
- If methodology depends on a particular counting rule (lines vs
  declarations vs files), specify which.
- "Approximately N" is acceptable only when exact counting is
  documented as out of scope and the approximation is bounded.
- Reference example: #169 friend-declarations went 13 -> 17 -> 16
  across three passes. Each pass was correct given its own counting
  rule, but the rules differed and were not stated. V-passes have
  re-derived counts on every iteration.

### Rule 4 — Distinguish predicted from verified

A claim about how a refactor will turn out is a *prediction*. A claim
about how the current code behaves is *verifiable*. Mark these
distinctly:

- "Predicted: this fixture fits cleanly" — labelled as a prediction.
- "Verified: this fixture's `unitTestData` member is reassigned in
  `SetUp()`" — labelled as verified, with the identifier cited.
- Reports may contain predictions, but the reader must be able to
  tell which claims are which.
- Reference example: E6's "16/22 fixtures fit cleanly" was a
  prediction (about consolidation outcome); V5 verified each fixture
  against the source body and corrected to "27/28 with `(subdir,
  suffix)`".

### Rule 5 — Re-derive inherited claims

When relying on a prior pass's finding, verify it against current
source:

- Cite the prior report, then state your own verification.
- If the prior claim depends on a count, re-derive the count.
- If the prior claim depends on a code path, walk it.
- Citation alone is not evidence; you must produce independent
  evidence for any claim load-bearing to your verdict.
- Reference example: B1 caught that the consolidator preserved the
  "#136 keystone for #134/#135/#137/#144/#149" chain "because it's
  load-bearing", inherited from pass-2 without verification. B1
  found that #135 does not actually depend on #136 — the chain was
  wrong about that link.

### Rule 6 — "Same shape" is not "same problem"

Structural similarity is a hypothesis to test, not a conclusion:

- For every alleged shared-shape site, walk it individually.
- A function name match is not evidence of behavioural match. A
  similar-looking signature is not evidence of identical contract.
- A proposed consolidation that says "these N sites are all the
  same" requires that the reviewer walked all N.
- Reference example: B2 found `forceAttrs`/`forceList` (inline,
  using `withTrace` on the error builder) are a different shape
  from `forceInt`/`forceFloat`/etc. (out-of-line, using `addTrace`
  in a `try/catch`). The prior reviewer lumped them as one
  10-overload family; the actual structurally-shared subset is 7.

### Rule 7 — Numbers require measurement

Performance claims, frequency claims, and quantitative comparisons
must be measured or labelled "structural reasoning, unmeasured":

- Do not invent figures. "1-2 ns" without a benchmark is
  unsupported.
- "Hot path" is a measurable claim — either profile it or label
  the claim as structural-reasoning.
- "Negligible" / "small" / "large" perf claims must either be
  backed by measurement, or downgraded to "structural reasoning,
  perf unmeasured".
- Reference example: E8's "Direction A yields negligible
  performance benefit" was unsupported; V4 confirmed Direction A on
  four non-perf grounds and downgraded the perf framing.

### Rule 8 — Comments are hypotheses

Source comments are evidence about intent, not evidence about
behaviour. Verify against code:

- When source claims something via comment, walk the code paths
  that should respect the claim and confirm.
- Code that contradicts a comment is the truth. Flag the divergence
  in your report.
- Reference example: `Value::vTrue` carries the comment "This is
  _not_ a singleton. Pointer equality is _not_ sufficient." E8
  found that every actual consumer treats it as a singleton; the
  comment is a defensive intent, but the code commits to the
  singleton interpretation. The comment lost.

## Required report content

Every report must include, near the top:

- A `## Method` section: which files you read end-to-end, which
  greps you ran (with the exact command), which call-graph traces
  you walked, which install rules you checked.
- A statement of which rules above were exercised, with one-line
  evidence per rule.
- Per-claim verifiability: every claim in the report must be either
  (a) verified-from-source with cited identifier, (b) labelled
  prediction, or (c) labelled "structural reasoning, unmeasured" /
  "unsupported".

## Required output formatting

- No emojis.
- No line numbers in the report (cite identifiers).
- Source-tree paths are absolute or relative-to-repo-root, not
  relative-to-the-agent's-working-directory.
- When citing a count, include the command that derives it.

## Self-check before submitting

Before reporting your verdict, ask:

1. For each behaviour claim, did I walk the call graph (rule 1)?
2. For each ABI claim, did I check `install_headers` (rule 2)?
3. For each count, did I cite the closed enumeration (rule 3)?
4. Did I distinguish predicted from verified (rule 4)?
5. For inherited claims, did I re-derive (rule 5)?
6. For "same shape" claims, did I walk each site (rule 6)?
7. For numerical claims, did I measure or label unmeasured (rule 7)?
8. For source comments, did I verify against code (rule 8)?

If any answer is "no" and the rule applies, fix the report or label
the affected claim "unsupported".

## Coordinating with other agents

If your work overlaps with another agent's, do not duplicate their
work. If you find their finding wrong, say so explicitly with
evidence — these reviews are adversarial, not consensus-building.
The standing rule across passes: if you cannot disagree with a prior
finding when source warrants it, you have not done the job.

## Adding a new rule

If you find a failure mode that is not covered by these eight rules,
flag it in your report under "Recommended rule addition" with a
proposed phrasing and a reference example. The user will decide
whether to add it.
