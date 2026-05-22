# F3 — PROTOCOL_VERSION catalog drift

E4 (continuation) flagged that `PROTOCOL_VERSION` in `worker-protocol.hh`
is currently `(1 << 8 | 39)`, but the catalog refers to `1.38` in places.
This file documents what's *actually* in tree as of the F3 verification.

## Verified facts

- **`PROTOCOL_VERSION` macro** in `src/libstore/include/nix/store/worker-protocol.hh`:
  `(1 << 8 | 39)` = `1.39`.
- **`MINIMUM_PROTOCOL_VERSION` macro** in same header:
  `(1 << 8 | 18)` = `1.18`.
- **`SERVE_PROTOCOL_VERSION` macro** in
  `src/libstore/include/nix/store/serve-protocol.hh`: `(2 << 8 | 8)` =
  `2.8`.
- **`WorkerProto::latest` C++ constant** in
  `src/libstore/worker-protocol.cc`:
  `{ .number = { .major = 1, .minor = 38, }, .features = { ... } }` =
  `{1, 38}`.
- **`WorkerProto::minimum` C++ constant** in same file: `{1, 18}`.

## What's load-bearing

- The `PROTOCOL_VERSION` macro and `WorkerProto::latest` C++ constant
  **are different version handles**. The macro is what the wire-format
  *negotiates*; the C++ constant is what the feature-gated paths
  *consult*. They drift legitimately when a wire-format addition lands
  before the corresponding `FeatureSet` entry.
- `WorkerProto::latest` minor was bumped from `37` to `38` when
  `featureDeleteDeadSpecificReferrers` was added. The next bump
  (paired with whatever `1.39`'s wire change introduces) will sync the
  two.

## Catalog corrections applied

- `INVENTORY.md`'s "Wire protocol versioning" invariant updated to
  document **both** numbers and explain the relationship:
  `PROTOCOL_VERSION = 1.39` (macro), `WorkerProto::latest = {1, 38}`
  (struct, used in feature-gated paths). The `1.38` reference was
  legitimate for the struct constant; the prior text was just
  ambiguous about which number it cited.
- `candidates/01-wire-serialisation.md` candidate #7's "FeatureSet
  (≥1.38)" reference is correct as-is — the FeatureSet exchange
  genuinely activates at `1.38` and the candidate is talking about
  the struct constant. Leave alone.

## E4 framing correction

E4 (continuation) wrote: "Also corrected the catalog: `PROTOCOL_VERSION`
is now `{1, 39}`, not `{1, 38}`." This framing is **partially wrong**:
the catalog was citing `WorkerProto::latest`, which is genuinely
`{1, 38}`. The macro is `1.39`, but the struct constant is `1.38`,
and the catalog is allowed to cite either. The invariant section in
INVENTORY.md needed clarification (which both numbers are now in), but
candidate #7 was correct.

## Recommendation for E5 (periodic verification)

The `verify-catalog-counts.sh` manifest E5 specified should track both
version numbers as separate entries:
- `worker_protocol_macro` — current `PROTOCOL_VERSION` value, expected
  to drift forward as wire changes land.
- `worker_protocol_struct_latest` — current `WorkerProto::latest.number`
  value, expected to drift forward more slowly (only when a feature
  entry lands).

When the two diverge, the catalog body that mentions either should
spell out which one it cites.

## No new candidates surfaced

This is housekeeping; no architectural debt found.
