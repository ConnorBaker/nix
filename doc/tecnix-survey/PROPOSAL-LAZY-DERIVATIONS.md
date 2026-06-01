# Virtual Derivations: Deferred, Batched `.drv` Materialisation

This document describes a design for making `.drv` writing **lazy and batched**, by the same content-addressed placeholder pattern the source-materialisation architecture already applies to source trees ([PROPOSAL.md §0](./PROPOSAL.md#0-underlying-pattern)). The thesis in one line: **a derivation is the output of an evaluator-level "build" — the evaluator computes its identity (`drvPath`) eagerly from in-memory data, threads that identity through evaluation, and materialises the `.drv` file (and its as-yet-unmaterialised source inputs) only at a resolution boundary, in one bulk flush.**

It is written in the timeless component/interplay style of [PROPOSAL.md](./PROPOSAL.md): what each piece does and how the pieces interplay, with `file:line` citations into the tree as a reading aid. Every "today X happens / after the change Y happens" claim is a *simulated* walk of the executing code path, per [REVIEW-DISCIPLINE.md](./REVIEW-DISCIPLINE.md) R1 — not a citation that a function exists.

> **Status: adversarially reviewed over five rounds (REVIEW-DISCIPLINE R1, simulated against master), including self-review and a final round of four parallel *independent* reviewers whose load-bearing findings were each re-verified against master by the delegator (R9).** The review reshaped the design materially — see the [Findings ledger](#findings-ledger) for what was retracted, corrected, or restructured (the independent round is V1–V4). In one line: the **write-deferral** half (async, DetSys-style) is sound; the **"one atomic transaction"** framing was wrong (the cited `addMultipleToStore` is N per-path transactions, [§4.3](#43-the-batched-store-flush)); the **"skip materialising substituted-away closures"** win is *partial* — the **target** `.drv` of any built output is unavoidable, but a fully-substitutable target reads **none** of its input `.drv`s, so the input-closure writes *are* elided where the inputs substitute ([§7](#7-performance-characteristics), C1); the real, clean win is **eval-store mass instantiation** (Hydra/CI: non-read-only, IFD-free, build-free — [§2](#2-the-unblock-primitive)), not read-only `nix eval`; and the **"two schedulers"** structure was collapsed to **one flush barrier + two path-discovery front-ends** (the `.drv` side is a write-queue, not a scheduler — [§4](#4-the-design)), with memoisation made explicit ([§4.1](#41-the-drv-write-queue)).

It builds on three precedents already in the tree or in the surveyed forks, and is explicit about which mechanism it borrows from each:

- the **source** `SourceVirtual` placeholder + `MaterialisationScheduler` + batched `outPathsOf` chokepoint ([PROPOSAL.md §2.O, §6.2](./PROPOSAL.md#o-sourceplaceholder--materialisationscheduler--content-keyed-memo)) — this design shares its *resolution-at-a-boundary* shape and reuses the source scheduler unchanged, but the `.drv` side is a write-queue, not a second scheduler ([§4](#4-the-design));
- upstream's **`DownstreamPlaceholder` + `Derivation::tryResolve`** (`downstream-placeholder.hh`, `derivations.cc:1151`) — the existing proof that a derivation's *output* path can be unknown at eval time and resolved at a boundary;
- DetSys's **`AsyncPathWriter`** (`nix-src/src/libstore/async-path-writer.{hh,cc}`) — the existing proof that the `.drv` *write* can be moved off the eval thread, **and** (in its disabled `#if 0` block) the first attempt at the bulk `addMultipleToStore` flush this design completes.

---

## Contents

- [0. The pattern: a derivation is an evaluator-level build output](#0-the-pattern)
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

**Simulated.** With `settings.readOnlyMode` set (`nix eval`, `nix search`, `nix-instantiate --eval`, `repl` — `installables.cc:235`, `nix-env.cc:990`, `repl.cc:582`, etc.), step 5 above takes the `computeStorePath(*state.store, drv)` branch: `drvPath` is computed purely in-memory and **`writeDerivation` is never called**. Steps 6-7 then build the complete attrset — `drvPath`, `outPath`, all per-output strings — and `prim_derivationStrict` returns it. Evaluation continues; consumers interpolate `.drvPath`/`.outPath` as strings. No `.drv` is on disk, and nothing in the returned value requires one.

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
- populate `drvHashes` exactly as today (`primops.cc:2038`) — **unchanged and load-bearing** ([§5](#5-soundness-obligations));
- build and return the attrset exactly as today.

**Memoisation — at three existing layers, plus one deliberate keying choice.** The design does not re-attempt work already done. Dedup is not a new mechanism; it falls out of layers already in the tree:

1. **`drvPath`-keyed enqueue (the one keying choice this adds).** `derivationStrict` is forced once per *textually distinct* call site (the arg thunk is a force-once `Value`), but two *different* call sites can produce the same `drvPath` (the same derivation reached two ways). Keying `pendingDrvs_` on `drvPath` makes the second enqueue a no-op. This is a small *improvement over DetSys*, whose `AsyncPathWriter::addPath` does `insert_or_assign` on the future but an **unconditional** `push_back` of the item (`async-path-writer.cc:92-103`) — so it would queue the same `.drv` twice and write it twice (the second write harmless but wasted). The map key removes the duplicate before it reaches the queue.
2. **`isValidPath` skip at write time.** Even an un-deduped enqueue is cheap: `writeDerivation` bails on `isValidPath` (`derivations.cc:143`, `if (isValidPath(path) && !repair) return path;`), and the bulk `addMultipleToStore`'s `processGraph` skips any already-valid node (`store-api.cc`). So a `.drv` already on disk (this process or a prior one) is never rewritten.
3. **`drvHashes` content-hash memo.** `hashDerivationModulo(drvPath)` is memoised in a `concurrent_flat_map` (`drvHashes`, declared `derivations.cc:858`; the idempotent `insert_or_assign` is at `derivations.cc:875` in `pathDerivationModulo` and `primops.cc:2038` at instantiation) so the recursive modulo-hash is computed once per `drvPath` per process ([§5](#5-soundness-obligations)).

The deepest memo is content-addressing itself: a fresh process that re-evaluates the same derivation computes the identical `drvPath`, finds it already valid, and skips — cross-process re-materialisation is free.

**Idempotent re-flush.** Flushing is not all-or-nothing at end-of-eval; it fires at every boundary ([§4.2](#42-resolution-boundaries)). Re-flush must not re-submit already-written entries — and the `AsyncPathWriter` harness gives this directly: the worker `std::swap`s the pending `items` vector out before writing (`async-path-writer.cc:52`), and `waitForAllPaths` `std::move`s the `futures` map out (`:124`), so a processed entry has left both structures. A second flush sees only what was enqueued since the first. (Verified by reading the harness, not assumed.)

A `VirtualDerivation` is not a new value type or a new `NixStringContextElem` variant — that is a deliberate non-goal ([§9](#9-non-goals-and-boundaries), and the [REVIEW-DISCIPLINE.md](./REVIEW-DISCIPLINE.md) R3 hazard a new context variant would incur). The existing `DrvDeep{drvPath}` and `Built{drvPath, output}` context elements already carry the `drvPath`; "virtual" is purely a *store-side* property — the path is named but its bytes are pending — exactly as a source `Opaque{stand-in}` is named but pending ([PROPOSAL.md §6.1](./PROPOSAL.md#61-input-materialisation)).

### 4.2. Resolution boundaries

A pending `.drv` must be flushed to disk before any operation that reads it back **as a file**. Enumerated by simulation (R1) — every site that needs the bytes, not just the path string:

1. **Build (the whole `.drv` closure, not just the target — Finding C1).** Building requires the target `.drv` *and every `.drv` in its input closure* valid on disk, before substitution. Simulated: `DerivationTrampolineGoal` loads the drv via `for (drvStore : {evalStore, store}) if (isValidPath(drvPath)) return readDerivation(drvPath); assert(false)` (`derivation-trampoline-goal.cc:121-127`) — an invalid `.drv` is not "rebuilt", it trips `assert(false)`; and `DerivationBuildingGoal` walks each input drv with the same `if (isValidPath(depDrvPath)) … else assert(false)` (`derivation-building-goal.cc:209-214`). So the build boundary must flush the *transitive* `.drv` closure. The CLI build entry points are where DetSys already places `waitForPath`/`waitForAllPaths` ([§3](#3-detsys-asyncpathwriter)); this design reuses the identical barrier set, made closure-transitive.
2. **Eval-cache `forceDerivation` (Finding C2 — a boundary the first draft missed).** On a *warm eval-cache hit*, `AttrCursor::forceDerivation` (`eval-cache.cc:799-815`) returns the cached `drvPath` string **without running `derivationStrict`**; if `!readOnlyMode && !isValidPath(drvPath)` it `forceValue()`s to regenerate and, if *still* invalid, throws `"don't know how to recreate store derivation"`. Under naive deferral the regeneration re-defers → stays invalid → **throws**. `forceDerivation` has exactly two callers — `installable-flake.cc:92` and `app.cc:129` (the common `nix build`/`nix run` paths). It is a *mandatory* flush site: `forceDerivation` must drain the pending `.drv` (gated, like the production code, on `!readOnlyMode`).
3. **In-eval `realiseContext` (IFD + `DrvDeep`).** `realiseContext`'s `Built` arm calls `buildStore->buildPaths(...)` *during eval* (`primops.cc:172`), reached by import-from-derivation and by the `DrvDeep` whole-closure edge (`primops.cc:1855` `computeFSClosure`). Both need the named `.drv` (and inputs) valid mid-eval. **For the target workload this is rare to absent:** Nixpkgs forbids IFD, and a `DrvDeep` edge arises only from `unsafeDiscardOutputDependency`-style constructs, not ordinary `${pkg}` interpolation (which is `Built`/`ensureSlot`, a pure in-memory insert — `primops.cc:1864`, simulated in [§1](#1-what-derivationstrict-does-today)). Where IFD *does* fire (non-Nixpkgs evals; IFD is default-on in Nix — `eval-settings.hh:214`), it forces a full build mid-eval anyway, so a `.drv` flush there is lost in the build's noise. Still, the flush-on-read guard must cover it for correctness.
4. **`import (drv.drvPath)` / `readDerivation` / `readInvalidDerivation`** (`store-api.cc:1168` `readDerivationCommon`). Reading a `.drv` back as a Nix value requires its bytes.
5. **CLI/C-API observation** — `nix show-derivation`, `nix derivation show`, copying a `.drv` to a binary cache, `--add-root` on a `.drv`. These are the `waitForAllPaths` sites.

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

A concrete payoff: `nix build --dry-run` over a flake (pure instantiation, thousands of `.drv`s, **zero builds**) today copies every `lib.cleanSource ./.` and writes every `.drv`. After this design it writes nothing — `--dry-run` *is* the read-only-shaped path, and a dry run that produces no store mutations is both faster and more honest about being a dry run.

## 7. Performance characteristics

"walk" = one tree traversal; "copy" = walk + store write; "txn" = one SQLite transaction / daemon round-trip.

| Scenario | Today | After this design |
|---|---|---|
| **Eval-store mass instantiation** (Hydra/`nix-eval-jobs`: M `.drv`s, no build, no IFD) — *the headline win* | M `.drv` writes (M daemon round-trips) + per-drv source copies | writes + copies deferred and flushed in bulk at end-of-eval (parallel ingestion, fewer round-trips); overlaps eval if async |
| `nix build --dry-run` / `nix eval .#x.drvPath`, source-heavy flake | read-only: already 0 `.drv` writes; N source copies | 0 source copies, 0 `.drv` writes — nothing flushed (only the source-copy deferral is new here) |
| `nix build` of 1 target, M-derivation closure, all built from source | M `.drv` writes + per-drv source copies | M `.drv` writes + source copies bulk-flushed; writes overlap eval if async — **same M writes, fewer round-trips, not fewer writes** |
| `nix build` where the target is substitutable from a cache | full instantiation: M `.drv` writes + source copies; substitution then skips most *builds* | the **target** `.drv` must be written (it is read to even construct the goal); a fully-substitutable target reads **none** of its input `.drv`s (it returns `Substituted` before the input-drv goals exist), so the input-closure `.drv` writes + source copies are **elided** exactly where the inputs substitute |
| Deep dependency chain (A→B→…→Z), then build from source | each `.drv` write a separate round-trip | bulk-flushed at the build boundary (reference-ordered); still N writes (the inputs are built, not substituted) |

**Finding C1 (re-narrowed by independent review) — the substituted-away-input win is real; the *target* `.drv` is not skippable.** The first draft over-claimed both ways and an independent code-walk corrected it. The precise truth, simulated against master:

- **The target `.drv` is always required for a build attempt.** `DerivationTrampolineGoal` loads it (`derivation-trampoline-goal.cc:121-126`, `if (isValidPath) readDerivation … else assert(false)`); an `Opaque` (deferred) target `.drv` that nothing can produce reaches that `assert(false)` via a re-entrant obtain-goal. So a deferred `.drv` *that a build needs* must be flushed first — the [§4.2](#42-resolution-boundaries) build boundary does exactly this.
- **But a fully-substitutable target reads *none* of its input `.drv`s.** `haveDerivation` attempts substitution of the *target's outputs* first and `co_return`s `Substituted` at `derivation-goal.cc:138` — **before** `makeDerivationResolutionGoal` (`:151`) or `makeDerivationBuildingGoal` (`:229`), the only goals that read the input `.drv` closure, are ever created. The input-closure validity requirement (`gaveUpOnSubstitution` → `derivation-building-goal.cc:209-214`, every input `.drv` valid else `assert(false)`) is reached **only after the per-target substitution attempt fails** — i.e. only for the subset actually built from source.

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

1. **(RESOLVED — Finding C1) Substitutability ordering.** *Answered:* the build path requires the `.drv` (and its whole input closure) valid on disk **before** substitution. `DerivationTrampolineGoal` (`derivation-trampoline-goal.cc:121-127`) and `DerivationBuildingGoal` (`derivation-building-goal.cc:209-214`) both `assert(false)` on an invalid `.drv`, ahead of `haveDerivation`'s substituter query (`derivation-goal.cc:86`). So the "skip substituted-away `.drv`s" win is **false**; the build-path win is async+batched-write + source-copy elision only. *Residue:* whether teaching `queryMissing`'s `res.unknown` shortcut (`misc.cc:208`) to actually substitute a missing `.drv` (its own `FIXME`) could recover part of the win — a separate, larger change.
2. **(RESOLVED — Findings M1/C2) In-eval flush frequency.** *Answered:* the dominant target workload (Nixpkgs/Hydra) is **IFD-free** (Nixpkgs forbids it; Hydra cannot build during eval), so the `realiseContext`/`Built` flush does not fire there; the `DrvDeep`/`computeFSClosure` edge arises only from `unsafeDiscardOutputDependency`-style constructs, not `${pkg}` interpolation (`Built`/`ensureSlot`, in-memory — `primops.cc:1864`). The first draft over-weighted IFD and **missed** the genuinely common in-eval boundary, `forceDerivation` on a warm eval cache (C2, `eval-cache.cc:799`). *Residue:* the `forceDerivation` flush must be implemented and tested on the warm-cache `nix build` path; without it that path throws.
3. **(R5, RESOLVED by independent review) Latency floor of the per-item narHash.** *Answered:* it is genuinely free, not a hidden second pass. `addToStoreFromDump` already computes a NAR hash for any non-`NixArchive`-SHA256 object (`local-store.cc:1271-1274`: `if (dumpMethod != NixArchive || hashAlgo != SHA256) { HashSink narSink; dumpPath(realPath, narSink); narHash = narSink.finish(); }`), and a `.drv` is `Raw::Text`/`Flat` — so that branch *already fires* on every `.drv` write today. The batched route computes the same hash the eager route does; it is not net-new work.
4. **(R4) Blast radius — confirmed nil.** This design changes *when* `.drv`s and sources are written, not *what* — `drvPath`, `outPath`, and the `.drv` contents are byte-identical to today. No rebuild scope. R2: `drvPath = hash(unparse(drv))` in both the eager and deferred paths, over the same `drv` struct.
5. **(R6) Alternatives.** (a) DetSys's async-only writer — adopted as the *base*, extended (it leaves source copies eager and writes per-item). (b) A daemon-side deferred-write registry — rejected: the pending state needs `EvalState` context the daemon lacks; the flush must be client-side before build submission. (c) Doing nothing and relying on OS page-cache — rejected on R5: the cost is fsync + SQLite txn commit per `.drv` (`registerValidPaths` syncs when `syncBeforeRegistering`, `local-store.cc:945`), which the page cache does not amortise.

A review of this document that does not walk items 1-2 against the source is a citation audit, not an adversarial review (R8). *(They have now been walked; the Findings ledger records the result.)*

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
| **V3** | Independent review (round-5) | Citation fixes against master: `isValidPath` bail is `derivations.cc:143` (not `:151`); `drvHashes` `insert_or_assign` is `:875`/`primops.cc:2038` (`:858` is the declaration); member `writeDerivation` is `store-api.hh:956` (`derivations.hh:602` is the unrelated serialiser); `forceDerivation` callers are `installable-flake.cc:92` + `app.cc:129` (not `installable-attr-path.cc:93`); `waitForAllPaths` move is `async-path-writer.cc:125`. All applied. | direct re-walk of each cited line |
| **V4** | Independent review (round-5) | §5 `drvHashes` discharge had a **hole**: "A instantiated before B" fails on a warm eval-cache hit (neither B nor A instantiated → cold sub-closure). Benign because build goals read `.drv` *bytes* (`readDerivation`), never recompute `hashDerivationModulo`. §5 rewritten to rest on the build-time bytes boundary, not instantiation order. | `derivation-trampoline-goal.cc:124` (bytes read) + grep: no `hashDerivationModulo` caller in `build/` |

Net: the **write-deferral + bulk-submission** core is sound; the **eval-store mass-instantiation** workload is the clean win; the **atomic-transaction** claim (C3) was corrected; the **skip-substituted-closure** claim swung from over-stated (round-1) to over-retracted (round-2/4) to its true scope by independent review (V1: *input* closures elide where they substitute, the *target* `.drv` does not); the **"two schedulers"** structure was collapsed to **one flush barrier + two path-discovery front-ends** (S1); memoisation/idempotence are explicit (B/C); the bulk-API unification is scoped honestly (D); the narHash fix is confirmed free (V2); and the §5 soundness argument was moved off the instantiation-order claim that the warm-cache path violates onto the build-time `.drv`-bytes boundary (V4). The four parallel reviewers' load-bearing findings were each re-verified against master by the delegator (R9), not trusted as reported.
