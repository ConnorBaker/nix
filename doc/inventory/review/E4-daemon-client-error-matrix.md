# E4 — daemon-client error-message preservation matrix

Charter: build a decision-ready test plan that lets candidate #125
(`boost::format` + `HintFmt` -> `std::format`) land safely across a wire boundary.
The risk is that a daemon at protocol version N+1 with rephrased exception text
talks to a client at version N (or vice versa) and silently changes user-visible
strings or breaks downstream parsers.

## Wire-crossing path

The exception-text wire path is:

1. Server-side `throw Error(fs, args...)` constructs `BaseError::err` with
   `msg = HintFmt(fs, args...)`. `HintFmt` wraps a `boost::format` instance and
   eagerly interpolates each argument via `operator%`, wrapping non-`Uncolored`
   arguments in `Magenta<T>` so they print with ANSI magenta. After construction,
   `HintFmt::str()` returns the fully-interpolated string with embedded ANSI
   escapes around interpolated arguments. The format-string template itself is
   discarded after interpolation; `HintFmt` does not retain it.

2. The thrown `Error` propagates up to `processConnection`'s outer try in
   `src/libstore/daemon.cc`. The catch site calls `tunnelLogger->stopWork(&e)`
   in `TunnelLogger`. For protocol version >= 1.26, the path is
   `to << STDERR_ERROR << *ex`. For older protocols (1.18..1.25 still in
   `MINIMUM_PROTOCOL_VERSION`), the path is
   `to << STDERR_ERROR << ex->what() << ex->info().status`. `ex->what()` returns
   `calcWhat()` which calls `showErrorInfo(oss, err, loggerSettings.showTrace)`.
   Note: this differs from `ex->info().msg.str()` — `what()` is the *full
   rendered* error including "error: " prefix, traces, and source location;
   `info().msg.str()` is just the un-prefixed message body.

3. The `Sink & operator<<(Sink &, const Error &)` overload in
   `src/libutil/serialise.cc` writes:
   - the literal string `"Error"` (kind discriminator)
   - the level (`Verbosity` int)
   - the literal string `"Error"` (a removed field — type-name placeholder)
   - `info.msg.str()` — the fully-interpolated message body, including ANSI
     codes from `Magenta<T>` wrapping
   - a position-present byte (always `0` — `info.errPos` is FIXMEd out)
   - the trace count
   - per trace: a position-present byte (always `0`), then `trace.hint.str()`

4. Client-side `processStderrReturn` in
   `src/libstore/worker-protocol-connection.cc` reads `STDERR_ERROR`. For
   proto >= 1.26 it calls `readError(from)` from `src/libutil/serialise.cc`,
   which reads the same fields and reconstructs an `Error` whose `info.msg` is
   `HintFmt(msg)` — the *string* constructor of `HintFmt`. That constructor is
   a special-case `HintFmt("%s", Uncolored(literal))`, so the received text is
   re-wrapped without re-formatting. For proto < 1.26 the client reads
   `(error_string, status)` and constructs `Error(status, error)`, which in
   `BaseError(unsigned, Args...)` runs the args through `HintFmt(args...)` —
   which means `error` *is* re-passed through `boost::format`. If the daemon
   text contains a literal `%` character, the client's older path will
   misinterpret it as a placeholder. (This pre-existing hazard explains why
   the v1.26 wire change exists.)

5. The reconstructed `Error` is thrown from `processStderrReturn` via
   `std::rethrow_exception(ex)` in `processStderr`. The catch is in callers
   of `processStderr` such as `RemoteStore::*` methods. There is one
   compatibility hook in `processStderrReturn`: for `Xp::DynamicDerivations`
   with proto < 1.36, the client greps `e.msg()` for substrings
   `"parsing derivation"` and `"expected string"` and `"Derive(["`; if all
   three are found it wraps the error with extra hint text. **This is the only
   in-tree user of error-text grepping today and is itself a soft-coupling.**

6. Position info is **stripped** on the wire (FIXME comments). Stack-frame
   positions are not recoverable client-side. Only message body and trace
   bodies survive.

### Text-mutation summary

The text on the wire is `info.msg.str()` after Boost interpolation. Migrating
`HintFmt` to `std::format` changes the interpolated text (potentially) at four
levels:

- The *format string* in source is rewritten (`%s` -> `{}`), but this is
  not what's on the wire.
- The *interpolation function* changes (`boost::format` vs `std::format`).
  These differ in: numeric output (`%d` vs `{}`), float precision/padding
  defaults, integer base default, locale handling (Boost respects locale by
  default for some types; `std::format` is locale-independent except `'L'`),
  string_view handling, `nullptr` rendering, pointer rendering, boolean
  rendering ("0"/"1" vs "true"/"false"), and behaviour when arg count
  mismatches.
- The *Magenta wrapping* is preserved if `std::formatter<Magenta<T>>` is added
  with the same `<<` semantics.
- The *positional-arg semantics* (`%1%`, `%2$s`) differ from `{0}`, `{1}`.
  Boost positional indexes are 1-based; `std::format` is 0-based. A naive
  textual `%1%` -> `{0}` rewrite catches this, but anything with reused
  positional refs (e.g., `%1% %1%`) needs human review.

### Operations that emit errors over the wire

`performOp` in `daemon.cc` is the canonical site. Every `case
WorkerProto::Op::*` either throws inline (with a literal-format-string `Error`)
or calls into `*store->...` which can throw any `Error` from libstore. Inline
throws in `performOp`:

- `WorkerProto::Op::AddToStore` (old-protocol branch only):
  `throw Error("unsupported FileIngestionMethod with value of %i; you may need to upgrade nix-daemon", recursive)`
- `WorkerProto::Op::BuildPaths`, `BuildPathsWithResults`:
  `throw Error("repairing is not allowed because you are not in 'trusted-users'")`
- `WorkerProto::Op::BuildDerivation`:
  `throw Error("you are not privileged to build input-addressed derivations")`
- `WorkerProto::Op::AddPermRoot`:
  `throw Error("you are not privileged to create perm roots\n\nhint: ...")`
- `WorkerProto::Op::CollectGarbage`:
  `throw Error("Garbage collecting specific paths requested but it is not supported by the negotiated protocol")`
  and `throw Error("you are not allowed to ignore liveness")`
- `WorkerProto::Op::VerifyStore`:
  `throw Error("you are not privileged to repair paths")`
- `WorkerProto::Op::AddBuildLog`:
  `throw Error("you are not privileged to add logs")`
- `default` case:
  `throw Error("invalid operation %1%", op)`

All other errors propagate through `*store->...` calls — i.e., **every**
`throw Error(...)` in `src/libstore/` is potentially wire-crossing when
running under daemon transport.

## Format-site categorisation

The codebase has roughly 213 raw `boost::format` / `HintFmt` matches and ~480
Boost-style `%`-pattern occurrences inside string literals across `src/`. We do
not enumerate them site-by-site; we partition by directory and call-pattern,
because the wire-crossing question is fully determined by the throwing
component.

### Local-only sites (do not cross the daemon wire)

These sites never produce text that travels over `WorkerProto`:

- `src/libexpr/` — `~107` `throw *Error(...)` sites. **Eval errors do not cross
  the daemon wire.** Verified: `src/libstore/daemon.cc`,
  `src/libstore/worker-protocol*.{cc,hh}`, and `src/libstore/include/` contain
  no `EvalState` / `EvalCache` / libexpr include. The daemon never evaluates
  Nix expressions during a `WorkerProto::Op`. Eval is driven by the *client*
  process (`nix`, `nix-build`, `nix-instantiate`, `nix-env`), which throws
  `EvalError`/`UndefinedVarError`/`TypeError`/`AssertionError` locally. Any
  eval-driver text the daemon would see is store-API text the eval driver
  passed *into* libstore calls, not eval text leaving the daemon.
- `src/libcmd/`, `src/nix/`, `src/libmain/`, `src/libfetchers/`, `src/libflake/`
  — together ~215 `throw *Error(...)` sites. These are all client-side
  components. They run alongside the eval driver and may receive daemon errors
  via `RemoteStore::*`, but their *own* throws never traverse the wire. They
  are local-only with respect to candidate #125's wire-preservation question.
- All `printError(...)`, `printInfo(...)`, `printWarning(...)`,
  `printTalkative(...)`, `notice(...)`, `debug(...)`, and `logger->log(...)`
  call sites — ~202 print-family calls plus ~17 explicit `logger->log` calls.
  These render directly to the local logger and are not exception-text.
  (Daemon-side log lines do travel the wire as `STDERR_NEXT`/`STDERR_RESULT`
  frames, separate from the `STDERR_ERROR` exception path; that is out of
  scope for this matrix because those frames are emitted via `Logger::log`
  rather than `HintFmt::str()` and the structured-result frames already use
  serialised fields, not free-form Boost text.)

### Wire-crossing sites (potentially cross the daemon wire)

- `src/libstore/` — 199 `throw *Error(...)` sites, of which 61 use the Boost
  positional `%1%`/`%2%` form. Inline throws in `daemon.cc::performOp` were
  enumerated in the prior section. Every other libstore throw is reachable via
  some `RemoteStore::*` call path that the daemon executes inside `performOp`.
  We treat **all** of `src/libstore/` as potentially wire-crossing.
- `src/libutil/` — 17 `throw *Error(...)` sites. These include `SysError`,
  `FormatError`, `WinError`, etc. Whether they cross depends on the call
  graph: `src/libutil/serialise.cc`, `src/libutil/file-system.cc`,
  `src/libutil/archive.cc`, `src/libutil/hash.cc`, `src/libutil/json-utils.cc`
  are all reachable from libstore (the daemon links libutil). Treat **all 17**
  libutil throws as potentially wire-crossing. The `FormatError("expected
  string '%1%'", ...)` in `src/libstore/derivations.cc` (libstore, not libutil
  — corrected count) is one of the strings the `DynamicDerivations` text-grep
  contract depends on.
- `boost::wformat` in `src/libutil/windows/` — out of scope; wide-format paths
  are Windows-only and never reach the Unix daemon serialise path. The two
  `boost::wformat` sites use `%X` width and produce strings stored in a
  separate exception path (`WinError`) but those would still cross via
  `BaseError::err.msg` if a Windows daemon ever existed; this is theoretical
  today.

### Both (sites that emit locally and may also be propagated)

Some libstore throws are caught locally by libstore's own retry/recover code
and re-emitted as warnings (e.g., GC-root cleanup warnings in
`local-store.cc`). Those go out as logger frames *and* may also be re-thrown.
Treat them as wire-crossing for safety; the categorisation rule is
"throwability is sufficient", not "always thrown".

### Rules of thumb

- Throw in `src/libstore/` or `src/libutil/`: assume wire-crossing.
- Throw in `src/libexpr/`, `src/libcmd/`, `src/nix/`, `src/libmain/`,
  `src/libfetchers/`, `src/libflake/`: not wire-crossing.
- `print*`, `notice`, `debug`, `logger->log`: not wire-crossing for the
  exception-text matrix (separate logger-frame question).

## Version-sensitive cases

We classified the format-string content of wire-crossing sites
(libstore + libutil throws plus the `performOp` inline throws). Counts are
from `grep` over the literal-string contents inside `throw *Error(...)`:

- **Plain `%s` (string)**: dominant. 625 occurrences across libstore + libutil
  in any context; the throw-only subset is the large majority of the 199 + 17
  sites. Mechanical `%s` -> `{}` translation is safe as long as the argument
  type already has a `<<` overload that matches `std::format` rendering.
  Subtle pitfall: Boost calls `operator<<` on the argument; `std::format`
  needs a `formatter<T>` specialisation. The candidate's stub `formatter<T>`
  that delegates to `<<` preserves text identity for all currently used
  argument types (verified via the `Magenta<T>` interaction in the prior
  section).
- **Plain `%d` / `%i` (integer)**: 104 + 2 occurrences in libstore, 35 + 0 in
  libutil. Identical numeric output between Boost `%d` and `std::format` `{}`
  for `int`/`long`/`size_t`. **Caveat**: Boost `%d` with a `bool` argument
  prints `"0"`/`"1"`; `std::format` `{}` prints `"true"`/`"false"`. Audit any
  throw whose `%d` argument has type `bool`. Quick scan finds none in
  wire-crossing throws (a few in eval-only paths).
- **Boost positional `%N%`**: many — 135 in libstore, 89 in libutil, 107 in
  libexpr; 61 of the libstore positional uses appear inside `throw` calls.
  Mechanical translation `%1%` -> `{0}`, `%2%` -> `{1}` is safe **only if no
  positional index is reused**. Reused positional refs (`%1% %1%`) found:
  - `src/libfetchers/git.cc` (`fmt("%1%:%1%", ref)`) — local, not
    wire-crossing.
  - No reused positional refs found in `src/libstore/` throws. (Verified with
    `grep -rPn '"\s*%([1-9])%[^"]*%\1%' src/libstore/`; no matches.)
  Translation can be mechanical for all libstore positional sites.
- **Precision specifiers `%.Nf` / `%.Ng` / `%.Ne`**: a few — 3 sites total in
  the codebase, of which 2 cross the wire:
  - `src/libstore/filetransfer.cc` — `"... duration = %.2f s"` inside what
    looks like a `printInfo(...)` call (verify); local-only.
  - `src/libstore/unix/build/derivation-builder.cc` — `"... user CPU %.3fs,
    system CPU %.3fs"` inside a `BuildError` thrown from the builder; this
    crosses the wire when a remote builder fails. Hard-break risk if locale
    or rounding differs between Boost and `std::format`.
  - `src/libutil/util.cc` — `fmt("%6.1f", result)` in `renderSize`; the result
    is embedded into other format strings via `%s`. Crosses indirectly when
    sizes appear in error text (e.g., `binary-cache-store.cc` "compressed
    %3$.1f%%").
- **Width modifiers `%Nx`, `%Ns`, `%Nd`**: ~6 in actual format-string
  contexts (most of the 52 raw matches were URL test data). All inside
  `fmt(...)` for column alignment in CLI output (`nix path-info`, `nix ls`);
  none in wire-crossing throws. Local-only.
- **Unusual conversions `%c` (char), `%x` / `%X` (hex), `%o` (octal),
  `%a`/`%g`/`%e`/`%p`**: a few:
  - `%c` in `src/libutil/base-nix-32.cc`, `src/libutil/base-n.cc`,
    `src/libutil/util.cc` — `FormatError(... '%c' ...)` for invalid character
    diagnostics. Wire-crossing (libutil). `std::format` `{}` on `char` prints
    the character (matches Boost `%c`); safe.
  - `%x` in `src/libstore/worker-protocol-connection.cc` —
    `Error("got unsupported field type %x from Nix daemon", (int) type)` and
    `Error("got unknown message type %x from Nix daemon", msg)`. These are
    *client-side* errors raised when parsing daemon output, so they never
    *enter* the wire; they describe wire-format bugs. `std::format` lacks a
    direct `%x` analogue — needs `{:x}`. Mechanical translation requires
    awareness of the conversion. Flag for human review.
  - `%X` in `src/libutil/windows/file-system.cc` — Windows-only, theoretical
    wire crossing as discussed above.
  - `%o`, `%a`, `%g`, `%e`, `%p` — none in wire-crossing throws.
- **Positional dollar-form `%N$X`**: 2 occurrences:
  - `src/libstore/binary-cache-store.cc` — `"copying path '%1%' (%2% bytes,
    compressed %3$.1f%% in %4% ms) to binary cache"` inside a `printInfo`.
    Local-only logger frame; not a wire-crossing throw, but uses both
    positional and precision in one string. Note: the `%%` produces a literal
    `%`, which `std::format` writes as `{{`/`}}` only for braces — `%` is not
    special in `std::format` so the `%%` literal needs to become `%`.
  - `src/libstore/unix/build/derivation-builder.cc` — similar.
- **`%%` literal-percent escapes**: appear wherever a literal percent sign is
  needed (e.g., percent-encoding hints, percent-completion hints). These are
  Boost-specific; in `std::format` the equivalent is just `%` (no escape).
  Mechanical translation: replace `%%` with `%` and rebalance. There are a
  handful of these in wire-crossing throws (verified via
  `grep -rPn '%%' src/libstore/`); each needs a mechanical edit.

### Aggregate verdict

The vast majority of wire-crossing format strings (>= 90%) are
`%s`/`%d`/`%N%`-only and translate mechanically. The risky long tail is:

1. The 2 wire-crossing `%.Nf` precision sites (locale/rounding).
2. The 2 client-side `%x` diagnostic sites (different conversion specifier).
3. The handful of `%%` literal escapes.
4. Anywhere `Magenta<T>` wrapping interacts with the new `formatter<T>`
   shim — the prior section already noted this is an invariant the migration
   must preserve.

## Test matrix specification

### Protocol versions in scope

`MINIMUM_PROTOCOL_VERSION` is `1.18` (`(1 << 8) | 18` in
`src/libstore/include/nix/store/worker-protocol.hh`); current
`PROTOCOL_VERSION` is `1.39` (the catalog inventory said `{1, 38}`; the value
has bumped to `{1, 39}` since the inventory was written — **flag the inventory
for refresh**). The wire-format change at `1.26` is the load-bearing version
boundary because the older format re-runs received text through `HintFmt`,
hitting the literal-`%` hazard. The `DynamicDerivations` text-grep contract is
specific to clients with proto `< 1.36` (per the prior section).

The matrix must cover:

- proto `1.18` (lowest supported client talking to current daemon)
- proto `1.25` (last version using the older error wire format)
- proto `1.26` (first version using `Sink << Error` structured wire format)
- proto `1.35` (last version where `DynamicDerivations` text-grep applies)
- proto `1.36` (first version after the text-grep contract retires)
- proto `1.39` (current)

Each case must run in **both directions**: old-client/new-daemon and
new-client/old-daemon, because the failing direction is asymmetric (the older
wire format re-interprets the text on the *receiving* side, so a daemon
emitting a `%`-bearing string to a `< 1.26` client is the failure mode).

### Call paths to test

1. **Every inline `throw Error(...)` in `performOp`** (8 sites enumerated in
   the prior section). Each must be triggerable by a crafted client request:
   - `AddToStore` with old-protocol `recursive` value out of range
   - `BuildPaths` / `BuildPathsWithResults` repair from non-trusted user
   - `BuildDerivation` input-addressed from non-trusted user
   - `AddPermRoot` from non-trusted user
   - `CollectGarbage` with specific paths on old protocol; ignoreLiveness
     from non-trusted user
   - `VerifyStore` repair from non-trusted user
   - `AddBuildLog` from non-trusted user
   - default-case unknown op (send `0xFFFF`)
2. **A representative sample of library-thrown errors** propagating through
   `performOp`. Required samples:
   - `InvalidPath("path '%s' is not valid", ...)` from `local-store.cc` —
     **hard test requirement**, see hazard below.
   - `FormatError("expected string '%1%'", ...)` from `derivations.cc` —
     **hard test requirement**, see hazard below.
   - `Error("error parsing derivation '%s': %s", ...)` from `store-api.cc` —
     **hard test requirement**, see hazard below.
   - `SysError(...)` from a filesystem failure (e.g., delete a store path
     between `queryPathInfo` and `narFromPath`).
   - `BuildError(...)` from a builder failure with the `%.3fs` CPU-time
     precision specifier (`derivation-builder.cc`).
   - `BadStorePathName(...)` triggered via `AddToStore` with an invalid name.
   - An error whose argument carries an embedded literal `%` (e.g., a path
     containing `%`). This is the Boost-`%`-rehydration trap on the older
     wire format.

### Hard test requirements

The matrix has **two** error-text-grep contracts that must not regress, not
one:

1. **`DynamicDerivations` triple** (proto `< 1.36`,
   `src/libstore/worker-protocol-connection.cc::processStderrReturn`): client
   greps `e.msg()` for all three of `"parsing derivation"`, `"expected
   string"`, `"Derive(["`. The test must construct an old-client / current-
   daemon scenario where the daemon throws while parsing a derivation file
   that begins with `Derive([` but is otherwise malformed, such that:
   - `store-api.cc` adds the prefix `"error parsing derivation '<path>': "`
   - `derivations.cc` raises `FormatError("expected string '%1%'", ...)`
     where the offending fragment contains `Derive([`.
   - The combined `e.msg()` contains all three substrings.
   The test asserts the client wraps with the legacy hint. If the migrated
   daemon rephrases any of these three substrings (e.g., to `"parsing
   derivation file"` or `"expected literal"` or to a non-`%1%` form that
   reformats `Derive([`), the wrapping breaks silently. **No CI check would
   notice without this test**.
2. **`"is not valid"` substring** (all proto versions,
   `WorkerProto::BasicClientConnection::queryPathInfo`): if the daemon throws
   any error containing `"is not valid"` during `queryPathInfo`, the client
   maps the exception to `std::nullopt` (path-not-found) instead of
   propagating. The daemon-side sources are
   `local-store.cc:811` (`InvalidPath("path '%s' is not valid", ...)`),
   `dummy-store.cc:328`, and `store-api.cc:610`/`630`. The migration must
   preserve the literal substring `"is not valid"` in the rendered text.
   `path.cc` also throws `BadStorePathName("name '%s' is not valid", ...)`,
   and (less obviously) the `binary-cache-store.cc` "...the reference '%s'
   is not valid" message *also* contains the substring. The contract is
   coarse: any daemon error containing `"is not valid"` during
   `queryPathInfo` means "no such path", regardless of why it was thrown.
   **The migration must preserve this exact substring** in every libstore
   path-validity message that can surface during `queryPathInfo`.

### Assertion strategy

Pick **canonical normalisation**, not exact-text pinning. Reasoning: ANSI
escape codes from `Magenta<T>` are part of `info.msg.str()` on the wire, but
they vary with terminal-detection state and would make exact-text assertions
brittle. The recommended assertion shape is:

1. Strip ANSI escape sequences from the received `e.msg()` (or pass through
   `filterANSIEscapes` if available — verify; the codebase has it).
2. Assert exact equality on the stripped text against a golden string.
3. For the two hard-break substring contracts above, additionally assert
   `e.msg().find("is not valid") != npos` (resp. the three
   `DynamicDerivations` substrings) on the **un-stripped** text — those
   contracts use the same field libstore greps in production.
4. Do **not** assert on `e.what()` — it includes the `"error: "` prefix,
   showTrace state, and source positions, none of which the wire preserves
   (positions are FIXMEd to zero per the prior section).

The matrix should run as a tabletest with one row per
(protocol-version, error-site) pair, sharing a single in-process daemon
fixture (`src/libstore-tests/` already has `WorkerProto` round-trip test
infrastructure; extend it).

## Migration order

### Phase 1 — local-only sites (low risk)

Migrate non-wire-crossing format strings first. Scope:

- `src/libexpr/` — all 107 throws plus the print/log family.
- `src/libcmd/`, `src/nix/`, `src/libmain/`, `src/libfetchers/`,
  `src/libflake/` — all 215 throws plus print/log.
- All `printError`/`printInfo`/`printWarning`/`logger->log` callers in any
  directory.

Test gate: existing unit tests + lang-tests + functional tests. No new test
harness required. Phase 1 is mechanical and parallelisable.

### Phase 2 — wire-crossing strings without modifiers

Scope: libstore + libutil throws whose format string is a subset of `%s`,
`%d`, `%i`, `%N%` (positional, no reuse), `%c`. **Roughly the great majority**
of the 199 + 17 = 216 throws.

Test gate: **the matrix test must land before Phase 2 begins**. Phase 2 is
mechanical translation but the ANSI-stripped golden strings will need to be
regenerated after each batch; the matrix test makes that a fast feedback
loop.

The two text-grep substring contracts (`"is not valid"` and the
`DynamicDerivations` triple) are validated as part of the matrix test, so
Phase 2 cannot regress them silently.

### Phase 3 — wire-crossing strings with modifiers (per-site human review)

Scope:

- The 2 wire-crossing `%.Nf` precision sites (one in `derivation-builder.cc`,
  one in `binary-cache-store.cc` indirectly via `renderSize`).
- The 2 client-side `%x` hex sites in `worker-protocol-connection.cc`.
- All `%%` literal-percent occurrences (verify with a final grep before
  Phase 3 starts).
- Any sites flagged by Phase 2 review as ambiguous (`bool` arguments through
  `%d`, `Magenta<T>` interactions, etc.).

Each site requires a per-site decision and a matrix-test row pinning the
expected output. No batch translation.

### Phase 4 — coordination with v1.36 cutover

The `DynamicDerivations` text-grep contract retires when minimum supported
client version reaches `1.36`. Two options:

- **Preserve the substrings forever**. Mechanical: keep `"parsing
  derivation"`, `"expected string"`, `"Derive(["` literally in the rendered
  text. Cheap; constrains future rephrasing.
- **Coordinate with `MINIMUM_PROTOCOL_VERSION` bump**. When the minimum
  bumps to `>= 1.36`, the `processStderrReturn` text-grep block becomes
  dead; remove it and document the cutover. The migration's sole obligation
  is to not break the contract before the minimum bump.

The `"is not valid"` substring contract has **no version cutoff** — it lives
in the current `queryPathInfo` path for all proto versions. Either preserve
the substring forever, or design a structured replacement (e.g., a typed
`InvalidPath` discriminator on the wire) before Phase 2 ships.

## Soft-break vs hard-break inventory

### Soft breaks

- Any `Magenta<T>` ANSI-escape changes (e.g., a different sequence for
  emphasis). User-visible only.
- Numeric formatting differences for `bool` through `%d` -> `{}` (`"0"` vs
  `"true"`). Visible but recoverable; document in changelog.
- Trailing-zero differences in `%.Nf` -> `{:.Nf}` translations. Visible in
  build-failure error text.
- Locale-sensitive separators (Boost respects locale for some types;
  `std::format` does not unless `{:L}`). Not currently triggered in any
  wire-crossing site — most `%d` arguments are `int`/`size_t` for which
  Boost also does not insert thousand-separators by default. Low-probability
  soft break.
- Source-line trace positions are stripped on the wire today (FIXME); not a
  break, just a pre-existing data loss.

### Hard breaks

In-tree text-grep contracts the migration must preserve (or coordinate
removal of):

1. `"parsing derivation"` + `"expected string"` + `"Derive(["` — all three
   substrings, in any order, in the rendered `e.msg()` of a single error.
   Used by `processStderrReturn` for proto `< 1.36`. Source sites:
   `src/libstore/store-api.cc`, `src/libstore/derivations.cc`,
   `src/libstore/build/derivation-goal.cc`,
   `src/libstore/build/derivation-building-goal.cc`,
   `src/libstore/misc.cc`.
2. `"is not valid"` — substring. Used by
   `WorkerProto::BasicClientConnection::queryPathInfo` for **all** proto
   versions. Source sites: `src/libstore/local-store.cc`,
   `src/libstore/dummy-store.cc`, `src/libstore/path.cc`,
   `src/libstore/store-api.cc`, `src/libstore/binary-cache-store.cc` (note:
   the `binary-cache-store.cc` site triggers the contract incidentally
   because its message contains `"the reference '%s' is not valid"`; this is
   probably a pre-existing latent bug — the client may interpret an
   add-to-cache failure as path-not-found. Flag for separate review).
3. `"unexpected end of file"` — substring. Used by `src/libcmd/repl.cc`
   `runRepl` to detect a multiline-continuation case. **Eval-only**, so this
   is not a daemon wire-crossing contract; included here only because it is
   the third in-tree text-grep contract and Phase 1 must not regress it
   either.

No other `e.what()` / `e.msg()` text-grep contracts were found in
non-test code (`grep -rn 'e\.what()\|e\.msg()' src/ --include='*.cc' |
grep find/search/contains`).

External (out-of-tree) hard-break risks the migration cannot enumerate but
must call out in the release notes:

- Any tooling that greps daemon error text via `nix-daemon --stdio` proxies.
- Any test fixtures in downstream projects (NixOS, nixpkgs CI tooling) that
  pin daemon exception text.
- Build-farm log scrapers that classify errors by substring.

The migration cannot prevent these; the obligation is to document the format
shift in the release notes and provide a migration guide pointing at the
canonical normalisation strategy.

## Open questions

1. **Phase 4 strategy for `"is not valid"`**: do we preserve the substring
   forever (cheap, constrains future rephrasing), or design a structured
   `InvalidPath`-discriminator wire change concurrent with the migration
   (expensive, clean)? Decision needed before Phase 2 begins.
2. **`binary-cache-store.cc` `"the reference is not valid"` site**: does it
   genuinely trigger the `queryPathInfo` substring contract in the wild,
   or is it unreachable from `queryPathInfo`? If reachable, this is a
   pre-existing bug that should be fixed before the migration so the
   migration can rely on a clean contract.
3. **`Magenta<T>` formatter shim**: will the migration provide a
   `std::formatter<Magenta<T>>` that delegates to `operator<<`, or rewrite
   the magenta-wrapping convention entirely? The wire format depends on the
   ANSI bytes being present in `info.msg.str()`; either choice works but
   needs to be made before Phase 2.
4. **Inventory drift**: the catalog claims current proto is `{1, 38}`; the
   header says `{1, 39}`. Refresh the catalog before relying on it for
   migration sequencing.
5. **Logger-frame text** (`STDERR_NEXT` / `STDERR_RESULT`) is out of scope
   for this matrix but uses the same `HintFmt` machinery on the daemon side.
   Should Phase 2 also cover daemon-side `Logger::log` calls, or is
   logger-frame text considered free to rephrase? Confirm with the user.
6. **Windows wide-format paths** (`boost::wformat` in `src/libutil/windows/`):
   are they in scope for the migration? They have no current wire-crossing
   path but block a fully-uniform migration.

