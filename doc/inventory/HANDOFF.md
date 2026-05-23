# Handoff guide for a fresh agent

This file is the single entry point for an agent (human or AI) picking
up the catalog work without prior context. Read this first; it points
at everything else and captures the operational lessons that don't
live in the in-tree docs themselves.

## Step 1 — read in order

1. [`INVENTORY.md`](INVENTORY.md) — codebase navigation map across the
   19 verified shards. Tells you what lives where.
2. [`candidates/README.md`](candidates/README.md) — catalog scope, the
   eight-debt-shape framing, the evidentiary standard, the verdict
   legend, and the section index (which candidate numbers live in
   which `candidates/NN-…md` file).
3. [`STATUS.md`](STATUS.md) — running ledger. The "Cleanup branches
   (live)" table is the source of truth for which shard branches
   exist, what they address, and whether they're pushed. The "Queued
   / blocked" and "Recently completed" sections track work-in-flight.
4. [`review/00-INDEX.md`](review/00-INDEX.md) — index of every review
   report under `review/`. Use this to find prior investigation of a
   candidate before doing your own.
5. [`review/AGENT-CHARTER.md`](review/AGENT-CHARTER.md) — the
   eight-rule evidentiary standard. Every claim you make in a report
   should address each rule where it applies.

## Step 2 — pick a queued item

The "Queued / blocked" section of `STATUS.md` lists candidates that
have been investigated but not yet implemented. Common shapes:

- An implementation request with prerequisites already met (e.g. `E1`
  for `impureOutputHash` deletion).
- A structural decision that needs source-level investigation before
  any code lands (e.g. `U10` for `boost::describe`).
- A verification task spawned by a prior review (`E*`/`F*`/`B*`/`V*`
  numbers).

Pick one whose prerequisites are met and whose effort fits the time
you have.

## Step 3 — operational rules you need before touching code

These are not in any in-tree doc; they were learned by doing. Read
them carefully — getting any of them wrong has historically masked
real bugs as "build green."

### Cleanup branches and worktrees

Each shard has a long-lived branch `vibe-coding/cleanup/<shard>` and
a corresponding worktree at
`/Users/cbaker2/ext-sources/nix-worktrees/cleanup-<shard>/`. Active
shards (as of last update): `libexpr`, `libfetchers`, `libflake`,
`libmain`, `libstore`, `libutil`, `nix-cli`. New candidates that
target an existing shard land as new commits on that branch (one
commit per candidate; do **not** bundle unrelated fixes into the same
commit). New candidates that target a shard with no branch yet need a
new branch + worktree off `master`.

### Build invocation

From the repo root (or any working directory), invoke ninja against
the **worktree's** build dir, with a Nix dev shell that has the
toolchain:

```
nix develop /Users/cbaker2/ext-sources/nix-worktrees/cleanup-<shard> \
  --command ninja -C /Users/cbaker2/ext-sources/nix-worktrees/cleanup-<shard>/build
```

Capture full output to `/tmp/<something>.log`; do NOT pipe through
`grep`, `head`, or `tail` inline (truncated build output has masked
errors before).

### The worktree-source-drift gotcha (load-bearing)

`meson setup --wipe build` resolves the source directory from the
shell's working directory. If you run `meson setup --wipe build` from
the master worktree (or with `pwd` on master because shells reset
between commands), meson configures the build dir to track the
**master** source tree, not the cleanup worktree. Subsequent `ninja`
invocations then compile master, not your branch. Builds report green
while your branch's modifications are unbuilt.

This bug masked two real build-breaks earlier and reported "ninja: no
work to do" while master was being silently compiled. The protocol
fix:

```
nix develop <worktree> --command meson setup --wipe <worktree>/build <worktree>
```

Pass the source dir **explicitly**. Verify after reconfigure:

```
python3 -c "import json; m = json.load(open('<worktree>/build/meson-info/meson-info.json')); print(m['directories']['source'])"
```

The output must be the worktree path. If it's `/Users/cbaker2/ext-sources/nix`,
your build dir is misconfigured.

### Verification protocol per change

For every code change on a cleanup branch:

1. Edit the source.
2. `touch` the file you edited (forces ninja to invalidate its cache;
   without this, ninja can decide a header edit is a no-op).
3. Build via the invocation above. Capture full log.
4. Confirm exit 0 AND grep the log for `error:` and `^FAILED`. Both
   counts must be zero in your changed files.
5. If your change touches a public/installed header (`meson.build`'s
   `install_headers` set), additionally:
   - Walk every consumer tree-wide via `grep -rn`.
   - Note that external consumers (Hydra, Lix, plugins, Rust bindings)
     also see these headers; deletions or signature changes are
     public-API breaks even if zero in-tree call sites exist.

### clang-tidy invocation

```
nix develop <worktree> --command ninja -C <worktree>/build clang-tidy
```

Five errors are pre-existing on master and not caused by anything you
do — filter them out:

- 4× `rapidcheck/Gen.hpp:72` — `current_exception` incomplete-type in
  the test framework.
- 1× `libcmd/network-proxy.cc:34` — missing `<iterator>` include for
  `std::inserter`.

Any other error in your changed files needs to be fixed before
landing the commit.

### Commit message convention

- Subject ≤ 70 chars: `<shard>/<area>: <action>` (e.g.
  `libstore/profiles: extract withLockedProfileGenerations prelude`).
- Body explains the *why*, not the *what*. The diff shows what; the
  message captures rationale, invariants preserved, caveats.
- Reference the catalog: `Refs candidate #NNN.` or `Catalog candidate
  #NNN.`.
- Co-author line: `Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>`.
- No emojis. No line numbers in comments or commit bodies (cite
  identifiers instead). No `--no-verify` / `--no-gpg-sign` unless the
  user explicitly approves.

### Catalog discipline

Every commit on a cleanup branch must:

1. Reference its candidate number(s) in the commit body.
2. Have a `**Branch:**` line on the candidate(s) in
   `candidates/NN-…md` pointing at the right shard.
3. The `**Branch:**` line should describe what shipped, particularly
   if the implementation deviates from the candidate body's
   prescription. Use parentheticals like
   `(extracted only the prelude; per-iteration logic stays in
   each caller's lambda because …)`.

When a follow-up commit extends a previously-landed candidate (e.g.,
amending an earlier commit's design after review), add the follow-up
note to the existing `**Branch:**` line rather than creating a new
candidate. Established patterns:

- `#6 (hoist-then-delete)` — the hoist landed, then a follow-up
  deleted the macros entirely after finding zero call sites.
- `#110 (extended to forceValueDeep)` — the helper landed across five
  sites, then a follow-up extended it to a sixth.
- `#4 (extended to template form)` — the macro consolidation landed,
  then a follow-up replaced the macros with a C++ template.

When a follow-up uncovers a new latent bug or design issue not in
scope of the original candidate, file it as a **new** candidate (e.g.
`#219` for the post-build-hook timeout silent-drop, surfaced during
post-cleanup adversarial review of `#157`).

### STATUS.md "Cleanup branches (live)" table

Update the `Addresses` column when a new candidate lands on a shard
branch. Update the `Status` column when a branch is pushed past its
prior tip ("Pushed" → "Local-only past `<sha>`" or vice versa).

### Push policy

**Explicit-only.** Never push to origin without the user's explicit
instruction for that specific push. "Push" approval is per-commit,
not per-session — even after the user approves one push, do not
push subsequent commits without a fresh approval.

### Use of agents

Spawning a sub-agent for investigation is encouraged when the
question is open-ended ("what would break if we …", "is there an
existing utility for …", "walk every call site of X"). Always:

1. Pin the worktree explicitly in the prompt: `read source from
   /Users/cbaker2/ext-sources/nix-worktrees/cleanup-<shard>/, never
   /Users/cbaker2/ext-sources/nix/`. Source-tree drift between agents
   has been a recurring failure mode.
2. Hand the agent the AGENT-CHARTER.md path so its claims are
   evidence-based.
3. Ask for a punch list at the end with severity tags
   (blocker / minor / cleanup).

## Step 4 — verify before declaring done

The session that produced the current state of these branches caught
real bugs in three places where "build green" was reported but the
build was actually compiling master:

- `libstore #119` — `BaseSetting<T>::overrideIfSet(T &)` signature
  rejected `std::optional<T>&` destinations. Caught only when the
  build was reconfigured against the worktree source and the four
  call sites in `HttpBinaryCacheStore::makeRequest` were re-compiled.
- `nix-cli #24` — `flake.cc` was missing `#include "command-register.hh"`
  after the `NIX_REGISTER_COMMAND` macro migration. Caught only when
  the build was reconfigured against the worktree source.
- `libstore #115` — clang-tidy raised
  `cppcoreguidelines-missing-std-forward` after the function was
  renamed and `std::forward<Body>(body)(...)` was lost in the rename.

The verification protocol above (step 3, "Verification protocol per
change") catches all three. Do not skip it.

For changes that touch wire formats, public headers, or runtime
ordering of events, additionally:

- **Wire-format changes** (anything under `length-prefixed-protocol-helper.hh`,
  `*-protocol.hh`, `*-protocol-impl.hh`): walk byte-by-byte against
  `git show <pre-batch-sha>:<file>`. Token-level diff. Confirm
  evaluation order (`{a, b, c}` brace-init pack expansion guarantees
  left-to-right; some other forms don't).
- **Public-header changes**: confirm the header is in
  `install_headers` via `meson.build`. If yes, deletions/signature
  changes are external-API breaks — document in commit body even if
  zero in-tree call sites exist.
- **Runtime ordering changes** (e.g., `Goal::ChildEvents`): walk
  every producer and every consumer tree-wide. The reviewer for
  `#159` caught a wrong-too-strict assertion this way.

## Step 5 — when finished

- Update `STATUS.md`'s "Recently completed" entry for the work you
  landed (most-recent first; truncate after a dozen entries).
- If the candidate's `**Branch:**` line needed amending to describe
  what actually shipped, do that on a `vibe-coding/simplifications`
  commit (or whichever doc branch is current).
- Mention any deferred decisions, follow-up candidates, or punch-list
  items in the commit body and/or as new catalog candidates.
- Do **not** push to origin without an explicit instruction.

## Common pitfalls (one-line digests)

- Build dir misconfigured against master source → ninja silently
  compiles master while reporting green for your branch. Reconfigure
  with explicit source dir.
- Editing a header doesn't trigger ninja rebuild → `touch` the
  consumer .cc files or the file you edited.
- Test pass on libexpr-tests but not on libstore/libutil → only
  libexpr was unit-tested in the recent batch; functional tests
  weren't run.
- "ninja: no work to do" after a meson reconfigure → the reconfigure
  may have silently picked up the wrong source dir (see above).
- A `**Branch:**` line says one thing and the commit shipped another
  → either the implementation deviated and the line needs updating,
  or the implementation was wrong; review the validation paragraph
  to decide.
- A commit lacks a candidate reference in its body → either it's
  catalog cleanup (acceptable, document in commit body), or it's a
  follow-up to a landed candidate (in which case extend that
  candidate's `**Branch:**` line; don't leave it dangling).
