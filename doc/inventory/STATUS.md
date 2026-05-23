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

(none.)

## Queued / blocked

- **#138 implementation** — relocate `EvalSettings::readOnlyMode` to
  `Store`/`StoreConfig`. V1's release-note caveat documented in #138's
  body: deleting the dead `nix::ref<bool> readOnlyMode` field in
  `nix_eval_state_builder` is a layout change to the publicly-installed
  `nix_api_expr_internal.h`, must be called out in release notes. The
  fix folds in the F1+V3 confirmed UAF repair (the dangling pointer in
  `EvalSettings::readOnlyMode` after `nix_eval_state_builder_free` goes
  away when the field is removed entirely).
- **#218 implementation** — `Hash::dummy`/`StorePath::dummy` placeholder
  pattern (new candidate added during integration). Two halves:
  placeholder-then-overwrite sites (6, in `local-store.cc`,
  `derivation-trampoline-goal.cc`, `make-content-addressed.cc`,
  `unix/build/derivation-builder.cc`, two FIXME-tagged sites in
  `nar-info.cc`) → migrate to `std::optional<Hash>` /
  `std::optional<StorePath>`; wire-marker sites (3, in `worker-protocol.cc`,
  `serve-protocol.cc`, `legacy-ssh-store.cc`) → rename sentinel and
  coordinate the protocol header. Aggregate effort medium because the
  wire-marker half touches a wire-format-adjacent header.
- **E1 implementation** — `impureOutputHash` deletion. Lands as an
  additional commit on the long-lived `vibe-coding/cleanup/libstore`
  branch. E1 cleared the prerequisites: internal linkage (verified via `nm -m`
  on the built dylib); zero in-tree readers; zero references in lix
  or Hydra; commit `50912d02e` (2022-03-31) intended full removal but
  missed the namespace-scope definition in `derivations.cc`. Pure
  deletion as a standalone PR — no deprecation cycle, no downstream
  coordination.
- **E5 verifiable-counts manifest** — implement `doc/inventory/counts.toml`
  + `verify-catalog-counts.sh` per E5's specification. Wire to weekly
  cron + manual pre-PR run. Open questions in E5 (manifest location,
  new-claim onboarding policy, file-rename handling) need a decision
  before scripting begins.
- **Cluster-G uplift series U10 (`boost::describe`)** — flagged as the
  highest-leverage middle-of-graph step in `candidates/README.md`'s
  uplift series. Not yet scheduled. Unblocks N16, N32, N35, N38, N39,
  plus the existing #9, #176, #182.
- **E3 implementation** — write the `Goal::Co` property test pinning
  O(1) frame depth, before any partial #160 (`boost::asio::awaitable`)
  migration. Specification ready in `review/E3-goal-co-property-test.md`.

## Recently completed

(Most recent first; truncate after a dozen entries.)

- **30 trivial candidates landed across 4 shard branches** (May 2026)
  — libexpr +11 (#64, #66, #68, #106, #107, #109, #110, #112, #196,
  #199, plus a forceValueDeep-to-SeenSet follow-up), libstore +14
  (#4, #5, #6 hoist-then-delete, #14, #47, #82, #114, #115, #119,
  #157, #159, #165, plus a macro→template follow-up and a
  GET_PROTOCOL_*-deletion follow-up), libutil +3 (#29, #99, #117),
  nix-cli +5 (#24, #77, #89, #91, #187). Two skipped on rule
  grounds: #194 (search-path.hh ships via install_headers, not
  internal as the candidate framing claimed) and #197 (lookupPath
  is private). Each shard went through 4 adversarial review agents
  + 4 `/code-review` skill passes (12 agent runs total); 20
  nits/deferrals were captured and addressed in a final polish
  pass. New candidate #219 (post-build-hook timeout silently
  dropped) added to the catalog from post-cleanup review.
- **Integration of 19 review reports** (HEAD) — folded findings from
  E1-E8, F1-F3, B1-B3, C1, V1-V5 into the candidate bodies. Notable
  edits: #134 expanded with C-API caveat per V1; #135 corrected per
  B1 (does NOT depend on #136); #138 expanded with F1+V3 UAF repair
  + V1 release-note caveat; #65 corrected per B2 (79 canonical sites
  + ~135 bespoke); #211 corrected per B2 (7 structurally-shared
  bodies, not 10); #18 corrected per B3 (17 hand-written derivations,
  not 14; 18a is not a refactor target); #213 expanded with E8+V4
  Direction A recommendation; #166 expanded with E7's three-header
  recommendation; #125 expanded with E4+V2+F2's wire-crossing
  reachability + two text-grep contracts + phasing constraint;
  #128 noted commit-bundling per CLAUDE.md; #140 expanded with E1
  verification details. New candidate #218 added (`Hash::dummy`/
  `StorePath::dummy` placeholder pattern). N41 corrected per E6+V5
  (28 fixtures, 19 fit cleanly with subdir-only ctor, 27/28 with
  `(subdir, suffix)` ctor pair, 1 structural hazard).
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
  (Now 219 + 44 after #218 added during integration and #219 added
  during post-cleanup adversarial review.)
- **Seven cleanup PRs pushed** — `vibe-coding/cleanup/*` shard branches
  on origin, ready for upstream review.

## Cleanup branches (live)

These are pushed to `origin` (`ConnorBaker/nix`) and ready for PR
creation against upstream. Each shard branch is **long-lived**: as
more candidates land in the same shard, they are appended as
additional commits on the same branch (one PR per shard, multi-commit
review). Each candidate addressed by a branch carries a `**Branch:**`
line in its body.

| Branch | Shard | Addresses | Status |
| ------ | ----- | --------- | ------ |
| `vibe-coding/cleanup/libexpr` | libexpr | #217, #64, #66, #68, #106, #107, #109, #110 (extended to forceValueDeep), #112, #196, #199 | Local-only past `437eea9d0` |
| `vibe-coding/cleanup/libfetchers` | libfetchers | #52 | Pushed |
| `vibe-coding/cleanup/libflake` | libflake | #48 | Pushed |
| `vibe-coding/cleanup/libmain` | libmain | #49, #128 | Pushed |
| `vibe-coding/cleanup/libstore` | libstore | #54, #56, #57, #60, #4, #5, #6, #14, #47, #82, #114, #115, #119, #157, #159, #165 | Local-only past `5f5b8151e` |
| `vibe-coding/cleanup/libutil` | libutil | #58, #130, #29, #99, #117 | Local-only past `784a4f4b4` |
| `vibe-coding/cleanup/nix-cli` | `src/nix/` (modern + legacy CLI) | #127, #142, #24, #77, #89, #91, #187 | Local-only past `48524e038` |

The branches whose Status reads "Pushed" are on `origin`
(`ConnorBaker/nix`) and ready for upstream PR creation. The four
Status: Local-only branches each have additional commits beyond
the pushed tip and have not been pushed yet — push policy is
explicit-only.

## Worktree layout

The seven shard branches are checked out as worktrees under
`/Users/cbaker2/ext-sources/nix-worktrees/cleanup-<shard>/`. Future
agents should use `git worktree list` to see what's checked out. New
work targeting an existing shard should reuse that shard's worktree
and append a commit; new shards get a new branch and a new worktree.

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
- Cleanup branches: `vibe-coding/cleanup/<shard>` — one branch per
  shard, long-lived, accumulating commits as more candidates land in
  that shard. One worktree per branch off master, named
  `nix-worktrees/cleanup-<shard>/`.
- Candidate cross-references: bare `#NN` for 1-217, `NN` (no hash)
  for cross-shard `N1-N44`, `EN` / `FN` / `BN` / `CN` / `VN` for
  reports.
