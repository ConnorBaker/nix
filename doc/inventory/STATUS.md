# Inventory work — running status

This file is the project ledger. It records what's queued, in flight,
blocked, and pending across the catalog work. Update it whenever a
phase advances, a verification lands, or a follow-up is queued.

The other entry points:
- [`INVENTORY.md`](INVENTORY.md) — codebase navigation map.
- [`candidates/README.md`](candidates/README.md) — catalog scope,
  eight-debt-shape framing, evidentiary standard.
- [`review/00-INDEX.md`](review/00-INDEX.md) — index of every review
  report.
- [`review/AGENT-CHARTER.md`](review/AGENT-CHARTER.md) — operational
  rules for any agent working on the catalog.

## Currently in flight

(none — integration is the next step.)

## Queued / blocked

- **Integration into the catalog** — the next step. Folds the 19
  review reports (E1-E8, F1-F3, B1-B3, C1, V1, V2, V3, V4, V5) into
  the candidate sections. Specifically:
  - Update #138's body with V1's "5 of 6 C-API libraries install
    `*_internal.h(h)` files" finding; flag the layout-change ABI
    impact in release notes.
  - Update #128's commit-bundling caveat per pass-2's note (the
    `blockInt` deletion was bundled with the `getIntArg` change on
    `cleanup/libmain-shared`).
  - Apply B1's correction to the `15-globals-settings.md` keystone
    block: drop `#135` from the #136-keystone list.
  - Apply B2's count corrections: #65's "~hundreds" is 79 canonical
    sites + ~135 bespoke; #211's "10 force* overloads" is 7
    structurally-shared bodies.
  - Apply B3's corrections: #18 hand-written `CloneableError`
    derivations is 22 (not 14); 18a is not a real refactor target;
    surface the `SQLiteBusy → SQLiteError → MakeError` cross-link.
  - Apply C1's new candidate proposal: `Hash::dummy` /
    `StorePath::dummy` placeholder-then-overwrite pattern across 5
    sites — file as a small candidate.
  - Apply E1's deletion path for `impureOutputHash` (recommended:
    drop as a standalone PR; no deprecation cycle; no downstream
    coordination needed). Land on a new
    `vibe-coding/cleanup/libstore-impure-hash` branch following the
    cleanup-branch convention.
  - Apply V2's count corrections to E4's catalog edits: positional
    `%N%` set is 71 in libstore (not 61), 50 in libutil; net
    wire-crossing positional ~95 (not 61). E4 missed five
    categories of sites (`store-api.hh` inline throws,
    `string2IntMustParse`, `TimedOut` constructor body, `%s`/`%d`
    non-positional, post-build `BuilderFailureError`). Phase 2 of
    #125 must batch positional and non-positional together for
    contract-bearing sites; the text-grep contract spans both forms.
  - Apply E5's verifiable-counts manifest as `doc/inventory/counts.toml`
    and the `verify-catalog-counts.sh` driver. Wire to weekly cron
    + manual pre-PR run.
  - Apply E6's "27/28 with (subdir, suffix)" correction to N41's
    body (V5 verified the count).
  - Apply E7's "ship `serialise-fwd.hh`, `store-fwd.hh`, AND
    `eval-fwd.hh` together" recommendation (the original #166
    omitted `eval-fwd.hh`).
  - Apply F1+V3's confirmed UAF in `nix_eval_state_builder` to
    catalog: extend #138's body, propose a fix that folds the UAF
    repair into #138's `readOnlyMode → Store` migration.
  - Apply F2's findings: the `"is not valid"` contract is mostly
    dead code today; flag the surviving `BadStorePathName` case as a
    pre-existing bug and the `binary-cache-store.cc` finding as
    not-bug.
  - Apply E8+V4's downgraded recommendation: keep Direction A on the
    four non-perf grounds; drop the unsupported "1-2 ns" framing.
  - Update STATUS.md and `review/00-INDEX.md` after integration to
    reflect the new state.
- **#138 implementation** — gated on V1's release-note coordination.
- **Cluster-G uplift series U10 (`boost::describe`)** — flagged as the
  highest-leverage middle-of-graph step in `candidates/README.md`'s
  uplift series. Not yet scheduled.
- **E1 implementation** — `impureOutputHash` deletion. Ready as soon
  as integration lands (E1 cleared the prerequisites: confirmed dead
  in-tree, internal linkage, no downstream consumers, lix already
  deleted it).

## Recently completed

(Most recent first; truncate after a dozen entries.)

- **Evidentiary standard added** (`7cb0eca34`) — eight rules in
  `candidates/README.md` plus operational form in
  `review/AGENT-CHARTER.md`. Derived from failure modes seen across
  the review passes.
- **18 review reports landed in-tree** (`7cb0eca34`) — E1-E8,
  F1-F3, B1-B3, C1, V1, V3, V4, V5.
- **Four-pass review consolidation** (`f3bc2c222`) — folded the
  ~5,300-line review output into the catalog. 89 findings applied,
  13 deferred (the E/F/B/C/V queue), 4 rejected.
- **Two adversarial passes + two pattern-discovery passes** —
  reports under `review/01-…md` through `review/05-…md`.
- **Catalog scope rewrite** — perf and modernisation are in scope on
  their own merits.
- **Catalog split into 25 sections** — original monolithic
  `CANDIDATES.md` deleted; 217 + 44 candidates organised by topic.
- **Seven cleanup PRs pushed** — `vibe-coding/cleanup/*` branches on
  origin, ready for upstream review.

## Cleanup branches (live)

These are pushed to `origin` (`ConnorBaker/nix`) and ready for PR
creation against upstream. Each addresses specific candidates per the
`**Branch:**` lines in the candidate sections.

| Branch | Addresses | Status |
| ------ | --------- | ------ |
| `vibe-coding/cleanup/libexpr-friend-dup` | #217 | Pushed |
| `vibe-coding/cleanup/libfetchers-curl-stub` | #52 | Pushed |
| `vibe-coding/cleanup/libflake-lockfile` | #48 | Pushed |
| `vibe-coding/cleanup/libmain-shared` | #49, #128 | Pushed |
| `vibe-coding/cleanup/libstore-dead-decls` | #54, #56, #57, #60 | Pushed |
| `vibe-coding/cleanup/libutil-misc` | #58, #130 | Pushed |
| `vibe-coding/cleanup/nix-run` | #127, #142 | Pushed |

The branches were pushed to a fork; PRs against upstream NixOS/nix
have not been opened. The user controls when to open them.

## Worktree layout

The seven cleanup branches are checked out as worktrees under
`/Users/cbaker2/ext-sources/nix-worktrees/`. Future agents should use
`git worktree list` to see what's checked out. New PR work for any
candidate should follow the same pattern (one worktree per branch off
master, descriptive name, `**Branch:**` line in the candidate body).

## How to pick up this work

Read in order:

1. `INVENTORY.md` — what the codebase looks like.
2. `candidates/README.md` — what the catalog tracks and how.
3. `STATUS.md` (this file) — what's pending.
4. `review/00-INDEX.md` — what investigations have been done.
5. `review/AGENT-CHARTER.md` — how to do investigations correctly.

Then pick a queued item from the "Queued / blocked" list above (or
propose a new one). Spawn an agent for it with `AGENT-CHARTER.md` as
"Read first". Update this file when the work lands or changes state.

## Conventions

- Reports go under `doc/inventory/review/`. Naming: `<E|F|B|C|V|N><N>-<topic>.md`.
  - **E** = eight follow-up agents queued from the consolidation
    pass.
  - **F** = "follow-up" findings flagged during E1-E8 that needed
    their own investigation.
  - **B** = "category B" structural decisions deferred from the
    consolidation pass.
  - **C** = "category C" marginal observations deferred from the
    consolidation pass.
  - **V** = "verification" pass — pure source-evidence verification
    of weak/mixed-evidence claims from the prior layers.
  - **N** = new candidate (the cross-shard pattern-discovery passes
    used N1-N44).
- Cleanup branches: `vibe-coding/cleanup/<name>`. One worktree per
  branch off master.
- Candidate cross-references: bare `#NN` for 1-217, `NN` (no hash)
  for cross-shard `N1-N44`, `EN` / `FN` / `BN` / `CN` / `VN` for
  reports.
