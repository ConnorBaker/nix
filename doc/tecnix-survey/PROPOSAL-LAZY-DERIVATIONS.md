# Virtual Derivations: Deferred, Batched `.drv` Materialisation

This document describes a design for making `.drv` writing **lazy and batched**, by the same content-addressed placeholder pattern the source-materialisation architecture already applies to source trees ([PROPOSAL.md §0](./PROPOSAL.md#0-underlying-pattern)). The thesis in one line: **a derivation is the output of an evaluator-level "build" — the evaluator computes its identity (`drvPath`) eagerly from in-memory data, threads that identity through evaluation, and materialises the `.drv` file (and its as-yet-unmaterialised source inputs) only at a resolution boundary, in one bulk flush.**

It is written in the timeless component/interplay style of [PROPOSAL.md](./PROPOSAL.md): what each piece does and how the pieces interplay, with `file:line` citations into the tree as a reading aid. Every "today X happens / after the change Y happens" claim is a *simulated* walk of the executing code path, per [REVIEW-DISCIPLINE.md](./REVIEW-DISCIPLINE.md) R1 — not a citation that a function exists.

> **Status: adversarially reviewed over five rounds (REVIEW-DISCIPLINE R1, simulated against master), including self-review and a final round of four parallel *independent* reviewers whose load-bearing findings were each re-verified against master by the delegator (R9).** The review reshaped the design materially — see the [Findings ledger](#findings-ledger) for what was retracted, corrected, or restructured (the independent round is V1–V4). In one line: the **write-deferral** half (async, DetSys-style) is sound; the **"one atomic transaction"** framing was wrong (the cited `addMultipleToStore` is N per-path transactions, [§4.3](#43-the-batched-store-flush)); the **"skip materialising substituted-away closures"** win is *partial* — the **target** `.drv` of any built output is unavoidable, but a fully-substitutable target reads **none** of its input `.drv`s, so the input-closure writes *are* elided where the inputs substitute ([§7](#7-performance-characteristics), C1); the real, clean win is **eval-store mass instantiation** (Hydra/CI: non-read-only, IFD-free, build-free — [§2](#2-the-unblock-primitive)), not a genuinely read-only command like `repl`/`nix search` (`nix eval` is itself **non-read-only** — it writes the `.drv` today, [§7](#7-performance-characteristics)); and the **"two schedulers"** structure was collapsed to **one flush barrier + two path-discovery front-ends** (the `.drv` side is a write-queue, not a scheduler — [§4](#4-the-design)), with memoisation made explicit ([§4.1](#41-the-drv-write-queue)). A sixth pass (this revision) re-walked the load-bearing claims against master (`~/Packages/nix`@`99a5f4fac`) and DetSys (`~/Packages/nix-src`@`35cd98d10`) and added the **selective-task** algebraic framing ([§0.5](#05-the-algebra)) with a law/test inventory ([§11](#11-laws-and-test-plan)); the framing corrected one *applicative* mis-grading of the build boundary ([§4.2](#42-resolution-boundaries) item 1) — *batched flush and substitutable-input elision are not alternatives; the selective grade is their conjunction* (ledger A1) — and minor citations (A2). Subsequent passes (ledger A3–A5) corrected the round-6 *overstatement* that `queryMissing` is a wired flush-driver (it is **display-only**; the elision is performed Worker-side by the build goals, and the client must *add* a `pendingDrvs_`-aware pre-pass), expanded the [§11](#11-laws-and-test-plan) law inventory (LD-P7/P8, S7–S9, the LD-D2–D5 semilattice laws, the three-reader LD-S4) with each claim re-walked against master (R9), and fixed a `readOnlyMode` error: **`nix eval` and `nix build --dry-run` are non-read-only and write `.drv`s today** ([§2](#2-the-unblock-primitive), [§7](#7-performance-characteristics)).

It builds on three precedents already in the tree or in the surveyed forks, and is explicit about which mechanism it borrows from each:

- the **source** `SourceVirtual` placeholder + `MaterialisationScheduler` + batched `outPathsOf` chokepoint ([PROPOSAL.md §2.O, §6.2](./PROPOSAL.md#o-sourceplaceholder--materialisationscheduler--content-keyed-memo)) — this design shares its *resolution-at-a-boundary* shape and reuses the source scheduler unchanged, but the `.drv` side is a write-queue, not a second scheduler ([§4](#4-the-design));
- upstream's **`DownstreamPlaceholder` + `Derivation::tryResolve`** (`downstream-placeholder.hh`, `derivations.cc:1151`) — the existing proof that a derivation's *output* path can be unknown at eval time and resolved at a boundary;
- DetSys's **`AsyncPathWriter`** (`nix-src/src/libstore/async-path-writer.{hh,cc}`) — the existing proof that the `.drv` *write* can be moved off the eval thread, **and** (in its disabled `#if 0` block) the first attempt at the bulk `addMultipleToStore` flush this design completes.

---

## Contents

- [0. The pattern: a derivation is an evaluator-level build output](#0-the-pattern)
- [0.5. The algebra: `.drv` materialisation as a content-addressed selective task](#05-the-algebra)
- [1. What `derivationStrict` does today (simulated)](#1-what-derivationstrict-does-today)
- [2. The unblock primitive: return the attrset without materialising](#2-the-unblock-primitive)
  - [2.1. The three output-path kinds, and why none needs the `.drv` on disk](#21-the-three-output-path-kinds)
  - [2.2. What is intrinsic to `drvPath` vs what is a deferrable side-effect](#22-intrinsic-vs-deferrable)
- [3. DetSys `AsyncPathWriter`: what it does and where it stops](#3-detsys-asyncpathwriter)
- [4. The design](#4-the-design)
  - [4.1. The `.drv` write-queue (and its memoisation)](#41-the-drv-write-queue)
  - [4.2. Resolution boundaries (where a `.drv` must be on disk)](#42-resolution-boundaries)
  - [4.3. The batched store flush + the narHash friction fix](#43-the-batched-store-flush)
- [5. Soundness obligations](#5-soundness-obligations)
- [6. Interaction with the lazy-source substrate](#6-interaction-with-the-lazy-source-substrate)
- [7. Performance characteristics and the workload-shape risk](#7-performance-characteristics)
- [8. Comparison: async vs batched vs lazy](#8-comparison)
- [9. Non-goals and boundaries](#9-non-goals-and-boundaries)
- [10. Open questions and review obligations](#10-open-questions-and-review-obligations)
- [11. Laws and test plan](#11-laws-and-test-plan)
- [12. Implementation plan](#12-implementation-plan)
- [Findings ledger (adversarial review)](#findings-ledger)

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
| **`select : f (Either a b) → f (a → b) → f b`** — run the second effect *only* for the `Left` branch | `queryMissing` (`misc.cc:294-301`): per node, query substituters; **`Right`** (all outputs substitutable) → recurse into output *references*, **not** `inputDrvs`; **`Left`** (must build) → `mustBuildDrv` recurses into `inputDrvs` (`:187-188`) |
| **the unchosen branch's effects never run** | a substitutable subtree's input-closure `.drv` writes and source copies are *never materialised* — the [§7](#7-performance-characteristics) elision |
| **static over-approximation** — you may *list* all potential dependencies without *running* them | eval names every `drvPath` (the over-approx); the flush runs only the demanded frontier — this duality **is** "the batch is known eagerly, the writes are chosen by demand" |
| **monadic escape** — a `bind` whose continuation's dependencies come from a built value | IFD / `realiseContext` (`primops.cc:172`) and the `DrvDeep` `computeFSClosure` (`:1855`): materialisation is *forced* mid-eval; rare for the target workload (Nixpkgs forbids IFD), build-forcing where it fires |

The defining property of a selective functor is precisely *"extract the dependency structure statically, choose which branches to execute dynamically."* Read as a sentence about this design it says: **one content-addressed batch is determined eagerly (applicative skeleton, order-free — [§4.3](#43-the-batched-store-flush)), while which `.drv`s are written is chosen by demand (the `select`, eliding substitutable subtrees — [§7](#7-performance-characteristics)).** The "bulk-flush-the-closure *or* demand-driven-elision" dichotomy was a false choice between the *applicative* and *monadic* grades; **selective is the name for their conjunction**, and `queryMissing` is the in-tree `select`. So the build boundary ([§4.2](#42-resolution-boundaries) item 1) runs the *selective* interpreter over the pending skeleton — flushing only the frontier — not an applicative flush of the whole closure.

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
| thunk (a suspended computation, named before it is forced) | a pending `.drv`: `drvPath` known, bytes pending — held in `pendingDrvs_` |
| the thunk's address | `drvPath` — but **content-addressed**, so stable across processes (a heap pointer is not — REVIEW-DISCIPLINE R2) |
| `force` to WHNF | flush: run `computeP`/`encodeO`, write the bytes |
| black-holing / "already forced?" | `isValidPath` (the `writeDerivation` bail, `derivations.cc:143`) |
| hash-consing / maximal sharing | content-addressing: two structurally-equal `drv`s **are** one heap cell (`pendingDrvs_` keyed by `drvPath` dedups the enqueue — Finding [B](#findings-ledger)) |
| demand analysis (force only what's needed; stop at already-WHNF values) | `queryMissing`: descend from the targets, **stop at substitutable nodes** (already-realisable = already-WHNF) — the input closure beneath them is never forced |
| spine-strict bulk force at a boundary | the batched flush ([§4.3](#43-the-batched-store-flush)) |

So the design is **call-by-need lifted from values to store objects**, and content-addressing is what upgrades a thunk's identity from a process-local pointer to a process-independent name — precisely why a second process cache-hits (R2) and why the placeholder is safe in error messages and lockfiles. Substitutable-subtree elision is "don't force a thunk whose value is already available"; the batch is "force the demanded spine in one pass." The selective grade is the *type-level* statement of the same thing: WHNF-demand that is statically analysable because (IFD-free) no forced value can introduce a new dependency.

### What the framing resolves

- **[§4.2](#42-resolution-boundaries) item 1 vs [§7](#7-performance-characteristics) — what performs the elision, and where.** The elision is performed by the build **goals**, Worker-side: `haveDerivation` substitutes a target's outputs and `co_return`s (`derivation-goal.cc:138`) *before* creating the goals that read input `.drv`s (C1/V1), so a substitutable subtree's input closure is never demanded. The *selective* reading names why this is sound and statically batchable; it does **not** mean a `select` is already wired at submission. The goals run in the Worker and cannot reach `EvalState::pendingDrvs_`, so the deferred-`.drv` flush must happen **client-side, before `buildPathsWithResults`** (`installables.cc:655`) and must pre-compute the demanded frontier. `queryMissing` is the existing *shape* of that pass (a client-side, substitution-aware closure walk producing `willBuild`/`willSubstitute`) — but **today it is display-only**: `printMissing → queryMissing` (`shared.cc:56-72`) flushes nothing, and is gated on `settings.printMissing` (`installables.cc:652`) or `--dry-run` (`build.cc:142-154`). So the design must *promote* a `queryMissing`-shaped, `pendingDrvs_`-aware pass from display-gated to a mandatory submission-time flush driver — not free, not already-wired. Item 1's "flush the *transitive* closure" was an *applicative* mis-grading; the fix is "flush the *selective frontier*," but the frontier must be **computed by a pass the design adds** ([ledger](#findings-ledger) A1, A3).
- **[§10](#10-open-questions-and-review-obligations) residue 1 (the `misc.cc:208` FIXME) is that pass's enabling change — with a new obligation.** Teaching `queryMissing` to read the pending queue instead of bailing into `res.unknown` on a deferred (invalid) `.drv` is what lets the frontier walk see deferred `.drv`s at all — the lever, not a separate enhancement. **But (LD-S6):** the frontier is computed from substitutability *at submission* while the goals **re-decide** it *at realisation*; if a `willSubstitute` frontier node flips to must-build (cache eviction) in between, its elided input closure is demanded and a Worker goal trips `assert(false)`. So "have both" is demand-driven elision *with a frontier↔build consistency obligation*, not a free lunch — [§11](#11-laws-and-test-plan) LD-S6 states it; mitigations (pre-substitute `willSubstitute` outputs before building, or a conservative non-eliding flush) are an open design choice ([§10](#10-open-questions-and-review-obligations)).
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
- **build-free** → the instantiation pass builds nothing, so the build path's "target `.drv` must be on disk to build it" constraint ([§7](#7-performance-characteristics), Finding C1) never fires at all.

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
2. **The batched store transaction is written and disabled.** `async-path-writer.cc:134-156` is a `#if 0` block that builds a `Store::PathsSource` and calls `store->addMultipleToStore(sources, act, repair)` — exactly the bulk path — guarded by the comment `// FIXME: addMultipleToStore() shouldn't require a NAR hash.` This design's [§4.3](#43-the-batched-store-flush) is, in essence, *that block, with the FIXME resolved.*

## 4. The design

**One flush barrier, two path-discovery front-ends — not two schedulers.** An earlier draft proposed a `DerivationMaterialisationScheduler` as a peer of the source `MaterialisationScheduler`. That was a mis-decomposition (it cargo-culted the source scheduler's *shape* onto a problem that doesn't have the source scheduler's *job*). The two halves answer different questions:

- *"How do I learn my store path?"* — **different.** A **source** doesn't know its path until an expensive **narHash walk** runs; the source `MaterialisationScheduler` exists entirely to memoise and coalesce *that walk* (`narHashByContent_`, the `inFlight_` `shared_future`, `outPathsOf` batching, `SourceContentId`'s N-calls-→-1-identity). A **`.drv`** knows its path **immediately** — `hash(ATerm)`, over bytes in hand, computed eagerly to fill `drvPath` ([§2.2](#22-intrinsic-vs-deferrable)). There is no walk to coalesce and no content-keyed identity to share (two identical drvs already *are* one `drvPath`). So the `.drv` side needs **none** of the scheduler's machinery — it is a **write-back queue** (which is exactly what DetSys named `AsyncPathWriter`, not a scheduler).
- *"When/how do I flush to the store?"* — **identical, and coupled.** Both defer a content-addressed store write to the same boundary; both are `EvalState`-resident and cleared on `resetFileCache`; and crucially **they share one reference DAG** — a `.drv` references its sources via `inputSrcs`, so a single reference-ordered bulk submission topologically orders sources-and-drvs *together* in one pass.

So the design is **(a)** the existing source scheduler, unchanged ([§4.1](#41-the-drv-write-queue) leaves it alone); **(b)** a thin `.drv` write-queue ([§4.1](#41-the-drv-write-queue)); and **(c)** one unified, reference-ordered flush both feed ([§4.3](#43-the-batched-store-flush)). The "two schedulers" framing is retired; the only thing that is genuinely two is the *map* of pending entries (one keyed by `SourceContentId`, one by `drvPath`), because the keys differ — not the flush, and not the orchestration.

### 4.1. The `.drv` write-queue

`prim_derivationStrict` (step 5 of [§1](#1-what-derivationstrict-does-today)) stops calling `writeDerivation` unconditionally. Instead:

- compute `drvPath` in-memory via `computeStorePath` (always — the read-only branch generalised to all modes);
- enqueue the pair `(drvPath, drv)` on a per-`EvalState` **`.drv` write-queue** — the ported `AsyncPathWriter` ([§3](#3-detsys-asyncpathwriter)), holding `pendingDrvs_ : concurrent_flat_map<StorePath, BasicDerivation>` keyed by the (already content-determined) `drvPath`;
- populate `drvHashes` exactly as today (`primops.cc:2038-2039`) — **unchanged and load-bearing** ([§5](#5-soundness-obligations));
- build and return the attrset exactly as today.

**Memoisation — at three existing layers, plus one deliberate keying choice.** The design does not re-attempt work already done. Dedup is not a new mechanism; it falls out of layers already in the tree:

1. **`drvPath`-keyed enqueue (the one keying choice this adds).** `derivationStrict` is forced once per *textually distinct* call site (the arg thunk is a force-once `Value`), but two *different* call sites can produce the same `drvPath` (the same derivation reached two ways). Keying `pendingDrvs_` on `drvPath` makes the second enqueue a no-op. This is a small *improvement over DetSys*, whose `AsyncPathWriter::addPath` does `insert_or_assign` on the future but an **unconditional** `push_back` of the item (`async-path-writer.cc:92-103`) — so it would queue the same `.drv` twice and write it twice (the second write harmless but wasted). The map key removes the duplicate before it reaches the queue.
2. **`isValidPath` skip at write time.** Even an un-deduped enqueue is cheap: `writeDerivation` bails on `isValidPath` (`derivations.cc:143`, `if (isValidPath(path) && !repair) return path;`), and the bulk `addMultipleToStore`'s `processGraph` skips any already-valid node (`store-api.cc`). So a `.drv` already on disk (this process or a prior one) is never rewritten.
3. **`drvHashes` content-hash memo.** `hashDerivationModulo(drvPath)` is memoised in a `concurrent_flat_map` (`drvHashes`, declared `derivations.cc:858`; the idempotent `insert_or_assign` is at `derivations.cc:875` in `pathDerivationModulo` and `primops.cc:2039` at instantiation) so the recursive modulo-hash is computed once per `drvPath` per process ([§5](#5-soundness-obligations)).

The deepest memo is content-addressing itself: a fresh process that re-evaluates the same derivation computes the identical `drvPath`, finds it already valid, and skips — cross-process re-materialisation is free.

**Idempotent re-flush.** Flushing is not all-or-nothing at end-of-eval; it fires at every boundary ([§4.2](#42-resolution-boundaries)). Re-flush must not re-submit already-written entries — and the `AsyncPathWriter` harness gives this directly: the worker `std::swap`s the pending `items` vector out before writing (`async-path-writer.cc:52`), and `waitForAllPaths` `std::move`s the `futures` map out (`:125`), so a processed entry has left both structures. A second flush sees only what was enqueued since the first. (Verified by reading the harness, not assumed.)

A `VirtualDerivation` is not a new value type or a new `NixStringContextElem` variant — that is a deliberate non-goal ([§9](#9-non-goals-and-boundaries), and the [REVIEW-DISCIPLINE.md](./REVIEW-DISCIPLINE.md) R3 hazard a new context variant would incur). The existing `DrvDeep{drvPath}` and `Built{drvPath, output}` context elements already carry the `drvPath`; "virtual" is purely a *store-side* property — the path is named but its bytes are pending — exactly as a source `Opaque{stand-in}` is named but pending ([PROPOSAL.md §6.1](./PROPOSAL.md#61-input-materialisation)).

### 4.2. Resolution boundaries

A pending `.drv` must be flushed to disk before any operation that reads it back **as a file**. Enumerated by simulation (R1) — every site that needs the bytes, not just the path string:

1. **Build (the selective frontier, not the whole closure — Finding C1/V1, [§0.5](#05-the-algebra)).** A build attempt *always* requires the **target** `.drv` valid on disk: `DerivationTrampolineGoal` loads it via `for (drvStore : {evalStore, store}) if (isValidPath(drvPath)) return readDerivation(drvPath); assert(false)` (`derivation-trampoline-goal.cc:122-125`) — an invalid target `.drv` is not "rebuilt", it trips `assert(false)`. But the **input** `.drv`s are read only *after* the per-target substitution attempt fails: `DerivationGoal::haveDerivation` `co_return`s `Substituted` (`derivation-goal.cc:138`) **before** `makeDerivationResolutionGoal` (`:151`) or `makeDerivationBuildingGoal` (`:229`) — the only goals that read input `.drv`s (`DerivationBuildingGoal` walks each immediate input with `if (isValidPath(depDrvPath)) … else assert(false)`, `derivation-building-goal.cc:211-213`) — are ever created. So a fully-substitutable target reads **zero** input `.drv`s, and the elision of a substitutable subtree's input closure is performed by the build **goals** themselves — they never create the goals that would read it. Because those goals run in the Worker (no access to `EvalState::pendingDrvs_`), the deferred-`.drv` flush must be a **client-side pre-pass before `buildPathsWithResults`** (`installables.cc:655`) that materialises exactly the *demanded frontier*: **`targets ∪ willBuild ∪ {immediate input `.drv`s of `willBuild` nodes}`** — the target is always read by its trampoline (`:124`) even when substitutable; a built node reads its immediate inputs at `derivation-building-goal.cc:211`; nothing strictly below a `willSubstitute` frontier node is read. That partition is exactly what `queryMissing` computes — but today only for *display* (`printMissing → queryMissing`, gated by `settings.printMissing`/`--dry-run`, `installables.cc:652`/`build.cc:142-154`); the design promotes a `queryMissing`-shaped, `pendingDrvs_`-aware walk ([§0.5](#05-the-algebra), the `misc.cc:208` FIXME) to a mandatory flush driver. The CLI build entry points are where DetSys already places `waitForPath`/`waitForAllPaths` ([§3](#3-detsys-asyncpathwriter)); this design drives that barrier from the frontier walk, subject to the frontier↔build substitutability-consistency obligation ([§11](#11-laws-and-test-plan) LD-S6).
2. **Eval-cache `forceDerivation` (Finding C2 — a boundary the first draft missed).** On a *warm eval-cache hit*, `AttrCursor::forceDerivation` (`eval-cache.cc:799-815`) returns the cached `drvPath` string **without running `derivationStrict`**; if `!readOnlyMode && !isValidPath(drvPath)` it `forceValue()`s to regenerate and, if *still* invalid, throws `"don't know how to recreate store derivation"`. Under naive deferral the regeneration re-defers → stays invalid → **throws**. `forceDerivation` has exactly two callers — `installable-flake.cc:92` and `app.cc:129` (the common `nix build`/`nix run` paths). It is a *mandatory* flush site: `forceDerivation` must drain the pending `.drv` (gated, like the production code, on `!readOnlyMode`).
3. **In-eval `realiseContext` (IFD + `DrvDeep`).** `realiseContext`'s `Built` arm calls `buildStore->buildPaths(...)` *during eval* (`primops.cc:172`), reached by import-from-derivation and by the `DrvDeep` whole-closure edge (`primops.cc:1855` `computeFSClosure`). Both need the named `.drv` (and inputs) valid mid-eval. **For the target workload this is rare to absent:** Nixpkgs forbids IFD, and a `DrvDeep` edge arises only from `unsafeDiscardOutputDependency`-style constructs, not ordinary `${pkg}` interpolation (which is `Built`/`ensureSlot`, a pure in-memory insert — `primops.cc:1864`, simulated in [§1](#1-what-derivationstrict-does-today)). Where IFD *does* fire (non-Nixpkgs evals; IFD is default-on in Nix — `eval-settings.hh:214`), it forces a full build mid-eval anyway, so a `.drv` flush there is lost in the build's noise. Still, the flush-on-read guard must cover it for correctness.
4. **`import (drv.drvPath)` / `readDerivation` / `readInvalidDerivation`** (`store-api.cc:1168` `readDerivationCommon`). Reading a `.drv` back as a Nix value requires its bytes.
5. **CLI/C-API observation, *including value output to stdout*.** `nix show-derivation`, `nix derivation show`, copying a `.drv` to a cache, `--add-root` on a `.drv` — and the easy-to-miss one: **emitting a derivation path as a serialised `Value`** (`nix eval .#x.drvPath`, `--raw`/`--json`, `nix-instantiate --eval`). `nix eval` is **non-read-only by default** (the `--read-only` flag, `installables.cc:230-236`, is opt-in and documents it "can cause errors when accessing store paths … during evaluation"), so today it writes the `.drv` eagerly mid-eval; to preserve that observable the deferred design must flush *before the bytes leave the process* — the **derivation-side twin** of the existing source-side cover-fix, which already calls `resolveSourceVirtualContext`/`ensureLazyPathsCopied` on the output `context` (`eval.cc:142-143`, rule at `:80-102`). Relying only on the end-of-process queue drain (`AsyncPathWriter` destructor `join`, `async-path-writer.cc:67-72`) keeps *sequential* use correct but races a *concurrent pipeline* (`nix eval .#x.drvPath | xargs nix-store -q …`); so stdout-emission of a derivation path is itself a flush site. These are the `waitForAllPaths` sites (the warm-eval-cache `drvPath` path is covered by `forceDerivation`'s C2 flush, item 2).

The flush trigger is therefore **two-tier**, exactly like the source `resolveSourceVirtualContext` chokepoint plus its per-path `ensureLazyPathCopied`:

- a **bulk flush at the artificial boundary** (end-of-eval / `waitForAllPaths`, and each build submission) — the batched fast path ([§4.3](#43-the-batched-store-flush));
- a **targeted flush-on-read** (`waitForPath(drvPath)` / a guard inside `forceDerivation`, `readInvalidDerivation`, and the `realiseContext`/`DrvDeep` arm) for the in-eval read-back, transitively flushing that `.drv`'s pending input `.drv`s first.

The flush-on-read guard is the same *shape* as the source-side boundary audit ([PROPOSAL.md §6.2](./PROPOSAL.md#62-the-addpath-source-virtualisation-boundary)): a finite, enumerable set of sites that consume the artifact, each of which must drain before consuming. The audit surface is `git grep` for `forceDerivation` / `readInvalidDerivation` / `readDerivation` / `computeFSClosure` / `buildPaths` callers reachable from eval.

### 4.3. The batched store flush

At the bulk boundary, instead of N independent `writeDerivation` calls (each its own daemon round-trip), flush all pending `.drv`s through one bulk submission (deferred source copies flush at the *same barrier* but may use their existing path — Finding D, [§6](#6-interaction-with-the-lazy-source-substrate)). **A round-1 draft of this section claimed this is a single atomic transaction via `addMultipleToStore`; that is wrong, and the correction (Finding C3) matters for what the win actually is.**

**What `addMultipleToStore` actually does (simulated, `store-api.cc:195`).** It drives `processGraph` — a `ThreadPool`-backed, topologically-ordered, reference-aware walk (`thread-pool.hh:90`) — calling, per node, `addToStore(info, source)` (`local-store.cc:1027`), which ends in `registerValidPath(info)` *singular* (`:1122`) → `registerValidPaths({…one…})` → **one `SQLiteTxn` per path** (`:935`, `:952`). So the bulk path is **N parallel, reference-ordered, single-path transactions**, *not* one atomic transaction. The win is real but narrower than "one txn": fewer daemon round-trips than N separate `writeDerivation` calls (one framed `AddMultipleToStore` op carries the whole batch on the wire — `remote-store.cc:470`), parallel ingestion, and the deferral lets the writes overlap continued eval if run async. It is **not** one fsync and **not** atomic across the batch.

**Where the *atomic* two-phase property lives, if it is wanted.** `LocalStore::registerValidPaths(ValidPathInfos)` *plural* (`local-store.cc:938`) *is* a single `SQLiteTxn`, two-phase (loop 1 `addValidPath` all, loop 2 wire `AddReference`, then `topoSort` for cycles, `:952-991`) — so intra-batch references are legal there. But `addMultipleToStore` does **not** call it, and it operates on **already-on-disk** paths (it registers validity; it does not copy bytes). To get atomic registration you would therefore run an explicit **two phases**: (1) copy every pending `.drv`/source's bytes into the store (the `addToStoreFromDump`/restore work), then (2) one `registerValidPaths`-plural transaction over all of them. That is a deliberate design choice (atomicity vs. the simpler per-path path), not the free property the first draft implied — call it out, don't assume it.

**The narHash friction (DetSys's `#if 0` FIXME) — still real on the `addMultipleToStore` route.** A `.drv` is `text:sha256` over *flat* bytes (`Raw::Text`, `FileSerialisationMethod::Flat`, `derivations.cc:150-151`). `addToStore(info, source)` re-hashes the incoming NAR and throws on `hashResult.hash != info.narHash` (`local-store.cc:1062`); there is no `nar:sha256` in hand for a flat text object. Resolution: **compute the per-item `narHash` when building the batch** — for a `.drv` ATerm (kilobytes) a NAR dump + SHA-256 is negligible and is the same pass `addToStoreFromDump` does internally; it satisfies the existing contract with no store-API change. (The alternative — a `Raw::Text` `addMultipleToStore` variant that skips the NAR verify — is deferred: it widens a security-relevant verify path, R4.)

Deferred **source** copies flush in the *same* batch: a source's `inputSrcs` entry is named by its narHash (intrinsic, already computed), so its `ValidPathInfo` is fully determined; the copy supplies the bytes. This is the cross-derivation batching the per-`derivationStrict` `outPathsOf` cannot do — N derivations sharing sources, or a deep dependency chain, copy and register in one bulk submission.

## 5. Soundness obligations

Each is stated with the invariant it rests on and how it is discharged.

- **The `drvHashes` memo / `.drv`-bytes split makes deferral sound (corrected by independent review — the original discharge had a hole).** *Obligation:* `hashDerivationModulo(B)` recurses through `B`'s `inputDrvs` via `pathDerivationModulo` → on a cold `drvHashes` entry, `readInvalidDerivation(A.drvPath)` — a **disk read of `A`'s `.drv`** (`derivations.cc:872`, via `store-api.cc:1218`). If `A`'s write were deferred and `drvHashes` cold, this read would fail. *The original discharge* — "`A` is instantiated before `B`, so the memo is warm" (`primops.cc:2038-2039`) — **holds only when `B` is instantiated by running `derivationStrict` this process.** The independent review found the gap: on a **warm eval-cache hit** ([§4.2](#42-resolution-boundaries) item 2), `forceDerivation` returns `B`'s cached `drvPath` *without* running `derivationStrict` for `B` *or* `A`, so `drvHashes` is **cold for the entire sub-closure**. *The real discharge:* nothing downstream of a cache hit recomputes the modulo hash. The build goals read `.drv` **bytes** directly (`readDerivation`, `derivation-trampoline-goal.cc:124`) and never call `hashDerivationModulo`/`pathDerivationModulo` (verified: no caller in `build/`). So the cold memo is **benign** — it would only bite a hypothetical in-process `hashDerivationModulo(B)` after a cache hit, which no current path performs; and the [§4.2](#42-resolution-boundaries) build boundary has already flushed (or, on a warm cache, the prior process already wrote) `A`'s `.drv` bytes, so even `readInvalidDerivation` would succeed. **The soundness rests on the build-time `.drv`-bytes boundary, not on instantiation ordering.** (R2: the memo key is `drvPath`, a content hash — process-independent — so a second process recomputes the identical key.)
- **Content-addressing closes the mint-vs-write gap.** *Obligation:* the `drvPath` returned at instantiation must equal the path eventually registered. *Discharge:* both are `makeFixedOutputPathFromCA(hash(unparse(drv)))` over the same in-memory `drv` — equal by construction. `writeDerivation`'s `assert(path2 == path)` (`derivations.cc:155`) is the existing backstop; it holds verbatim.
- **Intra-batch references resolve.** *Obligation:* a batched `.drv` referencing a same-batch source/`.drv` must register without a "missing reference" error. *Discharge:* depends on the chosen flush route ([§4.3](#43-the-batched-store-flush), Finding C3). The `addMultipleToStore` route handles it by `processGraph`'s **reference-ordered** ingestion (a referent is added after its references — `store-api.cc:195`), not by atomicity. The explicit two-phase route handles it by `registerValidPaths`-plural inserting all paths before wiring any reference (`local-store.cc:952-991`). Either works; the proposal must not claim the *former* gives the *latter*'s atomicity.
- **Flush-on-read covers every in-eval read-back.** *Obligation:* no eval-time consumer reads a pending `.drv`'s bytes. *Discharge:* the enumerated boundary set ([§4.2](#42-resolution-boundaries)) with `git grep`-able audit surface. The in-eval byte consumers are `AttrCursor::forceDerivation` (warm eval-cache, Finding C2), `realiseContext`/`buildPaths` (IFD + `DrvDeep`), and `readInvalidDerivation` — each gated by a flush that transitively drains pending input `.drv`s. *The first draft omitted `forceDerivation`; it is the most common one on the `nix build` path and its absence would surface as a `"don't know how to recreate store derivation"` throw, not silent corruption.*
- **Failure latches.** *Obligation:* a `.drv` write that fails (disk full, permissions) must not be silently swallowed — every later observer must see the error. *Discharge:* the source scheduler's `shared_future` winner-election + latched-`Failed` discipline ([PROPOSAL.md §6.1](./PROPOSAL.md#61-input-materialisation)) is reused; DetSys's `AsyncPathWriter` already does this with `promise.set_exception` (`async-path-writer.cc:61`).
- **`readOnlyMode` is unchanged.** Read-only never flushes (it never wrote before); the scheduler simply never drains. The returned attrset is byte-identical to today's read-only path.

## 6. Interaction with the lazy-source substrate

This design and the existing source-materialisation architecture **compose into one resolution boundary**, which is the larger prize. Today the two are sequenced *eagerly within* `derivationStrict`: source placeholders resolve (and copy) at step 2, then the `.drv` writes at step 5. After this design, both defer to the same flush:

- **Sources** already defer their narHash walk to the observation boundary and (for rev-pinned/deferred-mount inputs) defer the copy ([PROPOSAL.md §2.F, §6.1](./PROPOSAL.md#f-mountinput-known-narhash-fast-path)). What forces the *copy* early today is precisely that `derivationStrict` resolves `inputSrcs` to valid paths *before* writing the `.drv` (referential integrity at register time). Deferring the `.drv` write removes that forcing function: `inputSrcs` need only be *named* (narHash known) at instantiation, and *copied* at the same batched flush as the `.drv`.
- The unified flush is therefore: at a build/observation boundary, **bulk-copy all deferred sources and bulk-register all pending `.drv`s**, reference-ordered (or, if atomicity is wanted, the explicit copy-then-`registerValidPaths`-plural two-phase route — [§4.3](#43-the-batched-store-flush), Finding C3).

**The unification is the *boundary*, not necessarily a single store call (Finding D, self-review).** It is tempting to say "feed sources and `.drv`s into one `addMultipleToStore`." But `addMultipleToStore` takes `ValidPathInfo` with `narHash` **and `narSize`** pre-known, and a source is ingested today via `addToStore(name, path, method, …)` (`fetch-to-store.cc:400`) — the accessor overload that *walks* to compute exactly those. A deferred source has its `narHash` (intrinsic, [§2.2](#22-intrinsic-vs-deferrable)) but not its `narSize` without that walk. So `.drv`s (bytes in hand, [§4.3](#43-the-batched-store-flush)) drop straight into the bulk register, while sources may keep their existing `fetchToStore(Copy)` path — driven *at the same barrier*, but not necessarily through the same call. What is genuinely shared is **one barrier, one reference-ordering, one `ThreadPool`** — not one API call. Because the `.drv`→source dependency lives in `inputSrcs`, a single reference-ordered drain (one `processGraph`, or sources-then-drvs as two phases of one flush) suffices; there is **no separate scheduler to coordinate** — the earlier "two schedulers need an ordered flush" risk dissolves into one queue draining in DAG order.

The result is the design the original question described: *"when a derivation is forced, recursively and in parallel batch and do all the store copies, rewrites, and materialisation."* The rewrites are still eager (intrinsic, [§2.2](#22-intrinsic-vs-deferrable)); the copies and writes are the batched, parallel, boundary-triggered work. The parallelism is the source scheduler's existing `ThreadPool` fan-out ([PROPOSAL.md §2.O](./PROPOSAL.md#o-sourceplaceholder--materialisationscheduler--content-keyed-memo)) extended across the `.drv` write-queue.

A concrete payoff: `nix build --dry-run` over a flake (pure instantiation, thousands of `.drv`s, **zero builds**) today copies every `lib.cleanSource ./.` and writes every `.drv` — it is **not** `readOnlyMode` (`build.cc:142-154` returns before `Installable::build`; `toDerivedPaths` instantiates and writes). After this design it writes nothing: `--dry-run` is **instantiation-only** (it builds nothing), so the `pendingDrvs_`-aware `queryMissing` ([§4.2](#42-resolution-boundaries), the `misc.cc:208` fix) computes its "will be built / fetched" report from the in-memory queue and produces no store mutations — faster, and more honest about being a dry run.

## 7. Performance characteristics

"walk" = one tree traversal; "copy" = walk + store write; "txn" = one SQLite transaction / daemon round-trip.

| Scenario | Today | After this design |
|---|---|---|
| **Eval-store mass instantiation** (Hydra/`nix-eval-jobs`: M `.drv`s, no build, no IFD) — *the headline win* | M `.drv` writes (M daemon round-trips) + per-drv source copies | writes + copies deferred and flushed in bulk at end-of-eval (parallel ingestion, fewer round-trips); overlaps eval if async |
| `nix build --dry-run`, source-heavy flake (**non-read-only — writes today**) | M `.drv` writes + N source copies (instantiation writes `.drv`s; only the *build* is skipped — §6) | 0 writes, 0 copies — the `pendingDrvs_`-aware `queryMissing` (`misc.cc:208` fix) reports "will be built/fetched" from the in-memory queue and observes no `.drv` as a file (the genuine dry-run elision) |
| `nix eval .#x.drvPath`, source-heavy flake (**non-read-only — writes today**) | the `.drv` write + N source copies, eager | the `.drv` write **deferred to the output-emission boundary** ([§4.2](#42-resolution-boundaries) item 5) — *same write, batched, not elided* (the path is observed); its inputs' source copies flush with it |
| `nix build` of 1 target, M-derivation closure, all built from source | M `.drv` writes + per-drv source copies | M `.drv` writes + source copies bulk-flushed; writes overlap eval if async — **same M writes, fewer round-trips, not fewer writes** |
| `nix build` where the target is substitutable from a cache | full instantiation: M `.drv` writes + source copies; substitution then skips most *builds* | the **target** `.drv` must be written (it is read to even construct the goal); a fully-substitutable target reads **none** of its input `.drv`s (it returns `Substituted` before the input-drv goals exist), so the input-closure `.drv` writes + source copies are **elided** exactly where the inputs substitute |
| Deep dependency chain (A→B→…→Z), then build from source | each `.drv` write a separate round-trip | bulk-flushed at the build boundary (reference-ordered); still N writes (the inputs are built, not substituted) |

**Finding C1 (re-narrowed by independent review) — the substituted-away-input win is real; the *target* `.drv` is not skippable.** The first draft over-claimed both ways and an independent code-walk corrected it. The precise truth, simulated against master:

- **The target `.drv` is always required for a build attempt.** `DerivationTrampolineGoal` loads it (`derivation-trampoline-goal.cc:122-125`, `if (isValidPath) readDerivation … else assert(false)`); an `Opaque` (deferred) target `.drv` that nothing can produce reaches that `assert(false)` via a re-entrant obtain-goal. So a deferred `.drv` *that a build needs* must be flushed first — the [§4.2](#42-resolution-boundaries) build boundary does exactly this.
- **But a fully-substitutable target reads *none* of its input `.drv`s.** `haveDerivation` attempts substitution of the *target's outputs* first and `co_return`s `Substituted` at `derivation-goal.cc:138` — **before** `makeDerivationResolutionGoal` (`:151`) or `makeDerivationBuildingGoal` (`:229`), the only goals that read the input `.drv` closure, are ever created. The input-closure validity requirement (`gaveUpOnSubstitution` → `derivation-building-goal.cc:211-213`, every input `.drv` valid else `assert(false)`) is reached **only after the per-target substitution attempt fails** — i.e. only for the subset actually built from source.

So deferral **does** elide the input-closure `.drv` writes (and source copies) of any subtree that substitutes — a genuine win the round-2 retraction wrongly threw away — while the *target* `.drv` of each requested output is unavoidable. `queryMissing` reinforces this: an invalid top-level `.drv` is shortcut into `res.unknown` without recursing (`misc.cc:207-211`, `// FIXME: we could try to substitute the derivation`), and it is a substitution-aware *reporting* pass that runs before the build goals. (Source byte-copies are independently gated on the build needing them — `derivation-building-goal.cc:154-161`.)

**The workload-shape risk (R5/R6, stated as a blocker not an afterthought).** Deferral can *un-fuse* deliberately fused work and lose. Measured shapes:

- **Eval-store mass instantiation (the win):** non-read-only, IFD-free, build-free ([§2](#2-the-unblock-primitive)). Deferral is live, the batching window is uninterrupted, and the delta is N per-path round-trips → one bulk submission + parallel ingestion + async overlap. This is where to expect a clear gain.
- **Build-everything-from-source:** every `.drv` is written anyway (C1), so the only delta is round-trip coalescing and async overlap — neutral-to-positive, but the async worker adds a lock-handoff per `.drv`; on a tiny closure the handoff could dominate. Measure on a small closure.
- **Substitutable build:** the saving is source-copy elision (above), bounded by how much of the closure substitutes. **No longer the headline.**

The source-materialisation work already learned this lesson the expensive way (an eval-trace cache that was hot-7×-faster on one workload and cold-8.6×-slower on another); this design inherits the discipline of gating behind a setting and measuring each shape, not defaulting blind.

## 8. Comparison

Three distinct mechanisms, often conflated:

| | Async writer (DetSys, shipped) | Batched writer (this design, v1) | Lazy writer (this design, full) |
|---|---|---|---|
| `drvPath` computed | eagerly, eval thread | eagerly, eval thread | eagerly, eval thread |
| `.drv` write timing | bg thread, per-item loop | bg thread, bulk submission per flush (**N per-path txns, not 1** — C3) | skipped until a boundary reads it |
| Source copy | eager (unchanged) | **deferred, same flush** | deferred |
| Win | hide write *latency* | + reduce *daemon round-trip count*; parallel ingestion; cross-drv source batching | + elide the input-closure `.drv` writes **and** source copies of any substituted-away subtree (the target `.drv` is still written — C1) |
| Resolution boundary | `waitForPath` per CLI site | same + end-of-eval bulk flush + `forceDerivation` (C2) | same + transitive flush-on-read guard |
| Blocker | none (shipped) | narHash friction ([§4.3](#43-the-batched-store-flush)) — solved by per-item narHash, and it is *free* (the existing flat-CA write already NAR-hashes, `local-store.cc:1271-1274`) | the **target** `.drv` of any built output is unavoidable ([§7](#7-performance-characteristics), C1) — bounds the win, not a blocker |

They are **complementary, not alternatives.** The ideal end state is async + batched: DetSys's `#if 0` block *enabled* and running on the worker thread, with the narHash friction resolved and source copies folded into the same flush. This design is "port DetSys's async harness, then complete the batch it disabled, then extend the batch to sources." **Its clean win is eval-store mass instantiation ([§2](#2-the-unblock-primitive)); for the build path it degrades gracefully to async + batched-write plus elision of substituted-away input closures ([§7](#7-performance-characteristics), C1), with only each requested output's target `.drv` unavoidable.**

Relative to DetSys specifically: DetSys defers the `.drv` write but eagerly copies sources and writes per-item; this design defers both and bulk-submits both at one boundary. Relative to upstream/our-tree: neither has any of it — the member `Store::writeDerivation` is synchronous on our branch (`store-api.hh:956`, single overload; no `AsyncPathWriter`).

## 9. Non-goals and boundaries

- **Not a new value type or context variant.** `VirtualDerivation` is a store-side pending state keyed by the already-content-addressed `drvPath`; it reuses `DrvDeep`/`Built` context. A new `NixStringContextElem` variant would incur the R3 consumer-enumeration hazard (parser/printer/`eval-cache.cc::AttrDb::setString`/error paths) for no benefit — the `drvPath` is already in the existing elements.
- **Not lazy `drvPath` computation.** `drvPath` is computed eagerly, always — forced by content-addressing ([§2.2](#22-intrinsic-vs-deferrable)), confirmed by DetSys doing the same.
- **Not a derivation-output-resolution change.** `tryResolve` / `DownstreamPlaceholder` / the realisations table are unchanged; this design is about the `.drv` *file*, not output paths.
- **Not a daemon-protocol change (v1).** The batched flush uses the existing `addMultipleToStore` (op 44) and/or `registerValidPaths`; the per-item-narHash fix is client-side. The optional text-CA `addMultiple` variant ([§4.3](#43-the-batched-store-flush)) *would* be a protocol addition and is explicitly deferred.
- **Not a change to read-only semantics.** Read-only eval already returns the attrset without writing; this design generalises that path, it does not alter it.
- **Not async eval.** As with the source scheduler ([PROPOSAL.md §6.5](./PROPOSAL.md#65-design-boundaries-deliberately-not-crossed)), materialisation parallelises via `ThreadPool`/worker thread; eval itself stays thread-per-task.

## 10. Open questions and review obligations

Per [REVIEW-DISCIPLINE.md](./REVIEW-DISCIPLINE.md), the load-bearing claims that a review must *simulate* (R1), not cite. Items 1-2 were the open questions in the first draft; the adversarial pass **answered** them (against master) — recorded here as resolved, with the residue that remains open:

1. **(RESOLVED — Finding C1/V1) Substitutability ordering.** *Answered (as re-narrowed by independent review, V1 — superseding the round-2/4 over-retraction this item once stated):* a build attempt always requires the **target** `.drv` valid on disk (`DerivationTrampolineGoal` `readDerivation` → `assert(false)` on invalid, `derivation-trampoline-goal.cc:122-125`), but **not** its whole input closure. `haveDerivation` `co_return`s `Substituted` (`derivation-goal.cc:138`) *before* the resolution/building goals (`:151`/`:229`) that read input `.drv`s are created — so a fully-substitutable target reads **zero** input `.drv`s, and input-closure writes are elided *exactly where the inputs substitute* (`DerivationBuildingGoal`'s `if (isValidPath(depDrvPath)) … else assert(false)`, `derivation-building-goal.cc:211-213`, is reached only on the built-from-source subset). So "skip substituted-away `.drv`s" is *false for the target* but *true for substituted-away input closures* — that is the build-path win. *Residue:* the elision is realised by the build **goals** (Worker-side); to materialise it daemon-safely the design adds a client-side, `pendingDrvs_`-aware flush pre-pass of `queryMissing`'s shape (teaching the `misc.cc:208` `res.unknown` shortcut to read the pending queue — its own `FIXME`), subject to the frontier↔build consistency obligation ([§11](#11-laws-and-test-plan) LD-S6) — **resolved without pre-substituting**: substitution downloads stay in the Worker's parallel, build-interleaved pool (`maxSubstitutionJobs`, `worker.cc:273`), and the hazard (cheap local `.drv`s, not network outputs) is handled by backing the in-process `evalStore` with an on-demand `pendingDrvs_` source, or by shipping the cheap `.drv` closure to a remote daemon. The decomposition that makes this work: **output download** (network) always stays in the Worker's parallel pool; **source-copy** elision (Finding F, the expensive *local* work) is gated on the build and preserved every regime; only the cheap **`.drv`-write** elision is regime-dependent (full in-process, forfeited for a remote daemon).
2. **(RESOLVED — Findings M1/C2) In-eval flush frequency.** *Answered:* the dominant target workload (Nixpkgs/Hydra) is **IFD-free** (Nixpkgs forbids it; Hydra cannot build during eval), so the `realiseContext`/`Built` flush does not fire there; the `DrvDeep`/`computeFSClosure` edge arises only from `unsafeDiscardOutputDependency`-style constructs, not `${pkg}` interpolation (`Built`/`ensureSlot`, in-memory — `primops.cc:1864`). The first draft over-weighted IFD and **missed** the genuinely common in-eval boundary, `forceDerivation` on a warm eval cache (C2, `eval-cache.cc:799`). *Residue:* the `forceDerivation` flush must be implemented and tested on the warm-cache `nix build` path; without it that path throws.
3. **(R5, RESOLVED by independent review) Latency floor of the per-item narHash.** *Answered:* it is genuinely free, not a hidden second pass. `addToStoreFromDump` already computes a NAR hash for any non-`NixArchive`-SHA256 object (`local-store.cc:1271-1274`: `if (dumpMethod != NixArchive || hashAlgo != SHA256) { HashSink narSink; dumpPath(realPath, narSink); narHash = narSink.finish(); }`), and a `.drv` is `Raw::Text`/`Flat` — so that branch *already fires* on every `.drv` write today. The batched route computes the same hash the eager route does; it is not net-new work.
4. **(R4) Blast radius — confirmed nil.** This design changes *when* `.drv`s and sources are written, not *what* — `drvPath`, `outPath`, and the `.drv` contents are byte-identical to today. No rebuild scope. R2: `drvPath = hash(unparse(drv))` in both the eager and deferred paths, over the same `drv` struct.
5. **(R6) Alternatives.** (a) DetSys's async-only writer — adopted as the *base*, extended (it leaves source copies eager and writes per-item). (b) A daemon-side deferred-write registry — rejected: the pending state needs `EvalState` context the daemon lacks; the flush must be client-side before build submission. (c) Doing nothing and relying on OS page-cache — rejected on R5: the cost is fsync + SQLite txn commit per `.drv` (`registerValidPaths` syncs when `syncBeforeRegistering`, `local-store.cc:945`), which the page cache does not amortise.

A review of this document that does not walk items 1-2 against the source is a citation audit, not an adversarial review (R8). *(They have now been walked; the Findings ledger records the result.)*

## 11. Laws and test plan

Per [PROPOSAL.md §3](./PROPOSAL.md#3-algebraic-invariants), each law below holds "by construction, by test, and by review," and the test column states **property** (RapidCheck `RC_GTEST_PROP`/`RC_GTEST_FIXTURE_PROP`), **unit** (example-based `TEST`/`TEST_F`), **boundary audit** (drive the *real* production path and assert the guard fires — non-vacuous by construction, the `boundary-audit-*.cc` idiom), or **integration** (`tests/functional/.../*.sh`). Negative controls are named explicitly; the discipline ([PROPOSAL.md §5](./PROPOSAL.md#5-design-rationale): *"a unit test that calls a component directly cannot reveal that nothing in production does"*) is that the headline laws get an end-to-end test *and* a property, not one or the other. Proposed homes: `src/libstore-tests/lazy-derivation-*.cc`, an extension of the existing `src/libexpr-tests/boundary-audit-derivation-strict.cc`, and `tests/functional/lazy-derivations/*.sh`. (Fixtures `LibStoreTest`/`BoundaryAuditTest` exist; but **`Arbitrary<Derivation>` does *not*** — R9-confirmed by `grep` — so every `property` row presupposes a generator that must first be built in `src/libstore-test-support/include/nix/store/tests/derivation.hh`, following the `derived-path.hh` idiom: default `InputAddressed` outputs, gate the CA/`Deferred` arms behind the existing `CaDerivationTest`/`DynDerivationTest` fixtures to avoid the xp-feature `require`. Until then the `property` rows degrade to example units over hand-built drvs.)

The laws are layered exactly as [§0.5](#05-the-algebra): **LD-P** (projection), **LD-S** (selective task), **LD-V** (virtual ⇄ materialised phase).

### Layer 1 — projection laws (LD-P)

| ID | Obligation | Test (type · name) |
|---|---|---|
| **LD-P1** | **Key purity / eager identity.** `computeStorePath(drv)` is a pure function of the in-memory `drv`; computing it writes nothing and reads no `.drv`. | property · `LazyDrv.KeyIsPureNoStoreEffect` (+ negative control: `isValidPath(key)==false` after compute-only) |
| **LD-P2** | **Mint = write.** Deferred-instantiation and eventual-register agree: `computeStorePath(drv) == writeDerivation(drv)`. | unit · `LazyDrv.ComputedPathEqualsWrittenPath`; in-tree backstop `derivations.cc:155` `assert(path2==path)` |
| **LD-P3** | **Soundness.** Equal `drvPath` ⇒ equal ATerm bytes ⇒ equal reference set; distinct content ⇒ distinct key. | property · `LazyDrv.KeyDiscriminatesContent` (+ negative `MutatedFieldChangesKey`) |
| **LD-P4** | **Cross-process determinism (R2).** `computeStorePath` consumes no heap pointer; two `EvalState` instances agree on bytes. | property · `LazyDrv.KeyStableAcrossStates` (mirrors `SourceContentId.SameInputsSameId`) |
| **LD-P5** | **Idempotent materialisation (join-semilattice).** Re-flushing a valid `.drv` is a no-op; store membership grows monotonically. | property · `LazyDrv.ReflushIsNoOp` (store-size invariant); unit · the `derivations.cc:143` bail; Finding [C](#findings-ledger) `std::swap`/`std::move` |
| **LD-P6** | **Enqueue dedup.** Two enqueues of one `drvPath` collapse to one queue entry and one write (improvement over DetSys's unconditional `push_back`, Finding [B](#findings-ledger)). *Aspirational* — gates on `pendingDrvs_`, not in the tree yet. | property · `LazyDrv.DuplicateEnqueueWritesOnce` (`addToStoreFromDump` call-count == 1); negative control = a DetSys-parity regression (unconditional push ⇒ count 2) |
| **LD-P7** | **`computeP` triviality / cost asymmetry — *derives* S1.** The cost is in `keyP` (eager); `computeP = unparse` is the *same* byte-production `keyP` already did (`infoForDerivation` computes `contents` once, `derivations.cc:116`, feeding both `hash`→key and `addToStoreFromDump`→value). So deferral defers only `encodeO`, never a recompute, and the `.drv` side needs no memoising scheduler (Finding [S1](#findings-ledger) falls out, not asserted). | characterisation · `LazyDrv.UnparseComputedOnce` (call-count on `unparse` across compute+flush of one drv ≤ 1 extra); no clean negative control — a cost property, not a guard |
| **LD-P8** | **Reference-set fidelity through deferral** `[LOAD-BEARING]`. `references = inputSrcs ∪ keys(inputDrvs)` (`derivations.cc:109-111`) is a function of the captured `drv` struct, independent of *when* the flush runs — so the registered reference edges (GC/closure) are byte-identical to the eager write. Distinct from LD-P3 (the *key*): this is the *DB-edge* consumer (R3-shape). | unit · `LazyDrv.DeferredReferencesMatchEager` (eager vs enqueue+flush ⇒ equal `queryReferences`); integration · GC-after-deferred-flush keeps inputs alive. **Mutation caught:** a queue holding `drv` by pointer that is mutated before flush |

### Layer 2 — selective-task laws (LD-S)

| ID | Obligation | Test |
|---|---|---|
| **LD-S1** | **Static skeleton.** Post-eval, IFD-free, non-read-only: every reachable `drvPath` is computed and queued, with zero builds and zero `realiseContext` calls (the eval-store mass-instantiation headline, [§2](#2-the-unblock-primitive)). | integration · `lazy-derivations/mass-instantiation.sh` (N drvPaths queued, 0 builds, deferred-write marker count == N) |
| **LD-S2** | **Selective elision (headline).** For a substitutable target the input-closure `.drv`s are **not** materialised: the written set == `targets ∪ queryMissing(targets).willBuild ∪ {immediate inputDrvs of willBuild}` (the target's own `.drv` is read by its trampoline even when substitutable — A3), nothing strictly below a substitutable frontier node. | integration · `lazy-derivations/substitutable-input-elision.sh` **+ negative control** `…non-substitutable-writes-all.sh` (no substituter ⇒ whole closure written; elision must not over-fire) |
| **LD-S3** | **Skip preserves result.** drvPath, outPath, and realised outputs are byte-identical with deferral ON vs OFF (the unchosen branch is pure — analogue of cache-decorator transparency C1). | integration (differential) · `lazy-derivations/deferral-is-observationally-transparent.sh` |
| **LD-S4** | **Daemon safety / frontier sufficiency.** Every `.drv` a build goal reads is valid before the Worker runs; no goal hits `assert(false)` on a deferred `.drv`. The readers are **three** (R9-verified; the draft listed two): the **target** via `derivation-trampoline-goal.cc:124`; each built node's **immediate inputs** via `derivation-building-goal.cc:212` (`deepQueryDerivationOutputMap`); and — **gated under `impure-derivations`** — `derivation-resolution-goal.cc:59` (`readDerivation`, gate `:57-58`). | integration · `lazy-derivations/build-from-source-frontier.sh`; **boundary audit** · `forceDerivation` warm-cache flush (Finding C2); **meta** · `LazyDrv.NoUnenumeratedDrvReaderInBuild` (`git grep` of `readDerivation`/`deepQueryDerivationOutputMap`/`readInvalidDerivation` in `src/libstore/build/` == those three); **negative** · a dropped frontier input fails *loudly* |
| **LD-S5** | **Monadic escape forces.** When IFD/`realiseContext` (`primops.cc:172`) or a `DrvDeep` `computeFSClosure` (`:1855`) is reached, the named `.drv` and its pending input closure flush before the in-eval read. | integration · `lazy-derivations/ifd-forces-flush.sh`; boundary audit · the `realiseContext` arm |
| **LD-S6** | **Frontier ↔ build consistency (TOCTOU).** The submission flush computes the frontier from substitutability *at submission*; the goals re-decide it *at realisation*. A `willSubstitute` frontier node that flips to must-build (cache eviction in between) must not leave its input closure unflushed, else a Worker goal trips `assert(false)`. **Resolved without pre-substituting** (which would pull downloads out of the Worker's parallel, build-interleaved pool — `worker.cc:273` `maxSubstitutionJobs` — into a serial phase): the hazard is over cheap *local* `.drv`s, not network outputs. **In-process Worker** (local/eval-store) — back the `evalStore` the Worker already holds (`entry-points.cc:11`) with `pendingDrvs_`, materialising a demanded-but-elided `.drv` on the spot (local call, no protocol); **remote daemon** — ship the cheap `.drv` *closure* up front (KB each, batched), forfeiting local `.drv`-write elision for that regime only. Outputs are never pre-downloaded. | integration · `lazy-derivations/substitutability-flips-between-flush-and-build.sh` (evict the cache entry after the flush pre-pass, before the build; assert clean realisation, never a crash) + `worker.cc:273`/`:275` (substitution/build are separate parallel pools) + `entry-points.cc:11` (in-process Worker holds `evalStore`) |
| **LD-S7** | **Applicative/selective coherence — *the law that justifies elision* `[ENTAILED]` `[LOAD-BEARING]`.** Selective-flush-then-realise yields the same *output* closure as applicative-flush-the-whole-closure-then-realise: eliding the unchosen branch ≡ running it then discarding its `.drv` writes, because a substituted subtree's `.drv` is read by nothing below it (LD-S2). | integration differential · `lazy-derivations/selective-equals-applicative-on-outputs.sh` (build two ways; assert output closures byte-identical via `nix-store --query --hash`, valid-`.drv` sets differ b⊂a). **Pair with LD-S4's loud-failure control** (a frontier bug would match outputs yet crash a goal) |
| **LD-S8** | **Static-dependency over-approximation `[ENTAILED]`.** The statically-named edge set (`inputDrvs.map` populated at eval, `primops.cc:1864`) ⊇ the realisation-demanded set (LD-S2); no un-named dependency is ever demanded — IFD-free guarantees it, and where IFD fires it collapses the dynamic dep into the static set *eagerly* before the outer drv is hashed (the monadic escape, LD-S5). | integration · `lazy-derivations/static-deps-superset.sh` (parse `.drv` `inputDrvs` = named; `queryMissing.willBuild ∪` trampoline-reads = demanded; assert demanded ⊆ named). **Negative control:** an IFD expr — under IFD-free enforcement it's rejected/forced eagerly (superset restored) |
| **LD-S9** | **`queryMissing` partition + the flush-driver enabling change `[LOAD-BEARING]`.** `willBuild`/`willSubstitute`/`unknown` are populated by mutually-exclusive branches (`misc.cc:186`/`:317`/`:209,:311`); a deferred (invalid-bytes) `.drv` falls into `unknown` via the `:207-211` bail **today**, so the client-side flush-driver *must* teach that branch to consult `pendingDrvs_` (the `misc.cc:208` FIXME). This is the single gate the whole build-path flush rests on, and is currently untested even as a hypothesis. | unit · `LazyDrv.QueryMissingPartitionDisjoint` (built/substitutable/deferred graph; assert disjoint + deferred ∈ `unknown` *pre*-change); **enabling-change test** (feature-gated): post-fix the deferred node moves `unknown`→`willBuild`/`willSubstitute` |

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
- **LD-C3 — hash-consing of independently-equal drvs** `[NIX]` *(aspirational, gates on `pendingDrvs_`)*. Two *independently constructed* equal drvs reach one `drvPath` (LD-P4) and *then* dedup (LD-P6) — maximal sharing; distinct from LD-P6 (two enqueues of the *same* path). Test `LazyDrv.StructurallyEqualDrvsShareCell`.
- **LD-C2 / C4 / C5** are exact restatements of **LD-P5 / LD-S2 / LD-P4** in heap vocabulary (force-once, demand-stops-at-WHNF, process-independent-thunk-identity) — cross-references, no separate tests.

### Cross-cutting seam laws

Per PROPOSAL.md §3's "four cross-cutting seam laws," these sit at the seam between the framing and the rest of Nix:

- **LD-X4 — flush-on-read covers every in-eval byte consumer** `[LOAD-BEARING]`. The eval-time `.drv`-*byte* consumers are a finite, `git grep`-able set — `forceDerivation` (`eval-cache.cc:799-816`, C2), `realiseContext`/`buildPaths` (`primops.cc:172`), `DrvDeep` `computeFSClosure`+`readDerivation` (`:1855`,`:1859`), `readInvalidDerivation` — each must drain before reading; path-*string* renders (`mkOutputString`, LD-V1) are **not** in the set (no bytes read). Test: a boundary-audit per site + meta `LazyDrv.ByteConsumerAuditSurfaceComplete`. Same finite-enumerable-consumer shape as PROPOSAL.md's L-NoLeak / §6.2.
- **LD-X1 = LD-V4** (failure-latching across observers), **LD-X2 = LD-V2** (no new context variant — the R3 seam), **LD-X3 = LD-V5** (read-only invariance): promoted in *role* to seam laws; same statements and tests as their LD-V rows.

### Representative test drafts

The full set lives in the homes above; these four fix the idiom — one per category.

**LD-P3/P4 — RapidCheck property** (`src/libstore-tests/lazy-derivation-projection.cc`):

```cpp
#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include "nix/store/derivations.hh"
#include "nix/store/tests/libstore.hh"     // LibStoreTest fixture
#include "nix/store/tests/derivation.hh"   // Arbitrary<Derivation> (extend if absent)

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

**LD-V1 — boundary audit** (extends `src/libexpr-tests/boundary-audit-derivation-strict.cc`). **Two halves, because one passes today and one is feature-gated (A4).** The *rendering* half passes against master now. The *deferral* half cannot pass against master: `prim_derivationStrict` writes eagerly (`primops.cc:2029`), so the `.drv` *is* valid here — the existing `BoundaryAuditDerivationStrictTest` depends on exactly that, reading the drv back at `boundary-audit-derivation-strict.cc:34`. Gate it with gtest's `DISABLED_` until the write-queue lands; its NON-VACUITY is then real (re-enabling eager `writeDerivation` makes `EXPECT_FALSE` fail).

```cpp
// LD-V1 rendering half — passes against master TODAY.
TEST_F(BoundaryAuditTest, DerivationStrictRendersAttrset)
{
    auto v = evalDerivationStrict(R"(derivation {
                 name = "x"; system = "x"; builder = "/y"; })");  // REAL prim_derivationStrict
    EXPECT_FALSE(attrString(v, "drvPath").empty());
    EXPECT_FALSE(attrString(v, "outPath").empty());               // outPath renders with no .drv read
}

// LD-V1 deferral half — FEATURE-GATED. On master this FAILS (eager write ⇒ isValidPath TRUE);
// enable when the write-queue lands. Then it is non-vacuous: deleting the deferral re-fails EXPECT_FALSE.
TEST_F(BoundaryAuditTest, DISABLED_DerivationStrictDefersDrvUntilRead)
{
    auto v       = evalDerivationStrict(/* … as above … */);
    auto drvPath = state.store->parseStorePath(attrString(v, "drvPath"));
    EXPECT_FALSE(state.store->isValidPath(drvPath));        // deferred (not yet on disk)
    EXPECT_NO_THROW(state.store->readDerivation(drvPath));  // flush-on-read materialises it
    EXPECT_TRUE(state.store->isValidPath(drvPath));         // now valid
}
```

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

## 12. Implementation plan

A context-free starting point — concrete files, functions, and order — for an agent that has read this document but not written the code. Each increment names the [§11](#11-laws-and-test-plan) laws that are its acceptance criteria. **Gate the whole thing behind a setting** (`lazy-derivations`, default off, in `eval-settings.hh`); measure each workload shape ([§7](#7-performance-characteristics)) before flipping it on. DetSys source to port is at `~/Packages/nix-src` (pinned `35cd98d10`).

**The member precedent this follows verbatim:** `EvalState` already holds `const std::shared_ptr<MaterialisationScheduler> materialisationScheduler` (`eval.hh:559`, constructed `eval.cc:351`) and clears `inputMaterialisations_` in `resetFileCache()` (`eval.cc:1224`, `:1240`, with the comment "Cleared on `resetFileCache` — that's the safe boundary"). The `.drv` write-queue is a third such member, **drained** (not just cleared) at that same boundary.

The increments are ordered so that **1–3 ship the headline eval-store win on their own**; 4 makes the `nix build`/IFD paths correct; 5 is the larger build-path elision; 6 is the source-copy unification.

### Increment 1 — port `AsyncPathWriter` + add the dedup (substrate; no user-visible change)
- **Copy** `async-path-writer.cc` + `include/nix/store/async-path-writer.hh` from `~/Packages/nix-src/src/libstore/` into `src/libstore/`; add `'async-path-writer.cc'` to the sources list in `src/libstore/meson.build` (next to `'misc.cc'`/`'optimise-store.cc'`, ~`:325`–`:329`).
- It already provides `AsyncPathWriter::make(ref<Store>)`, `addPath(contents, name, refs, repair, provenance) → StorePath` (synchronous hash → return `StorePath`, enqueue), `waitForPath`/`waitForAllPaths`, a worker thread, and `set_exception` failure-latching (`async-path-writer.cc:61`).
- **One change vs DetSys:** replace the unconditional `state->items.push_back(...)` (`:93`) with a `drvPath`-keyed dedup — back the queue with `boost::concurrent_flat_map<StorePath, Item> pendingDrvs_` and `try_emplace` (no-op when the key is present). *Acceptance: **LD-P6**, **LD-X1**, **LD-P1**.*

### Increment 2 — defer the `.drv` write in `derivationStrict` (deferral becomes live)
- Add `const ref<AsyncPathWriter> asyncPathWriter` to `EvalState`; construct it in the ctor beside `materialisationScheduler` (`eval.cc:351`); call `asyncPathWriter->waitForAllPaths()` inside `resetFileCache()` (`eval.cc:1224`) so the queue drains at the same safe boundary as `inputMaterialisations_`.
- `primops.cc:2028-2029`: replace `settings.readOnlyMode ? computeStorePath(...) : writeDerivation(...)` with: compute `drvPath = computeStorePath(*state.store, drv)` **always**, then `if (!settings.readOnlyMode) state.asyncPathWriter->addPath(drv.unparse(*state.store, false), drvPath-name, references, state.repair, …)` — `references`/`unparse` exactly as `infoForDerivation` (`derivations.cc:107-123`). Keep the `drvHashes` memo (`:2038-2039`) and the returned attrset byte-identical. (DetSys's `writeDerivation(*state.asyncPathWriter, …)` overload at its `primops.cc:1881` is the template.) *Acceptance: **LD-P2, LD-P7, LD-V1, LD-V3, LD-V5**.*

### Increment 3 — batched bulk flush + observation boundaries (ships the headline win)
- **Enable** DetSys's disabled bulk path (`async-path-writer.cc:134-156`, the `#if 0`): build `Store::PathsSource`, call `addMultipleToStore`. **Resolve the narHash FIXME** by computing each item's `narHash`/`narSize` while constructing its `ValidPathInfo` (NAR-dump the ATerm + SHA-256 — the very hash `addToStoreFromDump` already computes for a `Raw::Text`/`Flat` object, `local-store.cc:1271-1274`, so it is free, not a second pass).
- **Insert the value-output drain** at the `eval.cc:142-143` slot that already drains source placeholders (`resolveSourceVirtualContext`/`ensureLazyPathsCopied`): add the `.drv` flush *before the bytes leave the process* (the derivation-side twin, §4.2 item 5). Add `waitForAllPaths()` likewise to `nix derivation show`, `nix copy` of a `.drv`, and `--add-root`. *Acceptance: **LD-D2/D3/D4, LD-X4, LD-S1** (the `mass-instantiation.sh` integration test green).*

> **After Increments 1–3 the eval-store mass-instantiation win ([§7](#7-performance-characteristics)) ships**, and `nix eval`/`nix-instantiate` are observationally identical (deferred-then-flushed at the output boundary).

### Increment 4 — in-eval flush-on-read guards (correctness for `nix build`/IFD)
- `eval-cache.cc:799-815` `forceDerivation`: before the `!isValidPath → throw`, drain the pending `.drv` (gated `!readOnlyMode`, like the production code). *Acceptance: **LD-S4** (Finding C2).*
- `realiseContext` (`primops.cc:172`) and the `DrvDeep` `computeFSClosure` (`primops.cc:1855`): `waitForPath` the named `.drv` and its pending inputs first. *Acceptance: **LD-S5**.*
- any other `readDerivation`/`readInvalidDerivation` reachable from eval: same guard. *Acceptance: **LD-X4** (`ByteConsumerAuditSurfaceComplete`).*

### Increment 5 — the build-path selective flush (larger, delicate; the LD-S6 piece)
- `misc.cc:207-211` `queryMissing`: when `!isValidPath(drvPath)` **and** the drv is in `pendingDrvs_`, read its `BasicDerivation` from the queue and continue (resolve the FIXME) rather than `res.unknown.insert`. *Acceptance: **LD-S9**.*
- Add a **client-side flush pre-pass** before `buildPathsWithResults` (`installables.cc:655`; the existing display call is gated at `:652`): run the `pendingDrvs_`-aware `queryMissing` and flush `targets ∪ willBuild ∪ {immediate inputDrvs of willBuild}`. *Acceptance: **LD-S2, LD-S4, LD-S7**.*
- **LD-S6 (TOCTOU):** for the in-process Worker (`entry-points.cc:11` holds `evalStore`), back `evalStore`'s `isValidPath`/`readDerivation` with `pendingDrvs_` so a goal that demands an elided `.drv` materialises it on the spot; for a remote daemon, ship the `.drv` closure. **Never pre-substitute** ([§10](#10-open-questions-and-review-obligations) residue 1; downloads stay in the Worker's parallel pool). *Acceptance: **LD-S6**.*

### Increment 6 — fold source copies into the same batch ([§6](#6-interaction-with-the-lazy-source-substrate))
- Defer the eager source `Copy` in `derivationStrict` step 2 (`primops.cc:1833-1840`): keep the narHash (intrinsic — [§2.2](#22-intrinsic-vs-deferrable)), drop the eager `fetchToStore2(Copy)`, and drive the copy at the same barrier (sources keep their `fetchToStore(Copy)` call but triggered at the flush — Finding D). *Acceptance: the source-copy columns of **LD-S2/S3/S7**.*

### Test scaffolding & commands
- **Build the `Arbitrary<Derivation>` generator first** ([§11](#11-laws-and-test-plan) intro): `src/libstore-test-support/include/nix/store/tests/derivation.{hh,cc}`, the `derived-path.hh` idiom, default `InputAddressed`, CA arms behind the `CaDerivationTest` fixture. Until it exists, every `property` row degrades to an example unit over hand-built drvs.
- **Unit/property:** `src/libstore-tests/lazy-derivation-*.cc` + extend `src/libexpr-tests/boundary-audit-derivation-strict.cc`; build and run in the dev shell (`ninja -C build && meson test -C build`).
- **Functional:** `tests/functional/lazy-derivations/*.sh` (+ its `meson.build` wiring), run via the functional-test target.
- **Acceptance is per-increment:** the named LD-* laws green; the `DISABLED_` deferral tests ([§11](#11-laws-and-test-plan)) flip to enabled as Increments 2–5 land.

## Findings ledger

The adversarial pass (REVIEW-DISCIPLINE R1, simulated against master) that this revision incorporates. Each finding names the walk that established it.

| ID | Severity | Finding | Walk |
|---|---|---|---|
| **C1** | Critical (re-narrowed by independent review) | The build path requires the **target** `.drv` valid on disk for any build attempt — but **not** its whole input `.drv` closure unconditionally. A *fully-substitutable* target `co_return`s `Substituted` (`derivation-goal.cc:138`) **before** the resolution/building goals (`:151`/`:229`) that read input `.drv`s ever exist — so it reads **zero** input `.drv`s. The input closure is required only for the subset actually **built from source** (`gaveUpOnSubstitution`, `derivation-building-goal.cc:209-214`, reached only after the per-target substitution attempt fails). So "skip substituted-away `.drv`s" is *false for the target* but *true for substituted-away inputs*; the build-path win is target-`.drv` writes are unavoidable, input-closure writes are elided exactly where the inputs substitute. | `derivation-goal.cc:138` (Substituted co_return) vs `:151`/`:229` (input-drv goals, after) + `derivation-building-goal.cc:209-214` (build-from-source only) + `derivation-trampoline-goal.cc:121-126` (target drv required, `assert(false)` via re-entrant obtain-goal) |
| **C2** | Critical | Missing mandatory flush boundary: `AttrCursor::forceDerivation` on a **warm eval-cache hit** returns a cached `drvPath` without `derivationStrict`; a deferred (invalid) `.drv` makes it throw `"don't know how to recreate store derivation"`. Added to §4.2. | `eval-cache.cc:799-815` (`!isValidPath` → `forceValue` → throw, gated `!readOnlyMode`), reached from `installable-flake.cc:92` + `app.cc:129` (the only two `forceDerivation` callers) |
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
| **A1** | Algebraic framing (round-6, this pass) | `.drv` materialisation placed as a content-addressed **selective task** ([§0.5](#05-the-algebra)): the pending closure is the applicative skeleton, `queryMissing` (`misc.cc:294-301`) is the *shape* of the `select` (it computes the `willBuild`/`willSubstitute` partition — though today only for display, A3), substitutable subtrees are unchosen branches whose input-closure writes are elided, IFD is the monadic escape. **Resolves the §4.2-vs-§7 tension**: [§4.2](#42-resolution-boundaries) item 1 was an *applicative* mis-grading ("flush the *transitive* closure" — would write everything, elide nothing), corrected to the selective frontier; and recasts [§10](#10-open-questions-and-review-obligations) residue 1 (`misc.cc:208`) as the **enabling change** for a client-side flush pre-pass the design must *add* (the elision itself is performed Worker-side by the build goals), not a separate enhancement. Adds the LD-P/S/V law inventory + test plan ([§11](#11-laws-and-test-plan)). | `derivation-goal.cc:138` (Substituted) vs `:151`/`:229` (input-drv goals, re-verified on execution order) + `misc.cc:207-301` (`queryMissing` select/elide frontier) + `derivation-building-goal.cc:211-213` |
| **A2** | Citation re-walk (this pass, R9) | Independent re-walk on a fresh checkout against master (`~/Packages/nix`@`99a5f4fac`) **and DetSys (`~/Packages/nix-src`@`35cd98d10`, the pinned SHA)**: all load-bearing claims hold — C1/V1 (execution order), V2 (`local-store.cc:1271-1274`), V4 (no `hashDerivationModulo` in `build/`), §2.1 rendering (`derivations.cc:21` + `eval.cc:1043,1053`). Citation fixes: `async-path-writer.cc:124`→`:125` in §4.1 (the `std::move`; V3 said `:125` but the §4.1 body lagged); `primops.cc` `insert_or_assign` `:2038`→`:2039` (`:2038` is the `hashDerivationModulo` call, `:2039` the memoise). | direct re-walk of each cited line in both trees |
| **A3** | Self-adversarial pass (round-7, this revision; R7/R8) | Attacked the round-6 additions ([§0.5](#05-the-algebra)/[§11](#11-laws-and-test-plan)) themselves. Found+fixed: (i) **mechanism overstated** — §0.5/§4.2/A1 implied `queryMissing` is *wired* as the build-path `select`/flush driver; against source it is **display-only and gated** (`printMissing`→`queryMissing`, `shared.cc:56-72`; `installables.cc:652` `if (settings.printMissing)`; `build.cc:142-154` `--dry-run` only), and the elision is performed by the build **goals** (Worker-side, no `pendingDrvs_`), so the design must *add* a client-side `pendingDrvs_`-aware flush pre-pass of `queryMissing`'s shape; (ii) **LD-S2 formula** omitted `targets` (a substitutable target's own `.drv` is read by its trampoline, `:124`) — fixed to `targets ∪ willBuild ∪ {immediate inputDrvs of willBuild}`; (iii) **missed TOCTOU** between the submission frontier and the goals' build-time substitutability re-decision → new obligation LD-S6; (iv) **stale §10 item 1** still stated the round-2/4 "win is false / whole closure before substitution" that V1 overturned — realigned to V1 (the same incomplete fold-in that left §4.2 item 1 stale). | `shared.cc:56-72` + `installables.cc:652,655` + `build.cc:142-154` (queryMissing = gated display; build is `buildPathsWithResults`) + `derivation-trampoline-goal.cc:124` (target always read) + `derivation-goal.cc:138` vs `:151`/`:229` |
| **A4** | Delegated law-research + R9 verification (this revision) | A background subagent enumerated the full law inventory across the four §0.5 pillars; **each load-bearing claim was re-walked against master before folding** (R9), not trusted. Verified+added: LD-P7 (`computeP` triviality, `derivations.cc:116`), LD-P8 (reference-set fidelity, `:109-111`), LD-S7/S8/S9 (selective coherence / static-deps superset / `queryMissing` partition + the `misc.cc:208` enabling-change), a **three**-reader LD-S4 (third = `derivation-resolution-goal.cc:59`, gated `impure-derivations` — the draft listed two), and the LD-D2–D5 semilattice expansion (`store-api.cc:221`,`:229`; `local-store.cc:957`,`:960`,`:974`). Corrected: the LD-V1 boundary-audit draft asserted `EXPECT_FALSE(isValidPath)`, which **fails on master** (eager write — `boundary-audit-derivation-strict.cc:34` reads the drv back) → re-marked feature-gated. Honest negatives kept: Pillar C is mostly re-description (only C1/C3 new); `Arbitrary<Derivation>` doesn't exist (must be built); the `pendingDrvs_`-gated laws (P6/C3/S6-mitigation/S9-gate) are aspirational. | `derivation-resolution-goal.cc:57-59` + `boundary-audit-derivation-strict.cc:34` + `store-api.cc:221,229` + `local-store.cc:952-974` (each re-walked) |
| **A5** | Full-document adversarial re-read (this revision; R1) | An end-to-end re-read caught a **`readOnlyMode` contradiction** the round-6 §4.2 edit had half-created: §2 and the §7 table treated `nix eval` (and `nix build --dry-run`) as *read-only / 0 writes today*, but **neither sets `readOnlyMode`** (verified against the full setter set; `nix eval` has none, and `build.cc:142-154` returns before `Installable::build`), so **both write `.drv`s eagerly today** — `nix eval` via `derivationStrict` (`primops.cc:2029`), `--dry-run` via `toDerivedPaths` instantiation. Fixed §2's command list + citations (real setters: `repl.cc:582`/`search.cc:65`/`nix-env.cc:990`/`nix-instantiate.cc:184`/`log.cc:29`; `installables.cc:235` is the opt-in `--read-only` flag), split the §7 dry-run/`nix eval` row (dry-run *elides* via the `pendingDrvs_`-aware `queryMissing`; `nix eval` *defers to the output boundary* — same write, not elided), and aligned §7's stale `:121-126`/`:209-214` cites to `:122-125`/`:211-213`. Also updated the status banner to record A3–A5. | `grep "readOnlyMode = true" src/` (full setter set) + `build.cc:142-154` (dry-run returns pre-build) + `eval.cc` run() (no setter) + `primops.cc:2029` |

Net: the **write-deferral + bulk-submission** core is sound; the **eval-store mass-instantiation** workload is the clean win; the **atomic-transaction** claim (C3) was corrected; the **skip-substituted-closure** claim swung from over-stated (round-1) to over-retracted (round-2/4) to its true scope by independent review (V1: *input* closures elide where they substitute, the *target* `.drv` does not); the **"two schedulers"** structure was collapsed to **one flush barrier + two path-discovery front-ends** (S1); memoisation/idempotence are explicit (B/C); the bulk-API unification is scoped honestly (D); the narHash fix is confirmed free (V2); and the §5 soundness argument was moved off the instantiation-order claim that the warm-cache path violates onto the build-time `.drv`-bytes boundary (V4). The four parallel reviewers' load-bearing findings were each re-verified against master by the delegator (R9), not trusted as reported.
