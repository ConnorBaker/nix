# Review discipline

These rules govern adversarial review passes on [README.md](./README.md) (the survey) and [PROPOSAL.md](./PROPOSAL.md). **Read this file before editing either document.**

Both documents were originally accepted as "thoroughly cited" after three rounds of adversarial review. A subsequent simulation pass against master found multiple structural defects in PROPOSAL.md that all three review rounds missed: behavioural claims layered on top of accurate citations without anyone tracing the data flow. The pattern was: cite-correctly, describe-behaviour-incorrectly. The reviews checked the citations and stopped.

The rules below are derived directly from those misses. They are mandatory for every future review pass on either file. **A review that does not exercise rules R1–R6 against load-bearing claims is not adversarial — it is a citation audit, and citation audits do not catch behaviour bugs.**

If you are reviewing either document, your report must include a top-level "Method" section stating which rules you exercised and on which claims. Reports that omit this section are rejected.

## Contents

- [R1 — Simulate, don't cite](#r1--simulate-dont-cite)
- [R2 — Two-process determinism check](#r2--two-process-determinism-check)
- [R3 — Adjacent-file consumer enumeration](#r3--adjacent-file-consumer-enumeration)
- [R4 — Blast-radius pass](#r4--blast-radius-pass)
- [R5 — Latency budget](#r5--latency-budget)
- [R6 — Out-of-frame alternatives](#r6--out-of-frame-alternatives)
- [R7 — Anti-anchoring](#r7--anti-anchoring)
- [R8 — No claim of "adversarial review" without R1–R6](#r8--no-claim-of-adversarial-review-without-r1r6)
- [R9 — Verify delegated review](#r9--verify-delegated-review)
- [Self-check before submitting any review](#self-check-before-submitting-any-review)

## R1 — Simulate, don't cite

For every "today X happens" or "after the change Y happens" claim, walk the actual code path that would execute *in this evaluation order, on this input*. Do not rest on a citation that the function exists. Five files maximum per walk; if the walk is hard, the walk *is* the deliverable — write it down in the report.

*Failure example.* PROPOSAL.md item j-pivot rested on the claim "today, addPath enters with `path.accessor = state.rootFS` (no fingerprint)." The first half is true; the second half required walking `fetchToStore → AllowList::getFingerprint → Caching::getFingerprint → Union::getFingerprint → Mounted::getFingerprint → GitSourceAccessor`, five files. No reviewer did the walk. The chain already returns the right fingerprint with the subpath stripped. The pivot duplicates work the existing stack performs.

## R2 — Two-process determinism check

Any claim that something is "content-determined", "stable across runs", or "cache-hit-able on second eval" must be backed by mentally executing the algorithm twice — once on `EvalState` instance A, once on instance B — and showing that the bytes the hash sees are identical. If the algorithm consumes a heap pointer (`Env*`, `ExprLambda*`, any C++ object address), it is not content-determined across runs. Period.

*Failure example.* A past version of PROPOSAL.md hashed captured nested lambdas by `(ExprLambda*, Env*)` pointer-pair identity. True within one process, fresh allocations across runs. The headline workload `lib.cleanSourceWith` constructs exactly such a wrapper. The "second cold eval cache-hits on `lib.cleanSource`" claim was unsupported by the algorithm as written. No reviewer ran the determinism check.

## R3 — Adjacent-file consumer enumeration

For every new on-disk format, encoding scheme, or new variant of an existing data type, enumerate every existing consumer in the codebase and verify compatibility. A new `NixStringContextElem` variant has consumers in parser/printer/visitor sites *and* on-disk persistence formats *and* error-message paths. Listing the parser/printer is not enough.

*Failure example.* A past version of PROPOSAL.md proposed a new `OpaqueWithSubpath` context element with body `?<storePath>!<canonSubpath>`. Reviewers checked the parser. None checked `eval-cache.cc::AttrDb::setString`, which space-separates serialised context elements. `CanonPath` permits spaces (only `\0` is forbidden). Any subpath containing a space corrupts the eval cache. Bumping the schema version does not fix it; a new delimiter is required, not addressed in the proposal.

## R4 — Blast-radius pass

For any change that affects content-addressing (anything that feeds into a hash, a derivation, or a store path), name every caller class and estimate the rebuild scope. "Reproducible mass rebuild" is *not* a free pass — it is an operational event that has to be sized.

*Failure example.* A past version of PROPOSAL.md proposed a "narrow-and-rewrite" plan that changes the text-hash of `prim_toFile` outputs that embed `${input}/sub`. Reviewers nodded at "one-time rebuild." `toFile` is the foundation of `writeScript`, `writeText`, `substituteAll`, `pkgs.writers`, and dozens more in nixpkgs. The rebuild scope is at the level of a staging-cycle event, not "one-time." No reviewer enumerated the callers.

## R5 — Latency budget

For any new I/O code path, write down the round-trip count and latency floor on a real workload (cold nixpkgs eval, CI matrix evaluation, monorepo first-touch). If the answer is "many × hours at typical RTT", that is a *prerequisite blocker*, not future work. Future-work classification requires the v1 latency floor to be acceptable on the workload the change is sold against.

*Failure example.* A past version of PROPOSAL.md classified per-OID coalescing in `PromisorBackend` as future work. `git_odb_backend::read` is synchronous; coalescing from inside `read` is structurally impossible. v1 floor is one HTTPS round-trip per missing blob — tens of thousands of RTTs on a cold nixpkgs eval, hours of wall-clock at 100 ms RTT. v1 would be slower than today's full clone. Coalescing is a prerequisite, not a future enhancement. No reviewer wrote the budget down.

## R6 — Out-of-frame alternatives

For every "we own this code indefinitely" commitment, enumerate alternatives across all languages and ecosystems, not just within the proposal's chosen frame. Document the rejection of each alternative with a specific reason. Silence on an obvious alternative is not a rejection; it is a gap.

*Failure example.* A past version of PROPOSAL.md committed Nix to maintaining a from-scratch protocol-v2 client indefinitely. `gitoxide` (Rust) ships v2 + `filter blob:none` today. The proposal did not mention it. The reviewers did not ask. The decision may still be from-scratch — the Rust toolchain dependency is real — but the comparison must be on the page.

## R7 — Anti-anchoring

Do not open a review with a quality verdict ("strongest piece of design writing", "well-reasoned", "thorough"). Quality verdicts before simulation are unfounded; once written, they bias the reviewer toward refining rather than refuting. State the verdict last, after R1–R6 have run.

*Failure example.* The first round-1 review of PROPOSAL.md opened with "the strongest piece of design writing I've seen produced for this codebase." Every subsequent pass anchored on that frame and looked for refinements. The structural defects in items j, k, h, and the early §5 went unfound until a fourth pass that explicitly ran R1.

## R8 — No claim of "adversarial review" without R1–R6

A review that confines itself to citation accuracy is a citation audit. Citation audits are useful and have their own value, but calling one "adversarial" sets the wrong expectation for the next reader, who will then assume the document has been simulated. Either run R1–R6 or label the report "citation audit, behaviour unverified."

## R9 — Verify delegated review

If you delegate review to subagents (parallel, sequential, or otherwise), you do not get to claim adversarial review based on their reports. The delegating reviewer must independently verify each load-bearing claim in the subagent's report against master, not merely read the report. Subagents reliably regress to citation-style verification regardless of how explicitly R1–R6 are mandated in their prompt. Treat their reports as *hypotheses* to test, not as conclusions to accept.

*Failure example.* Round 6 dispatched four subagents with explicit R1–R6 mandates and concrete code-paths to trace. Three of the four returned competent reports; verification of their reports against master found: (a) one agent claimed `Derivation::unparse` admits out-of-band annotations that survive `nix copy` — refuted on R3 verification (`unparse` is a fixed ATerm shape with no slots); (b) one agent claimed v2 protocol has a `no-thin` argument — wrong, the cure is to *omit* `thin-pack` not to send `no-thin`; (c) one agent's recommended source-region algorithm has the same `with`-shadowing soundness issue as competing algorithms, and the agent did not flag this without prompting. None of these defects would have been caught by trusting the agents' reports. Each was caught by the delegating reviewer reading master directly after the agent reported.

The corollary: when authoring a prompt for a delegated review, *expect* that the subagent will return findings that need adversarial verification. Budget time for the verification pass at roughly 1× the prompt-design time.

## Self-check before submitting any review

For each load-bearing claim in your report, answer:

1. Did I walk the call graph end-to-end (R1)?
2. Did I run the algorithm twice on different process instances (R2)?
3. Did I enumerate consumers of any new format (R3)?
4. Did I size the rebuild scope of any content-hash change (R4)?
5. Did I write down the latency floor of any new I/O path (R5)?
6. Did I enumerate cross-ecosystem alternatives (R6)?
7. Did I refrain from anchoring with a quality verdict before simulation (R7)?
8. If I am calling this report adversarial, did I exercise R1–R6 (R8)?
9. If I delegated any part of this review, did I verify the subagent's load-bearing claims against master myself (R9)?

If any answer is "no" and the rule applies, label the affected claim "unsupported" or fix the report. We will not have this discussion again.
