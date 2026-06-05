# Alternative design for shared-closure amortization — grounded in the §3b at-scale failure

2026-06-03. After measuring §3b at python3Packages scale and root-causing the
warm-cache thrash (`doc/eval-trace/measurements/3b-at-scale-2026-06-02.md`,
`derivation-producer-partition-rfc.md` §19), this asks: given what we now
*measured*, what would a sound design actually look like? Honest status: this is
a design analysis, not an implementation — but unlike the seven prior doc passes,
its constraints are measured, not reasoned from a 7-trace workload.

## 1. The two measured failure modes (separate them — they have different fixes)

§3b failed for TWO independent reasons. Conflating them is what kept the arc
circling.

**Flaw A — non-deterministic identity (the thrash).** A producer trace's deps are
`innerDeps = snapshotEpochRange(epochStart, epochEnd)` — a slice of the *global*
epoch log, which grows from both fresh recording AND replay. Whether a nested
derivation is *flattened* into that slice or *edged* depends on the process-local
`producerMap` state, which differs cold↔warm. So the producer hash is a function
of eval **context/order**, not of the derivation's inputs. Measured: 503
producers record a different hash for the same `drvHash` cold-vs-warm → cascade →
thrash above ~53–66K traces. **A content-addressed cache requires deterministic
hashes; §3b's producer hash is not one.**

**Flaw B — edges are slower than flattening on the hot path (the fragmentation).**
Independently of A, putting an edge on the *verify* path replaces a consumer's
inline deps (already in its blob, resolved with the session L1 dep-hash cache)
with a pointer to a *separate* producer trace that warm-verify must `loadTrace`
(SQLite read + zstd-decompress) + recursively verify. Measured on closures (where
§3b is deterministic enough not to thrash): aggressive hot **1.13s vs conservative
0.42s vs no-§3b 0.31s**, loading 11,320 separate blobs. **Even a perfectly
deterministic edge loses on hot, because inline-deps + L1-dedup is cheaper than
chasing a pointer into another blob.**

The decisive measured fact that reframes everything: **no-§3b flattening is STABLE
and FAST on hot** — python3Packages warm serves in **3.7s for 18.7M flattened
deps** (L1-deduped), with zero thrash. The cache's hot path already works. §3b
made it both slower (B) and unstable (A).

## 2. Two design principles that fall directly out of the measurements

1. **Identity must be STRUCTURAL, never eval-context.** Any cross-trace shared
   identity must be a pure function of the dependency *structure* (e.g.
   `drvPath = hashDerivationModulo(inputs)`, which the build layer already
   computes deterministically), NOT a slice of the global eval trace. This is the
   fix for Flaw A.
2. **Verification stays FLATTENED + inline on the hot path.** Never replace a
   consumer's inline deps with an edge-to-separate-trace on the verify path. Keep
   the no-§3b representation that measured stable+fast. This is the fix for Flaw B
   — and it means *the amortization target is COLD recording + STORAGE, not hot.*

§3b violated both: its identity was eval-context (A), and its edge was on the
verify path (B). Any viable design inverts both.

## 3. The design spectrum (simplest-safe → most-ambitious), with the measured cost it targets

The cache's *measured* costs are: HOT verify (already good — leave it), COLD
recording (python3Packages: **29–44s**, the flattened-dep hash+serialize+flush),
and STORAGE (**525MB** of 607×-duplicated deps). All three levers below keep
flattened hot verification untouched.

### Tier 0 — async recording + keep flattening (simplest; safe; no amortization)
Move cold trace recording off the eval critical path (record on a worker; the
eval thread hands off `(pathId, value, deps)` and continues; barrier at session
end). The no-§3b hot path is unchanged (stable, fast). Cold *wall* approaches
no-trace; the cold *work* (hashing 607×) is unchanged but hidden. STORAGE
unchanged. **This alone makes the cache a net win for the warm-serve (eval-jobs)
use case** — the case that matters — without any of §3b's risk. It is the
"Layer 2b" line the RFC already names, and it is ORTHOGONAL to producer-partition.
Lowest risk, real value, no new soundness surface.

**Measured + grounded (2026-06-03, the at-scale measurement doc "TIER-0 CEILING"
section).** python3Packages cold: no-trace 18.7s, eval-trace-on 66.7s, record
28.97s. `perf` self-time (comm=nix) splits the record into **movable ≈25s**
(BLAKE3 13.6s + serialize 2.1s + storage 9.5s) and **sync-residual ≈2.7s**
(`resolveDepKeyMaterial`/`collectPath` — the only part that reads the
single-writer `DataPathPool.nodes`/`vocab.paths`). The earlier fear that resolve
*dominates* (H-cold-1's "pool resolution is the dominant cost" comment) was
REFUTED post-H-cold-1: resolve is ~2.6%, the 3× BLAKE3 is the bulk (13.2%) and is
async-movable. **Sound seam:** the worker hashes from `ResolvedDepKeyMaterial`
(owned `path`/`encodedBlob` + stable thread-safe `StringInternTable` views),
serializes raw IDs, and stores under the store mutex — it never touches the
single-writer pool/vocab, which are read only during the eval-thread resolve.
**Corrected ceiling: cold 66.7 → ~42s** (the ~19s residual to no-trace is the
always-on dep-CAPTURE tax — a separate H3 lever, not Tier-0). Two byte-identical
implementation paths (eager-resolve vs buffer-the-preimage) are spelled out in
the measurement doc. NOT yet implemented.

### Tier 1 — sub-closure STORAGE dedup (reduce 525MB + cold serialize)
The existing `DepKeySets` table content-addresses *whole* flattened closures, so
it gets 0 sharing (10,397 traces → 10,397 distinct sets — architecture doc). Dedup
at *sub-closure* granularity instead: store the shared closure's dep-subset ONCE,
keyed by a structural identity, and have consumers reference it. **Crucially, this
must resolve into a shared in-memory dep pool during verify, NOT a separate blob
load** (else it reintroduces Flaw B). This is an interning/pooling scheme, closer
to the existing string/DataPath interning than to trace-edges. Reduces storage and
cold serialize; hot verify still resolves inline from the pool (L1-deduped).
Requires identifying the shared sub-closures structurally (Tier 2's identities).

### Tier 2 — deterministic compositional IDENTITY hashing (amortize the cold HASH)
The dominant cold cost is hashing the 607×-flattened deps. Amortize it the way the
build layer amortizes `hashDerivationModulo` via the `drvHashes` memo:
`id(node) = H(node's OWN leaf observations, sorted child identities)`, computed
ONCE per distinct node and memoized. Child identities are **structural and
deterministic**: derivations → `drvPath` (already computed, primops.cc:1994);
imported files → resolved store path (H1: flake source is store-copied, so the
path *is* a content-address) + `computeNixScopeHash`; attr-path children → their
path. This is deterministic (no Flaw A) and amortized (shared closure hashed once).
**Used for the trace IDENTITY only — NOT to replace inline verify deps** (no Flaw
B). Decouples identity (compositional, cold, deterministic) from verification
(flattened, hot, fast) — the decoupling §3b conflated.

## 4. The genuinely hard parts (honest — these are why Tier 2 is RFC-scale)

- **Structural identity for ALL shared nodes, not just derivations.** Determinism
  requires edging *every* identity-bearing shared sub-computation (or it flattens
  non-deterministically, exactly Flaw A). Derivations have `drvPath`; imports have
  a resolved path; but arbitrary memoized **function results** (`lib.foo args`) need
  `H(f-structure, arg-identities)` — the Tier-2 function-identity recursion and the
  "scalar-identity wall" (an `outPath` is a STRING with no slot to hang an identity
  on). This is the same wall every prior lever hit; it is real and unsolved.
- **A COMPOSABLE hash.** The current trace hash uses a positional `dep.ordinal`
  over a globally-sorted vector (hash.hh) — sub-closure hashes cannot compose into
  it. Compositional identity needs an order-independent **set hash** (a commutative/
  associative combine of per-node hashes), which is a preimage change = schema
  epoch bump + loss of the positional channel. (The L-A blocker, now with a reason
  to pay it.)
- **Facet soundness.** Unlike a build (observes only output paths), an eval node
  can observe a child's SHAPE (keys, `attrNames`, negative membership). A
  compositional identity edge must capture the *facet* the parent observed, or it
  stale-serves on a shape change. This is the keyset-escape work
  (`store/keyset-escape.cc`) — the prerequisite that makes this eval-specific, not
  a mechanical port of `hashDerivationModulo`.

## 5. What §3b should have been (the one-line diagnosis)

§3b tried to be a *verification* edge keyed by an *eval-context* hash. It should
have been (if anything) an *identity/hash* amortization keyed by a *structural*
address (`drvPath`), leaving verification flattened. It inverted both principles,
so it broke determinism (A) and hot cost (B) at once — and the measurements show
each is independently fatal.

## 6. Recommendation

- **Do Tier 0 (async recording).** Measured-safe, real warm-serve value, no §3b
  risk. This is the actual answer to the branch's cold-perf problem.
- **Consider Tier 1 (sub-closure storage dedup)** only if storage/cold-serialize is
  shown to matter after Tier 0 — and only via an in-memory pool, never a verify-path
  load.
- **Treat Tier 2 (compositional identity) as a genuine RFC**, gated on solving the
  scalar-identity wall + composable hash + facet rule — and explicitly forbid the
  two §3b anti-patterns: eval-context identity, and edges on the verify path.
- **Do NOT revive the producer-edge/§3b/OSPI direction.** It is measured-dead for
  two independent reasons (§19), and both fixes lead away from it, not back to it.

Caveats: this is unimplemented and unmeasured; the Tier-0/1/2 cost estimates are
projections from the measured profile, not benchmarks of the alternative. The
*constraints* (Flaws A/B, no-§3b-is-fast-and-stable) are measured.
