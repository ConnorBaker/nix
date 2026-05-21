# Inventory — Shard 13: libexpr primops (verified)

## File: src/libexpr/include/nix/expr/primops.hh

### Namespaces
- `nix` — primop registration / declarations namespace.

### Classes / structs / enums
- `RegisterPrimOp` — struct; static registry that collects `PrimOp` definitions (constructed at static-init time) into the global `primOps()` vector that `EvalState::createBaseEnv` later imports. Member typedef `PrimOps = std::vector<PrimOp>`.

### Free helper functions
- `RegisterPrimOp::primOps()` — static; returns reference to the singleton `PrimOps` vector backing the registry (declaration; defined in `primops.cc`).
- `RegisterPrimOp::RegisterPrimOp(PrimOp && primOp)` — ctor; appends a `PrimOp` to the registry vector (declaration; defined in `primops.cc`). Comment notes that arity 0 means "constant" and that `fun` is invoked during `EvalState` initialization, before the rest of the registry is populated and before `builtins` is sorted.
- `prim_importNative(EvalState & state, const PosIdx pos, Value ** args, Value & v)` — load a `ValueInitializer` from a DSO and return whatever it initializes (gated by `enableNativeCode`; non-static so plugins can use it).
- `prim_exec(EvalState & state, const PosIdx pos, Value ** args, Value & v)` — execute a program and parse its output (gated by `enableNativeCode`; non-static so plugins can use it).
- `makePositionThunks(EvalState & state, const PosIdx pos, Value & line, Value & column)` — install lazy thunks that look up line/column for a `PosIdx`.

### Primop functions (prim_*)
n/a (declarations only — see helper functions above).

### Primop registrations (RegisterPrimOp)
n/a (header file).

### Type aliases
- `RegisterPrimOp::PrimOps = std::vector<PrimOp>` — typedef (member typedef) for the registry container.

### Macros / globals
- `#pragma once` — include guard.

---

## File: src/libexpr/include/nix/expr/fetch-tree.hh

### Namespaces
- `nix`.

### Classes / structs / enums
None.

### Free helper functions
- `emitTreeAttrs(EvalState & state, const StorePath & storePath, const fetchers::Input & input, Value & v, bool emptyRevFallback = false, bool forceDirty = false)` — convert a libfetchers `Input` into a libexpr `Value` attribute set (used by `fetchTree` / `fetchGit` / `fetchMercurial`).

### Primop functions (prim_*)
None.

### Primop registrations (RegisterPrimOp)
None.

### Type aliases
None.

### Macros / globals
- `#pragma once` (no `///@file` guard; header includes `nix/expr/eval.hh`).

---

## File: src/libexpr/primops/context.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- `ContextInfo` (local struct inside `prim_getContext`) — fields `bool path = false`, `bool allOutputs = false`, `Strings outputs`; intermediate representation used while marshalling `NixStringContext` into a Nix value.

### Free helper functions
None outside the primops below.

### Primop functions (prim_*)
- `prim_unsafeDiscardStringContext` — coerce arg to a string and return a copy with empty string context.
- `prim_hasContext` — return `true` iff the string arg has a non-empty string context.
- `prim_unsafeDiscardOutputDependency` — turn every "derivation deep" context element into a plain "constant" element (inverse of `addDrvOutputDependencies`).
- `prim_addDrvOutputDependencies` — turn the single context element of a string from "constant" (Opaque) into a "derivation deep" element (inverse of `unsafeDiscardOutputDependency`); rejects multi-element contexts and "Built" outputs; idempotent on existing DrvDeep.
- `prim_getContext` — return the string context as a structured attribute set keyed by store path with `{ path?, allOutputs?, outputs }` per entry. Resolves `Built` derived paths through `resolveDerivedPath` and accumulates output names.
- `prim_appendContext` — append the structured attribute-set form of a string context onto a base string; validates each top-level key is a store path, ensures the path (calls `state.store->ensurePath` unless `readOnlyMode`), then re-emits Opaque/DrvDeep/Built elements.

### Primop registrations (RegisterPrimOp)
- `primop_unsafeDiscardStringContext` — `__unsafeDiscardStringContext`, args `{"s"}`.
- `primop_hasContext` — `__hasContext`, args `{"s"}`.
- `primop_unsafeDiscardOutputDependency` — `__unsafeDiscardOutputDependency`, args `{"s"}`.
- `primop_addDrvOutputDependencies` — `__addDrvOutputDependencies`, args `{"s"}`.
- `primop_getContext` — `__getContext`, args `{"s"}`.
- `primop_appendContext` — `__appendContext`, `arity = 2` (no docstring; no `args` list).

### Type aliases
None.

### Macros / globals
None.

---

## File: src/libexpr/primops/fetchClosure.cc

### Namespaces
- `nix`.

### Classes / structs / enums
None (uses local types only).

### Free helper functions
- `runFetchClosureWithRewrite(EvalState &, const PosIdx, Store & fromStore, const StorePath & fromPath, const std::optional<StorePath> & toPathMaybe, Value & v)` — content-address-rewrite handler: optionally rewrites `fromPath` to a CA path matching `toPathMaybe`; if `toPathMaybe` is absent it computes the rewritten path and reports it in the error message; if `toPath` already exists it asserts it is content-addressed.
- `runFetchClosureWithContentAddressedPath(EvalState &, const PosIdx, Store & fromStore, const StorePath & fromPath, Value & v)` — copy-closure path then assert it is content-addressed.
- `runFetchClosureWithInputAddressedPath(EvalState &, const PosIdx, Store & fromStore, const StorePath & fromPath, Value & v)` — copy-closure path then assert it is input-addressed (when `inputAddressed = true`).

### Primop functions (prim_*)
- `prim_fetchClosure` — fetch a store-path closure from a binary cache; parses `{ fromPath, fromStore, toPath?, inputAddressed? }`, validates store URL is `http`/`https` (or `file` under `_NIX_IN_TEST`), rejects URL params, and dispatches between rewrite, content-addressed, or input-addressed mode based on `toPath` / `inputAddressed` attrs.

### Primop registrations (RegisterPrimOp)
- `primop_fetchClosure` — `__fetchClosure`, args `{"args"}`, `experimentalFeature = Xp::FetchClosure`.

### Type aliases
- `StorePathOrGap = std::optional<StorePath>` — typedef used to distinguish "absent toPath" (outer optional empty) from "explicit empty-string toPath" (`StorePathOrGap{}`, i.e. inner optional empty).

### Macros / globals
None.

---

## File: src/libexpr/primops/fetchMercurial.cc

### Namespaces
- `nix`.

### Classes / structs / enums
None.

### Free helper functions
None outside the primop.

### Primop functions (prim_*)
- `prim_fetchMercurial` — fetch a Mercurial repo via `fetchers::Input` (type `hg`); accepts a URL string or attrset with `url`/`rev`/`ref`/`name` (where `rev` may be either a SHA1 revision or a branch/tag name); URL is prefixed `file://` if it has no scheme; calls `state.checkURI`; in pure-eval mode requires a revision; returns attrset with `outPath`, optional `branch`, `rev`, `shortRev`, optional `revCount`; calls `state.allowPath(storePath)`.

### Primop registrations (RegisterPrimOp)
- `r_fetchMercurial` — `fetchMercurial` (no `__` prefix; `arity = 1`; no docstring).

### Type aliases
None.

### Macros / globals
None.

---

## File: src/libexpr/primops/fetchTree.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- `LazyFetcherAttr : public ExternalValueBase, public gc_cleanup` — wraps a `fetchers::LazyAttr` so it can be embedded as an `nExternal` `Value`; only ever produced internally and forced via `prim_forceLazyFetcherAttr`. Member: `fetchers::LazyAttr lazy`. Public `force()` returns `fetchers::ResolvedAttr` (calls `lazy->compute()`). Overrides `print`, `showType`, `typeOf` (all `unreachable()` — never user-visible).
- `FetchTreeParams` — struct of bool flags `emptyRevFallback = false`, `allowNameArgument = false`, `isFetchGit = false`, `isFinal = false`; configures the shared `fetchTree` helper for the various entry-points.

### Free helper functions
- `resolvedAttrToValue(EvalState &, Value &, const fetchers::ResolvedAttr &)` — store a `ResolvedAttr` (string / uint64 / `Explicit<bool>` variant) into a `Value`.
- `emitLazyAttrThunk(EvalState &, const fetchers::LazyAttr & lazyAttr, Value & dest)` — create an `App(forceLazyFetcherAttr, externalValue)` thunk for lazy fetcher attributes (e.g. lazy `revCount`); allocates a `LazyFetcherAttr` external Value and a primop Value.
- `emitTreeAttrs(EvalState & state, const StorePath & storePath, const fetchers::Input & input, Value & v, bool emptyRevFallback, bool forceDirty)` — main result-attrset constructor for fetchTree-family primops; populates `outPath`, `narHash`, `submodules` (only for `git` inputs), `rev`/`shortRev` (with `emptyRevFallback` returning a zero SHA1 for `fetchGit` dirty trees), `revCount` (lazy via `emitLazyAttrThunk` if `maybeGetLazyAttr` finds one, else direct), `dirtyRev`/`dirtyShortRev`, `lastModified`/`lastModifiedDate` (via `std::put_time` `%Y%m%d%H%M%S`). Definition; declared in `fetch-tree.hh`.
- `fetchTree(EvalState &, const PosIdx, Value ** args, Value & v, const FetchTreeParams & params = FetchTreeParams{})` — shared core for `fetchTree`/`fetchGit`/`fetchFinalTree`: parses URL or attrset input, applies registry lookup (`lookupInRegistries` with `UseRegistries::Limited` only when `Xp::Flakes` is enabled and not pure-eval), enforces pure-eval locking, sets `__final` if `isFinal` / forbids `__final` otherwise, mounts cached input via `state.inputCache->getAccessor` + `state.mountInput`, then calls `emitTreeAttrs`. Special-cases: applies `fixGitURL` to the `url` attr when `isFetchGit`; defaults `exportIgnore = true` for non-submodules `fetchGit`; defaults `shallow = true` for non-`fetchGit` git-type fetchTree calls; rejects `name` unless `allowNameArgument`; allows int/string/bool/path attrs (and `publicKeys` JSON under `Xp::VerifiedFetches`).
- `fetch(EvalState &, const PosIdx, Value **, Value &, const std::string & who, bool unpack, std::string name)` — shared core for `__fetchurl`/`fetchTarball`: validates URL+sha256+name, applies `state.settings.resolvePseudoUrl` for `fetchTarball`, defaults `name` to `baseNameOf(url)`, runs `checkName` (catching `BadStorePathName` for a friendlier diagnostic), in pure-eval requires `sha256`, optionally short-circuits to a substituted store path via `makeFixedOutputPath` + `ensurePath`, otherwise downloads (file via `fetchers::downloadFile`, unpacked tarball via `fetchers::downloadTarball` + `fetchToStore`) and verifies hash (sets `withExitStatus(102)` on mismatch).

### Primop functions (prim_*)
- `prim_forceLazyFetcherAttr` — internal-only; `dynamic_cast`s `args[0]->external()` to `LazyFetcherAttr*`, calls `force()`, emits via `resolvedAttrToValue`.
- `prim_fetchTree` — wrapper around `fetchTree` with default params.
- `prim_fetchFinalTree` — wrapper around `fetchTree` with `isFinal = true`; non-static (declared in `src/libflake/include/nix/flake/flake.hh` for use by libflake).
- `prim_fetchurl` — wrapper around `fetch` with `who = "fetchurl"`, `unpack = false`, `name = ""`.
- `prim_fetchTarball` — wrapper around `fetch` with `who = "fetchTarball"`, `unpack = true`, `name = "source"`.
- `prim_fetchGit` — wrapper around `fetchTree` with `emptyRevFallback = true`, `allowNameArgument = true`, `isFetchGit = true`.

### Primop registrations (RegisterPrimOp)
- `primop_fetchTree` — `fetchTree`, args `{"input"}`, `experimentalFeature = Xp::FetchTree`; doc body is built dynamically from `fetchers::getAllInputSchemes()` (a lambda returning `std::string`).
- `primop_fetchFinalTree` — `fetchFinalTree`, args `{"input"}`, `internal = true` (no docstring).
- `primop_fetchurl` — `__fetchurl`, args `{"arg"}`.
- `primop_fetchTarball` — `fetchTarball`, args `{"args"}`.
- `primop_fetchGit` — `fetchGit`, args `{"args"}`.

Internal/anonymous primops (file-local, not registered in `RegisterPrimOp::primOps()`):
- `forcePrimOp` (function-local `static PrimOp` inside `emitLazyAttrThunk`) — `__forceLazyFetcherAttr`, `arity = 1`, `impl = prim_forceLazyFetcherAttr`, `internal = true`.

### Type aliases
None.

### Macros / globals
None.

---

## File: src/libexpr/primops/fromTOML.cc

### Namespaces
- `nix`.

### Classes / structs / enums
None.

### Free helper functions
- `normalizeSubsecondPrecision(toml::local_time lt)` — returns `0`, `3`, `6`, or `9` to match toml11<4.0 sub-second serialization, based on which of `millisecond`/`microsecond`/`nanosecond` are non-zero. (Compiled only when `HAVE_TOML11_4`.)
- `normalizeDatetimeFormat(toml::value & t)` — set `delimiter = upper_T`, `has_seconds = true`, and computed `subsecond_precision` on the `as_local_datetime_fmt` / `as_offset_datetime_fmt` / `as_local_time_fmt` formatter for local/offset datetime / local time values, to match older toml11 output. (Compiled only when `HAVE_TOML11_4`.)

### Primop functions (prim_*)
- `prim_fromTOML` — parse a TOML string into a Nix value via a recursive `auto self` lambda (deducing-this); handles tables, arrays, booleans, integers (`int64_t`), floats (`NixFloat`), strings (rejects null bytes via `forceNoNullByte`), datetime/date/time variants (gated by `Xp::ParseTomlTimestamps`, emitting `{ _type = "timestamp"; value = "<formatted>"; }`), and empty toml values (`mkNull`). Catches `std::exception` to throw `EvalError` with a `while parsing TOML: %s` message.

### Primop registrations (RegisterPrimOp)
- `primop_fromTOML` — `fromTOML`, args `{"e"}`.

### Type aliases
None.

### Macros / globals
- `#if HAVE_TOML11_4 / #endif` — conditional compilation around the toml11 4.x normalization helpers, and around the second `toml::parse` argument (`toml::spec::v(1, 0, 0)`).
- `HAVE_TOML11_4` is defined in the auto-generated `expr-config-private.hh`.
- Includes `<sstream>` and `<toml.hpp>`.

---

## File: src/libexpr/primops.cc

### Namespaces
- `nix` — encloses everything in this file.

### Classes / structs / enums
- `CompareValues` — functor used to compare two `Value *` instances for ordering (used by `prim_genericClosure`'s `keyToElem` map and by `prim_sort` when the comparator is `__lessThan`); throws `EvalError` for incomparable types. Holds `EvalState & state`, `const PosIdx pos`, `const std::string_view errorCtx`. Two `operator()` overloads (one inheriting `errorCtx`, one taking explicit `errorCtx`). Compares ints/floats with cross-type promotion, strings, paths (via `pathStrView`), and lists lexicographically (recursive). Wraps switch in `#pragma GCC diagnostic ignored "-Wswitch-enum"`.
- `LazyPosAccessors` (anonymous struct, with file-scope singleton instance `makeLazyPosAccessors`) — owns two `PrimOp` instances (`primop_lineOfPos`, `primop_columnOfPos`) and two `Value` thunks (`lineOfPos`, `columnOfPos`); ctor wires the `Value`s to the `PrimOp`s; `operator()` builds an `App(primop, posInt)` thunk for each of `line` and `column`.
- `RegexCache` — concurrency-safe regex cache used by `prim_match` / `prim_split`.
  - `RegexCache::Entry` — wraps `ref<const std::regex>`; ctor compiles a regex from `(s, count)` with `std::regex::extended`.
  - `RegexCache::cache` — `boost::concurrent_flat_map<std::string, Entry, StringViewHash, std::equal_to<>>`.
  - `RegexCache::get(std::string_view re)` — fetch-or-insert via `try_emplace_and_cvisit`.
- `Item` (local struct inside `prim_zipAttrsWith`) — fields `size_t size = 0`, `size_t pos = 0`, `std::optional<ListBuilder> list`; pre-sizes per-key result lists.
- `Constants` (local struct inside `fileTypeToString`) — `Value regular`, `directory`, `symlink`, `unknown`; populated once via lambda IIFE for Meyers-singleton return values.

### Free helper functions
- `mkString(EvalState &, const std::csub_match &)` (`static inline`) — allocate a `Value *` from a regex submatch (used by `prim_match`/`prim_split`).
- `EvalState::realiseString(Value & s, StorePathSet * storePathsOutMaybe, bool isIFD, const PosIdx pos)` — coerce-to-string + realise its context (calls `coerceToString`, then `realiseContext`, then `ensureLazyPathsCopied`, then `rewriteStrings`); member function defined here.
- `EvalState::realiseContext(const NixStringContext & context, StorePathSet * maybePathsOut, bool isIFD)` — build/substitute every derived path in a context, returning placeholder→storepath rewrites; honours `Xp::CaDerivations` for `DownstreamPlaceholder` mapping; respects `enableImportFromDerivation` and `traceImportFromDerivation` settings; copies closures across `buildStore`/`store`; allows closures under IFD.
- `EvalState::realisePath(const PosIdx pos, Value & v, std::optional<SymlinkResolution> resolveSymlinks, CopyLazyPaths copyLazyPaths)` — coerce a value to a `SourcePath`, realise its context if non-empty and accessor is `rootFS`, optionally resolve symlinks; rewraps `Error` with "while realising the context of path" trace.
- `mkOutputString(EvalState &, BindingsBuilder & attrs, const StorePath & drvPath, const std::pair<std::string, DerivationOutput> & o)` (`static`) — populate one output attribute on a derivation result attrset (delegates to `state.mkOutputString` with a `Built` derived-path).
- `derivationToValue(EvalState &, const PosIdx, const SourcePath & path, const StorePath & storePath, Value & v)` — handle `import` of a `.drv` by reading the derivation, building an attrset (`drvPath`/`name`/`outputs`), and applying `imported-drv-to-derivation.nix` (loaded via `state.evalFile`); not declared `static`.
- `scopedImport(EvalState &, const PosIdx, SourcePath & path, Value * vScope, Value & v)` (`static`) — implement `builtins.scopedImport` (parses with a custom static env populated from `vScope`); allocates `Env` and `StaticEnv`, parses with `parseExprFromFile(resolveExprPath(...))`, then evaluates.
- `import(EvalState &, const PosIdx, Value & vPath, Value * vScope, Value & v)` (`static`) — common implementation of `import` and `scopedImport`; dispatches to `derivationToValue` if path is a valid `.drv` in the store, else `scopedImport` if `vScope` is non-null, else `evalFile`.
- `withExceptionContext(Trace, const Callable &)` (template, `static inline`) — wraps a callable and pushes an extra trace on `Error` (calls `e.pushTrace(trace)`).
- `prim_lessThan` — forward declaration (just before `prim_sort`) so `prim_sort` can short-circuit the lessThan-comparator case.
- `anyOrAll(bool any, EvalState &, const PosIdx, Value **, Value &)` (`static`) — shared core for `prim_any` and `prim_all`; iterates the list and short-circuits on the first match.
- `derivationStrictInternal(EvalState &, std::string_view drvName, const Bindings * attrs, Value & v)` (`static`) — core of `prim_derivationStrict`; populates a `Derivation`, processes attributes in lexicographic order (handling `__structuredAttrs`/`__ignoreNulls`/`args`/`builder`/`system`/`outputHash`/`outputHashAlgo`/`outputHashMode`/`outputs`/`__contentAddressed`/`__impure`/`__json`), resolves `NixStringContext` into `inputDrvs`/`inputSrcs`, validates `builder` and `system` are non-empty, classifies into fixed-output/CA/impure/deferred, writes the `.drv` (or computes path under `readOnlyMode`), caches `hashDerivationModulo` in `drvHashes`, builds a result `{ drvPath, ...outputs }` attrset.
- `checkDerivationName(EvalState &, std::string_view drvName)` (`static`) — early validation of a derivation's `name` attribute (catches `BadStorePathName` to produce a friendlier "Please pass a different 'name'" error).
- `legacyBaseNameOf(std::string_view path)` (`static`) — backwards-compat baseNameOf that strips at most one trailing `/`; documented in-source as deliberately preserved for reproducibility.
- `fileTypeToString(EvalState &, SourceAccessor::Type)` (`static`, returns `const Value &`) — return a static `Value &` for `"regular"` / `"directory"` / `"symlink"` / `"unknown"` (Meyers-singleton-style cache; wraps switch in `#pragma GCC diagnostic ignored "-Wswitch-enum"`).
- `EvalState::callPathFilter(Value * filterFun, const SourcePath & path, PosIdx pos)` — invoke the user's path-filter callback with `(pathString, fileTypeString)` and force the resulting Bool; member function defined here. Asserts type is not "unknown".
- `addPath(EvalState &, const PosIdx, std::string_view name, SourcePath path, Value * filterFun, ContentAddressMethod, const std::optional<Hash> expectedHash, Value & v, const NixStringContext & context)` (`static`) — common implementation of `__filterSource` and `__path`; rewrites context paths if path is in store, wraps the user filter in a `PathFilter`, optionally short-circuits to `expectedStorePath` if substituted, otherwise calls `fetchToStore` (no refs path) or `state.store->addToStore` (with refs); calls `allowAndSetStorePathString`.
- `makeRegexCache()` — return a fresh `ref<RegexCache>` (called by `EvalState` ctor).
- `makePositionThunks(EvalState & state, const PosIdx pos, Value & line, Value & column)` — defined here; calls `makeLazyPosAccessors(state, pos, line, column)`. (Declared in `primops.hh`.)

### Primop functions (prim_*) — exhaustive (90 total)
1. `prim_importNative` — load a DSO via `dlopen`, resolve a `ValueInitializer` symbol via `dlsym`, run it (deliberately does not `dlclose`); declared in `primops.hh`; non-static; gated by `enableNativeCode`. Wrapped in `#ifndef _WIN32`.
2. `prim_exec` — execute an external program (built from `args[0]` list, with realised string context), call `runProgram(..., true, ...)`, parse stdout via `parseExprFromString`, then `eval`; declared in `primops.hh`; non-static; gated by `enableNativeCode`. Wrapped in `#ifndef _WIN32`.
3. `prim_typeOf` — return `"int"`/`"bool"`/`"string"`/`"path"`/`"null"`/`"set"`/`"list"`/`"lambda"`/`"float"` (or `external->typeOf()`); uses `mkStringNoCopy` with `"..."_sds` constants; `unreachable()` on `nThunk`/`nFailed`.
4. `prim_isNull` — true iff arg type is `nNull`.
5. `prim_isFunction` — true iff arg type is `nFunction`.
6. `prim_isInt` — true iff arg type is `nInt`.
7. `prim_isFloat` — true iff arg type is `nFloat`.
8. `prim_isString` — true iff arg type is `nString`.
9. `prim_isBool` — true iff arg type is `nBool`.
10. `prim_isPath` — true iff arg type is `nPath`.
11. `prim_genericClosure` — iterative transitive closure: `{ startSet, operator }` → list of attrsets keyed by `key`; uses `CompareValues` on `key` and traces include "while comparing element"/"with element" diagnostics; calls `state.callFunction(*op->value, ...)` to expand.
12. `prim_addErrorContext` — evaluate and return `args[1]`, attaching `args[0]` (coerced to string) as a trace whenever an error escapes; uses `TracePrint::Always`.
13. `prim_ceil` — `ceil` for `NixFloat` / `NixInt` with overflow / precision-loss diagnostics (returns `NixInt` if range fits; references `https://github.com/NixOS/nix/issues/12899`).
14. `prim_floor` — same as `prim_ceil` but using `floor(value)`.
15. `prim_tryEval` — return `{ success, value }` after attempting to force `args[0]`; only catches `AssertionError` (so `throw`/`assert` are caught). Increments `state.trylevel` (via `MaintainCount`) and temporarily nullifies `state.debugRepl` if `ignoreExceptionsDuringTry`.
16. `prim_getEnv` — return env var value; empty string if `restrictEval` or `pureEval`.
17. `prim_seq` — strict sequencing: force `args[0]` shallowly then return `args[1]` (also forces `args[1]`).
18. `prim_deepSeq` — like `seq` but `forceValueDeep` on `args[0]`.
19. `prim_trace` — print arg to stderr (string verbatim or pretty-printed value via `ValuePrinter`) and return `args[1]`; runs debug REPL if `builtinsTraceDebugger`.
20. `prim_warn` — emit a warning level log of a string arg (rejects non-string), optional debugger entry / abort-on-warn, then return `args[1]`.
21. `prim_second` — return `args[1]` unchanged (used as `__traceVerbose` impl when `--trace-verbose` is off).
22. `prim_derivationStrict` — public entrypoint to derivation construction; forces attrs, validates name, calls `derivationStrictInternal`; rewraps errors with "while evaluating derivation '%s'" frame trace.
23. `prim_placeholder` — return `hashPlaceholder(name)` for a derivation output name.
24. `prim_toPath` — coerce arg to a path, return as a string (deprecated; obsolete).
25. `prim_storePath` — promote a path that is already inside the store to a value with proper string context (forbidden in pure-eval). Resolves symlinks unless the path is itself a symlink directly in the store; ensures path; emits `Opaque` context.
26. `prim_pathExists` — true iff path arg exists; honors trailing-slash-must-be-dir (via `tDirectory` check); catches `RestrictedPathError` returning false.
27. `prim_baseNameOf` — return `legacyBaseNameOf` of the coerced string form; preserves context.
28. `prim_dirOf` — return everything before the last `/` of arg (preserves path/string type); special-cased for "no slash" → `"."` and "leading slash only" → `"/"`.
29. `prim_readFile` — read file contents into a string, scanning for store-path references via `PathRefScanSink::fromPaths(refs)` + `<<` to populate context (only if path is in-store); rejects null bytes.
30. `prim_findFile` — implement search-path lookup: `findFile [{prefix, path}, ...] lookup-path`. Realises context for each `path` entry.
31. `prim_hashFile` — base-16 hash of a file's contents using the named algorithm; uses `parseHashAlgo` and `path.readFile()` then `hashString`.
32. `prim_readFileType` — return the directory-entry type as a string via `fileTypeToString(state, path.lstat().type)`.
33. `prim_readDir` — return `{ entry = "regular"/"directory"/"symlink"/"unknown"; ... }`; uses lazy `__readFileType` thunks (via `state.getBuiltin("readFileType")`) for entries with unknown type.
34. `prim_outputOf` — chain a derived-path output reference (input-placeholder if unresolved); experimental `Xp::DynamicDerivations`. Calls `state.mkSingleDerivedPathString(SingleDerivedPath::Built{...}, v)`.
35. `prim_toXML` — serialize a value to XML via `printValueAsXML`.
36. `prim_toJSON` — serialize a value to JSON (with string context) via `printValueAsJSON`.
37. `prim_fromJSON` — parse a JSON string to a value (delegates to `parseJSON`); rewraps `JSONParseError` with "while decoding a JSON string" trace.
38. `prim_toFile` — write a string to a fixed-output content-addressed text store path (via `addToStoreFromDump` or `makeFixedOutputPathFromCA` under `readOnlyMode`); rejects derivation references in context (only allows `Opaque`).
39. `prim_filterSource` — copy a path into the store with a user-provided file-filter predicate; uses `ContentAddressMethod::Raw::NixArchive`, no expected hash.
40. `prim_path` — like `filterSource` but accepts an attrset `{ path, name?, filter?, recursive?, sha256? }`; `recursive` toggles between `NixArchive` and `Flat` ingestion.
41. `prim_attrNames` — sorted list of attribute names; uses `Value::toPtr(symbol)` static-string optimization.
42. `prim_attrValues` — values of attributes in attrName-sorted order; sorts pointers-to-Attr in-place then unpacks values.
43. `prim_getAttr` — dynamic `set.${name}` (non-static; friend-declared in `eval.hh`); increments `state.attrSelects[i->pos]` if `state.countCalls`.
44. `prim_unsafeGetAttrPos` — return position info for an attribute, or `null`; emits via `state.mkPos`.
45. `prim_hasAttr` — dynamic `set ? name`.
46. `prim_isAttrs` — true iff arg type is `nAttrs`.
47. `prim_removeAttrs` — remove a list of attribute names from a set (uses `boost::container::small_vector<Attr, 64>` + `std::set_difference`); marks output already-sorted.
48. `prim_listToAttrs` — build an attrset from a list of `{ name, value }` pairs; first occurrence wins; uses an in-place sort over `Bindings` allocation that (ab)uses `Attr::value` to stash a `Value **` into the original list, then unpacks values in a second pass.
49. `prim_intersectAttrs` — intersect by name keeping values from the right attrset; iterates the smaller side and `get`s on the larger; marks output already-sorted. Has a long inline comment discussing alternative algorithms.
50. `prim_catAttrs` — pluck a named attr from each set in a list, dropping missing; uses `SmallValueVector<nonRecursiveStackReservation>` for stack-allocated buffer.
51. `prim_functionArgs` — names→hasDefault attrset of a lambda's formal parameters; empty for primops/primOpApps and lambdas without formals.
52. `prim_mapAttrs` — apply `f name value` to every attribute (lazy in result values: produces `App(App(f, name), value)` thunks); marks output already-sorted.
53. `prim_zipAttrsWith` — transpose a list of attrsets and apply `f name list` to each key; uses `std::map<Symbol, Item, ...>` with `traceable_allocator` to count, allocate, populate, then build `Apply` thunks.
54. `prim_isList` — true iff arg type is `nList`.
55. `prim_elemAt` — `xs[n]`, with bounds check.
56. `prim_head` — first element of a list (errors on empty list).
57. `prim_tail` — list without first element (linear O(n) copy; errors on empty list).
58. `prim_map` — lazy `map f xs` (allocates an Apply thunk per element); short-circuits empty list.
59. `prim_filter` — eager filter; reuses input list when nothing was filtered out (`same` flag).
60. `prim_elem` — true iff `xs` contains an element equal to `x`; uses `state.eqValues`.
61. `prim_concatLists` — concatenate a list of lists (delegates to `state.concatLists`).
62. `prim_length` — list length.
63. `prim_foldlStrict` — strict left fold; allocates a fresh accumulator `Value *` for each step except the last (which writes into `v`).
64. `prim_any` — any-by-predicate (delegates to `anyOrAll(true, ...)`).
65. `prim_all` — all-by-predicate (delegates to `anyOrAll(false, ...)`).
66. `prim_genList` — `genList f n`; each element is a lazy `f i` thunk; rejects negative or oversized `n`.
67. `prim_sort` — peeksort with a user comparator; bypasses callFunction when comparator is `__lessThan` by inspecting `args[0]->primOp()->impl.get_fn().target<decltype(&prim_lessThan)>()`.
68. `prim_partition` — split list into `{ right = ... ; wrong = ... ; }` by predicate.
69. `prim_groupBy` — group list elements by stringly-typed `f`-result; returns attrset of lists; uses `ValueVectorMap`.
70. `prim_concatMap` — `concatLists (map f xs)`, fused; uses `SmallTemporaryValueVector<conservativeStackReservation>` for the per-element returned lists; bulk-`memcpy`s into the final list.
71. `prim_add` — int+int / float-promotes; checks integer overflow via `valueChecked()`.
72. `prim_sub` — int-int / float-promotes; checks integer overflow via `valueChecked()`.
73. `prim_mul` — int*int / float-promotes; checks integer overflow via `valueChecked()`.
74. `prim_div` — int/int with division-by-zero (forced by also checking `f2 == 0`) + overflow check; or float/float.
75. `prim_bitAnd` — bitwise AND of two ints.
76. `prim_bitOr` — bitwise OR of two ints.
77. `prim_bitXor` — bitwise XOR of two ints.
78. `prim_lessThan` — `<` over ints / floats / strings / paths / lists (lex); delegates to `CompareValues`.
79. `prim_toString` — coerce value to a string preserving context; uses `coerceToString(..., true, false)`.
80. `prim_substring` — `substring start len s`; rejects negative `start`; `len < 0` means "to end of string"; special-cased for `len == 0` to preserve context cheaply (returns `mkStringNoCopy(""_sds, ...)` with the input's context).
81. `prim_stringLength` — byte length of a string-coerced value.
82. `prim_hashString` — base-16 hash of a string's content using the named algorithm; uses `parseHashAlgo` and `hashString`.
83. `prim_convertHash` — re-encode a hash given `{ hash, hashAlgo?, toHashFormat }`; SRI is double-encoded `to_string(hf, hf == HashFormat::SRI)`.
84. `prim_match` — POSIX-extended-regex full match: returns list of group captures (or `null` for unmatched groups, `null` itself for no overall match) (non-static; friend-declared in `eval.hh`); uses `state.regexCache->get(re)` and `std::regex_match`. Catches `std::regex_error` distinguishing `error_space`.
85. `prim_split` — POSIX-extended-regex split: list of non-matching strings interleaved with per-match group lists (non-static; friend-declared in `eval.hh`); uses `std::cregex_iterator`. Catches `std::regex_error` like `prim_match`.
86. `prim_concatStringsSep` — join a list with a separator; reserves `(N+32)*sep.size()` bytes upfront.
87. `prim_replaceStrings` — replace each `from[i]` with `to[i]` in `s`; lazy in `to` (cached in `boost::unordered_flat_map<size_t, std::string_view>`); explicitly handles empty `from` strings (advances by one).
88. `prim_parseDrvName` — split derivation name into `{ name, version }` via the `DrvName` class.
89. `prim_compareVersions` — return `-1/0/1` from `compareVersions`.
90. `prim_splitVersion` — list of version components via `nextComponent`.

Internal/anonymous primops (file-local, not registered in `RegisterPrimOp::primOps()`):
- `LazyPosAccessors::primop_lineOfPos` — arity-1 inline lambda primop returning `state.positions[PosIdx(args[0]->integer().value)].line` as int.
- `LazyPosAccessors::primop_columnOfPos` — arity-1 inline lambda primop returning the column number.

### Primop registrations (RegisterPrimOp)
Top-level `RegisterPrimOp` static instances (each registers a `PrimOp` into `RegisterPrimOp::primOps()`), in source order:

- `primop_scopedImport` — `scopedImport`, args `{"scope", "path"}`; lambda dispatches into `import(state, pos, *args[1], args[0], v)`.
- `primop_import` — `import`, args `{"path"}`; lambda dispatches into `import(state, pos, *args[0], nullptr, v)`.
- `primop_typeOf` — `__typeOf`, args `{"e"}` → `prim_typeOf`.
- `primop_isNull` — `isNull`, args `{"e"}` → `prim_isNull`.
- `primop_isFunction` — `__isFunction`, args `{"e"}` → `prim_isFunction`.
- `primop_isInt` — `__isInt`, args `{"e"}` → `prim_isInt`.
- `primop_isFloat` — `__isFloat`, args `{"e"}` → `prim_isFloat`.
- `primop_isString` — `__isString`, args `{"e"}` → `prim_isString`.
- `primop_isBool` — `__isBool`, args `{"e"}` → `prim_isBool`.
- `primop_isPath` — `__isPath`, args `{"e"}` → `prim_isPath`.
- `primop_genericClosure` — `__genericClosure`, args `{"attrset"}`, `arity = 1` → `prim_genericClosure`.
- `primop_break` — `break`, args `{"v"}`; inline lambda enters the debug REPL when `state.canDebug()`, otherwise returns the argument.
- `primop_abort` — `abort`, args `{"s"}`; inline lambda raises an `Abort` error with `setIsFromExpr()`.
- `primop_throw` — `throw`, args `{"s"}`; inline lambda raises a `ThrownError` with `setIsFromExpr()`.
- `primop_addErrorContext` — `__addErrorContext`, args `{"context", "value"}`, `arity = 2`, `addTrace = false` → `prim_addErrorContext`.
- `primop_ceil` — `__ceil`, args `{"number"}` → `prim_ceil`.
- `primop_floor` — `__floor`, args `{"number"}` → `prim_floor`.
- `primop_tryEval` — `__tryEval`, args `{"e"}` → `prim_tryEval`.
- `primop_getEnv` — `__getEnv`, args `{"s"}` → `prim_getEnv`.
- `primop_seq` — `__seq`, args `{"e1", "e2"}` → `prim_seq`.
- `primop_deepSeq` — `__deepSeq`, args `{"e1", "e2"}` → `prim_deepSeq`.
- `primop_trace` — `__trace`, args `{"e1", "e2"}` → `prim_trace`.
- `primop_warn` — `__warn`, args `{"e1", "e2"}` → `prim_warn`.
- `primop_derivationStrict` — `derivationStrict`, `arity = 1` → `prim_derivationStrict` (no args list, no docstring).
- `primop_placeholder` — `placeholder`, args `{"output"}` → `prim_placeholder`.
- `primop_toPath` — `__toPath`, args `{"s"}` → `prim_toPath`.
- `primop_storePath` — `__storePath`, args `{"path"}` → `prim_storePath`.
- `primop_pathExists` — `__pathExists`, args `{"path"}` → `prim_pathExists`.
- `primop_baseNameOf` — `baseNameOf`, args `{"x"}` → `prim_baseNameOf`.
- `primop_dirOf` — `dirOf`, args `{"s"}` → `prim_dirOf`.
- `primop_readFile` — `__readFile`, args `{"path"}` → `prim_readFile`.
- `primop_findFile` — `__findFile`, args `{"search-path", "lookup-path"}` → `prim_findFile`.
- `primop_hashFile` — `__hashFile`, args `{"type", "p"}` → `prim_hashFile`.
- `primop_readFileType` — `__readFileType`, args `{"p"}` → `prim_readFileType`.
- `primop_readDir` — `__readDir`, args `{"path"}` → `prim_readDir`.
- `primop_outputOf` — `__outputOf`, args `{"derivation-reference", "output-name"}`, `experimentalFeature = Xp::DynamicDerivations` → `prim_outputOf`.
- `primop_toXML` — `__toXML`, args `{"e"}` → `prim_toXML`.
- `primop_toJSON` — `__toJSON`, args `{"e"}` → `prim_toJSON`.
- `primop_fromJSON` — `__fromJSON`, args `{"e"}` → `prim_fromJSON`.
- `primop_toFile` — `__toFile`, args `{"name", "s"}` → `prim_toFile`.
- `primop_filterSource` — `__filterSource`, args `{"e1", "e2"}` → `prim_filterSource`.
- `primop_path` — `__path`, args `{"args"}` → `prim_path`.
- `primop_attrNames` — `__attrNames`, args `{"set"}` → `prim_attrNames`.
- `primop_attrValues` — `__attrValues`, args `{"set"}` → `prim_attrValues`.
- `primop_getAttr` — `__getAttr`, args `{"s", "set"}` → `prim_getAttr`.
- `primop_unsafeGetAttrPos` — `__unsafeGetAttrPos`, args `{"s", "set"}`, `arity = 2` → `prim_unsafeGetAttrPos`.
- `primop_hasAttr` — `__hasAttr`, args `{"s", "set"}` → `prim_hasAttr`.
- `primop_isAttrs` — `__isAttrs`, args `{"e"}` → `prim_isAttrs`.
- `primop_removeAttrs` — `removeAttrs`, args `{"set", "list"}` → `prim_removeAttrs`.
- `primop_listToAttrs` — `__listToAttrs`, args `{"e"}` → `prim_listToAttrs`.
- `primop_intersectAttrs` — `__intersectAttrs`, args `{"e1", "e2"}` → `prim_intersectAttrs`.
- `primop_catAttrs` — `__catAttrs`, args `{"attr", "list"}` → `prim_catAttrs`.
- `primop_functionArgs` — `__functionArgs`, args `{"f"}` → `prim_functionArgs`.
- `primop_mapAttrs` — `__mapAttrs`, args `{"f", "attrset"}` → `prim_mapAttrs`.
- `primop_zipAttrsWith` — `__zipAttrsWith`, args `{"f", "list"}` → `prim_zipAttrsWith`.
- `primop_isList` — `__isList`, args `{"e"}` → `prim_isList`.
- `primop_elemAt` — `__elemAt`, args `{"xs", "n"}` → `prim_elemAt`.
- `primop_head` — `__head`, args `{"list"}` → `prim_head`.
- `primop_tail` — `__tail`, args `{"list"}` → `prim_tail`.
- `primop_map` — `map`, args `{"f", "list"}` → `prim_map`.
- `primop_filter` — `__filter`, args `{"f", "list"}` → `prim_filter`.
- `primop_elem` — `__elem`, args `{"x", "xs"}` → `prim_elem`.
- `primop_concatLists` — `__concatLists`, args `{"lists"}` → `prim_concatLists`.
- `primop_length` — `__length`, args `{"e"}` → `prim_length`.
- `primop_foldlStrict` — `__foldl'`, args `{"op", "nul", "list"}` → `prim_foldlStrict`.
- `primop_any` — `__any`, args `{"pred", "list"}` → `prim_any`.
- `primop_all` — `__all`, args `{"pred", "list"}` → `prim_all`.
- `primop_genList` — `__genList`, args `{"generator", "length"}` → `prim_genList`.
- `primop_sort` — `__sort`, args `{"comparator", "list"}` → `prim_sort`.
- `primop_partition` — `__partition`, args `{"pred", "list"}` → `prim_partition`.
- `primop_groupBy` — `__groupBy`, args `{"f", "list"}` → `prim_groupBy`.
- `primop_concatMap` — `__concatMap`, args `{"f", "list"}` → `prim_concatMap`.
- `primop_add` — `__add`, args `{"e1", "e2"}` → `prim_add`.
- `primop_sub` — `__sub`, args `{"e1", "e2"}` → `prim_sub`.
- `primop_mul` — `__mul`, args `{"e1", "e2"}` → `prim_mul`.
- `primop_div` — `__div`, args `{"e1", "e2"}` → `prim_div`.
- `primop_bitAnd` — `__bitAnd`, args `{"e1", "e2"}` → `prim_bitAnd`.
- `primop_bitOr` — `__bitOr`, args `{"e1", "e2"}` → `prim_bitOr`.
- `primop_bitXor` — `__bitXor`, args `{"e1", "e2"}` → `prim_bitXor`.
- `primop_lessThan` — `__lessThan`, args `{"e1", "e2"}` → `prim_lessThan`.
- `primop_toString` — `toString`, args `{"e"}` → `prim_toString`.
- `primop_substring` — `__substring`, args `{"start", "len", "s"}` → `prim_substring`.
- `primop_stringLength` — `__stringLength`, args `{"e"}` → `prim_stringLength`.
- `primop_hashString` — `__hashString`, args `{"type", "s"}` → `prim_hashString`.
- `primop_convertHash` — `__convertHash`, args `{"args"}` → `prim_convertHash`.
- `primop_match` — `__match`, args `{"regex", "str"}` → `prim_match`.
- `primop_split` — `__split`, args `{"regex", "str"}` → `prim_split`.
- `primop_concatStringsSep` — `__concatStringsSep`, args `{"separator", "list"}` → `prim_concatStringsSep`.
- `primop_replaceStrings` — `__replaceStrings`, args `{"from", "to", "s"}` → `prim_replaceStrings`.
- `primop_parseDrvName` — `__parseDrvName`, args `{"s"}` → `prim_parseDrvName`.
- `primop_compareVersions` — `__compareVersions`, args `{"s1", "s2"}` → `prim_compareVersions`.
- `primop_splitVersion` — `__splitVersion`, args `{"s"}` → `prim_splitVersion`.

Counting summary: 92 `static RegisterPrimOp` instances total. Of those, 87 have an `.impl = prim_X` for one of the 90 `prim_*` definitions in the file, and 5 (`primop_scopedImport`, `primop_import`, `primop_break`, `primop_abort`, `primop_throw`) are inline-lambda primops with no dedicated `prim_*` function. The 3 `prim_*` functions that have no corresponding `RegisterPrimOp` (`prim_importNative`, `prim_exec`, `prim_second`) are wired into base env directly via `addPrimOp` inside `createBaseEnv` (`prim_importNative`/`prim_exec` gated on `enableNativeCode`; `prim_second` selected for `__traceVerbose` when `traceVerbose` is off).

Constants and primops added directly via `addPrimOp` / `addConstant` inside `EvalState::createBaseEnv` (i.e. not through `RegisterPrimOp`):
- `addConstant("builtins", v, ...)` — empty attrset (allocated with `buildBindings(128)`), populated by the rest of `createBaseEnv`.
- `addConstant("true", true, ...)`.
- `addConstant("false", false, ...)`.
- `addConstant("null", &Value::vNull, ...)`.
- `addConstant("__currentTime", v, ...)` — `time(nullptr)` at init, only set when not pure-eval; marked `impureOnly = true`.
- `addConstant("__currentSystem", v, ...)` — `settings.getCurrentSystem()` (eval-system / system); marked `impureOnly = true`.
- `addConstant("__nixVersion", v, ...)` — `nixVersion` constant.
- `addConstant("__storeDir", v, ...)` — `store->storeDir`.
- `addConstant("__langVersion", 6, ...)` — bumped per language change.
- `addPrimOp({ name = "__importNative", arity = 2, impl = prim_importNative })` — gated on `enableNativeCode`. Wrapped in `#ifndef _WIN32`.
- `addPrimOp({ name = "__exec", arity = 1, impl = prim_exec })` — gated on `enableNativeCode`. Wrapped in `#ifndef _WIN32`.
- `addPrimOp({ name = "__traceVerbose", args = {"e1","e2"}, arity = 2, impl = settings.traceVerbose ? prim_trace : prim_second, ... })`.
- `addConstant("__nixPath", v, ...)` — list of `{ path, prefix }` entries from `state.lookupPath`.
- `addConstant("derivation", vDerivation, ...)` — `nFunction`-typed constant; the underlying value is `evalFile`-loaded from `state.derivationInternal` *after* baseEnv is sealed (so it can use `builtins`).

Method (the registry-driven step):
- `EvalState::createBaseEnv(const EvalSettings & evalSettings)` then iterates `RegisterPrimOp::primOps()` and `evalSettings.extraPrimOps`, fixing up arity (`std::max(args.size(), arity)`) before calling `addPrimOp`; primops gated on a non-default `experimentalFeature` are skipped unless that feature is enabled. Then `getBuiltins().attrs()->sort()` and `staticBaseEnv->sort()`. Finally `evalFile(derivationInternal, *vDerivation)` to populate the lazy `derivation` constant.

### Type aliases
- `ValueInitializer = void (*)(EvalState & state, Value & v)` — `extern "C" typedef` used by `prim_importNative` to call DSO-loaded initializers (declared inside the `#ifndef _WIN32` block).
- `ValueList = std::list<Value *, gc_allocator<Value *>>` — file-scope typedef used by `prim_genericClosure` for the work set.

### Macros / globals
- `RegisterPrimOp::PrimOps & RegisterPrimOp::primOps()` — Meyers-singleton vector storing the global primop registry (definition; static-local `primOps` returned by reference).
- `RegisterPrimOp::RegisterPrimOp(PrimOp && primOp)` — definition (appends to the registry via `primOps().push_back`).
- `static struct LazyPosAccessors { ... } makeLazyPosAccessors;` — file-scope singleton instance that lazily produces position thunks.
- `void EvalState::createBaseEnv(const EvalSettings &)` — top-level base-env initializer, replays `RegisterPrimOp::primOps()` plus the local `addConstant`/`addPrimOp` calls; defined here.
- Conditional compilation: `#ifndef _WIN32` around `prim_importNative` / `prim_exec` definitions and around the `addPrimOp({ ..., impl = prim_importNative })` / `prim_exec` registrations in `createBaseEnv`.
- `#pragma GCC diagnostic push/pop` blocks suppressing `-Wswitch-enum` inside `CompareValues::operator()` and `fileTypeToString`.
- Includes (notable): `<boost/container/small_vector.hpp>`, `<boost/unordered/concurrent_flat_map.hpp>`, `<boost/unordered/unordered_flat_map.hpp>`, `<nlohmann/json.hpp>`, `<dlfcn.h>` (in `#ifndef _WIN32`), `<cmath>`, `<regex>`, plus the project-internal evaluator/store/util headers.

---

## Cross-file observations

- **Argument-validation boilerplate is heavily duplicated.** Almost every `prim_*` opens with `state.forceValue` / `state.forceAttrs` / `state.forceList` / `state.forceString` / `state.forceStringNoCtx` / `state.forceBool` / `state.forceInt` / `state.forceFloat`, each with a hand-written "while evaluating the Nth argument passed to builtins.<name>" message. The patterns vary only by ordinal ("first/second/third"), the primop name, and the argument's purpose. A small constexpr helper that takes the primop name + arg-index could eliminate hundreds of nearly-identical strings (and reduce binary size) while making the messages uniform.
- **Fetcher primops share most of their structure but are not factored.** `fetchTree`, `fetchGit`, `fetchTarball`, `fetchurl`, `fetchClosure`, and `fetchMercurial` all do (1) attrset-or-string parsing, (2) `state.checkURI`, (3) pure-eval-locking checks (`pureEval` && `!isLocked` / `!rev`), (4) `state.allowAndSetStorePathString` / `mkStorePathString`, (5) optional substituter short-circuit (only `fetch(...)` in `fetchTree.cc` actually does this short-circuit; `fetchClosure` does its own copy-or-CA-rewrite). `fetchTree.cc` already factors `fetchTree(...)` and `fetch(...)` for the closely related entrypoints; the same job is done ad-hoc in `fetchClosure.cc` (three near-duplicate `runFetchClosureWith*` functions for the three modes) and in `fetchMercurial.cc` (single inline implementation). A higher-level "fetch primop" helper that takes a strategy could consolidate the three.
- **Symmetric pairs in `context.cc`.** `prim_unsafeDiscardOutputDependency` and `prim_addDrvOutputDependencies` are explicit inverses with mirror-image `std::visit(overloaded{...})` blocks, and `prim_getContext` / `prim_appendContext` form the encode/decode pair. They could share a common visitor over `NixStringContextElem` rather than re-spelling each variant in three places.
- **Numeric primops (`__add`, `__sub`, `__mul`, `__div`).** Each one is the same template instantiated four times: forceValue both args, dispatch on `nFloat` else int, check overflow with `valueChecked()`, raise `EvalError` with a slightly different verb. (`prim_div` additionally checks division-by-zero on the float and is structured around `f2`.) A helper templated on a binary op (or a function pointer plus a verb) would collapse them.
- **`prim_isNull` … `prim_isPath` (eight `is*` primops; nine if you count `prim_isAttrs` and `prim_isList`, ten if you count `prim_isFunction`).** All identical except for the enum-tag they compare against. A registration macro or table-driven approach would remove the boilerplate.
- **`prim_ceil` and `prim_floor` are byte-for-byte twins** (only `ceil(value)` vs `floor(value)` differs; the precision-loss/overflow blocks and the link to `https://github.com/NixOS/nix/issues/12899` are identical). Same opportunity as for the arithmetic primops.
- **`prim_match` and `prim_split`** share the `RegexCache::get` lookup, the `forceStringNoCtx` regex argument, the `forceString` haystack argument, and identical `regex_error` handling for `error_space` vs everything-else. The `mkString(state, match)` helper they share could be extended into a regex-result writer.
- **`prim_any` / `prim_all`** already factor through `anyOrAll(bool any, ...)`, which is the right pattern; the rest of the codebase rarely follows it.
- **Lazy-thunk machinery.** `LazyPosAccessors` (in `primops.cc`) and `LazyFetcherAttr` + `prim_forceLazyFetcherAttr` (in `fetchTree.cc`) re-implement essentially the same idea: an unregistered/internal `PrimOp` whose impl forces an external/integer-encoded value and a constructor that creates a pre-applied thunk via `mkApp`. Promoting this to a utility (e.g. `mkLazyValueThunk(primFn, externalState)`) would let other primops trivially defer expensive work the same way.
- **`fileTypeToString` plus the `_sds` static-string-data utilities** are used in `prim_typeOf` (`"int"_sds`, `"bool"_sds`, …) but a single `nValueTypeToString(state, v->type())` analogous to `fileTypeToString` would deduplicate the type-tag-to-string logic between `prim_typeOf` and any future use.
- **Attrset traversal patterns.** Every fetcher primop iterates `*args[0]->attrs()` with an `if (n == "x") ... else if (n == "y") ... else error("unsupported")` chain. `fetchTree`'s `for (auto & attr : *args[0]->attrs())`, `fetchClosure`'s identical loop, plus `fetchMercurial`'s and `fetch`'s, are ripe for a helper that takes a `{ name → handler }` table and yields a uniform "unsupported argument" error.
- **Hash-string / hash-file overlap.** `prim_hashString` and `prim_hashFile` both parse `algo` via `parseHashAlgo`, raise the same `unknown hash algorithm` error, then call `hashString` / `hashPath` and stringize at base-16. Could share a private helper.
- **`prim_typeOf` versus the type predicates.** `prim_typeOf` already enumerates every type tag. Implementing `prim_isString` etc. as `prim_typeOf(...) == s` would be more code, but the existing eight predicates could be code-generated from the same enum to keep them in sync.
- **`primop_*` static initializers run at static-init time, in unspecified order.** The registry is processed by `EvalState::createBaseEnv` in `RegisterPrimOp::primOps()` insertion order (which is link-order-dependent). The header comment on `RegisterPrimOp` explicitly notes that arity-0 primops (constants) are called during `EvalState` initialization "so there may be primops not yet added and builtins is not yet sorted." That makes it impossible to have one primop reference another at registration time — see the `addConstant("derivation", ...)` deferred-load that happens after baseEnv sealing. Worth documenting somewhere central.
- **Forward-declared `prim_lessThan`.** `prim_sort` reads `args[0]->primOp()->impl.get_fn().target<decltype(&prim_lessThan)>()` to unwrap and short-circuit. That's a one-off optimization for one specific primop comparator. If this pattern recurs (e.g., other functor-comparators) it should be a generic "is this primop X?" check rather than a per-callsite hack.
- **Numeric overflow diagnostics in `prim_ceil`/`prim_floor`** reference `https://github.com/NixOS/nix/issues/12899` twice each (once for the under/over flow and once for the precision-loss case); a shared helper that emits this diagnostic with the offending operand would cut roughly 30 lines of repeated text and centralize the link for when the issue is fixed.

