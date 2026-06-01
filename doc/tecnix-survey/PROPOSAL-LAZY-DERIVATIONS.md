# Virtual Derivations: Deferred, Batched `.drv` Materialisation

This document describes a design for making `.drv` writing **lazy and batched**, by the same content-addressed placeholder pattern the source-materialisation architecture already applies to source trees ([PROPOSAL.md §0](./PROPOSAL.md#0-underlying-pattern)). The thesis in one line: **a derivation is the output of an evaluator-level "build" — the evaluator computes its identity (`drvPath`) eagerly from in-memory data, threads that identity through evaluation, and materialises the `.drv` file (and its as-yet-unmaterialised source inputs) only at a resolution boundary, in one bulk flush.**

It is written in the timeless component/interplay style of [PROPOSAL.md](./PROPOSAL.md): what each piece does and how the pieces interplay, with `file:line` citations into the tree as a reading aid. Every "today X happens / after the change Y happens" claim is a *simulated* walk of the executing code path, per [REVIEW-DISCIPLINE.md](./REVIEW-DISCIPLINE.md) R1 — not a citation that a function exists.

> **Status: adversarially reviewed (REVIEW-DISCIPLINE R1, simulated against master).** The review reshaped the design materially — see the [Findings ledger](#findings-ledger) below for what was retracted or corrected. In one line: the **write-deferral** half (async, DetSys-style) is sound; the **"one atomic transaction"** framing was wrong (the cited `addMultipleToStore` is N per-path transactions, [§4.3](#43-the-batched-store-flush)); the **"skip materialising substituted-away closures"** win is *false for builds* (the build path requires the whole `.drv` closure valid before it runs, [§7](#7-performance-characteristics)); and the real, clean win is **eval-store mass instantiation** (Hydra/CI: non-read-only, IFD-free, build-free — [§2](#2-the-unblock-primitive)), not read-only `nix eval`.

It builds on three precedents already in the tree or in the surveyed forks, and is explicit about which mechanism it borrows from each:

- the **source** `SourceVirtual` placeholder + `MaterialisationScheduler` + batched `outPathsOf` chokepoint ([PROPOSAL.md §2.O, §6.2](./PROPOSAL.md#o-sourceplaceholder--materialisationscheduler--content-keyed-memo)) — this design is the *derivation-side analogue*;
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
  - [4.1. `VirtualDerivation` + `DerivationMaterialisationScheduler`](#41-virtualderivation--derivationmaterialisationscheduler)
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

**But read-only is the wrong frame for the *win* (review correction).** Read-only already writes nothing, so deferral changes nothing there — it only *proves the premise*. The workload where deferral is both **live** and **valuable** is **eval-store mass instantiation**: the Hydra / `nix-eval-jobs` model that evaluates a whole jobset to thousands of `.drv`s and builds them elsewhere. That pass is driven by `nix-instantiate` writing to a `--eval-store` (`common-eval-args.cc:141` — "to store derivations (`.drv` files) and inputs referenced by them"), and it is **not** read-only: `nix-instantiate` sets `readOnlyMode` only under `--eval` (`nix-instantiate.cc:183`); the `.drv`-producing path runs `requireDrvPath` + `addPermRoot` (`:93-111`). So this is precisely where today's eager write fires N thousand times and where deferral + batching has its longest uninterrupted window. Three properties of this workload, each verified, make it the clean case — and they are exactly the properties the [findings](#findings-ledger) show the *build* path lacks:

- **non-read-only** → the deferral is actually exercised (a `.drv` *is* normally written, so not-writing-it-yet is a real change);
- **IFD-free** → Nixpkgs forbids import-from-derivation precisely because Hydra evaluates without building and so cannot perform the mid-eval build IFD needs; with no IFD there is **no in-eval `realiseContext` flush** to interrupt the batch ([§4.2](#42-resolution-boundaries));
- **build-free** → the instantiation pass builds nothing, so the build path's "`.drv` closure must be valid first" constraint ([§7](#7-performance-characteristics), Finding C1) never fires.

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
- **`waitForPath(path)` / `waitForAllPaths()`** (`:109`, `:121`) are the sync barriers, scattered across every CLI entry that needs a `.drv` on disk: `installable-flake.cc:93`, `installable-attr-path.cc:93`, `app.cc:77`, `nix-build.cc:462`, `repl.cc:314`, `nix-env`/`user-env.cc`, `flake.cc:496`, the C API (`nix_api_expr.cc`, `nix_api_value.cc:398`). `prim_derivationStrict` routes through it via a `writeDerivation(*state.asyncPathWriter, drv, …)` overload (`primops.cc:1881`).
- It is **`.drv`-only**: sources (`addPath`/`fetchToStore`) do not go through it.

Two facts about DetSys's implementation directly shape this design:

1. **It also computes `drvPath` eagerly** (the synchronous hash). This independently confirms [§2.2](#22-intrinsic-vs-deferrable): even the fork that most aggressively defers cannot defer the path computation. So this design does not attempt to either.
2. **The batched store transaction is written and disabled.** `async-path-writer.cc:134-156` is a `#if 0` block that builds a `Store::PathsSource` and calls `store->addMultipleToStore(sources, act, repair)` — exactly the bulk path — guarded by the comment `// FIXME: addMultipleToStore() shouldn't require a NAR hash.` This design's [§4.3](#43-the-batched-store-flush) is, in essence, *that block, with the FIXME resolved.*

## 4. The design

The design has three components, mirroring the source-side trio of `SourcePlaceholder` / `MaterialisationScheduler` / batched `outPathsOf`.

### 4.1. `VirtualDerivation` + `DerivationMaterialisationScheduler`

`prim_derivationStrict` (step 5 of [§1](#1-what-derivationstrict-does-today)) stops calling `writeDerivation` unconditionally. Instead:

- compute `drvPath` in-memory via `computeStorePath` (always — the read-only branch generalised to all modes);
- register the pair `(drvPath, drv)` with a per-`EvalState` **`DerivationMaterialisationScheduler`** (the derivation-side analogue of `MaterialisationScheduler`, `materialisation-scheduler.hh`), which holds:
  - `pendingDrvs_ : concurrent_flat_map<StorePath, BasicDerivation>` — the unwritten `.drv`s, keyed by their (already content-determined) path;
  - a coalescing/dedup discipline identical to the source scheduler's `inFlight_`: a `drvPath` already pending or already valid is a no-op (`writeDerivation` already bails on `isValidPath`, `derivations.cc:151`);
- populate `drvHashes` exactly as today (`primops.cc:2038`) — **unchanged and load-bearing** ([§5](#5-soundness-obligations));
- build and return the attrset exactly as today.

A `VirtualDerivation` is not a new value type or a new `NixStringContextElem` variant — that is a deliberate non-goal ([§9](#9-non-goals-and-boundaries), and the [REVIEW-DISCIPLINE.md](./REVIEW-DISCIPLINE.md) R3 hazard a new context variant would incur). The existing `DrvDeep{drvPath}` and `Built{drvPath, output}` context elements already carry the `drvPath`; "virtual" is purely a *store-side* property — the path is named but its bytes are pending — exactly as a source `Opaque{stand-in}` is named but pending ([PROPOSAL.md §6.1](./PROPOSAL.md#61-input-materialisation)).

### 4.2. Resolution boundaries

A pending `.drv` must be flushed to disk before any operation that reads it back **as a file**. Enumerated by simulation (R1) — every site that needs the bytes, not just the path string:

1. **Build (the whole `.drv` closure, not just the target — Finding C1).** Building requires the target `.drv` *and every `.drv` in its input closure* valid on disk, before substitution. Simulated: `DerivationTrampolineGoal` loads the drv via `for (drvStore : {evalStore, store}) if (isValidPath(drvPath)) return readDerivation(drvPath); assert(false)` (`derivation-trampoline-goal.cc:121-127`) — an invalid `.drv` is not "rebuilt", it trips `assert(false)`; and `DerivationBuildingGoal` walks each input drv with the same `if (isValidPath(depDrvPath)) … else assert(false)` (`derivation-building-goal.cc:209-214`). So the build boundary must flush the *transitive* `.drv` closure. The CLI build entry points are where DetSys already places `waitForPath`/`waitForAllPaths` ([§3](#3-detsys-asyncpathwriter)); this design reuses the identical barrier set, made closure-transitive.
2. **Eval-cache `forceDerivation` (Finding C2 — a boundary the first draft missed).** On a *warm eval-cache hit*, `AttrCursor::forceDerivation` (`eval-cache.cc:799-815`) returns the cached `drvPath` string **without running `derivationStrict`**; if `!readOnlyMode && !isValidPath(drvPath)` it `forceValue()`s to regenerate and, if *still* invalid, throws `"don't know how to recreate store derivation"`. Under naive deferral the regeneration re-defers → stays invalid → **throws**. This is the common `nix build` path (`installable-flake.cc:92`, `installable-attr-path.cc:93`). It is a *mandatory* flush site: `forceDerivation` must drain the pending `.drv` (gated, like the production code, on `!readOnlyMode`).
3. **In-eval `realiseContext` (IFD + `DrvDeep`).** `realiseContext`'s `Built` arm calls `buildStore->buildPaths(...)` *during eval* (`primops.cc:172`), reached by import-from-derivation and by the `DrvDeep` whole-closure edge (`primops.cc:1855` `computeFSClosure`). Both need the named `.drv` (and inputs) valid mid-eval. **For the target workload this is rare to absent:** Nixpkgs forbids IFD, and a `DrvDeep` edge arises only from `unsafeDiscardOutputDependency`-style constructs, not ordinary `${pkg}` interpolation (which is `Built`/`ensureSlot`, a pure in-memory insert — `primops.cc:1864`, simulated in [§1](#1-what-derivationstrict-does-today)). Where IFD *does* fire (non-Nixpkgs evals; IFD is default-on in Nix — `eval-settings.hh:214`), it forces a full build mid-eval anyway, so a `.drv` flush there is lost in the build's noise. Still, the flush-on-read guard must cover it for correctness.
4. **`import (drv.drvPath)` / `readDerivation` / `readInvalidDerivation`** (`store-api.cc:1168` `readDerivationCommon`). Reading a `.drv` back as a Nix value requires its bytes.
5. **CLI/C-API observation** — `nix show-derivation`, `nix derivation show`, copying a `.drv` to a binary cache, `--add-root` on a `.drv`. These are the `waitForAllPaths` sites.

The flush trigger is therefore **two-tier**, exactly like the source `resolveSourceVirtualContext` chokepoint plus its per-path `ensureLazyPathCopied`:

- a **bulk flush at the artificial boundary** (end-of-eval / `waitForAllPaths`, and each build submission) — the batched fast path ([§4.3](#43-the-batched-store-flush));
- a **targeted flush-on-read** (`waitForPath(drvPath)` / a guard inside `forceDerivation`, `readInvalidDerivation`, and the `realiseContext`/`DrvDeep` arm) for the in-eval read-back, transitively flushing that `.drv`'s pending input `.drv`s first.

The flush-on-read guard is the same *shape* as the source-side boundary audit ([PROPOSAL.md §6.2](./PROPOSAL.md#62-the-addpath-source-virtualisation-boundary)): a finite, enumerable set of sites that consume the artifact, each of which must drain before consuming. The audit surface is `git grep` for `forceDerivation` / `readInvalidDerivation` / `readDerivation` / `computeFSClosure` / `buildPaths` callers reachable from eval.

### 4.3. The batched store flush

At the bulk boundary, instead of N independent `writeDerivation` calls (each its own daemon round-trip), flush all pending `.drv`s — and all deferred source copies — through one bulk submission. **A round-1 draft of this section claimed this is a single atomic transaction via `addMultipleToStore`; that is wrong, and the correction (Finding C3) matters for what the win actually is.**

**What `addMultipleToStore` actually does (simulated, `store-api.cc:195`).** It drives `processGraph` — a `ThreadPool`-backed, topologically-ordered, reference-aware walk (`thread-pool.hh:90`) — calling, per node, `addToStore(info, source)` (`local-store.cc:1027`), which ends in `registerValidPath(info)` *singular* (`:1122`) → `registerValidPaths({…one…})` → **one `SQLiteTxn` per path** (`:935`, `:952`). So the bulk path is **N parallel, reference-ordered, single-path transactions**, *not* one atomic transaction. The win is real but narrower than "one txn": fewer daemon round-trips than N separate `writeDerivation` calls (one framed `AddMultipleToStore` op carries the whole batch on the wire — `remote-store.cc:470`), parallel ingestion, and the deferral lets the writes overlap continued eval if run async. It is **not** one fsync and **not** atomic across the batch.

**Where the *atomic* two-phase property lives, if it is wanted.** `LocalStore::registerValidPaths(ValidPathInfos)` *plural* (`local-store.cc:938`) *is* a single `SQLiteTxn`, two-phase (loop 1 `addValidPath` all, loop 2 wire `AddReference`, then `topoSort` for cycles, `:952-991`) — so intra-batch references are legal there. But `addMultipleToStore` does **not** call it, and it operates on **already-on-disk** paths (it registers validity; it does not copy bytes). To get atomic registration you would therefore run an explicit **two phases**: (1) copy every pending `.drv`/source's bytes into the store (the `addToStoreFromDump`/restore work), then (2) one `registerValidPaths`-plural transaction over all of them. That is a deliberate design choice (atomicity vs. the simpler per-path path), not the free property the first draft implied — call it out, don't assume it.

**The narHash friction (DetSys's `#if 0` FIXME) — still real on the `addMultipleToStore` route.** A `.drv` is `text:sha256` over *flat* bytes (`Raw::Text`, `FileSerialisationMethod::Flat`, `derivations.cc:150-151`). `addToStore(info, source)` re-hashes the incoming NAR and throws on `hashResult.hash != info.narHash` (`local-store.cc:1062`); there is no `nar:sha256` in hand for a flat text object. Resolution: **compute the per-item `narHash` when building the batch** — for a `.drv` ATerm (kilobytes) a NAR dump + SHA-256 is negligible and is the same pass `addToStoreFromDump` does internally; it satisfies the existing contract with no store-API change. (The alternative — a `Raw::Text` `addMultipleToStore` variant that skips the NAR verify — is deferred: it widens a security-relevant verify path, R4.)

Deferred **source** copies flush in the *same* batch: a source's `inputSrcs` entry is named by its narHash (intrinsic, already computed), so its `ValidPathInfo` is fully determined; the copy supplies the bytes. This is the cross-derivation batching the per-`derivationStrict` `outPathsOf` cannot do — N derivations sharing sources, or a deep dependency chain, copy and register in one bulk submission.

## 5. Soundness obligations

Each is stated with the invariant it rests on and how it is discharged.

- **The `drvHashes` memo makes deferral sound within a process.** *Obligation:* a derivation `B` that depends on `A` computes `hashDerivationModulo(B)`, which recurses through `B`'s `inputDrvs` via `pathDerivationModulo` → on a cold `drvHashes` entry, `readInvalidDerivation(A.drvPath)` — a **disk read of `A`'s `.drv`** (`derivations.cc`, `store-api.cc:1218`). If `A`'s write were deferred and `drvHashes` cold, this read would fail. *Discharge:* `drvHashes` is populated for every derivation at instantiation (`primops.cc:2038-2039`), and `A` is instantiated before `B` (it is `B`'s dependency). So within one eval process the memo always answers and the disk read never fires. The disk read is the cross-process / fresh-process path, where `A` is already written and valid. **This obligation is why `drvHashes` population stays exactly as today and is not itself deferred.** (R2: the memo key is `drvPath`, a content hash — process-independent — so a second process recomputes the identical key.)
- **Content-addressing closes the mint-vs-write gap.** *Obligation:* the `drvPath` returned at instantiation must equal the path eventually registered. *Discharge:* both are `makeFixedOutputPathFromCA(hash(unparse(drv)))` over the same in-memory `drv` — equal by construction. `writeDerivation`'s `assert(path2 == path)` (`derivations.cc:159`) is the existing backstop; it holds verbatim.
- **Intra-batch references resolve.** *Obligation:* a batched `.drv` referencing a same-batch source/`.drv` must register without a "missing reference" error. *Discharge:* depends on the chosen flush route ([§4.3](#43-the-batched-store-flush), Finding C3). The `addMultipleToStore` route handles it by `processGraph`'s **reference-ordered** ingestion (a referent is added after its references — `store-api.cc:195`), not by atomicity. The explicit two-phase route handles it by `registerValidPaths`-plural inserting all paths before wiring any reference (`local-store.cc:952-991`). Either works; the proposal must not claim the *former* gives the *latter*'s atomicity.
- **Flush-on-read covers every in-eval read-back.** *Obligation:* no eval-time consumer reads a pending `.drv`'s bytes. *Discharge:* the enumerated boundary set ([§4.2](#42-resolution-boundaries)) with `git grep`-able audit surface. The in-eval byte consumers are `AttrCursor::forceDerivation` (warm eval-cache, Finding C2), `realiseContext`/`buildPaths` (IFD + `DrvDeep`), and `readInvalidDerivation` — each gated by a flush that transitively drains pending input `.drv`s. *The first draft omitted `forceDerivation`; it is the most common one on the `nix build` path and its absence would surface as a `"don't know how to recreate store derivation"` throw, not silent corruption.*
- **Failure latches.** *Obligation:* a `.drv` write that fails (disk full, permissions) must not be silently swallowed — every later observer must see the error. *Discharge:* the source scheduler's `shared_future` winner-election + latched-`Failed` discipline ([PROPOSAL.md §6.1](./PROPOSAL.md#61-input-materialisation)) is reused; DetSys's `AsyncPathWriter` already does this with `promise.set_exception` (`async-path-writer.cc:61`).
- **`readOnlyMode` is unchanged.** Read-only never flushes (it never wrote before); the scheduler simply never drains. The returned attrset is byte-identical to today's read-only path.

## 6. Interaction with the lazy-source substrate

This design and the existing source-materialisation architecture **compose into one resolution boundary**, which is the larger prize. Today the two are sequenced *eagerly within* `derivationStrict`: source placeholders resolve (and copy) at step 2, then the `.drv` writes at step 5. After this design, both defer to the same flush:

- **Sources** already defer their narHash walk to the observation boundary and (for rev-pinned/deferred-mount inputs) defer the copy ([PROPOSAL.md §2.F, §6.1](./PROPOSAL.md#f-mountinput-known-narhash-fast-path)). What forces the *copy* early today is precisely that `derivationStrict` resolves `inputSrcs` to valid paths *before* writing the `.drv` (referential integrity at register time). Deferring the `.drv` write removes that forcing function: `inputSrcs` need only be *named* (narHash known) at instantiation, and *copied* at the same batched flush as the `.drv`.
- The unified flush is therefore: at a build/observation boundary, **bulk-copy all deferred sources and bulk-register all pending `.drv`s in one submission**, reference-ordered (or, if atomicity is wanted, the explicit copy-then-`registerValidPaths`-plural two-phase route — [§4.3](#43-the-batched-store-flush), Finding C3).

The result is the design the original question described: *"when a derivation is forced, recursively and in parallel batch and do all the store copies, rewrites, and materialisation."* The rewrites are still eager (intrinsic, [§2.2](#22-intrinsic-vs-deferrable)); the copies and writes are the batched, parallel, boundary-triggered work. The parallelism is the source scheduler's existing `ThreadPool` fan-out ([PROPOSAL.md §2.O](./PROPOSAL.md#o-sourceplaceholder--materialisationscheduler--content-keyed-memo)) extended across derivations.

A concrete payoff: `nix build --dry-run` over a flake (pure instantiation, thousands of `.drv`s, **zero builds**) today copies every `lib.cleanSource ./.` and writes every `.drv`. After this design it writes nothing — `--dry-run` *is* the read-only-shaped path, and a dry run that produces no store mutations is both faster and more honest about being a dry run.

## 7. Performance characteristics

"walk" = one tree traversal; "copy" = walk + store write; "txn" = one SQLite transaction / daemon round-trip.

| Scenario | Today | After this design |
|---|---|---|
| **Eval-store mass instantiation** (Hydra/`nix-eval-jobs`: M `.drv`s, no build, no IFD) — *the headline win* | M `.drv` writes (M daemon round-trips) + per-drv source copies | writes + copies deferred and flushed in bulk at end-of-eval (parallel ingestion, fewer round-trips); overlaps eval if async |
| `nix build --dry-run` / `nix eval .#x.drvPath`, source-heavy flake | read-only: already 0 `.drv` writes; N source copies | 0 source copies, 0 `.drv` writes — nothing flushed (only the source-copy deferral is new here) |
| `nix build` of 1 target, M-derivation closure, all built from source | M `.drv` writes + per-drv source copies | M `.drv` writes + source copies bulk-flushed; writes overlap eval if async — **same M writes, fewer round-trips, not fewer writes** |
| `nix build` where the target is substitutable from a cache | full instantiation: M `.drv` writes + source copies; substitution then skips most *builds* | **same M `.drv` writes** — see C1: the build path requires the whole `.drv` closure valid *before* it can even consult substituters. Deferral saves the source *copies* of substituted-away outputs, **not** the `.drv` writes |
| Deep dependency chain (A→B→…→Z), then build | each `.drv` write a separate round-trip | bulk-flushed at the build boundary (reference-ordered); still N writes |

**Finding C1 — the "skip substituted-away closures" win is false for builds.** The first draft's marquee row claimed a substitutable target lets its dependency `.drv`s go unwritten. Simulated against master, it does not: building any target loads it via `readDerivation` behind an `isValidPath`-else-`assert(false)` in `DerivationTrampolineGoal` (`derivation-trampoline-goal.cc:121-127`), and `DerivationBuildingGoal` requires **every input `.drv` valid** with the same `assert(false)` (`derivation-building-goal.cc:209-214`) — all *before* `haveDerivation`'s substituter logic (`derivation-goal.cc:86-117`), which queries *output* substitutability, not the `.drv`. `queryMissing` does shortcut an *invalid* top-level `.drv` into `res.unknown` rather than recursing (`misc.cc:208-209`, `// FIXME: we could try to substitute the derivation`) — but that is a *reporting* path; the build itself still needs the closure on disk. So a build flushes its whole `.drv` closure regardless of substitutability. **What deferral still saves on the substitutable path is the source *byte-copies* of the substituted-away outputs** (those are gated on the build actually needing them, `derivation-building-goal.cc:154-161`) — a real but smaller win than the retracted one.

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
| Win | hide write *latency* | + reduce *daemon round-trip count*; parallel ingestion; cross-drv source batching | + source-copy elision for substituted-away outputs (**not** `.drv`-write elision — C1) |
| Resolution boundary | `waitForPath` per CLI site | same + end-of-eval bulk flush + `forceDerivation` (C2) | same + transitive flush-on-read guard |
| Blocker | none (shipped) | narHash friction ([§4.3](#43-the-batched-store-flush)) — solved by per-item narHash | the build path needs the whole `.drv` closure valid pre-substitution ([§7](#7-performance-characteristics), C1) — bounds the win, not a blocker |

They are **complementary, not alternatives.** The ideal end state is async + batched: DetSys's `#if 0` block *enabled* and running on the worker thread, with the narHash friction resolved and source copies folded into the same flush. This design is "port DetSys's async harness, then complete the batch it disabled, then extend the batch to sources." **Its clean win is eval-store mass instantiation ([§2](#2-the-unblock-primitive)); for the build path it degrades gracefully to async+batched-write (latency + round-trip wins), because C1 forbids skipping the `.drv` closure.**

Relative to DetSys specifically: DetSys defers the `.drv` write but eagerly copies sources and writes per-item; this design defers both and bulk-submits both at one boundary. Relative to upstream/our-tree: neither has any of it (`writeDerivation` is synchronous on our branch — `derivations.hh:602`, single overload, no `AsyncPathWriter`).

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
3. **(R5, open) Latency floor of the per-item narHash.** Confirm a NAR dump + SHA-256 of a typical `.drv` ATerm is negligible against the `addToStoreFromDump` it replaces — i.e. the friction fix ([§4.3](#43-the-batched-store-flush)) is genuinely free, not a hidden second pass.
4. **(R4) Blast radius — confirmed nil.** This design changes *when* `.drv`s and sources are written, not *what* — `drvPath`, `outPath`, and the `.drv` contents are byte-identical to today. No rebuild scope. R2: `drvPath = hash(unparse(drv))` in both the eager and deferred paths, over the same `drv` struct.
5. **(R6) Alternatives.** (a) DetSys's async-only writer — adopted as the *base*, extended (it leaves source copies eager and writes per-item). (b) A daemon-side deferred-write registry — rejected: the pending state needs `EvalState` context the daemon lacks; the flush must be client-side before build submission. (c) Doing nothing and relying on OS page-cache — rejected on R5: the cost is fsync + SQLite txn commit per `.drv` (`registerValidPaths` syncs when `syncBeforeRegistering`, `local-store.cc:945`), which the page cache does not amortise.

A review of this document that does not walk items 1-2 against the source is a citation audit, not an adversarial review (R8). *(They have now been walked; the Findings ledger records the result.)*

## Findings ledger

The adversarial pass (REVIEW-DISCIPLINE R1, simulated against master) that this revision incorporates. Each finding names the walk that established it.

| ID | Severity | Finding | Walk |
|---|---|---|---|
| **C1** | Critical | "Skip materialising substituted-away `.drv` closures" is **false** for builds — the build path requires the whole `.drv` closure valid before consulting substituters. Marquee §7 row retracted; reduced to source-copy elision. | `derivation-trampoline-goal.cc:121-127` (`assert(false)` on invalid drv) + `derivation-building-goal.cc:209-214` (every input drv) + `derivation-goal.cc:86` (substituter query is *after*) |
| **C2** | Critical | Missing mandatory flush boundary: `AttrCursor::forceDerivation` on a **warm eval-cache hit** returns a cached `drvPath` without `derivationStrict`; a deferred (invalid) `.drv` makes it throw `"don't know how to recreate store derivation"`. Added to §4.2. | `eval-cache.cc:799-815` (the `!isValidPath` → `forceValue` → throw path), reached from `installable-flake.cc:92` |
| **C3** | Critical | "Flush as **one atomic transaction** via `addMultipleToStore`" is wrong — it is N parallel per-path transactions (`registerValidPath` *singular*). The atomic two-phase property is in `registerValidPaths` *plural*, which `addMultipleToStore` does not call and which needs bytes already on disk. §4.3 rewritten. | `store-api.cc:195` → `processGraph` → `addToStore(info,source)` (`local-store.cc:1027`) → `registerValidPath` (`:1122`) → one `SQLiteTxn` (`:952`) |
| **M1** | Medium (retracted) | "IFD is the dominant in-eval flush trigger" — **withdrawn**: Nixpkgs forbids IFD (Hydra can't build during eval), so it never fires for the target workload; default-on elsewhere but build-forcing, so a flush is in the noise. Reframed §2/§4.2/§10. | `eval-settings.hh:214` (IFD default-on in Nix) + the Nixpkgs/Hydra policy the user supplied |
| **M2** | Medium | §2 proved the premise on read-only tools, where deferral changes nothing. Reframed around **eval-store mass instantiation** (non-read-only, the case where deferral is live and valuable). | `nix-instantiate.cc:183` (read-only only under `--eval`) + `common-eval-args.cc:141` (`--eval-store` writes `.drv`s) |
| **F** (survives) | — | Deferred **source-copy** soundness holds: a build copies only `inputSrcs` valid in `evalStore`, re-checking store validity, so a source need only be flushed at the same boundary as the `.drv`. | `derivation-building-goal.cc:154-161` |

Net: the **write-deferral + bulk-submission** core is sound; the **eval-store mass-instantiation** workload is the clean win; the **atomic-transaction** and **skip-substituted-closure** claims were corrected, not salvaged.
