# Session Types (libutil)

Typed channel protocols ported from [Dialectic]. This library is a generic
utility — it is **not currently consumed by any production subsystem in the
Nix tree**. The only callers of the `Chan<S,Tx,Rx>` family are the
session-types own unit tests under `src/libutil-tests/session-types/`.

## Status

Retained as a generic library for potential future use (e.g., IPC protocols,
pipelined workers, etc.). Eval-trace originally used session-typed protocols
(`VerifyPipeline`, `VerifyOrRecover`, `TraceStoreOp`, `PrefetchHint`) for the
verification pipeline, but Bundle 4 replaced them with direct typed method
calls under `Proof<BlockingTag>` plus `Linear<T>` typestates
(`DepResolution`, `RecoveryState`). See `doc/eval-trace/implementation.md`
§3.3 for the history and `src/libexpr/eval-trace/CLAUDE.md` for the current
mechanism.

If you're adding a new subsystem and considering a session-typed protocol,
this library is available. If you're maintaining eval-trace, you won't need
it — the ordering invariants you care about are encoded as linear typestates,
not channel protocols.

## Files

- `chan.hh` — `Chan<S, Tx, Rx>` typed channel.
- `protocol.hh` — `Send<T, Next>`, `Recv<T, Next>`, `Choose<L, R>`,
  `Offer<L, R>`, `Loop<Body>`, `Call<Sub, Next>`, `Done`.
- Additional combinators and compile-time-validation machinery.

See the CLAUDE.md in this directory for operational details.

[Dialectic]: https://docs.rs/dialectic
