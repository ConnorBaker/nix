# Session Types Library

C++23 port of the type-level protocol encoding from
[Dialectic](https://github.com/boltlabs-inc/dialectic) (MIT license).
Local reference: `~/dialectic`.

## What the headers provide

| Header | Purpose |
|--------|---------|
| `protocol.hh` | Protocol constructors (`Done`, `Send`, `Recv`, `Choose`, `Offer`, `Loop`, `Continue`, `Call`, `Split`) and `TypeList` utilities |
| `dual.hh` | Duality transform: `Dual_t<Send<T,P>>` = `Recv<T, Dual_t<P>>`, etc. |
| `scoped.hh` | De Bruijn well-scopedness check: `Scoped<P>` concept verifies every `Continue<I>` has `I < enclosing Loop depth` |
| `subst.hh` | Continue substitution: `Subst_t<P, Body>` replaces `Continue<0>` with `Body` (loop unrolling) |
| `lift.hh` | Index shifting: `Lift_t<P, N>` increments free Continue indices by N (needed when splicing protocols at different nesting depths) |
| `then.hh` | Protocol sequencing: `Then_t<P, Q>` replaces every `Done` in P with Q (lifted to the correct depth) |
| `actionable.hh` | Next-action extraction: `Action_t<Loop<P>>` unrolls the loop once via Subst, then recurses. `Actionable<P>` concept is well-formed when `Action_t` terminates; pathological infinite loops (e.g., `Loop<Continue<0>>`) cause a hard compiler error rather than cleanly evaluating to false |
| `select.hh` | Branch selection: `SelectBranch_t<Choose<A,B,C>, 1>` = `B`. Also `RemoveAt_t` for TypeList manipulation |
| `session.hh` | Master `Session<P>` concept: `Scoped<P> && HasDual<P> && Actionable<P>` |
| `backend.hh` | `Transmitter` / `Receiver` concepts (base: has tag type) and `CanTransmit<Tx,T>` / `CanReceive<Rx,T>` (per-type: `transmit(T)` returns `awaitable<void>`, `receive<T>()` returns `awaitable<T>`). Also `Unavailable` — placeholder half for `split()` |
| `branches.hh` | `Branches<Offer<...>, Tx, Rx>`: deferred offer result with exhaustive `visit(f1, f2, ...)` dispatch. Inherits `Linear<Branches<...>>` for linearity enforcement |
| `chan.hh` | `Chan<S, Tx, Rx>`: runtime session-typed channel. Methods: `send`, `recv`, `choose`, `offer`, `call`, `split`, `close`. Each consumes by move (`&&`-qualified) and returns the advanced channel type as `awaitable<Chan<...>>` |
| `in-process.hh` | `InProcessTx` / `InProcessRx`: async same-strand backend using shared `std::queue<std::any>` with `steady_timer` condvar. `channel<S>(executor)` factory creates a `(Chan<S>, Chan<Dual_t<S>>)` pair |

## How it maps to Dialectic

### Type-level encoding

| Dialectic (Rust) | This library (C++) | Notes |
|--|--|--|
| `Send<T, P>` | `Send<T, P>` | Identical |
| `Recv<T, P>` | `Recv<T, P>` | Identical |
| `Done` | `Done` | Identical |
| `Choose<(A, B, C)>` | `Choose<A, B, C>` | Rust uses tuple of types; C++ uses parameter pack |
| `Offer<(A, B, C)>` | `Offer<A, B, C>` | Same |
| `Loop<P>` | `Loop<P>` | Identical |
| `Continue<N>` | `Continue<N>` | Both use integer indices (`const usize` / `unsigned` NTTP). Dialectic converts to Peano internally for trait resolution; C++ uses direct arithmetic. De Bruijn semantics are the same |
| `Call<P, Q>` | `Call<P, Q>` | Identical |
| `Split<P, Q, R>` | `Split<P, Q, R>` | Identical |
| `HasDual` trait | `Dual_t<P>` alias | Rust: associated type. C++: template alias |
| `Scoped` trait | `Scoped<P>` concept | Rust: trait bound. C++: concept |
| `Actionable` trait + `NextAction` | `Actionable<P>` concept + `Action_t<P>` | Identical semantics |
| `Session` trait | `Session<P>` concept | Rust: `Scoped + Actionable + HasDual + 'static`. C++: no `'static` equivalent needed |

### Runtime channel

| Dialectic | This library | Notes |
|--|--|--|
| `Chan<S, Tx, Rx>` | `Chan<S, Tx, Rx>` | Same parameterization |
| `async fn send(self, val) -> Result<Chan<P>, Error>` | `auto send(val) && -> awaitable<Chan<P>>` | Both async + affine. Errors: Rust returns `Result`, C++ throws through `co_await` |
| `async fn recv(self) -> Result<(T, Chan<P>), Error>` | `auto recv() && -> awaitable<pair<T, Chan<P>>>` | Same |
| `async fn choose<N>(self)` | `auto choose<N>() && -> awaitable<Chan<P>>` | Rust: const generic. C++: NTTP |
| `async fn offer(self) -> Result<Branches, Error>` | `auto offer() && -> awaitable<Branches>` | Same |
| `Branches::case::<N>(f)` | `Branches::visit(f0, f1, ...)` | Dialectic matches one branch at a time. This library requires exhaustive match (all branches in one call) |
| `chan.call::<P, _, _>(f).await` | `co_await chan.call(f)` | Both async. f returns `awaitable<Chan<Done>>`. Bidirectional sub-protocols work because `co_await` allows scheduler interleaving |
| `chan.split(f).await` | `co_await chan.split(f)` | Both: `f` receives tx-only and rx-only half-channels. Dialectic uses `Unavailable` for the missing half; this library does the same |
| `Transmitter` / `Receiver` traits | `Transmitter` / `Receiver` concepts | Dialectic: async traits. This library: async concepts (`transmit()` → `awaitable<void>`, `receive<T>()` → `awaitable<T>`) |
| `Transmit<T, C>` / `Receive<T>` | `CanTransmit<Tx, T>` / `CanReceive<Rx, T>` | Dialectic has calling convention `C` (`Val`, `Ref`, `Mut`). This library: always by-value |

### Key differences from Dialectic

**Integer indices vs Peano internals.** Both Dialectic and this library
use integer indices at the user-facing level (`Continue<2>` in both).
Dialectic converts `const usize` to Peano types (`Z`, `S<Z>`, ...)
internally for trait resolution (via `Number<I>: ToUnary`). This library
uses `unsigned` NTTPs with direct arithmetic throughout. Both represent
De Bruijn indices with the same semantics.

**Calling conventions.** Dialectic's `Transmit<T, C>` has a calling
convention parameter `C` (`Val`, `Ref`, `Mut`) controlling whether the
value is sent by value, shared reference, or mutable reference. This
library always sends by value (`std::move`).

**No `Session!` macro.** Dialectic provides a proc macro that compiles
a human-readable protocol DSL (`send T; recv U; loop { choose { ... } }`)
into the nested type constructors. This library has no equivalent — protocols
are written directly as nested template types.

## What is not implemented

**Network/serialization backends.** Only the `InProcess` backend exists.
Dialectic ships `dialectic-tokio-mpsc`, `dialectic-tokio-serde`,
`dialectic-tokio-serde-json`, `dialectic-tokio-serde-bincode`, and
`dialectic-reconnect`. This library has no network, serialization, or
reconnection backends.

**Retry / reconnect.** Dialectic's `dialectic-reconnect` crate provides
automatic retry with session resumption. No equivalent exists here.

## Implementation Notes

**Linearity enforcement.** `Chan` and `Branches` inherit
`nix::Linear<Derived>` (from `linear.hh`) for unconditional destructor
abort if not consumed. `Chan` uses `Linear::reassign()` for its
move-assignment operator (needed for coroutine loop patterns).

**Construction restriction.** `Chan`'s constructor is private.
Internal construction — `send`/`recv`/`call`/`split`/`offer` via
`visit_dispatch` — uses cross-instantiation friendship. External
construction uses `makeChan<S>(tx, rx)` (public, `requires Session<S>`)
or `channel<S>(exec)` (InProcess convenience, calls `makeChan`).

**Dual involution.** `Dual_t<Dual_t<S>> == S` is verified by
`static_assert` in `channel<S>()`.

## Caveats

**Exception during `visit()`.** If a `visit()` handler throws after
receiving a `Chan`, the `Chan`'s destructor aborts (unconsumed). This is
intentional — it matches Rust's double-panic-is-abort semantics. A channel
that is not consumed is always a protocol violation, even during error
handling. Callers that need graceful error handling must `close()` or advance
the channel to `Done` before throwing.

## Where it is used

This library remains available for places that genuinely need typed channel
protocols. Eval-trace no longer uses it for the verification/runtime path:
Bundle 4 removed the old `VerifyPipeline`, `VerifyOrRecover`, and
`TraceStoreOp` in-process protocol shell in favor of direct typed method
calls plus `Linear` typestate where ordering still matters.

## Maintenance

When modifying session types, update this file and:

**This CLAUDE.md must be updated when:**
- Adding/removing protocol constructors (Send, Recv, Choose, etc.)
- Changing the `Session` concept definition
- Adding a new backend (beyond InProcess)
- Changing `Chan` method signatures

**Cross-reference with:**
- `src/libexpr/eval-trace/CLAUDE.md` Section 7 (Verification Typestate)
- `doc/eval-trace/implementation.md` Section 3.3 (update if protocol usage changes again)

**Verification after changes:**
```bash
# Current library self-checks
grep -r 'static_assert(Session' src/libutil/include/nix/util/session-types --include='*.hh'
```
