# #8285 — closure/env-capture trimming: prototype, measurements, and why it's parked

This branch (`vibe-coding/closure-trim-8285`) prototypes NixOS/nix issue **#8285** ("Naive thunk closures cause
unnecessary space leaks"). This document is self-contained: it explains what the branch does, what it measures,
and the conclusion, so the rationale travels with the branch.

**Bottom line:** the transformation is correct and byte-identical, and dramatic on synthetic over-capture, but it
does **not** help real Nixpkgs memory — because Nixpkgs closure environments are a heavily **shared** fixpoint, not
over-capture of dead variables (measured ~99.6% of retained-floor captured env slots are *used*). #8285 is
therefore **parked as a memory lever.** The lambda half is a harmless, byte-identical micro-opt; the thunk half
should not be enabled by default (it regresses real Nixpkgs).

---

## 1. What #8285 is, and what this branch prototypes

A Nix thunk (`ClosureThunk{Env* env; Expr* expr}`) and lambda (`Lambda{Env* env; ExprLambda* fun}`) each capture a
single `Env*` — the whole current activation record — which via `up` transitively pins the **entire** lexical
chain and every slot in it, used or not. #8285's idea: capture only the **free variables the body actually uses**
(a flat/"display" closure) for small, statically-resolved free-var sets, keeping the linked `Env` for the hard
cases (`rec` / large `let` / `with`).

This branch implements two halves, both as **representation changes only** (parser/`bindVars` + `Env` capture +
thunk/lambda construction), with **no change to the 16-byte bit-packed `Value` layout** and capturing the **same
`Value*` pointers** (no copy, no forcing -> laziness, identity and output preserved by construction):

- **Lambda half** (`f69dbeead`, gated `NIX_FLAT_CLOSURES`, default on): a **leaf** lambda (body contains no nested
  lambda) with a fully-static free set and no enclosing `with`/flat-boundary captures a single flat `Env` of
  exactly its free vars (`up == nullptr`) instead of the enclosing chain. `bindVars` installs a flat "boundary"
  StaticEnv, interns crossing references as dense slots, and rewrites the body's `ExprVar`s to index the flat env;
  `ExprLambda::eval` builds it. Leaf-only keeps <=1 boundary per chain, so the single-boundary addressing is exact.
  Non-leaf/`with`/`rec`/large frames stay on the linked path.

- **Thunk half** (`2107b7f3a`, gated `NIX_FLAT_THUNKS`, default on): a thunk of a **non-recursive `Plain` attrset
  attribute value** captures a flat `Env` of its free vars. The free set is computed once at **parse time**
  (`bindVars`, recorded as a per-`Expr` `FlatThunkRecipe`); `Expr::maybeThunk` gains a single predicted-not-taken
  branch, and only eligible thunks pay `allocEnv(k)` + k pointer copies — nothing is added to
  `forceValue`/`allocValue`. Carve-outs (all correct, kept linked): `rec`/`let` members (their frame is populated
  incrementally, so snapshotting siblings is unsound), `with`, nested boundaries, and REPL-bound exprs (the REPL
  creates binding thunks with raw `mkThunk`, bypassing the flat-aware path, so its `staticEnv` is marked
  `noFlatten`).

Diff size: **+288 / -20 across 5 files** (`src/libexpr/nixexpr.{cc,hh}`, `src/libexpr/eval.{cc,hh}` + 4 lines in
`src/libcmd/repl.cc`). Both halves are **byte-identical** to master:

- `tests/functional/lang` eval corpus: `eval-okay-*` (149 files, 146 non-empty) **0 diffs**, `eval-fail-*` (283)
  **0 diffs**, A/B flat-on vs flat-off (flat-off is master by construction).
- Full `nix-functional-tests` (which includes `lang` vs committed `.exp`) + `nix-expr-tests-run` pass with the
  optimizations **on**, on **x86_64-linux** (built on the remote builder) and **aarch64-linux**.

A real correctness bug was found and fixed during this (the REPL `noFlatten` carve-out above): the first thunk-half
build failed the `repl` suites because `parseReplBindings` produces an `ExprAttrs` whose Plain values were
flat-rewritten but then thunked with raw `mkThunk` (never building the flat env), resolving a var to the wrong
slot. Audited every `mkThunk` site: the REPL was the only broken one.

## 2. Measurements (stock Boehm GC, aarch64; A/B on one binary via the env gates)

### Synthetic over-capture (unshared) — large win
50 000 surviving closures each over-pinning a **unique, unshared** 1000-element sibling it never uses:

| | lambda half | thunk half |
|---|---|---|
| `gc.heapSize` | 2.03 GiB -> 0.375 GiB (**-80.2%**) | 2.03 GiB -> 0.40 GiB (**-80.2%**) |
| peak RSS | 2.06 GiB -> 0.44 GiB (**-78.5%**) | 2.17 GiB -> 0.47 GiB (**-78.5%**) |

This is the safe-for-space ideal: cumulative allocation is ~unchanged, live retention collapses because the dead
*unshared* siblings become unreachable.

### Real Nixpkgs — neutral (lambda) to regression (thunk)
nixpkgs 26.11, offline, hold all 28 421 top-level WHNF values live (`gc.heapSize` / peak RSS / `envs.number`):

| mode | heapSize | RSS | envs |
|---|---|---|---|
| master | 1.342 GiB | 1.845 GiB | 5.98M |
| lambda half | -1.2% | +0.6% | — |
| **thunk half** | **+5.0%** | **+3.5%** | **+31.7%** |

Plus **+3.6% eval CPU** for the thunk half on a transient full-nixpkgs eval (`nrThunks` identical; `envs.number`
+31-32% from the per-thunk flat `Env`s). The lambda half is ~neutral; the thunk half **regresses** memory.

## 3. Why it doesn't help Nixpkgs (the core finding)

The synthetic wins because each surviving closure pins an **unshared** dead sibling. Real Nixpkgs is the opposite:
closure environments are a deep **shared** fixpoint. A `callPackage`/`stdenv.mkDerivation` arg frame
(`{ lib, stdenv, fetchFromGitHub, cmake, ... }:`) is pinned for the whole eval and **shared** by many siblings
(every attr-thunk, `.override`/`.overrideAttrs` closure, `passthru`) and by the attrset's own `env2`. Flattening
one thunk does **not** free that frame (others still pin it); it only **adds** a per-thunk flat `Env` — naive flat
capture replaces sharing with per-thunk copies, hence the +32% envs / +3.5% RSS regression.

The decisive question is then: of captured env bytes, how much is **dead captured** (trimmable even by a correct,
sharing-preserving display closure) vs **shared-and-used** (not trimmable at all)? A sound eval-time #8285 can drop
a slot **iff no `ExprVar` references it** (statically-unused binding). Measured two ways:

- **Instrumented evaluator** (allocation-weighted, deep eval forcing every derivation's `drvPath`): **95.2% of
  captured env slots are used**, 4.8% provably-dead — and the dead is almost entirely **unused lambda formals**
  (lambda frames 94.3% used; `let` frames 99.95% used; `rec` <=1% of slots and undercounted, since `rec` attrs are
  also read via external attribute selection the metric can't see).
- **Retained-floor ground truth** (the frames that make up the live floor are the `callPackage` package-function
  arg frames; a dead slot there is a statically-unused formal — a static AST property). Measured directly on the
  snapshot with `deadnix` 1.3.2 (the real Nix parser) and an independent parser: of 187 001 by-name package
  formals, **~0.4% are unused** (deadnix 792/187001 = 0.42%; agrees across by-name, lib, and python-modules
  populations). **The retained lambda class is ~99.6% used.** Nixpkgs CI + `deadnix` actively strip unused
  callPackage args, so the retained floor is kept clean of exactly the dead capture #8285 would trim.

So retention-weighting (the stronger test) gives an *even lower* dead fraction than allocation-weighting
(~0.4% on the floor vs 4.8% allocated — the allocated figure is inflated by transient `mapAttrs`/`foldl'`/`_: v`
callbacks that die immediately and never retain). Re-weighting the frame-kind mix by retained dead fractions gives
a sound reclaim ceiling of **~=0.3% of env slots ~= ~0.03% of heap** (env bytes are ~10.8% of the retained heap) —
below the representation overhead of any flat/display scheme. **Nixpkgs retention is a genuine shared fixpoint, not
over-capture of dead variables.**

## 4. Decision: PARKED for memory

- The **lambda half** is a harmless, byte-identical micro-optimization (neutral on Nixpkgs, big only on synthetic
  unshared over-capture). Keep it available behind the flag; it costs nothing when it doesn't apply.
- The **thunk half** regresses real Nixpkgs (+3.5% RSS, +3.6% CPU). **Do not default `NIX_FLAT_THUNKS` on.** It is
  retained as a measurement vehicle that establishes the negative result.
- A sharing-preserving **display-closure** variant (one trimmed frame per site, shared) is also a dead end: its
  sound reclaim ceiling is ~0.03% of heap, and projecting a shared parent slot into N per-site trimmed frames
  duplicates it N ways — overhead grows with sharing, i.e. exactly where the floor lives.
- If the <1% dead (unused formals) were ever wanted, the cheaper fix is to drop provably-unused formal/`let` slots
  in the **existing linked `Env`** (`allocEnv(U)` instead of `allocEnv(S)`; no new representation, no sharing loss,
  formal-arity validation stays static on the `ExprLambda`). This is a strict net-positive — but at ~0.03% of heap
  it is not worth the parser/`bindVars` churn.

**The real whole-Nixpkgs memory levers are elsewhere:** reducing the *reachable-but-recomputable* residue
(unforcing — dropping values that can be recomputed rather than retained), and a **compacting/moving GC**
(#8626 / #13892) to reclaim fragmentation and pack the genuinely-live shared fixpoint. Closure-capture trimming is
not a Nixpkgs memory lever.

---

*Provenance: all percentages are from A/B runs on one instrumented binary (`NIX_FLAT_CLOSURES` / `NIX_FLAT_THUNKS`
/ `NIX_TRIM_STATS` gates, `NIX_CONFIG` unset) and from `deadnix`/static parses over the nixpkgs 26.11 snapshot.
Fuller derivations, the REPL-bug post-mortem, and the independent refutation of this park decision are in the
working notes that accompanied this branch's development.*
