# B3 — investigate #18 split into 18a/b/c

**Charter:** decide whether to apply pass-2's proposed split of catalog
candidate #18 (`MakeError`/`CloneableError` duplication) into 18a/18b/18c,
or leave the umbrella entry as-is. The consolidator deferred and chose
"keep #18 with three sub-axes spelled out in body". This investigation
re-examines that decision against the source.

## Source-level inventory

### The macro and the CRTP base

`src/libutil/include/nix/util/error.hh` defines:

- `class BaseError : public std::exception` — root of the hierarchy;
  carries a mutable `ErrorInfo`, a `what_` cache, a virtual
  `throwClone()` (pure on `BaseError`), a public copy/move surface, and
  a family of variadic constructors (`(status, fs, args...)`,
  `(fs, args...)`, `(Suggestions, args...)`, `(HintFmt)`,
  `(ErrorInfo&&)`, `(const ErrorInfo&)`).
- `template<class Derived, class Base> class CloneableError : public Base`
  — CRTP; declares `friend Derived`, defines a `default` private ctor,
  hoists `Base`'s constructors via `using Base::Base`, and overrides
  `throwClone()` as `throw Derived(static_cast<const Derived&>(*this))`.
- `MakeError(name, parent)` macro — expands to
  `class name : public CloneableError<name, parent> { public: using
   CloneableError<name, parent>::CloneableError; }`.

Three pre-baked `MakeError` calls sit immediately below the macro
definition: `Error`, `UsageError`, `UnimplementedError`. The `Interrupted`
exception is also `MakeError`-defined in `signals.hh`.

### `MakeError` call sites (52 unique invocations, 3 test sites)

`src/libutil-tests/logging.cc` (3 test-local sites), `src/libcmd/repl.cc`
(`IncompleteReplExpr`), `src/libcmd/include/nix/cmd/unix-socket-server.hh`
(`AbortServeSocket`), the eval-error block in
`src/libexpr/include/nix/expr/eval-error.hh` (`EvalError`, `ParseError`,
`AssertionError`, `ThrownError`, `Abort`, `TypeError`, `UndefinedVarError`,
`MissingArgumentError`, `InfiniteRecursionError`, `IFDError`,
`RecoverableEvalError`), plus single-line invocations in
`value-to-json.hh`, `attr-path.hh` (×2), `json-to-value.hh`, `hash.hh`,
`url.hh`, `canon-path.hh`, `compression-algo.hh`, `executable-path.hh`,
`serialise.hh`, `thread-pool.hh`, `error.hh`, `signals.hh`, `util.hh`,
`compression.hh`, `file-descriptor.hh`, `source-accessor.hh` (×6 incl.
`SourceAccessorError`, `FileNotFound`, `NotASymlink`, `NotADirectory`,
`NotARegularFile`, `RestrictedPathError`),
`http-binary-cache-store.cc` (`UploadToHTTP`),
`s3-binary-cache-store.cc` (`UploadToS3`), `realisation.cc`
(`InvalidDerivationOutputId`), `binary-cache-store.hh`
(`NoSuchBinaryCacheFile`), `posix-fs-canonicalise.hh` (`PathInUse`),
`store-dir-config.hh` (`BadStorePath`, `BadStorePathName`),
`store-api.hh` (`InvalidPath`, `Unsupported`, `SubstituteGone`,
`SubstituterDisabled`, `InvalidStoreReference`), `s3-url.hh`
(`InvalidS3AddressingStyle`), `sqlite.hh` (`SQLiteBusy`).

Notable: most `MakeError` calls have parent `Error` or `BaseError`; a
small minority extend other `MakeError`-defined classes (`AssertionError`
under `EvalError`, `ThrownError` under `AssertionError`, the four
`SourceAccessor*` errors under `SourceAccessorError`, `BadStorePathName`
under `BadStorePath`); and **one** extends a hand-written
`CloneableError` derivation: `MakeError(SQLiteBusy, SQLiteError)`.

### Hand-written `CloneableError<Derived, Base>` derivations

Source: 17 hand-written derivations (the catalog body said "14"; the
verified count is higher). Listed by header path with parent and
distinguishing reason:

| Class | Path | Parent | Why hand-written |
| --- | --- | --- | --- |
| `SystemError` | `libutil/include/nix/util/error.hh` | `Error` | Two extra fields (`std::error_code errorCode`, `std::string errorDetails`); two protected disambiguator-tag constructors (`DisambigHintFmt`, `DisambigVarArgs`); accessors (`ec()`, `is(std::errc)`). Acts as base for `SysError`/`WinError`. |
| `SysError` | `libutil/include/nix/util/error.hh` | `SystemError` | Adds `int errNo`; a variadic ctor that captures `errno` and calls `strerror(errno)`; an `(int, HintFmt)` overload; a constrained-template `mkHintFmt`-callable ctor with a private `captureErrno` helper; a private `(std::pair<int, HintFmt>&&)` delegating ctor. The `friend Derived` in `CloneableError` is irrelevant to its needs — what matters is access to `SystemError`'s protected `DisambigVarArgs`/`DisambigHintFmt` ctors via the inherited `using`. |
| `WinError` | `libutil/include/nix/util/error.hh` (Windows-only) | `SystemError` | Symmetric to `SysError`; adds `DWORD lastError`, `GetLastError()`-capturing ctors, a private `captureLastError` helper, and a `static std::string renderError(DWORD)`. |
| `ExecError` | `libutil/include/nix/util/processes.hh` | `Error` | Adds `int status`; one variadic ctor `(int status, args...)` that forwards args to `CloneableError(args...)`. Pure "extra field + extra ctor parameter" pattern. |
| `MissingExperimentalFeature` | `libutil/include/nix/util/experimental-features.hh` | `Error` | Adds `ExperimentalFeature missingFeature`, `std::string reason`. Single non-template ctor whose body is in the `.cc`. |
| `SymlinkNotAllowed` | `libutil/include/nix/util/source-accessor.hh` | `Error` | Adds `CanonPath path`; a fixed-message ctor `(CanonPath)` and a variadic ctor `(CanonPath, fs, args...)` that forwards to `CloneableError(fs, args...)`. |
| `BadNixStringContextElem` | `libexpr/include/nix/expr/value/context.hh` | `Error` | Adds `std::string_view raw`; variadic ctor that delegates to `CloneableError("")` then mutates `err.msg` directly using `HintFmt(args...)`. **Mutates `err.msg` post-construction** — relies on `BaseError::err` being protected/mutable. |
| `EvalBaseError` | `libexpr/include/nix/expr/eval-error.hh` | `Error` | Adds `EvalState & state` field; declares `EvalErrorBuilder<T>` as friend; provides a `(EvalState&, ErrorInfo&&)` ctor and a variadic `(EvalState&, fs, args...)` ctor. Every `MakeError` descendant of `EvalBaseError` (e.g. `EvalError`, `IFDError`, `RecoverableEvalError`) and transitively their descendants (`AssertionError`, `ThrownError`, `Abort`, `TypeError`, `UndefinedVarError`, `MissingArgumentError`, `InfiniteRecursionError`) inherit the `EvalState&`-first ctor signature. |
| `StackOverflowError` | `libexpr/include/nix/expr/eval-error.hh` | `EvalBaseError` | Single fixed-message ctor `(EvalState&)`. Does *not* inherit from `EvalError` because it must not be cached by the eval cache (resource exhaustion is non-deterministic). |
| `InvalidPathError` | `libexpr/include/nix/expr/eval-error.hh` | `EvalError` | Adds `StorePath path`; ctor body in `.cc`. |
| `CachedEvalError` | `libexpr/include/nix/expr/eval-cache.hh` | `EvalError` | Adds `const ref<AttrCursor> cursor`, `const Symbol attr`; adds custom `force()` method (not a constructor concern — a *behavioural* extension). |
| `BuildError` | `libstore/include/nix/store/build-result.hh` | `Error` | Adds `Status status` (enum), `bool isNonDeterministic`; nested `Args` struct for de-serialisation; default ctor (zero-arg, message `""`); `operator==` and `operator<=>`. Used as the `Failure` variant in `BuildResult::inner`. |
| `BuilderFailureError` | `libstore/include/nix/store/build/derivation-builder.hh` | `BuildError` | Adds `int builderStatus`, `std::string extraMsgAfter`; ctor takes `(BuildResult::Failure::Status, int, std::string)`. |
| `TimedOut` | `libstore/include/nix/store/build/goal.hh` | `BuildError` | Adds `time_t maxDuration`; single ctor declared in header, body in `.cc`. |
| `NotDeterministic` | `libstore/unix/build/derivation-builder.cc` | `BuildError` | Adds *no fields*; the only reason it bypasses `MakeError` is to set `isNonDeterministic = true` in the body and to always pass `BuildResult::Failure::NotDeterministic` as the status enum to the parent. Could plausibly be expressed as a tiny one-liner builder. |
| `MissingRealisation` | `libstore/include/nix/store/realisation.hh` | `Error` | No fields; *three* ctor overloads (delegating to a single `.cc` definition), each shaping the message from a `StoreDirConfig` reference and `DrvOutput`/`StorePath`/`SingleDerivedPath` arguments. |
| `AwsAuthError` | `libstore/include/nix/store/aws-creds.hh` | `Error` | Adds `std::optional<int> errorCode`; `using CloneableError::CloneableError` plus an extra `(int)` ctor and an accessor `getErrorCode()`. |
| `FileTransferError` | `libstore/include/nix/store/filetransfer.hh` | `Error` | Adds `FileTransfer::Error error`, `std::optional<std::string> response`; one variadic ctor `(FileTransfer::Error, std::optional<std::string>, args...)`. |
| `BuildEnvFileConflictError` | `libstore/include/nix/store/builtins/buildenv.hh` | `Error` | Adds `const std::filesystem::path fileA`, `fileB`, `int priority`; single fixed-message ctor. |
| `SQLiteError` | `libstore/include/nix/store/sqlite.hh` | `Error` | Adds `std::string path`, `errMsg`, `int errNo`, `extendedErrNo`, `offset`; one public ctor; one *protected* variadic ctor (delegated by the public one); a `static throw_(sqlite3*, ...)` factory that special-cases SQLite busy/protocol codes by constructing a `SQLiteBusy` (the `MakeError`-defined subclass) using the protected ctor. Acts as a parent for the macro-derived `SQLiteBusy` — the only `MakeError`/`CloneableError` cross-link in the codebase. |
| `InvalidSSHAuthority` | `libstore/ssh.cc` | `Error` | Adds *no fields*; one ctor that takes `(ParsedURL::Authority, std::string_view)` and formats the message. Like `NotDeterministic`, this bypasses `MakeError` only to inject a custom message-format function — could be a free helper. |
| `curlMultiError` | `libstore/filetransfer.cc` | `Error` | Adds `::CURLMcode code`; one ctor that maps the code via `curl_multi_strerror`. |
| `GitError` | `libfetchers/git-utils.cc` | `Error` | Adds *no fields*; two variadic ctors that capture libgit2's `git_error*` and rebuild `err.msg` post-construction (similar to `BadNixStringContextElem`). |

(Catalog body is missing `EvalBaseError`, `StackOverflowError`,
`CachedEvalError`, `InvalidPathError`, `BadNixStringContextElem`,
`GitError`. These are libexpr/libfetchers; the body's "14" enumerates
the libstore + libutil subset. Real count: ~22 hand-written
derivations, of which 3 (`SystemError`, `SysError`, `WinError`) form
the disambiguator-tag triad.)

### The `DisambigHintFmt` / `DisambigVarArgs` tag idiom

`SystemError` declares two empty tag types as protected nested structs
and uses them on protected constructors: one takes a pre-built `HintFmt`,
one is variadic and forwards to the first. Every public ctor on
`SystemError`, `SysError`, `WinError` ultimately funnels through the
`DisambigHintFmt`-tagged ctor. The pattern exists *only* on these
three classes; no other hand-written derivation uses it. The mechanism
is "build the platform-specific human-readable error message string
before calling the base ctor; pass the raw error code and details as
explicit arguments alongside the `HintFmt`".

## Why each hand-written derivation bypasses `MakeError`

Categorising the 22+ hand-written derivations:

| Reason | Classes |
| --- | --- |
| Adds extra fields *and* a custom ctor | `SystemError`, `SysError`, `WinError`, `ExecError`, `MissingExperimentalFeature`, `SymlinkNotAllowed`, `EvalBaseError`, `InvalidPathError`, `BuildError`, `BuilderFailureError`, `TimedOut`, `AwsAuthError`, `FileTransferError`, `BuildEnvFileConflictError`, `SQLiteError`, `curlMultiError`, `CachedEvalError`, `BadNixStringContextElem` |
| Adds extra ctor *only* (no fields) | `StackOverflowError`, `MissingRealisation`, `InvalidSSHAuthority`, `NotDeterministic`, `GitError` |
| Custom non-ctor methods | `SystemError` (`ec`, `is`), `BuildError` (`==`, `<=>`), `CachedEvalError` (`force`), `SQLiteError` (`throw_`) |
| Mutates `err.msg` after base ctor | `BadNixStringContextElem`, `GitError` |
| Tag-disambiguator protected ctors | `SystemError` (only), consumed by `SysError` and `WinError` |
| Acts as base for a `MakeError` derivation | `EvalBaseError` → `EvalError`/`IFDError`/`RecoverableEvalError`; `EvalError` → `AssertionError`/`Abort`/`TypeError`/`UndefinedVarError`/`MissingArgumentError`/`InfiniteRecursionError`; `BuildError` → (no `MakeError`-defined children, but `BuilderFailureError`/`TimedOut`/`NotDeterministic` are hand-written children); `SQLiteError` → `SQLiteBusy` (single such cross-link); `SystemError` → `SysError`/`WinError`; `SourceAccessorError` is itself `MakeError` and does have `MakeError` children, but is not in the hand-written list |

Two subtler observations:

1. The `MakeError` macro inherits parent ctors via `using
   CloneableError::CloneableError`, which itself does `using Base::Base`.
   This is what makes a `MakeError`-defined class *transparently* gain
   any extra-parameter ctors of a hand-written ancestor — see
   `EvalError(state, "msg", args...)` working out of the box, and
   `SQLiteBusy(path, errMsg, err, exterr, offset, hf)` likewise. Any
   refactor of `MakeError` must preserve this constructor inheritance
   behaviour or rewrite all such call sites.
2. The `friend Derived` declaration on `CloneableError` is currently
   load-bearing only for the protected-ctor pathway in
   `SystemError`/`SysError`/`WinError` (and arguably for the `default`
   private ctor of `CloneableError` itself, used by `BuildError`'s
   default ctor). It is *not* required for the simple field-extension
   patterns — those work via plain public-ctor inheritance.

## Independence analysis

### Can 18a (macro → CRTP base) land without touching the 14 (really ~22) hand-written derivations?

**Mostly, but not cleanly.** The `MakeError` macro currently expands
to a CRTP usage: it *already* uses `CloneableError`. The "consolidate
the macro into a CRTP base" framing in the catalog body is therefore
slightly off — the macro is already a thin sugar over CRTP. The
realistic 18a refactor is one of:

1. **Replace the macro with a C++ alias / using-declaration** —
   e.g. `template<class T, class Base = Error> using SimpleError =
   CloneableError<T, Base>` plus `inheriting-ctor` in the body. This
   doesn't reduce LOC much because each derived class still needs `class
   X final : public CloneableError<X, Parent> { public: using
   CloneableError::CloneableError; };`. Marginal value.
2. **Replace the macro with a deducing-this CRTP that emits the class
   declaration via reflection** — would require C++26 reflection, which
   is not available.
3. **Replace `MakeError(X, P)` with a single-line variable-template-style
   declaration** — `using X = CloneableError<X, P>` is illegal (CRTP
   needs `X` named). No way around `class X final : public
   CloneableError<X, P>` without reflection.
4. **Keep the macro but remove `MakeError` in favour of inline CRTP
   declarations everywhere** — makes the source longer, not shorter.
   Anti-pattern.

The honest 18a story is "this isn't really a refactor target — the
macro is already the minimum-viable sugar, and the duplication note
reflects that some sites use the macro and some don't, but the macro
itself is fine". The only meaningful 18a edit is "convert the
hand-written CloneableError sites that don't add anything (e.g.
`InvalidSSHAuthority`, `NotDeterministic`, `GitError`,
`BadNixStringContextElem`) to either `MakeError` or a free
factory function". That's not a `MakeError`-side change at all — it's
a per-class consolidation, which is exactly 18b.

**Conclusion:** 18a is not really an independent refactor of the
macro. The genuinely independent piece is "audit the hand-written
derivations and migrate the no-fields ones to `MakeError`", which is
properly 18b.

There is *one* genuine 18a target: the `MakeError(SQLiteBusy,
SQLiteError)` cross-link is the only place where the macro derives
from a hand-written `CloneableError`. If 18a were redefined to "audit
which `MakeError` parents are hand-written (and therefore inherit
non-trivial ctor signatures)", the answer is "exactly one, and it works
correctly". So even that audit is tiny.

### Can 18b (hand-written derivations → consolidated CRTP base with field support) land without touching 18a?

**Yes, with caveats.** Each of the ~22 hand-written derivations is
essentially self-contained. Touching them does not require modifying
`MakeError` or `CloneableError`. The work is:

- For each class, decide whether the extra fields/ctors *need* to be
  hand-rolled or whether a small extension to `CloneableError` (or a
  helper template like `CloneableErrorWithFields<Derived, Base, Fields...>`)
  could subsume them.
- Migrate the no-field hand-written cases (`InvalidSSHAuthority`,
  `NotDeterministic`, `GitError`, `MissingRealisation`,
  `StackOverflowError`) to `MakeError` plus a free helper, **or** keep
  them hand-written and accept the ~3-line declaration cost.

The proposal "a CRTP base with field support could subsume both
styles" oversells the gain. C++ inheritance does not allow declaring
arbitrary fields from a CRTP base parameter pack without macros or
variadic-tuple boilerplate that's worse than the current shape. The
realistic 18b outcome is "consolidate ~5 of the 22 hand-written cases
into `MakeError`; leave 17 as-is because their extra fields are
intrinsic to the error type". Effort: small-medium per class,
parallelisable, **independent of 18a/18c**.

The one cross-axis dependency: `MakeError(SQLiteBusy, SQLiteError)`.
If 18b refactors `SQLiteError`'s constructor surface, `SQLiteBusy`'s
inherited ctors change. But this is fully internal to libstore and
is what the catalog body would call "see also: per-class
consideration".

### Can 18c (`SysError` tag idiom) land independently of either?

**Yes, fully.** The `DisambigHintFmt`/`DisambigVarArgs` tag idiom is
strictly local to `SystemError`/`SysError`/`WinError`. Three options:

1. Replace the tag pattern with C++20 deducing-this or named ctor
   helpers — purely internal to those three classes.
2. Extract the "build the platform-specific message before passing to
   the base" logic into a `static` factory on `SystemError` and remove
   the protected tag ctors.
3. Push the platform-detail capture (`strerror(errNo)`,
   `renderError(GetLastError())`) into a free helper and let
   `SysError`/`WinError` call it from a single non-tagged ctor.

None of these touch `MakeError`, `CloneableError`, or any other
hand-written derivation. 18c is genuinely orthogonal — but it's also
**one local pattern in three classes**. It does not warrant a top-level
catalog candidate; it's a paragraph in `SysError`'s validation section.

## Decision

**Recommendation: keep #18 as the umbrella with three sub-axes (status
quo) — but rewrite the body to reflect what each axis is really about,
because the current sub-axis framing is partially mistaken.**

Specifically:

- **18a is not a real refactor target.** The catalog body's framing
  ("the macro could be subsumed by a CRTP base") is wrong: the macro
  *already is* a CRTP sugar. There is no 18a-shaped edit that's
  independent of 18b. Re-frame 18a as "the macro is fine; verify that
  no `MakeError` parent has constructor surface that callers depend
  on" (one site: `SQLiteBusy`/`SQLiteError`) — a one-line audit, not a
  candidate.
- **18b is a real but per-class consolidation candidate.** It is
  independent of 18a/18c. Effort is small-medium per class, with most
  of the ~22 cases warranting their hand-rolled shape because the
  extra fields are intrinsic. Migration candidates are the ~5
  no-fields hand-written cases (`InvalidSSHAuthority`,
  `NotDeterministic`, `GitError`, `MissingRealisation`,
  `StackOverflowError`).
- **18c is fully orthogonal but also tiny.** Three classes, one tag
  pattern. Does not deserve top-level candidate status; it's a
  validation-paragraph note on the `SysError` family in the libutil
  shard (already verified in shard 03).

The renumber-to-18a/18b/18c proposal would require either:

- **Renumber-and-cross-reference**: change the catalog ID space to
  numeric-with-letters, breaking every existing `#18` cross-reference
  in the catalog (`grep -n "#18\b"` shows the ID is referenced from
  the consolidation log multiple times, plus `INVENTORY.md`,
  `verified/03-libutil-runtime.md`, `verified/04-libutil-misc.md`).
- **New IDs in the high-200s**: 18a → 218, 18b → 219, 18c → 220 (or
  equivalent), with #18 becoming a "see #218/#219/#220" stub. Doesn't
  break existing cross-references but adds three new top-level entries
  for what is essentially **one and a half real refactors** (18a being
  a non-refactor and 18c being a paragraph).

Both options trade catalog-structural cost (renumber or add three IDs)
for marginal information gain. The consolidator's choice — keep #18
as umbrella, spell out three sub-axes in body — is the right call,
**but** the body's three-sub-axis text needs revision because:

1. It calls 18a "trivial mechanical edit for the no-fields cases"
   when in fact the no-fields cases are per-class hand-written
   migrations (i.e. they're 18b).
2. It implies 18c is independently actionable when in fact it is one
   local pattern in three classes.
3. It misses six hand-written derivations (`EvalBaseError`,
   `StackOverflowError`, `CachedEvalError`, `InvalidPathError`,
   `BadNixStringContextElem`, `GitError`) — the catalog body says "14"
   but the verified count is ~22.

I disagree with pass-2's framing that the three sub-axes are
"genuinely independent" in a way that warrants top-level candidate
status. They are independent in the trivial sense that they don't
share files — but 18a as written doesn't describe a real edit, and
18c is too small. The split would create three IDs for one substantive
refactor (18b).

## Implied catalog edits

In `doc/inventory/candidates/03-repeated-boilerplate.md`, candidate
#18:

1. **Correct the count**: change "a handful (14 names)" to "~22
   hand-written `CloneableError` derivations". List the missing six
   (`EvalBaseError`, `StackOverflowError`, `CachedEvalError`,
   `InvalidPathError`, `BadNixStringContextElem`, `GitError`).
2. **Rewrite the 18a sub-axis**: replace "trivial mechanical edit for
   the no-fields cases" with "audit the no-fields hand-written
   derivations (`InvalidSSHAuthority`, `NotDeterministic`, `GitError`,
   `MissingRealisation`, `StackOverflowError`) and migrate to
   `MakeError` if their custom ctors can be replaced by free
   factory functions". Note this is really part of 18b, not a separate
   axis.
3. **Rewrite the 18b sub-axis**: clarify that the field-bearing
   hand-written derivations are *intrinsically* hand-written — they
   carry payload data (`SystemError::errorCode`, `BuildError::status`,
   `MissingExperimentalFeature::missingFeature`, etc.) that cannot be
   subsumed by a generic CRTP without reflection or per-field macros.
   The realistic 18b refactor is **consolidate the no-fields cases**
   plus **audit ctor surfaces** — not "subsume both styles into one
   CRTP".
4. **Demote the 18c sub-axis**: clarify that the `SysError`
   tag idiom is a three-class internal pattern, not a separate
   refactor. Mention it as a validation note on the
   `SystemError`/`SysError`/`WinError` triad, with a hint that
   deducing-this could replace the tags.
5. **Note the cross-link**: `MakeError(SQLiteBusy, SQLiteError)` is
   the only `MakeError` derivation whose parent is hand-written; any
   refactor of `SQLiteError`'s ctor surface affects `SQLiteBusy`. This
   is the single non-trivial cross-axis coupling and the catalog body
   should call it out.
6. **Adjust the effort tag**: change "small (18a) / medium (18b) /
   small (18c)" to a single "small-medium" with the per-class
   migration of the no-fields cases tagged "small" and the field
   audits tagged "trivial-per-class".

In `doc/inventory/INVENTORY.md` (line 348 area), update the
"All exceptions descend from `BaseError`" sentence's count if it ever
quotes "14" — verified inventory count is ~22 hand-written
derivations.

In `doc/inventory/review/05-consolidation-log.md`, the existing entry
"Defer; keep #18 as umbrella with three sub-axes" remains correct in
direction but should be updated to record this re-investigation
(B3) and the framing corrections above.

## Open questions

1. **Should `MakeError(SQLiteBusy, SQLiteError)` be rewritten as a
   hand-written `CloneableError`?** It's the only `MakeError` with a
   non-trivial parent, and the protected-ctor inheritance through
   `MakeError`'s `using CloneableError::CloneableError` is subtle. A
   reader might miss the dependency. Keeping it as `MakeError` is
   terser but the dependency is implicit.
2. **Is `BadNixStringContextElem`'s and `GitError`'s post-construction
   `err.msg` mutation an anti-pattern that should be replaced with a
   factory?** Both classes call `CloneableError("")` then assign
   `err.msg = HintFmt(...)` after the fact. This works because
   `BaseError::err` is `protected mutable`, but it's fragile. If
   `BaseError`'s `err` ever becomes private, both break.
3. **Should `NotDeterministic` and `InvalidSSHAuthority` be migrated
   to `MakeError` plus a free helper?** Both have no fields. The only
   cost of `MakeError` is loss of the custom-message-builder ctor —
   easily replaceable with a free `notDeterministicError(...)` /
   `invalidSSHAuthority(...)` factory function. Recommend doing this
   as part of the cleanup pass that handles #18.
4. **Is `EvalBaseError`'s `EvalState&` field intrinsic, or is it the
   "thread the state through every error" anti-pattern that #213 / N21
   touches?** The field is explicitly carried because `EvalErrorBuilder`
   needs it to hang traces and pos info on the error. Worth a
   cross-reference to whichever candidate covers the
   `EvalState`-as-context-bag pattern, if any.
5. **Should the `DisambigHintFmt`/`DisambigVarArgs` pattern be replaced
   with deducing-this overload sets?** C++23 deducing-this enables
   tag-free overload selection in some cases, but the disambiguator
   here is not a `this`-based dispatch — it's a parameter-pack
   disambiguation between "I have a `HintFmt`" and "I have format
   args to build one with". `std::tag_invoke` or named-ctor helpers
   (`SystemError::fromHintFmt(...)`, `SystemError::fromArgs(...)`)
   would replace the tags more cleanly than deducing-this.
