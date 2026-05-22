# F2 — "is not valid" wire-crossing contract verification

Charter: characterise the second wire-crossing text-grep contract that E4 surfaced
(`WorkerProto::BasicClientConnection::queryPathInfo` greps client-received error
text for `"is not valid"`); decide its catalog impact; resolve E4's incidental
suspicion that `binary-cache-store.cc` carries a related pre-existing bug.

## Contract characterisation

### Grep site

`WorkerProto::BasicClientConnection::queryPathInfo` in
`src/libstore/worker-protocol-connection.cc`, after the inner `processStderr`
call, greps the caught `Error`'s `e.msg()` for `"is not valid"`. On match, the
method returns `std::nullopt`; on no-match it rethrows. The substring test is
a plain `std::string::find` — case-sensitive, exact-bytes — and applied to the
result of `BaseError::msg()`, which is `calcWhat()` (the *fully rendered* error
including any `error: ` prefix and any traces appended by `addTrace`), not
`info().msg.str()` alone.

The check sits *before* the proto >= 1.17 `valid` byte read, but `MINIMUM_PROTOCOL_VERSION`
is `1.18` (`PROTOCOL_VERSION` is `1.39`); the structured `valid`-byte path is
always taken on supported daemons. The grep is therefore a fallback, not the
primary discriminator, in the live protocol range.

### Server-side producers

Throws whose rendered text contains `"is not valid"` and that are reachable
from `Store::queryPathInfo` (the daemon's `WorkerProto::Op::QueryPathInfo`
target):

- `LocalStore::queryValidPathId` — `throw InvalidPath("path '%s' is not valid", printStorePath(path))`
  in `src/libstore/local-store.cc`. Caught by the daemon's
  `case WorkerProto::Op::QueryPathInfo` arm via `catch (InvalidPath &)`.
  Suppressed; replaced by a `0` byte on the wire.
- `Store::queryPathInfo` (the `Callback`-form in `src/libstore/store-api.cc`) —
  two `throw InvalidPath("path '%s' is not valid", printStorePath(storePath))`
  sites: one in the synchronous client-cache fast path, one in the async
  `queryPathInfoUncached` callback. Same daemon-side suppression.
- `path.cc::checkName` / `StorePath` ctor — `throw BadStorePathName("name '%s' is not valid", ...)`
  (and the two longer `"name '%s' is not valid: first dash-separated component must not be '%s'"`
  variants). `BadStorePathName` is `MakeError(BadStorePathName, BadStorePath)`;
  `BadStorePath` is `MakeError(BadStorePath, Error)`. **Not an `InvalidPath`**;
  the daemon's `catch (InvalidPath &)` does not catch it. Propagates to the
  outer `catch (Error & e)` in `processConnection`'s `performOp` retry loop,
  which calls `tunnelLogger->stopWork(&e)` and writes the error to the wire.
  Reachable from `WorkerProto::Serialise<StorePath>::read` when the daemon
  decodes the path string.
- `dummy-store.cc::readDerivation` — `throw Error("derivation '%s' is not valid", ...)`.
  **Not** in the `queryPathInfo` call chain (`DummyStore::queryPathInfoUncached`
  returns `nullptr` instead of throwing for missing paths; `Store::queryPathInfo`
  then throws `InvalidPath("path '%s' is not valid", ...)`). The dummy-store
  string only triggers the contract incidentally if a future caller invokes
  `readDerivation` from inside a `queryPathInfo` path.

### Client-side consumer

Only one: the `catch (Error & e)` in `BasicClientConnection::queryPathInfo`.
Behaviour:

- `e.msg().find("is not valid") != npos` → return `std::nullopt`.
- otherwise → rethrow the exception unmodified.

### Stability across libstore versions

Introduced in commit `ddea253ff` ("RemoteStore: Propagate InvalidPath
exceptions from the daemon", 2016). At that time it was the *only*
discriminator: pre-1.17 daemons threw `InvalidPath` on missing paths and
relied on the client recognising the substring. The structured `valid`-byte
wire change at proto `1.17` plus the daemon-side
`catch (InvalidPath &)` made the grep redundant for proto-conforming
daemons. The grep was kept as a "Ugly backwards compatibility hack" (the
in-code comment) to handle still-older daemons.

With `MINIMUM_PROTOCOL_VERSION = 1.18 > 1.17`, the original purpose of the
grep is **dead code in normal operation**. It only fires today if either:

1. A non-`InvalidPath` libstore error containing `"is not valid"` propagates
   from `WorkerProto::Op::QueryPathInfo` (e.g., `BadStorePathName` from
   path-string deserialisation, or any `Error`-but-not-`InvalidPath` raised
   beneath `Store::queryPathInfo`).
2. An out-of-tree daemon implementation lets `InvalidPath` propagate without
   the daemon-side catch, despite negotiating proto >= 1.17.

In case (1) the grep silently maps a *parse error* or *unrelated error* to
"path not found" — a latent class-of-bugs hazard.

### False-negative and false-positive analysis

**False negatives** (path-not-found error reaches client *without* the
substring, so `queryPathInfo` propagates instead of returning `nullopt`):
- Any future rephrasing of `InvalidPath`'s message (e.g., "is invalid",
  "does not exist", "not a valid path") silently breaks the contract. Today's
  surface is small (4 throw sites, all using the literal `"is not valid"`),
  but no compile-time enforcement exists.
- The structured `valid`-byte path covers proto >= 1.17 for the canonical
  `InvalidPath` case, so a rephrasing here would only be observable when
  the `valid`-byte path doesn't fire (the legacy fallback case).
- The `boost::format` → `std::format` migration (#125) must preserve the
  literal substring verbatim; nothing in the build forces this.

**False positives** (error reaches client *with* the substring but does NOT
mean path-not-found, so `queryPathInfo` returns `nullopt` for a real failure):
- `BadStorePathName` from daemon-side `Serialise<StorePath>::read`. Reachable
  if a client sends a structurally-invalid path name. In practice clients
  validate via the same `StorePath` constructor before sending, so this is
  an unusual case (malicious or buggy client).
- Any future libstore error whose `HintFmt` argument happens to *contain* the
  substring (e.g., a path or message that interpolates a path name like
  `name-that-says-is-not-valid`). `e.msg()` is `calcWhat()`, so trace text
  added by `addTrace` is included; a trace mentioning "`...because the
  reference '%s' is not valid`" added downstream of a queryPathInfo call would
  also match. This is a real (if narrow) latent-bug surface.

The contract is thus **coarse and non-monotonic**: any libstore exception
that contains `"is not valid"` anywhere in its rendered text — message body
or trace — and surfaces during `WorkerProto::Op::QueryPathInfo` is mapped to
`nullopt` regardless of intent.

## binary-cache-store.cc suspicion

E4 flagged `BinaryCacheStore::addToStoreCommon` (in `src/libstore/binary-cache-store.cc`),
which throws `Error("cannot add '%s' to the binary cache because the reference '%s' is not valid", ...)`.

**Verdict: not-bug. Not reachable from the queryPathInfo grep contract.**

Reasoning:

- The throw site is in `addToStoreCommon`, an *upload* path. The throw fires
  when `queryPathInfo(ref)` (called locally inside `addToStoreCommon` to
  validate references before pushing) raises `InvalidPath`, and the calling
  code rewraps it with the cache-add context.
- `BinaryCacheStore` is a `Store` subclass distinct from `RemoteStore`. The
  client-side grep in `BasicClientConnection::queryPathInfo` is the entry to
  the *daemon* wire protocol, not to a binary cache. `BinaryCacheStore` calls
  do not traverse `WorkerProto`.
- The daemon (`nix-daemon`) is conventionally configured against `LocalStore`,
  not `BinaryCacheStore`. Even when a non-`LocalStore` daemon is hypothetically
  configured, the addToStore throw fires on `WorkerProto::Op::AddToStore` /
  `WorkerProto::Op::AddMultipleToStore`, not on `WorkerProto::Op::QueryPathInfo`.
  The `BasicClientConnection::queryPathInfo` grep does not see it.

E4 was reading the hazard correctly in shape (any libstore message containing
`"is not valid"` is a candidate trigger of the substring contract) but
incorrectly in path: the binary-cache string only travels the wire if the
daemon's *own* binary-cache-store wrapper produces it during a `queryPathInfo`
op, which is not a path the code constructs.

The site is still relevant to the **migration constraint** (#125): a future
rephrase to "is invalid" here would not break the queryPathInfo contract,
but the grep-site invariant is "any `"is not valid"` message anywhere in
libstore is potentially load-bearing", so the safer policy is to preserve
the substring in all libstore producers regardless of reachability.

## Other text-grep contracts in the tree

Census of `e.msg()` / `e.what()` / `.message()` substring tests in `src/`:

```
grep -rn '\.(msg|what|message)\(\)\.find\|find(.*\.(msg|what|message)\(\)' src/
```

Three matches across the entire source tree:

1. **`worker-protocol-connection.cc::processStderrReturn`** — DynamicDerivations
   triple. `m.find("parsing derivation")` and `m.find("expected string")` and
   `m.find("Derive([")`. Soft-coupling; proto < 1.36 only. Documented in E4.
2. **`worker-protocol-connection.cc::queryPathInfo`** — the
   `"is not valid"` site under analysis. Hard-coupling; all proto versions.
3. **`libcmd/repl.cc`** — `e.msg().find("unexpected end of file")` to detect
   multiline-continuation in the eval REPL. Eval-only; not wire-crossing.

No other in-tree exception-text grep contracts found. (`build-result.cc` has a
`BuildResult::operator==` that compares `message()` strings between two
`BuildResult`s, but that is a value-equality check, not a substring contract.)

E4's matrix is therefore complete: the in-tree text-grep contract surface is
exactly three sites, of which two are wire-crossing (the daemon-protocol pair)
and one is local (the REPL).

## Catalog impact

Recommendation: **add a new candidate** plus annotate existing ones.

### New candidate (proposed slot in 18-daemon-protocol-audit or 22-cross-shard-patterns)

> **Wire-protocol error semantics carried by free-form text rather than
> structured discriminators.** The `WorkerProto` wire format encodes
> exceptions as a serialised `Error` (proto >= 1.26) or a `(message, status)`
> pair (proto < 1.26). It carries no exception-type discriminator. Two
> client-side text-grep contracts compensate:
>
> - `BasicClientConnection::processStderrReturn` greps `e.msg()` for the
>   `DynamicDerivations` triple (`"parsing derivation"`, `"expected string"`,
>   `"Derive([")`. Retires when minimum supported proto reaches 1.36.
> - `BasicClientConnection::queryPathInfo` greps `e.msg()` for `"is not valid"`
>   and maps to `std::nullopt`. No version cutoff. Coarse: catches any
>   libstore error containing the substring during `Op::QueryPathInfo`,
>   regardless of intent.
>
> Both are listed in the in-code comments as backwards-compatibility hacks
> (the `"is not valid"` grep dates to commit `ddea253f`, 2016). Both are
> hard breaks for the `boost::format` → `std::format` migration (#125):
> any rephrasing of any libstore exception in the wire-crossing set
> silently changes wire semantics with no compile-time enforcement.
>
> Replacement directions:
>
> 1. Add a structured exception-type tag to the wire format. The
>    `Sink << Error` overload (proto >= 1.26) currently writes a literal
>    `"Error"` placeholder where a type-name field used to live; reusing
>    that field for a typed discriminator (e.g., a fixed enum of
>    `{Error, InvalidPath, BadStorePath, BadStorePathName, FormatError, BuildError, SysError, ...}`)
>    permits structured client-side dispatch. Costs one wire field; gains
>    type-safety on every wire-crossing exception.
> 2. Or: introduce a `BaseError::what_kind()` virtual returning a stable
>    enum, and a `WorkerProto` op-result envelope that carries the kind
>    alongside the rendered text. Coexists with the legacy text-grep for
>    proto < kind-introducing-version; new code uses the kind. The
>    `DynamicDerivations` text-grep then becomes the *example* of how the
>    legacy escape hatch worked, not the *pattern* the next contract has
>    to follow.
> 3. Minimum acceptable: enforce the substring contract at compile-time —
>    a `static_assert` over the throw-site format strings, or a unit test
>    that round-trips every libstore `InvalidPath` / `BadStorePath` /
>    `BadStorePathName` and asserts the rendered text contains
>    `"is not valid"`. This does not fix the architectural debt but stops
>    the migration regressing it silently.
>
> Compounds with #125 (boost::format migration), #176 (`performOp`
> structuring), #182 (`RemoteStore::*` repetition), N23 (declarative
> opcode schema). Cluster fit: A6 (no declarative protocol schema).

### Body annotations on existing candidates

- **#125** (`boost::format` → `std::format` migration). Already references
  the substring contracts via E4's report. Add a body line: "**Hard
  invariants**: the literal substrings `"parsing derivation"`,
  `"expected string"`, `"Derive([")` (DynamicDerivations triple, proto
  < 1.36) and `"is not valid"` (path-info contract, all proto versions)
  must be preserved verbatim across the migration. See E4 and F2 for the
  full matrix and the compile-time enforcement options."
- **#176** (`performOp` switch). Add a body line: "The `case Op::QueryPathInfo`
  body silently swallows `InvalidPath` and writes a `0` byte; combined with
  the client-side `is-not-valid` substring fallback in
  `BasicClientConnection::queryPathInfo`, this is one of two text-grep
  contracts on the wire. A declarative opcode schema (N23) would let the
  exception-result envelope carry a typed discriminator instead of relying
  on rendered text."
- **#182** (`RemoteStore::*` repetition). Add a body line: "The text-grep
  contracts on the wire (DynamicDerivations triple and `is-not-valid`)
  live in `BasicClientConnection`, not in the per-op `RemoteStore::*`
  methods, but the `RemoteStore::queryPathInfoUncached` caller depends on
  the `nullopt`-mapping behaviour. A redesign that moves `RemoteStore::*`
  into the declarative table must preserve or replace this fallback."

## Cross-references

- `doc/inventory/review/E4-daemon-client-error-matrix.md` — full E4 matrix,
  introduces the substring-contract framing.
- `doc/inventory/candidates/14-vestigial-stdlib.md` — #125, the
  boost::format → std::format migration.
- `doc/inventory/candidates/18-daemon-protocol-audit.md` — #176 (`performOp`),
  #182 (`RemoteStore::*` opcode methods).
- `doc/inventory/candidates/22-cross-shard-patterns.md` — N23 (declarative
  opcode schema).
- `doc/inventory/candidates/README.md` — A6 (no declarative protocol schema)
  cluster.

Source sites named (no line numbers per repository convention):

- `src/libstore/worker-protocol-connection.cc` —
  `BasicClientConnection::queryPathInfo`, `processStderrReturn`.
- `src/libstore/local-store.cc` — `LocalStore::queryValidPathId`.
- `src/libstore/store-api.cc` — `Store::queryPathInfo` (sync wrapper),
  `Store::queryPathInfo` (callback form).
- `src/libstore/path.cc` — `checkName`.
- `src/libstore/dummy-store.cc` — `DummyStore::readDerivation`,
  `DummyStore::queryPathInfoUncached`.
- `src/libstore/binary-cache-store.cc` — `BinaryCacheStore::addToStoreCommon`.
- `src/libstore/daemon.cc` — `case WorkerProto::Op::QueryPathInfo`,
  `processConnection` outer catch.
- `src/libstore/include/nix/store/store-api.hh` — `MakeError(InvalidPath, Error)`.
- `src/libstore/include/nix/store/store-dir-config.hh` —
  `MakeError(BadStorePath, Error)`, `MakeError(BadStorePathName, BadStorePath)`.
- `src/libutil/error.cc` — `BaseError::recalcWhat`, `BaseError::calcWhat`.

## Open questions

1. **Phase decision for the substring contract**: preserve forever (cheap,
   constrains future libstore error wording), wire-format change to add a
   typed discriminator (expensive, structurally clean), or compile-time
   substring assertion plus eventual structural fix (intermediate)? E4
   listed this as an open question; the decision blocks Phase 2 of the
   `boost::format` migration.
2. **Should the daemon-side `catch (InvalidPath &)` in
   `case WorkerProto::Op::QueryPathInfo` be widened to also swallow
   `BadStorePathName` / `BadStorePath`?** Today, those throws (e.g. from
   `Serialise<StorePath>::read` on a malformed path string) propagate to
   `processConnection`'s outer catch as a generic error containing
   `"is not valid"`, and the client silently maps to `nullopt`. The current
   behaviour is arguably wrong (a malformed path *name* is not the same as
   a missing path), but the wire grep happens to absorb it. A pre-existing
   latent bug; not in scope for the boost::format migration but worth
   capturing.
3. **Should the migration plan also enumerate libstore throws whose
   rendered text *contains* `"is not valid"` as a substring of an interpolated
   argument (rather than the format string)?** E4 only enumerated format
   strings; no scan of `printStorePath`-produced text or path-name argument
   text was performed. Probability is low (no Nix path is named
   `is-not-valid`) but non-zero.
4. **Test coverage today**: does any unit or functional test exercise the
   `"is not valid"` substring fallback path? The `case Op::QueryPathInfo`
   structured-byte path is covered by `RemoteStore` tests, but the fallback
   substring grep would only trigger under proto < 1.17 — which the test
   harness cannot construct because `MINIMUM_PROTOCOL_VERSION` excludes it.
   The fallback is therefore *operationally untested in CI* and only fires
   if an out-of-tree daemon misbehaves. This is itself an argument for
   removing the fallback (or replacing it with a structured discriminator).
