# Inventory — Shard 14: libfetchers

This shard covers `src/libfetchers/`: the `Input`/`InputScheme` abstraction
plus all scheme implementations (git, github/gitlab/sourcehut, mercurial,
tarball/file, path, indirect) and supporting infrastructure (cache,
fetch-settings, fetch-to-store, attrs, registry, input-cache,
filtering-source-accessor, git-utils, git-lfs-fetch).

Files are processed in batches; each section lists namespaces, classes /
structs / enums, free + member functions, type aliases, and any macros or
globals. The Cross-file observations section at the end highlights the
duplication patterns across scheme implementations.

## File: src/libfetchers/attrs.cc

### Namespaces
- `nix::fetchers`.

### Classes / structs / enums
- None (only function definitions).

### Functions
- `forceAttr(const Attr &) -> ResolvedAttr` — `std::visit` over the variant; resolves a `LazyAttr` via its stored `compute()` callback and passes through the eager `string`/`uint64_t`/`Explicit<bool>` alternatives.
- `jsonToAttrs(const nlohmann::json &) -> Attrs` — copies a JSON object into the `Attrs` map, accepting only number/string/bool leaves; throws `Error("unsupported input attribute type in lock file")` on other types.
- `attrsToJSON(const Attrs &) -> nlohmann::json` — forces every attr (via `forceAttr`) and writes it back to JSON; calls `unreachable()` on unknown variant alternatives.
- `maybeGetLazyAttr(const Attrs &, const std::string &) -> std::optional<LazyAttr>` — returns the stored `LazyAttr` only if the attribute exists and still holds the lazy variant.
- `maybeGetStrAttr(const Attrs &, const std::string &) -> std::optional<std::string>` — typed string accessor; throws when present but not a string.
- `getStrAttr(const Attrs &, const std::string &) -> std::string` — wraps `maybeGetStrAttr`; throws `Error("input attribute '%s' is missing")` when absent.
- `maybeGetIntAttr(const Attrs &, const std::string &) -> std::optional<uint64_t>` — typed integer accessor; throws when present but not an integer.
- `getIntAttr(const Attrs &, const std::string &) -> uint64_t` — wraps `maybeGetIntAttr`; throws on missing.
- `maybeGetBoolAttr(const Attrs &, const std::string &) -> std::optional<bool>` — typed bool accessor; reads `Explicit<bool>::t`.
- `getBoolAttr(const Attrs &, const std::string &) -> bool` — wraps `maybeGetBoolAttr`; throws on missing.
- `attrsToQuery(const Attrs &) -> StringMap` — round-trips locked attrs into URL query form; renders `Explicit<bool>` as `"1"`/`"0"` and integers via `fmt("%d", ...)`.
- `getRevAttr(const Attrs &, const std::string &) -> Hash` — wraps `getStrAttr` with `Hash::parseAny(..., HashAlgorithm::SHA1)`.

### Type aliases
- None (provided by header).

### Macros / globals
- None.

## File: src/libfetchers/cache.cc

### Namespaces
- `nix::fetchers`.

### Classes / structs / enums
- `CacheImpl` (struct, `: Cache`) — concrete implementation backed by `fetcher-cache-v4.sqlite` in the user cache dir.
  - Nested `State` struct: `SQLite db`, `SQLiteStmt upsert`, `SQLiteStmt lookup`.
  - Members: `Sync<State> _state`, `const Settings & settings` (back-reference for TTL).
  - Constructor opens the DB (`useWAL` from `nix::settings.useSQLiteWAL`), calls `db.isCache()`, runs `schema`, and prepares both statements.
  - Override `upsert(const Key &, const Attrs &)` — inserts/replaces a row using JSON-dumped key/value and current `time(nullptr)`.
  - Override `lookup(const Key &) -> std::optional<Attrs>` — returns `lookupExpired` value irrespective of expiry.
  - Override `lookupWithTTL(const Key &) -> std::optional<Attrs>` — returns value only when not expired; debug-logs the ignored expired entry.
  - Override `lookupExpired(const Key &) -> std::optional<Result>` — runs the prepared statement; computes `expired` from `settings.tarballTtl` (treating `0` as already-expired).
  - Override `upsert(Key, Store &, Attrs, const StorePath &)` — mixes `store.storeDir` into the key under `"store"` and persists the result under `"storePath"` before delegating.
  - Override `lookupStorePath(Key, Store &) -> std::optional<ResultWithStorePath>` — adds a temproot before `isValidPath` to avoid GC races and silently ignores cache entries whose store paths have disappeared.
  - Override `lookupStorePathWithTTL(Key, Store &) -> std::optional<ResultWithStorePath>` — wraps `lookupStorePath` with the TTL gate.

### Functions
- `Settings::getCache() const -> ref<Cache>` — lazily constructs the singleton `CacheImpl` under the `Sync<>` lock on `_cache`.

### Type aliases
- None.

### Macros / globals
- `static const char * schema` — DDL for the `Cache(domain, key, value, timestamp)` table with composite primary key on `(domain, key)`.

## File: src/libfetchers/fetch-settings.cc

### Namespaces
- `nix::fetchers`.

### Classes / structs / enums
- None new (only the constructor body).

### Functions
- `Settings::Settings()` — empty body; setting registration happens via the `Setting<>` member initializers in the header.

### Type aliases
- None.

### Macros / globals
- None.

## File: src/libfetchers/fetch-to-store.cc

### Namespaces
- `nix` (the helpers live outside `fetchers::`).

### Classes / structs / enums
- None.

### Functions
- `makeSourcePathToHashCacheKey(std::string_view fingerprint, ContentAddressMethod method, const CanonPath & path) -> fetchers::Cache::Key` — assembles `("sourcePathToHash", {fingerprint, method.render(), path.abs()})`.
- `fetchToStore(const fetchers::Settings &, Store &, const SourcePath &, FetchMode, std::string_view name, ContentAddressMethod, PathFilter *, RepairFlag) -> StorePath` — convenience wrapper returning only the store path (delegates to `fetchToStore2`).
- `fetchToStore2(...) -> std::pair<StorePath, Hash>` — primary implementation: skips the source-path cache when a `filter` is set, otherwise queries the source-path cache via the fingerprint; on miss runs `Store::computeStorePath` (DryRun) or `Store::addToStore` (Copy), logs an `Activity`, and updates the cache with the resulting hash. Adds a temproot before checking `isValidPath` on cache hits.

### Type aliases
- None.

### Macros / globals
- `static auto barf = getEnv("_NIX_TEST_BARF_ON_UNCACHEABLE").value_or("") == "1"` (function-local static inside `fetchToStore2`) — test-only knob that turns missing fingerprints into hard errors.

## File: src/libfetchers/fetchers.cc

### Namespaces
- `nix::fetchers`, plus an `nlohmann` block for `adl_serializer<fetchers::PublicKey>`.

### Classes / structs / enums
- None new (`InputSchemeMap` re-aliased locally as `using InputSchemeMap = std::map<std::string_view, std::shared_ptr<InputScheme>>`).

### Functions
- `static inputSchemes() -> InputSchemeMap &` — Meyers-singleton holding the global scheme registry.
- `registerInputScheme(std::shared_ptr<InputScheme> &&)` — inserts into `inputSchemes()` keyed by `schemeName()`; throws `Error("Input scheme with name %s already registered", ...)` on collision.
- `getAllInputSchemes() -> const InputSchemeMap &` — read accessor.
- `Input::fromURL(const Settings &, const std::string &, bool requireTree)` — parses the URL string and delegates to the `ParsedURL` overload.
- `Input::fromURL(const Settings &, const ParsedURL &, bool requireTree)` — iterates every scheme, calls `inputScheme->inputFromURL`, runs `experimentalFeatureSettings.require`, calls `fixupInput`, and emits a `git+file` vs `file+git` hint (via `parseUrlScheme`) when no scheme matches.
- `static fixupInput(Input &)` — touches `getType()`/`getRef()`/`getRevCount()`/`getLastModified()` so attribute coercion errors surface eagerly.
- `Input::fromAttrs(const Settings &, Attrs &&)` — extracts the `type` attr, validates allowed attrs (special-casing `type` and `__final`), delegates to `inputScheme->inputFromAttrs`, and falls back to a schemeless raw input (still subjected to `fixupInput`) when no scheme matches or returns nullopt.
- `Input::getFingerprint(Store &) const -> std::optional<std::string>` — caches `scheme->getFingerprint` in the mutable `cachedFingerprint`; returns nullopt when no scheme.
- `Input::toURL() const -> ParsedURL` — delegates to `scheme->toURL`; throws when no scheme.
- `Input::toURLString(const StringMap & extraQuery) const -> std::string` — appends extra query parameters to the result of `toURL()`.
- `Input::to_string() const -> std::string` — `toURL().to_string()`.
- `Input::isDirect() const -> bool` — `!scheme || scheme->isDirect(*this)`.
- `Input::isLocked(const Settings &) const -> bool` — `scheme && scheme->isLocked(settings, *this)`.
- `Input::isFinal() const -> bool` — reads the `__final` boolean attr defaulting to false.
- `Input::isRelative() const -> std::optional<std::filesystem::path>` — asserts `scheme` and delegates.
- `Input::toAttrs() const -> Attrs` — returns the stored `attrs`.
- `Input::operator==(const Input &) const noexcept` — compares stored `attrs`.
- `Input::contains(const Input & other) const -> bool` — true if equal, or equal after stripping `ref`/`rev` from `other`.
- `Input::fetchToStore(const Settings &, Store &) const -> std::pair<StorePath, Input>` — wraps `getAccessorUnchecked`, copies into the store via `nix::fetchToStore` (FetchMode::Copy), stamps `narHash` (queried via `queryPathInfo`) and `__final` on the result, runs `checkLocks`; on `Error` adds a trace and rethrows.
- `Input::checkLocks(Input specified, Input & result)` — for final specified inputs, normalises both narHash strings to canonical SRI form, requires every shared field to be equal, and overwrites `result.attrs = specified.attrs`; for non-final specified inputs, requires narHash and rev (if previously present) to match (throws `Error(102, ...)` on narHash mismatch).
- `Input::getAccessor(const Settings &, Store &) const -> std::pair<ref<SourceAccessor>, Input>` — wraps `getAccessorUnchecked`, sets `__final`, runs `checkLocks`; on `Error` adds a trace and rethrows.
- `Input::getAccessorUnchecked(const Settings &, Store &) const -> std::pair<ref<SourceAccessor>, Input>` — substitutes from the store when possible (final + narHash; calls `computeStorePath`/`ensurePath`/`requireStoreObjectAccessor`, and seeds the source-path-to-hash cache for the substituted tree). Otherwise takes a `PathLocks` lock under `getCacheDir()/fetcher-locks/<sha256-of-attrs>`, optionally sleeps a second when `_NIX_TEST_CONCURRENT_FETCHES=1`, delegates to `scheme->getAccessor`, and propagates the fingerprint between the accessor and the result.
- `Input::applyOverrides(std::optional<std::string> ref, std::optional<Hash> rev) const -> Input` — schemeless inputs are returned unchanged; otherwise delegates.
- `Input::clone(const Settings &, Store &, const std::filesystem::path & destDir) const` — asserts `scheme` and delegates.
- `Input::getSourcePath() const -> std::optional<std::filesystem::path>` — asserts `scheme` and delegates.
- `Input::putFile(const CanonPath &, std::string_view, std::optional<std::string> commitMsg) const` — asserts `scheme` and delegates.
- `Input::getName() const -> std::string` — defaults missing `name` attribute to `"source"`.
- `Input::computeStorePath(Store &) const -> StorePath` — `makeFixedOutputPath(name, FixedOutputInfo{NixArchive, narHash, no refs})`; throws when no narHash.
- `Input::getType() const -> std::string` — `getStrAttr(attrs, "type")`.
- `Input::getNarHash() const -> std::optional<Hash>` — parses `narHash` as SRI (empty string -> empty SHA-256); throws `UsageError` if the algorithm is not SHA-256.
- `Input::getRef() const -> std::optional<std::string>` — returns the `ref` attr if any.
- `Input::getRev() const -> std::optional<Hash>` — first tries `Hash::parseAnyPrefixed`; on `BadHash` falls back to `Hash::parseAny(..., SHA1)` for legacy lock files.
- `Input::getRevCount() const -> std::optional<uint64_t>` — returns the `revCount` int attr if any.
- `Input::getLastModified() const -> std::optional<time_t>` — returns the `lastModified` int attr if any.
- `InputScheme::toURL(const Input &) const` (default) — throws "don't know how to convert input ... to a URL".
- `InputScheme::applyOverrides(const Input &, std::optional<std::string>, std::optional<Hash>) const` (default) — throws if `ref` or `rev` is set; otherwise returns input unchanged.
- `InputScheme::getSourcePath(const Input &) const` (default) — returns `{}`.
- `InputScheme::putFile(const Input &, const CanonPath &, std::string_view, std::optional<std::string>) const` (default) — throws "input '%s' does not support modifying file '%s'".
- `InputScheme::clone(const Settings &, Store &, const Input &, const std::filesystem::path & destDir) const` — generic implementation: errors when `destDir` exists, resolves via `getAccessor`, logs an `Activity`, and `copyRecursive`s into a `RestoreSink(/*startFsync=*/false)`.
- `InputScheme::experimentalFeature() const` (default) — `std::nullopt`.
- `publicKeys_to_string(const std::vector<PublicKey> &) -> std::string` — JSON dump.
- `nlohmann::adl_serializer<fetchers::PublicKey>::from_json` / `to_json` — `{type, key}` round-trip with `type` defaulting (uses `optionalValueAt` for `type`, mandatory `valueAt` for `key`).

### Type aliases
- `InputSchemeMap` (file-local re-alias of `std::map<std::string_view, std::shared_ptr<InputScheme>>`).

### Macros / globals
- `static auto inTest = getEnv("_NIX_TEST_CONCURRENT_FETCHES") == "1"` (function-local static inside `getAccessorUnchecked`) — test-only delay knob that sleeps for one second before delegating.
- `#ifndef DOXYGEN_SKIP` guard around the JSON serialiser definitions.

## File: src/libfetchers/filtering-source-accessor.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- `AllowListSourceAccessorImpl` (struct, `: AllowListSourceAccessor`) — production implementation backed by `SharedSync<std::set<CanonPath>> allowedPrefixes` and a `boost::concurrent_flat_set<CanonPath> allowedPaths`.
  - Constructor seeds both containers from the provided initial sets and forwards `next`/`makeNotAllowedError` to `AllowListSourceAccessor`.
  - Override `isAllowed(const CanonPath &)` — short-circuits on `allowedPaths.contains(path)`, else delegates to `path.isAllowed(*allowedPrefixes.readLock())` (read lock held for the full expression when the contains check fails).
  - Override `allowPrefix(CanonPath)` — write-locks the prefix set and inserts.

### Functions
- `FilteringSourceAccessor::getPhysicalPath(const CanonPath &)` — calls `checkAccess` then delegates to `next->getPhysicalPath(prefix / path)`.
- `FilteringSourceAccessor::readFile(const CanonPath &, Sink &, fun<void(uint64_t)> sizeCallback)` — same pattern as `getPhysicalPath`.
- `FilteringSourceAccessor::pathExists(const CanonPath &)` — `isAllowed && next->pathExists` (no exception thrown).
- `FilteringSourceAccessor::maybeLstat(const CanonPath &)` — returns nullopt when not allowed; else delegates.
- `FilteringSourceAccessor::lstat(const CanonPath &)` — `checkAccess` then delegate.
- `FilteringSourceAccessor::readDirectory(const CanonPath &)` — `checkAccess` for the directory itself, then iterates `next->readDirectory(...)` filtering each entry by `isAllowed(path / entry.first)`.
- `FilteringSourceAccessor::readLink(const CanonPath &)` — `checkAccess` then delegate.
- `FilteringSourceAccessor::showPath(const CanonPath &)` — `displayPrefix + next->showPath(...) + displaySuffix`.
- `FilteringSourceAccessor::getFingerprint(const CanonPath &)` — returns the stored `fingerprint` if any, else delegates to `next`.
- `FilteringSourceAccessor::checkAccess(const CanonPath &)` — throws `makeNotAllowedError(path)` when `!isAllowed`.
- `AllowListSourceAccessor::create(ref<SourceAccessor> next, const std::set<CanonPath> & allowedPrefixes, const std::unordered_set<CanonPath> & allowedPaths, MakeNotAllowedError &&)` — `make_ref<AllowListSourceAccessorImpl>` factory.
- `CachingFilteringSourceAccessor::isAllowed(const CanonPath &)` — memoises `isAllowedUncached` results in the inherited `cache` map.

### Type aliases
- None (uses `MakeNotAllowedError` from the header).

### Macros / globals
- None.

## File: src/libfetchers/registry.cc

### Namespaces
- `nix::fetchers`.

### Classes / structs / enums
- None new.

### Functions
- `Registry::read(const Settings &, const SourcePath &, RegistryType) -> std::shared_ptr<Registry>` — parses `version: 2` JSON; entries with a `dir` attribute extract that into `extraAttrs`; reads the optional `exact` boolean per entry; warns and returns an empty registry on JSON parse / read errors; throws on unsupported versions.
- `Registry::write(const std::filesystem::path &)` — serialises to `version: 2` JSON, merging `extraAttrs` into each `to` object via `obj["to"].update(...)`; creates parent directories.
- `Registry::add(const Input & from, const Input & to, const Attrs & extraAttrs)` — appends an entry with `exact = false`.
- `Registry::remove(const Input &)` — erases entries whose `from` equals the given input.
- `static getSystemRegistryPath() -> std::filesystem::path` — `nixConfDir() / "registry.json"`.
- `static getSystemRegistry(const Settings &) -> std::shared_ptr<Registry>` — function-local static reading the system registry once.
- `getUserRegistryPath() -> std::filesystem::path` — `getConfigDir() / "registry.json"`.
- `getUserRegistry(const Settings &) -> std::shared_ptr<Registry>` — function-local static for the user registry.
- `getCustomRegistry(const Settings &, const std::filesystem::path &) -> std::shared_ptr<Registry>` — function-local static result (so only the first call's path is honoured).
- `getFlagRegistry() -> std::shared_ptr<Registry>` — process-wide static registry for `--override-flake` style overrides.
- `overrideRegistry(const Input &, const Input &, const Attrs &)` — appends to the flag registry via `Registry::add`.
- `static getGlobalRegistry(const Settings &, Store &) -> std::shared_ptr<Registry>` — function-local static; reads `settings.flakeRegistry`; for non-absolute (treated as URL) paths downloads `flake-registry.json` into the store and adds a perm-root via `LocalFSStore::addPermRoot` when the store is local; absolute paths read the file via `getFSSourceAccessor` after symlink resolution; empty path yields an empty registry.
- `getRegistries(const Settings &, Store &) -> Registries` — returns flag, user, system, global in that order.
- `lookupInRegistries(const Settings &, Store &, const Input &, UseRegistries) -> std::pair<Input, Attrs>` — bails immediately when `useRegistries == No`; otherwise repeats lookups (cycle limit 100) using `entry.exact` exact-match or `entry.from.contains(input)` partial-match logic; for partial matches, applies any `ref`/`rev` from the unmatched input via `applyOverrides` only when the entry's `from` did not constrain them; respects `Limited` mode (only Flag and Global registries); throws if the result is still indirect.

### Type aliases
- None.

### Macros / globals
- None.

## File: src/libfetchers/indirect.cc

### Namespaces
- `nix::fetchers`.

### Classes / structs / enums
- `IndirectInputScheme` (struct, `: InputScheme`) — represents `flake:` URLs that route through registries.
  - Override `inputFromURL(const Settings &, const ParsedURL &, bool requireTree)` — accepts only `scheme == "flake"`, splits path segments via `pathSegments(skipEmpty=true)`, supports `flake:id`, `flake:id/(ref|rev)`, `flake:id/ref/rev` (uses `revRegex` and `isLegalRefName` to disambiguate), validates the id against `flakeRegex`, and writes `type=indirect`, `id`, `ref?`, `rev?` into the new input.
  - Override `schemeName()` — `"indirect"`.
  - Override `schemeDescription()` — `""` (TODO).
  - Override `allowedAttrs()` — static map with `id`, `ref`, `rev`, `narHash`.
  - Override `inputFromAttrs(const Settings &, const Attrs &)` — re-validates `id` against `flakeRegex`; otherwise copies the attrs through.
  - Override `toURL(const Input &)` — emits `flake:<id>[/<ref>][/<rev>]` URLs.
  - Override `applyOverrides(const Input &, std::optional<std::string>, std::optional<Hash>)` — sets `ref`/`rev` attrs unconditionally.
  - Override `getAccessor(const Settings &, Store &, const Input &)` — always throws; indirects must be resolved through the registry first.
  - Override `experimentalFeature()` — `Xp::Flakes`.
  - Override `isDirect(const Input &)` — `false`.

### Functions
- `static rIndirectInputScheme = OnStartup([] { registerInputScheme(std::make_unique<IndirectInputScheme>()); })` — auto-registers at startup.

### Type aliases
- None.

### Macros / globals
- `std::regex flakeRegex("[a-zA-Z][a-zA-Z0-9_-]*", std::regex::ECMAScript)` — file-scope regex for flake IDs.

## File: src/libfetchers/input-cache.cc

### Namespaces
- `nix::fetchers`.

### Classes / structs / enums
- `InputCacheImpl` (struct, `: InputCache`) — concrete cache.
  - Member: `Sync<std::map<Input, CachedInput>> cache_`.
  - Override `lookup(const Input &) const` — read-locks the map; debug-logs the mapping when found.
  - Override `upsert(Input, CachedInput)` — write-locks and `insert_or_assign`s.
  - Override `clear()` — clears the map and additionally calls `GitRepo::invalidateWorkdirInfoCache()` so `:reload` picks up workdir changes.

### Functions
- `InputCache::getAccessor(const Settings &, Store &, const Input & originalInput, UseRegistries) -> CachedResult` — top-level entry point. For direct inputs that miss the cache it calls `Input::getAccessor` and caches the result. For indirect inputs (when `useRegistries != No`) it calls `lookupInRegistries`, looks up the resolved input in the cache, fetches it on miss, and stores entries under both the original and the resolved keys (carrying `extraAttrs` from the registry); throws when an indirect input is supplied with `useRegistries == No`.
- `InputCache::create() -> ref<InputCache>` — `make_ref<InputCacheImpl>` factory.

### Type aliases
- None.

### Macros / globals
- None.

## File: src/libfetchers/path.cc

### Namespaces
- `nix::fetchers`.

### Classes / structs / enums
- `PathInputScheme` (struct, `: InputScheme`) — handles `path:` inputs.
  - Override `inputFromURL(const Settings &, const ParsedURL &, bool requireTree)` — rejects an authority component, sets `type=path` and `path` (decoded via `urlPathToPath`), accepts `rev`/`narHash` strings and integer `revCount`/`lastModified` query params (via `string2Int<uint64_t>`); throws on unknown query parameters.
  - Override `schemeName()` — `"path"`.
  - Override `schemeDescription()` — `""` (TODO).
  - Override `allowedAttrs()` — static map with `path`, `rev`, `revCount`, `lastModified`, `narHash`.
  - Override `inputFromAttrs(const Settings &, const Attrs &)` — only validates that `path` is present (via `getStrAttr`) and copies attrs through.
  - Override `toURL(const Input &)` — calls `attrsToQuery`, drops `path`/`type`/`__final`, emits `ParsedURL{scheme=path, path=pathToUrlPath(...), query}`.
  - Override `getSourcePath(const Input &)` — returns `getAbsPath(input)`.
  - Override `putFile(const Input &, const CanonPath &, std::string_view, std::optional<std::string>)` — `writeFile(getAbsPath(input) / path.rel(), contents)`.
  - Override `isRelative(const Input &)` — returns `path` when not absolute, nullopt otherwise.
  - Override `isLocked(const Settings &, const Input &)` — true iff `narHash` is present.
  - Helper `getAbsPath(const Input &) -> std::filesystem::path` — throws when path is relative; otherwise `canonPath`s it.
  - Override `getAccessor(const Settings &, Store &, const Input &)` — checks for an existing store path with name `"source"` (adds a temproot when found), otherwise dumps the absolute path with `dumpPathAndGetMtime` (capturing mtime) and `addToStoreFromDump`s it; assigns `accessor->fingerprint = "path:<narHash-SRI>"`; eagerly seeds the source-path-to-hash cache so `fetchToStore` does not copy again; synthesises a `lastModified` attribute from the dumped tree's mtime when the user did not supply one.
  - Override `experimentalFeature()` — `Xp::Flakes`.

### Functions
- `static rPathInputScheme = OnStartup([] { registerInputScheme(std::make_unique<PathInputScheme>()); })` — startup registration.

### Type aliases
- None.

### Macros / globals
- None.

## File: src/libfetchers/git.cc

### Namespaces
- `nix::fetchers`, plus an unnamed (anonymous) namespace inside it for static helpers.

### Classes / structs / enums
- `GitInputScheme` (struct, `: InputScheme`) — handles `git`, `git+http`, `git+https`, `git+ssh`, `git+file` URLs and `type = "git"` attrs.
  - Nested `RepoInfo` struct: `std::variant<std::filesystem::path, ParsedURL> location`, `GitRepo::WorkdirInfo workdirInfo`, `std::string gitDir = ".git"`. Methods `locationToArg()`, `getPath()`, `warnDirty(const Settings &)`.
  - Bool attribute helpers: `getShallowAttr`, `getSubmodulesAttr`, `getLfsAttr`, `getExportIgnoreAttr`, `getAllRefsAttr` — each defaults to `false`.
  - `getRepoInfo(const Input &) const -> RepoInfo` — checks rev hash algorithm (sha1/sha256), turns `file://` URLs into a `std::filesystem::path` location (warns about relative paths) unless they point at a bare repo or `_NIX_FORCE_HTTP` is set; for non-file URLs erases legacy `?dir=` query parameter; calls `GitRepo::getCachedWorkdirInfo` when a local repo path with no ref/rev is given.
  - `getLastModified(const Settings &, const RepoInfo &, const std::filesystem::path & repoDir, const Hash & rev) const -> uint64_t` — caches under the `gitLastModified` cache domain keyed by rev.
  - `getRevCount(ref<Cache>, const RepoInfo &, const std::filesystem::path &, const Hash &) const -> uint64_t` — throws on shallow repos; caches under the `gitRevCount` cache domain keyed by rev; logs an `Activity`.
  - `lazyRevCount(...)` — wraps `getRevCount` in a `LazyAttr` via `makeLazyAttr` so the value is computed at most once on demand.
  - `getDefaultRef(const Settings &, const RepoInfo &, bool shallow) const -> std::string` — uses `GitRepo::getWorkdirRef` for local paths and `readHeadCached` for remote URLs; warns and falls back to `"master"` when neither yields a ref.
  - Static `makeNotAllowedError(std::filesystem::path repoPath) -> MakeNotAllowedError` — produces `RestrictedPathError`s with hints (`git -C <repo> add ...`) for paths inside the repo, distinguishing tracked vs missing paths.
  - `verifyCommit(const Input &, std::shared_ptr<GitRepo>) const` — implements the `verifyCommit`/`publicKeys` experimental feature; throws when dirty (`repo == nullptr`); auto-enables verification when `publicKeys` is non-empty.
  - `getAccessorFromCommit(const Settings &, Store &, RepoInfo &, Input &&) const -> std::pair<ref<SourceAccessor>, Input>` — for revision-based fetches: clones into `getCachePath`, fetches under a `PathLocks` lock (with retry on stale ref), resolves the ref to a rev (ref `HEAD`, `refs/...`, or branch), populates `lastModified`/`revCount` (using `lazyRevCount`), recursively fetches submodules with synthesised attrs into a `MountedSourceAccessor`, applies `verifyCommit`, and updates `storeCachedHead` after a successful fetch.
  - `getAccessorFromWorkdir(const Settings &, Store &, RepoInfo &, Input &&) const -> std::pair<ref<SourceAccessor>, Input>` — for local workdirs: builds a `WorkdirInfo`-based accessor via `repo->getAccessor`, fetches submodule workdirs (and marks the parent dirty if a submodule is dirty), fills in `dirtyRev`/`dirtyShortRev` for dirty trees and `rev`/`revCount`/`ref` for clean trees, sets `lastModified` from the headRev (or 0 when no HEAD exists), runs `warnDirty` on dirty trees.
  - Override `inputFromURL` — accepts `git` and any `app+transport` URL with `application == "git"`, splits known query parameters: string attrs (`rev`, `ref`, `keytype`, `publicKey`, `publicKeys`) and `Explicit<bool>` attrs (`shallow`, `submodules`, `lfs`, `exportIgnore`, `allRefs`, `verifyCommit`); other params remain on the URL.
  - Override `schemeName` — `"git"`.
  - Override `schemeDescription` — `stripIndentation` documentation referencing `builtins.fetchGit`.
  - Override `allowedAttrs` — schema with full docs for `url`, `ref`, `rev`, `shallow`, `submodules`, `lfs`, `lastModified`, `revCount`, `allRefs` and bare entries for `exportIgnore`, `narHash`, `name`, `dirtyRev`, `dirtyShortRev`, `verifyCommit`, `keytype`, `publicKey`, `publicKeys`.
  - Override `inputFromAttrs` — gates `verifyCommit`/`keytype`/`publicKey`/`publicKeys` behind `Xp::VerifiedFetches`; calls `maybeGetBoolAttr(attrs, "verifyCommit")` (validates type), validates `ref` via `isLegalRefName`, normalises URL via `fixGitURL`; eagerly evaluates `getShallowAttr`/`getSubmodulesAttr`/`getAllRefsAttr` for type checking.
  - Override `toURL` — re-emits the URL with `git+`-prefix (unless already `git`), copies state-bearing booleans (`shallow`, `lfs`, `submodules`, `exportIgnore`, `verifyCommit`) into the query string, plus `keytype`+`publicKey` for a single key or `publicKeys` JSON for multiple.
  - Override `applyOverrides` — sets `rev`/`ref` attrs; throws when there is a `rev` without a `ref`.
  - Override `clone` — runs `git clone <repo> [--branch <ref>] <destDir>`; throws `UnimplementedError` for rev-pinned clones.
  - Override `getSourcePath` — returns `repoInfo.getPath()`.
  - Override `putFile` — writes file under the working tree, runs `git check-ignore`, then `git add --intent-to-add`; if a commit message was given, writes it to a temp file and runs `git commit -F <tmp>` with the logger suspended for GPG passphrase prompts.
  - Override `getAccessor` — picks `getAccessorFromCommit` vs `getAccessorFromWorkdir` based on whether a ref/rev/non-local-path was supplied; rejects `exportIgnore && submodules` (`UnimplementedError`).
  - Override `getFingerprint` — for inputs with a rev, returns `rev.gitRev()` plus `;s` (submodules), `;e` (exportIgnore), `;l` (lfs) suffixes; for dirty workdirs without submodules hashes the modified/deleted file paths and modified-file contents into a SHA-512 digest appended as `;d=<hex>`; otherwise returns nullopt.
  - Override `isLocked` — true iff `rev` is set and not equal to `nullRev`.

### Functions
- Anonymous-namespace helpers:
  - `static isCacheFileWithinTtl(const Settings &, time_t now, const PosixStat &) -> bool` — compares `st.st_mtime + tarballTtl` against `now`.
  - `getCachePath(std::string_view key, bool shallow) -> std::filesystem::path` — `getCacheDir()/gitv3/<sha256-of-key in Nix32>[-shallow]`.
  - `readHead(const std::filesystem::path &) -> std::optional<std::string>` — runs `git ls-remote --symref <path>` (`isInteractive=true`) and parses the first output line via `git::parseLsRemoteLine` filtering for `HEAD`.
  - `storeCachedHead(const std::string & actualUrl, bool shallow, const std::string & headRef) -> bool` — writes the HEAD via `git -C <cache> --git-dir . symbolic-ref -- HEAD <ref>`; returns false on a non-zero exit.
  - `static readHeadCached(const Settings &, const std::string & actualUrl, bool shallow) -> std::optional<std::string>` — TTL-aware wrapper; warns and returns the expired cached value when a fresh fetch fails.
  - `getPublicKeys(const Attrs &) -> std::vector<PublicKey>` — combines `publicKey`/`keytype` (defaulting to `ssh-ed25519`) and `publicKeys` (parsed as JSON array) attrs.
- File-scope helpers:
  - `static const Hash nullRev{HashAlgorithm::SHA1}` — sentinel commit hash for empty repos.
  - `static makeLazyAttr(fun<ResolvedAttr()>) -> LazyAttr` — wraps the closure in a `memo<>` so the lazy attr is computed at most once.
- `static rGitInputScheme = OnStartup([] { registerInputScheme(std::make_unique<GitInputScheme>()); })`.

### Type aliases
- None.

### Macros / globals
- `static bool forceHttp = getEnv("_NIX_FORCE_HTTP") == "1"` (function-local static inside `getRepoInfo`) — testing knob that forces remote-fetch behaviour for `file://` URLs.

## File: src/libfetchers/mercurial.cc

### Namespaces
- `nix::fetchers`.

### Classes / structs / enums
- `MercurialInputScheme` (struct, `: InputScheme`) — handles `hg+http`, `hg+https`, `hg+ssh`, `hg+file` URLs.
  - Helper `getActualUrl(const Input &) const -> std::variant<std::filesystem::path, std::string>` — distinguishes local file paths from remote URLs.
  - `fetchToStore(const Settings &, Store &, Input &) const -> StorePath` — primary fetcher: handles dirty local workdirs by copying tracked files via `addToStore` with a custom `PathFilter` (uses `hg status` + a directory-prefix filter); otherwise pulls into `getCacheDir()/hg/<sha256-of-url>` (recovers from abandoned transactions by running `hg recover`), runs `hg log` to resolve the rev/ref/branch (extracting `node`, `rev`, `branch`), `hg archive`s the tree (deleting the resulting `.hg_archival.txt`), and `addToStore`s it. Caches under `hgRefToRev` (TTL) and `hgRev` (with-store-path) domains.
  - Override `inputFromURL` — strips the `hg+` prefix (`url2.scheme = std::string(url.scheme, 3)`), passes `rev`/`ref` query params to attrs, leaves other parameters on the URL.
  - Override `schemeName` — `"hg"`.
  - Override `schemeDescription` — `""` (TODO).
  - Override `allowedAttrs` — static map with `url`, `ref`, `rev`, `revCount`, `narHash`, `name`.
  - Override `inputFromAttrs` — validates URL via `parseURL` and ref name against `refRegex`.
  - Override `toURL` — reattaches `hg+` prefix, copies `rev`/`ref` to query.
  - Override `applyOverrides` — sets `rev`/`ref` attrs unconditionally (no validation).
  - Override `getSourcePath` — only returns the path for `file://` URLs without ref/rev.
  - Override `putFile` — for local paths runs `hg add` and optional `hg commit -m <msg>`; throws for remote URLs.
  - Override `getAccessor` — calls `fetchToStore`, then wraps in `requireStoreObjectAccessor`, sets the path display.
  - Override `isLocked` — true iff `rev` is set.
  - Override `getFingerprint` — `rev->gitRev()` if available, else nullopt.

### Functions
- `static hgOptions(OsStrings) -> RunOptions` — sets the `HGPLAIN=""` env var and `lookupPath = true`.
- `static runHg(OsStrings) -> std::string` — wraps `runProgram` with `hgOptions`, throws `ExecError` on non-zero exit.
- `static rMercurialInputScheme = OnStartup([] { registerInputScheme(std::make_unique<MercurialInputScheme>()); })`.

### Type aliases
- None.

### Macros / globals
- None.

## File: src/libfetchers/github.cc

### Namespaces
- `nix::fetchers`.

### Classes / structs / enums
- `DownloadUrl` (struct) — pair of `ParsedURL url` and `Headers headers`.
- `GitArchiveInputScheme` (struct, `: InputScheme`, abstract) — shared base for forge-archive fetchers (GitHub/GitLab/SourceHut).
  - Pure virtuals: `accessHeaderFromToken(const std::string &) const -> std::optional<std::pair<std::string, std::string>>`, `getRevFromRef(const Settings &, Store &, const Input &) const -> RefInfo`, `getDownloadUrl(const Settings &, const Input &) const -> DownloadUrl`.
  - Nested `RefInfo { Hash rev; std::optional<Hash> treeHash; }` and `TarballInfo { Hash treeHash; time_t lastModified; }`.
  - Helpers: `makeHeadersWithAuthTokens(const Settings &, const std::string & host, const Input &)` (composes `host/owner/repo` for path-prefixed token matching) and `makeHeadersWithAuthTokens(const Settings &, const std::string & host, const std::string & hostAndPath)` (resolves token via `getAccessToken` and `accessHeaderFromToken`, warns on unrecognised token).
  - `downloadArchive(const Settings &, Store &, Input) const -> std::pair<Input, TarballInfo>` — defaults `ref` to `HEAD`; resolves ref to rev via `getRevFromRef`; checks `gitRevToTreeHash`/`gitRevToLastModified` cache and that the tree still exists in the tarball Git cache; otherwise streams the tarball via `getDownloadUrl`, unpacks into the tarball cache via `unpackTarfileToSink`, calls `dereferenceSingletonDirectory`, and stores results in the cache. Contains `#if 0`-guarded debug code for `upstreamTreeHash` mismatch warnings.
  - Override `inputFromURL` — accepts only its own `schemeName`; splits path segments via `pathSegments(skipEmpty=true)`; supports `<scheme>:<owner>/<repo>`, `<owner>/<repo>/<ref-or-rev>` (regex-matched against `revRegex`), and `<owner>/<repo>/<ref-with-slashes>`; query params support `rev`, `ref`, `host`, `narHash`; rejects duplicate ref/rev and unknown params.
  - Override `allowedAttrs` — static map with `owner`, `repo`, `ref`, `rev`, `narHash`, `lastModified`, `host`, `treeHash`.
  - Override `inputFromAttrs` — requires `owner`/`repo`; forbids `ref+rev` together; validates rev as SHA-1; validates ref via `isLegalRefName`; validates host against `hostRegex`.
  - Override `toURL` — emits `<scheme>:<owner>/<repo>[/<ref>][/<rev-base16>]`, propagates `narHash` (SRI) and `host` to query.
  - Override `applyOverrides` — forbids `ref+rev` together, erases the other when one is set.
  - Override `getAccessToken` — longest-prefix match of `<host>=<token>` settings against the supplied URL, requiring the matched prefix to end at end-of-string or `/`; falls back to a host-only lookup.
  - Override `getAccessor` — calls `downloadArchive`, optionally inserts `treeHash` (currently `#if 0`-guarded), inserts `lastModified`, and serves through the tarball Git cache via `getAccessor(treeHash, {}, ...)`.
  - Override `isLocked` — requires `rev` plus either `settings.trustTarballsFromGitForges` or `narHash`.
  - Override `experimentalFeature` — `Xp::Flakes`.
  - Override `getFingerprint` — `rev->gitRev()` if present, else nullopt.
- `GitHubInputScheme` (struct, `: GitArchiveInputScheme`).
  - Override `schemeName` — `"github"`.
  - Override `schemeDescription` — `""` (TODO).
  - Override `accessHeaderFromToken` — emits `Authorization: token <pat>`.
  - Helper methods `getHost(const Input &) const` (defaults to `"github.com"`), `getOwner`, `getRepo`.
  - Override `getRevFromRef` — calls `https://api.github.com/repos/<owner>/<repo>/commits/<ref>` (or `https://<host>/api/v3/...` for self-hosted), returns commit SHA and tree SHA from the JSON response.
  - Override `getDownloadUrl` — picks `https://<host>/<owner>/<repo>/archive/<rev>.tar.gz` for unauthenticated `github.com`, otherwise `https://api.github.com/repos/<owner>/<repo>/tarball/<rev>` or the self-hosted equivalent.
  - Override `clone` — synthesises `git+https://<host>/<owner>/<repo>.git`, applies overrides, and delegates to that input's clone.
- `GitLabInputScheme` (struct, `: GitArchiveInputScheme`).
  - Override `schemeName` — `"gitlab"`.
  - Override `schemeDescription` — `""` (TODO).
  - Override `accessHeaderFromToken` — distinguishes `OAuth2:<tok>` (`Authorization: Bearer ...`) from `PAT:<tok>` (`Private-token: ...`); falls back to `<prefix>:<value>` with a warning.
  - Override `getRevFromRef` — `https://<host>/api/v4/projects/<owner>%2F<repo>/repository/commits?ref_name=<ref>`; throws on empty array or unexpected response.
  - Override `getDownloadUrl` — `https://<host>/api/v4/projects/<owner>%2F<repo>/repository/archive.tar.gz?sha=<rev-base16>`; default host `gitlab.com`.
  - Override `clone` — like GitHub, using `git+https://<host>/<owner>/<repo>.git`.
- `SourceHutInputScheme` (struct, `: GitArchiveInputScheme`).
  - Override `schemeName` — `"sourcehut"`.
  - Override `schemeDescription` — `""` (TODO).
  - Override `accessHeaderFromToken` — `Authorization: Bearer <tok>`.
  - Override `getRevFromRef` — for `HEAD`, fetches `<base_url>/HEAD` and parses via `git::parseLsRemoteLine`; otherwise scans `<base_url>/info/refs` for `refs/(heads|tags)/<ref>`; throws when no match. Default host `git.sr.ht`.
  - Override `getDownloadUrl` — `https://<host>/<owner>/<repo>/archive/<rev-base16>.tar.gz`.
  - Override `clone` — like GitHub, using `git+https://<host>/<owner>/<repo>` (no `.git` suffix).

### Functions
- `static rGitHubInputScheme = OnStartup([] { registerInputScheme(std::make_unique<GitHubInputScheme>()); })`.
- `static rGitLabInputScheme = OnStartup([] { registerInputScheme(std::make_unique<GitLabInputScheme>()); })`.
- `static rSourceHutInputScheme = OnStartup([] { registerInputScheme(std::make_unique<SourceHutInputScheme>()); })`.

### Type aliases
- None.

### Macros / globals
- `const static std::string hostRegexS = "[a-zA-Z0-9.-]*"` and `std::regex hostRegex(hostRegexS, std::regex::ECMAScript)` — file-scope host-name validator.
- Two `#if 0` blocks inside `downloadArchive` and `getAccessor` for tree-hash mismatch warning and `treeHash` output attribute respectively.

## File: src/libfetchers/tarball.cc

### Namespaces
- `nix::fetchers`.

### Classes / structs / enums
- `CurlInputScheme` (struct, `: InputScheme`, abstract) — shared base for `file`/`tarball` fetchers.
  - Member `const StringSet transportUrlSchemes = {"file", "http", "https"}`.
  - `hasTarballExtension(const ParsedURL &) const -> bool` — checks suffixes `.zip`, `.tar`, `.tgz`, `.tar.gz`, `.tar.xz`, `.tar.bz2`, `.tar.zst` on the last path segment.
  - Pure virtual `isValidURL(const ParsedURL &, bool requireTree) const -> bool`.
  - Static `allowedAttrsImpl() -> const std::map<std::string, AttributeInfo> &` — shared schema (`url` (with full doc), plus bare entries for `narHash`, `name`, `unpack`, `rev`, `revCount`, `lastModified`).
  - Override `inputFromURL` — runs `isValidURL`; sets `url.scheme = parseUrlScheme(url.scheme).transport`; extracts `narHash`, `rev`, integer `revCount`/`lastModified` (via `string2Int<uint64_t>`); strips every key in `allowedAttrs()` from the outgoing query (so they aren't sent to the HTTP server).
  - Override `allowedAttrs` — returns `allowedAttrsImpl()`.
  - Override `inputFromAttrs` — copies attrs through (no validation beyond whatever happens upstream).
  - Override `toURL` — re-parses the stored `url` and re-attaches the `narHash` query parameter when present.
  - Override `isLocked` — true iff `narHash` is set.
- `FileInputScheme` (struct, `: CurlInputScheme`) — single-file downloader.
  - Override `schemeName` — `"file"`.
  - Override `schemeDescription` — `stripIndentation` referencing `builtins.fetchurl`.
  - Override `isValidURL` — transport must be in `transportUrlSchemes`; explicit `file+...` schemes always match, otherwise only when not `requireTree` and not a tarball extension.
  - Override `getAccessor` — downloads via `downloadFile`, queries narHash via `queryPathInfo`, exposes the file via `store.getFSAccessor`.
- `TarballInputScheme` (struct, `: CurlInputScheme`) — tarball downloader.
  - Override `schemeName` — `"tarball"`.
  - Override `schemeDescription` — `stripIndentation` referencing `builtins.fetchTarball`.
  - Override `allowedAttrs` — overrides the `url` doc with a tarball-specific example, otherwise reuses `CurlInputScheme::allowedAttrsImpl()`.
  - Override `isValidURL` — transport must be in `transportUrlSchemes`; explicit `tarball+...` schemes always match, otherwise matches when `requireTree || hasTarballExtension`.
  - Override `getAccessor` — calls `downloadTarball_`, replaces the input with the `immutableUrl` redirect's input (re-fetched as type `tarball`; rejects non-tarball immutable URLs), stamps `lastModified` only when the user did not supply it, and stamps `narHash` from `treeHashToNarHash`.
  - Override `getFingerprint` — prefers narHash (SRI), falls back to rev (gitRev), otherwise nullopt.

### Functions
- `downloadFile(Store &, const Settings &, const VerbatimURL &, const std::string & name, const Headers &) -> DownloadFileResult` — fetches a single file with ETag-based revalidation; caches under the `file` cache domain (with-store-path); reuses the cached store path when the server returns 304 (`res.cached`); otherwise creates a `ValidPathInfo` with `Flat` ingestion and adds the data to the store; updates a cache entry for every URL in the redirect chain (using `res.urls.rbegin()` as the final `effectiveUrl`); falls back to the cached store path on `FileTransferError` when a cached entry exists.
- `static downloadTarball_(const Settings &, const std::string & urlS, const Headers &, const std::string & displayPrefix) -> DownloadTarballResult` — emits clear errors for `file://` URLs that are non-existent, relative, directories, or git repos; uses the `tarball` cache domain (no store path); validates the cached tree still exists in the tarball cache (`hasObject`); falls back to writing zip files to disk first (libarchive symlink workaround for `.zip`); unpacks via `unpackTarfileToSink`, calls `dereferenceSingletonDirectory`; updates cache entries for every URL in the redirect chain.
- `downloadTarball(Store &, const Settings &, const std::string & url) -> ref<SourceAccessor>` — wrapper that builds an `Input` of type `tarball` and calls `Input::getAccessor` so fingerprints are populated.
- `static rTarballInputScheme = OnStartup([] { registerInputScheme(std::make_unique<TarballInputScheme>()); })`.
- `static rFileInputScheme = OnStartup([] { registerInputScheme(std::make_unique<FileInputScheme>()); })`.

### Type aliases
- None.

### Macros / globals
- `static const StringSet specialParams` (declared as a static class member of `CurlInputScheme`) — declared but never defined or referenced in this file; compiles only because nothing odr-uses it.

## File: src/libfetchers/git-utils.cc

### Namespaces
- Anonymous specialisation `std::hash<git_oid>` (provides hashing for libgit2 OIDs) and free `operator<<` / `operator==` for `git_oid`.
- `nix` (most code).
- `nix::fetchers` (a small block defining `Settings::getTarballCache`).

### Classes / structs / enums
- `GitError` (struct, `final`, `: public CloneableError<GitError, Error>`) — wraps libgit2 errors with the libgit2 error class/code embedded in the message; has a primary constructor taking an explicit `git_error` and a secondary constructor that pulls the most recent error from `git_error_last`.
- `PackBuilderContext` (struct) — captures an `std::exception_ptr` so `git_packbuilder` callbacks can rethrow on `GIT_EUSER`; method `handleException(const char * activity, int errCode)` switches on the libgit2 error code.
- `GitRepoImpl` (struct, `: GitRepo`, `: std::enable_shared_from_this<GitRepoImpl>`) — primary repository implementation.
  - Members: `std::filesystem::path path`, `Options options`, `Repository repo`, `git_odb_backend * mempackBackend`, `git_odb_backend * packBackend`.
  - Constructor: `initLibGit2`, `initRepoAtomically`, opens the repo; in `packfilesOnly` mode builds a fresh ODB with only the pack backend (avoiding loose-object syscalls) and installs it via `git_repository_set_odb`; always installs the mempack backend with priority 999.
  - `operator git_repository *()` — returns `repo.get()`.
  - Override `flush()` — runs `git_packbuilder` with `PACKBUILDER_PROGRESS_CHECK_INTERRUPT` callback, sets thread autodetect, calls `git_mempack_write_thin_pack` then `git_packbuilder_write_buf`, indexes the resulting buffer in 128 KiB chunks via `git_indexer_append`, commits, and resets the mempack backend.
  - `getPool() -> Pool<GitRepoImpl>` — unbounded pool of fresh `GitRepoImpl` clones; monkey-patches each clone's `packBackend->refresh = nullptr` to skip directory rescans.
  - Override `getRevCount(const Hash &) -> uint64_t` — parallel BFS over commit ancestors using a `boost::concurrent_flat_set<git_oid>` as the visited set and a `ThreadPool`; throws when a parent OID is missing (suggests `?shallow=1`).
  - Override `getLastModified(const Hash &) -> uint64_t` — `git_commit_time` of the peeled commit.
  - Override `isShallow() -> bool` — `git_repository_is_shallow`.
  - Override `setRemote(const std::string & name, const std::string & url)` — `git_remote_set_url`.
  - Override `resolveRef(std::string ref) -> Hash` — `git_revparse_single` with the `^{commit}` peel suffix.
  - `parseSubmodules(const std::filesystem::path & configFile) -> std::vector<Submodule>` — opens `.gitmodules` via `git_config_open_ondisk`, iterates `submodule.<name>.(path|url|branch)` via `git_config_iterator_glob_new`, and assembles `Submodule` records.
  - `static statusCallbackTrampoline(const char * path, unsigned int statusFlags, void * payload) -> int` — converts `std::function` payloads into the C-style `git_status_foreach` callback.
  - Override `getWorkdirInfo() -> WorkdirInfo` — runs `git_status_foreach_ext` with `GIT_STATUS_OPT_INCLUDE_UNMODIFIED | GIT_STATUS_OPT_EXCLUDE_SUBMODULES`; reads HEAD via `git_reference_name_to_id`; populates `files`, `dirtyFiles`, `deletedFiles`, `isDirty`; parses submodules from the on-disk `.gitmodules`.
  - Override `getWorkdirRef() -> std::optional<std::string>` — `git_reference_lookup` for HEAD then `git_reference_symbolic_target`.
  - Override `getSubmodules(const Hash & rev, bool exportIgnore) -> std::vector<std::tuple<Submodule, Hash>>` (declared inline, defined out-of-class below) — reads `.gitmodules` from the given commit via the source accessor, parses it via `parseSubmodules`, looks up each submodule's commit OID through a raw accessor.
  - Override `resolveSubmoduleUrl(const std::string &) -> std::string` — `git_submodule_resolve_url` into a `git_buf`.
  - Override `hasObject(const Hash &) -> bool` — `git_object_lookup`, returning false on `GIT_ENOTFOUND`.
  - `getRawAccessor(const Hash & rev, const GitAccessorOptions &) -> ref<GitSourceAccessor>` (declared inline, defined out-of-class).
  - Override `getAccessor(const Hash & rev, const GitAccessorOptions &, std::string displayPrefix) -> ref<SourceAccessor>` — wraps `getRawAccessor` and optionally the result in `GitExportIgnoreSourceAccessor` when `exportIgnore`.
  - Override `getAccessor(const WorkdirInfo &, const GitAccessorOptions &, MakeNotAllowedError) -> ref<SourceAccessor>` — wraps `makeFSSourceAccessor(path)` in `AllowListSourceAccessor::create` (always allowing the root) and optionally an export-ignore wrapper.
  - Override `getFileSystemObjectSink() -> ref<GitFileSystemObjectSink>` — `make_ref<GitFileSystemObjectSinkImpl>`.
  - Override `fetch(const std::string & url, const std::string & refspec, bool shallow)` — runs `git -C <path> --git-dir . fetch --progress --force [--depth 1] -- <url> <refspec>` after removing leftover `shallow.lock`; logs an `Activity`; throws on non-zero exit.
  - Override `verifyCommit(const Hash &, const std::vector<fetchers::PublicKey> &)` — builds a temporary `allowed_signers` file (mapping the SSH key types via `keyTypeMap`), runs `git verify-commit`, then matches the merged stderr+stdout against a regex containing the SHA-256 fingerprint of every allowed key (preventing acceptance of GPG signatures from unrelated keys); throws on failure.
  - Override `treeHashToNarHash(const fetchers::Settings &, const Hash & treeHash) -> Hash` — caches under the `treeHashToNarHash` cache domain; on miss, materialises an accessor and calls `accessor->hashPath(CanonPath::root)`.
  - Override `dereferenceSingletonDirectory(const Hash & oid) -> Hash` — drops a one-entry tree wrapper to match `tar` archives whose root has a single subdirectory.
- `GitSourceAccessor` (struct, `: SourceAccessor`) — read-only accessor over a libgit2 tree/blob.
  - Nested `State` struct: `ref<GitRepoImpl> repo`, `Object root`, `std::optional<lfs::Fetch> lfsFetch`, `GitAccessorOptions options`. Held under `Sync<State> state_`.
  - Constructor peels the rev to a tree-or-blob; constructs an `lfs::Fetch` when `options.smudgeLfs`.
  - `readBlob(const CanonPath &, bool symlink, Sink &, std::function<void(uint64_t)> sizeCallback)` — handles git-lfs smudging when `lfsFetch->shouldFetch(path)` returns true; otherwise streams the raw blob.
  - Override `readFile(const CanonPath &, Sink &, fun<void(uint64_t)> sizeCallback)` — delegates to `readBlob(..., symlink=false, ...)`.
  - Override `pathExists(const CanonPath &) -> bool` — true at root, otherwise checks `lookup`.
  - Override `maybeLstat(const CanonPath &) -> std::optional<Stat>` — root case returns based on `git_object_type`; else inspects `git_tree_entry_filemode` (`TREE`/`BLOB`/`BLOB_EXECUTABLE`/`LINK`/`COMMIT`); throws on unsupported file types.
  - Override `readDirectory(const CanonPath &) -> DirEntries` — iterates `git_tree_entry_byindex` for trees; submodules return an empty `DirEntries`.
  - Override `readLink(const CanonPath &) -> std::string` — wraps `readBlob(..., symlink=true, ...)`.
  - Helper `getSubmoduleRev(const CanonPath &) -> std::optional<Hash>` — returns the OID for `GIT_OBJECT_COMMIT` entries.
  - Member `boost::unordered_flat_map<CanonPath, TreeEntry> lookupCache` — speeds up subsequent path lookups.
  - Helpers: `lookup` (recursive tree walk that fills `lookupCache` for every sibling), `lookupTree`, `need` (throws when missing), `getTree` (returns `std::variant<Tree, Submodule>`), `getBlob` (validates filemode against `expectSymlink`).
  - Inner sentinel struct `Submodule {}` — return-type marker for `getTree`.
- `GitExportIgnoreSourceAccessor` (struct, `: CachingFilteringSourceAccessor`) — implements `[attr]export-ignore` filtering.
  - Members: `ref<GitRepoImpl> repo`, `std::optional<Hash> rev` (`nullopt` for workdir mode).
  - Constructor sets up the `RestrictedPathError` factory mentioning `exportIgnore`.
  - `gitAttrGet(const CanonPath &, const char * attrName, const char *& valueOut) -> bool` — uses `git_attr_get_ext` with `attr_commit_id = oid` and `GIT_ATTR_CHECK_INCLUDE_COMMIT | GIT_ATTR_CHECK_NO_SYSTEM` for revs; uses `git_attr_get` with `GIT_ATTR_CHECK_INDEX_ONLY | GIT_ATTR_CHECK_NO_SYSTEM` for the workdir.
  - `isExportIgnored(const CanonPath &) -> bool` — false on `GIT_ENOTFOUND`, otherwise `GIT_ATTR_IS_TRUE(value)`.
  - Override `isAllowedUncached(const CanonPath &) -> bool` — negation of `isExportIgnored`.
- `GitFileSystemObjectSinkImpl` (struct, `: GitFileSystemObjectSink`) — multi-threaded importer that builds a Git tree from `FileSystemObjectSink` calls.
  - Members: `ref<GitRepoImpl> repo`, `Pool<GitRepoImpl> repoPool`, `unsigned int concurrency = std::min(std::thread::hardware_concurrency(), 10U)`, `ThreadPool workers{concurrency}`, `std::atomic<size_t> totalBufSize{0}`, `Sync<State> _state`, `size_t nextId`, `std::map<CanonPath, CanonPath> hardLinks`.
  - Static constexpr `maxBufSize = 16 * 1024 * 1024` — backpressure threshold.
  - Destructor explicitly calls `workers.shutdown()` so worker threads finish before referenced state is destroyed.
  - Nested types: `Directory` (children map + optional OID + recursive `lookup`), `Child` (`git_filemode_t mode`, `std::variant<Directory, git_oid>` plus `size_t id` for tarball-overlay precedence), `State` (the root directory).
  - `addNode(State &, const CanonPath &, Child &&)` — walks/creates intermediate directories and only overwrites entries with a strictly greater `id`.
  - Override `createRegularFile(const CanonPath &, fun<void(CreateRegularFileSink &)>)` — uses an in-class `CRF : CreateRegularFileSink` with a fast path (in-memory buffer, async blob create on `workers`) and a slow path (libgit2 streaming via `git_blob_create_from_stream`) once `totalBufSize > maxBufSize`. Inline `WriteStream` alias provides RAII for `git_writestream`.
  - Override `createDirectory(const CanonPath &)` — adds a tree entry under the lock (no-op at root).
  - Override `createSymlink(const CanonPath &, const std::string & target)` — creates a blob asynchronously on `workers` and inserts it as a `GIT_FILEMODE_LINK`.
  - Override `createHardlink(const CanonPath &, const CanonPath & target)` — defers resolution into the `hardLinks` map until `flush()`.
  - Override `flush() -> Hash` — drains workers, resolves hard links (errors when the target is a directory), flushes every pooled repo's mempack to disk via a per-repo `ThreadPool`, then writes Git tree objects bottom-up using a recursive lambda, returning the root tree's OID.

### Functions
- `operator<<(std::ostream &, const git_oid &)` — pretty-prints a libgit2 OID via `git_oid_tostr_s`.
- `operator==(const git_oid &, const git_oid &)` — uses `git_oid_equal`.
- `static toHash(const git_oid &) -> Hash` — copies the OID bytes into a SHA-1 `Hash`.
- `static initLibGit2()` — `std::call_once` wrapper around `git_libgit2_init`.
- `static hashToOID(const Hash &) -> git_oid` — `git_oid_fromstr` on the gitRev.
- `static lookupObject(git_repository *, const git_oid &, git_object_t type = GIT_OBJECT_ANY) -> Object` — `git_object_lookup`.
- `template<typename T> static peelObject(git_object *, git_object_t type) -> T` — `git_object_peel`.
- `template<typename T> static dupObject(typename T::pointer) -> T` — `git_object_dup`.
- `static peelToTreeOrBlob(git_object *) -> Object` — handles blobs separately (since `git_object_peel` doesn't), otherwise peels to `GIT_OBJECT_TREE`.
- `extern "C" static packBuilderProgressCheckInterrupt(int stage, uint32_t current, uint32_t total, void * payload) -> int` — `git_packbuilder_progress` callback that stashes `std::current_exception()` into the payload's `PackBuilderContext` and returns `GIT_EUSER`.
- `static initRepoAtomically(std::filesystem::path & path, GitRepo::Options)` — creates a temp-dir repo via `git_repository_init`, renames into place, tolerating races (`std::errc::file_exists`/`directory_not_empty`).
- `GitRepo::openRepo(const std::filesystem::path &, GitRepo::Options) -> ref<GitRepo>` — `make_ref<GitRepoImpl>`.
- `GitRepoImpl::getRawAccessor(const Hash &, const GitAccessorOptions &)` (out-of-class definition).
- `GitRepoImpl::getAccessor(const Hash &, const GitAccessorOptions &, std::string)` (out-of-class definition).
- `GitRepoImpl::getAccessor(const WorkdirInfo &, const GitAccessorOptions &, MakeNotAllowedError)` (out-of-class definition).
- `GitRepoImpl::getFileSystemObjectSink()` (out-of-class definition).
- `GitRepoImpl::getSubmodules(const Hash &, bool)` (out-of-class definition) — writes `.gitmodules` to a temp file and parses it via `parseSubmodules`, then queries each submodule's commit OID via `getRawAccessor(...)->getSubmoduleRev`.
- `Settings::getTarballCache() const -> ref<GitRepo>` (in `nix::fetchers` block) — function-local-static `tarball-cache-v2` directory under the cache dir, opened with `{create=true, bare=true, packfilesOnly=true}`.
- `GitRepo::getCachedWorkdirInfo(const std::filesystem::path &) -> WorkdirInfo` — process-wide cache keyed by absolute path; on miss runs `getWorkdirInfo` and inserts.
- `GitRepo::invalidateWorkdirInfoCache()` — clears the cache under the lock.
- `isLegalRefName(const std::string &) -> bool` — accepts `@` and DEL byte explicitly as invalid; otherwise tries `git_reference_name_is_valid`, `git_branch_name_is_valid`, `git_tag_name_is_valid` in turn.

### Type aliases
- `Repository = std::unique_ptr<git_repository, Deleter<git_repository_free>>`.
- `TreeEntry = std::unique_ptr<git_tree_entry, Deleter<git_tree_entry_free>>`.
- `Tree = std::unique_ptr<git_tree, Deleter<git_tree_free>>`.
- `TreeBuilder = std::unique_ptr<git_treebuilder, Deleter<git_treebuilder_free>>`.
- `Blob = std::unique_ptr<git_blob, Deleter<git_blob_free>>`.
- `Object = std::unique_ptr<git_object, Deleter<git_object_free>>`.
- `Commit = std::unique_ptr<git_commit, Deleter<git_commit_free>>`.
- `Reference = std::unique_ptr<git_reference, Deleter<git_reference_free>>`.
- `DescribeResult = std::unique_ptr<git_describe_result, Deleter<git_describe_result_free>>`.
- `StatusList = std::unique_ptr<git_status_list, Deleter<git_status_list_free>>`.
- `Remote = std::unique_ptr<git_remote, Deleter<git_remote_free>>`.
- `GitConfig = std::unique_ptr<git_config, Deleter<git_config_free>>`.
- `ConfigIterator = std::unique_ptr<git_config_iterator, Deleter<git_config_iterator_free>>`.
- `ObjectDb = std::unique_ptr<git_odb, Deleter<git_odb_free>>`.
- `PackBuilder = std::unique_ptr<git_packbuilder, Deleter<git_packbuilder_free>>`.
- `Indexer = std::unique_ptr<git_indexer, Deleter<git_indexer_free>>`.

### Macros / globals
- `static git_packbuilder_progress PACKBUILDER_PROGRESS_CHECK_INTERRUPT = &packBuilderProgressCheckInterrupt`.
- `static Sync<std::map<std::filesystem::path, GitRepo::WorkdirInfo>> workdirInfoCache_` — process-wide cache for `getCachedWorkdirInfo`.
- `template<> struct std::hash<git_oid>` — specialisation that reinterprets the OID's first `size_t` bytes as the hash.

## File: src/libfetchers/git-lfs-fetch.cc

### Namespaces
- `nix::lfs`, plus a small unnamed namespace and an `nlohmann` JSON dependency.

### Classes / structs / enums
- `LfsApiInfo` (anonymous-namespace struct) — `std::string endpoint`, `std::optional<std::string> authHeader`.

### Functions
- `static downloadToSink(const std::string & url, const std::optional<std::string> & authHeader, StringSink &, std::string sha256Expected, size_t sizeExpected)` — fetches over HTTP with optional auth header into the sink, verifies the size and SHA-256 match, throws on mismatch.
- `static getLfsApi(const ParsedURL &) -> LfsApiInfo` — for `ssh://` URLs, runs `ssh ... git-lfs-authenticate <path> download` (using `getNixSshOpts`, supporting `port`/`user`) and parses the JSON for `href` and `header.Authorization`; otherwise returns `<url>/info/lfs` with no auth header.
- `static getLfsEndpointUrl(git_repository *) -> std::string` — checks the repo's `lfs.url` config first via `git_config_get_entry`, falls back to the `origin` remote URL via `git_remote_lookup`+`git_remote_url`; returns empty string when neither is available.
- `static parseLfsPointer(std::string_view content, std::string_view filename) -> std::optional<Pointer>` — validates the `version https://git-lfs.github.com/spec/v1` prefix; extracts `oid sha256:...` (must be 64 hex chars) and `size ...` (must be all digits); silently ignores unknown extensions and rejects other bad data with debug logs.
- `Fetch::Fetch(git_repository *, git_oid)` — stores repo and rev; computes the LFS endpoint URL from `getLfsEndpointUrl` and canonicalises it via `nix::fixGitURL(...).canonicalise()`.
- `Fetch::shouldFetch(const CanonPath &) const -> bool` — calls `git_attr_get_ext` (with `attr_commit_id = rev`, `GIT_ATTR_CHECK_INCLUDE_COMMIT | GIT_ATTR_CHECK_NO_SYSTEM`); returns true iff the resulting `filter` attribute equals `"lfs"`; throws on libgit2 errors.
- `static pointerToPayload(const std::vector<Pointer> &) -> nlohmann::json` — builds the Batch API request JSON array of `{oid, size}` pairs.
- `Fetch::fetchUrls(const std::vector<Pointer> &) const -> std::vector<nlohmann::json>` — POSTs to `<endpoint>/objects/batch` with LFS content-type/accept headers and `{operation: download, objects: [...]}`; returns the parsed `objects` array; logs the full response on parse failure.
- `Fetch::fetch(const std::string & content, const CanonPath & pointerFilePath, StringSink &, std::function<void(uint64_t)> sizeCallback) const` — short-circuits when `content.length() >= 1024` or pointer parsing fails (warns and writes the original content); checks the on-disk LFS cache (`getCacheDir()/git-lfs/<sha256-of-rel-path>/<oid>`); fetches via `fetchUrls` + `downloadToSink` (extracting `actions.download.href` and optional auth header); verifies oid/size from the server match the pointer; writes the contents to the cache on success.

### Type aliases
- `GitConfig = std::unique_ptr<git_config, Deleter<git_config_free>>` (file-scope).
- `GitConfigEntry = std::unique_ptr<git_config_entry, Deleter<git_config_entry_free>>` (file-scope).

### Macros / globals
- None.

## File: src/libfetchers/include/nix/fetchers/attrs.hh

### Namespaces
- `nix::fetchers` — fetcher-attribute helpers.

### Classes / structs / enums
- `LazyAttrComputation` (struct) — wraps a `fun<ResolvedAttr()> compute` so a deferred attribute can be resolved on demand; pointer identity (via `ref<>`) gives equality semantics.

### Functions
- `forceAttr(const Attr &) -> ResolvedAttr` — resolve a possibly-lazy `Attr` to its concrete value.
- `maybeGetLazyAttr(const Attrs &, const std::string &) -> std::optional<LazyAttr>` — fetch attr only when it is still lazy.
- `jsonToAttrs(const nlohmann::json &) -> Attrs` — convert a flat JSON object into the `Attrs` map.
- `attrsToJSON(const Attrs &) -> nlohmann::json` — inverse of `jsonToAttrs`; also forces lazies.
- `maybeGetStrAttr(const Attrs &, const std::string &) -> std::optional<std::string>` and `getStrAttr(...) -> std::string` — typed string accessors.
- `maybeGetIntAttr(const Attrs &, const std::string &) -> std::optional<uint64_t>` and `getIntAttr(...) -> uint64_t` — typed integer accessors.
- `maybeGetBoolAttr(const Attrs &, const std::string &) -> std::optional<bool>` and `getBoolAttr(...) -> bool` — typed bool accessors.
- `attrsToQuery(const Attrs &) -> StringMap` — render attrs as URL query parameters.
- `getRevAttr(const Attrs &, const std::string &) -> Hash` — parse a revision attribute as a SHA-1.

### Type aliases
- `ResolvedAttr = std::variant<std::string, uint64_t, Explicit<bool>>`.
- `LazyAttr = ref<LazyAttrComputation>`.
- `Attr = std::variant<std::string, uint64_t, Explicit<bool>, LazyAttr>`.
- `Attrs = std::map<std::string, Attr>` (declared via `typedef`).

### Macros / globals
- None.

## File: src/libfetchers/include/nix/fetchers/cache.hh

### Namespaces
- `nix::fetchers` — global cache abstraction shared between schemes.

### Classes / structs / enums
- `Cache` (struct, abstract) — virtual interface for the on-disk SQLite cache that maps `(Domain, Attrs)` keys to either an `Attrs` value (with TTL) or to an `Attrs` plus a `StorePath`. Has a virtual destructor.
  - Nested `Cache::Result` (struct) — `bool expired = false`, `Attrs value`.
  - Nested `Cache::ResultWithStorePath` (struct, `: Result`) — adds `StorePath storePath`.

### Functions
- All members are pure virtuals; no free functions.
- `Cache::upsert(const Key & key, const Attrs & value)`, `Cache::upsert(Key, Store &, Attrs, const StorePath &)` — insert.
- `Cache::lookup(const Key &) -> std::optional<Attrs>`, `Cache::lookupWithTTL(const Key &) -> std::optional<Attrs>`, `Cache::lookupExpired(const Key &) -> std::optional<Result>` — fetch attrs only.
- `Cache::lookupStorePath(Key, Store &) -> std::optional<ResultWithStorePath>`, `Cache::lookupStorePathWithTTL(Key, Store &) -> std::optional<ResultWithStorePath>` — fetch attrs + store path.

### Type aliases
- `Cache::Domain = std::string_view`.
- `Cache::Key = std::pair<Domain, Attrs>`.

### Macros / globals
- None.

## File: src/libfetchers/include/nix/fetchers/fetch-settings.hh

### Namespaces
- `nix::fetchers` — settings owned by a fetcher session. Forward-declares `nix::GitRepo` and `nix::fetchers::Cache`.

### Classes / structs / enums
- `Settings` (struct, `: public Config`) — fetcher configuration; owns lazy SQLite cache plus tarball Git cache.
  - Fields: `Setting<StringMap> accessTokens`, `Setting<bool> allowDirty` (default `true`), `Setting<bool> warnDirty` (default `true`), `Setting<bool> allowDirtyLocks` (default `false`, gated by `Xp::Flakes`), `Setting<bool> trustTarballsFromGitForges` (default `true`), `Setting<std::string> flakeRegistry` (default `https://channels.nixos.org/flake-registry.json`, gated by `Xp::Flakes`), `Setting<unsigned int> tarballTtl` (default `60 * 60`).
  - `getCache() const -> ref<Cache>`, `getTarballCache() const -> ref<GitRepo>` — lazily create and memoise singletons.
  - Private member `mutable Sync<std::shared_ptr<Cache>> _cache` — for thread-safe lazy init.

### Functions
- `Settings::Settings()` — registers settings with the config (definition lives in `fetch-settings.cc` with an empty body; the in-class `Setting<>` initialisers do the real work).

### Type aliases
- None.

### Macros / globals
- None.

## File: src/libfetchers/include/nix/fetchers/fetch-to-store.hh

### Namespaces
- `nix` — fetch-to-store helpers (not under `fetchers::`).

### Classes / structs / enums
- `FetchMode` (enum struct) — `DryRun`, `Copy`.

### Functions
- `fetchToStore(const fetchers::Settings &, Store &, const SourcePath &, FetchMode, std::string_view name = "source", ContentAddressMethod = ContentAddressMethod::Raw::NixArchive, PathFilter * filter = nullptr, RepairFlag = NoRepair) -> StorePath` — copy a source-path subtree into the store.
- `fetchToStore2(...) -> std::pair<StorePath, Hash>` — same signature; also returns the NAR hash.
- `makeSourcePathToHashCacheKey(std::string_view fingerprint, ContentAddressMethod, const CanonPath &) -> fetchers::Cache::Key` — key builder used by the source-path cache lookup.

### Type aliases
- None.

### Macros / globals
- None.

## File: src/libfetchers/include/nix/fetchers/fetchers.hh

### Namespaces
- `nix` (forward decls of `Store`, `StorePath`, `SourceAccessor`) and `nix::fetchers` (forward decls of `InputScheme`, `Settings`).

### Classes / structs / enums
- `Input` (struct) — value type binding an `InputScheme` together with the user-provided `Attrs` and a cached fingerprint.
  - Members: `std::shared_ptr<InputScheme> scheme` (can be null), `Attrs attrs`, `mutable std::optional<std::optional<std::string>> cachedFingerprint`.
  - Friend declaration of `InputScheme`.
  - Static factories `fromURL(const Settings &, const std::string &, bool requireTree = true)` (string + ParsedURL overloads), `fromAttrs(const Settings &, Attrs &&)`, plus static `checkLocks(Input specified, Input & result)` for invariant validation.
  - Higher-level operations: `toURL`, `toURLString(const StringMap & extraQuery = {})`, `to_string`, `toAttrs`, `isDirect`, `isLocked(const Settings &)`, `isRelative`, `isFinal`, `operator==`, `operator<` (compares `attrs`), `contains`, `fetchToStore(const Settings &, Store &)`, `getAccessor(const Settings &, Store &)` (with private `getAccessorUnchecked`), `applyOverrides`, `clone`, `getSourcePath`, `putFile`, `getName`, `computeStorePath(Store &)`, `getType`, `getNarHash`, `getRef`, `getRev`, `getRevCount`, `getLastModified`, `getFingerprint(Store &)`.
- `InputScheme` (struct, abstract) — pluggable fetcher implementation.
  - Pure virtuals: `inputFromURL`, `inputFromAttrs`, `schemeName`, `schemeDescription`, `allowedAttrs`, `getAccessor`.
  - Defaultable: `toURL`, `applyOverrides`, `clone`, `getSourcePath`, `putFile`, `experimentalFeature`, `isDirect` (default true), `getFingerprint` (default nullopt), `isLocked` (default false), `isRelative` (default nullopt), `getAccessToken` (default nullopt).
  - Nested `AttributeInfo` struct (`const char * type = "String"`, `bool required = true`, `const char * doc = ""`) consumed by docs generation.
- `PublicKey` (struct) — `std::string type = "ssh-ed25519"`, `std::string key`, with default `<=>`.

### Functions
- `registerInputScheme(std::shared_ptr<InputScheme> &&)` — global registry mutation.
- `getAllInputSchemes() -> const InputSchemeMap &` — read-only registry view (intended for docs, not lookup).
- `publicKeys_to_string(const std::vector<PublicKey> &) -> std::string` — JSON serialisation helper.

### Type aliases
- `InputSchemeMap = std::map<std::string_view, std::shared_ptr<InputScheme>>`.

### Macros / globals
- `JSON_IMPL(fetchers::PublicKey)` (file-scope) — declares JSON `to_json`/`from_json` adapters.

## File: src/libfetchers/include/nix/fetchers/filtering-source-accessor.hh

### Namespaces
- `nix` — accessor wrappers.

### Classes / structs / enums
- `FilteringSourceAccessor` (struct, `: SourceAccessor`, abstract) — wraps another accessor and rejects calls when `isAllowed()` returns false.
  - Members: `ref<SourceAccessor> next`, `CanonPath prefix`, `MakeNotAllowedError makeNotAllowedError`.
  - Constructor takes a `const SourcePath &` and `MakeNotAllowedError &&`; clears `displayPrefix`.
  - `using SourceAccessor::readFile` to expose the base `readFile(CanonPath) -> std::string` overload alongside the new sink override.
  - Overrides: `getPhysicalPath`, `readFile (sink+sizeCallback)`, `pathExists`, `lstat`, `maybeLstat`, `readDirectory`, `readLink`, `showPath`, `getFingerprint`, `invalidateCache` (delegates to `next->invalidateCache`).
  - Pure virtual `isAllowed(const CanonPath &) -> bool`; helper `checkAccess(const CanonPath &)` that raises via the stored exception factory.
- `AllowListSourceAccessor` (struct, `: public FilteringSourceAccessor`, abstract) — adds pure virtual `allowPrefix(CanonPath)` and a static `create(ref<SourceAccessor>, const std::set<CanonPath> &, const std::unordered_set<CanonPath> &, MakeNotAllowedError &&) -> ref<AllowListSourceAccessor>` factory; inherits the base constructor via `using`.
- `CachingFilteringSourceAccessor` (struct, `: FilteringSourceAccessor`, abstract) — memoises `isAllowed()` results in a `std::map<CanonPath, bool> cache`; subclasses implement `isAllowedUncached`. Inherits the base constructor via `using`.

### Functions
- (All non-virtual functions are members listed above.)

### Type aliases
- `MakeNotAllowedError = fun<RestrictedPathError(const CanonPath & path)>`.

### Macros / globals
- None.

## File: src/libfetchers/include/nix/fetchers/git-lfs-fetch.hh

### Namespaces
- `nix::lfs` — Git LFS pointer + fetch helpers.

### Classes / structs / enums
- `Pointer` (struct) — `std::string oid`, `size_t size`; represents a parsed LFS pointer file.
- `Fetch` (struct) — `const git_repository * repo`, `git_oid rev`, `nix::ParsedURL url`. Bound to a libgit2 repository + commit; constructs the LFS endpoint URL from the remote and provides `shouldFetch`, `fetch`, and `fetchUrls` helpers.

### Functions
- `Fetch::Fetch(git_repository *, git_oid)` — constructor.
- `Fetch::shouldFetch(const CanonPath &) const -> bool` — match LFS `filter` attribute.
- `Fetch::fetch(const std::string & content, const CanonPath & pointerFilePath, StringSink &, std::function<void(uint64_t)> sizeCallback) const` — populate sink with the LFS object.
- `Fetch::fetchUrls(const std::vector<Pointer> &) const -> std::vector<nlohmann::json>` — query the LFS server for download URLs.

### Type aliases
- None.

### Macros / globals
- None.

## File: src/libfetchers/include/nix/fetchers/git-utils.hh

### Namespaces
- `nix` — Git-related interfaces shared between fetchers and store layer; forward-declares `nix::fetchers::PublicKey` and `nix::fetchers::Settings`.

### Classes / structs / enums
- `GitFileSystemObjectSink` (struct, `: ExtendedFileSystemObjectSink`, abstract) — adds a pure virtual `flush() -> Hash` to obtain the resulting tree id.
- `GitAccessorOptions` (struct) — `bool exportIgnore = false`, `bool smudgeLfs = false`.
- `GitRepo` (struct, abstract) — the libgit2-backed repo facade. Has a virtual destructor.
  - Nested `Options` struct (`bool create = false`, `bool bare = false`, `bool packfilesOnly = false`).
  - Static `openRepo(const std::filesystem::path &, Options) -> ref<GitRepo>`.
  - Pure virtuals: `getRevCount`, `getLastModified`, `isShallow`, `resolveRef`, `setRemote`, `getWorkdirInfo`, `getWorkdirRef`, `getSubmodules`, `resolveSubmoduleUrl`, `hasObject`, two `getAccessor` overloads (rev-based and workdir-based), `getFileSystemObjectSink`, `flush`, `fetch`, `verifyCommit`, `treeHashToNarHash`, `dereferenceSingletonDirectory`.
  - Nested `Submodule` struct (`CanonPath path`, `std::string url`, `std::string branch`).
  - Nested `WorkdirInfo` struct (`bool isDirty = false`, `std::optional<Hash> headRev`, `std::set<CanonPath> files`, `std::set<CanonPath> dirtyFiles`, `std::set<CanonPath> deletedFiles`, `std::vector<Submodule> submodules`).
  - Static `getCachedWorkdirInfo(const std::filesystem::path &) -> WorkdirInfo` and `invalidateWorkdirInfoCache()` for the workdir-info cache.
- `Setter<T>` (template struct) — RAII helper; captures a libgit2 raw pointer in `p` and assigns it back into the held `T` (a `unique_ptr`-like) on destruction; `operator T::pointer *()` exposes the address-of-pointer for libgit2 out-params.

### Functions
- `isLegalRefName(const std::string &) -> bool` — coarse Git ref name validation.

### Type aliases
- None (uses `MakeNotAllowedError` from `filtering-source-accessor.hh`).

### Macros / globals
- None.

## File: src/libfetchers/include/nix/fetchers/input-cache.hh

### Namespaces
- `nix::fetchers`. Forward declares `enum class UseRegistries : int` and `struct Settings`.

### Classes / structs / enums
- `InputCache` (struct, abstract) — in-memory cache mapping unresolved `Input`s to their resolved/locked twins plus an accessor. Has a virtual destructor.
  - Nested `CachedResult` (returned from `getAccessor`): `ref<SourceAccessor> accessor`, `Input resolvedInput`, `Input lockedInput`, `Attrs extraAttrs`.
  - Nested `CachedInput` (lookup payload): `Input lockedInput`, `ref<SourceAccessor> accessor`, `Attrs extraAttrs`.
  - Pure virtuals `lookup`, `upsert`, `clear`.
  - Static factory `create() -> ref<InputCache>`.

### Functions
- `InputCache::getAccessor(const Settings &, Store &, const Input & originalInput, UseRegistries) -> CachedResult` — combines registry lookup, cache lookup, and `Input::getAccessor` to produce a `CachedResult` (definition in `input-cache.cc`).

### Type aliases
- None.

### Macros / globals
- None.

## File: src/libfetchers/include/nix/fetchers/registry.hh

### Namespaces
- `nix::fetchers`. Forward-declares `nix::Store`.

### Classes / structs / enums
- `Registry` (struct) — represents one of the cascading registries (flag/user/system/global/custom) and stores a list of `(from, to, extraAttrs, exact)` entries.
  - Nested `enum RegistryType { Flag = 0, User = 1, System = 2, Global = 3, Custom = 4 }`.
  - Nested `Entry` struct: `Input from, to`, `Attrs extraAttrs`, `bool exact = false`.
  - Members: `RegistryType type`, `std::vector<Entry> entries`.
  - Constructor `Registry(RegistryType)`.
  - Static `read(const Settings &, const SourcePath &, RegistryType) -> std::shared_ptr<Registry>`.
  - Methods: `write(const std::filesystem::path &)`, `add(const Input &, const Input &, const Attrs &)`, `remove(const Input &)`.
- `enum class UseRegistries : int { No, All, Limited }` (Limited = global + flag only).

### Functions
- `getUserRegistry(const Settings &) -> std::shared_ptr<Registry>` and `getCustomRegistry(const Settings &, const std::filesystem::path &) -> std::shared_ptr<Registry>` — lazy loaders.
- `getUserRegistryPath() -> std::filesystem::path` — XDG path for the user registry.
- `getRegistries(const Settings &, Store &) -> Registries` — fetches all registries in priority order.
- `overrideRegistry(const Input & from, const Input & to, const Attrs & extraAttrs)` — adds a process-wide override.
- `lookupInRegistries(const Settings &, Store &, const Input &, UseRegistries) -> std::pair<Input, Attrs>` — resolve an input via the chain.

### Type aliases
- `Registries = std::vector<std::shared_ptr<Registry>>` (declared via `typedef`).

### Macros / globals
- None.

## File: src/libfetchers/include/nix/fetchers/tarball.hh

### Namespaces
- `nix::fetchers`. Forward-declares `nix::Store`, `nix::SourceAccessor`, `nix::fetchers::Settings`.

### Classes / structs / enums
- `DownloadFileResult` (struct) — `StorePath storePath`, `std::string etag`, `std::string effectiveUrl`, `std::optional<std::string> immutableUrl`.
- `DownloadTarballResult` (struct) — `Hash treeHash`, `time_t lastModified`, `std::optional<std::string> immutableUrl`, `ref<SourceAccessor> accessor`.

### Functions
- `downloadFile(Store &, const Settings &, const VerbatimURL &, const std::string & name, const Headers & = {}) -> DownloadFileResult` — single-file fetch with caching.
- `downloadTarball(Store &, const Settings &, const std::string & url) -> ref<SourceAccessor>` — tarball fetch + import into the Git cache; returns an accessor over the imported tree.

### Type aliases
- None.

### Macros / globals
- None.

## Cross-file observations

### `InputScheme` hierarchy
The `InputScheme` interface is implemented by:

| Scheme | Header trigger | File |
| ------ | -------------- | ---- |
| `IndirectInputScheme` | `flake:` | `indirect.cc` |
| `PathInputScheme` | `path:` | `path.cc` |
| `GitInputScheme` | `git`, `git+http`, `git+https`, `git+ssh`, `git+file` | `git.cc` |
| `MercurialInputScheme` | `hg+http`, `hg+https`, `hg+ssh`, `hg+file` | `mercurial.cc` |
| `GitArchiveInputScheme` (abstract) | shared base for forge tarball fetchers | `github.cc` |
| `GitHubInputScheme` | `github` | `github.cc` |
| `GitLabInputScheme` | `gitlab` | `github.cc` |
| `SourceHutInputScheme` | `sourcehut` | `github.cc` |
| `CurlInputScheme` (abstract) | shared base for curl-downloadable resources | `tarball.cc` |
| `FileInputScheme` | `file`, `http(s)` (and `file+...`) | `tarball.cc` |
| `TarballInputScheme` | `tarball` and tarball-extension URLs | `tarball.cc` |

Every scheme registers itself at startup via `static auto rXxx = OnStartup([] { registerInputScheme(std::make_unique<XxxInputScheme>()); });`. The same idiom repeats verbatim across all eight scheme files (one `OnStartup` per concrete `InputScheme`). The registry is a Meyers-singleton `inputSchemes()` keyed by `schemeName()`; collisions throw at startup.

### Required overrides
The pure virtuals `inputFromURL`, `inputFromAttrs`, `schemeName`, `schemeDescription`, `allowedAttrs`, `getAccessor` are implemented in every concrete scheme.

`experimentalFeature() == Xp::Flakes` is overridden only in `IndirectInputScheme`, `PathInputScheme`, and `GitArchiveInputScheme` (so all of GitHub/GitLab/SourceHut). `GitInputScheme`, `MercurialInputScheme`, `CurlInputScheme`/`FileInputScheme`/`TarballInputScheme` do not gate on an experimental feature and accept the inherited default (nullopt).

`getFingerprint` is overridden by `GitInputScheme` (rev-based plus optional dirty-workdir SHA-512 digest), `MercurialInputScheme` (rev only), `GitArchiveInputScheme` (rev only, applies to all three forge schemes), and `TarballInputScheme` (narHash preferred over rev). `IndirectInputScheme`, `PathInputScheme`, `CurlInputScheme` (and thus `FileInputScheme`) inherit the default nullopt.

`isLocked` is overridden by `PathInputScheme` (narHash), `GitInputScheme` (rev != nullRev), `MercurialInputScheme` (rev set), `GitArchiveInputScheme` (rev plus (`trustTarballsFromGitForges` || narHash)), and `CurlInputScheme` (narHash). `IndirectInputScheme` inherits the default (always false).

`isDirect` defaults to true; only `IndirectInputScheme` returns false. `isRelative` is overridden only by `PathInputScheme`. `getAccessToken` is overridden only by `GitArchiveInputScheme` (the longest-prefix matcher).

### Recurring code shapes
- **URL parsing**: each scheme strips Nix-specific query parameters into `Attrs` and then re-emits them in `toURL`. The pattern (`{rev, ref, narHash}` + booleans) is repeated in `git`, `mercurial`, `path`, `github`-family and `curl`-family code, with bespoke logic for the path-segment layout (`flake:id`, `path:/abs`, `<owner>/<repo>/...`, etc.).
- **Cache-key construction**: every scheme builds `Cache::Key{<domain>, <attrs>}` ad hoc. Domains seen here:
  - `sourcePathToHash` (`fetch-to-store.cc`).
  - `gitLastModified`, `gitRevCount` (`git.cc`).
  - `gitRevToTreeHash`, `gitRevToLastModified` (`github.cc`).
  - `treeHashToNarHash` (`git-utils.cc`).
  - `hgRefToRev`, `hgRev` (`mercurial.cc`).
  - `tarball`, `file` (`tarball.cc`).
  - The `Cache` class is shared but each scheme picks its own attribute schema; there is no helper to construct or document these keys uniformly.
- **`OnStartup` registration**: identical idiom in every scheme.
- **`makeHeadersWithAuthTokens`**: only in `GitArchiveInputScheme`, and the longest-prefix `accessTokens` matching logic in `getAccessToken` is unique to that base class (other schemes inherit the default nullopt).
- **Submodule recursion**: `git.cc` builds a `MountedSourceAccessor` from `repo->getSubmodules(rev, exportIgnore)`; `getAccessorFromWorkdir` does the same with submodule workdirs. Both repeat synthesised-Attrs construction (`type=git`, `url=...`, `submodules=true`, `lfs=...`, `exportIgnore=...`, `allRefs=true`) — this could plausibly be a shared helper.
- **Dirty-workdir handling**: `git.cc` and `mercurial.cc` both honour `settings.allowDirty`/`settings.warnDirty`. Git seeds `dirtyRev`/`dirtyShortRev` and uses `AllowListSourceAccessor::create` (with submodule-aware path filtering); Mercurial copies tracked files into the store via `addToStore` with a custom `PathFilter` driven by `hg status` output.
- **`clone()` implementation**: `GitInputScheme` runs `git clone` directly, while `GitHub`, `GitLab`, and `SourceHut` all delegate to `Input::fromURL("git+https://...").applyOverrides(...).clone(...)` — i.e. they synthesise a different kind of input and reuse Git's clone. `Mercurial`/`Path`/`Indirect`/`File`/`Tarball` rely on the default `InputScheme::clone` (resolve via `getAccessor`, then `copyRecursive` into a `RestoreSink` with `startFsync=false`).
- **Default ref handling**: `git.cc::getDefaultRef` falls back to `getWorkdirRef`/`readHeadCached` and then to `"master"`; `mercurial.cc` defaults to `"default"`; `GitArchiveInputScheme::downloadArchive` defaults to `"HEAD"`. None share a helper.

### Caching layers
- On-disk SQLite: `CacheImpl` (`cache.cc`) keyed by `(domain, JSON-attrs)`, with optional store-path entries (used heavily by `tarball.cc`, `mercurial.cc`, `git.cc`, `github.cc`, `git-utils.cc::treeHashToNarHash`, and `fetch-to-store.cc`). Stored at `getCacheDir() / "fetcher-cache-v4.sqlite"`.
- In-memory: `InputCacheImpl` (`input-cache.cc`) keyed by full `Input`; `clear()` additionally calls `GitRepo::invalidateWorkdirInfoCache()`.
- Workdir info: `static workdirInfoCache_` in `git-utils.cc` keyed by absolute path; flushed when `InputCacheImpl::clear()` runs.
- Per-accessor libgit2 lookup cache: `GitSourceAccessor::lookupCache` (per accessor instance, not shared).
- Tarball Git cache: `Settings::getTarballCache()` returns a singleton bare libgit2 repo at `<cache>/tarball-cache-v2` opened with `{create=true, bare=true, packfilesOnly=true}`. All forge fetchers and the curl tarball scheme route their unpacked trees here so `treeHashToNarHash` can be cached.
- Source-path hash cache: `makeSourcePathToHashCacheKey` in `fetch-to-store.cc` is also called by `Input::getAccessorUnchecked` (to seed the cache for substituted final inputs) and by `PathInputScheme::getAccessor` (to short-circuit `fetchToStore`).
- LFS object cache: `git-lfs-fetch.cc::Fetch::fetch` caches at `getCacheDir() / "git-lfs/<sha256-of-rel-path>/<oid>"`.

### Concurrency primitives
- `Sync<T>` for state under `CacheImpl`, `InputCacheImpl`, `GitSourceAccessor`, `GitFileSystemObjectSinkImpl`, `workdirInfoCache_`; also the `mutable Sync<std::shared_ptr<Cache>> Settings::_cache`.
- `SharedSync<T>` only inside `AllowListSourceAccessorImpl::allowedPrefixes`.
- `boost::concurrent_flat_set<>` — visited set for parallel `getRevCount` BFS in `git-utils.cc` (`git_oid` keys), and for `allowedPaths` in `filtering-source-accessor.cc` (`CanonPath` keys).
- `boost::unordered_flat_map<CanonPath, TreeEntry>` — `GitSourceAccessor::lookupCache`.
- `boost::unordered_flat_map<std::string_view, std::string_view>` — `keyTypeMap` in `verifyCommit`.
- `Pool<GitRepoImpl>` — multithreaded blob writes and tree flushing in `GitFileSystemObjectSinkImpl`.
- `ThreadPool` — `getRevCount` BFS, `flush()` per-pool-repo flush, `GitFileSystemObjectSinkImpl` worker pool.
- `PathLocks` — `Input::getAccessorUnchecked` (per-input lock under `getCacheDir()/fetcher-locks`), `git.cc::getAccessorFromCommit` (per cache-dir lock).
- Atomic counter `std::atomic<size_t> totalBufSize` for backpressure in `GitFileSystemObjectSinkImpl`.

### Pure helpers worth noting
- `makeLazyAttr` (`git.cc`, file-static) — combines `LazyAttrComputation` with a `memo<>` so `LazyAttr`s are computed at most once.
- `Setter<T>` (in `git-utils.hh`) — RAII wrapper that captures a libgit2 raw pointer into a `unique_ptr`-like.
- `extern "C" packBuilderProgressCheckInterrupt` — propagates `checkInterrupt()` exceptions through libgit2's C callback ABI by stashing them in `PackBuilderContext::exception`.

### Unused / loose ends spotted
- `CurlInputScheme::specialParams` (`tarball.cc`) is declared but never defined or referenced; compiles only because nothing odr-uses it.
- `getCustomRegistry` (`registry.cc`) memoises the result in a function-local static, so only the first call's path is honoured. The comment is silent on this.
- `#if 0` blocks in `github.cc` (treeHash mismatch warning inside `downloadArchive`, treeHash output attribute inside `getAccessor`) hint at unfinished tree-hash propagation.
- `std::hash<git_oid>` reinterprets the OID's first `size_t` bytes as the hash; correct for SHA-1 because the structure has no padding.
- `Settings::Settings()` body is empty — all real work happens via the in-class `Setting<>` initialisers (the `Config` base sees the registrations through them).
- `InputScheme::AttributeInfo` defaults `required = true`, but most concrete schemes use `{}` (relying on the field being optional in practice); the comment in `fetchers.hh` notes "TODO remove these defaults".
