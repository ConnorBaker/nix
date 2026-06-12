# Virtual Derivations: Deferred, Batched `.drv` Materialisation

This document describes the branch's gated implementation of **lazy and batched `.drv` materialisation**, by the same content-addressed placeholder pattern the source-materialisation architecture already applies to source trees ([PROPOSAL.md §0](./PROPOSAL.md#0-underlying-pattern)). The thesis in one line: **a derivation is the output of an evaluator-level "build" — the evaluator computes its identity (`drvPath`) eagerly from in-memory data, threads that identity through evaluation, and materialises the `.drv` file (and its as-yet-unmaterialised source inputs) only at a resolution boundary, using a store-kind-aware drain or a lazy store overlay.**

It is written in the timeless component/interplay style of [PROPOSAL.md](./PROPOSAL.md): what each piece does and how the pieces interplay, with `file:line` citations into the tree as a reading aid. Every "today X happens / after the change Y happens" claim is a *simulated* walk of the executing code path, per [REVIEW-DISCIPLINE.md](./REVIEW-DISCIPLINE.md) R1 — not a citation that a function exists.

> **Status: implemented on this branch behind `lazy-derivations` (default off), and adversarially reviewed under REVIEW-DISCIPLINE R1/R9.** The review reshaped the design materially; the historical record remains in the [Findings ledger](#findings-ledger). Current facts: write deferral is implemented as `AsyncPathWriter`; the `.drv` side is a path-keyed write queue, not a scheduler; `addMultipleToStore` is N per-path registrations, not one atomic transaction; substituted-target elision is real but scoped; and the earlier `queryMissing`-prepass plan has been superseded by `makeLazyDrvStore` for in-process builds and dry-run reports, plus a conservative full flush before handing a remote daemon a closure.

It builds on three precedents already in the tree or in the surveyed forks, and is explicit about which mechanism it borrows from each:

- the **source** `SourceVirtual` placeholder + `MaterialisationScheduler` + batched `outPathsOf` chokepoint ([PROPOSAL.md §2.O, §6.2](./PROPOSAL.md#o-sourceplaceholder--materialisationscheduler--content-keyed-memo)) — this design shares its *resolution-at-a-boundary* shape and reuses the source scheduler unchanged, but the `.drv` side is a write-queue, not a second scheduler ([§4](#4-the-design));
- upstream's **`DownstreamPlaceholder` + `Derivation::tryResolve`** (`downstream-placeholder.hh`, `derivations.cc:1151`) — the existing proof that a derivation's *output* path can be unknown at eval time and resolved at a boundary;
- DetSys's **`AsyncPathWriter`** (`nix-src/src/libstore/async-path-writer.{hh,cc}`) — the existing proof that the `.drv` *write* can be moved off the eval thread, **and** (in its disabled `#if 0` block) the first attempt at the remote-store bulk `addMultipleToStore` flush this design completes.

---

## Contents

- [Current branch status](#current-branch-status)
- [0. The pattern: a derivation is an evaluator-level build output](#0-the-pattern)
- [0.5. The algebra: `.drv` materialisation as a content-addressed selective task](#05-the-algebra)
- [1. What `derivationStrict` does today (simulated)](#1-what-derivationstrict-does-today)
- [2. The unblock primitive: return the attrset without materialising](#2-the-unblock-primitive)
  - [2.1. The three output-path kinds, and why none needs the `.drv` on disk](#21-the-three-output-path-kinds)
  - [2.2. What is intrinsic to `drvPath` vs what is a deferrable side-effect](#22-intrinsic-vs-deferrable)
- [3. DetSys `AsyncPathWriter`: what it does and where it stops](#3-detsys-asyncpathwriter)
- [4. The design](#4-the-design)
  - [4.1. The `.drv` write-queue (and its memoisation)](#41-the-drv-write-queue)
  - [4.2. Resolution boundaries (where `.drv` bytes must be readable)](#42-resolution-boundaries)
  - [4.3. The batched store flush + the narHash friction fix](#43-the-batched-store-flush)
- [5. Soundness obligations](#5-soundness-obligations)
- [6. Interaction with the lazy-source substrate](#6-interaction-with-the-lazy-source-substrate)
- [7. Performance characteristics and the workload-shape risk](#7-performance-characteristics)
- [8. Comparison: async vs batched vs lazy](#8-comparison)
- [9. Non-goals and boundaries](#9-non-goals-and-boundaries)
- [10. Open questions and review obligations](#10-open-questions-and-review-obligations)
- [11. Laws and test plan](#11-laws-and-test-plan)
- [12. Implementation status and maintenance checklist](#12-implementation-status-and-maintenance-checklist)
- [Findings ledger (adversarial review)](#findings-ledger)

---

## Current branch status

Compared with local `master` at `NixOS/nix@2d309b18e`, this is no longer only a proposal. The branch implements the core design behind the `lazy-derivations` setting:

- `AsyncPathWriter` lives in `src/libstore/async-path-writer.{hh,cc}` and queues content-addressed `.drv` bytes by `StorePath`. It supports `waitForPath`, `waitForPaths`, `waitForAllPaths`, local background drain, deferred sources via `addSource`, and `makeLazyDrvStore` for reading pending `.drv`s without forcing a write.
- `EvalState` owns the writer, and `derivationStrict` computes `drvPath` eagerly while enqueuing the `.drv` and source copies only when `lazy-derivations = true` and evaluation is not read-only.
- Observation boundaries are implemented in the build path, dry-run reporting, `nix eval`, `nix-instantiate`, `realiseContext`/IFD, eval-cache force paths, and `.drv` read-back paths.
- In-process builds use `makeLazyDrvStore` so the Worker can inspect pending `.drv`s without materialising them; remote-daemon builds flush the queue first because the daemon cannot see the client's pending state.
- Tests exist under `src/libstore-tests/lazy-derivation-*.cc`, `src/libexpr-tests/lazy-derivation-defer.cc`, and `tests/functional/lazy-derivations/`.

Remaining gates are performance and default-policy gates: the setting stays default-off until the workload-specific benchmarks justify changing that.

---

## 0. The pattern

The source-materialisation architecture ([PROPOSAL.md §0](./PROPOSAL.md#0-underlying-pattern)) borrows three moves from content-addressed derivation output resolution: **a content-determined placeholder standing in for an artifact, threaded through evaluation, resolved at a boundary.** This design observes that the `.drv` file *itself* is a fourth instance of the same pattern, hiding in plain sight.

A `.drv` file is a content-addressed store object. From `infoForDerivation` (`derivations.cc:107`):

```
references = drv.inputSrcs ∪ {keys of drv.inputDrvs}
contents   = drv.unparse()                                 // the ATerm bytes
hash       = hashString(SHA256, contents)
ca         = TextInfo{ .hash = hash, .references }
drvPath    = makeFixedOutputPathFromCA(name + ".drv", ca)  // text:sha256, flat
```

So `drvPath = hash(its own ATerm + reference set)` — a **pure function of in-memory data**, computable without writing anything. `computeStorePath` (`derivations.cc:127`) is exactly this minus the write, and `writeDerivation` (`derivations.cc:133`) is `computeStorePath` *plus* an `addToStoreFromDump`.

The pattern, instantiated for derivations:

| Pattern move | Source trees (PROPOSAL.md) | Derivations (this design) |
|---|---|---|
| Content-determined identity | `SourceContentId` → `SourcePlaceholder` `/<base32>` | `drvPath = hash(ATerm)` (already content-addressed) |
| Threaded through eval | `SourceVirtual` context, `nString` interpolation | `DrvDeep`/`Built` context, the returned attrset |
| Materialised at a boundary | `outPathsOf` at the serialise chokepoint | the `.drv` write at a resolution boundary ([§4.2](#42-resolution-boundaries)) |

The crucial disanalogy that bounds the design — and is stated up front so it is not mistaken for a limitation discovered late — is in [§2.2](#22-intrinsic-vs-deferrable): a content-addressed *output*'s path is unknown because computing it **requires running the builder** (a real build); a *`drvPath`* is unknown only until you **hash bytes you already hold**. So `drvPath` laziness defers a hash and a disk write, never a build. There is no organic "build" event to be its resolution boundary the way a CA output has one — the boundary is *artificial* (end-of-eval, or first read-back of the `.drv` as a file). That is precisely what DetSys's `waitForPath` barriers are ([§3](#3-detsys-asyncpathwriter)), and what [§4.2](#42-resolution-boundaries) enumerates.

## 0.5 The algebra

**`.drv` materialisation as a content-addressed selective task.** [§0](#0-the-pattern) places this design as a fourth instance of the placeholder pattern. This section makes the placement precise in the same terms [PROPOSAL.md §0–§3](./PROPOSAL.md#0-underlying-pattern) uses for the source side — because, as there, the payoff is not vocabulary but **laws**: a categorical placement hands us a fixed inventory of properties an implementation must satisfy, each of which becomes a property-based or example test ([§11](#11-laws-and-test-plan)). Three layers; the **selective-task** layer is load-bearing and comes first, the operational call-by-need reading second.

### The selective-task axis

[PROPOSAL.md §0](./PROPOSAL.md#0-underlying-pattern) fixes the substrate: *Build Systems à la Carte* (Mokhov, Mitchell & Peyton Jones, 2018), whose `Task` abstraction is **graded by a constraint** — `Applicative` (dependencies fixed statically: Make), `Selective` (dependencies known statically, but *which* are demanded is chosen from a value: Dune/Excel), `Monadic` (dependencies computed from built values: Shake, and Nix-with-IFD). `.drv` materialisation sits at the **selective** grade, and that grade is exactly what lets deferral-with-batching coexist with substitution-elision:

| Selective `Task` element | In this design |
|---|---|
| **Applicative skeleton** — the full static dependency graph | the pending `.drv` closure: every reachable `drvPath` is computed eagerly during eval ([§2.2](#22-intrinsic-vs-deferrable)); IFD-free ⇒ the graph is complete before any build |
| **`select : f (Either a b) → f (a → b) → f b`** — run the second effect *only* for the `Left` branch | the build Worker chooses per node: substitute outputs and stop, or build and then demand the `.drv` reference closure. `makeLazyDrvStore` lets that choice read pending `.drv` bytes without first writing them. `queryMissing` remains the reporting analogue of this walk for `--dry-run`, also backed by the lazy store overlay |
| **the unchosen branch's effects never run** | the substituted node's *output build* is skipped — the [§7](#7-performance-characteristics) elision. **Caveat (A6):** this is a property of the *build graph*, not of `.drv` materialisation. A substitutable node's input-closure `.drv` writes are elided *only when that node is the requested target*; a substitutable *input* of a must-build target is still materialised, because the built target's `.drv` reference closure (`inputDrvs`/`inputSrcs`) must be valid on disk (referential integrity). So "the unchosen branch writes nothing" holds for the build, not for the referentially-closed `.drv` set |
| **static over-approximation** — you may *list* all potential dependencies without *running* them | eval names every `drvPath` (the over-approx); the flush runs only the demanded frontier — this duality **is** "the batch is known eagerly, the writes are chosen by demand" |
| **monadic escape** — a `bind` whose continuation's dependencies come from a built value | IFD / `realiseContext` (`primops.cc:172`) and the `DrvDeep` `computeFSClosure` (`:1855`): materialisation is *forced* mid-eval; rare for the target workload (Nixpkgs forbids IFD), build-forcing where it fires |

The defining property of a selective functor is precisely *"extract the dependency structure statically, choose which branches to execute dynamically."* Read as a sentence about this design it says: **one content-addressed batch is determined eagerly (applicative skeleton, order-free — [§4.3](#43-the-batched-store-flush)), while which `.drv`s are written is chosen by demand (the `select`, eliding substitutable subtrees — [§7](#7-performance-characteristics)).** The "bulk-flush-the-closure *or* demand-driven-elision" dichotomy was a false choice between the *applicative* and *monadic* grades; **selective is the name for their conjunction**. In the implemented build path, the selective interpreter is the Worker reading the pending skeleton through `makeLazyDrvStore`: substitutable targets can be inspected without writing their input closure, while must-build targets force the referentially-closed `.drv` set they need.

It also makes the **IFD-free** property of the headline workload *constitutive*, not incidental: IFD-free **is** the selective grade; IFD **is** the monadic grade. The `realiseContext` flush ([§4.2](#42-resolution-boundaries) item 3) is the monadic-escape `bind` — it cannot be statically batched because its continuation's dependencies are built values, the one situation the selective grade excludes. The [§2](#2-the-unblock-primitive) workload analysis and the [§4.2](#42-resolution-boundaries)/[§7](#7-performance-characteristics) flush story are one fact on the Applicative → Selective → Monadic ladder.

### The projection instance

Beneath the task grade is the same **`Projection<>`** [PROPOSAL.md §0/§2.A](./PROPOSAL.md#0-underlying-pattern) defines — a content-keyed partial function `P : I ⇀ O` with `keyP`/`computeP`/`encodeO` and three laws. The `.drv` write is one:

| `Projection<>` role | Source side ([PROPOSAL.md](./PROPOSAL.md)) | `.drv` side (this design) |
|---|---|---|
| `keyP` (content key, **eager**) | `SourceContentId` | `drvPath = makeFixedOutputPathFromCA(SHA256, unparse(drv))` (`derivations.cc:107-123`) |
| `computeP` (deterministic, **deferrable**) | the NAR walk → narHash | `unparse(drv)` → ATerm bytes (`derivations.cc:116`) |
| `encodeO` (bytes stored) | NAR into store | ATerm into store (`addToStoreFromDump`, `derivations.cc:147`) |
| cache backend / membership test | `sourceContentToNarHash` | the store itself (`isValidPath`, `derivations.cc:143`) |

The three §0 laws instantiate directly ([§11](#11-laws-and-test-plan) turns each into a test): **soundness** (equal `drvPath` ⇒ equal bytes — content-addressing); **composition** (a dependent derivation references the `drvPath` by content key — `ensureSlot`, `primops.cc:1864` — and the `drvHashes` memo is keyed by `drvPath`, not call path); **cache-merge** (`isValidPath`-skip *is* `INSERT OR IGNORE`; a fresh process re-mints the identical `drvPath` and finds it valid — the join-semilattice is the cross-process free-rematerialisation [§4.1](#41-the-drv-write-queue) calls "the deepest memo"). This *derives* finding [S1](#findings-ledger) rather than asserting it: the source scheduler exists to memoise an expensive **`computeP`** (the narHash walk *is* the value); a `.drv`'s expensive part is its **`keyP`** (eager, to fill `drvPath`), its `computeP = unparse` trivial. "Scheduler vs write-queue" is a one-line consequence of *which half of the projection is costly* — key or value.

### The operational intuition: call-by-need over a hash-consed store

The selective/projection framing has an exact operational shadow — **call-by-need evaluation**, with the store as the heap:

| Lazy-evaluation notion | In this design |
|---|---|
| heap (a memo table address → value) | the store: `storePath ⇀ bytes`, membership = `isValidPath` |
| thunk (a suspended computation, named before it is forced) | a pending `.drv`: `drvPath` known, bytes pending — held in `AsyncPathWriter`'s pending map |
| the thunk's address | `drvPath` — but **content-addressed**, so stable across processes (a heap pointer is not — REVIEW-DISCIPLINE R2) |
| `force` to WHNF | flush: run `computeP`/`encodeO`, write the bytes |
| black-holing / "already forced?" | `isValidPath` (the `writeDerivation` bail, `derivations.cc:143`) |
| hash-consing / maximal sharing | content-addressing: two structurally-equal `drv`s **are** one heap cell (`AsyncPathWriter` keyed by `drvPath` dedups the enqueue — Finding [B](#findings-ledger)) |
| demand analysis (force only what's needed; stop at already-WHNF values) | build goals, and `queryMissing` for dry-run reports, read pending `.drv`s through `makeLazyDrvStore`; substitutable targets stop before the input closure is forced |
| spine-strict force at a boundary | the store-kind-aware drain ([§4.3](#43-the-batched-store-flush)) |

So the design is **call-by-need lifted from values to store objects**, and content-addressing is what upgrades a thunk's identity from a process-local pointer to a process-independent name — precisely why a second process cache-hits (R2) and why the placeholder is safe in error messages and lockfiles. Substitutable-subtree elision is "don't force a thunk whose value is already available"; the batch is "force the demanded spine in one pass." The selective grade is the *type-level* statement of the same thing: WHNF-demand that is statically analysable because (IFD-free) no forced value can introduce a new dependency.

### What the framing resolves

- **[§4.2](#42-resolution-boundaries) item 1 vs [§7](#7-performance-characteristics) — what performs the elision, and where.** The elision is performed by the build **goals**, Worker-side: `haveDerivation` substitutes a target's outputs and `co_return`s (`derivation-goal.cc:138`) *before* creating the goals that read input `.drv`s (C1/V1), so a substitutable subtree's input closure is never demanded. The implemented local-build path does not compute a separate client-side frontier. Instead, `installables.cc` wraps the `evalStore` in `makeLazyDrvStore`, so the Worker can read a pending target `.drv` without materialising it; only a must-build target later calls `materialiseDeferred` and writes its referential closure. Remote daemons cannot see the client's queue, so that path flushes the pending queue before the closure is copied to the daemon.
- **`queryMissing` is a report reader, not the write driver.** The earlier `pendingDrvs_`-aware `queryMissing` pre-pass plan is superseded by the same lazy-store overlay used for local builds. For `nix build --dry-run`, `build.cc` runs `printMissing`/`queryMissing` against a `makeLazyDrvStore` wrapper, so the report can classify pending `.drv`s without mutating the store. The `misc.cc:208` shortcut remains useful context for why the overlay is necessary, but it is no longer the enabling change.
- **Several [§5](#5-soundness-obligations) obligations collapse into laws.** Idempotent re-flush (Finding [C](#findings-ledger)), cross-process determinism (R2), and "two maps, one flush" are the join-semilattice monotonicity of one projection — not three separate arguments. [§11](#11-laws-and-test-plan) states them once, as laws, with tests.

## 1. What `derivationStrict` does today

A simulated walk of `prim_derivationStrict` → `derivationStrictInternal` (`primops.cc`), in execution order, on a derivation with one source input and one derivation dependency:

1. Parse the attrset into a `BasicDerivation drv` (builder, args, env, outputs, …).
2. **Resolve every source placeholder eagerly** (`primops.cc:1833-1840`): collect each `SourceVirtual` from the string context into `svBatch`, then `svResolved = state.materialisationScheduler->outPathsOf(svBatch)`. For a NixArchive source on a cold store this dispatches to `materialiseFused` → `fetchToStore2(Copy)` → `addToStore` — it **walks *and copies* the source bytes into the store now** ([PROPOSAL.md §2.O](./PROPOSAL.md#o-sourceplaceholder--materialisationscheduler--content-keyed-memo)).
3. Walk the string context (`primops.cc:1844-1894`) to populate `drv.inputSrcs` / `drv.inputDrvs`:
   - `Opaque` / `SourceVirtual` → `ensureLazyPathCopied` + insert the real path into `inputSrcs` (`:1866-1891`);
   - `Built` (the common `${pkgs.foo}/bin/x` output-dependency edge) → `drv.inputDrvs.ensureSlot(*b.drvPath).value.insert(b.output)` (`:1864`). **Simulated:** `ensureSlot` (`derived-path-map.hh:84`) is a pure in-memory map insert — *it reads no `.drv` from disk*;
   - `DrvDeep` (the whole-closure `builtins.unsafeDiscardOutputDependency` edge) → `computeFSClosure(d.drvPath, refs)` (`:1855`), which *does* require the dependency `.drv` and its closure to be valid in the store.
4. Apply `svRewrites` to `builder`/`args`/`env`/`platform`/`structuredAttrs` (`:1910-1935`) so the placeholder renders become real store paths in the persisted fields.
5. **Write the `.drv`** (`:2028-2029`): `settings.readOnlyMode ? computeStorePath(...) : state.store->writeDerivation(drv, state.repair)`.
6. Compute and **memoise** `hashDerivationModulo` into `drvHashes` (`:2037-2040`) — required even in read-only mode, "because in that case we don't actually write store derivations, so we can't read them later."
7. Build and return the result attrset: `drvPath` (`:2043`), and per-output strings via `mkOutputString` (`:2051`).

Steps 2 and 5 are the eager work this design defers. Step 6 is the load-bearing observation for soundness ([§5](#5-soundness-obligations)): the in-process memo is *already* populated, so dependent derivations evaluated later in the same process never need to read an earlier `.drv` back from disk.

## 2. The unblock primitive

> **The evaluator can be unblocked by returning the derivation attrset without materialising the `.drv`.** This is not a hypothesis — `readOnlyMode` does exactly it today.

**Simulated.** With `settings.readOnlyMode` set (`repl` `repl.cc:582`, `nix search` `search.cc:65`, `nix-env` `nix-env.cc:990`, `nix-instantiate --eval` `nix-instantiate.cc:184`, `nix log` `log.cc:29`, or the opt-in `--read-only` flag `installables.cc:235` — **but *not* `nix eval` or `nix build --dry-run`, which are non-read-only and write the `.drv` eagerly today**, [§4.2](#42-resolution-boundaries) item 5 / [§7](#7-performance-characteristics)), step 5 above takes the `computeStorePath(*state.store, drv)` branch: `drvPath` is computed purely in-memory and **`writeDerivation` is never called**. Steps 6-7 then build the complete attrset — `drvPath`, `outPath`, all per-output strings — and `prim_derivationStrict` returns it. Evaluation continues; consumers interpolate `.drvPath`/`.outPath` as strings. No `.drv` is on disk, and nothing in the returned value requires one.

The returned attrset is a pure function of the in-memory `drv` struct. The `.drv` *file* is a side-effect that **no field of the returned value consumes**.

**But read-only is the wrong frame for the *win* (review correction).** Read-only already writes nothing, so deferral changes nothing there — it only *proves the premise*. The workload where deferral is both **live** and **valuable** is **eval-store mass instantiation**: the Hydra / `nix-eval-jobs` model that evaluates a whole jobset to thousands of `.drv`s and builds them elsewhere. That pass is driven by `nix-instantiate` writing to a `--eval-store` (`common-eval-args.cc:145` — "to store derivations (`.drv` files) and inputs referenced by them"), and it is **not** read-only: `nix-instantiate` sets `readOnlyMode` only under `evalOnly && !wantsReadWrite` (`nix-instantiate.cc:183-184`) — i.e. `--eval` without `--read-write-mode` (independent review also found `--dry-run`, `:172`, and `--parse`, `:152`, set it; none apply to a plain instantiation). The `.drv`-producing path runs `requireDrvPath` + `addPermRoot` (`:93-111`). So this is precisely where today's eager write fires N thousand times and where deferral + batching has its longest uninterrupted window. Three properties of this workload, each verified, make it the clean case — and they are exactly the properties the [findings](#findings-ledger) show the *build* path lacks:

- **non-read-only** → the deferral is actually exercised (a `.drv` *is* normally written, so not-writing-it-yet is a real change);
- **IFD-free** → Nixpkgs forbids import-from-derivation precisely because Hydra evaluates without building and so cannot perform the mid-eval build IFD needs; with no IFD there is **no in-eval `realiseContext` flush** to interrupt the batch ([§4.2](#42-resolution-boundaries));
- **build-free** → the instantiation pass builds nothing, so the build path's "target `.drv` bytes must be readable to build it" constraint ([§7](#7-performance-characteristics), Finding C1) never fires at all.

The read-only proof below stands as the *existence* argument; the eval-store workload is the *value* argument.

### 2.1. The three output-path kinds

The only subtle field is `outPath`. There are three kinds, and none needs the `.drv` on disk to render (`mkOutputString` → `EvalState::mkOutputStringRaw`, `eval.cc`, and `DerivationOutput::path`, `derivations.cc:21`):

| Output kind | `outPath` at eval | Needs `.drv` on disk? |
|---|---|---|
| `InputAddressed` | `makeOutputPath(hashDerivationModulo(drv))` — pure in-memory | No |
| `CAFixed` (fixed-output) | `makeFixedOutputPathFromCA(ca)` — pure in-memory | No |
| `CAFloating` / `Deferred` | `DerivationOutput::CAFloating::path()` returns `nullopt` → render `DownstreamPlaceholder::fromSingleDerivedPathBuilt(b)` (`eval.cc`), resolved post-**build** | No (resolved at build, never from the `.drv` file) |

So the floating-CA case the original question invoked is the *strongest* evidence: its `outPath` is **already** a placeholder threaded through eval and resolved at the build boundary, with no `.drv`-file dependency at all. Input-addressed and fixed outputs are computed in-memory. Every kind renders without the file.

### 2.2. Intrinsic vs deferrable

What is **intrinsic** to producing the returned attrset (cannot be deferred past the moment `.drvPath`/`.outPath` is observed):

- the **source placeholder → realpath rewrites** (they sit inside `args`/`env`, which the ATerm hash covers, which `drvPath` is the hash of);
- each source's **narHash** (a `DryRun` walk) — needed to name the source's CA path that goes into `inputSrcs`, which is in the ATerm;
- `hashDerivationModulo` for input-addressed `outPath`.

What is a **deferrable side-effect** (nothing in the returned attrset consumes it):

- the **`.drv` file write** (`writeDerivation`'s `addToStoreFromDump`);
- the source **byte-copy** (`fetchToStore2(Copy)` — the narHash `DryRun` is intrinsic, the `Copy` is not);
- registration of either in the store DB.

This is the precise boundary. `drvPath` laziness is bounded to "defer the write and the copy, keep the hash" — because, restating [§0](#0-the-pattern): the hash is over bytes already in hand, whereas the copy and the write are real I/O. (The narHash *walk* for a source genuinely reads bytes, so it is real I/O too — but it is intrinsic because the resulting hash *names* the path that lands in the ATerm. Only the *copy* of those bytes is deferrable.)

## 3. DetSys `AsyncPathWriter`

DetSys (`nix-src@35cd98d10`) implements the *write-deferral* half of this design for `.drv` files. Reading `async-path-writer.cc`:

- **`addPath(contents, name, refs, …) → StorePath`** (`:74`) hashes **synchronously on the eval thread** (`hashString(SHA256, contents)`, `makeFixedOutputPathFromCA(TextInfo{...})`, `:81-88`), returns the `StorePath` **immediately**, and queues an `Item{storePath, contents, hash, …, promise}` for a single background `workerThread` (`:42`).
- The worker `std::swap`s the whole pending vector under lock each wakeup (`:52`) — so it **naturally batches** whatever queued between wakeups — but then writes them in a **per-item `addToStoreFromDump` loop** (`:158-171`), not one transaction.
- **`waitForPath(path)` / `waitForAllPaths()`** (`:109`, `:121`) are the sync barriers, scattered across every CLI entry that needs a `.drv` on disk: `installable-flake.cc`, `app.cc:77`, `nix-build.cc:462`, `repl.cc:314`, `nix-env`/`user-env.cc`, `flake.cc:496`, the C API (`nix_api_expr.cc`, `nix_api_value.cc:398`). `prim_derivationStrict` routes through it via a `writeDerivation(*state.asyncPathWriter, drv, …)` overload (`primops.cc:1881`).
- It is **`.drv`-only**: sources (`addPath`/`fetchToStore`) do not go through it.

Two facts about DetSys's implementation directly shape this design:

1. **It also computes `drvPath` eagerly** (the synchronous hash). This independently confirms [§2.2](#22-intrinsic-vs-deferrable): even the fork that most aggressively defers cannot defer the path computation. So this design does not attempt to either.
2. **The remote-store batched path is written and disabled.** `async-path-writer.cc:134-156` is a `#if 0` block that builds a `Store::PathsSource` and calls `store->addMultipleToStore(sources, act, repair)` — exactly the daemon/SSH bulk path — guarded by the comment `// FIXME: addMultipleToStore() shouldn't require a NAR hash.` This branch resolves that FIXME for remote stores, while deliberately keeping LocalStore on the cheaper flat `addToStoreFromDump` path.

## 4. The design

**One flush barrier, two path-discovery front-ends — not two schedulers.** An earlier draft proposed a `DerivationMaterialisationScheduler` as a peer of the source `MaterialisationScheduler`. That was a mis-decomposition (it cargo-culted the source scheduler's *shape* onto a problem that doesn't have the source scheduler's *job*). The two halves answer different questions:

- *"How do I learn my store path?"* — **different.** A **source** doesn't know its path until an expensive **narHash walk** runs; the source `MaterialisationScheduler` exists entirely to memoise and coalesce *that walk* (`narHashByContent_`, the `inFlight_` `shared_future`, `outPathsOf` batching, `SourceContentId`'s N-calls-→-1-identity). A **`.drv`** knows its path **immediately** — `hash(ATerm)`, over bytes in hand, computed eagerly to fill `drvPath` ([§2.2](#22-intrinsic-vs-deferrable)). There is no walk to coalesce and no content-keyed identity to share (two identical drvs already *are* one `drvPath`). So the `.drv` side needs **none** of the scheduler's machinery — it is a **write-back queue** (which is exactly what DetSys named `AsyncPathWriter`, not a scheduler).
- *"When/how do I flush to the store?"* — **identical, and coupled.** Both defer a content-addressed store write to observation boundaries; both are `EvalState`-resident; and crucially **they share one reference DAG** — a `.drv` references its sources via `inputSrcs`, so a single reference-ordered materialisation can topologically order sources-and-drvs together. `resetFileCache()` clears source materialisation memo state, but intentionally does **not** drain the deferred `.drv` queue: an undemanded `.drv` is content-addressed and may remain virtual until it is demanded or dropped with the `EvalState`.

So the design is **(a)** the existing source scheduler, unchanged ([§4.1](#41-the-drv-write-queue) leaves it alone); **(b)** a thin `.drv` write-queue ([§4.1](#41-the-drv-write-queue)); and **(c)** one unified, reference-ordered flush both feed ([§4.3](#43-the-batched-store-flush)). The "two schedulers" framing is retired; the only thing that is genuinely two is the *map* of pending entries (one keyed by `SourceContentId`, one by `drvPath`), because the keys differ — not the flush, and not the orchestration.

### 4.1. The `.drv` write-queue

`prim_derivationStrict` (step 5 of [§1](#1-what-derivationstrict-does-today)) stops calling `writeDerivation` unconditionally. Instead:

- compute `drvPath` in-memory via `computeStorePath` (always — the read-only branch generalised to all modes);
- enqueue the pair `(drvPath, drv-bytes)` on a per-`EvalState` **`.drv` write-queue** — `AsyncPathWriter`, whose implementation holds a path-keyed pending map and serves those bytes through `makeLazyDrvStore` until they are materialised;
- populate `drvHashes` exactly as today (`primops.cc:2038-2039`) — **unchanged and load-bearing** ([§5](#5-soundness-obligations));
- build and return the attrset exactly as today.

**Memoisation — at three existing layers, plus one deliberate keying choice.** The design does not re-attempt work already done. Dedup is not a new mechanism; it falls out of layers already in the tree:

1. **`drvPath`-keyed enqueue (the one keying choice this adds).** `derivationStrict` is forced once per *textually distinct* call site (the arg thunk is a force-once `Value`), but two *different* call sites can produce the same `drvPath` (the same derivation reached two ways). Keying `AsyncPathWriter`'s pending map on `drvPath` makes the second enqueue a no-op. This is a small *improvement over DetSys*, whose earlier async writer queued the same `.drv` twice and wrote it twice (the second write harmless but wasted). The map key removes the duplicate before it reaches the queue.
2. **`isValidPath` skip at write time.** Even an un-deduped enqueue is cheap: `writeDerivation` bails on `isValidPath` (`derivations.cc:143`, `if (isValidPath(path) && !repair) return path;`), and `AsyncPathWriter::materialise` black-holes any already-valid node before encoding or writing it. So a `.drv` already on disk (this process or a prior one) is never rewritten.
3. **`drvHashes` content-hash memo.** `hashDerivationModulo(drvPath)` is memoised in a `concurrent_flat_map` (`drvHashes`, declared `derivations.cc:858`; the idempotent `insert_or_assign` is at `derivations.cc:875` in `pathDerivationModulo` and `primops.cc:2039` at instantiation) so the recursive modulo-hash is computed once per `drvPath` per process ([§5](#5-soundness-obligations)).

The deepest memo is content-addressing itself: a fresh process that re-evaluates the same derivation computes the identical `drvPath`, finds it already valid, and skips — cross-process re-materialisation is free.

**Idempotent re-flush.** Flushing is not all-or-nothing at end-of-eval; it fires at every boundary ([§4.2](#42-resolution-boundaries)). Re-flush must not re-submit already-written entries. The branch's `AsyncPathWriter` removes materialised paths from its pending maps and black-holes already-valid store paths, so a second flush sees only entries enqueued since the first. The behaviour is covered by `ReflushIsNoOp`, `AlreadyValidIsNotRewrittenAcrossWriters`, and the duplicate-enqueue tests.

A `VirtualDerivation` is not a new value type or a new `NixStringContextElem` variant — that is a deliberate non-goal ([§9](#9-non-goals-and-boundaries), and the [REVIEW-DISCIPLINE.md](./REVIEW-DISCIPLINE.md) R3 hazard a new context variant would incur). The existing `DrvDeep{drvPath}` and `Built{drvPath, output}` context elements already carry the `drvPath`; "virtual" is purely a *store-side* property — the path is named but its bytes are pending — exactly as a source `Opaque{stand-in}` is named but pending ([PROPOSAL.md §6.1](./PROPOSAL.md#61-input-materialisation)).

### 4.2. Resolution boundaries

A pending `.drv` must be made readable before any operation that reads it back **as a file**. That may mean a real flush (`waitForPath(s)`/`waitForAllPaths`/`materialiseDeferred`) or, for the local build and dry-run report paths, reading through `makeLazyDrvStore`. Enumerated by simulation (R1) — every site that needs the bytes, not just the path string:

1. **Build (the selective frontier, not the whole closure — Finding C1/V1, [§0.5](#05-the-algebra)).** A build attempt must be able to **read** the target `.drv`: `DerivationTrampolineGoal` loads it via `for (drvStore : {evalStore, store}) if (isValidPath(drvPath)) return readDerivation(drvPath); assert(false)` (`derivation-trampoline-goal.cc:122-125`). With `lazy-derivations`, the in-process build path wraps `evalStore` in `makeLazyDrvStore`, so that read can be satisfied from the pending queue without writing the `.drv`. If the target substitutes, `DerivationGoal::haveDerivation` `co_return`s `Substituted` (`derivation-goal.cc:138`) before creating the goals that read input `.drv`s, so the input closure stays virtual and can be elided. If the target must build, `DerivationBuildingGoal` calls `materialiseDeferred(drvPath)` before it copies inputs; materialisation then writes the target `.drv` and its transitive pending references, preserving store referential integrity. For a remote daemon or SSH build store, the Worker cannot see the client's pending queue, so `installables.cc` drains `AsyncPathWriter` before the closure is copied to the daemon; that regime preserves correctness but forfeits local `.drv`-write elision.
2. **Eval-cache `forceDerivation` (Finding C2 — a boundary the first draft missed).** On a *warm eval-cache hit*, `AttrCursor::forceDerivation` (`eval-cache.cc:799-815`) returns the cached `drvPath` string **without running `derivationStrict`**; if `!readOnlyMode && !isValidPath(drvPath)` it `forceValue()`s to regenerate and, if *still* invalid, throws `"don't know how to recreate store derivation"`. Under naive deferral the regeneration re-defers → stays invalid → **throws**. `forceDerivation` has exactly two callers — `installable-flake.cc:92` and `app.cc:129` (the common `nix build`/`nix run` paths). On this branch it is a mandatory **pending-tolerance** site, not a flush site: if the writer knows the `.drv` is pending, `forceDerivation` returns the `drvPath` and lets the later build/output boundary make the bytes readable.
3. **In-eval `realiseContext` (IFD + `DrvDeep`).** `realiseContext`'s `Built` arm calls `buildStore->buildPaths(...)` *during eval* (`primops.cc:172`), reached by import-from-derivation and by the `DrvDeep` whole-closure edge (`primops.cc:1855` `computeFSClosure`). Both need the named `.drv` (and inputs) valid mid-eval. **For the target workload this is rare to absent:** Nixpkgs forbids IFD, and a `DrvDeep` edge arises only from `unsafeDiscardOutputDependency`-style constructs, not ordinary `${pkg}` interpolation (which is `Built`/`ensureSlot`, a pure in-memory insert — `primops.cc:1864`, simulated in [§1](#1-what-derivationstrict-does-today)). Where IFD *does* fire (non-Nixpkgs evals; IFD is default-on in Nix — `eval-settings.hh:214`), it forces a full build mid-eval anyway, so a `.drv` flush there is lost in the build's noise. Still, the flush-on-read guard must cover it for correctness.
4. **`import (drv.drvPath)` / `readDerivation` / `readInvalidDerivation`** (`store-api.cc:1168` `readDerivationCommon`). Reading a `.drv` back as a Nix value requires its bytes.
5. **CLI/C-API observation, *including value output to stdout*.** `nix show-derivation`, `nix derivation show`, copying a `.drv` to a cache, `--add-root` on a `.drv` — and the easy-to-miss one: **emitting a derivation path as a serialised `Value`** (`nix eval .#x.drvPath`, `--raw`/`--json`, `nix-instantiate --eval`). `nix eval` is **non-read-only by default** (the `--read-only` flag, `installables.cc:230-236`, is opt-in and documents it "can cause errors when accessing store paths … during evaluation"), so today it writes the `.drv` eagerly mid-eval; to preserve that observable the deferred design must flush *before the bytes leave the process* — the **derivation-side twin** of the existing source-side cover-fix, which already calls `resolveSourceVirtualContext`/`ensureLazyPathsCopied` on the output `context` (`eval.cc:142-143`, rule at `:80-102`). The branch deliberately drops un-demanded pending entries when the writer is destroyed; the destructor is not a correctness boundary. Therefore stdout-emission of a derivation path is itself a flush site, including concurrent pipelines such as `nix eval .#x.drvPath | xargs nix-store -q …`. These are the `waitForAllPaths` sites; warm eval-cache `drvPath` lookup is covered by `forceDerivation`'s C2 pending-tolerance guard (item 2).

The flush trigger is therefore **two-tier**, exactly like the source `resolveSourceVirtualContext` chokepoint plus its per-path `ensureLazyPathCopied`:

- an **observation-boundary drain** (end-of-eval / `waitForAllPaths`, value output, and remote build submission) — batched on daemon/SSH stores, and reference-ordered per-item on LocalStore because that is faster there ([§4.3](#43-the-batched-store-flush));
- a **targeted guard at in-eval read-back**: `forceDerivation` treats a pending `.drv` as valid-enough because it only returns a path string, while byte consumers (`readInvalidDerivation`, `realiseContext`/IFD, and the `DrvDeep` arm) flush with `waitForPath(s)` before reading, transitively flushing that `.drv`'s pending input `.drv`s first.

The boundary guard is the same *shape* as the source-side boundary audit ([PROPOSAL.md §6.2](./PROPOSAL.md#62-the-addpath-source-virtualisation-boundary)): a finite, enumerable set of sites that consume the artifact, each of which must either accept pending paths explicitly or drain before consuming bytes. The audit surface is `git grep` for `forceDerivation` / `readInvalidDerivation` / `readDerivation` / `computeFSClosure` / `buildPaths` callers reachable from eval.

### 4.3. The batched store flush

At a drain boundary, instead of N independent `writeDerivation` calls from the evaluator, `AsyncPathWriter` materialises the pending reference closure once. The store API route is intentionally store-kind dependent: LocalStore uses the same flat `addToStoreFromDump` path as eager Nix because it avoids `addMultipleToStore`'s NAR-import overhead; daemon/SSH stores use one framed `addMultipleToStore` submission to coalesce protocol round-trips. Deferred source copies flush at the *same barrier* but may use their existing path — Finding D, [§6](#6-interaction-with-the-lazy-source-substrate). **A round-1 draft of this section claimed the bulk route is a single atomic transaction via `addMultipleToStore`; that is wrong, and the correction (Finding C3) matters for what the win actually is.**

**What `addMultipleToStore` actually does on the remote-store route (simulated, `store-api.cc:195`).** It drives `processGraph` — a `ThreadPool`-backed, topologically-ordered, reference-aware walk (`thread-pool.hh:90`) — calling, per node, `addToStore(info, source)` (`local-store.cc:1027`), which ends in `registerValidPath(info)` *singular* (`:1122`) → `registerValidPaths({…one…})` → **one `SQLiteTxn` per path** (`:935`, `:952`). So the bulk path is **N parallel, reference-ordered, single-path transactions**, *not* one atomic transaction. The daemon win is real but narrower than "one txn": fewer protocol round-trips than N separate `writeDerivation` calls (one framed `AddMultipleToStore` op carries the whole batch on the wire — `remote-store.cc:470`) plus parallel ingestion. It is **not** one fsync and **not** atomic across the batch.

**Where the *atomic* two-phase property lives, if it is wanted.** `LocalStore::registerValidPaths(ValidPathInfos)` *plural* (`local-store.cc:938`) *is* a single `SQLiteTxn`, two-phase (loop 1 `addValidPath` all, loop 2 wire `AddReference`, then `topoSort` for cycles, `:952-991`) — so intra-batch references are legal there. But `addMultipleToStore` does **not** call it, and it operates on **already-on-disk** paths (it registers validity; it does not copy bytes). To get atomic registration you would therefore run an explicit **two phases**: (1) copy every pending `.drv`/source's bytes into the store (the `addToStoreFromDump`/restore work), then (2) one `registerValidPaths`-plural transaction over all of them. That is a deliberate design choice (atomicity vs. the simpler per-path path), not the free property the first draft implied — call it out, don't assume it.

**The narHash friction (DetSys's `#if 0` FIXME) — still real on the `addMultipleToStore` route.** A `.drv` is `text:sha256` over *flat* bytes (`Raw::Text`, `FileSerialisationMethod::Flat`, `derivations.cc:150-151`). `addToStore(info, source)` re-hashes the incoming NAR and throws on `hashResult.hash != info.narHash` (`local-store.cc:1062`); there is no `nar:sha256` in hand for a flat text object. Resolution: **compute the per-item `narHash` when building the batch** — for a `.drv` ATerm (kilobytes) a NAR dump + SHA-256 is negligible and is the same pass `addToStoreFromDump` does internally; it satisfies the existing contract with no store-API change. (The alternative — a `Raw::Text` `addMultipleToStore` variant that skips the NAR verify — is deferred: it widens a security-relevant verify path, R4.)

Deferred **source** copies flush at the *same barrier*: a source's `inputSrcs` entry is named by its narHash (intrinsic, already computed), so its `ValidPathInfo` is fully determined; the copy supplies the bytes. This is the cross-derivation sharing the per-`derivationStrict` `outPathsOf` could not express — N derivations sharing sources, or a deep dependency chain, copy sources only when a materialised `.drv` actually demands them.

## 5. Soundness obligations

Each is stated with the invariant it rests on and how it is discharged.

- **The `drvHashes` memo / `.drv`-bytes split makes deferral sound (corrected by independent review — the original discharge had a hole).** *Obligation:* `hashDerivationModulo(B)` recurses through `B`'s `inputDrvs` via `pathDerivationModulo` → on a cold `drvHashes` entry, `readInvalidDerivation(A.drvPath)` — a **disk read of `A`'s `.drv`** (`derivations.cc:872`, via `store-api.cc:1218`). If `A`'s write were deferred and `drvHashes` cold, this read would fail. *The original discharge* — "`A` is instantiated before `B`, so the memo is warm" (`primops.cc:2038-2039`) — **holds only when `B` is instantiated by running `derivationStrict` this process.** The independent review found the gap: on a **warm eval-cache hit** ([§4.2](#42-resolution-boundaries) item 2), `forceDerivation` returns `B`'s cached `drvPath` *without* running `derivationStrict` for `B` *or* `A`, so `drvHashes` is **cold for the entire sub-closure**. *The real discharge:* nothing downstream of a cache hit recomputes the modulo hash. The build goals read `.drv` **bytes** directly (`readDerivation`, `derivation-trampoline-goal.cc:124`) and never call `hashDerivationModulo`/`pathDerivationModulo` (verified: no caller in `build/`). So the cold memo is **benign** — it would only bite a hypothetical in-process `hashDerivationModulo(B)` after a cache hit, which no current path performs; and the [§4.2](#42-resolution-boundaries) build boundary has already made `A`'s `.drv` bytes readable, either through `makeLazyDrvStore` or by a real flush for remote daemons. **The soundness rests on the build-time `.drv`-bytes boundary, not on instantiation ordering.** (R2: the memo key is `drvPath`, a content hash — process-independent — so a second process recomputes the identical key.)
- **Content-addressing closes the mint-vs-write gap.** *Obligation:* the `drvPath` returned at instantiation must equal the path eventually registered. *Discharge:* both are `makeFixedOutputPathFromCA(hash(unparse(drv)))` over the same in-memory `drv` — equal by construction. `writeDerivation`'s `assert(path2 == path)` (`derivations.cc:155`) is the existing backstop; it holds verbatim.
- **Intra-batch references resolve.** *Obligation:* a batched `.drv` referencing a same-batch source/`.drv` must register without a "missing reference" error. *Discharge:* depends on the chosen flush route ([§4.3](#43-the-batched-store-flush), Finding C3). The `addMultipleToStore` route handles it by `processGraph`'s **reference-ordered** ingestion (a referent is added after its references — `store-api.cc:195`), not by atomicity. The explicit two-phase route handles it by `registerValidPaths`-plural inserting all paths before wiring any reference (`local-store.cc:952-991`). Either works; the proposal must not claim the *former* gives the *latter*'s atomicity.
- **Flush/tolerance covers every in-eval read-back.** *Obligation:* no eval-time consumer reads a pending `.drv`'s bytes, and no path-only consumer treats a pending `.drv` as unrecreatable. *Discharge:* the enumerated boundary set ([§4.2](#42-resolution-boundaries)) with `git grep`-able audit surface. `AttrCursor::forceDerivation` (warm eval-cache, Finding C2) is path-only and accepts pending `.drv`s; the byte consumers (`realiseContext`/`buildPaths`, `DrvDeep`, and `readInvalidDerivation`) flush before reading. *The first draft omitted `forceDerivation`; it is the most common one on the `nix build` path and its absence would surface as a `"don't know how to recreate store derivation"` throw, not silent corruption.*
- **Failure latches.** *Obligation:* a `.drv` write that fails (disk full, permissions) must not be silently swallowed — every later observer must see the error. *Discharge:* the source scheduler's `shared_future` winner-election + latched-`Failed` discipline ([PROPOSAL.md §6.1](./PROPOSAL.md#61-input-materialisation)) is reused; DetSys's `AsyncPathWriter` already does this with `promise.set_exception` (`async-path-writer.cc:61`).
- **`readOnlyMode` is unchanged.** Read-only never flushes (it never wrote before); the scheduler simply never drains. The returned attrset is byte-identical to today's read-only path.

## 6. Interaction with the lazy-source substrate

This design and the existing source-materialisation architecture **compose into one resolution boundary**, which is the larger prize. Today the two are sequenced *eagerly within* `derivationStrict`: source placeholders resolve (and copy) at step 2, then the `.drv` writes at step 5. After this design, both defer to the same flush:

- **Sources** already defer their narHash walk to the observation boundary and (for rev-pinned/deferred-mount inputs) defer the copy ([PROPOSAL.md §2.F, §6.1](./PROPOSAL.md#f-mountinput-known-narhash-fast-path)). What forces the *copy* early today is precisely that `derivationStrict` resolves `inputSrcs` to valid paths *before* writing the `.drv` (referential integrity at register time). Deferring the `.drv` write removes that forcing function: `inputSrcs` need only be *named* (narHash known) at instantiation, and *copied* only when a materialised `.drv` actually demands them.
- The unified flush is therefore: at a build/observation boundary, **materialise the demanded pending source/`.drv` reference closure**, reference-ordered. The exact store call is store-kind dependent (`addToStoreFromDump` locally, `addMultipleToStore` for daemon/SSH), and an atomic variant would be the explicit copy-then-`registerValidPaths`-plural two-phase route ([§4.3](#43-the-batched-store-flush), Finding C3).

**The unification is the *boundary*, not necessarily a single store call (Finding D, self-review).** It is tempting to say "feed sources and `.drv`s into one `addMultipleToStore`." But `addMultipleToStore` takes `ValidPathInfo` with `narHash` **and `narSize`** pre-known, and a source is ingested today via `addToStore(name, path, method, …)` (`fetch-to-store.cc:400`) — the accessor overload that *walks* to compute exactly those. A deferred source has its `narHash` (intrinsic, [§2.2](#22-intrinsic-vs-deferrable)) but not its `narSize` without that walk. So `.drv`s (bytes in hand, [§4.3](#43-the-batched-store-flush)) can use the daemon bulk route, while LocalStore keeps the flat eager write path and sources keep their existing `fetchToStore(Copy)` path — all driven *at the same barrier*, but not necessarily through the same call. What is genuinely shared is **one barrier and one reference ordering** — not one API call. Because the `.drv`→source dependency lives in `inputSrcs`, a single reference-ordered drain (sources before referring `.drv`s) suffices; there is **no separate scheduler to coordinate** — the earlier "two schedulers need an ordered flush" risk dissolves into one queue draining in DAG order.

The result is the design the original question described: *"when a derivation is forced, recursively and in parallel batch and do all the store copies, rewrites, and materialisation."* The rewrites are still eager (intrinsic, [§2.2](#22-intrinsic-vs-deferrable)); the copies and writes are demand-boundary work. Parallelism and batching are store-kind dependent: source materialisation keeps the scheduler's existing `ThreadPool` fan-out, remote `.drv` drains use the daemon bulk route, and LocalStore deliberately stays on the cheaper flat write path.

A concrete payoff: `nix build --dry-run` over a flake (pure instantiation, thousands of `.drv`s, **zero builds**) today copies every `lib.cleanSource ./.` and writes every `.drv` — it is **not** `readOnlyMode` (`build.cc:142-154` returns before `Installable::build`; `toDerivedPaths` instantiates and writes). With `lazy-derivations`, dry-run still runs `printMissing`/`queryMissing`, but against a `makeLazyDrvStore` wrapper. The report can read pending `.drv` bytes from memory, classify "will be built / fetched", and produce no store mutations — faster, and more honest about being a dry run.

## 7. Performance characteristics

"walk" = one tree traversal; "copy" = walk + store write; "txn" = one SQLite transaction / daemon round-trip.

| Scenario | Today | After this design |
|---|---|---|
| **Eval-store mass instantiation** (Hydra/`nix-eval-jobs`: M `.drv`s, no build, no IFD) — *the headline win* | M `.drv` writes (M daemon round-trips when the eval store is remote) + per-drv source copies | writes + copies deferred to the end-of-eval boundary; daemon/SSH stores coalesce round-trips through one framed bulk op, while LocalStore relies on overlap/deferral and is workload-dependent |
| `nix build --dry-run`, source-heavy flake (**non-read-only — writes today**) | M `.drv` writes + N source copies (instantiation writes `.drv`s; only the *build* is skipped — §6) | 0 writes, 0 copies — `printMissing`/`queryMissing` reads pending `.drv`s through `makeLazyDrvStore` and reports "will be built/fetched" without observing any `.drv` as a file (the genuine dry-run elision) |
| `nix eval .#x.drvPath`, source-heavy flake (**non-read-only — writes today**) | the `.drv` write + N source copies, eager | the `.drv` write **deferred to the output-emission boundary** ([§4.2](#42-resolution-boundaries) item 5) — *same write, batched, not elided* (the path is observed); its inputs' source copies flush with it |
| `nix build` of 1 target, M-derivation closure, all built from source | M `.drv` writes + per-drv source copies | M `.drv` writes + demanded source copies materialised at the build boundary; daemon/SSH stores coalesce `.drv` transfer, LocalStore stays per-item — **same M writes, not fewer writes** |
| `nix build` where **the requested target itself** is substitutable from a cache | full instantiation: M `.drv` writes + source copies; substitution then skips most *builds* | the **target** `.drv` must be readable, but local in-process builds can read it from `makeLazyDrvStore` without writing it; a fully-substitutable target reads **none** of its input `.drv`s (it returns `Substituted` before the input-drv goals exist), so its whole input-closure `.drv` writes + source copies are **elided** |
| `nix build` of a **must-build** target over **substitutable inputs** (the realistic monorepo: your code on cached deps) | full instantiation: M `.drv` writes + source copies | **NO input-closure elision (A6).** `materialiseDeferred(target)` → `materialise` walks the target `.drv`'s transitive `inputDrv`/`inputSrc` refs and writes the **whole** input closure (referential integrity), incl. sub-nodes whose *outputs* substitute; only those nodes' *output builds* are skipped. Empirically: chain of 6, cache one intermediate's output, build the root → all 6 `.drv`s + 6 sources still materialised. Elision is all-or-nothing **at the requested target** |
| Deep dependency chain (A→B→…→Z), then build from source | each `.drv` write is eager | materialised at the build boundary in reference order; daemon/SSH stores coalesce transfer, LocalStore stays per-item; still N writes (the inputs are built, not substituted) |

**Finding C1 (re-narrowed by independent review) — the substituted-away-input win is real; the *target* `.drv` is not skippable.** The first draft over-claimed both ways and an independent code-walk corrected it. The precise truth, simulated against master:

- **The target `.drv` is always required for a build attempt.** `DerivationTrampolineGoal` loads it (`derivation-trampoline-goal.cc:122-125`, `if (isValidPath) readDerivation … else assert(false)`); an `Opaque` (deferred) target `.drv` that nothing can produce reaches that `assert(false)` via a re-entrant obtain-goal. So a deferred `.drv` *that a build needs* must be readable first: local builds provide it through `makeLazyDrvStore`, while remote builds flush it before handing the closure to the daemon.
- **But a fully-substitutable target reads *none* of its input `.drv`s.** `haveDerivation` attempts substitution of the *target's outputs* first and `co_return`s `Substituted` at `derivation-goal.cc:138` — **before** `makeDerivationResolutionGoal` (`:151`) or `makeDerivationBuildingGoal` (`:229`), the only goals that read the input `.drv` closure, are ever created. The input-closure validity requirement (`gaveUpOnSubstitution` → `derivation-building-goal.cc:211-213`, every input `.drv` valid else `assert(false)`) is reached **only after the per-target substitution attempt fails** — i.e. only for the subset actually built from source.

So deferral **does** elide the input-closure `.drv` writes (and source copies) of **a requested target that itself fully substitutes** — a genuine win the round-2 retraction wrongly threw away — while the target `.drv` must remain readable. **But this is narrower than "any subtree that substitutes" (A6, 2026-06-03):** a substitutable node that is an `inputDrv` of a *must-build* target is materialised anyway, because `materialiseDeferred(target)` → `materialise` walks the built target `.drv`'s transitive `inputDrv`/`inputSrc` references and writes the whole input closure (referential integrity). The *build graph* is selective (the substituted node's *output build* is skipped), but the built target's *`.drv` reference closure* is not — so the elision is all-or-nothing at the requested target, and the realistic "your code on cached deps" shape (must-build target) elides nothing. Dry-run reporting mirrors the same classification through `makeLazyDrvStore`, but it remains a report path rather than the mechanism that drives materialisation.

**The workload-shape risk (R5/R6, stated as a blocker not an afterthought).** Deferral can *un-fuse* deliberately fused work and lose. Measured shapes:

- **Eval-store mass instantiation (the win, especially for daemon eval stores):** non-read-only, IFD-free, build-free ([§2](#2-the-unblock-primitive)). Deferral is live and the window is uninterrupted; on a daemon/SSH eval store the delta is N per-path round-trips → one framed bulk submission, while on LocalStore the payoff depends on async overlap and eval/write balance.
- **Build-everything-from-source:** every `.drv` is written anyway (C1), so the only delta is daemon round-trip coalescing and, for LocalStore eval-population paths, async overlap. On a tiny local closure, deferral overhead can dominate. Measure on a small closure.
- **Substitutable build:** the saving is source-copy elision (above), bounded by how much of the closure substitutes. **No longer the headline.**

The source-materialisation work already learned this lesson the expensive way (an eval-trace cache that was hot-7×-faster on one workload and cold-8.6×-slower on another); this design inherits the discipline of gating behind a setting and measuring each shape, not defaulting blind.

## 8. Comparison

Three distinct mechanisms, often conflated:

| | Async writer (DetSys, shipped) | Batched writer (this design, v1) | Lazy writer (this design, full) |
|---|---|---|---|
| `drvPath` computed | eagerly, eval thread | eagerly, eval thread | eagerly, eval thread |
| `.drv` write timing | bg thread, per-item loop | bg thread, store-kind-dependent drain: flat per-item locally, bulk submission remotely (**N per-path txns, not 1** — C3) | skipped until a boundary reads it |
| Source copy | eager (unchanged) | **deferred, same flush** | deferred |
| Win | hide write *latency* | + reduce *daemon round-trip count*; preserve cheap LocalStore writes; cross-drv source sharing | + elide the input-closure `.drv` writes **and** source copies of a fully-substitutable requested target; local builds may also leave the substitutable target `.drv` virtual because it is read through `makeLazyDrvStore` |
| Resolution boundary | `waitForPath` per CLI site | same + end-of-eval drain + `forceDerivation` pending tolerance (C2) | same + transitive flush-on-read guard |
| Blocker | none (shipped) | narHash friction ([§4.3](#43-the-batched-store-flush)) — solved by per-item narHash, and it is *free* (the existing flat-CA write already NAR-hashes, `local-store.cc:1271-1274`) | any build must be able to read the **target** `.drv`; local builds satisfy that virtually, while remote daemons require a real flush |

They are **complementary, not alternatives.** The ideal end state is async + demand-aware: DetSys's disabled remote bulk route enabled where it helps, LocalStore kept on the cheap flat write path, and source copies folded into the same demand boundary. This design is "port DetSys's async harness, complete the remote batch it disabled, avoid that batch where it is slower, then extend the demand boundary to sources." **Its clean win is daemon eval-store mass instantiation ([§2](#2-the-unblock-primitive)); for the build path it degrades gracefully to async + demand-boundary writes plus target-level elision ([§7](#7-performance-characteristics), C1/A6), with the target `.drv` readability requirement handled virtually for local builds and by a real flush for remote daemons.**

Relative to DetSys specifically: DetSys defers the `.drv` write but eagerly copies sources and writes per-item; this design defers both, uses a remote bulk route only where it helps, and drains sources and `.drv`s at one demand boundary. Relative to local `master`, none of this exists there — `Store::writeDerivation` is synchronous and there is no `AsyncPathWriter`. This branch adds the writer, the lazy-store overlay, and the `lazy-derivations` setting.

## 9. Non-goals and boundaries

- **Not a new value type or context variant.** `VirtualDerivation` is a store-side pending state keyed by the already-content-addressed `drvPath`; it reuses `DrvDeep`/`Built` context. A new `NixStringContextElem` variant would incur the R3 consumer-enumeration hazard (parser/printer/`eval-cache.cc::AttrDb::setString`/error paths) for no benefit — the `drvPath` is already in the existing elements.
- **Not lazy `drvPath` computation.** `drvPath` is computed eagerly, always — forced by content-addressing ([§2.2](#22-intrinsic-vs-deferrable)), confirmed by DetSys doing the same.
- **Not a derivation-output-resolution change.** `tryResolve` / `DownstreamPlaceholder` / the realisations table are unchanged; this design is about the `.drv` *file*, not output paths.
- **Not a daemon-protocol change (v1).** The remote batched route uses the existing `addMultipleToStore` (op 44); the per-item-narHash fix is client-side. The optional text-CA `addMultiple` variant ([§4.3](#43-the-batched-store-flush)) *would* be a protocol addition and is explicitly deferred.
- **Not a change to read-only semantics.** Read-only eval already returns the attrset without writing; this design generalises that path, it does not alter it.
- **Not async eval.** As with the source scheduler ([PROPOSAL.md §6.5](./PROPOSAL.md#65-design-boundaries-deliberately-not-crossed)), materialisation parallelises via `ThreadPool`/worker thread; eval itself stays thread-per-task.

## 10. Open questions and review obligations

Per [REVIEW-DISCIPLINE.md](./REVIEW-DISCIPLINE.md), the load-bearing claims that a review must *simulate* (R1), not cite. Items 1-2 were the open questions in the first draft; the adversarial pass **answered** them (against master) — recorded here as resolved, with the residue that remains open:

1. **(RESOLVED — Finding C1/V1) Substitutability ordering.** *Answered (as re-narrowed by independent review, V1 — superseding the round-2/4 over-retraction this item once stated):* a build attempt must be able to **read** the target `.drv` (`DerivationTrampolineGoal` `readDerivation` → `assert(false)` on invalid, `derivation-trampoline-goal.cc:122-125`), but does **not** necessarily materialise its whole input closure. `haveDerivation` `co_return`s `Substituted` (`derivation-goal.cc:138`) *before* the resolution/building goals (`:151`/`:229`) that read input `.drv`s are created — so a fully-substitutable target reads zero input `.drv`s, and input-closure writes are elided exactly where the requested target substitutes. The implemented residue is daemon visibility, not frontier computation: local in-process builds back `evalStore` with `makeLazyDrvStore`, so a pending target `.drv` is readable without being written; remote daemons cannot see that queue, so the branch flushes the `.drv` closure before copying it to the daemon. Substitution downloads stay in the Worker's parallel, build-interleaved pool (`maxSubstitutionJobs`, `worker.cc:273`); only the cheap local `.drv`-write elision is regime-dependent (full in-process, forfeited for a remote daemon).
2. **(RESOLVED — Findings M1/C2) In-eval flush frequency.** *Answered:* the dominant target workload (Nixpkgs/Hydra) is **IFD-free** (Nixpkgs forbids it; Hydra cannot build during eval), so the `realiseContext`/`Built` flush does not fire there; the `DrvDeep`/`computeFSClosure` edge arises only from `unsafeDiscardOutputDependency`-style constructs, not `${pkg}` interpolation (`Built`/`ensureSlot`, in-memory — `primops.cc:1864`). The first draft over-weighted IFD and **missed** the genuinely common in-eval boundary, `forceDerivation` on a warm eval cache (C2, `eval-cache.cc:799`). The branch covers this with pending-derivation tolerance in `forceDerivation`, plus real flushes at byte consumers, and the `lazy-derivations/warm-eval-cache.sh` functional test.
3. **(R5, RESOLVED by independent review) Latency floor of the per-item narHash.** *Answered:* it is genuinely free, not a hidden second pass. `addToStoreFromDump` already computes a NAR hash for any non-`NixArchive`-SHA256 object (`local-store.cc:1271-1274`: `if (dumpMethod != NixArchive || hashAlgo != SHA256) { HashSink narSink; dumpPath(realPath, narSink); narHash = narSink.finish(); }`), and a `.drv` is `Raw::Text`/`Flat` — so that branch *already fires* on every `.drv` write today. The batched route computes the same hash the eager route does; it is not net-new work.
4. **(R4) Blast radius — confirmed nil.** This design changes *when* `.drv`s and sources are written, not *what* — `drvPath`, `outPath`, and the `.drv` contents are byte-identical to today. No rebuild scope. R2: `drvPath = hash(unparse(drv))` in both the eager and deferred paths, over the same `drv` struct.
5. **(R6) Alternatives.** (a) DetSys's async-only writer — adopted as the *base*, extended (it leaves source copies eager and writes per-item). (b) A daemon-side deferred-write registry — rejected: the pending state needs `EvalState` context the daemon lacks; local builds keep that state visible with `makeLazyDrvStore`, while remote builds flush before submission. (c) Doing nothing and relying on OS page-cache — rejected on R5: the cost is fsync + SQLite txn commit per `.drv` (`registerValidPaths` syncs when `syncBeforeRegistering`, `local-store.cc:945`), which the page cache does not amortise.

A review of this document that does not walk items 1-2 against the source is a citation audit, not an adversarial review (R8). *(They have now been walked; the Findings ledger records the result.)*

## 11. Laws and test plan

Per [PROPOSAL.md §3](./PROPOSAL.md#3-algebraic-invariants), each law below holds "by construction, by test, and by review," and the test column states **property** (RapidCheck `RC_GTEST_PROP`/`RC_GTEST_FIXTURE_PROP`), **unit** (example-based `TEST`/`TEST_F`), **boundary audit** (drive the *real* production path and assert the guard fires — non-vacuous by construction, the `boundary-audit-*.cc` idiom), or **integration** (`tests/functional/.../*.sh`). Negative controls are named explicitly; the discipline ([PROPOSAL.md §5](./PROPOSAL.md#5-design-rationale): *"a unit test that calls a component directly cannot reveal that nothing in production does"*) is that the headline laws get an end-to-end test *and* a property, not one or the other. Test homes now exist in `src/libstore-tests/lazy-derivation-*.cc`, `src/libexpr-tests/lazy-derivation-defer.cc`, and `tests/functional/lazy-derivations/*.sh`. `Arbitrary<Derivation>` also exists in `src/libstore-test-support/include/nix/store/tests/derivation.hh`, following the `derived-path.hh` idiom for input-addressed outputs while leaving CA/`Deferred`/`Impure` variants to feature-gated fixtures.

The laws are layered exactly as [§0.5](#05-the-algebra): **LD-P** (projection), **LD-S** (selective task), **LD-V** (virtual ⇄ materialised phase).

### Layer 1 — projection laws (LD-P)

| ID | Obligation | Test (type · name) |
|---|---|---|
| **LD-P1** | **Key purity / eager identity.** `computeStorePath(drv)` is a pure function of the in-memory `drv`; computing it writes nothing and reads no `.drv`. | property · `LazyDrv.KeyIsPureNoStoreEffect` (+ negative control: `isValidPath(key)==false` after compute-only) |
| **LD-P2** | **Mint = write.** Deferred-instantiation and eventual-register agree: `computeStorePath(drv) == writeDerivation(drv)`. | unit · `LazyDrv.ComputedPathEqualsWrittenPath`; in-tree backstop `derivations.cc:155` `assert(path2==path)` |
| **LD-P3** | **Soundness.** Equal `drvPath` ⇒ equal ATerm bytes ⇒ equal reference set; distinct content ⇒ distinct key. | property · `LazyDrv.KeyDiscriminatesContent` (+ negative `MutatedFieldChangesKey`) |
| **LD-P4** | **Cross-process determinism (R2).** `computeStorePath` consumes no heap pointer; two `EvalState` instances agree on bytes. | property · `LazyDrv.KeyStableAcrossStates` (mirrors `SourceContentId.SameInputsSameId`) |
| **LD-P5** | **Idempotent materialisation (join-semilattice).** Re-flushing a valid `.drv` is a no-op; store membership grows monotonically. | property · `LazyDrv.ReflushIsNoOp` (store-size invariant); unit · the `derivations.cc:143` bail; coverage · `AlreadyValidIsNotRewrittenAcrossWriters` |
| **LD-P6** | **Enqueue dedup.** Two enqueues of one `drvPath` collapse to one queue entry and one write (improvement over DetSys's unconditional duplicate queueing, Finding [B](#findings-ledger)). | unit/property · `LazyDrv.DuplicateEnqueueWritesOnce` (`addToStoreFromDump` call-count == 1); negative control · `LazyDrv.DistinctEnqueuesWriteEach` |
| **LD-P7** | **`computeP` triviality / cost asymmetry — *derives* S1.** The cost is in `keyP` (eager); `computeP = unparse` is the *same* byte-production `keyP` already did (`infoForDerivation` computes `contents` once, `derivations.cc:116`, feeding both `hash`→key and `addToStoreFromDump`→value). So deferral defers only `encodeO`, never a recompute, and the `.drv` side needs no memoising scheduler (Finding [S1](#findings-ledger) falls out, not asserted). | characterisation · `LazyDrv.UnparseComputedOnce` (call-count on `unparse` across compute+flush of one drv ≤ 1 extra); no clean negative control — a cost property, not a guard |
| **LD-P8** | **Reference-set fidelity through deferral** `[LOAD-BEARING]`. `references = inputSrcs ∪ keys(inputDrvs)` (`derivations.cc:109-111`) is a function of the captured `drv` struct, independent of *when* the flush runs — so the registered reference edges (GC/closure) are byte-identical to the eager write. Distinct from LD-P3 (the *key*): this is the *DB-edge* consumer (R3-shape). | unit · `LazyDrv.DeferredReferencesMatchEager` (eager vs enqueue+flush ⇒ equal `queryReferences`); integration · GC-after-deferred-flush keeps inputs alive. **Mutation caught:** a queue holding `drv` by pointer that is mutated before flush |

### Layer 2 — selective-task laws (LD-S)

| ID | Obligation | Test |
|---|---|---|
| **LD-S1** | **Static skeleton.** Post-eval, IFD-free, non-read-only: every reachable `drvPath` is computed and queued, with zero builds and zero `realiseContext` calls (the eval-store mass-instantiation headline, [§2](#2-the-unblock-primitive)). | integration · `lazy-derivations/mass-instantiation.sh` (N drvPaths queued, 0 builds, deferred-write marker count == N) |
| **LD-S2** | **Selective elision (headline).** For an in-process build of a fully-substitutable requested target, the input-closure `.drv`s are **not** materialised: the Worker reads the target through `makeLazyDrvStore`, substitutes the output, and never demands the input closure. For a must-build target, materialising the target writes its transitive pending reference closure. | integration · `lazy-derivations/substitutable-input-elision.sh` **+ negative control** `…non-substitutable-writes-all.sh` (no substituter ⇒ whole closure written; elision must not over-fire) |
| **LD-S3** | **Skip preserves result.** drvPath, outPath, and realised outputs are byte-identical with deferral ON vs OFF (the unchosen branch is pure — analogue of cache-decorator transparency C1). | integration (differential) · `lazy-derivations/deferral-is-observationally-transparent.sh` |
| **LD-S4** | **Daemon safety / frontier sufficiency.** Every `.drv` a build goal reads is valid before the Worker runs; no goal hits `assert(false)` on a deferred `.drv`. The readers are **three** (R9-verified; the draft listed two): the **target** via `derivation-trampoline-goal.cc:124`; each built node's **immediate inputs** via `derivation-building-goal.cc:212` (`deepQueryDerivationOutputMap`); and — **gated under `impure-derivations`** — `derivation-resolution-goal.cc:59` (`readDerivation`, gate `:57-58`). | integration · `lazy-derivations/build-from-source-frontier.sh`; **boundary audit** · `forceDerivation` warm-cache pending tolerance (Finding C2); **meta** · `LazyDrv.NoUnenumeratedDrvReaderInBuild` (`git grep` of `readDerivation`/`deepQueryDerivationOutputMap`/`readInvalidDerivation` in `src/libstore/build/` == those three); **negative** · a dropped frontier input fails *loudly* |
| **LD-S5** | **Monadic escape forces.** When IFD/`realiseContext` (`primops.cc:172`) or a `DrvDeep` `computeFSClosure` (`:1855`) is reached, the named `.drv` and its pending input closure flush before the in-eval read. | integration · `lazy-derivations/ifd-forces-flush.sh`; boundary audit · the `realiseContext` arm |
| **LD-S6** | **Build consistency across substitutability changes.** No client-side `willSubstitute` frontier is frozen for local builds: the in-process Worker re-decides substitution while reading pending `.drv`s through `makeLazyDrvStore`, and a demanded pending `.drv` can be materialised on the spot. Remote daemons cannot access that overlay, so the branch ships the cheap `.drv` closure up front, forfeiting local `.drv`-write elision only in that regime. Outputs are never pre-downloaded. | integration · `lazy-derivations/substitutability-flips.sh` (cache state changes between attempts; assert clean realisation, never a crash) + `daemon-build.sh` / `daemon-source-build.sh` for the remote regime |
| **LD-S7** | **Applicative/selective coherence — *the law that justifies elision* `[ENTAILED]` `[LOAD-BEARING]`.** Selective-flush-then-realise yields the same *output* closure as applicative-flush-the-whole-closure-then-realise: eliding the unchosen branch ≡ running it then discarding its `.drv` writes, because a substituted subtree's `.drv` is read by nothing below it (LD-S2). | integration differential · `lazy-derivations/selective-equals-applicative-on-outputs.sh` (build two ways; assert output closures byte-identical via `nix-store --query --hash`, valid-`.drv` sets differ b⊂a). **Pair with LD-S4's loud-failure control** (a frontier bug would match outputs yet crash a goal) |
| **LD-S8** | **Static-dependency over-approximation `[ENTAILED]`.** The statically-named edge set (`inputDrvs.map` populated at eval, `primops.cc:1864`) ⊇ the realisation-demanded set (LD-S2); no un-named dependency is ever demanded — IFD-free guarantees it, and where IFD fires it collapses the dynamic dep into the static set *eagerly* before the outer drv is hashed (the monadic escape, LD-S5). | integration · `lazy-derivations/static-deps-superset.sh` (parse `.drv` `inputDrvs` = named; demanded build/read-back edges ⊆ named). **Negative control:** an IFD expr — under IFD-free enforcement it is rejected/forced eagerly (superset restored) |
| **LD-S9** | **Dry-run/report visibility for deferred `.drv`s.** `queryMissing` is a reporting pass, and without help it treats an invalid-bytes deferred `.drv` as unknown (`misc.cc:207-211`). The branch does not make `queryMissing` the flush driver; it runs dry-run reporting against `makeLazyDrvStore`, so pending `.drv` bytes are readable without writes and the report remains mutation-free. | integration · `lazy-derivations/dry-run-reports-deferred.sh`; meta · report paths use `makeLazyDrvStore` rather than changing `misc.cc` into a materialisation driver |

### Layer 3 — phase laws (LD-V: virtual ⇄ materialised)

| ID | Obligation | Test |
|---|---|---|
| **LD-V1** | **Virtual rendering reads no bytes ([§2.1](#21-the-three-output-path-kinds)).** The returned attrset (drvPath, outPath, per-output strings) is constructible with no `.drv` on disk, for InputAddressed/CAFixed/CAFloating; CAFloating renders a `DownstreamPlaceholder`. | property/unit · `LazyDrv.AttrsetRendersWithoutDrvFile` (per kind) **+ negative control** `ReadingDrvFileMaterialises` (mirrors `VirtualProjection.ReadingDoesMaterialiseBytes`) |
| **LD-V2** | **No new context variant ([§9](#9-non-goals-and-boundaries)).** A virtual `.drv` reuses `DrvDeep`/`Built`; the `NixStringContextElem` variant set is unchanged (avoids the R3 consumer-enumeration hazard). | structural unit · `LazyDrv.ContextVariantSetUnchanged` (`static_assert`/count); negative · enumerate R3 consumers (`AttrDb::setString` et al.) and assert untouched |
| **LD-V3** | **`drvHashes` memo benign-when-cold (V4).** No `build/` caller recomputes `hashDerivationModulo`/`pathDerivationModulo`; build reads `.drv` bytes. A cold memo after a warm eval-cache hit cannot fail a deferred read. | meta/source-grep · `LazyDrv.NoModuloHashCallerInBuild` (the audit surface as a test); integration · warm-eval-cache `nix build` succeeds |
| **LD-V4** | **Failure latches ([§5](#5-soundness-obligations)).** A failed `.drv` write propagates to every later observer via `set_exception`; no observer sees a silently-missing `.drv`. | unit (negative) · `LazyDrv.WriteFailureLatchesToAllObservers` (read-only store dir ⇒ every `waitForPath` throws the same error) |
| **LD-V5** | **read-only invariance ([§5](#5-soundness-obligations)).** Under `readOnlyMode` the attrset is byte-identical to today and nothing flushes. | integration (differential) · `lazy-derivations/read-only-unchanged.sh` (0 store mutations) |

### Layer 0 — semilattice laws (LD-D: monotone store membership)

Materialisation is a join into `(valid store paths, ∪)`. §0.5 folds these into "monotonicity of one projection," but they are **independent axioms** a buggy-but-monotone flush can violate, so each gets its own negative control (matching PROPOSAL.md §3's separate L2/L3 semilattice laws).

| ID | Obligation | Test |
|---|---|---|
| **LD-D1** | **Idempotence** `[ENTAILED]` — *is* LD-P5 (the `derivations.cc:143` bail). Cross-reference, not a separate test. | (LD-P5 `ReflushIsNoOp`) |
| **LD-D2** | **Commutativity** `[ENTAILED]` `[LOAD-BEARING]`. Independent members flush in either order to the same store; dependent members are reference-ordered, not submission-ordered. `processGraph` wires edges by `info.references` (`store-api.cc:229`) and looks members up by **path** (`infosMap.at(path)`, `:221`/`:235`), not submission index; `registerValidPaths`-plural adds all paths (`local-store.cc:960`) before wiring references. | property · `LazyDrv.FlushOrderIndependentForIndependentDrvs` (two independent drvs, both orders, fresh stores ⇒ equal `queryAllValidPaths` + per-path references). **Mutation caught:** wiring references by submission index, not `StorePath` |
| **LD-D3** | **Associativity** `[ENTAILED]`. `{X,Y};{Z}` ≡ `{X};{Y,Z}` ≡ `{X};{Y};{Z}` on the final store; grouping changes round-trip *count*, not *result*. | property · `LazyDrv.BatchGroupingAssociative` (all groupings of a 3-drv closure ⇒ identical store) + a *cost* characterisation asserting round-trip count *differs* (makes §7's perf claim falsifiable, distinct from confluence) |
| **LD-D4** | **Monotonicity / domain-extension** `[ENTAILED]` `[LOAD-BEARING]`. A flush only *adds* keys; no existing valid path's identity mutates (`registerValidPaths:957` `updatePathInfo` idempotent-CA, never deletes; the `!repair` half of `:143` guards existing paths). | property · `LazyDrv.FlushIsMonotone` (snapshot `queryAllValidPaths`, flush, assert superset + every prior path still valid, unchanged hash). **Mutation caught:** an unconditional `repair`-rewrite |
| **LD-D5** | **Cross-process merge — "the deepest memo"** `[ENTAILED]` `[LOAD-BEARING]`. A fresh process re-mints the identical `drvPath` (LD-P4), finds it valid (LD-P5), skips — `isValidPath`-skip *is* `INSERT OR IGNORE`. | integration · `lazy-derivations/cross-process-rematerialisation-free.sh` (A flushes a closure; B, fresh `EvalState`, instantiates the same flake ⇒ writes **zero** `.drv`s). **Negative control:** perturb the store dir between A and B ⇒ different key ⇒ B *does* write |

### Operational coincidences (LD-C: call-by-need over a hash-consed store)

The call-by-need reading (§0.5) is mostly a faithful *re-description* of laws already above — stated so they are not double-counted. Only two are genuinely new:

- **LD-C1 — black-hole-*before*-encode** `[ENTAILED]`. The "already forced?" check (`isValidPath`, `derivations.cc:143`) must *precede* byte production (`addToStoreFromDump`, `:147`), else force is idempotent in *result* but does the work twice. Test `LazyDrv.BlackHoleBeforeEncode` (call-count on `unparse`/dump == 0 on the already-valid path). Sharpens LD-P5 with an *ordering* obligation.
- **LD-C3 — hash-consing of independently-equal drvs** `[NIX]`. Two *independently constructed* equal drvs reach one `drvPath` (LD-P4) and *then* dedup (LD-P6) — maximal sharing; distinct from LD-P6 (two enqueues of the *same* path). Test `LazyDrv.StructurallyEqualShareOnePath`.
- **LD-C2 / C4 / C5** are exact restatements of **LD-P5 / LD-S2 / LD-P4** in heap vocabulary (force-once, demand-stops-at-WHNF, process-independent-thunk-identity) — cross-references, no separate tests.

### Cross-cutting seam laws

Per PROPOSAL.md §3's "four cross-cutting seam laws," these sit at the seam between the framing and the rest of Nix:

- **LD-X4 — pending tolerance / flush-on-read covers every in-eval consumer** `[LOAD-BEARING]`. The eval-time `.drv` consumers are a finite, `git grep`-able set — `forceDerivation` (`eval-cache.cc:799-816`, C2) consumes only the path string and must accept pending `.drv`s; byte consumers (`realiseContext`/`buildPaths`, `DrvDeep` `computeFSClosure`+`readDerivation`, `readInvalidDerivation`) must drain before reading. Path-string renders (`mkOutputString`, LD-V1) are **not** byte consumers. Test: a boundary-audit per site + meta `LazyDrv.ByteConsumerAuditSurfaceComplete`. Same finite-enumerable-consumer shape as PROPOSAL.md's L-NoLeak / §6.2.
- **LD-X1 = LD-V4** (failure-latching across observers), **LD-X2 = LD-V2** (no new context variant — the R3 seam), **LD-X3 = LD-V5** (read-only invariance): promoted in *role* to seam laws; same statements and tests as their LD-V rows.

### Representative test drafts

The full set lives in the homes above; these four fix the idiom — one per category.

**LD-P3/P4 — RapidCheck property** (`src/libstore-tests/lazy-derivation-projection.cc`):

```cpp
#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include "nix/store/derivations.hh"
#include "nix/store/tests/libstore.hh"     // LibStoreTest fixture
#include "nix/store/tests/derivation.hh"   // Arbitrary<Derivation>

namespace nix {

// LD-P3 (soundness/discrimination) + LD-P1 (key purity, no store effect).
RC_GTEST_FIXTURE_PROP(LibStoreTest, LazyDrv_KeyIsContentDetermined, (const Derivation & drv))
{
    auto k1 = computeStorePath(*store, drv);
    auto k2 = computeStorePath(*store, drv);     // second call, same inputs
    RC_ASSERT(k1 == k2);                          // deterministic (LD-P4)
    RC_ASSERT(!store->isValidPath(k1));           // LD-P1: computing the key materialises nothing
}

// LD-P3 discrimination: any field mutation changes the key.
RC_GTEST_FIXTURE_PROP(LibStoreTest, LazyDrv_MutatedFieldChangesKey,
                      (const Derivation & drv, const std::string & k))
{
    RC_PRE(!k.empty() && !drv.env.contains(k));
    auto drv2 = drv; drv2.env.insert_or_assign(k, "x");
    RC_ASSERT(computeStorePath(*store, drv) != computeStorePath(*store, drv2));
}

} // namespace nix
```

**LD-P2 — positive unit** (same file):

```cpp
TEST_F(LibStoreTest, LazyDrv_ComputedPathEqualsWrittenPath)
{
    auto drv     = makeSimpleDerivation();         // fixture helper
    auto minted  = computeStorePath(*store, drv);  // the deferred path
    auto written = store->writeDerivation(drv);    // the eager register
    EXPECT_EQ(minted, written);                    // LD-P2 (lifts the derivations.cc:155 assert)
    EXPECT_TRUE(store->isValidPath(written));
}
```

**LD-V1 — boundary audit / phase law.** The old feature-gated sketch has landed as `LazyDerivationDeferTest.DefersDrvUntilFlushAndRendersAttrset` in `src/libexpr-tests/lazy-derivation-defer.cc`: it drives the real `prim_derivationStrict`, proves `drvPath`/`outPath` render while the `.drv` is not yet valid, then drains `AsyncPathWriter` and reads the materialised `.drv`. The same file covers the fixed-output and floating-CA output arms, and `boundary-audit-derivation-strict.cc` remains the source-placeholder leak audit for persisted derivation fields.

**LD-S2 — functional/integration, the headline** (`tests/functional/lazy-derivations/substitutable-input-elision.sh`), in the log-marker idiom of `known-narhash-defer.sh`:

```bash
source common.sh
# Target T depends on input I. Seed a cache with T's *output* only; build T from
# the cache and assert I's .drv is NEVER materialised locally (the elided branch).

clearStore
drvT=$(nix-instantiate ./elision.nix -A target)
outT=$(nix-store -q "$drvT")
nix copy --to "file://$cacheDir" "$outT"        # push only T's output
clearStore

nix build -L --substituters "file://$cacheDir" ./elision.nix#target   # T substitutes

drvI=$(nix-instantiate --readonly-mode ./elision.nix -A input)        # I's drvPath (pure, LD-P1)
if nix-store --query --valid-paths "$drvI" 2>/dev/null | grep -q "$drvI"; then
    echo "FAIL (LD-S2): input .drv $drvI was materialised though its parent substituted"; exit 1
fi
echo "ok: substitutable-input elision (LD-S2)"
```

```bash
# NEGATIVE CONTROL (…non-substitutable-writes-all.sh): no substituter ⇒ build from
# source ⇒ I's .drv MUST be written (the elision must not over-fire).
clearStore
nix build -L ./elision.nix#target                # no --substituters
drvI=$(nix-instantiate --readonly-mode ./elision.nix -A input)
nix-store --query --valid-paths "$drvI" | grep -q "$drvI" \
    || { echo "FAIL: input .drv not written on a from-source build"; exit 1; }
echo "ok: from-source writes the closure (LD-S2 negative control)"
```

## 12. Implementation status and maintenance checklist

The old implementation plan has been executed on this branch behind the default-off `lazy-derivations` setting. Keep this section as the maintenance map for future changes:

| Area | Branch state | Maintenance rule |
|---|---|---|
| `AsyncPathWriter` substrate | Implemented in `src/libstore/async-path-writer.{hh,cc}`. `addPath` computes the CA `StorePath`, retains bytes, deduplicates by path, and materialises through `waitForPath`, `waitForPaths`, or `waitForAllPaths`; failures latch to all observers. | Preserve LD-P/LD-D laws when changing queue storage, background drain, or bulk submission. The queue is path-keyed; do not reintroduce per-call duplicate entries. |
| Deferred sources | Implemented with `AsyncPathWriter::addSource`. The path is named from the known `narHash`; the copy thunk runs only when a referring `.drv` is materialised. | Source copies and `.drv` writes share the same demand boundary, but not necessarily one store API call. Keep deferred-source tests (`source-copy-elision*`, `source-copied-when-built`, `daemon-source-build`) green. |
| Eval integration | `EvalState` owns the writer; `derivationStrict` computes `drvPath` eagerly and enqueues `.drv`/source writes only when `lazy-derivations` is on and evaluation is not read-only. | Do not drain in `resetFileCache()`: pending `.drv`s are content-addressed and may remain virtual across cache resets. Drain only at observation boundaries. |
| Observation boundaries | Implemented for value output and error paths (`nix eval`, `nix-instantiate`), `realiseContext`/IFD, eval-cache force paths, `.drv` read-back, build, dry-run, and roots/copy-like consumers. | Any new consumer that reads `.drv` bytes must either call `waitForPath(s)`, `waitForAllPaths`, `materialiseDeferred`, or read through `makeLazyDrvStore`. Extend `lazy-derivations/observation-completeness.sh` or the meta audit when adding one. |
| Local build path | `installables.cc` wraps `evalStore` with `makeLazyDrvStore` for in-process builds. The Worker can read pending target `.drv`s without writing them; must-build targets materialise their referential closure. | Keep substitution decisions inside the Worker. Do not replace the overlay with a stale client-side frontier unless the TOCTOU story is re-proven. |
| Remote build path | Remote daemon/SSH builds flush `AsyncPathWriter` before copying the closure, because the daemon cannot see the client's pending queue. | This forfeits local `.drv`-write elision for the remote regime; keep it conservative unless the daemon protocol gains an explicit pending-object channel. |
| Dry-run reporting | `nix build --dry-run` runs `printMissing`/`queryMissing` against a `makeLazyDrvStore` wrapper, so reports can read pending `.drv`s without writes. | `queryMissing` remains a report walk, not a materialisation driver. Do not mutate the store from dry-run classification. |
| Tests | Unit/property tests live in `src/libstore-tests/lazy-derivation-*.cc` and `src/libexpr-tests/lazy-derivation-defer.cc`; functional tests live in `tests/functional/lazy-derivations/`. The `Arbitrary<Derivation>` generator exists in `src/libstore-test-support/include/nix/store/tests/derivation.hh`. | Add tests at the layer touched: projection/queue changes in libstore tests, eval/context changes in libexpr tests, and CLI/build/dry-run behaviour in functional tests. |

The default remains off. The benchmark corrections in A6-A8 are the current policy gate: do not propose flipping the default without workload-specific best-of-N measurements that smoke-test expressions, verify effect counts, and distinguish local eval-store, remote/daemon latency, fully-substitutable targets, and must-build targets.

## Findings ledger

The adversarial pass (REVIEW-DISCIPLINE R1, simulated against master) that this revision incorporates. Each finding names the walk that established it. This ledger is historical: some entries preserve claims later superseded by the implemented `makeLazyDrvStore` path and by the test scaffolding now present on this branch. The current implementation guidance is [§12](#12-implementation-status-and-maintenance-checklist).

| ID | Severity | Finding | Walk |
|---|---|---|---|
| **C1** | Critical (re-narrowed by independent review) | The build path requires the **target** `.drv` valid on disk for any build attempt — but **not** its whole input `.drv` closure unconditionally. A *fully-substitutable* target `co_return`s `Substituted` (`derivation-goal.cc:138`) **before** the resolution/building goals (`:151`/`:229`) that read input `.drv`s ever exist — so it reads **zero** input `.drv`s. The input closure is required only for the subset actually **built from source** (`gaveUpOnSubstitution`, `derivation-building-goal.cc:209-214`, reached only after the per-target substitution attempt fails). So "skip substituted-away `.drv`s" is *false for the target* but *true for substituted-away inputs*; the build-path win is target-`.drv` writes are unavoidable, input-closure writes are elided exactly where the inputs substitute. | `derivation-goal.cc:138` (Substituted co_return) vs `:151`/`:229` (input-drv goals, after) + `derivation-building-goal.cc:209-214` (build-from-source only) + `derivation-trampoline-goal.cc:121-126` (target drv required, `assert(false)` via re-entrant obtain-goal) |
| **C2** | Critical | Missing mandatory boundary: `AttrCursor::forceDerivation` on a **warm eval-cache hit** returns a cached `drvPath` without `derivationStrict`; a deferred (invalid) `.drv` makes it throw `"don't know how to recreate store derivation"`. The branch implements this as pending tolerance, not a real flush: pending `.drv`s are valid-enough for this path-only consumer. Added to §4.2. | `eval-cache.cc:799-815` (`!isValidPath` → `forceValue` → throw, gated `!readOnlyMode`), reached from `installable-flake.cc:92` + `app.cc:129` (the only two `forceDerivation` callers) |
| **C3** | Critical | "Flush as **one atomic transaction** via `addMultipleToStore`" is wrong — it is N parallel per-path transactions (`registerValidPath` *singular*). The atomic two-phase property is in `registerValidPaths` *plural*, which `addMultipleToStore` does not call and which needs bytes already on disk. §4.3 rewritten. | `store-api.cc:195` → `processGraph` → `addToStore(info,source)` (`local-store.cc:1027`) → `registerValidPath` (`:1122`) → one `SQLiteTxn` (`:952`) |
| **M1** | Medium (retracted) | "IFD is the dominant in-eval flush trigger" — **withdrawn**: Nixpkgs forbids IFD (Hydra can't build during eval), so it never fires for the target workload; default-on elsewhere but build-forcing, so a flush is in the noise. Reframed §2/§4.2/§10. | `eval-settings.hh:214` (IFD default-on in Nix) + the Nixpkgs/Hydra policy the user supplied |
| **M2** | Medium | §2 proved the premise on read-only tools, where deferral changes nothing. Reframed around **eval-store mass instantiation** (non-read-only, the case where deferral is live and valuable). (Independent review refined the gate: `readOnlyMode` is `evalOnly && !wantsReadWrite`, and `--dry-run`/`--parse` also set it — but none apply to a plain instantiation, so the conclusion holds.) | `nix-instantiate.cc:183-184` (`evalOnly && !wantsReadWrite`) + `common-eval-args.cc:145` (`--eval-store` writes `.drv`s) |
| **F** (survives) | — | Deferred **source-copy** soundness holds: a build copies only `inputSrcs` valid in `evalStore`, re-checking store validity, so a source need only be flushed at the same boundary as the `.drv`. | `derivation-building-goal.cc:154-161` |
| **S1** | Structural (round-4) | "Two schedulers" was a mis-decomposition. The source scheduler memoises an expensive narHash *walk*; a `.drv` has no walk (`drvPath = hash(ATerm)` eagerly), so its side is a *write-queue*, not a scheduler. Collapsed §4 to one barrier + two front-ends (source scheduler unchanged; `.drv` write-queue), one reference-ordered flush. | `materialisation-scheduler.hh:99,117` (walk-coalescing machinery) vs. `drvPath` eager hash ([§2.2](#22-intrinsic-vs-deferrable)) |
| **B** | Correction (self-review) | `derivationStrict` runs once per call site, but two sites can yield one `drvPath`; DetSys's `addPath` `push_back`s unconditionally (`async-path-writer.cc:92-103`), queueing it twice. The `drvPath`-keyed `pendingDrvs_` map dedups the enqueue — an *improvement over* DetSys, not parity. §4.1. | `async-path-writer.cc:92-103` (unconditional `push_back`) + `derivation.nix:36` (`strict = derivationStrict drvAttrs`, force-once) |
| **C** (idempotence) | Confirmed | Re-flush is safe: the worker `std::swap`s `items` out before writing (`async-path-writer.cc:52`); `waitForAllPaths` `std::move`s `futures` out (`:125`). Processed entries leave the queue. Stated as fact in §4.1. | `async-path-writer.cc:52,125` |
| **D** | Caveat (self-review) | "One `addMultipleToStore` for sources *and* `.drv`s" overclaims: that API needs `narHash` + `narSize` pre-known, but a source's `narSize` needs the walk its ingestion (`addToStore(name, path, …)`) performs. The *boundary* unifies; the *call* need not. Sources may keep `fetchToStore(Copy)`. §6. | `fetch-to-store.cc:400` (path-overload walks) + `store-api.cc:660,665` (`PathsSource` = `ValidPathInfo` + `Source`) |
| **V1** | Independent review (round-5) | C1's round-2/4 "build needs the **whole** `.drv` closure valid" was an **over-correction**. A fully-substitutable target returns `Substituted` (`derivation-goal.cc:138`) before any input-`.drv` goal exists, reading zero input `.drv`s — so deferral *does* elide substituted-away input-closure writes. Only the **target** `.drv` of each built output is unavoidable. C1 row + §7 + §8 + banner re-narrowed. | four parallel reviewers, each verified against master by the delegator (R9): `derivation-goal.cc:138` vs `:151`/`:229` |
| **V2** | Independent review (round-5) | §10-Q3 (per-item narHash "is it free?") **resolved**: `addToStoreFromDump` already NAR-hashes any non-`NixArchive`/SHA256 object, and a `.drv` is `Raw::Text`/`Flat`, so that branch already fires on every `.drv` write today. The batched narHash is the *same* pass, not net-new. | `local-store.cc:1271-1274` |
| **V3** | Independent review (round-5) | Citation fixes against master: `isValidPath` bail is `derivations.cc:143` (not `:151`); `drvHashes` `insert_or_assign` is `:875`/`primops.cc:2039` (`:858` is the declaration); member `writeDerivation` is `store-api.hh:956` (`derivations.hh:602` is the unrelated serialiser); `forceDerivation` callers are `installable-flake.cc:92` + `app.cc:129` (not `installable-attr-path.cc:93`); `waitForAllPaths` move is `async-path-writer.cc:125`. All applied. | direct re-walk of each cited line |
| **V4** | Independent review (round-5) | §5 `drvHashes` discharge had a **hole**: "A instantiated before B" fails on a warm eval-cache hit (neither B nor A instantiated → cold sub-closure). Benign because build goals read `.drv` *bytes* (`readDerivation`), never recompute `hashDerivationModulo`. §5 rewritten to rest on the build-time bytes boundary, not instantiation order. | `derivation-trampoline-goal.cc:124` (bytes read) + grep: no `hashDerivationModulo` caller in `build/` |
| **A1** | Algebraic framing (round-6, this pass) | `.drv` materialisation placed as a content-addressed **selective task** ([§0.5](#05-the-algebra)): the pending closure is the applicative skeleton, `queryMissing` (`misc.cc:294-301`) is the *shape* of the `select` (it computes the `willBuild`/`willSubstitute` partition), substitutable subtrees are unchosen branches whose input-closure writes are elided, IFD is the monadic escape. **Resolves the §4.2-vs-§7 tension**: [§4.2](#42-resolution-boundaries) item 1 was an *applicative* mis-grading ("flush the *transitive* closure" — would write everything, elide nothing), corrected to the selective frontier. At this historical pass, the design still expected a client-side frontier pass; the branch later superseded that with `makeLazyDrvStore` for local builds and dry-run reports. Adds the LD-P/S/V law inventory + test plan ([§11](#11-laws-and-test-plan)). | `derivation-goal.cc:138` (Substituted) vs `:151`/`:229` (input-drv goals, re-verified on execution order) + `misc.cc:207-301` (`queryMissing` select/elide frontier) + `derivation-building-goal.cc:211-213` |
| **A2** | Citation re-walk (this pass, R9) | Independent re-walk on a fresh checkout against master (`~/Packages/nix`@`99a5f4fac`) **and DetSys (`~/Packages/nix-src`@`35cd98d10`, the pinned SHA)**: all load-bearing claims hold — C1/V1 (execution order), V2 (`local-store.cc:1271-1274`), V4 (no `hashDerivationModulo` in `build/`), §2.1 rendering (`derivations.cc:21` + `eval.cc:1043,1053`). Citation fixes: `async-path-writer.cc:124`→`:125` in §4.1 (the `std::move`; V3 said `:125` but the §4.1 body lagged); `primops.cc` `insert_or_assign` `:2038`→`:2039` (`:2038` is the `hashDerivationModulo` call, `:2039` the memoise). | direct re-walk of each cited line in both trees |
| **A3** | Self-adversarial pass (round-7, this revision; R7/R8) | Attacked the round-6 additions ([§0.5](#05-the-algebra)/[§11](#11-laws-and-test-plan)) themselves. Found+fixed: (i) **mechanism overstated** — §0.5/§4.2/A1 implied `queryMissing` is *wired* as the build-path `select`/flush driver; against source it is **display-only and gated** (`printMissing`→`queryMissing`, `shared.cc:56-72`; `installables.cc:652` `if (settings.printMissing)`; `build.cc:142-154` `--dry-run` only), and the elision is performed by the build **goals**. The next design step at that time was a client-side frontier pass; the implemented branch instead uses `makeLazyDrvStore` so the Worker can read pending `.drv`s directly. (ii) **LD-S2 formula** omitted `targets` (a substitutable target's own `.drv` is read by its trampoline, `:124`) — fixed to `targets ∪ willBuild ∪ {immediate inputDrvs of willBuild}`; (iii) **missed TOCTOU** between the submission frontier and the goals' build-time substitutability re-decision → new obligation LD-S6, later discharged by the overlay; (iv) **stale §10 item 1** still stated the round-2/4 "win is false / whole closure before substitution" that V1 overturned — realigned to V1 (the same incomplete fold-in that left §4.2 item 1 stale). | `shared.cc:56-72` + `installables.cc:652,655` + `build.cc:142-154` (queryMissing = gated display; build is `buildPathsWithResults`) + `derivation-trampoline-goal.cc:124` (target always read) + `derivation-goal.cc:138` vs `:151`/`:229` |
| **A4** | Delegated law-research + R9 verification (this revision) | A background subagent enumerated the full law inventory across the four §0.5 pillars; **each load-bearing claim was re-walked against master before folding** (R9), not trusted. Verified+added: LD-P7 (`computeP` triviality, `derivations.cc:116`), LD-P8 (reference-set fidelity, `:109-111`), LD-S7/S8/S9 (selective coherence / static-deps superset / `queryMissing` partition), a **three**-reader LD-S4 (third = `derivation-resolution-goal.cc:59`, gated `impure-derivations` — the draft listed two), and the LD-D2–D5 semilattice expansion (`store-api.cc:221`,`:229`; `local-store.cc:957`,`:960`,`:974`). Corrected: the LD-V1 boundary-audit draft asserted `EXPECT_FALSE(isValidPath)`, which **fails on master** (eager write — `boundary-audit-derivation-strict.cc:34` reads the drv back) → re-marked feature-gated. At that point `Arbitrary<Derivation>` and the pending-queue laws were not yet implemented; on this branch they now live in the test-support and lazy-derivation test files named in §11/§12. | `derivation-resolution-goal.cc:57-59` + `boundary-audit-derivation-strict.cc:34` + `store-api.cc:221,229` + `local-store.cc:952-974` (each re-walked) |
| **A5** | Full-document adversarial re-read (this revision; R1) | An end-to-end re-read caught a **`readOnlyMode` contradiction** the round-6 §4.2 edit had half-created: §2 and the §7 table treated `nix eval` (and `nix build --dry-run`) as *read-only / 0 writes today*, but **neither sets `readOnlyMode`** (verified against the full setter set; `nix eval` has none, and `build.cc:142-154` returns before `Installable::build`), so **both write `.drv`s eagerly today** — `nix eval` via `derivationStrict` (`primops.cc:2029`), `--dry-run` via `toDerivedPaths` instantiation. Fixed §2's command list + citations (real setters: `repl.cc:582`/`search.cc:65`/`nix-env.cc:990`/`nix-instantiate.cc:184`/`log.cc:29`; `installables.cc:235` is the opt-in `--read-only` flag), split the §7 dry-run/`nix eval` row (dry-run *elides* via the `pendingDrvs_`-aware `queryMissing`; `nix eval` *defers to the output boundary* — same write, not elided), and aligned §7's stale `:121-126`/`:209-214` cites to `:122-125`/`:211-213`. Also updated the status banner to record A3–A5. | `grep "readOnlyMode = true" src/` (full setter set) + `build.cc:142-154` (dry-run returns pre-build) + `eval.cc` run() (no setter) + `primops.cc:2029` |
| **A6** | Independent benchmark + code-walk (2026-06-03) — **refines V1/C1** | V1's "input closures elide where they substitute" holds **only for a substitutable *target***, not a substitutable *input* of a must-build target. A built target reaches `gaveUpOnSubstitution` → `materialiseDeferred(targetDrv)` → `AsyncPathWriter::materialise`, which walks the target `.drv`'s **transitive pending references** (its `inputDrvs` *and* `inputSrcs`) and writes the **whole input closure** — referential integrity requires it (a registered `.drv`'s `inputDrv` references must be valid on disk). So a node whose *output* substitutes but which is an `inputDrv` of a must-build node is **NOT** elided: its `.drv` + `inputSrcs` materialise as part of the must-build target's referential closure; only the substituted node's *output build* is skipped. **Empirical:** chain p0←…←p5, cache ONLY p2's *output*, build p5 → **all 6 `.drv`s + all 6 sources materialised** (p2 substituted, p3/p4/p5 built, only p0/p1 *outputs* absent). So elision is **all-or-nothing at the requested target**: fully-substitutable target ⇒ whole closure elided; must-build target ⇒ whole input closure materialised, incl. substitutable sub-nodes. The realistic monorepo (build your code on cached deps) is the must-build case ⇒ **~zero input-closure/source elision**. (Input-addressed only; CA `resolve` rewrites `inputDrv`→output refs and could elide — untested.) Qualifies §7 "substitutable build" row + §0.5 "the unchosen branch's effects never run": the *build graph* is selective, but a built target's *`.drv` reference closure* is not. | `async-path-writer.cc:329-403` (`materialise` walks transitive *pending* refs; black-hole `isValidPath` `:367`) + `gaveUpOnSubstitution`→`materialiseDeferred` + empirical DAG store-inspection (cache p2, build p5 → 6 `.drv`s/6 srcs) |
| **A7** | Benchmark correction (self-adversarial, 2026-06-03) — **prior multipliers were n=1 and several were WRONG** | Re-measured best-of-N with smoke-tested exprs, per-run **effect-count verification**, and silent-build-failure guards. (a) **Elision** (substitutable target, 8 sources, best-of-5, verified eager-copies-N/lazy-0, **page-cache-WARM**): **1.4–1.7×** (was "~5×"/"~2×"), Δ grows with size but **sub-linearly** — lazy still pays the *intrinsic narHash walk* (`drvPath` needs it), eliding only the *copy*; both pay the walk. Cold-disk would be larger but `drop_caches` needs root (unmeasurable here); substitutable-**target**-only (A6). (b) **Populate** (small-N **local** `nix eval` of N `drvPath`s, best-of-5): lazy **~12% SLOWER** — deferral overhead, can't defer the intrinsic walk, writes everything anyway → a slight **loss**, not a win. (Workload-dependent: on the eval-heavy ~16k-`.drv` closure case the async overlap makes lazy ~neutral-to-7%-faster; and this is the LOCAL eval-store — the §7 mass-instantiation headline is the *daemon* round-trip win, (c) below, not this local case.) (c) **Daemon latency** (best-of-3, `unix://` proxy injecting per-request delay): the win **GROWS with latency** — **1.1× at 0 ms, 1.8× at 5 ms** — NOT the "~2× latency-independent" first reported (the L=0 "2.2×" was an n=1 artifact); request-bursts halved (~800→~407), the floor being the un-coalesced per-`.drv` `isValidPath` in the flush walk. **Methodology (now mandatory):** a compacted nix path expr `./.+(x)` parses as the **path literal** `./.+`, not `./. + (x)` → both modes fail-fast on a parse error, and a count-blind harness reads "neutral" — I nearly **retracted the real elision win from broken runs**. So: smoke-test the expr parses, verify the effect-count per run, best-of-N with success+count guards, treat page-cache + this env's intermittent silent `nix build` failures as confounds. The three `doc/tecnix-survey/bench-*.sh` check drv-count-**match** across modes (catch closure-divergence) but **not all-modes-fail**, and are page-cache-warm; `bench-elision.sh`'s header "~5×" was the stale n=1 number (corrected in-file). | best-of-N re-runs with verified counts + `async-path-writer.cc:367` (per-path `isValidPath`) vs `:475` (one batched `addMultipleToStore`) |
| **A8** | Medium impl bug (error-path) — **FIXED** (2026-06-03) | `nix eval` left **fewer `.drv`s than eager** when evaluation throws (violating "observably eager"). The failing `--expr` is evaluated in `SourceExprCommand::parseInstallables` (the pre-run `state->eval`) — **before** `CmdEval::run`'s body — so a `run()`-local barrier can't cover it (read the call chain: `InstallableCommand::run`→`parseInstallable`→`parseInstallables`, not `CmdEval::run`'s `toValue`). Fixed at **both** throw sites: **(#1)** try/catch around the parseInstallables eval → `asyncPathWriter->waitForAllPaths()` then rethrow (generic over `SourceExprCommand`s, no-op in eager mode so the suite is unaffected, swallows flush errors via `ignoreExceptionExceptInterrupt`; sound because an eval-throw ⇒ no successful build ⇒ can't defeat success-path elision); **(#2)** a `Finally` in `CmdEval::run` for in-run deep-force throws (e.g. `--json {a=drv; b=throw}`), replacing the old explicit end-barrier. Verified eager==lazy for the `.drv` **count, the original error message, the exit code, and N>1**; non-vacuous with the drain off (`_NIX_LAZY_DRV_NO_OVERLAP=1`). Gate `Ok: 217, Fail: 0`. | `installables.cc` `parseInstallables` (pre-run `state->eval`) + `command-installable-value.cc:5-9` (forwards to `run`) + `eval.cc` `CmdEval::run` (`flushDeferred` Finally) + verified runs |

Net: the **write-deferral + demand-boundary materialisation** core is sound; **eval-store mass-instantiation *populate* is neutral-to-slightly-slower locally** (A7: ~12% on a small `drvPath` eval — deferral overhead with nothing to elide), and the clean wins are **source-elision for fully-substitutable targets** (A6/A7: 1.4–1.7× warm, target-only — a must-build target materialises its whole input closure) and **daemon round-trip coalescing** (A7: grows with latency, ~1.1×→1.8×); the **atomic-transaction** claim (C3) was corrected; the **skip-substituted-closure** claim swung from over-stated (round-1) to over-retracted (round-2/4) to its true target-level scope; the **"two schedulers"** structure was collapsed to **one demand boundary + two path-discovery front-ends** (S1); memoisation/idempotence are explicit (B/C); the bulk-API unification is scoped honestly (D); the narHash fix is confirmed free for the remote bulk route (V2); and the §5 soundness argument was moved off the instantiation-order claim that the warm-cache path violates onto the build-time `.drv`-bytes boundary (V4). The four parallel reviewers' load-bearing findings were each re-verified against master by the delegator (R9), not trusted as reported. A later self-adversarial **benchmark** pass (A6–A8, 2026-06-03) then corrected the *performance* claims to best-of-N with **verified effect-counts** — elision **1.4–1.7×** (warm, fully-substitutable-target only), populate **~12% slower**, daemon coalescing **1.1×→1.8×** (growing with latency, not flat) — **refined the substituted-input elision to target-only** (a must-build target materialises its *whole* input `.drv` closure by referential integrity, A6), and fixed the `nix eval` error-path `.drv`-undercount (A8). The recurring fault was **benchmark discipline** (n=1, a path-expr parse trap that nearly forced a false retraction, page-cache + flaky-build confounds), not the implementation.
